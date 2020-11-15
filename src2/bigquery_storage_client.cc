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


//void rgpr_default_log(gpr_log_func_args* args) {
//  Rcout << args->message << std::endl;
//}

//' Check grpc version
//' @return Version string and what g stands for
//' @export
// [[cpp11::register]]
//CharacterVector grpc_version() {
//  return CharacterVector::create(grpc_version_string(), grpc_g_stands_for());
//}

std::string readfile(std::string filename)
{

  std::ifstream ifs(filename);
  std::string content( (std::istreambuf_iterator<char>(ifs) ),
                       (std::istreambuf_iterator<char>()    ) );

  return content;
}

CreateReadSessionRequest MakeCreateReadSessionRequest(std::string parent, ReadSession rs, ::google::protobuf::int32 msc = 0) {
  CreateReadSessionRequest rsr;
  rsr.set_parent(parent);
  rsr.set_allocated_read_session(&rs);
  rsr.set_max_stream_count(msc);
  return rsr;
};

ReadSession MakeReadSession(std::string project_id, std::string dataset_id, std::string table_id, ReadSession_TableReadOptions ro, ReadSession_TableModifiers tm) {
  // projects/{project_id}/datasets/{dataset_id}/tables/{table_id}
  ReadSession rs;
  rs.set_data_format(DataFormat::ARROW);
  rs.set_table("projects/" + project_id + "/datasets/" + dataset_id + "/tables/" + table_id);
  rs.set_allocated_read_options(&ro);
  rs.set_allocated_table_modifiers(&tm);
  return rs;
};

ReadSession_TableReadOptions MakeReadSession_TableReadOptions() {
  ReadSession_TableReadOptions o;
  return o;
};

class BigQueryReadClient {
public:
  BigQueryReadClient(std::shared_ptr<Channel> channel)
    : stub_(BigQueryRead::NewStub(channel)) {
  }
  void SetClient(const std::string &client) {
    client_ = client;
  }
  ReadSession CreateReadSession(const std::string &parent, const std::string &pj_id, const std::string &ds_id, const std::string &tb_id) {
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
    context.AddMetadata("x-goog-api-client", client_);
    ReadSession rsrp;

    // The actual RPC.
    Status status = stub_->CreateReadSession(&context, rsrq, &rsrp);
    if (status.ok()) {
      return rsrp;
    } else {
      std::cout << status.error_code() << ": " << status.error_message() << std::endl;
      return rsrp;
    }

  }
  std::vector<std::string> ReadRows(ReadSession &rs) {

    std::vector<std::string> srbv;

    ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + rs.streams(0).name());
    context.AddMetadata("x-goog-api-client", client_);

    ReadRowsRequest rrreq;
    rrreq.set_read_stream(rs.streams(0).name());
    rrreq.set_offset(0);

    ReadRowsResponse rrres;

    std::unique_ptr<ClientReader<ReadRowsResponse> > reader(
        stub_->ReadRows(&context, rrreq));
    while (reader->Read(&rrres)) {
      srbv.push_back(rrres.arrow_record_batch().serialized_record_batch());
      rrreq.set_offset(rrreq.offset() + rrres.row_count());
    }
    Status status = reader->Finish();
    if (status.ok()) {
      std::cout << "ListFeatures rpc succeeded." << std::endl;
    } else {
      std::cout << "ListFeatures rpc failed." << std::endl;
    }
    return srbv;
  }
private:
  std::unique_ptr<BigQueryRead::Stub> stub_;
  std::string client_;
};

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  grpc::ChannelArguments channel_args;
  channel_args.SetServiceConfigJSON(readfile("bigquerystorage_grpc_service_config.json"));
  BigQueryReadClient bqr_client(
      grpc::CreateCustomChannel("bigquerystorage.googleapis.com:443",
                                grpc::GoogleDefaultCredentials(),
                                channel_args));
  bqr_client.SetClient(argv[5]);


  std::cout << "-------------- Client created --------------" << std::endl;
  ReadSession rs = bqr_client.CreateReadSession(argv[1], argv[2], argv[3], argv[4]);
  std::vector<std::string> res = bqr_client.ReadRows(rs);
  std::cout << "Number of Record batches : " <<  res.size() << std::endl;

  std::string schema;
  rs.arrow_schema().SerializeToString(&schema);
  cpp11::list list()

  return 0;
}
