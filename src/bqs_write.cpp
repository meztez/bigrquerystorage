#include "bqs_client.h"

#include <deque>
#include <nanoarrow/r.h>

namespace {

int64_t execute_parallel_write(BigQueryReadClient* client_ptr,
                               const std::string& parent,
                               const std::string& schema_bytes,
                               std::vector<std::string>* batches,
                               const std::vector<int64_t>& batch_rows,
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

  ParallelState create_state(num_streams);
  ParallelState state(num_streams);
  std::atomic<size_t> next_batch{0};
  std::atomic<bool> all_claimed{false};
  std::vector<uint8_t> stream_used(static_cast<size_t>(num_streams), 0);
  std::vector<std::string> errors(static_cast<size_t>(num_streams));
  std::vector<int64_t> finalized_rows(static_cast<size_t>(num_streams), 0);
  WorkerGroup group(num_streams);

  RProgress::RProgress pb(
    "\033[42m\033[30mUploading (:percent)\033[39m\033[49m [:bar] eta[:eta|:elapsed]");
  pb.set_cursor_char(">");
  pb.set_total(100);

  for (int w = 0; w < num_streams; w++) {
    group.workers.emplace_back([&, w]() {
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
              w,
              stream_names[wu],
              is_default,
              schema_bytes,
              *batches,
              batch_rows,
              &next_batch,
              &all_claimed,
              &create_state,
              &state,
              &used,
              &errors[wu])) {
          create_state.CancelAll();
          state.CancelAll();
        }
        stream_used[wu] = used ? 1u : 0u;
      }
      if (!is_default && stream_used[wu] != 0 && !state.cancel.load()) {
        if (!client_ptr->FinalizeWriteStreamWorker(
              w, stream_names[wu], &state, &finalized_rows[wu], &errors[wu])) {
          create_state.CancelAll();
          state.CancelAll();
        }
      }
      group.mark_done();
    });
  }

  wait_for_workers_with_progress(
    group,
    &pb,
    quiet,
    50,
    [&](RProgress::RProgress& bar) {
      double ratio = static_cast<double>(state.pages.load()) / static_cast<double>(m);
      bar.update(std::min(0.99, std::max(0.0, ratio)));
    },
    [&]() {
      create_state.CancelAll();
      state.CancelAll();
    });

  for (const auto& e : errors) {
    if (!e.empty()) Rcpp::stop(e.c_str());
  }

  int64_t committed_rows = total_rows;
  int used_count = 0;
  if (!is_default) {
    committed_rows = 0;
    std::vector<std::string> commit_streams;
    commit_streams.reserve(static_cast<size_t>(num_streams));
    for (int w = 0; w < num_streams; w++) {
      size_t wu = static_cast<size_t>(w);
      if (stream_used[wu] != 0) {
        commit_streams.push_back(stream_names[wu]);
        committed_rows += finalized_rows[wu];
      }
    }
    used_count = static_cast<int>(commit_streams.size());
    if (!commit_streams.empty()) {
      client_ptr->BatchCommitWriteStreams(parent, commit_streams);
    }
  } else {
    for (uint8_t u : stream_used) {
      if (u != 0) used_count++;
    }
  }

  if (!quiet) {
    pb.update(1);
    REprintf("Uploaded %lld rows across %d stream(s) in %zu message(s).\n",
             static_cast<long long>(committed_rows), std::max(1, used_count), m);
  }
  return committed_rows;
}

} // namespace

bool BigQueryReadClient::CreateWriteStreamWorker(
    int worker,
    int num_streams,
    const std::string& parent,
    google::cloud::bigquery::storage::v1::WriteStream_Type stream_type,
    ParallelState* create_state,
    const std::atomic<bool>& all_claimed,
    std::string* stream_name_out,
    std::string* err_out) {
  google::cloud::bigquery::storage::v1::CreateWriteStreamRequest req;
  req.set_parent(parent);
  req.mutable_write_stream()->set_type(stream_type);

  auto& stub = write_stubs_[static_cast<size_t>(worker) % write_stubs_.size()];
  google::cloud::bigquery::storage::v1::WriteStream resp;
  grpc::Status status;
  const int deadline_ms = (num_streams > 1 && worker > 0) ? 3500 : 15000;
  static constexpr int MAX_ATTEMPTS = 12;

  for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
    if (create_state->cancel.load() || all_claimed.load()) return false;
    grpc::ClientContext context;
    InitContext(&context, "parent=" + parent, deadline_ms);
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
    if (!bqs_cancellable_sleep(sleep_ms, create_state, &all_claimed)) return false;
  }
  if (all_claimed.load()) return false;
  if (err_out) {
    *err_out = "gRPC method CreateWriteStream error -> " + status.error_message();
  }
  return false;
}

bool BigQueryReadClient::FinalizeWriteStreamWorker(int worker,
                                                   const std::string& stream_name,
                                                   ParallelState* state,
                                                   int64_t* row_count_out,
                                                   std::string* err_out) {
  google::cloud::bigquery::storage::v1::FinalizeWriteStreamRequest req;
  req.set_name(stream_name);

  auto& stub = write_stubs_[static_cast<size_t>(worker) % write_stubs_.size()];
  google::cloud::bigquery::storage::v1::FinalizeWriteStreamResponse resp;
  grpc::Status status;

  for (int attempt = 0; attempt < 5; attempt++) {
    if (state->cancel.load()) return false;
    grpc::ClientContext context;
    InitContext(&context, "name=" + stream_name, 15000);
    state->Register(worker, &context);
    status = stub->FinalizeWriteStream(&context, req, &resp);
    state->Unregister(worker);
    if (status.ok()) {
      *row_count_out = resp.row_count();
      return true;
    }
    if (state->cancel.load() && status.error_code() == grpc::StatusCode::CANCELLED) {
      return false;
    }
    auto code = status.error_code();
    if ((code != grpc::StatusCode::UNAVAILABLE &&
         code != grpc::StatusCode::NOT_FOUND &&
         code != grpc::StatusCode::DEADLINE_EXCEEDED) ||
        attempt == 4) {
      break;
    }
    if (!bqs_cancellable_sleep(150 * (1 << attempt), state)) return false;
  }
  if (err_out) {
    *err_out = "gRPC method FinalizeWriteStream error -> " + status.error_message();
  }
  return false;
}

void BigQueryReadClient::BatchCommitWriteStreams(
    const std::string& parent,
    const std::vector<std::string>& stream_names) {
  google::cloud::bigquery::storage::v1::BatchCommitWriteStreamsRequest req;
  req.set_parent(parent);
  for (const auto& s : stream_names) req.add_write_streams(s);

  google::cloud::bigquery::storage::v1::BatchCommitWriteStreamsResponse resp;
  grpc::Status status;
  for (int attempt = 0; attempt < 6; attempt++) {
    Rcpp::checkUserInterrupt();
    grpc::ClientContext context;
    InitContext(&context, "parent=" + parent, 30000);
    status = write_stubs_[0]->BatchCommitWriteStreams(&context, req, &resp);
    auto code = status.error_code();
    if (status.ok() ||
        (code != grpc::StatusCode::UNAVAILABLE &&
         code != grpc::StatusCode::NOT_FOUND &&
         code != grpc::StatusCode::DEADLINE_EXCEEDED &&
         code != grpc::StatusCode::INTERNAL) ||
        attempt == 5) {
      break;
    }
    int sleep_ms = 150 * (1 << attempt);
    for (int slept = 0; slept < sleep_ms; slept += 50) {
      Rcpp::checkUserInterrupt();
      std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, sleep_ms - slept)));
    }
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

bool BigQueryReadClient::AppendArrowBatchesWorkStealing(
    int worker,
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
    std::string* err_out) {
  struct AssignedBatch {
    size_t batch_idx;
    int64_t stream_offset;
    bool acked;
  };

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

  std::vector<AssignedBatch> assigned{{first_idx, 0, false}};
  int64_t next_offset = (first_idx < batch_rows.size()) ? batch_rows[first_idx] : 0;
  size_t total_acked = 0;

  static constexpr int MAX_APPEND_ATTEMPTS = 12;
  for (int attempt = 0; attempt < MAX_APPEND_ATTEMPTS; attempt++) {
    if (state->cancel.load()) return false;

    std::vector<size_t> replay_slots;
    for (size_t s = 0; s < assigned.size(); s++) {
      if (!assigned[s].acked) replay_slots.push_back(s);
    }
    if (replay_slots.empty() && next_batch->load() >= m) {
      return !state->cancel.load();
    }

    grpc::ClientContext context;
    InitContext(&context, "write_stream=" + stream_name);
    state->Register(worker, &context);
    auto& stub = write_stubs_[static_cast<size_t>(worker) % write_stubs_.size()];
    auto stream = stub->AppendRows(&context);

    static constexpr size_t MAX_INFLIGHT = 3;
    std::mutex win_mu;
    std::condition_variable win_cv;
    std::deque<size_t> inflight_slots;
    size_t in_flight = 0;
    bool reader_done = false;
    std::atomic<bool> local_failed{false};
    int resp_err_code = 0;
    std::string resp_error;

    std::thread reader_thread([&]() {
      google::cloud::bigquery::storage::v1::AppendRowsResponse resp;
      while (stream->Read(&resp)) {
        if (local_failed.load() || state->cancel.load()) continue;
        size_t slot = 0;
        bool have_slot = false;
        {
          std::lock_guard<std::mutex> lk(win_mu);
          if (!inflight_slots.empty()) {
            slot = inflight_slots.front();
            inflight_slots.pop_front();
            have_slot = true;
          }
        }
        if (resp.has_error() && !(!is_default && resp.error().code() == 6 && have_slot)) {
          resp_err_code = resp.error().code();
          std::string err = "gRPC method AppendRows row error (" +
                            std::to_string(resp_err_code) + ") -> " + resp.error().message();
          for (int j = 0; j < resp.row_errors_size() && j < 5; j++) {
            err += "\n  Row " + std::to_string(resp.row_errors(j).index()) + ": " +
                   resp.row_errors(j).message();
          }
          resp_error = std::move(err);
          local_failed.store(true);
          {
            std::lock_guard<std::mutex> lk(win_mu);
            reader_done = true;
          }
          win_cv.notify_all();
          context.TryCancel();
          continue;
        }
        bool newly_acked = false;
        {
          std::lock_guard<std::mutex> lk(win_mu);
          if (in_flight > 0) in_flight--;
          if (have_slot && !assigned[slot].acked) {
            assigned[slot].acked = true;
            total_acked++;
            newly_acked = true;
          }
        }
        win_cv.notify_one();
        if (newly_acked) state->pages.fetch_add(1);
      }
      {
        std::lock_guard<std::mutex> lk(win_mu);
        reader_done = true;
      }
      win_cv.notify_all();
    });

    bool is_first = true;
    size_t replay_pos = 0;
    while (true) {
      size_t batch_idx = 0;
      int64_t req_offset = 0;
      {
        std::unique_lock<std::mutex> lk(win_mu);
        win_cv.wait(lk, [&]() {
          size_t cap = (is_default && total_acked == 0) ? 1u : MAX_INFLIGHT;
          return in_flight < cap || reader_done || local_failed.load() || state->cancel.load();
        });
        if (reader_done || local_failed.load() || state->cancel.load()) break;
        size_t slot = 0;
        if (replay_pos < replay_slots.size()) {
          slot = replay_slots[replay_pos++];
        } else {
          size_t next_idx = next_batch->fetch_add(1);
          if (next_idx >= m) {
            all_claimed->store(true);
            create_state->CancelAll();
            break;
          }
          if (next_idx + 1 >= m) {
            all_claimed->store(true);
            create_state->CancelAll();
          }
          slot = assigned.size();
          assigned.push_back({next_idx, next_offset, false});
          next_offset += (next_idx < batch_rows.size()) ? batch_rows[next_idx] : 0;
        }
        batch_idx = assigned[slot].batch_idx;
        req_offset = assigned[slot].stream_offset;
        inflight_slots.push_back(slot);
        in_flight++;
      }

      google::cloud::bigquery::storage::v1::AppendRowsRequest req;
      if (is_first) {
        req.set_write_stream(stream_name);
        req.mutable_arrow_rows()->mutable_writer_schema()->set_serialized_schema(schema_bytes);
        is_first = false;
      }
      if (!is_default) req.mutable_offset()->set_value(req_offset);
      req.mutable_arrow_rows()->mutable_rows()->set_serialized_record_batch(batches[batch_idx]);
      if (!stream->Write(req)) break;
    }

    if (local_failed.load() || state->cancel.load()) context.TryCancel();
    else stream->WritesDone();

    if (reader_thread.joinable()) reader_thread.join();
    grpc::Status status = stream->Finish();
    state->Unregister(worker);

    bool all_assigned_acked = true;
    for (const auto& a : assigned) {
      if (!a.acked) {
        all_assigned_acked = false;
        break;
      }
    }

    if (resp_error.empty() && status.ok() && all_assigned_acked) {
      return !state->cancel.load();
    }
    if (state->cancel.load() && status.error_code() == grpc::StatusCode::CANCELLED) {
      return false;
    }

    bool retryable = false;
    if (attempt < MAX_APPEND_ATTEMPTS - 1) {
      if (is_default) {
        retryable = (total_acked == 0) && (assigned.size() == 1) && (resp_err_code == 5);
      } else {
        auto sc = status.error_code();
        retryable = (
          resp_err_code == 5 || resp_err_code == 9 || resp_err_code == 10 ||
          resp_err_code == 13 || resp_err_code == 14 ||
          (resp_err_code == 0 && (
            sc == grpc::StatusCode::NOT_FOUND ||
            sc == grpc::StatusCode::FAILED_PRECONDITION ||
            sc == grpc::StatusCode::ABORTED ||
            sc == grpc::StatusCode::INTERNAL ||
            sc == grpc::StatusCode::UNAVAILABLE ||
            sc == grpc::StatusCode::DEADLINE_EXCEEDED ||
            !all_assigned_acked)));
      }
    }
    if (retryable) {
      int sleep_ms = std::min(4000, 100 * (1 << std::min(attempt, 6)));
      if (!bqs_cancellable_sleep(sleep_ms, state)) return false;
      continue;
    }

    if (!resp_error.empty()) {
      if (err_out) *err_out = resp_error;
    } else if (!status.ok()) {
      if (err_out) *err_out = "gRPC method AppendRows error -> " + status.error_message();
    } else if (err_out) {
      *err_out = "gRPC method AppendRows closed before acknowledging all batches.";
    }
    create_state->CancelAll();
    state->CancelAll();
    return false;
  }
  return false;
}

// [[Rcpp::export(rng=false)]]
void bqs_warm_write_channels(SEXP client, int n_channels = 0) {
  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);
  client_ptr->EnsureWriteChannels(static_cast<size_t>(n_channels > 0 ? n_channels : 10), true);
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
  std::string schema_bytes;
  std::vector<std::string> batches;
  std::vector<int64_t> batch_rows;
  int64_t total_rows = 0;
  std::string split_err;
  if (!bqs_ipc::split_arrow_ipc_stream(
        RAW(ipc_bytes),
        static_cast<size_t>(ipc_bytes.size()),
        &schema_bytes,
        &batches,
        &batch_rows,
        &total_rows,
        &split_err)) {
    Rcpp::stop(split_err.c_str());
  }
  if (batches.empty()) return 0;

  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);
  std::string parent = "projects/" + project + "/datasets/" + dataset + "/tables/" + table;

  int target_threads = max_stream_count > 0 ? max_stream_count : (max_threads > 0 ? max_threads : 10);
  const size_t min_bytes_per_stream = 3u * 1024u * 1024u;
  int by_size = std::max(
    1,
    static_cast<int>((static_cast<size_t>(ipc_bytes.size()) + min_bytes_per_stream - 1) / min_bytes_per_stream));
  int num_streams = std::min<int>({target_threads, by_size, static_cast<int>(batches.size())});

  return static_cast<double>(execute_parallel_write(
    client_ptr.get(), parent, schema_bytes, &batches, batch_rows, total_rows,
    stream_type, num_streams, quiet));
}

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
  auto cleanup_arrow = [&]() {
    for (auto& a : src_arrays) {
      if (a.release) a.release(&a);
    }
    src_arrays.clear();
    if (schema.release) schema.release(&schema);
  };

  int64_t total_rows = 0;
  int64_t total_uncompressed_bytes = 0;
  while (true) {
    ArrowArray arr;
    arr.release = nullptr;
    if (stream->get_next(stream, &arr) != 0) {
      const char* err = stream->get_last_error ? stream->get_last_error(stream) : "Failed to read ArrowArray";
      std::string err_str = err ? err : "Failed to read ArrowArray";
      cleanup_arrow();
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

  if (total_rows <= 0 || src_arrays.empty()) {
    cleanup_arrow();
    return 0;
  }

  size_t sch_len = bqs_ipc::strip_ipc_eos(RAW(schema_ipc), static_cast<size_t>(schema_ipc.size()));
  std::string schema_bytes(reinterpret_cast<const char*>(RAW(schema_ipc)), sch_len);

  int enc_threads = max_threads > 0 ? max_threads : BQS_DEFAULT_THREADS;
  int default_cap = (stream_type == "DEFAULT") ? 12 : 10;
  int max_streams = max_stream_count > 0
                      ? max_stream_count
                      : (max_threads > 0 ? max_threads : default_cap);

  const int64_t min_bytes_per_stream = 3584LL * 1024LL;
  int streams_by_bytes = std::max(
    1, static_cast<int>((total_uncompressed_bytes + min_bytes_per_stream - 1) / min_bytes_per_stream));
  int streams_by_rows = std::max(1, static_cast<int>(total_rows / 4096));
  int num_streams = max_stream_count > 0
                      ? std::max(1, std::min(max_stream_count, streams_by_rows))
                      : std::max(1, std::min({max_streams, streams_by_bytes, streams_by_rows}));

  std::vector<bqs_ipc::BatchSliceSpec> specs = bqs_ipc::plan_batch_slices(
    &schema,
    src_arrays,
    total_rows,
    total_uncompressed_bytes,
    row_batch_size,
    num_streams > 1 ? (num_streams * 2) : 1,
    true);

  num_streams = std::min<int>(num_streams, static_cast<int>(specs.size()));
  client_ptr->EnsureWriteChannels(static_cast<size_t>(num_streams), true);

  std::vector<int64_t> batch_rows(specs.size());
  for (size_t i = 0; i < specs.size(); i++) batch_rows[i] = specs[i].row_len;

  std::vector<std::string> batches;
  std::string encode_err;
  bool enc_ok = bqs_ipc::encode_batches_parallel(
    &schema, src_arrays, specs, use_zstd, enc_threads, &batches, &encode_err);
  cleanup_arrow();
  if (!enc_ok) Rcpp::stop(encode_err.c_str());

  return static_cast<double>(execute_parallel_write(
    client_ptr.get(), parent, schema_bytes, &batches, batch_rows, total_rows,
    stream_type, num_streams, quiet));
}
