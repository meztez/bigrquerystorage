#include <fstream>
#include <string>
#include <vector>
#include <grpc/grpc.h>
#include <grpc/support/log.h>
#include <grpcpp/grpcpp.h>
#include "google/cloud/bigquery/storage/v1/stream.pb.h"
#include "google/cloud/bigquery/storage/v1/storage.pb.h"
#include "google/cloud/bigquery/storage/v1/storage.grpc.pb.h"
#include <Rcpp.h>
#include "RProgress.h"

using google::cloud::bigquery::storage::v1::ReadSession;
using google::cloud::bigquery::storage::v1::BigQueryRead;

// -- Utilities and logging ----------------------------------------------------

// Define a default logger for gRPC
void bqs_default_log(gpr_log_func_args* args) {
  Rcpp::Rcerr << args->message << std::endl;
}

// Set gRPC verbosity level
// [[Rcpp::export]]
void bqs_set_log_verbosity(int severity) {
  //-1 UNSET
  // 0 DEBUG
  // 1 INFO
  // 2 ERROR
  // 3 QUIET
  gpr_set_log_verbosity(static_cast<gpr_log_severity>(severity));
}

// Set gRPC default logger
// [[Rcpp::export]]
void bqs_init_logger() {
  gpr_set_log_function(bqs_default_log);
  bqs_set_log_verbosity(2);
}

// Check gRPC version
// [[Rcpp::export]]
std::string grpc_version() {
  std::string version;
  version += grpc_version_string();
  version += " ";
  version += grpc_g_stands_for();
  return version;
}

// Simple read file to read configuration from json
std::string readfile(std::string filename)
{
  std::ifstream ifs(filename);
  std::string content( (std::istreambuf_iterator<char>(ifs) ),
                       (std::istreambuf_iterator<char>()    ) );
  return content;
}

// append std::string at the end of a std::vector<uint8_t> vector
void to_raw(const std::string input, std::vector<uint8_t>* output) {
  output->insert(output->end(), input.begin(), input.end());
}

// -- Client class -------------------------------------------------------------

class BigQueryReadClient {
public:
  BigQueryReadClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(BigQueryRead::NewStub(channel)) {
  }
  void SetClientInfo(const std::string &client_info) {
    client_info_ = client_info;
  }

  // Creation read sessions
  ReadSession CreateReadSession(const std::string& project,
                                const std::string& dataset,
                                const std::string& table,
                                const std::string& parent,
                                const std::int64_t& timestamp_seconds,
                                const std::int32_t& timestamp_nanos,
                                const std::vector<std::string>& selected_fields,
                                const std::string& row_restriction
  ) {
    google::cloud::bigquery::storage::v1::CreateReadSessionRequest method_request;
    ReadSession *read_session = method_request.mutable_read_session();
    std::string table_fullname =
      "projects/" + project + "/datasets/" + dataset + "/tables/" + table;
    read_session->set_table(table_fullname);
    read_session->set_data_format(
        google::cloud::bigquery::storage::v1::DataFormat::ARROW);
    if (timestamp_seconds > 0 || timestamp_nanos > 0) {
      read_session->mutable_table_modifiers()->
        mutable_snapshot_time()->set_seconds(timestamp_seconds);
      read_session->mutable_table_modifiers()->
        mutable_snapshot_time()->set_nanos(timestamp_nanos);
    }
    if (!row_restriction.empty()) {
      read_session->mutable_read_options()->
        set_row_restriction(row_restriction);
    }
    for (int i = 0; i < int(selected_fields.size()); i++) {
      read_session->mutable_read_options()->
        add_selected_fields(selected_fields[i]);
    }
    method_request.set_parent("projects/" + parent);
    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params",
                        "read_session.table=" + table_fullname);
    context.AddMetadata("x-goog-api-client", client_info_);
    ReadSession method_response;

    // The actual RPC.
    grpc::Status status = stub_->
      CreateReadSession(&context, method_request, &method_response);
    if (!status.ok()) {
      std::string err;
      err += "gRPC method CreateReadSession error -> ";
      err += status.error_message();
      Rcpp::stop(err.c_str());
    }
    return method_response;
  }

  // Read rows from a stream
  void ReadRows(const std::string stream,
                std::vector<uint8_t>* ipc_stream,
                std::int64_t& n,
                long int& rows_count,
                long int& pages_count,
                bool quiet) {

    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);

    google::cloud::bigquery::storage::v1::ReadRowsRequest method_request;
    method_request.set_read_stream(stream);
    method_request.set_offset(0);

    google::cloud::bigquery::storage::v1::ReadRowsResponse method_response;

    std::unique_ptr<grpc::ClientReader<google::cloud::bigquery::storage::v1::ReadRowsResponse> > reader(
        stub_->ReadRows(&context, method_request));

    RProgress::RProgress pb(
        "\033[42m\033[30mReading (:percent)\033[39m\033[49m [:bar] ETA :eta|:elapsed");
    pb.set_cursor_char(">");
    if (n > 0) {
      pb.set_total(n);
    }

    while (reader->Read(&method_response)) {
      to_raw(method_response.arrow_record_batch().serialized_record_batch(),
             ipc_stream);
      method_request.set_offset(
        method_request.offset() + method_response.row_count());
      pages_count += 1;
      if (!quiet) {
        if (n > 0) {
          if (method_request.offset() + rows_count >= n) {
            pb.update(1);
            break;
          }
          pb.tick(method_response.row_count());
        } else {
          pb.update(method_response.stats().progress().at_response_end() * 2);
        }
      } else {
        Rcpp::checkUserInterrupt();
      }
    }
    if (n < 0) {
      grpc::Status status = reader->Finish();
      if (!status.ok()) {
        std::string err;
        err += "grpc method ReadRows error -> ";
        err += status.error_message();
        Rcpp::stop(err.c_str());
      }
    }
    rows_count += method_request.offset();
  }

  // Split stream
  std::vector<std::string> SplitReadStream(
      std::string& stream, double& fraction) {

    google::cloud::bigquery::storage::v1::SplitReadStreamRequest method_request;
    method_request.set_name(stream);
    method_request.set_fraction(fraction);

    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params",
                        "name=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);
    google::cloud::bigquery::storage::v1::SplitReadStreamResponse method_response;

    // The actual RPC.
    grpc::Status status = stub_->
      SplitReadStream(&context, method_request, &method_response);
    if (!status.ok()) {
      std::string err;
      err += "gRPC method SplitReadStream error -> ";
      err += status.error_message();
      Rcpp::stop(err.c_str());
    }

    return {method_response.primary_stream().name(),
            method_response.remainder_stream().name()};
  }
private:
  std::unique_ptr<BigQueryRead::Stub> stub_;
  std::string client_info_;
};



// -- Credentials functions ----------------------------------------------------

std::shared_ptr<grpc::ChannelCredentials> bqs_ssl(
    std::string root_certificate) {
  grpc::SslCredentialsOptions ssl_options;
  if (!root_certificate.empty()) {
    ssl_options.pem_root_certs = root_certificate;
  }
  return grpc::SslCredentials(ssl_options);
}

std::shared_ptr<grpc::ChannelCredentials> bqs_credentials(
    std::shared_ptr<grpc::ChannelCredentials> channel_cred,
    std::shared_ptr<grpc::CallCredentials> call_cred = nullptr) {
  if (channel_cred) {
    if (call_cred) {
      channel_cred = grpc::CompositeChannelCredentials(
        channel_cred,
        call_cred
      );
    }
  }
  return channel_cred == nullptr ? nullptr : channel_cred;
}

std::shared_ptr<grpc::ChannelCredentials> bqs_google_credentials() {
  auto gcp_cred = grpc::GoogleDefaultCredentials();
  return bqs_credentials(gcp_cred);
}

std::shared_ptr<grpc::ChannelCredentials> bqs_refresh_token_credentials(
    std::string refresh_token, std::string root_certificate = "") {
  auto ssl_cred = bqs_ssl(root_certificate);
  auto token_cred = grpc::GoogleRefreshTokenCredentials(refresh_token);
  return bqs_credentials(ssl_cred, token_cred);
}

std::shared_ptr<grpc::ChannelCredentials> bqs_access_token_credentials(
    std::string access_token, std::string root_certificate = "") {
  auto ssl_cred = bqs_ssl(root_certificate);
  auto token_cred = grpc::AccessTokenCredentials(access_token);
  return bqs_credentials(ssl_cred, token_cred);
}

// -- Client functions ---------------------------------------------------------

SEXP bqs_read_client(std::shared_ptr<grpc::ChannelCredentials> cred,
                     std::string client_info,
                     std::string service_configuration,
                     std::string target) {

  grpc::ChannelArguments channel_arguments;
  channel_arguments.SetServiceConfigJSON(service_configuration);

  BigQueryReadClient *client = new BigQueryReadClient(
    grpc::CreateCustomChannel(target, cred, channel_arguments)
  );

  client->SetClientInfo(client_info);

  Rcpp::XPtr<BigQueryReadClient> ptr(client, true);

  return ptr;

}

// [[Rcpp::export]]
SEXP bqs_client(std::string client_info,
                std::string service_configuration,
                std::string refresh_token = "",
                std::string access_token = "",
                std::string root_certificate = "",
                std::string target = "bigquerystorage.googleapis.com:443") {

  std::shared_ptr<grpc::ChannelCredentials> cred;
  if (!refresh_token.empty()) {
    cred = bqs_refresh_token_credentials(refresh_token,
                                         readfile(root_certificate));
  }
  if (!cred && !access_token.empty()) {
    cred = bqs_access_token_credentials(access_token,
                                        readfile(root_certificate));
  }
  if (!cred) {
    cred = bqs_google_credentials();
  }
  if (!cred) {
    Rcpp::stop("Could not create credentials.");
  }

  return bqs_read_client(cred, client_info,
                         readfile(service_configuration), target);

}

// [[Rcpp::export]]
SEXP bqs_ipc_stream(SEXP client,
                    std::string project,
                    std::string dataset,
                    std::string table,
                    std::string parent,
                    std::int64_t n,
                    std::vector<std::string> selected_fields,
                    std::string row_restriction = "",
                    std::int64_t timestamp_seconds = 0,
                    std::int32_t timestamp_nanos = 0,
                    bool quiet = false) {

  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);

  std::vector<uint8_t> schema;
  std::vector<uint8_t> ipc_stream;
  long int rows_count = 0;
  long int pages_count = 0;

  // Retrieve ReadSession
  ReadSession read_session = client_ptr->CreateReadSession(
    project,
    dataset,
    table,
    parent,
    timestamp_seconds,
    timestamp_nanos,
    selected_fields,
    row_restriction);
  // Add schema to IPC stream
  to_raw(read_session.arrow_schema().serialized_schema(), &schema);

  // Add batches to IPC stream
  for (int i = 0; i < read_session.streams_size(); i++) {
    client_ptr->ReadRows(read_session.streams(i).name(), &ipc_stream,
                         n, rows_count, pages_count, quiet);
  }

  if (!quiet) {
    REprintf("Streamed %ld rows in %ld messages.\n", rows_count, pages_count);
  }

  // Return stream
  return Rcpp::List::create(schema, ipc_stream);
}
