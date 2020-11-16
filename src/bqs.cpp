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

using namespace cpp11;
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

cpp11::raws to_raw(const std::string input) {
  cpp11::writable::raws rv(input.size());
  for(unsigned long i=0; i<input.size(); i++) {
    rv[i] = input.c_str()[i];
  }
  return rv;
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
      err += "grpc CreateReadSession failed with code ";
      err += status.error_code();
      err += ": ";
      err += status.error_message();
      cpp11::stop(err.c_str());
    }
    return method_response;

  }
  cpp11::list ReadRows(const std::string &stream) {

    ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);

    ReadRowsRequest method_request;
    method_request.set_read_stream(stream);
    method_request.set_offset(0);

    cpp11::writable::list received_batches;
    ReadRowsResponse method_response;

    std::unique_ptr<ClientReader<ReadRowsResponse> > reader(
        stub_->ReadRows(&context, method_request));
    while (reader->Read(&method_response)) {
      received_batches.push_back(to_raw(method_response.arrow_record_batch().serialized_record_batch()));
      method_request.set_offset(method_request.offset() + method_response.row_count());
    }
    Status status = reader->Finish();
    if (!status.ok()) {
      stop("grpc ReadRows failed.");
    }
    return received_batches;
  }
private:
  std::unique_ptr<BigQueryRead::Stub> stub_;
  std::string client_info_;
};

//' @noRd
[[cpp11::register]]
cpp11::list bqs_dl_arrow_batches(std::string parent, std::string project, std::string dataset, std::string table, std::string client_info, std::string service_configuration) {
  grpc::ChannelArguments channel_arguments;
  channel_arguments.SetServiceConfigJSON(readfile(service_configuration));
  BigQueryReadClient client(
      grpc::CreateCustomChannel("bigquerystorage.googleapis.com:443",
                                grpc::GoogleDefaultCredentials(),
                                channel_arguments));

  client.SetClientInfo(client_info);

  ReadSession read_session = client.CreateReadSession(parent, project, dataset, table);

  cpp11::list client_response = client.ReadRows(read_session.streams(0).name());
  cpp11::raws schema = to_raw(read_session.arrow_schema().serialized_schema());

  cpp11::writable::list li({"schema"_nm = schema, "arrow_batches"_nm = client_response});

  return li;
}
