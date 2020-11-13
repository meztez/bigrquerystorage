#
# Copyright 2015 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

HOST_SYSTEM = $(shell uname | cut -f 1 -d_)
SYSTEM ?= $(HOST_SYSTEM)
CXX = g++
CPPFLAGS += `pkg-config --cflags protobuf grpc`
CXXFLAGS += -std=c++11 -I.
ifeq ($(SYSTEM),Darwin)
LDFLAGS += -L/usr/local/lib `pkg-config --libs protobuf grpc++`\
					 -pthread\
           -lgrpc++_reflection\
           -ldl\
           -I.
else
LDFLAGS += -L/usr/local/lib `pkg-config --libs protobuf grpc++`\
           -pthread\
           -Wl,--no-as-needed -lgrpc++_reflection -Wl,--as-needed\
           -ldl\
           -I.
endif
PROTOC = protoc
GRPC_CPP_PLUGIN = grpc_cpp_plugin
GRPC_CPP_PLUGIN_PATH ?= `which $(GRPC_CPP_PLUGIN)`

PROTOS_PATH = ../scripts/googleapis

vpath %.proto $(PROTOS_PATH)

all: system-check protoc_grpc_cpp bigquery_storage_client

bigquery_storage_client: google/api/resource.pb.o google/api/field_behavior.pb.o google/api/client.pb.o google/api/http.pb.o google/api/annotations.pb.o google/cloud/bigquery/storage/v1/arrow.pb.o google/cloud/bigquery/storage/v1/avro.pb.o google/cloud/bigquery/storage/v1/stream.pb.o google/cloud/bigquery/storage/v1/storage.pb.o google/api/resource.grpc.pb.o google/api/field_behavior.grpc.pb.o google/api/client.grpc.pb.o google/api/http.grpc.pb.o google/api/annotations.grpc.pb.o google/cloud/bigquery/storage/v1/arrow.grpc.pb.o google/cloud/bigquery/storage/v1/avro.grpc.pb.o google/cloud/bigquery/storage/v1/stream.grpc.pb.o google/cloud/bigquery/storage/v1/storage.grpc.pb.o bigquery_storage_client.o
	$(CXX) $^ $(LDFLAGS) -o $@

protoc_grpc_cpp: protoc/google/api/resource protoc/google/api/field_behavior protoc/google/api/client protoc/google/api/http protoc/google/api/annotations protoc/google/cloud/bigquery/storage/v1/arrow protoc/google/cloud/bigquery/storage/v1/avro protoc/google/cloud/bigquery/storage/v1/stream protoc/google/cloud/bigquery/storage/v1/storage

protoc/%: %.proto
	$(PROTOC) -I $(PROTOS_PATH) --grpc_out=. --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) $<
	$(PROTOC) -I $(PROTOS_PATH) --cpp_out=. $<
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $*.pb.o $*.pb.cc
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $*.grpc.pb.o $*.grpc.pb.cc

clean:
	rm -r -f *.o *.pb.cc *.pb.h google


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
	@echo "   https://github.com/google/protobuf/releases/tag/v3.0.0"
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
