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

// Define a default logger for gRPC
void rgpr_default_log(gpr_log_func_args* args) {
  args->severity >= GPR_LOG_SEVERITY_ERROR
  ? REprintf(args->message) : Rprintf(args->message);
  args->severity >= GPR_LOG_SEVERITY_ERROR
    ? REprintf("\n") : Rprintf("\n");
}

// Set gRPC default logger
[[cpp11::register]]
void bqs_init_logger() {
  gpr_set_log_function(rgpr_default_log);
}

// Set gRPC verbosity level
[[cpp11::register]]
void bqs_set_log_verbosity(bool verbose = false) {
  if (verbose) {
    gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);
  } else {
    gpr_set_log_verbosity(GPR_LOG_SEVERITY_ERROR);
  }
}

//' Check gRPC version
[[cpp11::register]]
std::string grpc_version() {
  std::string version;
  version += grpc_version_string();
  version += " ";
  version += grpc_g_stands_for();
  return version;
}

//' Simple read file to read configuration from json
std::string readfile(std::string filename)
{
  std::ifstream ifs(filename);
  std::string content( (std::istreambuf_iterator<char>(ifs) ),
                       (std::istreambuf_iterator<char>()    ) );
  return content;
}

//' append std::string at the end of a cpp11::raws vector
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
  ReadSession CreateReadSession(const std::string& project,
                                const std::string& dataset,
                                const std::string& table,
                                const std::string& parent,
                                const std::int64_t& timestamp_seconds,
                                const std::int32_t& timestamp_nanos,
                                const std::vector<std::string>& selected_fields,
                                const std::string& row_restriction
  ) {
    CreateReadSessionRequest method_request;
    ReadSession *read_session = method_request.mutable_read_session();
    std::string table_fullname = "projects/" + project + "/datasets/" + dataset + "/tables/" + table;
    read_session->set_table(table_fullname);
    read_session->set_data_format(DataFormat::ARROW);
    if (timestamp_seconds > 0 || timestamp_nanos > 0) {
      read_session->mutable_table_modifiers()->mutable_snapshot_time()->set_seconds(timestamp_seconds);
      read_session->mutable_table_modifiers()->mutable_snapshot_time()->set_nanos(timestamp_nanos);
    };
    if (!row_restriction.empty()) {
      read_session->mutable_read_options()->set_row_restriction(row_restriction);
    };
    if (selected_fields.size() > 0) {
      for (const std::string& field : selected_fields) {
        read_session->mutable_read_options()->add_selected_fields(field);
      };
    };
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
      err += "gRPC method CreateReadSession error -> ";
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
      err += "grpc method ReadRows error -> ";
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
cpp11::raws bqs_ipc_stream(std::string project,
                           std::string dataset,
                           std::string table,
                           std::string parent,
                           std::string client_info,
                           std::string service_configuration,
                           std::string access_token,
                           std::int64_t timestamp_seconds,
                           std::int32_t timestamp_nanos,
                           std::vector<std::string> selected_fields,
                           std::string row_restriction) {

  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  if (access_token.empty()) {
    channel_credentials = grpc::GoogleDefaultCredentials();
  } else {
    channel_credentials = grpc::CompositeChannelCredentials(
      grpc::SslCredentials(grpc::SslCredentialsOptions()),
      grpc::AccessTokenCredentials(access_token));
  };

  grpc::ChannelArguments channel_arguments;
  channel_arguments.SetServiceConfigJSON(readfile(service_configuration));

  BigQueryReadClient client(
      grpc::CreateCustomChannel("bigquerystorage.googleapis.com:443",
                                channel_credentials,
                                channel_arguments));

  client.SetClientInfo(client_info);

  cpp11::writable::raws ipc_stream;

  // Retrieve ReadSession
  ReadSession read_session = client.CreateReadSession(project,
                                                      dataset,
                                                      table,
                                                      parent,
                                                      timestamp_seconds,
                                                      timestamp_nanos,
                                                      selected_fields,
                                                      row_restriction);
  // Add schema to IPC stream
  to_raw(read_session.arrow_schema().serialized_schema(), &ipc_stream);

  // Add batches to IPC stream
  client.ReadRows(read_session.streams(0).name(), &ipc_stream);

  // Remove extra allocation
  ipc_stream.resize(ipc_stream.size());

  // Return stream
  return ipc_stream;
}
