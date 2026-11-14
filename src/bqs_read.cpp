#include "bqs_client.h"

#include <cstring>
#include <utility>

using google::cloud::bigquery::storage::v1::ArrowSerializationOptions;
using google::cloud::bigquery::storage::v1::ReadRowsResponse;
using google::cloud::bigquery::storage::v1::ReadSession;

namespace {

struct StreamBatches {
  std::vector<std::string> batches;
  std::vector<int64_t> rows;
  size_t bytes = 0;

  void add(ReadRowsResponse* response) {
    std::string* batch =
      response->mutable_arrow_record_batch()->release_serialized_record_batch();
    bytes += batch->size();
    batches.push_back(std::move(*batch));
    delete batch;
    rows.push_back(response->row_count());
  }
};

struct RawBatchSink {
  StreamBatches* out;

  bool accept(ReadRowsResponse* resp, std::string*) {
    out->add(resp);
    return true;
  }
};

struct DecodedBatchSink {
  std::vector<bqs_ipc::DecodedBatch>* out;
  bqs_ipc::ZstdDecompressContext* zctx;

  bool accept(ReadRowsResponse* resp, std::string* err_out) {
    std::string* raw_batch =
      resp->mutable_arrow_record_batch()->release_serialized_record_batch();
    if (!raw_batch) return true;
    std::unique_ptr<std::string> guard(raw_batch);
    if (raw_batch->empty()) return true;
    bqs_ipc::DecodedBatch decoded;
    if (!bqs_ipc::decode_ipc_record_batch(std::move(*raw_batch), &decoded, zctx, err_out)) {
      return false;
    }
    out->push_back(std::move(decoded));
    return true;
  }
};

ReadSession open_read_session(BigQueryReadClient* client_ptr,
                              const std::string& project,
                              const std::string& dataset,
                              const std::string& table,
                              const std::string& parent,
                              int64_t n,
                              const std::vector<std::string>& selected_fields,
                              const std::string& row_restriction,
                              double sample_percentage,
                              int64_t timestamp_seconds,
                              int32_t timestamp_nanos,
                              int32_t max_stream_count,
                              int32_t compression_codec,
                              int32_t target_threads,
                              bool overrequest_streams) {
  int32_t requested_streams = max_stream_count;
  if (n >= 0 && n <= 100000 && requested_streams == 0) {
    requested_streams = 1;
  }
  int32_t preferred_min_streams = target_threads;
  if (requested_streams > 0) {
    preferred_min_streams = std::min(requested_streams, preferred_min_streams);
  } else if (overrequest_streams) {
    requested_streams = std::max<int32_t>(target_threads * 2, 32);
  }
  return client_ptr->CreateReadSession(
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
}

void update_read_progress(RProgress::RProgress& pb,
                          const ParallelState& state,
                          double expected_rows,
                          int total_slots) {
  double ratio = 0.0;
  if (expected_rows > 0) {
    ratio = static_cast<double>(state.rows.load()) / expected_rows;
  } else if (total_slots <= 1) {
    ratio = static_cast<double>(state.progress_permille.load()) / 1000.0;
  } else {
    ratio = static_cast<double>(state.streams_done.load()) / total_slots;
  }
  pb.set_extra(state.throttle.load());
  pb.update(std::min(0.99, std::max(0.0, ratio)));
}

double resolve_expected_rows(const ReadSession& session, int64_t n) {
  double expected = static_cast<double>(session.estimated_row_count());
  if (n > 0 && (expected <= 0 || n < expected)) {
    expected = static_cast<double>(n);
  }
  return expected;
}

} // namespace

ReadSession BigQueryReadClient::CreateReadSession(
    const std::string& project,
    const std::string& dataset,
    const std::string& table,
    const std::string& parent,
    int64_t timestamp_seconds,
    int32_t timestamp_nanos,
    const std::vector<std::string>& selected_fields,
    const std::string& row_restriction,
    double sample_percentage,
    int32_t max_stream_count,
    int32_t compression_codec,
    int32_t preferred_min_stream_count) {
  google::cloud::bigquery::storage::v1::CreateReadSessionRequest req;
  ReadSession* read_session = req.mutable_read_session();
  std::string table_fullname =
    "projects/" + project + "/datasets/" + dataset + "/tables/" + table;
  read_session->set_table(table_fullname);
  read_session->set_data_format(google::cloud::bigquery::storage::v1::DataFormat::ARROW);
  if (timestamp_seconds > 0 || timestamp_nanos > 0) {
    auto* ts = read_session->mutable_table_modifiers()->mutable_snapshot_time();
    ts->set_seconds(timestamp_seconds);
    ts->set_nanos(timestamp_nanos);
  }
  for (const auto& f : selected_fields) {
    read_session->mutable_read_options()->add_selected_fields(f);
  }
  if (!row_restriction.empty()) {
    read_session->mutable_read_options()->set_row_restriction(row_restriction);
  }
  if (sample_percentage >= 0) {
    read_session->mutable_read_options()->set_sample_percentage(sample_percentage);
  }
  if (compression_codec == 1) {
    read_session->mutable_read_options()
      ->mutable_arrow_serialization_options()
      ->set_buffer_compression(ArrowSerializationOptions::LZ4_FRAME);
  } else if (compression_codec == 2) {
    read_session->mutable_read_options()
      ->mutable_arrow_serialization_options()
      ->set_buffer_compression(ArrowSerializationOptions::ZSTD);
  }
  if (max_stream_count > 0) req.set_max_stream_count(max_stream_count);
  if (preferred_min_stream_count > 0) {
    req.set_preferred_min_stream_count(preferred_min_stream_count);
  }
  req.set_parent("projects/" + parent);

  grpc::ClientContext context;
  InitContext(&context, "read_session.table=" + table_fullname);
  ReadSession resp;
  grpc::Status status = stubs_[0]->CreateReadSession(&context, req, &resp);
  if (!status.ok()) {
    std::string err = "gRPC method CreateReadSession error -> " + status.error_message();
    Rcpp::stop(err.c_str());
  }
  return resp;
}

bool BigQueryReadClient::TrySplitReadStreamWorker(size_t stub_idx,
                                                  const std::string& stream,
                                                  double fraction,
                                                  std::string* primary_out,
                                                  std::string* remainder_out) {
  google::cloud::bigquery::storage::v1::SplitReadStreamRequest req;
  req.set_name(stream);
  req.set_fraction(fraction);

  grpc::ClientContext context;
  InitContext(&context, "name=" + stream, 150);

  google::cloud::bigquery::storage::v1::SplitReadStreamResponse resp;
  grpc::Status status = stubs_[stub_idx % stubs_.size()]->SplitReadStream(&context, req, &resp);
  if (!status.ok() || resp.primary_stream().name().empty() ||
      resp.remainder_stream().name().empty()) {
    return false;
  }
  *primary_out = resp.primary_stream().name();
  *remainder_out = resp.remainder_stream().name();
  return true;
}

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
  int target_threads = max_threads > 0 ? max_threads : BQS_DEFAULT_THREADS;

  ReadSession read_session = open_read_session(
    client_ptr.get(),
    project,
    dataset,
    table,
    parent,
    n,
    selected_fields,
    row_restriction,
    sample_percentage,
    timestamp_seconds,
    timestamp_nanos,
    max_stream_count,
    compression_codec,
    target_threads,
    false);

  int n_streams = (n == 0) ? 0 : read_session.streams_size();
  const std::string& schema_bytes = read_session.arrow_schema().serialized_schema();
  std::vector<StreamBatches> stream_batches(static_cast<size_t>(std::max(1, n_streams)));

  long rows_count = 0;
  long pages_count = 0;

  if (n_streams > 0) {
    int num_threads = std::min(n_streams, target_threads);
    client_ptr->EnsureChannels(static_cast<size_t>(num_threads), true);

    ParallelState state(num_threads);
    state.n = n;
    std::vector<std::string> errors(static_cast<size_t>(n_streams));
    std::atomic<int> next_stream{0};
    WorkerGroup group(num_threads);

    for (int t = 0; t < num_threads; t++) {
      group.workers.emplace_back([&, t]() {
        while (!state.cancel.load()) {
          int idx = next_stream.fetch_add(1);
          if (idx >= n_streams) break;
          RawBatchSink sink{&stream_batches[static_cast<size_t>(idx)]};
          client_ptr->ReadRowsWorker(
            t, read_session.streams(idx).name(), sink, &state, &errors[static_cast<size_t>(idx)]);
          state.streams_done.fetch_add(1);
        }
        group.mark_done();
      });
    }

    RProgress::RProgress pb(
      "\033[42m\033[30mStreaming (:percent)\033[39m\033[49m [:bar] eta[:eta|:elapsed] throt[:extra]");
    pb.set_cursor_char(">");
    pb.set_total(100);
    double expected_rows = resolve_expected_rows(read_session, n);

    wait_for_workers_with_progress(
      group,
      &pb,
      quiet,
      50,
      [&](RProgress::RProgress& bar) { update_read_progress(bar, state, expected_rows, n_streams); },
      [&]() { state.CancelAll(); });

    for (const auto& e : errors) {
      if (!e.empty()) Rcpp::stop(e.c_str());
    }
    rows_count = static_cast<long>(state.rows.load());
    pages_count = static_cast<long>(state.pages.load());
    if (!quiet) pb.update(1);
  }

  if (!quiet) {
    REprintf("Streamed %ld rows in %ld messages.\n", rows_count, pages_count);
  }

  Rcpp::List streams_out;
  int64_t kept_rows = 0;
  for (int i = 0; i < n_streams; i++) {
    StreamBatches& sb = stream_batches[static_cast<size_t>(i)];
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
                         bool split_streams) {
  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);
  int target_threads = max_threads > 0 ? max_threads : 24;

  ReadSession read_session = open_read_session(
    client_ptr.get(),
    project,
    dataset,
    table,
    parent,
    n,
    selected_fields,
    row_restriction,
    sample_percentage,
    timestamp_seconds,
    timestamp_nanos,
    max_stream_count,
    compression_codec,
    target_threads,
    true);

  int orig_n = (n == 0) ? 0 : read_session.streams_size();
  const std::string& schema_bytes = read_session.arrow_schema().serialized_schema();

  bool do_async_split = split_streams && n < 0 && orig_n >= 2 &&
                        (orig_n * 2 <= target_threads) &&
                        read_session.estimated_total_bytes_scanned() >= 96LL * 1024LL * 1024LL;

  int total_slots = do_async_split ? (orig_n * 2) : std::max(1, orig_n);
  std::vector<std::vector<bqs_ipc::DecodedBatch>> stream_batches(static_cast<size_t>(total_slots));

  long rows_count = 0;
  long pages_count = 0;

  if (orig_n > 0) {
    RProgress::RProgress pb(
      "\033[42m\033[30mStreaming (:percent)\033[39m\033[49m [:bar] eta[:eta|:elapsed] throt[:extra]");
    pb.set_cursor_char(">");
    pb.set_total(100);
    double expected_rows = resolve_expected_rows(read_session, n);

    if (do_async_split) {
      int num_threads = orig_n * 2;
      client_ptr->EnsureChannels(static_cast<size_t>(num_threads), true);

      ParallelState state(num_threads);
      state.n = n;
      std::vector<std::string> errors(static_cast<size_t>(total_slots));
      WorkerGroup group(num_threads);

      std::mutex rem_mu;
      std::condition_variable rem_cv;
      std::vector<std::pair<int, std::string>> rem_queue;
      rem_queue.reserve(static_cast<size_t>(orig_n));
      int pending_splits = orig_n;

      for (int i = 0; i < orig_n; i++) {
        group.workers.emplace_back([&, i]() {
          bqs_ipc::ZstdDecompressContext zctx;
          std::string orig_stream = read_session.streams(i).name();
          std::string p1, p2;
          bool split_ok = !state.cancel.load() &&
                          client_ptr->TrySplitReadStreamWorker(
                            static_cast<size_t>(i), orig_stream, 0.5, &p1, &p2);
          {
            std::lock_guard<std::mutex> lk(rem_mu);
            if (split_ok && !state.cancel.load()) {
              rem_queue.emplace_back(2 * i + 1, std::move(p2));
            }
            pending_splits--;
          }
          rem_cv.notify_all();

          if (!state.cancel.load()) {
            size_t slot = static_cast<size_t>(2 * i);
            DecodedBatchSink sink{&stream_batches[slot], &zctx};
            client_ptr->ReadRowsWorker(i, split_ok ? p1 : orig_stream, sink, &state, &errors[slot]);
            state.streams_done.fetch_add(1);
          }
          group.mark_done();
        });
      }

      for (int h = orig_n; h < num_threads; h++) {
        group.workers.emplace_back([&, h]() {
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
              } else {
                break;
              }
            }
            if (has_item) {
              size_t slot = static_cast<size_t>(item.first);
              DecodedBatchSink sink{&stream_batches[slot], &zctx};
              client_ptr->ReadRowsWorker(h, item.second, sink, &state, &errors[slot]);
              state.streams_done.fetch_add(1);
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
        [&](RProgress::RProgress& bar) { update_read_progress(bar, state, expected_rows, total_slots); },
        [&]() {
          state.CancelAll();
          rem_cv.notify_all();
        });

      for (const auto& e : errors) {
        if (!e.empty()) Rcpp::stop(e.c_str());
      }
      rows_count = static_cast<long>(state.rows.load());
      pages_count = static_cast<long>(state.pages.load());
    } else {
      int num_threads = std::min(orig_n, target_threads);
      client_ptr->EnsureChannels(static_cast<size_t>(num_threads), true);

      ParallelState state(num_threads);
      state.n = n;
      std::vector<std::string> errors(static_cast<size_t>(orig_n));
      std::atomic<int> next_stream{0};
      WorkerGroup group(num_threads);

      for (int t = 0; t < num_threads; t++) {
        group.workers.emplace_back([&, t]() {
          bqs_ipc::ZstdDecompressContext zctx;
          while (!state.cancel.load()) {
            int idx = next_stream.fetch_add(1);
            if (idx >= orig_n) break;
            size_t u = static_cast<size_t>(idx);
            DecodedBatchSink sink{&stream_batches[u], &zctx};
            client_ptr->ReadRowsWorker(t, read_session.streams(idx).name(), sink, &state, &errors[u]);
            state.streams_done.fetch_add(1);
          }
          group.mark_done();
        });
      }

      wait_for_workers_with_progress(
        group,
        &pb,
        quiet,
        50,
        [&](RProgress::RProgress& bar) { update_read_progress(bar, state, expected_rows, orig_n); },
        [&]() { state.CancelAll(); });

      for (const auto& e : errors) {
        if (!e.empty()) Rcpp::stop(e.c_str());
      }
      rows_count = static_cast<long>(state.rows.load());
      pages_count = static_cast<long>(state.pages.load());
    }

    if (!quiet) pb.update(1);
  }

  if (!quiet) {
    REprintf("Streamed %ld rows in %ld messages.\n", rows_count, pages_count);
  }

  SEXP schema_xptr = PROTECT(bqs_parse_schema_ipc(schema_bytes));
  SEXP res = PROTECT(bqs_convert_decoded_streams(
    schema_xptr,
    &stream_batches,
    n,
    bigint,
    as_data_frame,
    target_threads));
  UNPROTECT(2);
  return res;
}
