#ifndef BQS_ARROW_IPC_H
#define BQS_ARROW_IPC_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <Rcpp.h>
#include <nanoarrow/r.h>

namespace bqs_ipc {

bool has_zstd_support();

template <typename T>
inline T load_unaligned(const void* ptr) {
  T val;
  std::memcpy(&val, ptr, sizeof(T));
  return val;
}

int fixed_width_bytes(const char* fmt);
bool can_fast_encode_field(const ArrowSchema* sch);
bool can_fast_encode_schema(const ArrowSchema* root_schema);

int64_t estimate_field_bytes(const ArrowSchema* sch,
                             const ArrowArray* arr,
                             int64_t logical_start,
                             int64_t len);
int64_t estimate_batch_bytes(const ArrowSchema* root_schema,
                             const ArrowArray* root_array);

int64_t count_nulls_in_bitmap(const uint8_t* valid, int64_t start, int64_t len);
const uint8_t* slice_bitmap(const uint8_t* src_bits,
                            int64_t start,
                            int64_t len,
                            std::vector<uint8_t>* scratch);

struct FieldNodeDesc {
  int64_t length;
  int64_t null_count;
};

struct BufferDesc {
  int64_t offset;
  int64_t length;
};

struct BatchSliceSpec {
  size_t array_idx;
  int64_t row_start;
  int64_t row_len;
};

class RecordBatchEncoder {
public:
  explicit RecordBatchEncoder(bool use_zstd, int zstd_level = 1);

  std::string encode(const ArrowSchema* root_schema,
                     const ArrowArray* root_array,
                     int64_t row_start,
                     int64_t row_len,
                     std::string* err_out);

private:
  bool use_zstd_;
  int zstd_level_;
  std::vector<FieldNodeDesc> nodes_;
  std::vector<BufferDesc> buffers_;
  std::vector<uint8_t> body_;
  std::vector<uint8_t> bitmap_scratch_;
  std::vector<int32_t> offs32_scratch_;
  std::vector<int64_t> offs64_scratch_;

  bool append_buffer(const uint8_t* raw_data, int64_t raw_len, std::string* err_out);

  template <typename OffsetT>
  bool append_sliced_offsets(const ArrowArray* arr,
                             int64_t start,
                             int64_t len,
                             std::vector<OffsetT>* scratch,
                             OffsetT* base_out,
                             OffsetT* end_out,
                             std::string* err_out);

  bool visit_field(const ArrowSchema* sch,
                   const ArrowArray* arr,
                   int64_t logical_start,
                   int64_t len,
                   std::string* err_out);
};

std::vector<BatchSliceSpec> plan_batch_slices(const ArrowSchema* schema,
                                              const std::vector<ArrowArray>& src_arrays,
                                              int64_t total_rows,
                                              int64_t total_uncompressed_bytes,
                                              int row_batch_size,
                                              int target_min_batches = 1,
                                              bool align_64 = false);

size_t strip_ipc_eos(const uint8_t* data, size_t len);

bool encode_batches_parallel(const ArrowSchema* schema,
                             const std::vector<ArrowArray>& src_arrays,
                             const std::vector<BatchSliceSpec>& specs,
                             bool use_zstd,
                             int n_threads,
                             std::vector<std::string>* out_batches,
                             std::string* err_out);

struct DecodedBatch {
  int64_t length = 0;
  std::vector<FieldNodeDesc> nodes;
  std::vector<BufferDesc> buffers;
  std::string raw_ipc;
  size_t raw_body_offset = 0;
  std::unique_ptr<uint8_t[]> decompressed;
  const uint8_t* body_ptr = nullptr;
  size_t body_size = 0;

  DecodedBatch() = default;
  DecodedBatch(const DecodedBatch&) = delete;
  DecodedBatch& operator=(const DecodedBatch&) = delete;

  DecodedBatch(DecodedBatch&& other) noexcept
      : length(other.length),
        nodes(std::move(other.nodes)),
        buffers(std::move(other.buffers)),
        raw_ipc(std::move(other.raw_ipc)),
        raw_body_offset(other.raw_body_offset),
        decompressed(std::move(other.decompressed)),
        body_ptr(nullptr),
        body_size(other.body_size) {
    refresh_body_ptr();
    other.body_ptr = nullptr;
    other.body_size = 0;
  }

  DecodedBatch& operator=(DecodedBatch&& other) noexcept {
    if (this != &other) {
      length = other.length;
      nodes = std::move(other.nodes);
      buffers = std::move(other.buffers);
      raw_ipc = std::move(other.raw_ipc);
      raw_body_offset = other.raw_body_offset;
      decompressed = std::move(other.decompressed);
      body_size = other.body_size;
      refresh_body_ptr();
      other.body_ptr = nullptr;
      other.body_size = 0;
    }
    return *this;
  }

  void refresh_body_ptr() {
    if (decompressed) {
      body_ptr = decompressed.get();
    } else if (!raw_ipc.empty() && raw_body_offset <= raw_ipc.size()) {
      body_ptr = reinterpret_cast<const uint8_t*>(raw_ipc.data()) + raw_body_offset;
    } else {
      body_ptr = nullptr;
    }
  }

  void clear_memory() {
    std::string().swap(raw_ipc);
    raw_body_offset = 0;
    decompressed.reset();
    body_ptr = nullptr;
    body_size = 0;
  }
};

class ZstdDecompressContext {
public:
  ZstdDecompressContext();
  ~ZstdDecompressContext();
  ZstdDecompressContext(const ZstdDecompressContext&) = delete;
  ZstdDecompressContext& operator=(const ZstdDecompressContext&) = delete;
  void* get() const { return dctx_; }

private:
  void* dctx_ = nullptr;
};

bool decode_ipc_record_batch(std::string&& raw_msg,
                             DecodedBatch* out,
                             ZstdDecompressContext* zctx,
                             std::string* err_out);

void count_schema_nodes_and_buffers(const ArrowSchema* sch,
                                    size_t* n_nodes,
                                    size_t* n_bufs);

bool validate_borrowed_field_buffers(const ArrowSchema* sch,
                                     const DecodedBatch& batch,
                                     int64_t min_len,
                                     bool exact_len,
                                     size_t* node_pos,
                                     size_t* buf_pos,
                                     std::string* err_out);

struct BorrowedArrayPriv {
  const void* buffers[3] = {nullptr, nullptr, nullptr};
};

void release_borrowed_array(struct ArrowArray* a);

ArrowArray* build_borrowed_field_array(const ArrowSchema* sch,
                                       const DecodedBatch& batch,
                                       size_t* node_pos,
                                       size_t* buf_pos);

void truncate_borrowed_field_array(ArrowArray* arr,
                                   const ArrowSchema* sch,
                                   int64_t new_len);

bool split_arrow_ipc_stream(const uint8_t* data,
                            size_t len,
                            std::string* schema_out,
                            std::vector<std::string>* batches_out,
                            std::vector<int64_t>* batch_rows_out,
                            int64_t* total_rows_out,
                            std::string* err_out);

} // namespace bqs_ipc

SEXP bqs_parse_schema_ipc(const std::string& schema_bytes);

SEXP bqs_convert_decoded_streams(
    SEXP schema_xptr,
    std::vector<std::vector<bqs_ipc::DecodedBatch>>* stream_batches,
    int64_t n_max,
    const std::string& bigint,
    bool as_data_frame,
    int n_threads);

#endif // BQS_ARROW_IPC_H