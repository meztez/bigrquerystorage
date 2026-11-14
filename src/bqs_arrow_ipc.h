#ifndef BQS_ARROW_IPC_H
#define BQS_ARROW_IPC_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <Rcpp.h>
#include <nanoarrow/r.h>

#ifdef HAVE_BQS_ZSTD
#include <zstd.h>
#endif

namespace bqs_ipc {

inline bool has_zstd_support() {
#ifdef HAVE_BQS_ZSTD
  return true;
#else
  return false;
#endif
}

template <typename T>
inline T load_unaligned(const void* ptr) {
  T val;
  std::memcpy(&val, ptr, sizeof(T));
  return val;
}

// Determine fixed-width byte size for primitive/temporal/decimal Arrow formats,
// or -1 if not a fixed-width leaf format.
inline int fixed_width_bytes(const char* fmt) {
  if (!fmt || fmt[0] == '\0') return -1;
  switch (fmt[0]) {
    case 'c':
    case 'C':
      return fmt[1] == '\0' ? 1 : -1;
    case 's':
    case 'S':
    case 'e':
      return fmt[1] == '\0' ? 2 : -1;
    case 'i':
    case 'I':
    case 'f':
      return fmt[1] == '\0' ? 4 : -1;
    case 'l':
    case 'L':
    case 'g':
      return fmt[1] == '\0' ? 8 : -1;
    case 't':
      if (std::strcmp(fmt, "tdD") == 0 || std::strcmp(fmt, "tts") == 0 ||
          std::strcmp(fmt, "ttm") == 0) {
        return 4;
      }
      if (std::strcmp(fmt, "tdm") == 0 || std::strcmp(fmt, "ttu") == 0 ||
          std::strcmp(fmt, "ttn") == 0 || std::strncmp(fmt, "ts", 2) == 0) {
        return 8;
      }
      return -1;
    case 'd':
      if (fmt[1] == ':') {
        const char* c1 = std::strchr(fmt + 2, ',');
        if (!c1) return -1;
        const char* c2 = std::strchr(c1 + 1, ',');
        if (c2) {
          int bw = std::atoi(c2 + 1);
          return bw > 0 ? (bw / 8) : -1;
        }
        return 16;
      }
      return -1;
    case 'w':
      if (fmt[1] == ':') {
        int w = std::atoi(fmt + 2);
        return w >= 0 ? w : -1;
      }
      return -1;
    default:
      return -1;
  }
}

// Check recursively whether all fields in `schema` are supported by the fast
// zero-copy / parallel C++ RecordBatch encoder.
inline bool can_fast_encode_field(const ArrowSchema* sch) {
  if (!sch || !sch->format) return false;
  if (sch->dictionary != nullptr) return false;
  const char* fmt = sch->format;
  if (std::strcmp(fmt, "n") == 0 || std::strcmp(fmt, "b") == 0 ||
      std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0 ||
      std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    return true;
  }
  if (fixed_width_bytes(fmt) >= 0) {
    return true;
  }
  if (std::strcmp(fmt, "+s") == 0) {
    for (int64_t j = 0; j < sch->n_children; j++) {
      if (!can_fast_encode_field(sch->children[j])) return false;
    }
    return true;
  }
  if (std::strcmp(fmt, "+l") == 0 || std::strcmp(fmt, "+L") == 0 ||
      std::strncmp(fmt, "+w:", 3) == 0) {
    return sch->n_children == 1 && can_fast_encode_field(sch->children[0]);
  }
  return false;
}

inline bool can_fast_encode_schema(const ArrowSchema* root_schema) {
  if (!root_schema || !root_schema->format || std::strcmp(root_schema->format, "+s") != 0) {
    return false;
  }
  for (int64_t j = 0; j < root_schema->n_children; j++) {
    if (!can_fast_encode_field(root_schema->children[j])) return false;
  }
  return true;
}

// Estimate uncompressed byte size of a field slice [logical_start, logical_start + len)
inline int64_t estimate_field_bytes(const ArrowSchema* sch,
                                    const ArrowArray* arr,
                                    int64_t logical_start,
                                    int64_t len) {
  if (!sch || !arr || len <= 0) return 0;
  const char* fmt = sch->format;
  if (std::strcmp(fmt, "n") == 0) {
    return 0;
  }
  int64_t start = arr->offset + logical_start;
  int64_t total = (arr->null_count != 0 && arr->buffers != nullptr && arr->buffers[0] != nullptr)
                    ? ((len + 7) >> 3)
                    : 0;

  if (std::strcmp(fmt, "b") == 0) {
    return total + ((len + 7) >> 3);
  }
  int fw = fixed_width_bytes(fmt);
  if (fw >= 0) {
    return total + len * fw;
  }
  if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0) {
    const int32_t* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                            ? static_cast<const int32_t*>(arr->buffers[1])
                            : nullptr;
    int64_t data_bytes = offs ? (static_cast<int64_t>(offs[start + len]) - static_cast<int64_t>(offs[start])) : 0;
    return total + (len + 1) * 4 + std::max<int64_t>(0, data_bytes);
  }
  if (std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    const int64_t* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                            ? static_cast<const int64_t*>(arr->buffers[1])
                            : nullptr;
    int64_t data_bytes = offs ? (offs[start + len] - offs[start]) : 0;
    return total + (len + 1) * 8 + std::max<int64_t>(0, data_bytes);
  }
  if (std::strcmp(fmt, "+s") == 0) {
    for (int64_t j = 0; j < sch->n_children; j++) {
      const ArrowArray* child_arr = (arr->children != nullptr && j < arr->n_children) ? arr->children[j] : nullptr;
      total += estimate_field_bytes(sch->children[j], child_arr, start, len);
    }
    return total;
  }
  if (std::strcmp(fmt, "+l") == 0) {
    const int32_t* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                            ? static_cast<const int32_t*>(arr->buffers[1])
                            : nullptr;
    int32_t c_start = offs ? offs[start] : 0;
    int32_t c_end = offs ? offs[start + len] : 0;
    const ArrowArray* child_arr = (arr->children != nullptr && arr->n_children > 0) ? arr->children[0] : nullptr;
    return total + (len + 1) * 4 +
           estimate_field_bytes(sch->children[0], child_arr, c_start, std::max<int32_t>(0, c_end - c_start));
  }
  if (std::strcmp(fmt, "+L") == 0) {
    const int64_t* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                            ? static_cast<const int64_t*>(arr->buffers[1])
                            : nullptr;
    int64_t c_start = offs ? offs[start] : 0;
    int64_t c_end = offs ? offs[start + len] : 0;
    const ArrowArray* child_arr = (arr->children != nullptr && arr->n_children > 0) ? arr->children[0] : nullptr;
    return total + (len + 1) * 8 +
           estimate_field_bytes(sch->children[0], child_arr, c_start, std::max<int64_t>(0, c_end - c_start));
  }
  if (std::strncmp(fmt, "+w:", 3) == 0) {
    int64_t w = std::max<int64_t>(0, std::atoll(fmt + 3));
    const ArrowArray* child_arr = (arr->children != nullptr && arr->n_children > 0) ? arr->children[0] : nullptr;
    return total + estimate_field_bytes(sch->children[0], child_arr, start * w, len * w);
  }
  return total;
}

inline int64_t estimate_batch_bytes(const ArrowSchema* root_schema, const ArrowArray* root_array) {
  if (!root_schema || !root_array || root_array->length <= 0) return 0;
  int64_t total = 0;
  for (int64_t j = 0; j < root_schema->n_children; j++) {
    total += estimate_field_bytes(
      root_schema->children[j], root_array->children[j], root_array->offset, root_array->length);
  }
  return total;
}

inline int64_t count_nulls_in_bitmap(const uint8_t* valid, int64_t start, int64_t len) {
  if (!valid || len <= 0) return 0;
  int64_t set_bits = 0;
  int64_t i = 0;
  while (i < len && ((start + i) & 63) != 0) {
    int64_t idx = start + i;
    set_bits += (valid[idx >> 3] >> (idx & 7)) & 1;
    i++;
  }
  while (i + 64 <= len) {
    uint64_t word = 0;
    std::memcpy(&word, valid + ((start + i) >> 3), 8);
#if defined(__GNUC__) || defined(__clang__)
    set_bits += __builtin_popcountll(word);
#else
    for (int b = 0; b < 64; b++) set_bits += static_cast<int64_t>((word >> b) & 1ULL);
#endif
    i += 64;
  }
  while (i < len) {
    int64_t idx = start + i;
    set_bits += (valid[idx >> 3] >> (idx & 7)) & 1;
    i++;
  }
  return len - set_bits;
}

inline const uint8_t* slice_bitmap(const uint8_t* src_bits,
                                   int64_t start,
                                   int64_t len,
                                   std::vector<uint8_t>* scratch) {
  if (len <= 0 || src_bits == nullptr) return nullptr;
  int64_t out_bytes = (len + 7) >> 3;
  int bit_off = static_cast<int>(start & 7);
  const uint8_t* byte_ptr = src_bits + (start >> 3);
  if (bit_off == 0) {
    return byte_ptr;
  }
  scratch->assign(static_cast<size_t>(out_bytes), 0);
  uint8_t* dst = scratch->data();
  int64_t total_src_bytes = ((start + len + 7) >> 3) - (start >> 3);
  for (int64_t i = 0; i < out_bytes; i++) {
    uint8_t lo = byte_ptr[i] >> bit_off;
    uint8_t hi = (i + 1 < total_src_bytes)
                   ? static_cast<uint8_t>(byte_ptr[i + 1] << (8 - bit_off))
                   : 0;
    dst[i] = lo | hi;
  }
  int rem = static_cast<int>(len & 7);
  if (rem != 0) {
    dst[out_bytes - 1] &= static_cast<uint8_t>((1u << rem) - 1u);
  }
  return dst;
}

struct FieldNodeDesc {
  int64_t length;
  int64_t null_count;
};

struct BufferDesc {
  int64_t offset;
  int64_t length;
};

class RecordBatchEncoder {
public:
  explicit RecordBatchEncoder(bool use_zstd, int zstd_level = 1)
      : use_zstd_(use_zstd), zstd_level_(zstd_level) {
#ifndef HAVE_BQS_ZSTD
    use_zstd_ = false;
#endif
  }

  std::string encode(const ArrowSchema* root_schema,
                     const ArrowArray* root_array,
                     int64_t row_start,
                     int64_t row_len,
                     std::string* err_out) {
    nodes_.clear();
    buffers_.clear();
    body_.clear();

    int64_t est_raw = 0;
    for (int64_t j = 0; j < root_schema->n_children; j++) {
      est_raw += estimate_field_bytes(
        root_schema->children[j], root_array->children[j], root_array->offset + row_start, row_len);
    }
    body_.reserve(static_cast<size_t>(use_zstd_ ? (est_raw / 2 + 4096) : (est_raw + 1024)));

    for (int64_t j = 0; j < root_schema->n_children; j++) {
      if (!visit_field(root_schema->children[j],
                       root_array->children[j],
                       root_array->offset + row_start,
                       row_len,
                       err_out)) {
        return std::string();
      }
    }

    // Build exact Arrow IPC Message(RecordBatch) FlatBuffer header:
    // Fixed 100-byte prefix [0x00..0x63], followed by:
    // - buffers vector at 0x64: u32 num_buffers + num_buffers * 16 bytes
    // - 4 bytes zero pad
    // - nodes vector at 0x6c + num_buffers * 16: u32 num_nodes + num_nodes * 16 bytes
    const uint32_t num_buffers = static_cast<uint32_t>(buffers_.size());
    const uint32_t num_nodes = static_cast<uint32_t>(nodes_.size());
    const uint32_t nodes_pos = 0x6Cu + num_buffers * 16u;
    const uint32_t meta_len = nodes_pos + 4u + num_nodes * 16u; // Always 0 mod 8
    const int64_t body_len = static_cast<int64_t>(body_.size()); // Always 0 mod 8

    std::string out;
    out.resize(8u + static_cast<size_t>(meta_len) + body_.size());
    uint8_t* p = reinterpret_cast<uint8_t*>(&out[0]);
    std::memset(p, 0, 8u + static_cast<size_t>(meta_len));

    // 8-byte IPC continuation + metadata_size prefix
    const uint32_t continuation = 0xFFFFFFFFu;
    const int32_t meta_len_i32 = static_cast<int32_t>(meta_len);
    std::memcpy(p + 0, &continuation, 4);
    std::memcpy(p + 4, &meta_len_i32, 4);

    uint8_t* fb = p + 8;
    // Root offset -> Message table at 0x14
    put_u32(fb + 0x00, 0x14u);

    // Message vtable at 0x08 (12 bytes)
    put_u16(fb + 0x08, 12u);
    put_u16(fb + 0x0A, 24u);
    put_u16(fb + 0x0C, 6u);   // version -> +6 (0x1A)
    put_u16(fb + 0x0E, 5u);   // header_type -> +5 (0x19)
    put_u16(fb + 0x10, 8u);   // header -> +8 (0x1C)
    put_u16(fb + 0x12, 12u);  // bodyLength -> +12 (0x20)

    // Message table at 0x14 (24 bytes)
    put_i32(fb + 0x14, 12);   // vtable soffset (0x14 - 0x08)
    fb[0x19] = 3u;            // MessageHeader::RecordBatch = 3
    put_i16(fb + 0x1A, 4);    // MetadataVersion::V5 = 4
    put_u32(fb + 0x1C, 0x1Cu);// RecordBatch table at 0x1C + 0x1C = 0x38
    put_i64(fb + 0x20, body_len);

    // RecordBatch vtable at 0x2C (12 bytes)
    put_u16(fb + 0x2C, 12u);
    put_u16(fb + 0x2E, 24u);
    put_u16(fb + 0x30, 16u);  // length -> +16 (0x48)
    put_u16(fb + 0x32, 4u);   // nodes -> +4 (0x3C)
    put_u16(fb + 0x34, 8u);   // buffers -> +8 (0x40)
    put_u16(fb + 0x36, use_zstd_ ? 12u : 0u); // compression -> +12 (0x44) or absent

    // RecordBatch table at 0x38 (24 bytes)
    put_i32(fb + 0x38, 12);                       // vtable soffset (0x38 - 0x2C)
    put_u32(fb + 0x3C, nodes_pos - 0x3Cu);        // nodes vector uoffset
    put_u32(fb + 0x40, 0x64u - 0x40u);            // buffers vector uoffset (0x24)
    put_u32(fb + 0x44, use_zstd_ ? (0x5Cu - 0x44u) : 0u); // BodyCompression table at 0x5C
    put_i64(fb + 0x48, row_len);

    if (use_zstd_) {
      // BodyCompression vtable at 0x56 (6 bytes) & table at 0x5C (8 bytes)
      put_u16(fb + 0x56, 6u);
      put_u16(fb + 0x58, 8u);
      put_u16(fb + 0x5A, 7u);  // codec -> +7 (0x63); method = default 0 (BUFFER)
      put_i32(fb + 0x5C, 6);   // vtable soffset (0x5C - 0x56)
      fb[0x63] = 1u;           // CompressionType::ZSTD = 1
    }

    // Buffers vector at 0x64
    put_u32(fb + 0x64, num_buffers);
    uint8_t* buf_dst = fb + 0x68;
    for (uint32_t i = 0; i < num_buffers; i++) {
      put_i64(buf_dst + i * 16u + 0u, buffers_[i].offset);
      put_i64(buf_dst + i * 16u + 8u, buffers_[i].length);
    }

    // Nodes vector at nodes_pos (4 zero pad bytes at 0x68 + num_buffers*16 already zeroed)
    put_u32(fb + nodes_pos, num_nodes);
    uint8_t* node_dst = fb + nodes_pos + 4u;
    for (uint32_t i = 0; i < num_nodes; i++) {
      put_i64(node_dst + i * 16u + 0u, nodes_[i].length);
      put_i64(node_dst + i * 16u + 8u, nodes_[i].null_count);
    }

    if (!body_.empty()) {
      std::memcpy(p + 8u + static_cast<size_t>(meta_len), body_.data(), body_.size());
    }
    return out;
  }

private:
  bool use_zstd_;
  int zstd_level_;
  std::vector<FieldNodeDesc> nodes_;
  std::vector<BufferDesc> buffers_;
  std::vector<uint8_t> body_;
  std::vector<uint8_t> bitmap_scratch_;
  std::vector<int32_t> offs32_scratch_;
  std::vector<int64_t> offs64_scratch_;

  static inline void put_u16(uint8_t* dst, uint16_t v) { std::memcpy(dst, &v, 2); }
  static inline void put_i16(uint8_t* dst, int16_t v) { std::memcpy(dst, &v, 2); }
  static inline void put_u32(uint8_t* dst, uint32_t v) { std::memcpy(dst, &v, 4); }
  static inline void put_i32(uint8_t* dst, int32_t v) { std::memcpy(dst, &v, 4); }
  static inline void put_i64(uint8_t* dst, int64_t v) { std::memcpy(dst, &v, 8); }

  bool append_buffer(const uint8_t* raw_data, int64_t raw_len, std::string* err_out) {
    int64_t buf_offset = static_cast<int64_t>(body_.size());
    if (raw_len <= 0 || raw_data == nullptr) {
      buffers_.push_back({buf_offset, 0});
      return true;
    }
#ifdef HAVE_BQS_ZSTD
    if (use_zstd_) {
      size_t bound = ZSTD_compressBound(static_cast<size_t>(raw_len));
      size_t start_pos = body_.size();
      body_.resize(start_pos + 8u + bound);
      size_t csize = ZSTD_compress(
        body_.data() + start_pos + 8u,
        bound,
        raw_data,
        static_cast<size_t>(raw_len),
        zstd_level_);
      if (ZSTD_isError(csize)) {
        if (err_out) *err_out = std::string("ZSTD_compress failed: ") + ZSTD_getErrorName(csize);
        return false;
      }
      put_i64(body_.data() + start_pos, raw_len);
      int64_t written_len = 8 + static_cast<int64_t>(csize);
      int64_t padded_len = (written_len + 7) & ~static_cast<int64_t>(7);
      body_.resize(start_pos + static_cast<size_t>(padded_len), 0);
      if (padded_len > written_len) {
        std::memset(body_.data() + start_pos + static_cast<size_t>(written_len),
                    0,
                    static_cast<size_t>(padded_len - written_len));
      }
      buffers_.push_back({buf_offset, written_len});
      return true;
    }
#else
    (void)err_out;
    (void)zstd_level_;
#endif
    size_t start_pos = body_.size();
    int64_t padded_len = (raw_len + 7) & ~static_cast<int64_t>(7);
    body_.resize(start_pos + static_cast<size_t>(padded_len), 0);
    std::memcpy(body_.data() + start_pos, raw_data, static_cast<size_t>(raw_len));
    if (padded_len > raw_len) {
      std::memset(body_.data() + start_pos + static_cast<size_t>(raw_len),
                  0,
                  static_cast<size_t>(padded_len - raw_len));
    }
    buffers_.push_back({buf_offset, raw_len});
    return true;
  }

  bool visit_field(const ArrowSchema* sch,
                   const ArrowArray* arr,
                   int64_t logical_start,
                   int64_t len,
                   std::string* err_out) {
    const char* fmt = sch->format;
    int64_t start = arr->offset + logical_start;

    if (std::strcmp(fmt, "n") == 0) {
      nodes_.push_back({len, len});
      return true;
    }

    const uint8_t* valid = (arr->buffers != nullptr && arr->n_buffers > 0)
                             ? static_cast<const uint8_t*>(arr->buffers[0])
                             : nullptr;
    int64_t slice_nulls = 0;
    if (arr->null_count != 0 && valid != nullptr && len > 0) {
      slice_nulls = count_nulls_in_bitmap(valid, start, len);
    }
    nodes_.push_back({len, slice_nulls});

    if (slice_nulls > 0) {
      const uint8_t* vslice = slice_bitmap(valid, start, len, &bitmap_scratch_);
      if (!append_buffer(vslice, (len + 7) >> 3, err_out)) return false;
    } else {
      if (!append_buffer(nullptr, 0, err_out)) return false;
    }

    if (std::strcmp(fmt, "+s") == 0) {
      if (!arr->children || arr->n_children < sch->n_children) {
        if (err_out) *err_out = "Invalid ArrowArray struct: missing child arrays.";
        return false;
      }
      for (int64_t j = 0; j < sch->n_children; j++) {
        if (!visit_field(sch->children[j], arr->children[j], start, len, err_out)) {
          return false;
        }
      }
      return true;
    }

    if (std::strcmp(fmt, "+l") == 0) {
      if (!arr->children || arr->n_children < 1) {
        if (err_out) *err_out = "Invalid ArrowArray list: missing child array.";
        return false;
      }
      const int32_t* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                              ? static_cast<const int32_t*>(arr->buffers[1])
                              : nullptr;
      if (len <= 0 || offs == nullptr) {
        int32_t zero = 0;
        if (!append_buffer(reinterpret_cast<const uint8_t*>(&zero), 4, err_out)) return false;
        return visit_field(sch->children[0], arr->children[0], 0, 0, err_out);
      }
      int32_t base_off = offs[start];
      int32_t end_off = offs[start + len];
      if (base_off == 0) {
        if (!append_buffer(reinterpret_cast<const uint8_t*>(offs + start),
                           (len + 1) * 4,
                           err_out)) {
          return false;
        }
      } else {
        offs32_scratch_.resize(static_cast<size_t>(len + 1));
        for (int64_t i = 0; i <= len; i++) {
          offs32_scratch_[static_cast<size_t>(i)] = offs[start + i] - base_off;
        }
        if (!append_buffer(reinterpret_cast<const uint8_t*>(offs32_scratch_.data()),
                           (len + 1) * 4,
                           err_out)) {
          return false;
        }
      }
      return visit_field(sch->children[0], arr->children[0], base_off, end_off - base_off, err_out);
    }

    if (std::strcmp(fmt, "+L") == 0) {
      if (!arr->children || arr->n_children < 1) {
        if (err_out) *err_out = "Invalid ArrowArray large_list: missing child array.";
        return false;
      }
      const int64_t* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                              ? static_cast<const int64_t*>(arr->buffers[1])
                              : nullptr;
      if (len <= 0 || offs == nullptr) {
        int64_t zero = 0;
        if (!append_buffer(reinterpret_cast<const uint8_t*>(&zero), 8, err_out)) return false;
        return visit_field(sch->children[0], arr->children[0], 0, 0, err_out);
      }
      int64_t base_off = offs[start];
      int64_t end_off = offs[start + len];
      if (base_off == 0) {
        if (!append_buffer(reinterpret_cast<const uint8_t*>(offs + start),
                           (len + 1) * 8,
                           err_out)) {
          return false;
        }
      } else {
        offs64_scratch_.resize(static_cast<size_t>(len + 1));
        for (int64_t i = 0; i <= len; i++) {
          offs64_scratch_[static_cast<size_t>(i)] = offs[start + i] - base_off;
        }
        if (!append_buffer(reinterpret_cast<const uint8_t*>(offs64_scratch_.data()),
                           (len + 1) * 8,
                           err_out)) {
          return false;
        }
      }
      return visit_field(sch->children[0], arr->children[0], base_off, end_off - base_off, err_out);
    }

    if (std::strncmp(fmt, "+w:", 3) == 0) {
      if (!arr->children || arr->n_children < 1) {
        if (err_out) *err_out = "Invalid ArrowArray fixed_size_list: missing child array.";
        return false;
      }
      int64_t w = std::max<int64_t>(0, std::atoll(fmt + 3));
      return visit_field(sch->children[0], arr->children[0], start * w, len * w, err_out);
    }

    if (std::strcmp(fmt, "b") == 0) {
      const uint8_t* bits = (arr->buffers != nullptr && arr->n_buffers > 1)
                              ? static_cast<const uint8_t*>(arr->buffers[1])
                              : nullptr;
      if (len <= 0 || bits == nullptr) {
        return append_buffer(nullptr, 0, err_out);
      }
      const uint8_t* bslice = slice_bitmap(bits, start, len, &bitmap_scratch_);
      return append_buffer(bslice, (len + 7) >> 3, err_out);
    }

    if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0) {
      const int32_t* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                              ? static_cast<const int32_t*>(arr->buffers[1])
                              : nullptr;
      const uint8_t* data = (arr->buffers != nullptr && arr->n_buffers > 2)
                              ? static_cast<const uint8_t*>(arr->buffers[2])
                              : nullptr;
      if (len <= 0 || offs == nullptr) {
        int32_t zero = 0;
        if (!append_buffer(reinterpret_cast<const uint8_t*>(&zero), 4, err_out)) return false;
        return append_buffer(nullptr, 0, err_out);
      }
      int32_t byte_start = offs[start];
      int32_t byte_end = offs[start + len];
      if (byte_start == 0) {
        if (!append_buffer(reinterpret_cast<const uint8_t*>(offs + start),
                           (len + 1) * 4,
                           err_out)) {
          return false;
        }
      } else {
        offs32_scratch_.resize(static_cast<size_t>(len + 1));
        for (int64_t i = 0; i <= len; i++) {
          offs32_scratch_[static_cast<size_t>(i)] = offs[start + i] - byte_start;
        }
        if (!append_buffer(reinterpret_cast<const uint8_t*>(offs32_scratch_.data()),
                           (len + 1) * 4,
                           err_out)) {
          return false;
        }
      }
      int64_t data_len = static_cast<int64_t>(byte_end) - static_cast<int64_t>(byte_start);
      return append_buffer(data ? (data + byte_start) : nullptr, data_len, err_out);
    }

    if (std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
      const int64_t* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                              ? static_cast<const int64_t*>(arr->buffers[1])
                              : nullptr;
      const uint8_t* data = (arr->buffers != nullptr && arr->n_buffers > 2)
                              ? static_cast<const uint8_t*>(arr->buffers[2])
                              : nullptr;
      if (len <= 0 || offs == nullptr) {
        int64_t zero = 0;
        if (!append_buffer(reinterpret_cast<const uint8_t*>(&zero), 8, err_out)) return false;
        return append_buffer(nullptr, 0, err_out);
      }
      int64_t byte_start = offs[start];
      int64_t byte_end = offs[start + len];
      if (byte_start == 0) {
        if (!append_buffer(reinterpret_cast<const uint8_t*>(offs + start),
                           (len + 1) * 8,
                           err_out)) {
          return false;
        }
      } else {
        offs64_scratch_.resize(static_cast<size_t>(len + 1));
        for (int64_t i = 0; i <= len; i++) {
          offs64_scratch_[static_cast<size_t>(i)] = offs[start + i] - byte_start;
        }
        if (!append_buffer(reinterpret_cast<const uint8_t*>(offs64_scratch_.data()),
                           (len + 1) * 8,
                           err_out)) {
          return false;
        }
      }
      int64_t data_len = byte_end - byte_start;
      return append_buffer(data ? (data + byte_start) : nullptr, data_len, err_out);
    }

    int fw = fixed_width_bytes(fmt);
    if (fw >= 0) {
      const uint8_t* data = (arr->buffers != nullptr && arr->n_buffers > 1)
                              ? static_cast<const uint8_t*>(arr->buffers[1])
                              : nullptr;
      int64_t data_len = len * fw;
      return append_buffer((data && data_len > 0) ? (data + start * fw) : nullptr,
                           data_len,
                           err_out);
    }

    if (err_out) *err_out = std::string("Unsupported Arrow format in fast IPC encoder: ") + fmt;
    return false;
  }
};

struct BatchSliceSpec {
  size_t array_idx;
  int64_t row_start;
  int64_t row_len;
};

inline bool encode_batches_parallel(const ArrowSchema* schema,
                                    const std::vector<ArrowArray>& src_arrays,
                                    const std::vector<BatchSliceSpec>& specs,
                                    bool use_zstd,
                                    int n_threads,
                                    std::vector<std::string>* out_batches,
                                    std::string* err_out) {
  size_t m = specs.size();
  out_batches->resize(m);
  if (m == 0) return true;

  if (n_threads <= 0) {
    unsigned int hw = std::thread::hardware_concurrency();
    n_threads = hw > 0 ? static_cast<int>(std::min(hw, 16u)) : 4;
  }
  int n_workers = static_cast<int>(std::min<size_t>(static_cast<size_t>(std::max(1, n_threads)), m));

  std::atomic<size_t> next_idx{0};
  std::atomic<bool> failed{false};
  std::vector<std::string> worker_errs(static_cast<size_t>(n_workers));

  auto worker_fn = [&](int w) {
    RecordBatchEncoder enc(use_zstd, 1);
    while (!failed.load(std::memory_order_relaxed)) {
      size_t idx = next_idx.fetch_add(1);
      if (idx >= m) break;
      const BatchSliceSpec& sp = specs[idx];
      std::string err;
      std::string encoded = enc.encode(
        schema, &src_arrays[sp.array_idx], sp.row_start, sp.row_len, &err);
      if (!err.empty()) {
        worker_errs[static_cast<size_t>(w)] = err;
        failed.store(true);
        break;
      }
      (*out_batches)[idx] = std::move(encoded);
    }
  };

  if (n_workers <= 1) {
    worker_fn(0);
  } else {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(n_workers - 1));
    for (int w = 1; w < n_workers; w++) {
      threads.emplace_back(worker_fn, w);
    }
    worker_fn(0);
    for (auto& th : threads) {
      if (th.joinable()) th.join();
    }
  }

  for (const auto& e : worker_errs) {
    if (!e.empty()) {
      if (err_out) *err_out = e;
      return false;
    }
  }
  return true;
}

// -- In-flight Arrow IPC RecordBatch Decoder & Zero-Copy View Builder ---------

struct DecodedBatch {
  int64_t length = 0;
  std::vector<FieldNodeDesc> nodes;
  std::vector<BufferDesc> buffers;
  std::string raw_ipc;                        // Kept zero-copy when uncompressed & 8-byte aligned
  size_t raw_body_offset = 0;
  std::unique_ptr<uint8_t[]> decompressed;    // Allocated once (8-byte aligned) when decompressed/repacked
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
  ZstdDecompressContext() {
#ifdef HAVE_BQS_ZSTD
    dctx_ = ZSTD_createDCtx();
#endif
  }
  ~ZstdDecompressContext() {
#ifdef HAVE_BQS_ZSTD
    if (dctx_) ZSTD_freeDCtx(dctx_);
#endif
  }
  ZstdDecompressContext(const ZstdDecompressContext&) = delete;
  ZstdDecompressContext& operator=(const ZstdDecompressContext&) = delete;

#ifdef HAVE_BQS_ZSTD
  ZSTD_DCtx* get() const { return dctx_; }
#else
  void* get() const { return nullptr; }
#endif

private:
#ifdef HAVE_BQS_ZSTD
  ZSTD_DCtx* dctx_ = nullptr;
#endif
};

inline uint16_t fb_read_u16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}

inline int32_t fb_read_i32(const uint8_t* p) {
  int32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

inline uint32_t fb_read_u32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

inline int64_t fb_read_i64(const uint8_t* p) {
  int64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

inline bool fb_open_table(const uint8_t* fb,
                          size_t meta_size,
                          uint32_t table_off,
                          uint32_t* vt_off_out,
                          uint16_t* vt_len_out) {
  if (static_cast<uint64_t>(table_off) + 4u > meta_size) return false;
  int32_t soff = fb_read_i32(fb + table_off);
  int64_t voff = static_cast<int64_t>(table_off) - static_cast<int64_t>(soff);
  if (voff < 0 || static_cast<uint64_t>(voff) + 4u > meta_size) return false;
  uint16_t vlen = fb_read_u16(fb + voff);
  if (vlen < 4u || static_cast<uint64_t>(voff) + vlen > meta_size) return false;
  *vt_off_out = static_cast<uint32_t>(voff);
  *vt_len_out = vlen;
  return true;
}

inline bool fb_field_checked(const uint8_t* fb,
                             size_t meta_size,
                             uint32_t table_off,
                             uint32_t vt_off,
                             uint16_t vt_len,
                             uint16_t slot_idx,
                             size_t field_bytes,
                             uint32_t* pos_out) {
  *pos_out = 0u;
  uint32_t entry = 4u + static_cast<uint32_t>(slot_idx) * 2u;
  if (entry + 2u > vt_len) return true; // Field absent -> default
  uint16_t foff = fb_read_u16(fb + vt_off + entry);
  if (foff == 0u) return true; // Field absent -> default
  uint64_t fpos = static_cast<uint64_t>(table_off) + static_cast<uint64_t>(foff);
  if (fpos + field_bytes > meta_size) return false;
  *pos_out = static_cast<uint32_t>(fpos);
  return true;
}

// Parse an Arrow IPC RecordBatch message (`raw_msg`, transferred in by move)
// with full FlatBuffer & body bounds validation, and decompress ZSTD buffers in-flight.
inline bool decode_ipc_record_batch(std::string&& raw_msg,
                                    DecodedBatch* out,
                                    ZstdDecompressContext* zctx,
                                    std::string* err_out) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(raw_msg.data());
  size_t len = raw_msg.size();
  if (len < 8) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: message shorter than 8 bytes.";
    return false;
  }

  uint32_t cont = fb_read_u32(data);
  int32_t meta_len = 0;
  size_t fb_start = 0;
  if (cont == 0xFFFFFFFFu) {
    meta_len = fb_read_i32(data + 4);
    fb_start = 8;
  } else {
    meta_len = static_cast<int32_t>(cont);
    fb_start = 4;
  }
  if (meta_len < 4 || fb_start + static_cast<size_t>(meta_len) > len) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: truncated FlatBuffer metadata.";
    return false;
  }

  const size_t meta_size = static_cast<size_t>(meta_len);
  const uint8_t* fb = data + fb_start;
  uint32_t root_off = fb_read_u32(fb);
  uint32_t msg_vt_off = 0;
  uint16_t msg_vt_len = 0;
  if (!fb_open_table(fb, meta_size, root_off, &msg_vt_off, &msg_vt_len)) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds Message table.";
    return false;
  }

  uint32_t ht_pos = 0, hdr_pos = 0, bl_pos = 0;
  if (!fb_field_checked(fb, meta_size, root_off, msg_vt_off, msg_vt_len, 1, 1, &ht_pos) ||
      !fb_field_checked(fb, meta_size, root_off, msg_vt_off, msg_vt_len, 2, 4, &hdr_pos) ||
      !fb_field_checked(fb, meta_size, root_off, msg_vt_off, msg_vt_len, 3, 8, &bl_pos)) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds Message fields.";
    return false;
  }

  uint8_t header_type = ht_pos ? fb[ht_pos] : 0;
  if (header_type != 3) {
    if (err_out) *err_out = "Unexpected Arrow IPC header_type (expected RecordBatch = 3).";
    return false;
  }
  if (!hdr_pos) {
    if (err_out) *err_out = "Missing RecordBatch header in Arrow IPC Message.";
    return false;
  }

  uint64_t rb_off64 = static_cast<uint64_t>(hdr_pos) + fb_read_u32(fb + hdr_pos);
  if (rb_off64 > meta_size) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds RecordBatch offset.";
    return false;
  }
  uint32_t rb_off = static_cast<uint32_t>(rb_off64);

  int64_t body_len = bl_pos ? fb_read_i64(fb + bl_pos) : 0;
  size_t body_offset_in_msg = fb_start + meta_size;
  if (body_len < 0 || static_cast<uint64_t>(body_len) > static_cast<uint64_t>(len - body_offset_in_msg)) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: truncated body.";
    return false;
  }
  const uint64_t ubody_len = static_cast<uint64_t>(body_len);

  uint32_t rb_vt_off = 0;
  uint16_t rb_vt_len = 0;
  if (!fb_open_table(fb, meta_size, rb_off, &rb_vt_off, &rb_vt_len)) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds RecordBatch vtable.";
    return false;
  }

  uint32_t len_pos = 0, nodes_pos = 0, bufs_pos = 0, comp_pos = 0;
  if (!fb_field_checked(fb, meta_size, rb_off, rb_vt_off, rb_vt_len, 0, 8, &len_pos) ||
      !fb_field_checked(fb, meta_size, rb_off, rb_vt_off, rb_vt_len, 1, 4, &nodes_pos) ||
      !fb_field_checked(fb, meta_size, rb_off, rb_vt_off, rb_vt_len, 2, 4, &bufs_pos) ||
      !fb_field_checked(fb, meta_size, rb_off, rb_vt_off, rb_vt_len, 3, 4, &comp_pos)) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds RecordBatch fields.";
    return false;
  }

  out->length = len_pos ? fb_read_i64(fb + len_pos) : 0;
  if (out->length < 0) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: negative row length.";
    return false;
  }

  // FieldNodes vector (slot 1)
  if (nodes_pos) {
    uint64_t vec = static_cast<uint64_t>(nodes_pos) + fb_read_u32(fb + nodes_pos);
    if (vec + 4u > meta_size) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds nodes vector.";
      return false;
    }
    uint32_t n_nodes = fb_read_u32(fb + vec);
    if (vec + 4u + static_cast<uint64_t>(n_nodes) * 16u > meta_size) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: truncated nodes vector.";
      return false;
    }
    out->nodes.resize(n_nodes);
    const uint8_t* np = fb + vec + 4;
    for (uint32_t i = 0; i < n_nodes; i++) {
      int64_t nl = fb_read_i64(np + i * 16u);
      int64_t nc = fb_read_i64(np + i * 16u + 8u);
      if (nl < 0 || nc < 0) {
        if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: negative FieldNode count.";
        return false;
      }
      out->nodes[i] = {nl, nc};
    }
  } else {
    out->nodes.clear();
  }

  // Buffers vector (slot 2)
  std::vector<BufferDesc> raw_bufs;
  if (bufs_pos) {
    uint64_t vec = static_cast<uint64_t>(bufs_pos) + fb_read_u32(fb + bufs_pos);
    if (vec + 4u > meta_size) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds buffers vector.";
      return false;
    }
    uint32_t n_bufs = fb_read_u32(fb + vec);
    if (vec + 4u + static_cast<uint64_t>(n_bufs) * 16u > meta_size) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: truncated buffers vector.";
      return false;
    }
    raw_bufs.resize(n_bufs);
    const uint8_t* bp = fb + vec + 4;
    for (uint32_t i = 0; i < n_bufs; i++) {
      int64_t bo = fb_read_i64(bp + i * 16u);
      int64_t bl = fb_read_i64(bp + i * 16u + 8u);
      if (bl < 0 || (bl > 0 && (bo < 0 || static_cast<uint64_t>(bo) + static_cast<uint64_t>(bl) > ubody_len))) {
        if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: buffer range exceeds message body.";
        return false;
      }
      raw_bufs[i] = {bo, bl};
    }
  }

  // Optional BodyCompression table (slot 3)
  bool is_compressed = false;
  int8_t comp_codec = 0;
  int8_t comp_method = 0;
  if (comp_pos && fb_read_u32(fb + comp_pos) != 0) {
    is_compressed = true;
    uint64_t comp_off64 = static_cast<uint64_t>(comp_pos) + fb_read_u32(fb + comp_pos);
    uint32_t comp_vt_off = 0;
    uint16_t comp_vt_len = 0;
    if (comp_off64 > meta_size ||
        !fb_open_table(fb, meta_size, static_cast<uint32_t>(comp_off64), &comp_vt_off, &comp_vt_len)) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds BodyCompression table.";
      return false;
    }
    uint32_t codec_pos = 0, method_pos = 0;
    if (!fb_field_checked(fb, meta_size, static_cast<uint32_t>(comp_off64), comp_vt_off, comp_vt_len, 0, 1, &codec_pos) ||
        !fb_field_checked(fb, meta_size, static_cast<uint32_t>(comp_off64), comp_vt_off, comp_vt_len, 1, 1, &method_pos)) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds BodyCompression fields.";
      return false;
    }
    comp_codec = codec_pos ? static_cast<int8_t>(fb[codec_pos]) : 0;
    comp_method = method_pos ? static_cast<int8_t>(fb[method_pos]) : 0;
  }

  if (!is_compressed) {
    static const uint64_t MAX_UNCOMPRESSED_BATCH_BYTES = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    bool offsets_aligned = true;
    for (const auto& bd : raw_bufs) {
      if (bd.length > 0 && (static_cast<uint64_t>(bd.offset) & 7ULL) != 0ULL) {
        offsets_aligned = false;
        break;
      }
    }
    if (offsets_aligned) {
      out->buffers = std::move(raw_bufs);
      out->raw_ipc = std::move(raw_msg);
      out->raw_body_offset = body_offset_in_msg;
      out->decompressed.reset();
      out->body_size = static_cast<size_t>(body_len);
      out->refresh_body_ptr();
      if (out->body_size == 0 || (reinterpret_cast<uintptr_t>(out->body_ptr) & 7u) == 0u) {
        return true;
      }
      // Recover raw_msg if std::string buffer was not 8-byte aligned (e.g. legacy 4-byte framing)
      raw_bufs = std::move(out->buffers);
      raw_msg = std::move(out->raw_ipc);
      data = reinterpret_cast<const uint8_t*>(raw_msg.data());
    }

    const uint8_t* src_body = data + body_offset_in_msg;
    uint64_t total_aligned64 = 0;
    for (const auto& bd : raw_bufs) {
      if (bd.length > 0) {
        total_aligned64 += (static_cast<uint64_t>(bd.length) + 7ULL) & ~7ULL;
        if (total_aligned64 > MAX_UNCOMPRESSED_BATCH_BYTES) {
          if (err_out) *err_out = "Arrow RecordBatch exceeds 2 GiB safety limit.";
          return false;
        }
      }
    }
    size_t total_aligned = static_cast<size_t>(total_aligned64);
    out->decompressed.reset(total_aligned > 0 ? new uint8_t[total_aligned] : nullptr);
    uint8_t* dst_base = out->decompressed.get();
    size_t dst_pos = 0;
    out->buffers.resize(raw_bufs.size());
    for (size_t i = 0; i < raw_bufs.size(); i++) {
      int64_t blen = raw_bufs[i].length;
      int64_t boff = raw_bufs[i].offset;
      if (blen <= 0) {
        out->buffers[i] = {static_cast<int64_t>(dst_pos), 0};
        continue;
      }
      size_t slen = static_cast<size_t>(blen);
      std::memcpy(dst_base + dst_pos, src_body + boff, slen);
      out->buffers[i] = {static_cast<int64_t>(dst_pos), blen};
      size_t padded = (slen + 7u) & ~static_cast<size_t>(7u);
      if (padded > slen) std::memset(dst_base + dst_pos + slen, 0, padded - slen);
      dst_pos += padded;
    }
    std::string().swap(raw_msg);
    out->raw_body_offset = 0;
    out->body_ptr = dst_base;
    out->body_size = total_aligned;
    return true;
  }

#ifdef HAVE_BQS_ZSTD
  if (comp_codec == 1 && comp_method == 0) {
    const uint8_t* src_body = data + body_offset_in_msg;
    size_t n_bufs = raw_bufs.size();
    out->buffers.resize(n_bufs);

    // Pass 1: compute exact decompressed byte total from 8-byte prefixes
    static const uint64_t MAX_DECOMPRESSED_BATCH_BYTES = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    uint64_t total_out_bytes64 = 0;
    for (size_t i = 0; i < n_bufs; i++) {
      int64_t blen = raw_bufs[i].length;
      int64_t boff = raw_bufs[i].offset;
      if (blen <= 0) continue;
      if (blen < 8) {
        if (err_out) *err_out = "Invalid compressed Arrow buffer (< 8 bytes).";
        return false;
      }
      int64_t ulen = fb_read_i64(src_body + boff);
      if (ulen < -1 || static_cast<uint64_t>(ulen > 0 ? ulen : 0) > MAX_DECOMPRESSED_BATCH_BYTES) {
        if (err_out) *err_out = "Invalid compressed Arrow buffer uncompressed length.";
        return false;
      }
      uint64_t actual = (ulen == -1) ? static_cast<uint64_t>(blen - 8)
                                     : static_cast<uint64_t>(std::max<int64_t>(0, ulen));
      total_out_bytes64 += (actual + 7ULL) & ~7ULL;
      if (total_out_bytes64 > MAX_DECOMPRESSED_BATCH_BYTES) {
        if (err_out) *err_out = "Decompressed Arrow RecordBatch exceeds 2 GiB safety limit.";
        return false;
      }
    }
    size_t total_out_bytes = static_cast<size_t>(total_out_bytes64);

    // Single uninitialized allocation (no redundant zero-memset before ZSTD overwrite)
    out->decompressed.reset(total_out_bytes > 0 ? new uint8_t[total_out_bytes] : nullptr);
    uint8_t* dst_base = out->decompressed.get();
    size_t dst_pos = 0;

    // Pass 2: decompress all buffers directly into dst_base
    for (size_t i = 0; i < n_bufs; i++) {
      int64_t blen = raw_bufs[i].length;
      int64_t boff = raw_bufs[i].offset;
      if (blen <= 0) {
        out->buffers[i] = {static_cast<int64_t>(dst_pos), 0};
        continue;
      }
      int64_t ulen = fb_read_i64(src_body + boff);
      const uint8_t* csrc = src_body + boff + 8;
      size_t clen = static_cast<size_t>(blen - 8);
      if (ulen == -1) {
        if (clen > 0) std::memcpy(dst_base + dst_pos, csrc, clen);
        out->buffers[i] = {static_cast<int64_t>(dst_pos), static_cast<int64_t>(clen)};
        size_t padded = (clen + 7u) & ~static_cast<size_t>(7u);
        if (padded > clen) std::memset(dst_base + dst_pos + clen, 0, padded - clen);
        dst_pos += padded;
      } else if (ulen <= 0) {
        out->buffers[i] = {static_cast<int64_t>(dst_pos), 0};
      } else {
        size_t target_len = static_cast<size_t>(ulen);
        size_t res = (zctx && zctx->get())
                       ? ZSTD_decompressDCtx(zctx->get(), dst_base + dst_pos, target_len, csrc, clen)
                       : ZSTD_decompress(dst_base + dst_pos, target_len, csrc, clen);
        if (ZSTD_isError(res) || res != target_len) {
          if (err_out) *err_out = std::string("ZSTD_decompress failed: ") + ZSTD_getErrorName(res);
          return false;
        }
        out->buffers[i] = {static_cast<int64_t>(dst_pos), ulen};
        size_t padded = (target_len + 7u) & ~static_cast<size_t>(7u);
        if (padded > target_len) std::memset(dst_base + dst_pos + target_len, 0, padded - target_len);
        dst_pos += padded;
      }
    }

    std::string().swap(raw_msg); // Free compressed bytes immediately
    out->body_ptr = dst_base;
    out->body_size = total_out_bytes;
    return true;
  }
#else
  (void)zctx;
  (void)comp_codec;
  (void)comp_method;
#endif

  if (err_out) *err_out = "Unsupported Arrow IPC BodyCompression codec/method.";
  return false;
}

inline void count_schema_nodes_and_buffers(const ArrowSchema* sch,
                                           size_t* n_nodes,
                                           size_t* n_bufs) {
  const char* fmt = (sch && sch->format) ? sch->format : "";
  (*n_nodes) += 1;
  if (std::strcmp(fmt, "n") == 0) {
    // 0 buffers
  } else if (std::strcmp(fmt, "+s") == 0 || std::strncmp(fmt, "+w:", 3) == 0) {
    (*n_bufs) += 1;
  } else if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0 ||
             std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    (*n_bufs) += 3;
  } else {
    (*n_bufs) += 2;
  }
  for (int64_t c = 0; c < sch->n_children; c++) {
    count_schema_nodes_and_buffers(sch->children[c], n_nodes, n_bufs);
  }
}

// Validate node lengths, validity bitmaps, monotonic list/string/binary offsets,
// and leaf data buffer capacities recursively before building borrowed ArrowArray views.
inline bool validate_borrowed_field_buffers(const ArrowSchema* sch,
                                            const DecodedBatch& batch,
                                            int64_t min_len,
                                            bool exact_len,
                                            size_t* node_pos,
                                            size_t* buf_pos,
                                            std::string* err_out) {
  if (!sch || !sch->format || sch->dictionary != nullptr) {
    if (err_out) *err_out = "Unsupported Arrow schema in direct RecordBatch decoder.";
    return false;
  }
  if (*node_pos >= batch.nodes.size()) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: missing FieldNode.";
    return false;
  }
  const char* fmt = sch->format;
  const FieldNodeDesc& nd = batch.nodes[(*node_pos)++];
  if (nd.length < min_len || (exact_len && nd.length != min_len) || nd.null_count > nd.length) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: inconsistent FieldNode length/null_count.";
    return false;
  }

  if (std::strcmp(fmt, "n") == 0) {
    return true;
  }

  if (*buf_pos >= batch.buffers.size()) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: missing validity buffer.";
    return false;
  }
  const BufferDesc& vbuf = batch.buffers[(*buf_pos)++];
  if (nd.null_count > 0 && nd.length > 0 &&
      (vbuf.length < ((nd.length + 7) >> 3) || !batch.body_ptr)) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: validity bitmap shorter than row count.";
    return false;
  }

  if (std::strcmp(fmt, "+s") == 0) {
    for (int64_t c = 0; c < sch->n_children; c++) {
      if (!validate_borrowed_field_buffers(
            sch->children[c], batch, nd.length, true, node_pos, buf_pos, err_out)) {
        return false;
      }
    }
    return true;
  }

  if (std::strncmp(fmt, "+w:", 3) == 0) {
    int64_t w = std::atoll(fmt + 3);
    if (w < 0 || sch->n_children != 1) {
      if (err_out) *err_out = "Invalid Arrow FixedSizeList schema.";
      return false;
    }
    return validate_borrowed_field_buffers(
      sch->children[0], batch, nd.length * w, true, node_pos, buf_pos, err_out);
  }

  if (std::strcmp(fmt, "+l") == 0 || std::strcmp(fmt, "+L") == 0) {
    if (sch->n_children != 1 || *buf_pos >= batch.buffers.size()) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: missing list offset buffer.";
      return false;
    }
    const BufferDesc& obuf = batch.buffers[(*buf_pos)++];
    bool is_large = (std::strcmp(fmt, "+L") == 0);
    int64_t elem_sz = is_large ? 8 : 4;
    int64_t child_min_len = 0;
    if (nd.length > 0) {
      if (obuf.length < (nd.length + 1) * elem_sz || !batch.body_ptr) {
        if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: list offset buffer shorter than row count.";
        return false;
      }
      const uint8_t* op = batch.body_ptr + obuf.offset;
      if (!is_large) {
        int32_t prev = load_unaligned<int32_t>(op);
        if (prev < 0) {
          if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: negative list offset.";
          return false;
        }
        for (int64_t i = 1; i <= nd.length; i++) {
          int32_t cur = load_unaligned<int32_t>(op + i * 4);
          if (cur < prev) {
            if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: non-monotonic list offsets.";
            return false;
          }
          prev = cur;
        }
        child_min_len = prev;
      } else {
        int64_t prev = load_unaligned<int64_t>(op);
        if (prev < 0) {
          if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: negative list offset.";
          return false;
        }
        for (int64_t i = 1; i <= nd.length; i++) {
          int64_t cur = load_unaligned<int64_t>(op + i * 8);
          if (cur < prev) {
            if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: non-monotonic list offsets.";
            return false;
          }
          prev = cur;
        }
        child_min_len = prev;
      }
    }
    return validate_borrowed_field_buffers(
      sch->children[0], batch, child_min_len, false, node_pos, buf_pos, err_out);
  }

  if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0 ||
      std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    if (*buf_pos + 1 >= batch.buffers.size()) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: missing binary/string buffers.";
      return false;
    }
    const BufferDesc& obuf = batch.buffers[(*buf_pos)++];
    const BufferDesc& dbuf = batch.buffers[(*buf_pos)++];
    bool is_large = (fmt[0] == 'U' || fmt[0] == 'Z');
    int64_t elem_sz = is_large ? 8 : 4;
    if (nd.length > 0) {
      if (obuf.length < (nd.length + 1) * elem_sz || !batch.body_ptr) {
        if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: offset buffer shorter than row count.";
        return false;
      }
      const uint8_t* op = batch.body_ptr + obuf.offset;
      int64_t last_off = 0;
      if (!is_large) {
        int32_t prev = load_unaligned<int32_t>(op);
        if (prev < 0) {
          if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: negative string/binary offset.";
          return false;
        }
        for (int64_t i = 1; i <= nd.length; i++) {
          int32_t cur = load_unaligned<int32_t>(op + i * 4);
          if (cur < prev) {
            if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: non-monotonic string/binary offsets.";
            return false;
          }
          prev = cur;
        }
        last_off = prev;
      } else {
        int64_t prev = load_unaligned<int64_t>(op);
        if (prev < 0) {
          if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: negative string/binary offset.";
          return false;
        }
        for (int64_t i = 1; i <= nd.length; i++) {
          int64_t cur = load_unaligned<int64_t>(op + i * 8);
          if (cur < prev) {
            if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: non-monotonic string/binary offsets.";
            return false;
          }
          prev = cur;
        }
        last_off = prev;
      }
      if (last_off > dbuf.length || (last_off > 0 && !batch.body_ptr)) {
        if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: offsets exceed data buffer bounds.";
        return false;
      }
    }
    return true;
  }

  if (*buf_pos >= batch.buffers.size()) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: missing data buffer.";
    return false;
  }
  const BufferDesc& dbuf = batch.buffers[(*buf_pos)++];
  if (std::strcmp(fmt, "b") == 0) {
    int64_t req = (nd.length + 7) >> 3;
    if (req > 0 && (dbuf.length < req || !batch.body_ptr)) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: boolean buffer shorter than row count.";
      return false;
    }
    return true;
  }

  int fw = fixed_width_bytes(fmt);
  if (fw < 0) {
    if (err_out) *err_out = std::string("Unsupported Arrow format in direct RecordBatch decoder: ") + fmt;
    return false;
  }
  int64_t req = nd.length * fw;
  if (req > 0 && (dbuf.length < req || !batch.body_ptr)) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: data buffer shorter than row count.";
    return false;
  }
  return true;
}

struct BorrowedArrayPriv {
  const void* buffers[3] = {nullptr, nullptr, nullptr};
};

inline void release_borrowed_array(struct ArrowArray* a) {
  if (!a || !a->release) return;
  if (a->children) {
    for (int64_t i = 0; i < a->n_children; i++) {
      if (a->children[i]) {
        if (a->children[i]->release) a->children[i]->release(a->children[i]);
        free(a->children[i]);
      }
    }
    free(a->children);
  }
  delete static_cast<BorrowedArrayPriv*>(a->private_data);
  a->release = nullptr;
}

// Construct a zero-copy ArrowArray view for a single field subtree from a DecodedBatch
inline ArrowArray* build_borrowed_field_array(const ArrowSchema* sch,
                                              const DecodedBatch& batch,
                                              size_t* node_pos,
                                              size_t* buf_pos) {
  ArrowArray* arr = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
  BorrowedArrayPriv* priv = new BorrowedArrayPriv();
  const char* fmt = (sch && sch->format) ? sch->format : "";

  const FieldNodeDesc& nd = batch.nodes[(*node_pos)++];
  int n_bufs = 0;
  if (std::strcmp(fmt, "n") == 0) {
    n_bufs = 0;
  } else if (std::strcmp(fmt, "+s") == 0 || std::strncmp(fmt, "+w:", 3) == 0) {
    n_bufs = 1;
  } else if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0 ||
             std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    n_bufs = 3;
  } else {
    n_bufs = 2;
  }

  for (int b = 0; b < n_bufs; b++) {
    const BufferDesc& bd = batch.buffers[(*buf_pos)++];
    if (b == 0 && (bd.length == 0 || nd.null_count == 0)) {
      priv->buffers[b] = nullptr;
    } else {
      priv->buffers[b] = (bd.length > 0 && batch.body_ptr)
                           ? (batch.body_ptr + bd.offset)
                           : static_cast<const void*>("");
    }
  }

  arr->length = nd.length;
  arr->null_count = nd.null_count;
  arr->offset = 0;
  arr->n_buffers = n_bufs;
  arr->n_children = sch->n_children;
  arr->buffers = priv->buffers;
  arr->dictionary = nullptr;
  arr->private_data = priv;
  arr->release = release_borrowed_array;

  if (sch->n_children > 0) {
    arr->children = static_cast<ArrowArray**>(malloc(static_cast<size_t>(sch->n_children) * sizeof(ArrowArray*)));
    for (int64_t c = 0; c < sch->n_children; c++) {
      arr->children[c] = build_borrowed_field_array(sch->children[c], batch, node_pos, buf_pos);
    }
  } else {
    arr->children = nullptr;
  }
  return arr;
}

// Safely truncate a borrowed ArrowArray view to `new_len` logical rows and
// recompute exact sliced `null_count` (including nested STRUCT / FixedSizeList children).
inline void truncate_borrowed_field_array(ArrowArray* arr,
                                          const ArrowSchema* sch,
                                          int64_t new_len) {
  if (!arr || !sch || new_len >= arr->length) return;
  const char* fmt = sch->format ? sch->format : "";
  arr->length = new_len;
  if (std::strcmp(fmt, "n") == 0) {
    arr->null_count = new_len;
    return;
  }
  if (arr->null_count != 0 && arr->buffers != nullptr && arr->buffers[0] != nullptr) {
    arr->null_count = count_nulls_in_bitmap(
      static_cast<const uint8_t*>(arr->buffers[0]), arr->offset, new_len);
  } else {
    arr->null_count = 0;
  }
  if (std::strcmp(fmt, "+s") == 0) {
    for (int64_t c = 0; c < sch->n_children && c < arr->n_children; c++) {
      truncate_borrowed_field_array(arr->children[c], sch->children[c], new_len);
    }
  } else if (std::strncmp(fmt, "+w:", 3) == 0 && sch->n_children == 1 && arr->n_children == 1) {
    int64_t w = std::atoll(fmt + 3);
    truncate_borrowed_field_array(arr->children[0], sch->children[0], new_len * std::max<int64_t>(0, w));
  }
}

// Parse an Arrow IPC stream buffer (supporting both modern 8-byte 0xFFFFFFFF
// continuation framing and legacy 4-byte V4 framing) into canonical 8-byte
// framed Schema and RecordBatch messages, per-batch row counts, and total rows.
inline bool split_arrow_ipc_stream(const uint8_t* data,
                                   size_t len,
                                   std::string* schema_out,
                                   std::vector<std::string>* batches_out,
                                   std::vector<int64_t>* batch_rows_out,
                                   int64_t* total_rows_out,
                                   std::string* err_out) {
  int64_t total_rows = 0;
  size_t pos = 0;
  while (pos < len) {
    if (pos + 4 > len) {
      if (err_out) *err_out = "Invalid Arrow IPC stream: truncated message prefix.";
      return false;
    }
    uint32_t continuation = 0;
    int32_t meta_len = 0;
    size_t prefix_len = 0;
    std::memcpy(&continuation, data + pos, 4);
    if (continuation == 0xFFFFFFFFu) {
      if (pos + 8 > len) {
        if (err_out) *err_out = "Invalid Arrow IPC stream: truncated continuation prefix.";
        return false;
      }
      std::memcpy(&meta_len, data + pos + 4, 4);
      if (meta_len == 0) {
        break; // 8-byte EOS marker (0xFFFFFFFF 0x00000000)
      }
      prefix_len = 8;
    } else {
      meta_len = static_cast<int32_t>(continuation);
      if (meta_len == 0) {
        break; // Legacy 4-byte EOS marker (0x00000000)
      }
      prefix_len = 4;
    }
    if (meta_len < 4 || pos + prefix_len + static_cast<size_t>(meta_len) > len) {
      if (err_out) *err_out = "Invalid Arrow IPC stream: truncated metadata.";
      return false;
    }
    const size_t meta_size = static_cast<size_t>(meta_len);
    const uint8_t* fb = data + pos + prefix_len;
    uint32_t root_off = fb_read_u32(fb);
    uint32_t msg_vt_off = 0;
    uint16_t msg_vt_len = 0;
    if (!fb_open_table(fb, meta_size, root_off, &msg_vt_off, &msg_vt_len)) {
      if (err_out) *err_out = "Invalid Arrow IPC stream: out-of-bounds Message table.";
      return false;
    }

    uint32_t ht_pos = 0, hdr_pos = 0, bl_pos = 0;
    if (!fb_field_checked(fb, meta_size, root_off, msg_vt_off, msg_vt_len, 1, 1, &ht_pos) ||
        !fb_field_checked(fb, meta_size, root_off, msg_vt_off, msg_vt_len, 2, 4, &hdr_pos) ||
        !fb_field_checked(fb, meta_size, root_off, msg_vt_off, msg_vt_len, 3, 8, &bl_pos)) {
      if (err_out) *err_out = "Invalid Arrow IPC stream: out-of-bounds Message fields.";
      return false;
    }

    uint8_t header_type = ht_pos ? fb[ht_pos] : 0;
    int64_t body_len = bl_pos ? fb_read_i64(fb + bl_pos) : 0;
    if (body_len < 0 ||
        static_cast<uint64_t>(body_len) > static_cast<uint64_t>(len - (pos + prefix_len + meta_size))) {
      if (err_out) *err_out = "Invalid Arrow IPC stream: truncated message body.";
      return false;
    }
    size_t msg_len = prefix_len + meta_size + static_cast<size_t>(body_len);

    auto pack_canonical_msg = [&]() -> std::string {
      if (prefix_len == 8) {
        return std::string(reinterpret_cast<const char*>(data + pos), msg_len);
      }
      size_t aligned_meta = (meta_size + 7u) & ~static_cast<size_t>(7u);
      std::string out(8u + aligned_meta + static_cast<size_t>(body_len), '\0');
      const uint32_t cont_tag = 0xFFFFFFFFu;
      const int32_t mlen_i32 = static_cast<int32_t>(aligned_meta);
      std::memcpy(&out[0], &cont_tag, 4);
      std::memcpy(&out[4], &mlen_i32, 4);
      std::memcpy(&out[8], fb, meta_size);
      if (body_len > 0) {
        std::memcpy(&out[8u + aligned_meta],
                    fb + meta_size,
                    static_cast<size_t>(body_len));
      }
      return out;
    };

    if (header_type == 1) {
      if (schema_out) *schema_out = pack_canonical_msg();
    } else if (header_type == 3) {
      if (batches_out) batches_out->push_back(pack_canonical_msg());
      int64_t rb_rows = 0;
      if (hdr_pos) {
        uint64_t rb_off64 = static_cast<uint64_t>(hdr_pos) + fb_read_u32(fb + hdr_pos);
        uint32_t rb_vt_off = 0;
        uint16_t rb_vt_len = 0;
        uint32_t len_pos = 0;
        if (rb_off64 > meta_size ||
            !fb_open_table(fb, meta_size, static_cast<uint32_t>(rb_off64), &rb_vt_off, &rb_vt_len) ||
            !fb_field_checked(fb, meta_size, static_cast<uint32_t>(rb_off64), rb_vt_off, rb_vt_len, 0, 8, &len_pos)) {
          if (err_out) *err_out = "Invalid Arrow IPC stream: out-of-bounds RecordBatch header.";
          return false;
        }
        if (len_pos) {
          rb_rows = fb_read_i64(fb + len_pos);
          if (rb_rows < 0) {
            if (err_out) *err_out = "Invalid Arrow IPC stream: negative RecordBatch row count.";
            return false;
          }
          total_rows += rb_rows;
        }
      }
      if (batch_rows_out) batch_rows_out->push_back(rb_rows);
    }
    pos += msg_len;
  }
  if (batches_out && !batches_out->empty() && schema_out && schema_out->empty()) {
    if (err_out) *err_out = "Invalid Arrow IPC stream: missing Schema message.";
    return false;
  }
  if (total_rows_out) *total_rows_out = total_rows;
  return true;
}

} // namespace bqs_ipc

// Implemented in convert.cpp: converts in-flight decoded batches directly to an
// R tibble / data.frame in parallel across C++ threads.
SEXP bqs_convert_decoded_streams(
    SEXP schema_xptr,
    std::vector<std::vector<bqs_ipc::DecodedBatch>>* stream_batches,
    int64_t n_max,
    const std::string& bigint,
    bool as_data_frame,
    int n_threads,
    Rcpp::Function int64_ptype_fn,
    Rcpp::Function check_pkg_fn,
    Rcpp::Function na_convert_stream,
    Rcpp::Function na_convert_stream_quiet,
    Rcpp::Function as_bigint);

#endif // BQS_ARROW_IPC_H
