HDS_PATH = ../
CXX = g++
CPPFLAGS += -I/usr/local/include -pthread -O3 -I$(HDS_PATH) -g -Wall -fopenmp -I../
CXXFLAGS += -std=c++11 -O3 -mavx2 -mavx -fopenmp  -DMKL_ILP64 -m64 -I/opt/intel/mkl/include -I../
LDFLAGS += -L/usr/local/lib `pkg-config --libs grpc++ grpc` -lprotobuf -lpthread -I /usr/local/include -lflann -fopenmp -L/usr/lib64 -lstdc++ -lssl -lcrypto -fopenmp -Wl,--start-group /opt/intel/mkl/lib/intel64/libmkl_intel_ilp64.a /opt/intel/mkl/lib/intel64/libmkl_gnu_thread.a /opt/intel/mkl/lib/intel64/libmkl_core.a -Wl,--end-group -lgomp -lpthread -lm -ldl -I../
PROTOC = protoc
GRPC_CPP_PLUGIN = grpc_cpp_plugin
GRPC_CPP_PLUGIN_PATH ?= `which $(GRPC_CPP_PLUGIN)`

PROTOS_PATH = ../protoc_files
BUCKET_PATH = ../lookup_service
INDEX_PATH = ../mid_tier_service

vpath %.proto $(PROTOS_PATH)

all: system-check load_generator_open_loop_warmup load_generator_closed_loop load_generator_open_loop kill_router_server_empty

load_generator_closed_loop: $(PROTOS_PATH)/router.pb.o $(PROTOS_PATH)/router.grpc.pb.o helper_files/loadgen_router_client_helper.o $(INDEX_PATH)/service/helper_files/timing.o $(INDEX_PATH)/service/helper_files/utils.o load_generator_closed_loop.o
	$(CXX) $^ -O3 -o $@ $(LDFLAGS)

load_generator_open_loop: $(PROTOS_PATH)/router.pb.o $(PROTOS_PATH)/router.grpc.pb.o helper_files/loadgen_router_client_helper.o $(INDEX_PATH)/service/helper_files/timing.o $(INDEX_PATH)/service/helper_files/utils.o load_generator_open_loop.o 
	$(CXX) $^ -O3 -o $@ $(LDFLAGS)

load_generator_open_loop_warmup: $(PROTOS_PATH)/router.pb.o $(PROTOS_PATH)/router.grpc.pb.o helper_files/loadgen_router_client_helper.o $(INDEX_PATH)/service/helper_files/timing.o $(INDEX_PATH)/service/helper_files/utils.o load_generator_open_loop_warmup.o
	$(CXX) $^ -O3 -o $@ $(LDFLAGS)

kill_router_server_empty: $(PROTOS_PATH)/router.pb.o $(PROTOS_PATH)/router.grpc.pb.o helper_files/loadgen_router_client_helper.o $(INDEX_PATH)/service/helper_files/timing.o $(INDEX_PATH)/service/helper_files/utils.o kill_router_server_empty.o
	$(CXX) $^ -O3 -o $@ $(LDFLAGS)

.PRECIOUS: %.grpc.pb.cc
%.grpc.pb.cc: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --grpc_out=. --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) $<

.PRECIOUS: %.pb.cc
%.pb.cc: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --cpp_out=. $<

clean:
	rm -f *.o *.pb.cc *.pb.h load_generator_open_loop_warmup load_generator_closed_loop load_generator_open_loop kill_router_server_empty


# The following is to test your system and ensure a smoother experience.
# They are by no means necessary to actually compile a grpc-enabled software.

PROTOC_CMD = which $(PROTOC)
PROTOC_CHECK_CMD = $(PROTOC) --version | grep -q libprotoc.3
PLUGIN_CHECK_CMD = which $(GRPC_CPP_PLUGIN)
HAS_PROTOC = $(shell $(PROTOC_CMD) > /dev/null && echo true || echo false)
ifeq ($(HAS_PROTOC),true)
HAS_VALID_PROTOC = $(shell $(PROTOC_CHECK_CMD) 2> /dev/null && echo true || echo false)
endif
HAS_PLUGIN = $(shell $(PLUGIN_CHECK_CMD) > /dev/null && echo true || echo false)

SYSTEM_OK = false
ifeq ($(HAS_VALID_PROTOC),true)
ifeq ($(HAS_PLUGIN),true)
SYSTEM_OK = true
endif
endif

system-check:
ifneq ($(HAS_VALID_PROTOC),true)
	@echo " DEPENDENCY ERROR"
	@echo
	@echo "You don't have protoc 3.0.0 installed in your path."
	@echo "Please install Google protocol buffers 3.0.0 and its compiler."
	@echo "You can find it here:"
	@echo
	@echo "   https://github.com/google/protobuf/releases/tag/v3.0.0-beta-2"
	@echo
	@echo "Here is what I get when trying to evaluate your version of protoc:"
	@echo
	-$(PROTOC) --version
	@echo
	@echo
endif
ifneq ($(HAS_PLUGIN),true)
	@echo " DEPENDENCY ERROR"
	@echo
	@echo "You don't have the grpc c++ protobuf plugin installed in your path."
	@echo "Please install grpc. You can find it here:"
	@echo
	@echo "   https://github.com/grpc/grpc"
	@echo
	@echo "Here is what I get when trying to detect if you have the plugin:"
	@echo
	-which $(GRPC_CPP_PLUGIN)
	@echo
	@echo
endif
ifneq ($(SYSTEM_OK),true)
	@false
endif
