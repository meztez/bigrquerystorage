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
// #include <Rcpp.h>

// using namespace Rcpp;
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
// [[Rcpp::export]]
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


class BigQueryReadClient {
public:
  BigQueryReadClient(std::shared_ptr<Channel> channel)
    : stub_(BigQueryRead::NewStub(channel)) { }
  std::string CreateReadSession() {
    CreateReadSessionRequest rsrq;
    ReadSession rs;
    std::string table("projects/bigquery-public-data/datasets/usa_names/tables/usa_1910_current");
    rs.set_table(table);
    rs.set_data_format(DataFormat::ARROW);
    ReadSession_TableReadOptions ro;
    ro.add_selected_fields("name");
    ro.add_selected_fields("number");
    ro.add_selected_fields("state");
    ro.set_row_restriction("state = \"WA\"");
    rs.set_allocated_read_options(&ro);
    rsrq.set_parent("projects/labo-brunotremblay-253317");
    rsrq.set_max_stream_count(1);
    rsrq.set_allocated_read_session(&rs);
    ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_session.table=projects/bigquery-public-data/datasets/usa_names/tables/usa_1910_current");
    ReadSession rsrp;


    // The actual RPC.
    Status status = stub_->CreateReadSession(&context, rsrq, &rsrp);
    if (status.ok()) {
      std::cout << "-------------- Why --------------" << std::endl;
      return "OK";
    } else {
      std::cout << status.error_code() << ": " << status.error_message()
                << std::endl;
      return "RPC failed";
    }

  }
  void ReadRows() { }
private:
  std::unique_ptr<BigQueryRead::Stub> stub_;
};

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  grpc::ChannelArguments channel_args;
  channel_args.SetServiceConfigJSON(readfile("bigquerystorage_grpc_service_config.json"));
  BigQueryReadClient bqr_client(
      grpc::CreateCustomChannel("bigquerystorage.googleapis.com:443",
                                grpc::GoogleDefaultCredentials(),
                                channel_args));


  std::cout << "-------------- Client created --------------" << std::endl;

  std::string status = bqr_client.CreateReadSession();
  std::cout << "RPC Response status" <<  status << std::endl;

  return 0;
}
