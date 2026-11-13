#ifndef BQS_ARROW_IPC_H
#define BQS_ARROW_IPC_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
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
  int64_t start = arr->offset + logical_start;
  int64_t total = (arr->null_count != 0 && arr->buffers[0] != nullptr) ? ((len + 7) >> 3) : 0;

  if (std::strcmp(fmt, "n") == 0) {
    return 0;
  }
  if (std::strcmp(fmt, "b") == 0) {
    return total + ((len + 7) >> 3);
  }
  int fw = fixed_width_bytes(fmt);
  if (fw >= 0) {
    return total + len * fw;
  }
  if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0) {
    const int32_t* offs = static_cast<const int32_t*>(arr->buffers[1]);
    int64_t data_bytes = offs ? (offs[start + len] - offs[start]) : 0;
    return total + (len + 1) * 4 + data_bytes;
  }
  if (std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    const int64_t* offs = static_cast<const int64_t*>(arr->buffers[1]);
    int64_t data_bytes = offs ? (offs[start + len] - offs[start]) : 0;
    return total + (len + 1) * 8 + data_bytes;
  }
  if (std::strcmp(fmt, "+s") == 0) {
    for (int64_t j = 0; j < sch->n_children; j++) {
      total += estimate_field_bytes(sch->children[j], arr->children[j], start, len);
    }
    return total;
  }
  if (std::strcmp(fmt, "+l") == 0) {
    const int32_t* offs = static_cast<const int32_t*>(arr->buffers[1]);
    int32_t c_start = offs ? offs[start] : 0;
    int32_t c_end = offs ? offs[start + len] : 0;
    return total + (len + 1) * 4 +
           estimate_field_bytes(sch->children[0], arr->children[0], c_start, c_end - c_start);
  }
  if (std::strcmp(fmt, "+L") == 0) {
    const int64_t* offs = static_cast<const int64_t*>(arr->buffers[1]);
    int64_t c_start = offs ? offs[start] : 0;
    int64_t c_end = offs ? offs[start + len] : 0;
    return total + (len + 1) * 8 +
           estimate_field_bytes(sch->children[0], arr->children[0], c_start, c_end - c_start);
  }
  if (std::strncmp(fmt, "+w:", 3) == 0) {
    int64_t w = std::atoll(fmt + 3);
    return total + estimate_field_bytes(sch->children[0], arr->children[0], start * w, len * w);
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

    const uint8_t* valid = static_cast<const uint8_t*>(arr->buffers[0]);
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
      for (int64_t j = 0; j < sch->n_children; j++) {
        if (!visit_field(sch->children[j], arr->children[j], start, len, err_out)) {
          return false;
        }
      }
      return true;
    }

    if (std::strcmp(fmt, "+l") == 0) {
      const int32_t* offs = static_cast<const int32_t*>(arr->buffers[1]);
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
      const int64_t* offs = static_cast<const int64_t*>(arr->buffers[1]);
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
      int64_t w = std::atoll(fmt + 3);
      return visit_field(sch->children[0], arr->children[0], start * w, len * w, err_out);
    }

    if (std::strcmp(fmt, "b") == 0) {
      const uint8_t* bits = static_cast<const uint8_t*>(arr->buffers[1]);
      if (len <= 0 || bits == nullptr) {
        return append_buffer(nullptr, 0, err_out);
      }
      const uint8_t* bslice = slice_bitmap(bits, start, len, &bitmap_scratch_);
      return append_buffer(bslice, (len + 7) >> 3, err_out);
    }

    if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0) {
      const int32_t* offs = static_cast<const int32_t*>(arr->buffers[1]);
      const uint8_t* data = static_cast<const uint8_t*>(arr->buffers[2]);
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
      const int64_t* offs = static_cast<const int64_t*>(arr->buffers[1]);
      const uint8_t* data = static_cast<const uint8_t*>(arr->buffers[2]);
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
      const uint8_t* data = static_cast<const uint8_t*>(arr->buffers[1]);
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

} // namespace bqs_ipc

#endif // BQS_ARROW_IPC_H
