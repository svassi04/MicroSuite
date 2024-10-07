/* Author: Akshitha Sriraman
   Ph.D. Candidate at the University of Michigan - Ann Arbor*/

#include <iostream>
#include <memory>
#include <random>
#include <stdlib.h> 
#include <string>
#include <sys/time.h>
#include <unistd.h>

#include <grpc++/grpc++.h>
#include <thread>

#include "recommender_service/src/atomics.cpp"
#include "load_generator/helper_files/loadgen_recommender_client_helper.h"
#include "recommender_service/service/helper_files/timing.h"
#include "recommender_service/service/helper_files/utils.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using recommender::RecommenderRequest;
using recommender::UtilRequest;
using recommender::UtilResponse;
using recommender::RecommenderResponse;
using recommender::RecommenderService;

unsigned int number_of_cf_servers = 0;
Atomics* num_requests = new Atomics();
Atomics* responses_recvd = new Atomics();
Atomics* util_requests = new Atomics();

float qps = 0.0;
std::string cpu = "";
std::string ip = "localhost";
bool resp_recvd = false, kill_ack = false;
std::mutex resp_recvd_mutex, last_req_mutex;
std::mutex responses_recvd_mutex, start_counter_mutex, outstanding_mutex, global_stats_mutex, kill_ack_lock;
std::string timing_file_name = "", qps_file_name = "";
uint64_t interval_start_time = GetTimeInSec();
UtilInfo* previous_util = new UtilInfo();
GlobalStats* global_stats = new GlobalStats();
bool last_request = false, start_counter = false, first_req_flag = false;
uint64_t outstanding = 0;
std::vector<uint64_t> recommender_times, cf_times;
std::map<uint64_t, uint64_t> resp_times;
uint64_t c6_residency_start=0, c6_residency_end=0;
int num_inline = 0, num_workers = 0, num_resp = 0;

std::map<uint64_t, uint64_t> start_map;
std::vector<uint64_t> end_vec;
std::vector<uint64_t> start_vec;

class RecommenderServiceClient {
    public:
        explicit RecommenderServiceClient(std::shared_ptr<Channel> channel)
            : stub_(RecommenderService::NewStub(channel)) {}

        // Assembles the client's payload and sends it to the server.
        void Recommender(const std::pair<int, int> &query,
                const bool util_request,
                const bool kill) 
        {
            RecommenderRequest recommender_request;
            uint64_t start = GetTimeInMicro();
            CreateRecommenderServiceRequest(query,
                    util_request,
                    &recommender_request);
            recommender_request.set_kill(kill);

            recommender_request.set_last_request(last_request);
            recommender_request.set_resp_time(GetTimeInMicro());

            // Call object to store rpc data
            AsyncClientCall* call = new AsyncClientCall;
            uint64_t request_id = num_requests->AtomicallyReadCount();
            recommender_request.set_request_id(request_id);
            resp_times[request_id] = GetTimeInMicro();

            call->recommender_reply.set_create_recommender_req_time(GetTimeInMicro() - start);

            // stub_->AsyncSayHello() performs the RPC call, returning an instance to
            // store in "call". Because we are using the asynchronous API, we need to
            // hold on to the "call" instance in order to get updates on the ongoing RPC.

            try {
                call->response_reader = stub_->AsyncRecommender(&call->context, recommender_request, &cq_);

                // Request that, upon completion of the RPC, "reply" be updated with the
                // server's response; "status" with the indication of whether the operation
                // was successful. Tag the request with the memory address of the call object.
                call->response_reader->Finish(&call->recommender_reply, &call->status, (void*)call);
            } catch( ... ) {
                CHECK(false, " * ");
            }
        }

        // Loop while listening for completed responses.
        // Prints out the response from the server.
        void AsyncCompleteRpc() {
            void* got_tag;
            bool ok = false;
            //std::string timing_file_name_final = timing_file_name + std::to_string(qps) + ".txt";
            //std::ofstream timing_file;
            // Block until the next result is available in the completion queue "cq".
            //while(true)
            //{
            //auto r = cq_.AsyncNext(&got_tag, &ok, gpr_time_0(GPR_CLOCK_REALTIME));
            //if (r == CompletionQueue::GOT_EVENT) {
            while (cq_.Next(&got_tag, &ok)) {

                //timing_file.open(timing_file_name_final, std::ios_base::app);
                // The tag in this example is the memory location of the call object
                AsyncClientCall* call = static_cast<AsyncClientCall*>(got_tag);
                // Verify that the request was completed successfully. Note that "ok"
                // corresponds solely to the request for updates introduced by Finish().
                //GPR_ASSERT(ok);

                if (call->status.ok())
                {
                    kill_ack_lock.lock();
                    if (call->recommender_reply.kill_ack()) {
                        kill_ack = true;
                        std::cout << "got kill ack\n";
                        std::cout << std::flush;
                    }
                    kill_ack_lock.unlock();

                    if(util_requests->AtomicallyReadCount() == 1 && !first_req_flag) {
                        first_req_flag = true;
                        number_of_cf_servers = call->recommender_reply.number_of_cf_servers();
                        previous_util->cf_srv_util = new Util[number_of_cf_servers];
                        global_stats->percent_util_info.cf_srv_util_percent = new PercentUtil[number_of_cf_servers];
                    }

                    uint64_t request_id = call->recommender_reply.request_id();
                    resp_times[request_id] = GetTimeInMicro() - resp_times[request_id];
                    resp_recvd = true;
                    uint64_t start_time, end_time;
                    float rating = 0.0;
                    TimingInfo* timing_info = new TimingInfo();
                    PercentUtilInfo* percent_util_info = new PercentUtilInfo();
                    percent_util_info->cf_srv_util_percent = new PercentUtil[number_of_cf_servers];
                    start_time = GetTimeInMicro();
                    UnpackRecommenderServiceResponse(call->recommender_reply,
                            &rating,
                            timing_info,
                            previous_util,
                            percent_util_info);
                    end_time = GetTimeInMicro();

                    timing_info->unpack_recommender_resp_time = end_time - start_time;
                    timing_info->create_recommender_req_time = call->recommender_reply.create_recommender_req_time();
                    timing_info->update_recommender_util_time = call->recommender_reply.update_recommender_util_time();
                    timing_info->get_cf_srv_responses_time = call->recommender_reply.get_cf_srv_responses_time();
                    timing_info->total_resp_time = resp_times[request_id];

                    resp_times[request_id] = 0;


                    global_stats_mutex.lock();
                    UpdateGlobalTimingStats(*timing_info,
                            global_stats);
                    global_stats_mutex.unlock();

                    if((util_requests->AtomicallyReadCount() != 1) && (call->recommender_reply.util_response().util_present())) {
                        UpdateGlobalUtilStats(percent_util_info,
                                number_of_cf_servers,
                                global_stats);
                    }

                    responses_recvd->AtomicallyIncrementCount();
                    if (responses_recvd->AtomicallyReadCount() == 1) { 
                        // Print the index config that we got this data for.
                        std::cout << call->recommender_reply.num_inline() << " " << call->recommender_reply.num_workers() << " " << call->recommender_reply.num_resp() << " ";

                    }

                } else {
                    sleep(2);
                    std::string s = "./kill_recommender_server_empty " + ip;
                    char* cmd = new char[s.length() + 1];
                    std::strcpy(cmd, s.c_str());
                    ExecuteShellCommand(cmd);
                    std::cout << "Load generator failed\n";
                    CHECK(false, "");
                }

                delete call;
            } 
        }

            private:

        // struct for keeping state and data information
        struct AsyncClientCall {
            // Container for the data we expect from the server.
            RecommenderResponse recommender_reply;

            // Context for the client. It could be used to convey extra information to
            // the server and/or tweak certain RPC behaviors.
            ClientContext context;

            // Storage for the status of the RPC upon completion.
            Status status;


            std::unique_ptr<ClientAsyncResponseReader<RecommenderResponse>> response_reader;
        };

        // Out of the passed in Channel comes the stub, stored here, our view of the
        // server's exposed services.
        std::unique_ptr<RecommenderService::Stub> stub_;

        // The producer-consumer queue we use to communicate asynchronously with the
        // gRPC runtime.
        CompletionQueue cq_;
        };

        int main(int argc, char** argv) {
            std::string queries_file_name, result_file_name;
            struct LoadGenCommandLineArgs* load_gen_command_line_args = new struct LoadGenCommandLineArgs();
            load_gen_command_line_args = ParseLoadGenCommandLine(argc,
                    argv);
            queries_file_name = load_gen_command_line_args->queries_file_name;
            result_file_name = load_gen_command_line_args->result_file_name;
            uint64_t time_duration = load_gen_command_line_args->time_duration;
            qps = load_gen_command_line_args->qps;
            ip = load_gen_command_line_args->ip;
            cpu = load_gen_command_line_args->cpu;
            CHECK((time_duration >= 0), "ERROR: Offered load (time in seconds) must always be a positive value");
            struct TimingInfo timing_info;
            uint64_t start_time, end_time;
            start_time = GetTimeInMicro();
            
            std::vector<std::pair<int, int> > queries;
            CreateQueriesFromFile(queries_file_name,
                    &queries);
            end_time = GetTimeInMicro();
            timing_info.create_queries_time = end_time - start_time;

            std::string ip_port = ip;
            RecommenderServiceClient recommender_client(grpc::CreateChannel(
                        ip_port, grpc::InsecureChannelCredentials()));

            // Spawn reader thread that loops indefinitely
            std::thread thread_ = std::thread(&RecommenderServiceClient::AsyncCompleteRpc, &recommender_client);
            long queries_size = queries.size();
            uint64_t query_id = rand() % queries_size;
            std::pair<int, int> query = queries[query_id];

            double center = 1000000.0/(double)(qps);
            double curr_time = (double)GetTimeInMicro();
            double exit_time = curr_time + (double)(time_duration*1000000.0);

            std::default_random_engine generator;
            std::poisson_distribution<int> distribution(center);
            double next_time = distribution(generator) + curr_time;
            std::string line;

            //warmup phase
            int c6_counter = 0;
            std::ifstream myfile;
            
            while (responses_recvd->AtomicallyReadCount() != 50)
            {
            
                //check if responses received == requests send
                myfile.open("/sys/devices/system/cpu/cpu" + cpu + "/cpuidle/state3/time");
                if ( responses_recvd->AtomicallyReadCount() == num_requests->AtomicallyReadCount())
                {
                    //check if c6 enabled
                    if (myfile) 
                    {
                        getline (myfile, line);
                        c6_residency_end = std::stoul(line, nullptr, 10);
                    }
                    else
                    {
                        c6_residency_end = std::stoul("100", nullptr, 10);
                    }
                    myfile.close();
                    
                    
                    //send request
                    if (c6_residency_end > c6_residency_start && num_requests->AtomicallyReadCount()!=0 )
                    {
                        c6_counter = c6_counter + 1;
                    }

                    num_requests->AtomicallyIncrementCount();
                    recommender_client.Recommender(query,
                                false,
                                false);  
                    
                    sleep(1);

                    //check if c6 enabled
                    myfile.open("/sys/devices/system/cpu/cpu" + cpu + "/cpuidle/state3/time");
                    if (myfile) 
                    {
                        getline (myfile, line);
                        c6_residency_start = std::stoul(line, nullptr, 10);
                    }
                    else
                    {
                        c6_residency_start = std::stoul("0", nullptr, 10);
                    }
                    myfile.close();
                

                    sleep(6);

                    query_id = rand() % queries_size;
                    query = queries[query_id];
                }
            }

            myfile.close();
            myfile.open("/sys/devices/system/cpu/cpu" + cpu + "/cpuidle/state3/time");
            if (myfile) 
            {
                getline (myfile, line);
                c6_residency_end = std::stoul(line, nullptr, 10);
            }
            else
            {
                c6_residency_end = std::stoul("100", nullptr, 10);
            }
            myfile.close();
            if (c6_residency_end > c6_residency_start && num_requests->AtomicallyReadCount()!=0 )
            {
                c6_counter = c6_counter + 1;
            }

            if ( c6_counter == 0 )
            {
                std::cout << "Warmup C6 Transitions: " << c6_counter << "\n";
                std::cout << "Warmup Troughput: " << responses_recvd->AtomicallyReadCount()  << "\n";
                exit(1);
            }
            std::cout << "Warmup C6 Transitions: " << c6_counter << "\n";
            std::cout << "Warmup Troughput: " << responses_recvd->AtomicallyReadCount()  << "\n";


            
            //actual run
            c6_counter=0;
            query_id = rand() % queries_size;
            query = queries[query_id];
            while (responses_recvd->AtomicallyReadCount() != 100)
            {
                
                //check if responses received == requests send
                if ( responses_recvd->AtomicallyReadCount() == num_requests->AtomicallyReadCount())
                {
                    //check if c6 enabled
                    myfile.open("/sys/devices/system/cpu/cpu" + cpu + "/cpuidle/state3/time");
                    if (myfile) 
                    {
                        getline (myfile, line);
                        c6_residency_end = std::stoul(line, nullptr, 10);
                    }
                    else
                    {
                        c6_residency_end = std::stoul("100", nullptr, 10);
                    }
                    myfile.close();

                    //send request
                    if (c6_residency_end > c6_residency_start && num_requests->AtomicallyReadCount()!=50 )
                    {
                        c6_counter = c6_counter + 1;
                    }

                    //send request
                    num_requests->AtomicallyIncrementCount();
                    if((num_requests->AtomicallyReadCount() == 51) )
                    {
                        util_requests->AtomicallyIncrementCount();
                        recommender_client.Recommender(query,
                                true,
                                false);
                    }
                    else
                    {
                        recommender_client.Recommender(query,
                                false,
                                false);  
                    }

                    sleep(1);

                    //check if c6 enabled
                    myfile.open("/sys/devices/system/cpu/cpu" + cpu + "/cpuidle/state3/time");
                    if (myfile) 
                    {
                        getline (myfile, line);
                        c6_residency_start = std::stoul(line, nullptr, 10);
                    }
                    else
                    {
                        c6_residency_start = std::stoul("100", nullptr, 10);
                    }
                    myfile.close();

                    sleep(6);

                    query_id = rand() % queries_size;
                    query = queries[query_id];
                
                }
            }
            
            myfile.close();
            myfile.open("/sys/devices/system/cpu/cpu" + cpu + "/cpuidle/state3/time");
            if (myfile) 
            {
                getline (myfile, line);
                c6_residency_end = std::stoul(line, nullptr, 10);
            }
            else
            {
                c6_residency_end = std::stoul("100", nullptr, 10);
            }
            myfile.close();
            if (c6_residency_end > c6_residency_start && num_requests->AtomicallyReadCount()!=0 )
            {
                c6_counter = c6_counter + 1;
            }

            float achieved_qps = (float)responses_recvd->AtomicallyReadCount()/(float)time_duration;
            std::cout << "Actual Run C6 Transitions: " << c6_counter << "\n";
            std::cout << "Actual Run Troughput: " << responses_recvd->AtomicallyReadCount()  << "\n";
            
            global_stats_mutex.lock();
            PrintLatency(*global_stats,
                    number_of_cf_servers,
                    (util_requests->AtomicallyReadCount() - 1),
                    responses_recvd->AtomicallyReadCount());

            float query_cost = ComputeQueryCost(*global_stats, 
                    (util_requests->AtomicallyReadCount() - 1),
                    number_of_cf_servers, 
                    achieved_qps);
            global_stats_mutex.unlock();

            std::cout << query_cost << " ";
            std::cout << std::endl;
            exit(1);
            while (true) {
                //std::cout << "trying to send kill\n";
                std::cout << std::flush;
                kill_ack_lock.lock();
                if (kill_ack) {
                    //std::cout << "got kill ack dying\n";
                    //std::cout << std::flush;
                    CHECK(false, "");
                }
                kill_ack_lock.unlock();
                sleep(2);

                recommender_client.Recommender(query,
                        false,
                        true);
            }

            CHECK(false, "Load generator exiting\n");

            thread_.join(); 
            return 0;

        }
