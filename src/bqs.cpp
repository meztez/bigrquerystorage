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
#ifdef __clang__
# pragma GCC diagnostic ignored "-Winconsistent-missing-override"
#endif
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "google/cloud/bigquery/storage/v1/storage.grpc.pb.h"
#include <Rcpp.h>
#include <nanoarrow/r.h>
#include "RProgress.h"
#include "bqs_arrow_ipc.h"

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
    EnsureChannels(24, true);
    EnsureWriteChannels(12, true);
  }
  void SetClientInfo(const std::string &client_info) {
    client_info_ = client_info;
  }

  // Keep a pool of channels so concurrent ReadRows calls each get their own
  // HTTP/2 connection instead of being multiplexed on a single one. Channels
  // need distinct arguments, otherwise gRPC shares the underlying subchannel.
  void EnsureChannels(size_t n, bool warm_connect = true) {
    while (stubs_.size() < n) {
      grpc::ChannelArguments channel_arguments;
      channel_arguments.SetMaxReceiveMessageSize(134217728);
      channel_arguments.SetServiceConfigJSON(service_configuration_);
      channel_arguments.SetInt("grpc.use_local_subchannel_pool", 1);
      channel_arguments.SetInt("grpc.http2.bdp_probe", 1);
      channel_arguments.SetInt("grpc.http2.lookahead_bytes", 32 * 1024 * 1024);
      channel_arguments.SetInt("bigrquerystorage.channel_id", static_cast<int>(stubs_.size()));
      auto channel = grpc::CreateCustomChannel(target_, cred_, channel_arguments);
      if (warm_connect) {
        channel->GetState(true);
      }
      read_channels_.push_back(channel);
      stubs_.push_back(BigQueryRead::NewStub(channel));
    }
    if (warm_connect) {
      for (size_t i = 0; i < n && i < read_channels_.size(); i++) {
        read_channels_[i]->GetState(true);
      }
    }
  }

  void EnsureWriteChannels(size_t n, bool warm_connect = true) {
    while (write_stubs_.size() < n) {
      grpc::ChannelArguments channel_arguments;
      channel_arguments.SetMaxReceiveMessageSize(134217728);
      channel_arguments.SetMaxSendMessageSize(134217728);
      channel_arguments.SetServiceConfigJSON(service_configuration_);
      channel_arguments.SetInt("grpc.use_local_subchannel_pool", 1);
      channel_arguments.SetInt("grpc.http2.bdp_probe", 1);
      channel_arguments.SetInt("grpc.http2.write_buffer_size", 4 * 1024 * 1024);
      channel_arguments.SetInt("bigrquerystorage.write_channel_id", static_cast<int>(write_stubs_.size()));
      auto channel = grpc::CreateCustomChannel(target_, cred_, channel_arguments);
      if (warm_connect) {
        channel->GetState(true);
      }
      write_channels_.push_back(channel);
      write_stubs_.push_back(google::cloud::bigquery::storage::v1::BigQueryWrite::NewStub(channel));
    }
    if (warm_connect) {
      for (size_t i = 0; i < n && i < write_channels_.size(); i++) {
        write_channels_[i]->GetState(true);
      }
    }
  }

  bool CreateWriteStreamWorker(
      int worker,
      int num_streams,
      const std::string& parent,
      google::cloud::bigquery::storage::v1::WriteStream_Type stream_type,
      ParallelRead* create_state,
      const std::atomic<bool>& all_claimed,
      std::string* stream_name_out,
      std::string* err_out) {
    google::cloud::bigquery::storage::v1::CreateWriteStreamRequest req;
    req.set_parent(parent);
    req.mutable_write_stream()->set_type(stream_type);

    size_t stub_idx = static_cast<size_t>(worker);
    auto& stub = write_stubs_[stub_idx % write_stubs_.size()];
    google::cloud::bigquery::storage::v1::WriteStream resp;
    grpc::Status status;
    const int deadline_ms = (num_streams > 1 && worker > 0) ? 3500 : 15000;
    static const int MAX_ATTEMPTS = 12; // ~30s total retry window for table recreation propagation
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
      if (create_state->cancel.load() || all_claimed.load()) return false;
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_ms));
      context.AddMetadata("x-goog-request-params", "parent=" + parent);
      context.AddMetadata("x-goog-api-client", client_info_);
      create_state->Register(worker, &context);
      status = stub->CreateWriteStream(&context, req, &resp);
      create_state->Unregister(worker);
      if (status.ok()) {
        *stream_name_out = resp.name();
        return true;
      }
      if (all_claimed.load() ||
          (create_state->cancel.load() && status.error_code() == grpc::StatusCode::CANCELLED)) {
        return false;
      }
      auto code = status.error_code();
      if ((code != grpc::StatusCode::NOT_FOUND &&
           code != grpc::StatusCode::UNAVAILABLE &&
           code != grpc::StatusCode::DEADLINE_EXCEEDED &&
           code != grpc::StatusCode::CANCELLED) ||
          attempt == MAX_ATTEMPTS - 1) {
        break;
      }
      int sleep_ms = (code == grpc::StatusCode::DEADLINE_EXCEEDED)
                       ? 25
                       : std::min(4000, 100 * (1 << std::min(attempt, 6)));
      for (int slept = 0; slept < sleep_ms; slept += 50) {
        if (create_state->cancel.load() || all_claimed.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, sleep_ms - slept)));
      }
    }
    if (all_claimed.load()) return false;
    if (err_out) {
      *err_out = "gRPC method CreateWriteStream error -> " + status.error_message();
    }
    return false;
  }

  bool FinalizeWriteStreamWorker(size_t stub_idx,
                                 const std::string& stream_name,
                                 int64_t* row_count_out,
                                 std::string* err_out) {
    google::cloud::bigquery::storage::v1::FinalizeWriteStreamRequest req;
    req.set_name(stream_name);

    auto& stub = write_stubs_[stub_idx % write_stubs_.size()];
    google::cloud::bigquery::storage::v1::FinalizeWriteStreamResponse resp;
    grpc::Status status;
    for (int attempt = 0; attempt < 5; attempt++) {
      grpc::ClientContext context;
      context.AddMetadata("x-goog-request-params", "name=" + stream_name);
      context.AddMetadata("x-goog-api-client", client_info_);
      status = stub->FinalizeWriteStream(&context, req, &resp);
      if (status.ok()) {
        *row_count_out = resp.row_count();
        return true;
      }
      auto code = status.error_code();
      if ((code != grpc::StatusCode::UNAVAILABLE && code != grpc::StatusCode::NOT_FOUND) ||
          attempt == 4) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(150 * (1 << attempt)));
    }
    if (err_out) {
      *err_out = "gRPC method FinalizeWriteStream error -> " + status.error_message();
    }
    return false;
  }

  void BatchCommitWriteStreams(const std::string& parent,
                               const std::vector<std::string>& stream_names) {
    google::cloud::bigquery::storage::v1::BatchCommitWriteStreamsRequest req;
    req.set_parent(parent);
    for (const auto& s : stream_names) {
      req.add_write_streams(s);
    }

    google::cloud::bigquery::storage::v1::BatchCommitWriteStreamsResponse resp;
    grpc::Status status;
    for (int attempt = 0; attempt < 6; attempt++) {
      grpc::ClientContext context;
      context.AddMetadata("x-goog-request-params", "parent=" + parent);
      context.AddMetadata("x-goog-api-client", client_info_);
      status = write_stubs_[0]->BatchCommitWriteStreams(&context, req, &resp);
      auto code = status.error_code();
      if (status.ok() ||
          (code != grpc::StatusCode::UNAVAILABLE && code != grpc::StatusCode::NOT_FOUND) ||
          attempt == 5) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(150 * (1 << attempt)));
    }
    if (!status.ok()) {
      std::string err = "gRPC method BatchCommitWriteStreams error -> " + status.error_message();
      Rcpp::stop(err.c_str());
    }
    if (!resp.has_commit_time() || resp.stream_errors_size() > 0) {
      std::string err = "gRPC method BatchCommitWriteStreams failed";
      if (resp.stream_errors_size() > 0) {
        err += ": " + resp.stream_errors(0).error_message();
      }
      Rcpp::stop(err.c_str());
    }
  }

  // Dynamic work-stealing bounded-window AppendRows worker with transient
  // NOT_FOUND / UNAVAILABLE retry on freshly created tables.
  bool AppendArrowBatchesWorkStealing(int worker,
                                      const std::string& stream_name,
                                      const std::string& schema_bytes,
                                      const std::vector<std::string>& batches,
                                      std::atomic<size_t>* next_batch,
                                      std::atomic<bool>* all_claimed,
                                      ParallelRead* create_state,
                                      ParallelRead* state,
                                      bool* used_out,
                                      std::string* err_out) {
    size_t m = batches.size();
    size_t first_idx = next_batch->fetch_add(1);
    if (first_idx >= m) {
      all_claimed->store(true);
      create_state->CancelAll();
      *used_out = false;
      return true;
    }
    if (first_idx + 1 >= m) {
      all_claimed->store(true);
      create_state->CancelAll();
    }
    *used_out = true;

    std::vector<size_t> pending_replay;
    pending_replay.push_back(first_idx);

    static const int MAX_APPEND_ATTEMPTS = 12; // ~30s total retry window for table recreation
    for (int attempt = 0; attempt < MAX_APPEND_ATTEMPTS; attempt++) {
      if (state->cancel.load()) return false;

      grpc::ClientContext context;
      context.AddMetadata("x-goog-request-params", "write_stream=" + stream_name);
      context.AddMetadata("x-goog-api-client", client_info_);

      state->Register(worker, &context);
      auto& stub = write_stubs_[static_cast<size_t>(worker) % write_stubs_.size()];
      auto stream = stub->AppendRows(&context);

      static const size_t MAX_INFLIGHT = 3;
      std::mutex win_mu;
      std::condition_variable win_cv;
      size_t in_flight = 0;
      size_t acked_here = 0;
      bool reader_done = false;
      std::atomic<bool> local_failed{false};
      int resp_err_code = 0;
      std::string resp_error;

      std::thread reader_thread([&]() {
        google::cloud::bigquery::storage::v1::AppendRowsResponse resp;
        while (stream->Read(&resp)) {
          if (resp.has_error()) {
            resp_err_code = resp.error().code();
            std::string err = "gRPC method AppendRows row error (" +
              std::to_string(resp_err_code) + ") -> " + resp.error().message();
            for (int j = 0; j < resp.row_errors_size() && j < 5; j++) {
              err += "\n  Row " + std::to_string(resp.row_errors(j).index()) + ": " +
                resp.row_errors(j).message();
            }
            resp_error = err;
            local_failed.store(true);
            {
              std::lock_guard<std::mutex> lk(win_mu);
              reader_done = true;
            }
            win_cv.notify_all();
            break;
          }
          {
            std::lock_guard<std::mutex> lk(win_mu);
            if (in_flight > 0) in_flight--;
            acked_here++;
          }
          win_cv.notify_one();
          state->pages.fetch_add(1);
        }
        {
          std::lock_guard<std::mutex> lk(win_mu);
          reader_done = true;
        }
        win_cv.notify_all();
      });

      bool is_first = true;
      size_t replay_pos = 0;
      std::vector<size_t> sent_this_attempt;

      while (true) {
        size_t idx = 0;
        {
          std::unique_lock<std::mutex> lk(win_mu);
          win_cv.wait(lk, [&]() {
            return in_flight < MAX_INFLIGHT || reader_done ||
                   local_failed.load() || state->cancel.load();
          });
          if (reader_done || local_failed.load() || state->cancel.load()) {
            break;
          }
          if (replay_pos < pending_replay.size()) {
            idx = pending_replay[replay_pos++];
          } else {
            idx = next_batch->fetch_add(1);
            if (idx >= m) {
              all_claimed->store(true);
              create_state->CancelAll();
              break;
            }
            if (idx + 1 >= m) {
              all_claimed->store(true);
              create_state->CancelAll();
            }
          }
          in_flight++;
        }

        sent_this_attempt.push_back(idx);
        google::cloud::bigquery::storage::v1::AppendRowsRequest req;
        if (is_first) {
          req.set_write_stream(stream_name);
          req.mutable_arrow_rows()->mutable_writer_schema()->set_serialized_schema(schema_bytes);
          is_first = false;
        }
        req.mutable_arrow_rows()->mutable_rows()->set_serialized_record_batch(batches[idx]);

        if (!stream->Write(req)) {
          break;
        }
      }

      stream->WritesDone();
      if (reader_thread.joinable()) reader_thread.join();
      grpc::Status status = stream->Finish();
      state->Unregister(worker);

      if (resp_error.empty() && status.ok()) {
        return !state->cancel.load();
      }
      if (state->cancel.load() && status.error_code() == grpc::StatusCode::CANCELLED) {
        return false;
      }

      bool retryable = (acked_here == 0) && (attempt < MAX_APPEND_ATTEMPTS - 1) && (
        resp_err_code == 5 /* NOT_FOUND */ ||
        resp_err_code == 9 /* FAILED_PRECONDITION */ ||
        resp_err_code == 10 /* ABORTED */ ||
        resp_err_code == 14 /* UNAVAILABLE */ ||
        status.error_code() == grpc::StatusCode::NOT_FOUND ||
        status.error_code() == grpc::StatusCode::FAILED_PRECONDITION ||
        status.error_code() == grpc::StatusCode::ABORTED ||
        status.error_code() == grpc::StatusCode::UNAVAILABLE
      );
      if (retryable) {
        // Replay any unacked batches claimed during this attempt
        for (size_t k = replay_pos; k < pending_replay.size(); k++) {
          sent_this_attempt.push_back(pending_replay[k]);
        }
        pending_replay = std::move(sent_this_attempt);
        int sleep_ms = std::min(4000, 100 * (1 << std::min(attempt, 6)));
        for (int slept = 0; slept < sleep_ms; slept += 50) {
          if (state->cancel.load()) return false;
          std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, sleep_ms - slept)));
        }
        continue;
      }

      if (!resp_error.empty()) {
        if (err_out) *err_out = resp_error;
      } else if (err_out) {
        *err_out = "gRPC method AppendRows error -> " + status.error_message();
      }
      create_state->CancelAll();
      state->CancelAll();
      return false;
    }
    return false;
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

  // Thread-safe SplitReadStream helper (returns true when stream split into 2 non-empty sub-streams)
  bool TrySplitReadStreamWorker(size_t stub_idx,
                                const std::string& stream,
                                double fraction,
                                std::string* primary_out,
                                std::string* remainder_out) {
    google::cloud::bigquery::storage::v1::SplitReadStreamRequest req;
    req.set_name(stream);
    req.set_fraction(fraction);

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(150));
    context.AddMetadata("x-goog-request-params", "name=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);

    google::cloud::bigquery::storage::v1::SplitReadStreamResponse resp;
    grpc::Status status = stubs_[stub_idx % stubs_.size()]->SplitReadStream(&context, req, &resp);
    if (!status.ok()) return false;
    if (resp.primary_stream().name().empty() || resp.remainder_stream().name().empty()) {
      return false;
    }
    *primary_out = resp.primary_stream().name();
    *remainder_out = resp.remainder_stream().name();
    return true;
  }

  // Single-stream ReadRows with in-flight ZSTD decompression and progress bar
  void ReadRowsDecoded(const std::string& stream,
                       std::vector<bqs_ipc::DecodedBatch>* out,
                       bqs_ipc::ZstdDecompressContext* zctx,
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
    std::unique_ptr<grpc::ClientReader<ReadRowsResponse>> reader(
        stubs_[0]->ReadRows(&context, method_request));

    while (reader->Read(&method_response)) {
      int64_t batch_rows = method_response.row_count();
      std::string* raw_batch = method_response.mutable_arrow_record_batch()->release_serialized_record_batch();
      if (raw_batch && !raw_batch->empty()) {
        bqs_ipc::DecodedBatch decoded;
        std::string dec_err;
        bool ok = bqs_ipc::decode_ipc_record_batch(std::move(*raw_batch), &decoded, zctx, &dec_err);
        delete raw_batch;
        if (!ok) {
          context.TryCancel();
          reader->Finish();
          Rcpp::stop(dec_err.c_str());
        }
        out->push_back(std::move(decoded));
      } else {
        delete raw_batch;
      }

      method_request.set_offset(method_request.offset() + batch_rows);
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
          pb->tick(batch_rows);
        } else {
          pb->tick(
              (method_response.stats().progress().at_response_end() -
               method_response.stats().progress().at_response_start()) * 200);
        }
      } else if (n > 0 && method_request.offset() + rows_count >= n) {
        context.TryCancel();
        break;
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
      std::string err = std::string("grpc method ReadRows error -> ") + status.error_message();
      Rcpp::stop(err.c_str());
    }
    rows_count += method_request.offset();
    if (last_stream && !quiet) {
      pb->update(1);
    }
  }

  // Worker-thread ReadRows with in-flight ZSTD decompression (zero R API calls)
  void ReadRowsDecodedWorker(int worker,
                             const std::string& stream,
                             std::vector<bqs_ipc::DecodedBatch>* out,
                             bqs_ipc::ZstdDecompressContext* zctx,
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
    std::unique_ptr<grpc::ClientReader<ReadRowsResponse>> reader(
        stubs_[static_cast<size_t>(worker) % stubs_.size()]->ReadRows(&context, method_request));

    while (reader->Read(&method_response)) {
      int64_t batch_rows = method_response.row_count();
      std::string* raw_batch = method_response.mutable_arrow_record_batch()->release_serialized_record_batch();
      if (raw_batch && !raw_batch->empty()) {
        bqs_ipc::DecodedBatch decoded;
        std::string dec_err;
        bool ok = bqs_ipc::decode_ipc_record_batch(std::move(*raw_batch), &decoded, zctx, &dec_err);
        delete raw_batch;
        if (!ok) {
          *error_msg = dec_err;
          state->CancelAll();
          break;
        }
        out->push_back(std::move(decoded));
      } else {
        delete raw_batch;
      }

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
    if (error_msg->empty() && !status.ok() &&
        !(state->cancel.load() && status.error_code() == grpc::StatusCode::CANCELLED)) {
      *error_msg = std::string("grpc method ReadRows error -> ") + status.error_message();
      state->CancelAll();
    }
  }
private:
  std::shared_ptr<grpc::ChannelCredentials> cred_;
  std::string target_;
  std::string service_configuration_;
  std::vector<std::shared_ptr<grpc::Channel>> read_channels_;
  std::vector<std::unique_ptr<BigQueryRead::Stub>> stubs_;
  std::vector<std::shared_ptr<grpc::Channel>> write_channels_;
  std::vector<std::unique_ptr<google::cloud::bigquery::storage::v1::BigQueryWrite::Stub>> write_stubs_;
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

// Parse an Arrow IPC stream buffer into the serialized Schema message,
// individual serialized RecordBatch messages, and total row count.
static int64_t split_arrow_ipc_stream(const uint8_t* data,
                                      size_t len,
                                      std::string* schema_out,
                                      std::vector<std::string>* batches_out) {
  int64_t total_rows = 0;
  size_t pos = 0;
  while (pos + 8 <= len) {
    uint32_t continuation = 0;
    int32_t meta_len = 0;
    std::memcpy(&continuation, data + pos, 4);
    if (continuation == 0xFFFFFFFFu) {
      std::memcpy(&meta_len, data + pos + 4, 4);
      if (meta_len <= 0) {
        break; // EOS marker (0xFFFFFFFF 0x00000000)
      }
    } else {
      meta_len = static_cast<int32_t>(continuation);
    }
    if (pos + 8 + static_cast<size_t>(meta_len) > len) {
      Rcpp::stop("Invalid Arrow IPC stream: truncated metadata.");
    }
    const uint8_t* fb = data + pos + 8;
    int32_t root_off = 0;
    std::memcpy(&root_off, fb, 4);
    int32_t vtable_rel = 0;
    std::memcpy(&vtable_rel, fb + root_off, 4);
    int32_t vtable_off = root_off - vtable_rel;
    uint16_t vtable_len = 0;
    std::memcpy(&vtable_len, fb + vtable_off, 2);

    uint8_t header_type = 0;
    if (vtable_len >= 8) {
      uint16_t ht_off = 0;
      std::memcpy(&ht_off, fb + vtable_off + 6, 2);
      if (ht_off > 0) {
        header_type = fb[root_off + ht_off];
      }
    }
    int64_t body_len = 0;
    if (vtable_len >= 12) {
      uint16_t bl_off = 0;
      std::memcpy(&bl_off, fb + vtable_off + 10, 2);
      if (bl_off > 0) {
        std::memcpy(&body_len, fb + root_off + bl_off, 8);
      }
    }
    size_t msg_len = 8 + static_cast<size_t>(meta_len) + static_cast<size_t>(body_len);
    if (pos + msg_len > len) {
      Rcpp::stop("Invalid Arrow IPC stream: truncated message body.");
    }
    if (header_type == 1) {
      schema_out->assign(reinterpret_cast<const char*>(data + pos), msg_len);
    } else if (header_type == 3) {
      batches_out->emplace_back(reinterpret_cast<const char*>(data + pos), msg_len);
      if (vtable_len >= 10) {
        uint16_t hdr_off = 0;
        std::memcpy(&hdr_off, fb + vtable_off + 8, 2);
        if (hdr_off > 0) {
          int32_t hdr_uoff = 0;
          std::memcpy(&hdr_uoff, fb + root_off + hdr_off, 4);
          int32_t rb_off = root_off + hdr_off + hdr_uoff;
          int32_t rb_vt_rel = 0;
          std::memcpy(&rb_vt_rel, fb + rb_off, 4);
          int32_t rb_vt_off = rb_off - rb_vt_rel;
          uint16_t rb_vt_len = 0;
          std::memcpy(&rb_vt_len, fb + rb_vt_off, 2);
          if (rb_vt_len >= 6) {
            uint16_t len_off = 0;
            std::memcpy(&len_off, fb + rb_vt_off + 4, 2);
            if (len_off > 0) {
              int64_t rb_rows = 0;
              std::memcpy(&rb_rows, fb + rb_off + len_off, 8);
              total_rows += rb_rows;
            }
          }
        }
      }
    }
    pos += msg_len;
  }
  return total_rows;
}

// Execute work-stealing multi-stream parallel AppendRows (+ Finalize & BatchCommit).
// Fast streams immediately pull batches without waiting for straggler CreateWriteStreams,
// and straggler CreateWriteStreams are cancelled as soon as all batches are claimed.
static int64_t execute_parallel_write(BigQueryReadClient* client_ptr,
                                      const std::string& parent,
                                      const std::string& schema_bytes,
                                      std::vector<std::string>* batches,
                                      int64_t total_rows,
                                      const std::string& stream_type,
                                      int num_streams,
                                      bool quiet) {
  size_t m = batches->size();
  if (m == 0) return 0;

  num_streams = std::max(1, std::min<int>(num_streams, static_cast<int>(m)));
  client_ptr->EnsureWriteChannels(static_cast<size_t>(num_streams), true);

  bool is_default = (stream_type == "DEFAULT");
  std::vector<std::string> stream_names(static_cast<size_t>(num_streams));
  if (is_default) {
    for (int w = 0; w < num_streams; w++) {
      stream_names[static_cast<size_t>(w)] = parent + "/streams/_default";
    }
  }

  ParallelRead create_state(num_streams);
  ParallelRead state(num_streams);
  std::atomic<size_t> next_batch{0};
  std::atomic<bool> all_claimed{false};
  std::vector<bool> stream_used(static_cast<size_t>(num_streams), false);
  std::vector<std::string> errors(static_cast<size_t>(num_streams));
  std::vector<int64_t> finalized_rows(static_cast<size_t>(num_streams), 0);
  std::atomic<int> running(num_streams);
  std::mutex done_mu;
  std::condition_variable done_cv;

  RProgress::RProgress pb(
      "\033[42m\033[30mUploading (:percent)\033[39m\033[49m [:bar] eta[:eta|:elapsed]");
  pb.set_cursor_char(">");
  pb.set_total(100);

  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(num_streams));
  for (int w = 0; w < num_streams; w++) {
    workers.emplace_back([&, w]() {
      size_t wu = static_cast<size_t>(w);
      bool ready = true;
      if (!is_default) {
        ready = client_ptr->CreateWriteStreamWorker(
          w,
          num_streams,
          parent,
          google::cloud::bigquery::storage::v1::WriteStream_Type_PENDING,
          &create_state,
          all_claimed,
          &stream_names[wu],
          &errors[wu]);
        if (!ready && !all_claimed.load() && !state.cancel.load()) {
          create_state.CancelAll();
          state.CancelAll();
        }
      }
      if (ready && !state.cancel.load()) {
        bool used = false;
        if (!client_ptr->AppendArrowBatchesWorkStealing(
              w, stream_names[wu], schema_bytes, *batches,
              &next_batch, &all_claimed, &create_state, &state,
              &used, &errors[wu])) {
          create_state.CancelAll();
          state.CancelAll();
        }
        stream_used[wu] = used;
      }
      if (!is_default && stream_used[wu] && !state.cancel.load()) {
        if (!client_ptr->FinalizeWriteStreamWorker(
              wu, stream_names[wu], &finalized_rows[wu], &errors[wu])) {
          create_state.CancelAll();
          state.CancelAll();
        }
      }
      std::lock_guard<std::mutex> lk(done_mu);
      running.fetch_sub(1);
      done_cv.notify_all();
    });
  }

  auto join_all = [&]() {
    for (auto& th : workers) {
      if (th.joinable()) th.join();
    }
  };

  while (true) {
    {
      std::unique_lock<std::mutex> lk(done_mu);
      done_cv.wait_for(lk, std::chrono::milliseconds(50), [&]() { return running.load() == 0; });
    }
    if (running.load() == 0) break;
    try {
      Rcpp::checkUserInterrupt();
    } catch (...) {
      create_state.CancelAll();
      state.CancelAll();
      join_all();
      throw;
    }
    if (!quiet) {
      double ratio = static_cast<double>(state.pages.load()) / static_cast<double>(m);
      pb.update(std::min(0.99, std::max(0.0, ratio)));
    }
  }
  join_all();

  for (int w = 0; w < num_streams; w++) {
    if (!errors[static_cast<size_t>(w)].empty()) {
      Rcpp::stop(errors[static_cast<size_t>(w)].c_str());
    }
  }

  int64_t committed_rows = total_rows;
  int used_count = 0;
  if (!is_default) {
    committed_rows = 0;
    std::vector<std::string> commit_streams;
    commit_streams.reserve(static_cast<size_t>(num_streams));
    for (int w = 0; w < num_streams; w++) {
      size_t wu = static_cast<size_t>(w);
      if (stream_used[wu]) {
        commit_streams.push_back(stream_names[wu]);
        committed_rows += finalized_rows[wu];
      }
    }
    used_count = static_cast<int>(commit_streams.size());
    if (!commit_streams.empty()) {
      client_ptr->BatchCommitWriteStreams(parent, commit_streams);
    }
  } else {
    for (int w = 0; w < num_streams; w++) {
      if (stream_used[static_cast<size_t>(w)]) used_count++;
    }
  }

  if (!quiet) {
    pb.update(1);
    REprintf("Uploaded %lld rows across %d stream(s) in %zu message(s).\n",
             static_cast<long long>(committed_rows), std::max(1, used_count), m);
  }
  return committed_rows;
}

// Pre-warm write channels asynchronously so TCP/TLS/HTTP2 handshakes overlap
// with REST table creation and Arrow conversion.
// [[Rcpp::export(rng=false)]]
void bqs_warm_write_channels(SEXP client, int n_channels = 0) {
  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);
  int target = n_channels > 0 ? n_channels : 10;
  client_ptr->EnsureWriteChannels(static_cast<size_t>(target), true);
}

// [[Rcpp::export(rng=false)]]
double bqs_ipc_write(SEXP client,
                     std::string project,
                     std::string dataset,
                     std::string table,
                     Rcpp::RawVector ipc_bytes,
                     std::string stream_type = "PENDING",
                     bool quiet = false,
                     int max_stream_count = 0,
                     int max_threads = 0) {
  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);
  std::string parent = "projects/" + project + "/datasets/" + dataset + "/tables/" + table;

  std::string schema_bytes;
  std::vector<std::string> batches;
  int64_t total_rows = split_arrow_ipc_stream(RAW(ipc_bytes), ipc_bytes.size(), &schema_bytes, &batches);
  if (batches.empty()) {
    return 0;
  }

  int target_threads = max_threads > 0 ? max_threads : 10;
  if (max_stream_count > 0) {
    target_threads = max_stream_count;
  }
  const size_t min_bytes_per_stream = 3u * 1024u * 1024u;
  int by_size = std::max(1, static_cast<int>((static_cast<size_t>(ipc_bytes.size()) + min_bytes_per_stream - 1) / min_bytes_per_stream));
  int num_streams = std::min<int>({target_threads, by_size, static_cast<int>(batches.size())});

  int64_t committed = execute_parallel_write(
    client_ptr.get(), parent, schema_bytes, &batches, total_rows,
    stream_type, num_streams, quiet);
  return static_cast<double>(committed);
}

// High-throughput direct ArrowArrayStream -> Parallel ZSTD IPC Encode ->
// Work-Stealing Multi-Stream Parallel AppendRows + Finalize + BatchCommit.
// [[Rcpp::export(rng=false)]]
double bqs_arrow_write(SEXP client,
                       std::string project,
                       std::string dataset,
                       std::string table,
                       SEXP stream_xptr,
                       Rcpp::RawVector schema_ipc,
                       int row_batch_size = 100000,
                       std::string stream_type = "PENDING",
                       bool use_zstd = true,
                       bool quiet = false,
                       int max_stream_count = 0,
                       int max_threads = 0) {
  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);
  std::string parent = "projects/" + project + "/datasets/" + dataset + "/tables/" + table;

  struct ArrowArrayStream* stream = nanoarrow_array_stream_from_xptr(stream_xptr);
  struct ArrowSchema schema;
  schema.release = nullptr;
  if (stream->get_schema(stream, &schema) != 0) {
    const char* err = stream->get_last_error ? stream->get_last_error(stream) : "Failed to read ArrowSchema";
    Rcpp::stop(err ? err : "Failed to read ArrowSchema");
  }

  std::vector<ArrowArray> src_arrays;
  int64_t total_rows = 0;
  int64_t total_uncompressed_bytes = 0;
  while (true) {
    ArrowArray arr;
    arr.release = nullptr;
    int rc = stream->get_next(stream, &arr);
    if (rc != 0) {
      const char* err = stream->get_last_error ? stream->get_last_error(stream) : "Failed to read ArrowArray";
      std::string err_str = err ? err : "Failed to read ArrowArray";
      for (auto& a : src_arrays) if (a.release) a.release(&a);
      if (schema.release) schema.release(&schema);
      Rcpp::stop(err_str.c_str());
    }
    if (arr.release == nullptr) break;
    if (arr.length > 0) {
      total_rows += arr.length;
      total_uncompressed_bytes += bqs_ipc::estimate_batch_bytes(&schema, &arr);
      src_arrays.push_back(arr);
    } else {
      arr.release(&arr);
    }
  }

  auto cleanup_arrow = [&]() {
    for (auto& a : src_arrays) {
      if (a.release) a.release(&a);
    }
    src_arrays.clear();
    if (schema.release) schema.release(&schema);
  };

  if (total_rows <= 0 || src_arrays.empty()) {
    cleanup_arrow();
    return 0;
  }

  // Strip 8-byte EOS marker from schema_ipc if present
  size_t sch_len = static_cast<size_t>(schema_ipc.size());
  if (sch_len >= 8) {
    uint32_t cont = 0;
    int32_t mlen = 0;
    std::memcpy(&cont, RAW(schema_ipc) + sch_len - 8, 4);
    std::memcpy(&mlen, RAW(schema_ipc) + sch_len - 4, 4);
    if (cont == 0xFFFFFFFFu && mlen == 0) sch_len -= 8;
  }
  std::string schema_bytes(reinterpret_cast<const char*>(RAW(schema_ipc)), sch_len);

  int enc_threads = max_threads > 0 ? max_threads : BQS_DEFAULT_THREADS;
  int default_cap = (stream_type == "DEFAULT") ? 12 : 10;
  int max_streams = max_stream_count > 0
                      ? max_stream_count
                      : (max_threads > 0 ? max_threads : default_cap);

  // Scale stream concurrency smoothly (~3.5 MB uncompressed per stream):
  // - < 3.5 MB -> 1 stream
  // - 22.4 MB  -> 7 streams
  // - 136.6 MB -> 12 streams (DEFAULT) / 10 streams (PENDING)
  const int64_t min_bytes_per_stream = 3584LL * 1024LL;
  int streams_by_bytes = std::max(1, static_cast<int>((total_uncompressed_bytes + min_bytes_per_stream - 1) / min_bytes_per_stream));
  int streams_by_rows = std::max(1, static_cast<int>(total_rows / 4096));
  int num_streams = max_stream_count > 0
                      ? std::max(1, std::min(max_stream_count, streams_by_rows))
                      : std::max(1, std::min({max_streams, streams_by_bytes, streams_by_rows}));

  // Target >= 2 * num_streams batches (each <= 5 MB uncompressed) so fast streams
  // dynamically steal work if any single stream experiences higher latency.
  const int64_t max_batch_bytes = 5LL * 1024LL * 1024LL;
  double bytes_per_row = std::max(16.0, static_cast<double>(total_uncompressed_bytes) / static_cast<double>(total_rows));
  int64_t max_rows_by_bytes = std::max<int64_t>(512, static_cast<int64_t>(static_cast<double>(max_batch_bytes) / bytes_per_row));
  int target_min_batches = (num_streams > 1) ? (num_streams * 2) : 1;
  int64_t max_rows_for_stealing = std::max<int64_t>(512, (total_rows + target_min_batches - 1) / target_min_batches);
  int64_t effective_batch_rows = std::min<int64_t>({
    static_cast<int64_t>(std::max(1, row_batch_size)),
    max_rows_by_bytes,
    max_rows_for_stealing
  });
  if (effective_batch_rows >= 64) {
    effective_batch_rows = (effective_batch_rows / 64) * 64;
  }

  std::vector<bqs_ipc::BatchSliceSpec> specs;
  for (size_t a_idx = 0; a_idx < src_arrays.size(); a_idx++) {
    int64_t alen = src_arrays[a_idx].length;
    for (int64_t s = 0; s < alen; s += effective_batch_rows) {
      specs.push_back({a_idx, s, std::min<int64_t>(effective_batch_rows, alen - s)});
    }
  }
  num_streams = std::min<int>(num_streams, static_cast<int>(specs.size()));
  client_ptr->EnsureWriteChannels(static_cast<size_t>(num_streams), true);

  // Encode & ZSTD-compress all RecordBatches in parallel across CPU cores (~15-25 ms),
  // then immediately release input Arrow memory and launch work-stealing write streams.
  std::vector<std::string> batches;
  std::string encode_err;
  bool enc_ok = bqs_ipc::encode_batches_parallel(
    &schema, src_arrays, specs, use_zstd, enc_threads, &batches, &encode_err);

  cleanup_arrow();

  if (!enc_ok) {
    Rcpp::stop(encode_err.c_str());
  }

  int64_t committed = execute_parallel_write(
    client_ptr.get(), parent, schema_bytes, &batches, total_rows,
    stream_type, num_streams, quiet);
  return static_cast<double>(committed);
}

// High-throughput fused download pipeline:
// CreateReadSession -> (optional parallel SplitReadStream) ->
// Parallel ReadRows with in-flight ZSTD decompression ->
// Parallel Direct C++ Arrow-to-R Tibble/DataFrame conversion.
// [[Rcpp::export(rng=false)]]
SEXP bqs_download_direct(SEXP client,
                         std::string project,
                         std::string dataset,
                         std::string table,
                         std::string parent,
                         std::int64_t n,
                         std::vector<std::string> selected_fields,
                         std::string row_restriction,
                         std::double_t sample_percentage,
                         std::int64_t timestamp_seconds,
                         std::int32_t timestamp_nanos,
                         bool quiet,
                         int32_t max_stream_count,
                         int32_t compression_codec,
                         int32_t max_threads,
                         std::string bigint,
                         bool as_data_frame,
                         bool split_streams,
                         Rcpp::Function read_nanoarrow_fn,
                         Rcpp::Function int64_ptype_fn,
                         Rcpp::Function check_pkg_fn,
                         Rcpp::Function na_convert_stream,
                         Rcpp::Function na_convert_stream_quiet,
                         Rcpp::Function as_bigint) {
  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);

  int target_threads = max_threads > 0 ? max_threads : 24;

  int32_t requested_streams = max_stream_count;
  if (n > 0 && n <= 100000 && requested_streams == 0) {
    requested_streams = 1;
  }
  int32_t preferred_min_streams = target_threads;
  if (requested_streams > 0) {
    preferred_min_streams = std::min(requested_streams, preferred_min_streams);
  } else {
    requested_streams = std::max<int32_t>(target_threads * 2, 32);
  }

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

  int orig_n = read_session.streams_size();
  const std::string& schema_bytes = read_session.arrow_schema().serialized_schema();

  bool do_async_split = split_streams && n < 0 && orig_n >= 2 &&
                        (orig_n * 2 <= target_threads) &&
                        read_session.estimated_total_bytes_scanned() >= 96LL * 1024LL * 1024LL;

  int total_slots = do_async_split ? (orig_n * 2) : std::max(1, orig_n);
  std::vector<std::vector<bqs_ipc::DecodedBatch>> stream_batches(static_cast<size_t>(total_slots));

  long int rows_count = 0;
  long int pages_count = 0;
  RProgress::RProgress pb(
      "\033[42m\033[30mStreaming (:percent)\033[39m\033[49m [:bar] eta[:eta|:elapsed] throt[:extra]");
  pb.set_cursor_char(">");

  if (orig_n <= 1) {
    pb.set_total(n > 0 ? static_cast<double>(n) : 100.0);
    bqs_ipc::ZstdDecompressContext zctx;
    for (int i = 0; i < orig_n; i++) {
      client_ptr->ReadRowsDecoded(read_session.streams(i).name(),
                                  &stream_batches[i],
                                  &zctx,
                                  n, rows_count, pages_count, quiet,
                                  &pb, i == orig_n - 1);
    }
  } else if (do_async_split) {
    // Non-blocking per-stream SplitReadStream + immediate ReadRows:
    // Each primary worker splits stream `i` (150ms budget), dispatches `remainder`
    // to an already-waiting helper worker on its own pre-warmed channel, and
    // immediately reads `primary` into slot `2*i` (with `remainder` in `2*i + 1`).
    int num_threads = orig_n * 2;
    client_ptr->EnsureChannels(static_cast<size_t>(num_threads), true);

    ParallelRead state(num_threads);
    state.n = n;
    std::vector<std::string> errors(static_cast<size_t>(total_slots));
    std::atomic<int> running(num_threads);
    std::mutex done_mu;
    std::condition_variable done_cv;

    std::mutex rem_mu;
    std::condition_variable rem_cv;
    std::vector<std::pair<int, std::string>> rem_queue;
    rem_queue.reserve(static_cast<size_t>(orig_n));
    int pending_splits = orig_n;

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(num_threads));

    // Primary workers 0 .. orig_n - 1
    for (int i = 0; i < orig_n; i++) {
      workers.emplace_back([&, i]() {
        bqs_ipc::ZstdDecompressContext zctx;
        std::string orig_stream = read_session.streams(i).name();
        std::string p1, p2;
        bool split_ok = false;
        if (!state.cancel.load()) {
          split_ok = client_ptr->TrySplitReadStreamWorker(
            static_cast<size_t>(i), orig_stream, 0.5, &p1, &p2);
        }
        {
          std::lock_guard<std::mutex> lk(rem_mu);
          if (split_ok && !state.cancel.load()) {
            rem_queue.emplace_back(2 * i + 1, std::move(p2));
          }
          pending_splits--;
        }
        rem_cv.notify_all();

        if (!state.cancel.load()) {
          const std::string& s_read = split_ok ? p1 : orig_stream;
          int slot = 2 * i;
          client_ptr->ReadRowsDecodedWorker(
            i, s_read, &stream_batches[slot], &zctx, &state, &errors[slot]);
          state.streams_done.fetch_add(1);
        }
        std::lock_guard<std::mutex> lock(done_mu);
        running.fetch_sub(1);
        done_cv.notify_all();
      });
    }

    // Helper workers orig_n .. 2*orig_n - 1 for remainder streams
    for (int h = orig_n; h < num_threads; h++) {
      workers.emplace_back([&, h]() {
        bqs_ipc::ZstdDecompressContext zctx;
        while (!state.cancel.load()) {
          std::pair<int, std::string> item;
          bool has_item = false;
          {
            std::unique_lock<std::mutex> lk(rem_mu);
            rem_cv.wait(lk, [&]() {
              return !rem_queue.empty() || pending_splits == 0 || state.cancel.load();
            });
            if (!rem_queue.empty() && !state.cancel.load()) {
              item = std::move(rem_queue.back());
              rem_queue.pop_back();
              has_item = true;
            } else if (pending_splits == 0 || state.cancel.load()) {
              break;
            }
          }
          if (has_item) {
            client_ptr->ReadRowsDecodedWorker(
              h, item.second, &stream_batches[item.first], &zctx, &state, &errors[item.first]);
            state.streams_done.fetch_add(1);
          }
        }
        std::lock_guard<std::mutex> lock(done_mu);
        running.fetch_sub(1);
        done_cv.notify_all();
      });
    }

    double expected_rows = static_cast<double>(read_session.estimated_row_count());
    pb.set_total(100);

    auto join_all = [&]() {
      rem_cv.notify_all();
      for (auto& w : workers) {
        if (w.joinable()) w.join();
      }
    };

    while (true) {
      {
        std::unique_lock<std::mutex> lock(done_mu);
        done_cv.wait_for(lock, std::chrono::milliseconds(50),
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
          : static_cast<double>(state.streams_done.load()) / total_slots;
        pb.set_extra(state.throttle.load());
        pb.update(std::min(0.99, std::max(0.0, ratio)));
      }
    }
    join_all();

    for (int i = 0; i < total_slots; i++) {
      if (!errors[i].empty()) {
        Rcpp::stop(errors[i].c_str());
      }
    }
    rows_count = static_cast<long int>(state.rows.load());
    pages_count = static_cast<long int>(state.pages.load());
    if (!quiet) {
      pb.update(1);
    }
  } else {
    int n_streams = orig_n;
    int num_threads = std::min(n_streams, target_threads);
    client_ptr->EnsureChannels(static_cast<size_t>(num_threads), true);

    ParallelRead state(num_threads);
    state.n = n;
    std::vector<std::string> errors(static_cast<size_t>(n_streams));
    std::atomic<int> next_stream(0);
    std::atomic<int> running(num_threads);
    std::mutex done_mu;
    std::condition_variable done_cv;

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(num_threads));
    for (int t = 0; t < num_threads; t++) {
      workers.emplace_back([&, t]() {
        bqs_ipc::ZstdDecompressContext zctx;
        while (!state.cancel.load()) {
          int idx = next_stream.fetch_add(1);
          if (idx >= n_streams) break;
          client_ptr->ReadRowsDecodedWorker(
            t, read_session.streams(idx).name(), &stream_batches[idx], &zctx, &state, &errors[idx]);
          state.streams_done.fetch_add(1);
        }
        std::lock_guard<std::mutex> lock(done_mu);
        running.fetch_sub(1);
        done_cv.notify_all();
      });
    }

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
        done_cv.wait_for(lock, std::chrono::milliseconds(50),
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

  // Decode the ~600-byte ArrowSchema once via nanoarrow::read_nanoarrow
  Rcpp::RawVector schema_raw(schema_bytes.size());
  if (!schema_bytes.empty()) {
    std::memcpy(RAW(schema_raw), schema_bytes.data(), schema_bytes.size());
  }
  SEXP schema_reader_xptr = PROTECT(read_nanoarrow_fn(schema_raw));
  struct ArrowArrayStream* sch_stream = nanoarrow_array_stream_from_xptr(schema_reader_xptr);

  SEXP schema_xptr = PROTECT(nanoarrow_schema_owning_xptr());
  struct ArrowSchema* out_schema = nanoarrow_output_schema_from_xptr(schema_xptr);
  if (sch_stream->get_schema(sch_stream, out_schema) != 0) {
    const char* err = sch_stream->get_last_error ? sch_stream->get_last_error(sch_stream) : "Failed to parse ArrowSchema";
    UNPROTECT(2);
    Rcpp::stop(err ? err : "Failed to parse ArrowSchema");
  }

  SEXP res = PROTECT(bqs_convert_decoded_streams(
    schema_xptr,
    &stream_batches,
    n,
    bigint,
    as_data_frame,
    target_threads,
    int64_ptype_fn,
    check_pkg_fn,
    na_convert_stream,
    na_convert_stream_quiet,
    as_bigint));
  UNPROTECT(3);
  return res;
}

