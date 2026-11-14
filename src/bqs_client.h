#ifndef BQS_CLIENT_H
#define BQS_CLIENT_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <grpc/grpc.h>
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <grpcpp/grpcpp.h>
#include "google/cloud/bigquery/storage/v1/stream.pb.h"
#include "google/cloud/bigquery/storage/v1/storage.pb.h"
#ifdef __clang__
# pragma GCC diagnostic ignored "-Winconsistent-missing-override"
#endif
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "google/cloud/bigquery/storage/v1/storage.grpc.pb.h"

#include <Rcpp.h>
#include "RProgress.h"
#include "bqs_arrow_ipc.h"

static constexpr int BQS_DEFAULT_THREADS = 16;

struct ParallelState {
  std::atomic<bool> cancel{false};
  std::atomic<int64_t> rows{0};
  std::atomic<int64_t> pages{0};
  std::atomic<int> throttle{0};
  std::atomic<int> streams_done{0};
  std::atomic<int> progress_permille{0};
  int64_t n = -1;
  std::mutex mu;
  std::vector<grpc::ClientContext*> contexts;

  explicit ParallelState(int n_workers)
      : contexts(static_cast<size_t>(std::max(0, n_workers)), nullptr) {}

  void Register(int worker, grpc::ClientContext* ctx) {
    std::lock_guard<std::mutex> lock(mu);
    contexts[static_cast<size_t>(worker)] = ctx;
    if (cancel.load()) ctx->TryCancel();
  }

  void Unregister(int worker) {
    std::lock_guard<std::mutex> lock(mu);
    contexts[static_cast<size_t>(worker)] = nullptr;
  }

  void CancelAll() {
    cancel.store(true);
    std::lock_guard<std::mutex> lock(mu);
    for (auto* ctx : contexts) {
      if (ctx) ctx->TryCancel();
    }
  }
};

struct WorkerGroup {
  std::atomic<int> running;
  std::mutex done_mu;
  std::condition_variable done_cv;
  std::vector<std::thread> workers;

  explicit WorkerGroup(int n_workers) : running(n_workers) {
    workers.reserve(static_cast<size_t>(std::max(0, n_workers)));
  }

  void mark_done() {
    std::lock_guard<std::mutex> lock(done_mu);
    running.fetch_sub(1);
    done_cv.notify_all();
  }

  void join_all() {
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }
  }
};

template <typename ProgressFn, typename CancelFn>
inline void wait_for_workers_with_progress(WorkerGroup& group,
                                           RProgress::RProgress* pb,
                                           bool quiet,
                                           int poll_ms,
                                           ProgressFn&& progress_fn,
                                           CancelFn&& cancel_fn) {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(group.done_mu);
      group.done_cv.wait_for(
        lock,
        std::chrono::milliseconds(poll_ms),
        [&]() { return group.running.load() == 0; });
    }
    if (group.running.load() == 0) break;
    try {
      Rcpp::checkUserInterrupt();
    } catch (...) {
      cancel_fn();
      group.join_all();
      throw;
    }
    if (!quiet && pb) {
      progress_fn(*pb);
    }
  }
  group.join_all();
}

inline bool bqs_cancellable_sleep(int sleep_ms,
                                  const ParallelState* state,
                                  const std::atomic<bool>* extra_stop = nullptr) {
  for (int slept = 0; slept < sleep_ms; slept += 50) {
    if ((state && state->cancel.load()) || (extra_stop && extra_stop->load())) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, sleep_ms - slept)));
  }
  return !((state && state->cancel.load()) || (extra_stop && extra_stop->load()));
}

class BigQueryReadClient {
public:
  using BigQueryRead = google::cloud::bigquery::storage::v1::BigQueryRead;
  using BigQueryWrite = google::cloud::bigquery::storage::v1::BigQueryWrite;
  using ReadSession = google::cloud::bigquery::storage::v1::ReadSession;
  using ReadRowsRequest = google::cloud::bigquery::storage::v1::ReadRowsRequest;
  using ReadRowsResponse = google::cloud::bigquery::storage::v1::ReadRowsResponse;

  BigQueryReadClient(std::shared_ptr<grpc::ChannelCredentials> cred,
                     const std::string& target,
                     const std::string& service_configuration);

  void SetClientInfo(const std::string& client_info) { client_info_ = client_info; }
  void EnsureChannels(size_t n, bool warm_connect = true);
  void EnsureWriteChannels(size_t n, bool warm_connect = true);
  void InitContext(grpc::ClientContext* ctx,
                   const std::string& request_params,
                   int deadline_ms = 0) const;

  ReadSession CreateReadSession(const std::string& project,
                                const std::string& dataset,
                                const std::string& table,
                                const std::string& parent,
                                int64_t timestamp_seconds,
                                int32_t timestamp_nanos,
                                const std::vector<std::string>& selected_fields,
                                const std::string& row_restriction,
                                double sample_percentage,
                                int32_t max_stream_count = 0,
                                int32_t compression_codec = 0,
                                int32_t preferred_min_stream_count = 0);

  bool TrySplitReadStreamWorker(size_t stub_idx,
                                const std::string& stream,
                                double fraction,
                                std::string* primary_out,
                                std::string* remainder_out);

  template <typename BatchSink>
  void ReadRowsWorker(int worker,
                      const std::string& stream,
                      BatchSink& sink,
                      ParallelState* state,
                      std::string* error_msg) {
    grpc::ClientContext context;
    InitContext(&context, "read_stream=" + stream);

    ReadRowsRequest req;
    req.set_read_stream(stream);
    req.set_offset(0);

    ReadRowsResponse resp;
    state->Register(worker, &context);
    std::unique_ptr<grpc::ClientReader<ReadRowsResponse>> reader(
      stubs_[static_cast<size_t>(worker) % stubs_.size()]->ReadRows(&context, req));

    while (reader->Read(&resp)) {
      int64_t batch_rows = resp.row_count();
      if (!sink.accept(&resp, error_msg)) {
        state->CancelAll();
        context.TryCancel();
        break;
      }
      if (resp.has_throttle_state()) {
        state->throttle.store(resp.throttle_state().throttle_percent());
      }
      if (resp.has_stats() && resp.stats().has_progress()) {
        state->progress_permille.store(
          static_cast<int>(resp.stats().progress().at_response_end() * 1000.0));
      }
      state->pages.fetch_add(1);
      int64_t total = state->rows.fetch_add(batch_rows) + batch_rows;
      if (state->n > 0 && total >= state->n) {
        state->CancelAll();
        context.TryCancel();
        break;
      }
      if (state->cancel.load()) {
        context.TryCancel();
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

  bool CreateWriteStreamWorker(
      int worker,
      int num_streams,
      const std::string& parent,
      google::cloud::bigquery::storage::v1::WriteStream_Type stream_type,
      ParallelState* create_state,
      const std::atomic<bool>& all_claimed,
      std::string* stream_name_out,
      std::string* err_out);

  bool FinalizeWriteStreamWorker(int worker,
                                 const std::string& stream_name,
                                 ParallelState* state,
                                 int64_t* row_count_out,
                                 std::string* err_out);

  void BatchCommitWriteStreams(const std::string& parent,
                               const std::vector<std::string>& stream_names);

  bool AppendArrowBatchesWorkStealing(int worker,
                                      const std::string& stream_name,
                                      bool is_default,
                                      const std::string& schema_bytes,
                                      const std::vector<std::string>& batches,
                                      const std::vector<int64_t>& batch_rows,
                                      std::atomic<size_t>* next_batch,
                                      std::atomic<bool>* all_claimed,
                                      ParallelState* create_state,
                                      ParallelState* state,
                                      bool* used_out,
                                      std::string* err_out);

private:
  template <typename ServiceT>
  void EnsureChannelPool(size_t n,
                         bool warm_connect,
                         bool is_write,
                         std::vector<std::shared_ptr<grpc::Channel>>* channels,
                         std::vector<std::unique_ptr<typename ServiceT::Stub>>* stubs);

  std::shared_ptr<grpc::ChannelCredentials> cred_;
  std::string target_;
  std::string service_configuration_;
  std::vector<std::shared_ptr<grpc::Channel>> read_channels_;
  std::vector<std::unique_ptr<BigQueryRead::Stub>> stubs_;
  std::vector<std::shared_ptr<grpc::Channel>> write_channels_;
  std::vector<std::unique_ptr<BigQueryWrite::Stub>> write_stubs_;
  std::string client_info_;
};

#endif // BQS_CLIENT_H
