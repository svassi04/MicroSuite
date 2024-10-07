/* Author: Akshitha Sriraman
   Ph.D. Candidate at the University of Michigan - Ann Arbor*/

#include <iostream>
#include <memory>
#include <omp.h>
#include <string>
#include <sys/time.h>
#include <thread>
#include <grpc++/grpc++.h>
#include "cf_service/service/helper_files/server_helper.h"
#include "cf_service/service/helper_files/timing.h"
#include "cf_service/service/helper_files/utils.h"

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using collaborative_filtering::UtilRequest;
using collaborative_filtering::UtilResponse;
using collaborative_filtering::CFRequest;
using collaborative_filtering::TimingDataInMicro;
using collaborative_filtering::CFResponse;
using collaborative_filtering::CFService;

using namespace mlpack;
using namespace mlpack::cf;

/* Make dataset a global, so that the dataset can be loaded
   even before the server starts running. */
Matrix dataset;

std::string ip_port = "";
std::mutex cf_mutex;
unsigned int cf_parallelism = 0;

int num_cores = 0, cf_server_number = 0, num_cf_servers = 0;
CF* cf_matrix;

void ProcessRequest(CFRequest &request,
        CFResponse* reply)
{
    /* If the index server is asking for util info,
       it means the time period has expired, so 
       the cf must read /proc/stat to provide user, system, io, and idle times.*/
    if(request.util_request().util_request())
    {
        uint64_t user_time = 0, system_time = 0, io_time = 0, idle_time = 0;
        GetCpuTimes(&user_time,
                &system_time,
                &io_time,
                &idle_time);
        reply->mutable_util_response()->set_user_time(user_time);
        reply->mutable_util_response()->set_system_time(system_time);
        reply->mutable_util_response()->set_io_time(io_time);
        reply->mutable_util_response()->set_idle_time(idle_time);
        reply->mutable_util_response()->set_util_present(true);
    }

    /* Simply copy request id into the reply - this was just a 
       piggyback message.*/
    reply->set_request_id(request.request_id());

    /* Get the current idle time and total time
       so as to calculate the CPU util when the cf is done.*/
    size_t idle_time_initial = 0, total_time_initial = 0, idle_time_final = 0, total_time_final = 0;
    //GetCpuTimes(&idle_time_initial, &total_time_initial);

    // Unpack received queries and point IDs
    uint32_t cf_server_id, shard_size;
    uint64_t start_time, end_time;
    start_time = GetTimeInMicro();
    Request user_item;
    UnpackCFServiceRequest(request,
            &user_item);

    end_time = GetTimeInMicro();
    reply->mutable_timing_data_in_micro()->set_unpack_cf_srv_req_time_in_micro((end_time - start_time));
    /* Next piggy back message - sent the received query back to the 
       index server. Helps to merge async responses.*/
    // Remove duplicate point IDs.
    //RemoveDuplicatePointIDs(point_ids_vec);

    // Dataset dimension must be equal to queries dimension.
#if 0
    dataset.ValidateDimensions(dataset.GetPointDimension(),
            queries.GetPointDimension());
#endif

    // Calculate the top K distances for all queries.
    start_time = GetTimeInMicro();
    float rating = 0.0;
    CalculateRating(user_item,
            cf_matrix,
            &rating);
    end_time = GetTimeInMicro();
    reply->mutable_timing_data_in_micro()->set_calculate_cf_srv_time_in_micro((end_time - start_time));

    // Convert K-NN into form suitable for GRPC.
    start_time = GetTimeInMicro();
    PackCFServiceResponse(rating, 
            reply);
    end_time = GetTimeInMicro();
    reply->mutable_timing_data_in_micro()->set_pack_cf_srv_resp_time_in_micro((end_time - start_time));
    //GetCpuTimes(&idle_time_final, &total_time_final);
    const float idle_time_delta = idle_time_final - idle_time_initial;
    const float total_time_delta = total_time_final - total_time_initial;
    const float cpu_util = (100.0 * (1.0 - (idle_time_delta/total_time_delta)));
    reply->mutable_timing_data_in_micro()->set_cpu_util(cpu_util);
}

// Logic and data behind the server's behavior.
class ServiceImpl final {
    public:
        ~ServiceImpl() {
            server_->Shutdown();
            // Always shutdown the completion queue after the server.
            cq_->Shutdown();
        }
        // There is no shutdown handling in this code.
        void Run() {
            std::string server_address(ip_port);
            ServerBuilder builder;
            // Listen on the given address without any authentication mechanism.
            try
            {
                builder.AddListeningPort(server_address,
                        grpc::InsecureServerCredentials());
            } catch(...) {
                CHECK(false, "ERROR: Enter a valid IP address follwed by port number - IP:Port number\n");
            }
            // Register "service_" as the instance through which we'll communicate with
            // clients. In this case it corresponds to an *asynchronous* service.
            builder.RegisterService(&service_);
            // Get hold of the completion queue used for the asynchronous communication
            // with the gRPC runtime.
            cq_ = builder.AddCompletionQueue();
            // Finally assemble the server.
            server_ = builder.BuildAndStart();
            std::cout << "Server listening on " << server_address << std::endl;
            // Proceed to the server's main loop.
            if (cf_parallelism == 1) {
                HandleRpcs();
            }
            omp_set_dynamic(0);
            omp_set_num_threads(cf_parallelism);
            omp_set_nested(2);
#pragma omp parallel
            {
                HandleRpcs();
            }
        }    
    private:
        // Class encompasing the state and logic needed to serve a request.
        class CallData {
            public:
                // Take in the "service" instance (in this case representing an asynchronous
                // server) and the completion queue "cq" used for asynchronous communication
                // with the gRPC runtime.
                CallData(CFService::AsyncService* service, ServerCompletionQueue* cq)
                    : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE) {
                        // Invoke the serving logic right away.
                        Proceed();
                    }

                void Proceed() {
                    if (status_ == CREATE) {
                        // Make this instance progress to the PROCESS state.
                        status_ = PROCESS;

                        // As part of the initial CREATE state, we *request* that the system
                        // start processing requests. In this request, "this" acts are
                        // the tag uniquely identifying the request (so that different CallData
                        // instances can serve different requests concurrently), in this case
                        // the memory address of this CallData instance.
                        service_->RequestCF(&ctx_, &request_, &responder_, cq_, cq_,
                                this);
                    } else if (status_ == PROCESS) {
                        // Spawn a new CallData instance to serve new clients while we process
                        // the one for this CallData. The instance will deallocate itself as
                        // part of its FINISH state.
                        new CallData(service_, cq_);
                        // The actual processing.
                        ProcessRequest(request_, &reply_);
                        // And we are done! Let the gRPC runtime know we've finished, using the
                        // memory address of this instance as the uniquely identifying tag for
                        // the event.
                        status_ = FINISH;
                        responder_.Finish(reply_, Status::OK, this);
                    } else {
                        //GPR_ASSERT(status_ == FINISH);
                        // Once in the FINISH state, deallocate ourselves (CallData).
                        delete this;
                    }
                }
            private:
                // The means of communication with the gRPC runtime for an asynchronous
                // server.
                CFService::AsyncService* service_;
                // The producer-consumer queue where for asynchronous server notifications.
                ServerCompletionQueue* cq_;
                // Context for the rpc, allowing to tweak aspects of it such as the use
                // of compression, authentication, as well as to send metadata back to the
                // client.
                ServerContext ctx_;

                // What we get from the client.
                CFRequest request_;
                // What we send back to the client.
                CFResponse reply_;

                // The means to get back to the client.
                ServerAsyncResponseWriter<CFResponse> responder_;

                // Let's implement a tiny state machine with the following states.
                enum CallStatus { CREATE, PROCESS, FINISH };
                CallStatus status_;  // The current serving state.
        };

        // This can be run in multiple threads if needed.
        void HandleRpcs() {
            // Spawn a new CallData instance to serve new clients.
            new CallData(&service_, cq_.get());
            void* tag;  // uniquely identifies a request.
            bool ok;
            while (true) {
                // Block waiting to read the next event from the completion queue. The
                // event is uniquely identified by its tag, which in this case is the
                // memory address of a CallData instance.
                // The return value of Next should always be checked. This return value
                // tells us whether there is any kind of event or cq_ is shutting down.
                //GPR_ASSERT(cq_->Next(&tag, &ok));
                cq_->Next(&tag, &ok);
                /*auto r = cq_->AsyncNext(&tag, &ok, gpr_time_0(GPR_CLOCK_REALTIME));
                  if (r == ServerCompletionQueue::GOT_EVENT) {
                //GPR_ASSERT(ok);
                static_cast<CallData*>(tag)->Proceed();
                }
                if (r == ServerCompletionQueue::TIMEOUT) continue;*/
                //GPR_ASSERT(ok);
                static_cast<CallData*>(tag)->Proceed();
            }
        }

        std::unique_ptr<ServerCompletionQueue> cq_;
        CFService::AsyncService service_;
        std::unique_ptr<Server> server_;
};

int main(int argc, char** argv) {
    std::string dataset_file_name;
    if (argc == 7) {
        try
        {
            dataset_file_name = argv[1];
        }
        catch(...)
        {
            CHECK(false, "Enter a valid string for dataset file path\n");
        }
    } else {
        CHECK(false, "Format: ./<cf_server> <dataset file path> <IP address:Port Number> <Mode 1 - read dataset from text file OR Mode 2 - read dataset from binary file > <num of cores: -1 if you want all cores on the machine> <cf server number> <number of cf servers in the system>\n");
    }
    std::cout << "Reading variables..." << "\n";	
    // Load the cf server IP
    ip_port = argv[2];
    int mode = atoi(argv[3]);
    num_cores = atoi(argv[4]);
    if ( (num_cores == -1) || (num_cores > GetNumProcs()) ) {
        num_cores = GetNumProcs();
    }
    cf_parallelism = num_cores;
    cf_server_number = atoi(argv[5]);
    num_cf_servers = atoi(argv[6]);

    std::cout << "Creating dataset..." << "\n";   
    CreateDatasetFromFile(dataset_file_name, &dataset);

    std::cout << "Setting up matrix..." << "\n";   
    //CF cf_matrix(dataset, amf::NMFALSFactorizer(), 5, 5);    
    cf_matrix = new CF(dataset, amf::NMFALSFactorizer(), 5, 5);    
    //cf_matrix->Init();

    std::cout << "Starting server..." << "\n";   
    ServiceImpl server;
    server.Run();
    return 0;
}
