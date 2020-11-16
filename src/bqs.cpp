#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpc/impl/codegen/log.h>
#include "google/cloud/bigquery/storage/v1/arrow.pb.h"
#include "google/cloud/bigquery/storage/v1/avro.pb.h"
#include "google/cloud/bigquery/storage/v1/stream.pb.h"
#include "google/cloud/bigquery/storage/v1/storage.pb.h"
#include "google/cloud/bigquery/storage/v1/storage.grpc.pb.h"
#include <fstream>
#include <string>
#include <cpp11.hpp>
#include <Rinternals.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using grpc::ChannelArguments;
using google::cloud::bigquery::storage::v1::ArrowRecordBatch;
using google::cloud::bigquery::storage::v1::ArrowSchema;
using google::cloud::bigquery::storage::v1::AvroRows;
using google::cloud::bigquery::storage::v1::AvroSchema;
using google::cloud::bigquery::storage::v1::CreateReadSessionRequest;
using google::cloud::bigquery::storage::v1::DataFormat;
using google::cloud::bigquery::storage::v1::ReadRowsRequest;
using google::cloud::bigquery::storage::v1::ReadRowsResponse;
using google::cloud::bigquery::storage::v1::ReadSession;
using google::cloud::bigquery::storage::v1::ReadSession_TableModifiers;
using google::cloud::bigquery::storage::v1::ReadSession_TableReadOptions;
using google::cloud::bigquery::storage::v1::ReadStream;
using google::cloud::bigquery::storage::v1::SplitReadStreamRequest;
using google::cloud::bigquery::storage::v1::SplitReadStreamResponse;
using google::cloud::bigquery::storage::v1::StreamStats;
using google::cloud::bigquery::storage::v1::StreamStats_Progress;
using google::cloud::bigquery::storage::v1::ThrottleState;
using google::cloud::bigquery::storage::v1::BigQueryRead;

//' Check grpc version
//' @return Version string and what g stands for
//' @export
[[cpp11::register]]
std::string grpc_version() {
  std::string version;
  version += grpc_version_string();
  version += " ";
  version += grpc_g_stands_for();
  return version;
}

std::string readfile(std::string filename)
{

  std::ifstream ifs(filename);
  std::string content( (std::istreambuf_iterator<char>(ifs) ),
                       (std::istreambuf_iterator<char>()    ) );

  return content;
}

void to_raw(const std::string input, cpp11::writable::raws* output) {
  for(unsigned long i=0; i<input.size(); i++) {
    output->push_back(input.c_str()[i]);
  }
}

class BigQueryReadClient {
public:
  BigQueryReadClient(std::shared_ptr<Channel> channel)
    : stub_(BigQueryRead::NewStub(channel)) {
  }
  void SetClientInfo(const std::string &client_info) {
    client_info_ = client_info;
  }
  ReadSession CreateReadSession(const std::string parent, const std::string &project, const std::string &dataset, const std::string &table) {
    CreateReadSessionRequest method_request;
    ReadSession *read_session = method_request.mutable_read_session();
    std::string table_fullname = "projects/" + project + "/datasets/" + dataset + "/tables/" + table;
    read_session->set_table(table_fullname);
    read_session->set_data_format(DataFormat::ARROW);
    // Single stream for now;
    method_request.set_max_stream_count(1);
    method_request.set_parent("projects/" + parent);
    ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_session.table=" + table_fullname);
    context.AddMetadata("x-goog-api-client", client_info_);
    ReadSession method_response;

    // The actual RPC.
    Status status = stub_->CreateReadSession(&context, method_request, &method_response);
    if (!status.ok()) {
      std::string err;
      err += "grpc method CreateReadSession failed with code ";
      err += status.error_code();
      err += ": ";
      err += status.error_message();
      cpp11::stop(err.c_str());
    }
    return method_response;

  }
  void ReadRows(const std::string stream, cpp11::writable::raws* ipc_stream) {

    ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);

    ReadRowsRequest method_request;
    method_request.set_read_stream(stream);
    method_request.set_offset(0);

    ReadRowsResponse method_response;

    std::unique_ptr<ClientReader<ReadRowsResponse> > reader(
        stub_->ReadRows(&context, method_request));
    while (reader->Read(&method_response)) {
      to_raw(method_response.arrow_record_batch().serialized_record_batch(), ipc_stream);
      method_request.set_offset(method_request.offset() + method_response.row_count());
      R_CheckUserInterrupt();
    }
    Status status = reader->Finish();
    if (!status.ok()) {
      std::string err;
      err += "grpc method ReadRows failed with code ";
      err += status.error_code();
      err += ": ";
      err += status.error_message();
      cpp11::stop(err.c_str());
    }
  }
private:
  std::unique_ptr<BigQueryRead::Stub> stub_;
  std::string client_info_;
};

//' @noRd
[[cpp11::register]]
cpp11::raws bqs_ipc_stream(std::string parent, std::string project, std::string dataset, std::string table, std::string client_info, std::string service_configuration) {
  grpc::ChannelArguments channel_arguments;
  channel_arguments.SetServiceConfigJSON(readfile(service_configuration));
  BigQueryReadClient client(
      grpc::CreateCustomChannel("bigquerystorage.googleapis.com:443",
                                grpc::GoogleDefaultCredentials(),
                                channel_arguments));

  client.SetClientInfo(client_info);

  cpp11::writable::raws ipc_stream;

  ReadSession read_session = client.CreateReadSession(parent, project, dataset, table);

  // Add schema to IPC stream
  to_raw(read_session.arrow_schema().serialized_schema(), &ipc_stream);


  // Add batches to IPC stream
  client.ReadRows(read_session.streams(0).name(), &ipc_stream);

  // Remove extra allocation
  ipc_stream.resize(ipc_stream.size());

  // Return stream
  return ipc_stream;
}
