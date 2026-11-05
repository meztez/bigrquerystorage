#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <grpc/grpc.h>
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#if __has_include(<grpcpp/version_info.h>)
#include <grpcpp/version_info.h>
#if GRPC_CPP_VERSION_MAJOR >= 1 & GRPC_CPP_VERSION_MINOR >= 66
#define ABSL_LOGGING 1
#endif
#endif

#ifndef ABSL_LOGGING
#include <grpc/support/log.h>
#endif

#ifdef ABSL_LOGGING
#include <absl/base/no_destructor.h>
#include <absl/base/log_severity.h>
#include <absl/log/globals.h>
#include <absl/log/initialize.h>
#include <absl/log/log_sink_registry.h>
#include <absl/log/log_sink.h>
#endif

#include <grpcpp/grpcpp.h>
#include "google/cloud/bigquery/storage/v1/stream.pb.h"
#include "google/cloud/bigquery/storage/v1/storage.pb.h"
# pragma GCC diagnostic ignored "-Winconsistent-missing-override"
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "google/cloud/bigquery/storage/v1/storage.grpc.pb.h"
#include <Rcpp.h>
#include "RProgress.h"

using google::cloud::bigquery::storage::v1::ReadSession;
using google::cloud::bigquery::storage::v1::BigQueryRead;
using google::cloud::bigquery::storage::v1::ArrowSerializationOptions;

// -- Utilities and logging ----------------------------------------------------
// Define a default logger for gRPC
#ifndef ABSL_LOGGING
void bqs_default_log(gpr_log_func_args* args) {
  Rcpp::Rcerr << args->message << std::endl;
}
#endif

#ifdef ABSL_LOGGING
class RLogSink : public absl::LogSink {
public:
  void Send(const absl::LogEntry& entry) override {
    Rcpp::Rcerr << entry.text_message_with_prefix_and_newline() << std::endl;
  }
};
#endif

// Set gRPC verbosity level
// [[Rcpp::export(rng=false)]]
void bqs_set_log_verbosity(int severity) {

#ifndef ABSL_LOGGING
  //-1 UNSET
  // 0 DEBUG
  // 1 INFO
  // 2 ERROR
  // 3 QUIET
  gpr_set_log_verbosity(static_cast<gpr_log_severity>(severity));
#endif

#ifdef ABSL_LOGGING
  // -1 UNSET
  //  0 INFO
  //  1 WARNING
  //  2 ERROR
  //  3 FATAL
  absl::SetMinLogLevel(static_cast<absl::LogSeverityAtLeast>(severity));
#endif

}

// Set gRPC default logger
// [[Rcpp::export(rng=false)]]
void bqs_init_logger() {

#ifndef ABSL_LOGGING
  gpr_set_log_function(bqs_default_log);
#endif

#ifdef ABSL_LOGGING
  absl::InitializeLog();
  static absl::NoDestructor<RLogSink> r_log_sink;
  absl::AddLogSink(r_log_sink.get());
#endif

  bqs_set_log_verbosity(2);
}

// Check gRPC version
// [[Rcpp::export(rng=false)]]
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

// -- Client class -------------------------------------------------------------

using google::cloud::bigquery::storage::v1::ReadRowsRequest;
using google::cloud::bigquery::storage::v1::ReadRowsResponse;

// Serialized Arrow record batches received from one read stream
struct StreamBatches {
  std::vector<std::string> batches;
  std::vector<int64_t> rows;
  size_t bytes = 0;

  void add(ReadRowsResponse* response) {
    std::string* batch = response->mutable_arrow_record_batch()->release_serialized_record_batch();
    bytes += batch->size();
    batches.push_back(std::move(*batch));
    delete batch;
    rows.push_back(response->row_count());
  }
};

// Shared state of a parallel multi-stream read
struct ParallelRead {
  std::atomic<bool> cancel{false};
  std::atomic<int64_t> rows{0};
  std::atomic<int64_t> pages{0};
  std::atomic<int> throttle{0};
  std::atomic<int> streams_done{0};
  int64_t n = -1;
  std::mutex mu;
  std::vector<grpc::ClientContext*> contexts;

  explicit ParallelRead(int n_workers) : contexts(n_workers, nullptr) {}

  void Register(int worker, grpc::ClientContext* ctx) {
    std::lock_guard<std::mutex> lock(mu);
    contexts[worker] = ctx;
    if (cancel.load()) ctx->TryCancel();
  }
  void Unregister(int worker) {
    std::lock_guard<std::mutex> lock(mu);
    contexts[worker] = nullptr;
  }
  // Stop every worker, including those blocked waiting for the next message
  void CancelAll() {
    cancel.store(true);
    std::lock_guard<std::mutex> lock(mu);
    for (auto* ctx : contexts) {
      if (ctx) ctx->TryCancel();
    }
  }
};

class BigQueryReadClient {
public:
  BigQueryReadClient(std::shared_ptr<grpc::ChannelCredentials> cred,
                     const std::string& target,
                     const std::string& service_configuration)
    : cred_(cred), target_(target), service_configuration_(service_configuration) {
    EnsureChannels(1);
  }
  void SetClientInfo(const std::string &client_info) {
    client_info_ = client_info;
  }

  // Keep a pool of channels so concurrent ReadRows calls each get their own
  // HTTP/2 connection instead of being multiplexed on a single one. Channels
  // need distinct arguments, otherwise gRPC shares the underlying subchannel.
  void EnsureChannels(size_t n) {
    while (stubs_.size() < n) {
      grpc::ChannelArguments channel_arguments;
      channel_arguments.SetMaxReceiveMessageSize(134217728);
      channel_arguments.SetServiceConfigJSON(service_configuration_);
      channel_arguments.SetInt("grpc.use_local_subchannel_pool", 1);
      channel_arguments.SetInt("bigrquerystorage.channel_id", static_cast<int>(stubs_.size()));
      stubs_.push_back(BigQueryRead::NewStub(
        grpc::CreateCustomChannel(target_, cred_, channel_arguments)));
    }
  }

  // Creation read sessions
  ReadSession CreateReadSession(const std::string& project,
                                const std::string& dataset,
                                const std::string& table,
                                const std::string& parent,
                                const std::int64_t& timestamp_seconds,
                                const std::int32_t& timestamp_nanos,
                                const std::vector<std::string>& selected_fields,
                                const std::string& row_restriction,
                                const std::double_t& sample_percentage,
                                const int32_t max_stream_count = 0,
                                const int32_t compression_codec = 0,
                                const int32_t preferred_min_stream_count = 0
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
    for (int i = 0; i < int(selected_fields.size()); i++) {
      read_session->mutable_read_options()->
        add_selected_fields(selected_fields[i]);
    }
    if (!row_restriction.empty()) {
      read_session->mutable_read_options()->
        set_row_restriction(row_restriction);
    }
    if (sample_percentage >= 0) {
      read_session->mutable_read_options()->
        set_sample_percentage(sample_percentage);
    }
    if (compression_codec == 1) {
      read_session->mutable_read_options()->
        mutable_arrow_serialization_options()->
        set_buffer_compression(ArrowSerializationOptions::LZ4_FRAME);
    } else if (compression_codec == 2) {
      read_session->mutable_read_options()->
        mutable_arrow_serialization_options()->
        set_buffer_compression(ArrowSerializationOptions::ZSTD);
    }
    if (max_stream_count > 0) {
      method_request.set_max_stream_count(max_stream_count);
    }
    if (preferred_min_stream_count > 0) {
      method_request.set_preferred_min_stream_count(preferred_min_stream_count);
    }
    method_request.set_parent("projects/" + parent);
    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params",
                        "read_session.table=" + table_fullname);
    context.AddMetadata("x-goog-api-client", client_info_);
    ReadSession method_response;

    // The actual RPC.
    grpc::Status status = stubs_[0]->
      CreateReadSession(&context, method_request, &method_response);
    if (!status.ok()) {
      std::string err;
      err += "gRPC method CreateReadSession error -> ";
      err += status.error_message();
      Rcpp::stop(err.c_str());
    }
    return method_response;
  }

  // Read rows from a stream (single-stream path with progress bar)
  void ReadRows(const std::string& stream,
                StreamBatches* out,
                std::int64_t& n,
                long int& rows_count,
                long int& pages_count,
                bool quiet,
                RProgress::RProgress* pb,
                bool last_stream) {

    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);

    ReadRowsRequest method_request;
    method_request.set_read_stream(stream);
    method_request.set_offset(0);

    ReadRowsResponse method_response;

    std::unique_ptr<grpc::ClientReader<ReadRowsResponse> > reader(
        stubs_[0]->ReadRows(&context, method_request));

    while (reader->Read(&method_response)) {
      out->add(&method_response);

      method_request.set_offset(
        method_request.offset() + method_response.row_count());
      pages_count += 1;
      if (!quiet) {
        if (method_response.has_throttle_state()) {
          pb->set_extra(method_response.throttle_state().throttle_percent());
        }
        if (n > 0) {
          if (method_request.offset() + rows_count >= n) {
            context.TryCancel();
            pb->update(1);
            break;
          }
          pb->tick(method_response.row_count());
        } else {
          pb->tick(
              (method_response.stats().progress().at_response_end()-
               method_response.stats().progress().at_response_start()) * 200);
        }
      } else {
        if (n > 0) {
          if (method_request.offset() + rows_count >= n) {
            context.TryCancel();
            break;
          }
        }
      }
      if (pages_count % 100 == 0L) {
        try {
          Rcpp::checkUserInterrupt();
        } catch (...) {
          context.TryCancel();
          reader->Finish();
          throw;
        }
      }
    }
    grpc::Status status = reader->Finish();
    if ((n < 0 && !status.ok()) || (n > 0 && !(status.ok() || status.error_code() == grpc::StatusCode::CANCELLED))) {
      std::string err;
      err += "grpc method ReadRows error -> ";
      err += status.error_message();
      Rcpp::stop(err.c_str());
    }
    rows_count += method_request.offset();
    if (last_stream && !quiet) {
      pb->update(1);
    }
  }

  // Read rows from a stream on a worker thread (no R API calls). Each worker
  // uses its own channel.
  void ReadRowsWorker(int worker,
                      const std::string& stream,
                      StreamBatches* out,
                      ParallelRead* state,
                      std::string* error_msg) {
    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);

    ReadRowsRequest method_request;
    method_request.set_read_stream(stream);
    method_request.set_offset(0);

    ReadRowsResponse method_response;
    state->Register(worker, &context);
    std::unique_ptr<grpc::ClientReader<ReadRowsResponse> > reader(
        stubs_[worker % stubs_.size()]->ReadRows(&context, method_request));

    while (reader->Read(&method_response)) {
      int64_t batch_rows = method_response.row_count();
      out->add(&method_response);
      if (method_response.has_throttle_state()) {
        state->throttle.store(method_response.throttle_state().throttle_percent());
      }
      state->pages.fetch_add(1);
      int64_t total = state->rows.fetch_add(batch_rows) + batch_rows;
      if (state->n > 0 && total >= state->n) {
        state->CancelAll();
        break;
      }
      if (state->cancel.load()) {
        break;
      }
    }
    grpc::Status status = reader->Finish();
    state->Unregister(worker);
    if (!status.ok() && !(state->cancel.load() && status.error_code() == grpc::StatusCode::CANCELLED)) {
      *error_msg = std::string("grpc method ReadRows error -> ") + status.error_message();
      state->CancelAll();
    }
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
    grpc::Status status = stubs_[0]->
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
  std::shared_ptr<grpc::ChannelCredentials> cred_;
  std::string target_;
  std::string service_configuration_;
  std::vector<std::unique_ptr<BigQueryRead::Stub>> stubs_;
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

  BigQueryReadClient *client = new BigQueryReadClient(
    cred, target, service_configuration
  );

  client->SetClientInfo(client_info);

  Rcpp::XPtr<BigQueryReadClient> ptr(client, true);

  return ptr;

}

// [[Rcpp::export(rng=false)]]
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

// Default number of concurrent ReadRows streams. Workers mostly wait on the
// network, so this is not tied to the number of CPU cores.
static const int BQS_DEFAULT_THREADS = 16;

// Download a table with the BigQuery Storage Read API.
//
// Returns a list with `schema` (the serialized Arrow schema) and `streams`, a
// list with one Arrow IPC buffer (schema + record batches) per read stream so
// the batches can be decoded in parallel. When `n` > 0, batches beyond the
// first `n` rows are dropped.
// [[Rcpp::export(rng=false)]]
SEXP bqs_ipc_stream(SEXP client,
                    std::string project,
                    std::string dataset,
                    std::string table,
                    std::string parent,
                    std::int64_t n,
                    std::vector<std::string> selected_fields,
                    std::string row_restriction = "",
                    std::double_t sample_percentage = -1,
                    std::int64_t timestamp_seconds = 0,
                    std::int32_t timestamp_nanos = 0,
                    bool quiet = false,
                    int32_t max_stream_count = 0,
                    int32_t compression_codec = 0,
                    int32_t max_threads = 0) {

  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);

  long int rows_count = 0;
  long int pages_count = 0;

  int target_threads = max_threads > 0 ? max_threads : BQS_DEFAULT_THREADS;

  int32_t requested_streams = max_stream_count;
  if (n > 0 && n <= 100000 && requested_streams == 0) {
    requested_streams = 1;
  }
  // Ask for at least as many streams as workers so the download can be parallel
  int32_t preferred_min_streams = target_threads;
  if (requested_streams > 0) {
    preferred_min_streams = std::min(requested_streams, preferred_min_streams);
  }

  // Retrieve ReadSession
  ReadSession read_session = client_ptr->CreateReadSession(
    project,
    dataset,
    table,
    parent,
    timestamp_seconds,
    timestamp_nanos,
    selected_fields,
    row_restriction,
    sample_percentage,
    requested_streams,
    compression_codec,
    preferred_min_streams);

  int n_streams = read_session.streams_size();
  const std::string& schema_bytes = read_session.arrow_schema().serialized_schema();

  std::vector<StreamBatches> stream_batches(std::max(1, n_streams));

  RProgress::RProgress pb(
      "\033[42m\033[30mStreaming (:percent)\033[39m\033[49m [:bar] eta[:eta|:elapsed] throt[:extra]");
  pb.set_cursor_char(">");

  if (n_streams <= 1) {
    if (n > 0) {
      pb.set_total(n);
    } else {
      pb.set_total(100 * std::max(1, n_streams));
    }
    for (int i = 0; i < n_streams; i++) {
      client_ptr->ReadRows(read_session.streams(i).name(),
                           &stream_batches[i],
                           n, rows_count, pages_count, quiet,
                           &pb, i == n_streams - 1);
    }
  } else {
    // Parallel multi-stream read, one channel per worker
    int num_threads = std::min(n_streams, target_threads);
    client_ptr->EnsureChannels(num_threads);

    ParallelRead state(num_threads);
    state.n = n;
    std::vector<std::string> errors(n_streams);
    std::atomic<int> next_stream(0);
    std::atomic<int> running(num_threads);
    std::mutex done_mu;
    std::condition_variable done_cv;

    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (int t = 0; t < num_threads; t++) {
      workers.emplace_back([&, t]() {
        while (!state.cancel.load()) {
          int idx = next_stream.fetch_add(1);
          if (idx >= n_streams) break;
          client_ptr->ReadRowsWorker(t, read_session.streams(idx).name(),
                                     &stream_batches[idx], &state, &errors[idx]);
          state.streams_done.fetch_add(1);
        }
        std::lock_guard<std::mutex> lock(done_mu);
        running.fetch_sub(1);
        done_cv.notify_all();
      });
    }

    // Progress is based on the estimated row count when available
    double expected_rows = static_cast<double>(read_session.estimated_row_count());
    if (n > 0 && (expected_rows <= 0 || n < expected_rows)) {
      expected_rows = static_cast<double>(n);
    }
    pb.set_total(100);

    auto join_all = [&]() {
      for (auto& w : workers) {
        if (w.joinable()) w.join();
      }
    };

    while (true) {
      {
        std::unique_lock<std::mutex> lock(done_mu);
        done_cv.wait_for(lock, std::chrono::milliseconds(100),
                         [&]() { return running.load() == 0; });
      }
      if (running.load() == 0) break;
      try {
        Rcpp::checkUserInterrupt();
      } catch (...) {
        state.CancelAll();
        join_all();
        throw;
      }
      if (!quiet) {
        double ratio = expected_rows > 0
          ? static_cast<double>(state.rows.load()) / expected_rows
          : static_cast<double>(state.streams_done.load()) / n_streams;
        pb.set_extra(state.throttle.load());
        pb.update(std::min(0.99, std::max(0.0, ratio)));
      }
    }
    join_all();

    for (int i = 0; i < n_streams; i++) {
      if (!errors[i].empty()) {
        Rcpp::stop(errors[i].c_str());
      }
    }
    rows_count = static_cast<long int>(state.rows.load());
    pages_count = static_cast<long int>(state.pages.load());
    if (!quiet) {
      pb.update(1);
    }
  }

  if (!quiet) {
    REprintf("Streamed %ld rows in %ld messages.\n", rows_count, pages_count);
  }

  // One IPC buffer per stream: the schema followed by its record batches,
  // keeping only what is needed to return the first `n` rows.
  Rcpp::List streams_out;
  int64_t kept_rows = 0;
  for (int i = 0; i < n_streams; i++) {
    StreamBatches& sb = stream_batches[i];
    size_t n_keep = 0;
    size_t bytes = schema_bytes.size();
    while (n_keep < sb.batches.size() && !(n > 0 && kept_rows >= n)) {
      kept_rows += sb.rows[n_keep];
      bytes += sb.batches[n_keep].size();
      n_keep++;
    }
    if (n_keep > 0) {
      Rcpp::RawVector buf(bytes);
      uint8_t* dst = RAW(buf);
      std::memcpy(dst, schema_bytes.data(), schema_bytes.size());
      dst += schema_bytes.size();
      for (size_t k = 0; k < n_keep; k++) {
        std::memcpy(dst, sb.batches[k].data(), sb.batches[k].size());
        dst += sb.batches[k].size();
      }
      streams_out.push_back(buf);
    }
    std::vector<std::string>().swap(sb.batches);
  }

  Rcpp::RawVector schema_out(schema_bytes.size());
  if (!schema_bytes.empty()) {
    std::memcpy(RAW(schema_out), schema_bytes.data(), schema_bytes.size());
  }

  return Rcpp::List::create(
    Rcpp::Named("schema") = schema_out,
    Rcpp::Named("streams") = streams_out
  );
}
