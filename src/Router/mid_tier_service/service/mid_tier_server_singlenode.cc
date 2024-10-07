/* Author: Akshitha Sriraman
   Ph.D. Candidate at the University of Michigan - Ann Arbor*/

#include <memory>
#include <omp.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include <grpc++/grpc++.h>

#include "lookup_service/service/helper_files/client_helper.h"
#include "mid_tier_service/service/helper_files/router_server_helper.h"
#include "mid_tier_service/service/helper_files/timing.h"
#include "mid_tier_service/service/helper_files/utils.h"

#define NODEBUG

typedef unsigned char UINT8;

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using router::RouterRequest;
using router::LookupResponse;
using router::RouterService;

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using lookup::Key;
using lookup::UtilRequest;
using lookup::TimingDataInMicro;
using lookup::UtilResponse;
using lookup::Value;
using lookup::LookupService;

// Class declarations.
class ServerImpl;
class LookupServiceClient;

// Function declarations.
void ProcessRequest(RouterRequest &router_request,
        uint64_t unique_request_id_value,
        int tid);

// Global variable declarations.
/* dataset_dim is global so that we can validate query dimensions whenever 
   batches of queries are received.*/
unsigned int router_parallelism = 1, number_of_response_threads = 1, dispatch_parallelism = 1;
int replication_cnt = 1, number_of_lookup_servers = 1;
uint64_t create_lookup_srv_req_time = 0, unpack_lookup_srv_resp_time = 0, unpack_lookup_srv_req_time = 0, lookup_srv_time = 0, pack_lookup_srv_resp_time = 0;
std::string ip = "localhost", lookup_server_ips_file;
std::vector<std::string> lookup_server_ips;

uint64_t num_requests = 0;
std::vector<LookupServiceClient*> lookup_srv_connections;
/* Server object is global so that the async lookup_srv client
   thread can access it after it has merged all responses.*/
ServerImpl* server;
ResponseMap response_count_down_map;

ThreadSafeQueue<bool> kill_notify;
/* Fine grained locking while looking at individual responses from
   multiple lookup_srv servers. Coarse grained locking when we want to add
   or remove an element from the map.*/
std::mutex response_map_mutex, thread_id, lookup_server_id_mutex, map_coarse_mutex;
std::vector<mutex_wrapper> lookup_srv_conn_mutex;
std::map<uint64_t, std::unique_ptr<std::mutex> > map_fine_mutex;
int get_profile_stats = 0;
bool first_req = false;

CompletionQueue* lookup_srv_cq = new CompletionQueue();

bool kill_signal = false;

ThreadSafeQueue<DispatchedData*> dispatched_data_queue;
std::mutex dispatched_data_queue_mutex;
Atomics* started = new Atomics();

class ServerImpl final {
    public:
        ~ServerImpl() {
            server_->Shutdown();
            // Always shutdown the completion queue after the server.
            cq_->Shutdown();
        }

        // There is no shutdown handling in this code.
        void Run() {
            std::string ip_port = ip;
            std::string server_address(ip_port);

            ServerBuilder builder;
            // Listen on the given address without any authentication mechanism.
            builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
            // Register "service_" as the instance through which we'll communicate with
            // clients. In this case it corresponds to an *asynchronous* service.
            builder.RegisterService(&service_);
            // Get hold of the completion queue used for the asynchronous communication
            // with the gRPC runtime.
            cq_ = builder.AddCompletionQueue();
            // Finally assemble the server.
            server_ = builder.BuildAndStart();
            std::cout << "Server listening on " << server_address << std::endl;

            std::vector<std::thread> worker_threads;
            for(unsigned int i = 0; i < dispatch_parallelism; i++)
            {
                /* Launch the dispatch threads. When there are
                   no requests, the threads just keeps spinning on a
                   "dispatch queue".*/
                worker_threads.emplace_back(std::thread(&ServerImpl::Dispatch, this, i));
            }

            // Proceed to the server's main loop.
            /* This section of the code is parallelized to handle 
               multiple requests at once.*/
            omp_set_dynamic(0);
            omp_set_nested(1);
            omp_set_num_threads(router_parallelism);
            int tid = -1;
#pragma omp parallel
            {
                thread_id.lock();
                int tid_local = ++tid;
                thread_id.unlock();
                HandleRpcs(tid_local);
            }

            for(unsigned int i = 0; i < dispatch_parallelism; i++)
            {
                worker_threads[i].join();
            }

        }

        void Finish(uint64_t unique_request_id,
                LookupResponse* router_reply)
        {

            CallData* call_data_req_to_finish = (CallData*) unique_request_id;
            call_data_req_to_finish->Finish(router_reply);
        }

    private:
        // Class encompasing the state and logic needed to serve a request.
        class CallData {
            public:
                // Take in the "service" instance (in this case representing an asynchronous
                // server) and the completion queue "cq" used for asynchronous communication
                // with the gRPC runtime.
                CallData(RouterService::AsyncService* service, ServerCompletionQueue* cq)
                    : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE) {
                        // Invoke the serving logic right away.
                        int tid = 0;
                        Proceed(tid);
                    }

                void Proceed(int tid) {
                    if (status_ == CREATE) {
                        // Make this instance progress to the PROCESS state.
                        status_ = PROCESS;

                        // As part of the initial CREATE state, we *request* that the system
                        // start processing SayHello requests. In this request, "this" acts are
                        // the tag uniquely identifying the request (so that different CallData
                        // instances can serve different requests concurrently), in this case
                        // the memory address of this CallData instance.
                        service_->RequestRouter(&ctx_, &router_request_, &responder_, cq_, cq_,
                                this);
                    } else if (status_ == PROCESS) {
                        // Spawn a new CallData instance to serve new clients while we process
                        // the one for this CallData. The instance will deallocate itself as
                        // part of its FINISH state.
                        new CallData(service_, cq_);
                        uint64_t unique_request_id_value = reinterpret_cast<uintptr_t>(this);
                        //uint64_t unique_request_id_value = num_reqs->AtomicallyIncrementCount();
                        // The actual processing.
                        ProcessRequest(router_request_, 
                                unique_request_id_value, 
                                tid);
                        // And we are done! Let the gRPC runtime know we've finished, using the
                        // memory address of this instance as the uniquely identifying tag for
                        // the event.
                        //status_ = FINISH;
                        //responder_.Finish(router_reply_, Status::OK, this);
                    } else {
                        //GPR_ASSERT(status_ == FINISH);
                        // Once in the FINISH state, deallocate ourselves (CallData).
                        delete this;
                    }
                }

                void Finish(LookupResponse* router_reply)
                {
                    status_ = FINISH;
                    //GPR_ASSERT(status_ == FINISH);
                    responder_.Finish(*router_reply, Status::OK, this);
                }

            private:
                // The means of communication with the gRPC runtime for an asynchronous
                // server.
                RouterService::AsyncService* service_;
                // The producer-consumer queue where for asynchronous server notifications.
                ServerCompletionQueue* cq_;
                // Context for the rpc, allowing to tweak aspects of it such as the use
                // of compression, authentication, as well as to send metadata back to the
                // client.
                ServerContext ctx_;
                // What we get from the client.
                RouterRequest router_request_;
                // What we send back to the client.
                LookupResponse router_reply_;

                // The means to get back to the client.
                ServerAsyncResponseWriter<LookupResponse> responder_;

                // Let's implement a tiny state machine with the following states.
                enum CallStatus { CREATE, PROCESS, FINISH };
                CallStatus status_;  // The current serving state.
        };

        /* Function called by thread that is the worker. Network poller 
           hands requests to this worker thread via a 
           producer-consumer style queue.*/
        void Dispatch(int worker_tid) {
            /* Continuously spin and keep checking if there is a
               dispatched request that needs to be processed.*/
            while(true)
            {
                /* As long as there is a request to be processed,
                   process it. Outer while is just to ensure
                   that we keep waiting for a request when there is
                   nothing in the queue.*/
                DispatchedData* dispatched_request = dispatched_data_queue.pop();
                static_cast<CallData*>(dispatched_request->tag)->Proceed(worker_tid);
                delete dispatched_request;
            }
        }

        // This can be run in multiple threads if needed.
        void HandleRpcs(int tid) {
            // Spawn a new CallData instance to serve new clients.
            new CallData(&service_, cq_.get());
            void* tag;  // uniquely identifies a request.
            bool ok;
            int cnt = 0;
            while (true) {
                // Block waiting to read the next event from the completion queue. The
                // event is uniquely identified by its tag, which in this case is the
                // memory address of a CallData instance.
                cq_->Next(&tag, &ok);
                if (cnt == 0) {
                    cnt++;
                    kill_notify.push(true);
                }
                /* When we have a new request, we create a new object
                   to the dispatch queue.*/
                DispatchedData* request_to_be_dispatched = new DispatchedData();
                request_to_be_dispatched->tag = tag;
                dispatched_data_queue.push(request_to_be_dispatched);
            }
        }

        std::unique_ptr<ServerCompletionQueue> cq_;
        RouterService::AsyncService service_;
        std::unique_ptr<Server> server_;
};


/* Declaring lookup_srv client here because the router server must
   invoke the lookup_srv client to send the queries+PointIDs to the lookup_srv server.*/
class LookupServiceClient {
    public:
        explicit LookupServiceClient(std::shared_ptr<Channel> channel)
            : stub_(LookupService::NewStub(channel)) {}
        /* Assambles the client's payload, sends it and presents the response back
           from the server.*/
        void KeyLookup(const uint32_t lookup_server_id,
                const bool util_present,
                const uint64_t request_id,
                Key request_to_lookup_srv)
        {
            // Declare the set of queries that must be sent.
            // Create RCP request by adding queries, point IDs, and number of NN.
            CreateLookupServiceRequest(lookup_server_id,
                    util_present,
                    &request_to_lookup_srv);

            request_to_lookup_srv.set_request_id(request_id);
            //lookup_srv_timing_info->create_lookup_srv_request_time = end_time - start_time;
            // Container for the data we expect from the server.
            Value reply;
            // Context for the client. 
            ClientContext context;
            // Call object to store rpc data
            AsyncClientCall* call = new AsyncClientCall;
            // stub_->AsyncSayHello() performs the RPC call, returning an instance to
            // store in "call". Because we are using the asynchronous API, we need to
            // hold on to the "call" instance in order to get updates on the ongoing RPC.
            call->response_reader = stub_->AsyncKeyLookup(&call->context, request_to_lookup_srv, lookup_srv_cq);
            // Request that, upon completion of the RPC, "reply" be updated with the
            // server's response; "status" with the indication of whether the operation
            // was successful. Tag the request with the memory address of the call object.
            call->response_reader->Finish(&call->reply, &call->status, (void*)call);
        }

        // Loop while listening for completed responses.
        // Prints out the response from the server.
        void AsyncCompleteRpc() {
            void* got_tag;
            bool ok = false;
            lookup_srv_cq->Next(&got_tag, &ok);
            //auto r = cq_.AsyncNext(&got_tag, &ok, gpr_time_0(GPR_CLOCK_REALTIME));
            //if (r == ServerCompletionQueue::TIMEOUT) return;
            //if (r == ServerCompletionQueue::GOT_EVENT) {
            // The tag in this example is the memory location of the call object
            AsyncClientCall* call = static_cast<AsyncClientCall*>(got_tag);

            // Verify that the request was completed successfully. Note that "ok"
            // corresponds solely to the request for updates introduced by Finish().
            //GPR_ASSERT(ok);

            if (call->status.ok())
            {
                uint64_t s1 = GetTimeInMicro();
                uint64_t unique_request_id = call->reply.request_id();
                /* When this is not the last response, we need to decrement the count
                   as well as collect response meta data - knn answer, lookup_srv util, and
                   lookup_srv timing info.
                   When this is the last request, we remove this request from the map and 
                   merge responses from all lookup_srvs.*/
                /* Create local DistCalc, LookupSrvTimingInfo, BucketUtil variables,
                   so that this thread can unpack received lookup_srv data into these variables
                   and then grab a lock to append to the response array in the map.*/
                std::string value;
                LookupSrvTimingInfo lookup_srv_timing_info;
                LookupSrvUtil lookup_srv_util;
                uint64_t start_time = GetTimeInMicro();
#ifndef NODEBUG
                std::cout << "bef unpack lookup resp\n";
#endif
                UnpackLookupServiceResponse(call->reply,
                        &value,
                        &lookup_srv_timing_info,
                        &lookup_srv_util);
#ifndef NODEBUG
                std::cout << "aft unpack lookup resp\n";
#endif
                uint64_t end_time = GetTimeInMicro();
                // Make sure that the map entry corresponding to request id exists.
                map_coarse_mutex.lock();
                try {
                    response_count_down_map.at(unique_request_id);
                } catch( ... ) {
                    CHECK(false, "ERROR: Map entry corresponding to request id does not exist\n");
                }
                map_coarse_mutex.unlock();
#ifndef NODEBUG
                std::cout << "bef assigning lookup val\n";
#endif
                map_fine_mutex[unique_request_id]->lock();
                int lookup_srv_resp_id = response_count_down_map[unique_request_id].responses_recvd;
                *(response_count_down_map[unique_request_id].response_data[lookup_srv_resp_id].value) = value;
                *(response_count_down_map[unique_request_id].response_data[lookup_srv_resp_id].lookup_srv_timing_info) = lookup_srv_timing_info;
                *(response_count_down_map[unique_request_id].response_data[lookup_srv_resp_id].lookup_srv_util) = lookup_srv_util;

                response_count_down_map[unique_request_id].response_data[lookup_srv_resp_id].lookup_srv_timing_info->unpack_lookup_srv_resp_time = end_time - start_time;
#ifndef NODEBUG
                std::cout << "aft assigning lookup val\n";
#endif

                if (response_count_down_map[unique_request_id].responses_recvd != (replication_cnt - 1)) {
                    response_count_down_map[unique_request_id].responses_recvd++;
                    map_fine_mutex[unique_request_id]->unlock();
                } else {
                    uint64_t lookup_srv_resp_start_time = response_count_down_map[unique_request_id].router_reply->get_lookup_srv_responses_time();
                    response_count_down_map[unique_request_id].router_reply->set_get_lookup_srv_responses_time(GetTimeInMicro() - lookup_srv_resp_start_time);
                    /* Time to merge all responses received and then 
                       call terminate so that the response can be sent back
                       to the load generator.*/
                    /* We now know that all lookup_srvs have responded, hence we can 
                       proceed to merge responses.*/

                    std::string lookup_val = "";
                    start_time = GetTimeInMicro();
#ifndef NODEBUG
                    std::cout << "bef merge\n";
#endif
                    MergeAndPack(response_count_down_map[unique_request_id].response_data,
                            replication_cnt,
                            response_count_down_map[unique_request_id].router_reply);
#ifndef NODEBUG
                    std::cout << "aft merge\n";
#endif
                    end_time = GetTimeInMicro();
                    response_count_down_map[unique_request_id].router_reply->set_merge_time(end_time - start_time);
                    response_count_down_map[unique_request_id].router_reply->set_pack_router_resp_time(end_time - start_time); 
                    //response_count_down_map[unique_request_id].router_reply->set_router_time(router_times[unique_request_id]);
                    /* Call server finish for this particular request,
                       and pass the response so that it can be sent
                       by the server to the frontend.*/
                    uint64_t prev_rec = response_count_down_map[unique_request_id].router_reply->router_time();
                    response_count_down_map[unique_request_id].router_reply->set_router_time(prev_rec + (GetTimeInMicro() - s1));
                    map_fine_mutex[unique_request_id]->unlock();

                    map_coarse_mutex.lock();
                    server->Finish(unique_request_id, 
                            response_count_down_map[unique_request_id].router_reply);
                    map_coarse_mutex.unlock();
                }
            } else {
                CHECK(false, "lookup_srv does not exist\n");
            }
            // Once we're complete, deallocate the call object.
            delete call;
        }

            private:
        // struct for keeping state and data information
        struct AsyncClientCall {
            // Container for the data we expect from the server.
            Value reply;
            // Context for the client. It could be used to convey extra information to
            // the server and/or tweak certain RPC behaviors.
            ClientContext context;
            // Storage for the status of the RPC upon completion.
            Status status;
            std::unique_ptr<ClientAsyncResponseReader<Value>> response_reader;
        };

        // Out of the passed in Channel comes the stub, stored here, our view of the
        // server's exposed services.
        std::unique_ptr<LookupService::Stub> stub_;

        // The producer-consumer queue we use to communicate asynchronously with the
        // gRPC runtime.
        CompletionQueue cq_;
        };

        void ProcessRequest(RouterRequest &router_request, 
                uint64_t unique_request_id_value,
                int tid)
        {
            uint64_t s1 =0, e1 =0;
            s1 = GetTimeInMicro();
            if (!started->AtomicallyReadFlag()) {
                started->AtomicallySetFlag(true);
            }
            /* Deine the map entry corresponding to this
               unique request.*/
            // Declare the size of the final response that the map must hold.
            map_coarse_mutex.lock();
            ResponseMetaData meta_data;
            response_count_down_map.erase(unique_request_id_value);
            map_fine_mutex.erase(unique_request_id_value);
            map_fine_mutex[unique_request_id_value] = std::make_unique<std::mutex>();
            response_count_down_map[unique_request_id_value] = meta_data;
            map_coarse_mutex.unlock();

            map_fine_mutex[unique_request_id_value]->lock();
            if (router_request.kill()) {
                kill_signal = true;
                response_count_down_map[unique_request_id_value].router_reply->set_kill_ack(true);
                server->Finish(unique_request_id_value,
                        response_count_down_map[unique_request_id_value].router_reply);
                sleep(4);
                CHECK(false, "Exit signal received\n");
            }
            response_count_down_map[unique_request_id_value].responses_recvd = 0;
            response_count_down_map[unique_request_id_value].response_data.resize(replication_cnt, ResponseData());
            response_count_down_map[unique_request_id_value].router_reply->set_request_id(router_request.request_id());
            response_count_down_map[unique_request_id_value].router_reply->set_num_inline(router_parallelism);
            response_count_down_map[unique_request_id_value].router_reply->set_num_workers(dispatch_parallelism);
            response_count_down_map[unique_request_id_value].router_reply->set_num_resp(number_of_response_threads);
            //map_fine_mutex[unique_request_id_value]->unlock();

            bool util_present = router_request.util_request().util_request();
            /* If the load generator is asking for util info,
               it means the time period has expired, so 
               the router must read /proc/stat to provide user, system, and io times.*/
            if(util_present)
            {
                uint64_t start = GetTimeInMicro();
                uint64_t user_time = 0, system_time = 0, io_time = 0, idle_time = 0;
                GetCpuTimes(&user_time,
                        &system_time,
                        &io_time,
                        &idle_time);
                //map_fine_mutex[unique_request_id_value]->lock();
                response_count_down_map[unique_request_id_value].router_reply->mutable_util_response()->mutable_router_util()->set_user_time(user_time);
                response_count_down_map[unique_request_id_value].router_reply->mutable_util_response()->mutable_router_util()->set_system_time(system_time);
                response_count_down_map[unique_request_id_value].router_reply->mutable_util_response()->mutable_router_util()->set_io_time(io_time);
                response_count_down_map[unique_request_id_value].router_reply->mutable_util_response()->mutable_router_util()->set_idle_time(idle_time);
                response_count_down_map[unique_request_id_value].router_reply->mutable_util_response()->set_util_present(true);
                response_count_down_map[unique_request_id_value].router_reply->set_update_router_util_time(GetTimeInMicro() - start);
                //map_fine_mutex[unique_request_id_value]->unlock();
            }
            uint64_t start_time = GetTimeInMicro();
            std::string key = "", value = "";
            uint32_t operation = 1;
            Key request_to_lookup_srv;
#ifndef NODEBUG
            std::cout << "bef unpack router service req\n";
#endif
            UnpackRouterServiceRequest(router_request,
                    &key,
                    &value,
                    &operation,
                    &request_to_lookup_srv);
#ifndef NODEBUG
            std::cout << "aft unpack router service req\n";
#endif
            uint64_t end_time = GetTimeInMicro();
            //map_fine_mutex[unique_request_id_value]->lock();
            response_count_down_map[unique_request_id_value].router_reply->set_unpack_router_req_time((end_time-start_time));
            //map_fine_mutex[unique_request_id_value]->unlock();
            //float points_sent_percent = PercentDataSent(point_ids, queries_size, dataset_size);
            //printf("Amount of dataset sent to lookup_srv server in the form of point IDs = %.5f\n", points_sent_percent);
            //(*response_count_down_map)[unique_request_id_value]->router_reply->set_percent_data_sent(points_sent_percent);

            //map_fine_mutex[unique_request_id_value]->lock();
            response_count_down_map[unique_request_id_value].router_reply->set_get_lookup_srv_responses_time(GetTimeInMicro());
            //map_fine_mutex[unique_request_id_value]->unlock();

            int lookup_srv_to_send_req_to = 0;
            //int replication_count = 1;
            int replication_count = replication_cnt;
            if (request_to_lookup_srv.operation() == 2) {
                replication_count = replication_cnt;
            }
            for(int i = 0; i < replication_count; i++) {
                lookup_srv_to_send_req_to = (SpookyHash::Hash32(key.c_str(), key.size(), 0) + i) % number_of_lookup_servers;
                int router = (tid * number_of_lookup_servers) + lookup_srv_to_send_req_to;

                lookup_srv_connections[router]->KeyLookup(lookup_srv_to_send_req_to,
                        util_present,
                        unique_request_id_value,
                        request_to_lookup_srv);
            }
            e1 = GetTimeInMicro() - s1;
            response_count_down_map[unique_request_id_value].router_reply->set_router_time(e1);
            map_fine_mutex[unique_request_id_value]->unlock();
        }

        /* The request processing thread runs this 
           function. It checks all the lookup_srv socket connections one by
           one to see if there is a response. If there is one, it then
           implements the count down mechanism in the global map.*/
        void ProcessResponses()
        {
            while(true)
            {
                lookup_srv_connections[0]->AsyncCompleteRpc();
                sleep(1);
            }

        }

        void FinalKill()
        {
#if 0
            kill_notify.pop();
            long int sleep_time = 50 * 1000 * 1000;
            usleep(sleep_time);
            CHECK(false, "couldn't die, so timer killed it\n");
#endif
        }

        void Perf()
        {
            while (true) {
                if (started->AtomicallyReadFlag()) {
                    std::cout << "cs\n";
                    std::string s = "sudo perf stat -e cs -I 30000 -p " + std::to_string(getpid());
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    break;
                }
            }
        }

        void SysCount()
        {
            while (true) {
                if (started->AtomicallyReadFlag()) {
                    std::cout << "syscnt\n";
                    std::string s = "sudo /usr/share/bcc/tools/syscount -i 30 -p " + std::to_string(getpid()) + " > " + "syscount.txt";
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    std::cout << "executed syscount\n";
                    break;
                }
            }
        }

        void Hardirqs()
        {
            while (true) {
                if (started->AtomicallyReadFlag()) {
                    std::cout << "hardirqs\n";
                    std::string s = "sudo /usr/share/bcc/tools/hardirqs -d -T 30 1 > hardirqs.txt";
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    break;
                    std::cout << "executed hardirqs\n";
                }
            }
        }

        void Wakeuptime()
        {
            while (true) {
                if (started->AtomicallyReadFlag()) {
                    std::string s = "sudo /usr/share/bcc/tools/wakeuptime -p " + std::to_string(getpid()) + " 30 > wakeuptime.txt";
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    break;
                }
            }
        }

        void Softirqs()
        {
            while (true) {
                if (started->AtomicallyReadFlag()) {
                    std::string s = "sudo /usr/share/bcc/tools/softirqs -T 30 1 -d > softirqs.txt";
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    break;
                }
            }
        }

        void Runqlat()
        {
            while (true) {
                if (started->AtomicallyReadFlag()) {
                    std::string s = "sudo /usr/share/bcc/tools/runqlat 30 1 > runqlat.txt";
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    break;
                }
            }
        }

        void Hitm()
        {
            while (true) {
                if (started->AtomicallyReadFlag()) {
                    std::string s = "sudo perf c2c record -p " + std::to_string(getpid());
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    break;
                }
            }
        }

        void Tcpretrans()
        {   
            while (true) {
                if (started->AtomicallyReadFlag()) {
                    std::string s = "sudo /usr/share/bcc/tools/tcpretrans -c -l > tcpretrans.txt";
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    break;
                }
            }
        }



        int main(int argc, char** argv) {
            if (argc == 8) {
                number_of_lookup_servers = atoi(argv[1]);
                lookup_server_ips_file = argv[2];
                ip = argv[3];
                router_parallelism = atoi(argv[4]);
                dispatch_parallelism = atoi(argv[5]);
                number_of_response_threads = atoi(argv[6]);
                replication_cnt = atoi(argv[7]);
            } else {
                CHECK(false, "<./router_server> <number of lookup servers> <lookup server ips file> <ip:port number> <router parallelism> <dispatch parallelism> <num of response threads> <replication cnt>\n");
            }

            CHECK((replication_cnt <= number_of_lookup_servers), "Replication count must be less than or equal to number of lookup servers\n");
            // Load lookup server IPs into a string vector
            GetLookupServerIPs(lookup_server_ips_file, &lookup_server_ips);


            for(unsigned int i = 0; i < dispatch_parallelism; i++)
            {
                for(int j = 0; j < number_of_lookup_servers; j++)
                {
                    std::string ip = lookup_server_ips[j];
                    lookup_srv_connections.emplace_back(new LookupServiceClient(grpc::CreateChannel(
                                    ip, grpc::InsecureChannelCredentials())));
                }
            }
            std::vector<std::thread> response_threads;
            for(unsigned int i = 0; i < number_of_response_threads; i++)
            {
                response_threads.emplace_back(std::thread(ProcessResponses));
            }

        //    std::thread kill_ack = std::thread(FinalKill);
#if 0
        //    std::thread perf(Perf);
#endif
        /*    std::thread perf(Perf);
            std::thread syscount(SysCount);
            std::thread hardirqs(Hardirqs);
            std::thread wakeuptime(Wakeuptime);
            std::thread softirqs(Softirqs);
            std::thread runqlat(Runqlat);
            //std::thread hitm(Hitm);
            std::thread tcpretrans(Tcpretrans);
        */
            server = new ServerImpl();
            server->Run();
            for(unsigned int i = 0; i < number_of_response_threads; i++)
            {
                response_threads[i].join();
            }

          /*  kill_ack.join();
            perf.join();
            syscount.join();
            hardirqs.join();
            wakeuptime.join();
            softirqs.join();
            runqlat.join();
            //hitm.join();
            tcpretrans.join(); */
#if 0
          //  perf.join();
#endif

            return 0;
        }
