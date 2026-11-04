#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
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
#include <nanoarrow/r.h>
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
                                const std::string& row_restriction,
                                const std::double_t& sample_percentage,
                                const int32_t max_stream_count = 0,
                                const int32_t compression_codec = 0
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

  // Read rows from a stream (single-stream path with progress bar)
  void ReadRows(const std::string& stream,
                std::vector<std::string>* batches,
                size_t* stream_bytes,
                std::int64_t& n,
                long int& rows_count,
                long int& pages_count,
                bool quiet,
                RProgress::RProgress* pb,
                bool last_stream) {

    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);

    google::cloud::bigquery::storage::v1::ReadRowsRequest method_request;
    method_request.set_read_stream(stream);
    method_request.set_offset(0);

    google::cloud::bigquery::storage::v1::ReadRowsResponse method_response;

    std::unique_ptr<grpc::ClientReader<google::cloud::bigquery::storage::v1::ReadRowsResponse> > reader(
        stub_->ReadRows(&context, method_request));

    while (reader->Read(&method_response)) {
      std::string* batch = method_response.mutable_arrow_record_batch()->release_serialized_record_batch();
      *stream_bytes += batch->size();
      batches->push_back(std::move(*batch));
      delete batch;

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
        Rcpp::checkUserInterrupt();
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

  // Read rows from a stream on a worker thread (no R API calls)
  void ReadRowsWorker(const std::string& stream,
                      std::vector<std::string>* batches,
                      size_t* stream_bytes,
                      long int* stream_rows,
                      long int* stream_pages,
                      std::atomic<bool>* cancel_flag,
                      std::string* error_msg) {
    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params", "read_stream=" + stream);
    context.AddMetadata("x-goog-api-client", client_info_);

    google::cloud::bigquery::storage::v1::ReadRowsRequest method_request;
    method_request.set_read_stream(stream);
    method_request.set_offset(0);

    google::cloud::bigquery::storage::v1::ReadRowsResponse method_response;
    std::unique_ptr<grpc::ClientReader<google::cloud::bigquery::storage::v1::ReadRowsResponse> > reader(
        stub_->ReadRows(&context, method_request));

    while (reader->Read(&method_response)) {
      if (cancel_flag->load(std::memory_order_relaxed)) {
        context.TryCancel();
        break;
      }
      std::string* batch = method_response.mutable_arrow_record_batch()->release_serialized_record_batch();
      *stream_bytes += batch->size();
      batches->push_back(std::move(*batch));
      delete batch;

      *stream_rows += method_response.row_count();
      *stream_pages += 1;
    }
    grpc::Status status = reader->Finish();
    if (!status.ok() && !(cancel_flag->load(std::memory_order_relaxed) && status.error_code() == grpc::StatusCode::CANCELLED)) {
      *error_msg = std::string("grpc method ReadRows error -> ") + status.error_message();
      cancel_flag->store(true, std::memory_order_relaxed);
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
  channel_arguments.SetMaxReceiveMessageSize(134217728);
  channel_arguments.SetServiceConfigJSON(service_configuration);

  BigQueryReadClient *client = new BigQueryReadClient(
    grpc::CreateCustomChannel(target, cred, channel_arguments)
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
                    int32_t compression_codec = 0) {

  Rcpp::XPtr<BigQueryReadClient> client_ptr(client);

  long int rows_count = 0;
  long int pages_count = 0;

  int32_t requested_streams = max_stream_count;
  if (n > 0 && n <= 100000 && requested_streams == 0) {
    requested_streams = 1;
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
    compression_codec);

  int n_streams = read_session.streams_size();
  const std::string& schema_bytes = read_session.arrow_schema().serialized_schema();

  std::vector<std::vector<std::string>> stream_batches(std::max(1, n_streams));
  std::vector<size_t> stream_bytes(std::max(1, n_streams), 0);

  if (n_streams <= 1 || n > 0) {
    RProgress::RProgress pb(
        "\033[42m\033[30mStreaming (:percent)\033[39m\033[49m [:bar] eta[:eta|:elapsed] throt[:extra]");
    pb.set_cursor_char(">");
    if (n > 0) {
      pb.set_total(n);
    } else {
      pb.set_total(100 * std::max(1, n_streams));
    }

    for (int i = 0; i < n_streams; i++) {
      client_ptr->ReadRows(read_session.streams(i).name(),
                           &stream_batches[i],
                           &stream_bytes[i],
                           n, rows_count, pages_count, quiet,
                           &pb, i == n_streams - 1);
      if (n > 0 && rows_count >= n) {
        break;
      }
    }
  } else {
    // Parallel multi-stream read for full table / unrestricted downloads
    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 4;
    int num_threads = std::min(n_streams, static_cast<int>(std::min(hw_threads, 16u)));

    std::vector<long int> per_stream_rows(n_streams, 0);
    std::vector<long int> per_stream_pages(n_streams, 0);
    std::vector<std::string> per_stream_errors(n_streams);
    std::atomic<int> next_stream(0);
    std::atomic<bool> cancel_flag(false);

    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (int t = 0; t < num_threads; t++) {
      workers.emplace_back([&]() {
        while (!cancel_flag.load(std::memory_order_relaxed)) {
          int idx = next_stream.fetch_add(1, std::memory_order_relaxed);
          if (idx >= n_streams) break;
          client_ptr->ReadRowsWorker(
            read_session.streams(idx).name(),
            &stream_batches[idx],
            &stream_bytes[idx],
            &per_stream_rows[idx],
            &per_stream_pages[idx],
            &cancel_flag,
            &per_stream_errors[idx]
          );
        }
      });
    }

    for (auto& w : workers) {
      w.join();
    }

    for (int i = 0; i < n_streams; i++) {
      if (!per_stream_errors[i].empty()) {
        Rcpp::stop(per_stream_errors[i].c_str());
      }
      rows_count += per_stream_rows[i];
      pages_count += per_stream_pages[i];
    }
  }

  if (!quiet) {
    REprintf("Streamed %ld rows in %ld messages.\n", rows_count, pages_count);
  }

  size_t total_bytes = schema_bytes.size();
  for (int i = 0; i < n_streams; i++) {
    total_bytes += stream_bytes[i];
  }

  Rcpp::RawVector out(total_bytes);
  uint8_t* dst = RAW(out);
  std::memcpy(dst, schema_bytes.data(), schema_bytes.size());
  dst += schema_bytes.size();
  for (int i = 0; i < n_streams; i++) {
    for (const auto& batch : stream_batches[i]) {
      std::memcpy(dst, batch.data(), batch.size());
      dst += batch.size();
    }
  }

  return out;
}

// -- Fast Arrow C Stream conversion helpers -----------------------------------

static bool parse_decimal128_scale(const char* fmt, int* scale_out) {
  if (!fmt || fmt[0] != 'd' || fmt[1] != ':') return false;
  const char* comma = std::strchr(fmt + 2, ',');
  if (!comma) return false;
  const char* comma2 = std::strchr(comma + 1, ',');
  if (comma2) {
    int bitwidth = std::atoi(comma2 + 1);
    if (bitwidth != 128) return false;
  }
  *scale_out = std::atoi(comma + 1);
  return true;
}

static void noop_release_schema(struct ArrowSchema* s) {
  s->release = NULL;
}

struct SubStreamData {
  ArrowSchema root_schema;
  std::vector<ArrowSchema*> child_schema_ptrs;
  std::vector<ArrowArray>* batches;
  std::vector<int> keep_cols;
  size_t batch_idx = 0;

  static int get_schema(struct ArrowArrayStream* s, struct ArrowSchema* out) {
    SubStreamData* self = static_cast<SubStreamData*>(s->private_data);
    *out = self->root_schema;
    out->release = noop_release_schema;
    return 0;
  }

  static int get_next(struct ArrowArrayStream* s, struct ArrowArray* out) {
    SubStreamData* self = static_cast<SubStreamData*>(s->private_data);
    if (self->batch_idx >= self->batches->size()) {
      out->release = NULL;
      return 0;
    }
    ArrowArray& src = (*self->batches)[self->batch_idx++];
    size_t k = self->keep_cols.size();
    ArrowArray** children = static_cast<ArrowArray**>(malloc(k * sizeof(ArrowArray*)));
    for (size_t j = 0; j < k; j++) {
      children[j] = src.children[self->keep_cols[j]];
    }
    out->length = src.length;
    out->null_count = src.null_count;
    out->offset = src.offset;
    out->n_buffers = src.n_buffers;
    out->n_children = k;
    out->buffers = src.buffers;
    out->children = children;
    out->dictionary = NULL;
    out->private_data = children;
    out->release = [](struct ArrowArray* a) {
      if (a->private_data) free(a->private_data);
      a->release = NULL;
    };
    return 0;
  }

  static const char* get_last_error(struct ArrowArrayStream*) { return NULL; }
  static void release(struct ArrowArrayStream* s) { s->release = NULL; }
};

// Fast hybrid ArrowArrayStream converter:
// Directly converts top-level Decimal128 (NUMERIC/BIGNUMERIC -> double) and
// Int64 (when bigint == "integer" -> integer32 with overflow warning) in C++,
// and delegates remaining columns to nanoarrow::convert_array_stream.
// [[Rcpp::export(rng=false)]]
SEXP bqs_convert_stream(SEXP stream_xptr,
                        SEXP ptype_df,
                        std::string bigint,
                        Rcpp::Function na_convert_stream) {
  struct ArrowArrayStream* stream = nanoarrow_array_stream_from_xptr(stream_xptr);
  struct ArrowSchema schema;
  if (stream->get_schema(stream, &schema) != 0) {
    const char* err = stream->get_last_error ? stream->get_last_error(stream) : "Failed to get schema from ArrowArrayStream";
    Rcpp::stop(err ? err : "Failed to get schema from ArrowArrayStream");
  }

  int64_t n_cols = schema.n_children;
  std::vector<int> dec_cols;
  std::vector<double> dec_scales;
  std::vector<int> int32_cols;
  std::vector<int> other_cols;

  for (int64_t j = 0; j < n_cols; j++) {
    const char* fmt = schema.children[j]->format;
    int scale = 0;
    if (
#if defined(__SIZEOF_INT128__)
        parse_decimal128_scale(fmt, &scale)
#else
        false
#endif
    ) {
      dec_cols.push_back(j);
      dec_scales.push_back(std::pow(10.0, -scale));
    } else if (bigint == "integer" && fmt && std::strcmp(fmt, "l") == 0) {
      int32_cols.push_back(j);
    } else {
      other_cols.push_back(j);
    }
  }

  if (dec_cols.empty() && int32_cols.empty()) {
    schema.release(&schema);
    return na_convert_stream(stream_xptr, ptype_df);
  }

  std::vector<ArrowArray> batches;
  int64_t total_rows = 0;
  while (true) {
    ArrowArray arr;
    arr.release = NULL;
    int rc = stream->get_next(stream, &arr);
    if (rc != 0) {
      const char* err = stream->get_last_error ? stream->get_last_error(stream) : "Failed to read next batch from ArrowArrayStream";
      std::string err_str = err ? err : "Failed to read next batch from ArrowArrayStream";
      for (auto& b : batches) b.release(&b);
      schema.release(&schema);
      Rcpp::stop(err_str.c_str());
    }
    if (arr.release == NULL) break;
    total_rows += arr.length;
    batches.push_back(arr);
  }

  Rcpp::List out_cols(n_cols);

#if defined(__SIZEOF_INT128__)
  // 1. Convert Decimal128 columns directly in C++
  for (size_t k = 0; k < dec_cols.size(); k++) {
    int col_idx = dec_cols[k];
    double scale_mult = dec_scales[k];
    Rcpp::NumericVector col_vec(total_rows);
    double* out_ptr = REAL(col_vec);
    int64_t pos = 0;
    for (size_t b = 0; b < batches.size(); b++) {
      ArrowArray* child = batches[b].children[col_idx];
      int64_t len = child->length;
      int64_t offset = child->offset;
      int64_t null_count = child->null_count;
      const uint8_t* valid = static_cast<const uint8_t*>(child->buffers[0]);
      const __int128* vals = static_cast<const __int128*>(child->buffers[1]);
      if (null_count == 0 || valid == nullptr) {
        for (int64_t i = 0; i < len; i++) {
          out_ptr[pos + i] = static_cast<double>(vals[offset + i]) * scale_mult;
        }
      } else {
        for (int64_t i = 0; i < len; i++) {
          int64_t idx = offset + i;
          if ((valid[idx >> 3] >> (idx & 7)) & 1) {
            out_ptr[pos + i] = static_cast<double>(vals[idx]) * scale_mult;
          } else {
            out_ptr[pos + i] = NA_REAL;
          }
        }
      }
      pos += len;
    }
    out_cols[col_idx] = col_vec;
  }
#endif

  // 2. Convert top-level int64 -> integer32 directly in C++ when bigint == "integer"
  bool any_int_overflow = false;
  for (size_t k = 0; k < int32_cols.size(); k++) {
    int col_idx = int32_cols[k];
    Rcpp::IntegerVector col_vec(total_rows);
    int* out_ptr = INTEGER(col_vec);
    int64_t pos = 0;
    for (size_t b = 0; b < batches.size(); b++) {
      ArrowArray* child = batches[b].children[col_idx];
      int64_t len = child->length;
      int64_t offset = child->offset;
      int64_t null_count = child->null_count;
      const uint8_t* valid = static_cast<const uint8_t*>(child->buffers[0]);
      const int64_t* vals = static_cast<const int64_t*>(child->buffers[1]);
      if (null_count == 0 || valid == nullptr) {
        for (int64_t i = 0; i < len; i++) {
          int64_t v = vals[offset + i];
          if (v < -2147483647LL || v > 2147483647LL) {
            out_ptr[pos + i] = NA_INTEGER;
            any_int_overflow = true;
          } else {
            out_ptr[pos + i] = static_cast<int>(v);
          }
        }
      } else {
        for (int64_t i = 0; i < len; i++) {
          int64_t idx = offset + i;
          if ((valid[idx >> 3] >> (idx & 7)) & 1) {
            int64_t v = vals[idx];
            if (v < -2147483647LL || v > 2147483647LL) {
              out_ptr[pos + i] = NA_INTEGER;
              any_int_overflow = true;
            } else {
              out_ptr[pos + i] = static_cast<int>(v);
            }
          } else {
            out_ptr[pos + i] = NA_INTEGER;
          }
        }
      }
      pos += len;
    }
    out_cols[col_idx] = col_vec;
  }
  if (any_int_overflow) {
    Rf_warning("NAs produced by integer overflow");
  }

  // 3. Delegate remaining columns to nanoarrow
  if (!other_cols.empty()) {
    Rcpp::List full_ptype(ptype_df);
    Rcpp::List sub_ptype(other_cols.size());
    Rcpp::CharacterVector full_names = full_ptype.names();
    Rcpp::CharacterVector sub_names(other_cols.size());
    for (size_t j = 0; j < other_cols.size(); j++) {
      sub_ptype[j] = full_ptype[other_cols[j]];
      sub_names[j] = full_names[other_cols[j]];
    }
    sub_ptype.attr("names") = sub_names;
    sub_ptype.attr("class") = "data.frame";
    sub_ptype.attr("row.names") = Rcpp::IntegerVector::create(NA_INTEGER, 0);

    SubStreamData sub_data;
    sub_data.batches = &batches;
    sub_data.keep_cols = other_cols;
    sub_data.child_schema_ptrs.resize(other_cols.size());
    for (size_t j = 0; j < other_cols.size(); j++) {
      sub_data.child_schema_ptrs[j] = schema.children[other_cols[j]];
    }
    sub_data.root_schema = schema;
    sub_data.root_schema.n_children = other_cols.size();
    sub_data.root_schema.children = sub_data.child_schema_ptrs.data();
    sub_data.root_schema.release = noop_release_schema;

    SEXP sub_stream_xptr = PROTECT(nanoarrow_array_stream_owning_xptr());
    struct ArrowArrayStream* sub_stream = reinterpret_cast<struct ArrowArrayStream*>(R_ExternalPtrAddr(sub_stream_xptr));
    sub_stream->get_schema = SubStreamData::get_schema;
    sub_stream->get_next = SubStreamData::get_next;
    sub_stream->get_last_error = SubStreamData::get_last_error;
    sub_stream->release = SubStreamData::release;
    sub_stream->private_data = &sub_data;

    Rcpp::List sub_res = na_convert_stream(sub_stream_xptr, sub_ptype, static_cast<double>(total_rows), R_PosInf);
    UNPROTECT(1);

    for (size_t j = 0; j < other_cols.size(); j++) {
      out_cols[other_cols[j]] = sub_res[j];
    }
  }

  for (auto& b : batches) b.release(&b);
  schema.release(&schema);

  Rcpp::List full_ptype(ptype_df);
  out_cols.attr("names") = full_ptype.names();
  out_cols.attr("class") = "data.frame";
  out_cols.attr("row.names") = Rcpp::IntegerVector::create(NA_INTEGER, -static_cast<int>(total_rows));
  return out_cols;
}

