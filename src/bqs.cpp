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
  std::string v;
  v += grpc_version_string();
  v += " ";
  v += grpc_g_stands_for();
  return v;
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
  void SetClientInfo(const std::string &clientInfo) {
    clientInfo_ = clientInfo;
  }
  ReadSession CreateReadSession(const std::string parent, const std::string &pj_id, const std::string &ds_id, const std::string &tb_id) {
    CreateReadSessionRequest rsrq;
    ReadSession *rs = rsrq.mutable_read_session();
    std::string table = "projects/" + pj_id + "/datasets/" + ds_id + "/tables/" + tb_id;
    rs->set_table(table);
    rs->set_data_format(DataFormat::ARROW);
    // Single stream for now;
    rsrq.set_max_stream_count(1);
    rsrq.set_parent("projects/" + parent);
    ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_session.table=" + table);
    context.AddMetadata("x-goog-api-client", clientInfo_);
    ReadSession rsrp;

    // The actual RPC.
    Status status = stub_->CreateReadSession(&context, rsrq, &rsrp);
    if (status.ok()) {
      return rsrp;
    } else {
      std::string err;
      err += "grpc CreateReadSession failed with code ";
      err += status.error_code();
      err += ": ";
      err += status.error_message();
      cpp11::stop(err.c_str());
    }

  }
  cpp11::list ReadRows(ReadSession &rs) {

    cpp11::writable::list srbv;

    ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + rs.streams(0).name());
    context.AddMetadata("x-goog-api-client", clientInfo_);

    ReadRowsRequest rrreq;
    rrreq.set_read_stream(rs.streams(0).name());
    rrreq.set_offset(0);

    ReadRowsResponse rrres;

    std::unique_ptr<ClientReader<ReadRowsResponse> > reader(
        stub_->ReadRows(&context, rrreq));
    while (reader->Read(&rrres)) {
      srbv.push_back(to_raw(rrres.arrow_record_batch().serialized_record_batch()));
      rrreq.set_offset(rrreq.offset() + rrres.row_count());
    }
    Status status = reader->Finish();
    if (status.ok()) {
    } else {
      stop("grpc ReadRows failed.");
    }
    return srbv;
  }
private:
  std::unique_ptr<BigQueryRead::Stub> stub_;
  std::string clientInfo_;
};

//' @noRd
[[cpp11::register]]
cpp11::list bqs_dl_arrow_batches(std::string parent, std::string pj_id, std::string ds_id, std::string tb_id, std::string clientInfo, std::string config) {
  grpc::ChannelArguments channel_args;
  channel_args.SetServiceConfigJSON(readfile(config));
  BigQueryReadClient client(
      grpc::CreateCustomChannel("bigquerystorage.googleapis.com:443",
                                grpc::GoogleDefaultCredentials(),
                                channel_args));
  client.SetClientInfo(clientInfo);

  ReadSession rs = client.CreateReadSession(parent, pj_id, ds_id, tb_id);

  cpp11::list res = client.ReadRows(rs);
  cpp11::raws schema = to_raw(rs.arrow_schema().serialized_schema());

  cpp11::writable::list li({"schema"_nm = schema, "arrow_batches"_nm = res});

  return li;
}
