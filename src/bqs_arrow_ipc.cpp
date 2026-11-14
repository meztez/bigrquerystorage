#include "bqs_arrow_ipc.h"
#include "bqs_parallel.h"

#include <algorithm>
#include <cstdlib>

#ifdef HAVE_BQS_ZSTD
#include <zstd.h>
#endif

namespace bqs_ipc {

namespace {

inline void put_u16(uint8_t* dst, uint16_t v) { std::memcpy(dst, &v, 2); }
inline void put_i16(uint8_t* dst, int16_t v) { std::memcpy(dst, &v, 2); }
inline void put_u32(uint8_t* dst, uint32_t v) { std::memcpy(dst, &v, 4); }
inline void put_i32(uint8_t* dst, int32_t v) { std::memcpy(dst, &v, 4); }
inline void put_i64(uint8_t* dst, int64_t v) { std::memcpy(dst, &v, 8); }

inline uint16_t fb_read_u16(const uint8_t* p) { return load_unaligned<uint16_t>(p); }
inline int32_t fb_read_i32(const uint8_t* p) { return load_unaligned<int32_t>(p); }
inline uint32_t fb_read_u32(const uint8_t* p) { return load_unaligned<uint32_t>(p); }
inline int64_t fb_read_i64(const uint8_t* p) { return load_unaligned<int64_t>(p); }

template <typename OffsetT>
inline int64_t offset_span_bytes(const ArrowArray* arr, int64_t start, int64_t len) {
  const OffsetT* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                          ? static_cast<const OffsetT*>(arr->buffers[1])
                          : nullptr;
  if (!offs) return 0;
  return std::max<int64_t>(
    0, static_cast<int64_t>(offs[start + len]) - static_cast<int64_t>(offs[start]));
}

template <typename OffsetT>
inline int64_t estimate_list_bytes(const ArrowSchema* sch,
                                   const ArrowArray* arr,
                                   int64_t start,
                                   int64_t len) {
  const OffsetT* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                          ? static_cast<const OffsetT*>(arr->buffers[1])
                          : nullptr;
  int64_t c_start = offs ? static_cast<int64_t>(offs[start]) : 0;
  int64_t c_end = offs ? static_cast<int64_t>(offs[start + len]) : 0;
  const ArrowArray* child_arr =
    (arr->children != nullptr && arr->n_children > 0) ? arr->children[0] : nullptr;
  return (len + 1) * static_cast<int64_t>(sizeof(OffsetT)) +
         estimate_field_bytes(sch->children[0], child_arr, c_start, std::max<int64_t>(0, c_end - c_start));
}

bool fb_open_table(const uint8_t* fb,
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

bool fb_field_checked(const uint8_t* fb,
                      size_t meta_size,
                      uint32_t table_off,
                      uint32_t vt_off,
                      uint16_t vt_len,
                      uint16_t slot_idx,
                      size_t field_bytes,
                      uint32_t* pos_out) {
  *pos_out = 0u;
  uint32_t entry = 4u + static_cast<uint32_t>(slot_idx) * 2u;
  if (entry + 2u > vt_len) return true;
  uint16_t foff = fb_read_u16(fb + vt_off + entry);
  if (foff == 0u) return true;
  uint64_t fpos = static_cast<uint64_t>(table_off) + static_cast<uint64_t>(foff);
  if (fpos + field_bytes > meta_size) return false;
  *pos_out = static_cast<uint32_t>(fpos);
  return true;
}

struct ParsedIpcFrame {
  bool is_eos = false;
  size_t prefix_len = 0;
  size_t meta_size = 0;
  const uint8_t* fb = nullptr;
  uint8_t header_type = 0;
  uint32_t hdr_pos = 0;
  int64_t body_len = 0;
  size_t msg_len = 0;
};

bool parse_ipc_message_frame(const uint8_t* data,
                             size_t avail,
                             ParsedIpcFrame* out,
                             std::string* err_out) {
  *out = ParsedIpcFrame{};
  if (avail < 4) {
    if (err_out) *err_out = "Invalid Arrow IPC: truncated message prefix.";
    return false;
  }
  uint32_t cont = fb_read_u32(data);
  int32_t meta_len = 0;
  if (cont == 0xFFFFFFFFu) {
    if (avail < 8) {
      if (err_out) *err_out = "Invalid Arrow IPC: truncated continuation prefix.";
      return false;
    }
    meta_len = fb_read_i32(data + 4);
    if (meta_len == 0) {
      out->is_eos = true;
      out->msg_len = 8;
      return true;
    }
    out->prefix_len = 8;
  } else {
    meta_len = static_cast<int32_t>(cont);
    if (meta_len == 0) {
      out->is_eos = true;
      out->msg_len = 4;
      return true;
    }
    out->prefix_len = 4;
  }

  if (meta_len < 4 || out->prefix_len + static_cast<size_t>(meta_len) > avail) {
    if (err_out) *err_out = "Invalid Arrow IPC: truncated FlatBuffer metadata.";
    return false;
  }
  out->meta_size = static_cast<size_t>(meta_len);
  out->fb = data + out->prefix_len;

  uint32_t root_off = fb_read_u32(out->fb);
  uint32_t msg_vt_off = 0;
  uint16_t msg_vt_len = 0;
  if (!fb_open_table(out->fb, out->meta_size, root_off, &msg_vt_off, &msg_vt_len)) {
    if (err_out) *err_out = "Invalid Arrow IPC: out-of-bounds Message table.";
    return false;
  }

  uint32_t ht_pos = 0, bl_pos = 0;
  if (!fb_field_checked(out->fb, out->meta_size, root_off, msg_vt_off, msg_vt_len, 1, 1, &ht_pos) ||
      !fb_field_checked(out->fb, out->meta_size, root_off, msg_vt_off, msg_vt_len, 2, 4, &out->hdr_pos) ||
      !fb_field_checked(out->fb, out->meta_size, root_off, msg_vt_off, msg_vt_len, 3, 8, &bl_pos)) {
    if (err_out) *err_out = "Invalid Arrow IPC: out-of-bounds Message fields.";
    return false;
  }

  out->header_type = ht_pos ? out->fb[ht_pos] : 0;
  out->body_len = bl_pos ? fb_read_i64(out->fb + bl_pos) : 0;
  size_t body_off = out->prefix_len + out->meta_size;
  if (out->body_len < 0 ||
      static_cast<uint64_t>(out->body_len) > static_cast<uint64_t>(avail - body_off)) {
    if (err_out) *err_out = "Invalid Arrow IPC: truncated message body.";
    return false;
  }
  out->msg_len = body_off + static_cast<size_t>(out->body_len);
  return true;
}

template <typename OffsetT>
bool validate_monotonic_offsets(const BufferDesc& obuf,
                                const uint8_t* body_ptr,
                                int64_t len,
                                const char* kind,
                                int64_t* last_off_out,
                                std::string* err_out) {
  *last_off_out = 0;
  if (len <= 0) return true;
  const int64_t elem_sz = static_cast<int64_t>(sizeof(OffsetT));
  if (obuf.length < (len + 1) * elem_sz || !body_ptr) {
    if (err_out) *err_out = std::string("Invalid Arrow IPC RecordBatch: ") + kind + " offset buffer shorter than row count.";
    return false;
  }
  const uint8_t* op = body_ptr + obuf.offset;
  OffsetT prev = load_unaligned<OffsetT>(op);
  if (prev < 0) {
    if (err_out) *err_out = std::string("Invalid Arrow IPC RecordBatch: negative ") + kind + " offset.";
    return false;
  }
  for (int64_t i = 1; i <= len; i++) {
    OffsetT cur = load_unaligned<OffsetT>(op + i * elem_sz);
    if (cur < prev) {
      if (err_out) *err_out = std::string("Invalid Arrow IPC RecordBatch: non-monotonic ") + kind + " offsets.";
      return false;
    }
    prev = cur;
  }
  *last_off_out = static_cast<int64_t>(prev);
  return true;
}

} // namespace

bool has_zstd_support() {
#ifdef HAVE_BQS_ZSTD
  return true;
#else
  return false;
#endif
}

int fixed_width_bytes(const char* fmt) {
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

bool can_fast_encode_field(const ArrowSchema* sch) {
  if (!sch || !sch->format || sch->dictionary != nullptr) return false;
  const char* fmt = sch->format;
  if (std::strcmp(fmt, "n") == 0 || std::strcmp(fmt, "b") == 0 ||
      std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0 ||
      std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0 ||
      fixed_width_bytes(fmt) >= 0) {
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

bool can_fast_encode_schema(const ArrowSchema* root_schema) {
  if (!root_schema || !root_schema->format || std::strcmp(root_schema->format, "+s") != 0) {
    return false;
  }
  for (int64_t j = 0; j < root_schema->n_children; j++) {
    if (!can_fast_encode_field(root_schema->children[j])) return false;
  }
  return true;
}

int64_t estimate_field_bytes(const ArrowSchema* sch,
                             const ArrowArray* arr,
                             int64_t logical_start,
                             int64_t len) {
  if (!sch || !arr || len <= 0) return 0;
  const char* fmt = sch->format;
  if (std::strcmp(fmt, "n") == 0) return 0;

  int64_t start = arr->offset + logical_start;
  int64_t total = (arr->null_count != 0 && arr->buffers != nullptr && arr->buffers[0] != nullptr)
                    ? ((len + 7) >> 3)
                    : 0;

  if (std::strcmp(fmt, "b") == 0) return total + ((len + 7) >> 3);
  int fw = fixed_width_bytes(fmt);
  if (fw >= 0) return total + len * fw;
  if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0) {
    return total + (len + 1) * 4 + offset_span_bytes<int32_t>(arr, start, len);
  }
  if (std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    return total + (len + 1) * 8 + offset_span_bytes<int64_t>(arr, start, len);
  }
  if (std::strcmp(fmt, "+s") == 0) {
    for (int64_t j = 0; j < sch->n_children; j++) {
      const ArrowArray* child_arr =
        (arr->children != nullptr && j < arr->n_children) ? arr->children[j] : nullptr;
      total += estimate_field_bytes(sch->children[j], child_arr, start, len);
    }
    return total;
  }
  if (std::strcmp(fmt, "+l") == 0) return total + estimate_list_bytes<int32_t>(sch, arr, start, len);
  if (std::strcmp(fmt, "+L") == 0) return total + estimate_list_bytes<int64_t>(sch, arr, start, len);
  if (std::strncmp(fmt, "+w:", 3) == 0) {
    int64_t w = std::max<int64_t>(0, std::atoll(fmt + 3));
    const ArrowArray* child_arr =
      (arr->children != nullptr && arr->n_children > 0) ? arr->children[0] : nullptr;
    return total + estimate_field_bytes(sch->children[0], child_arr, start * w, len * w);
  }
  return total;
}

int64_t estimate_batch_bytes(const ArrowSchema* root_schema, const ArrowArray* root_array) {
  if (!root_schema || !root_array || root_array->length <= 0) return 0;
  int64_t total = 0;
  for (int64_t j = 0; j < root_schema->n_children; j++) {
    total += estimate_field_bytes(
      root_schema->children[j], root_array->children[j], root_array->offset, root_array->length);
  }
  return total;
}

int64_t count_nulls_in_bitmap(const uint8_t* valid, int64_t start, int64_t len) {
  if (!valid || len <= 0) return 0;
  int64_t set_bits = 0;
  int64_t i = 0;
  while (i < len && ((start + i) & 63) != 0) {
    int64_t idx = start + i;
    set_bits += (valid[idx >> 3] >> (idx & 7)) & 1;
    i++;
  }
  while (i + 64 <= len) {
    uint64_t word = load_unaligned<uint64_t>(valid + ((start + i) >> 3));
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

const uint8_t* slice_bitmap(const uint8_t* src_bits,
                            int64_t start,
                            int64_t len,
                            std::vector<uint8_t>* scratch) {
  if (len <= 0 || src_bits == nullptr) return nullptr;
  int64_t out_bytes = (len + 7) >> 3;
  int bit_off = static_cast<int>(start & 7);
  const uint8_t* byte_ptr = src_bits + (start >> 3);
  if (bit_off == 0) return byte_ptr;

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

RecordBatchEncoder::RecordBatchEncoder(bool use_zstd, int zstd_level)
    : use_zstd_(use_zstd), zstd_level_(zstd_level) {
#ifndef HAVE_BQS_ZSTD
  use_zstd_ = false;
#endif
}

bool RecordBatchEncoder::append_buffer(const uint8_t* raw_data,
                                       int64_t raw_len,
                                       std::string* err_out) {
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

template <typename OffsetT>
bool RecordBatchEncoder::append_sliced_offsets(const ArrowArray* arr,
                                               int64_t start,
                                               int64_t len,
                                               std::vector<OffsetT>* scratch,
                                               OffsetT* base_out,
                                               OffsetT* end_out,
                                               std::string* err_out) {
  const OffsetT* offs = (arr->buffers != nullptr && arr->n_buffers > 1)
                          ? static_cast<const OffsetT*>(arr->buffers[1])
                          : nullptr;
  if (len <= 0 || offs == nullptr) {
    OffsetT zero = 0;
    *base_out = 0;
    *end_out = 0;
    return append_buffer(
      reinterpret_cast<const uint8_t*>(&zero), static_cast<int64_t>(sizeof(OffsetT)), err_out);
  }
  OffsetT base_off = offs[start];
  OffsetT end_off = offs[start + len];
  *base_out = base_off;
  *end_out = end_off;
  const int64_t nbytes = (len + 1) * static_cast<int64_t>(sizeof(OffsetT));
  if (base_off == 0) {
    return append_buffer(reinterpret_cast<const uint8_t*>(offs + start), nbytes, err_out);
  }
  scratch->resize(static_cast<size_t>(len + 1));
  for (int64_t i = 0; i <= len; i++) {
    (*scratch)[static_cast<size_t>(i)] = offs[start + i] - base_off;
  }
  return append_buffer(reinterpret_cast<const uint8_t*>(scratch->data()), nbytes, err_out);
}

bool RecordBatchEncoder::visit_field(const ArrowSchema* sch,
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
  int64_t slice_nulls =
    (arr->null_count != 0 && valid != nullptr && len > 0)
      ? count_nulls_in_bitmap(valid, start, len)
      : 0;
  nodes_.push_back({len, slice_nulls});

  if (slice_nulls > 0) {
    const uint8_t* vslice = slice_bitmap(valid, start, len, &bitmap_scratch_);
    if (!append_buffer(vslice, (len + 7) >> 3, err_out)) return false;
  } else if (!append_buffer(nullptr, 0, err_out)) {
    return false;
  }

  if (std::strcmp(fmt, "+s") == 0) {
    if (!arr->children || arr->n_children < sch->n_children) {
      if (err_out) *err_out = "Invalid ArrowArray struct: missing child arrays.";
      return false;
    }
    for (int64_t j = 0; j < sch->n_children; j++) {
      if (!visit_field(sch->children[j], arr->children[j], start, len, err_out)) return false;
    }
    return true;
  }

  if (std::strcmp(fmt, "+l") == 0) {
    if (!arr->children || arr->n_children < 1) {
      if (err_out) *err_out = "Invalid ArrowArray list: missing child array.";
      return false;
    }
    int32_t base_off = 0, end_off = 0;
    if (!append_sliced_offsets(arr, start, len, &offs32_scratch_, &base_off, &end_off, err_out)) {
      return false;
    }
    return visit_field(sch->children[0], arr->children[0], base_off, end_off - base_off, err_out);
  }

  if (std::strcmp(fmt, "+L") == 0) {
    if (!arr->children || arr->n_children < 1) {
      if (err_out) *err_out = "Invalid ArrowArray large_list: missing child array.";
      return false;
    }
    int64_t base_off = 0, end_off = 0;
    if (!append_sliced_offsets(arr, start, len, &offs64_scratch_, &base_off, &end_off, err_out)) {
      return false;
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
    if (len <= 0 || bits == nullptr) return append_buffer(nullptr, 0, err_out);
    return append_buffer(slice_bitmap(bits, start, len, &bitmap_scratch_), (len + 7) >> 3, err_out);
  }

  if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0) {
    int32_t byte_start = 0, byte_end = 0;
    if (!append_sliced_offsets(arr, start, len, &offs32_scratch_, &byte_start, &byte_end, err_out)) {
      return false;
    }
    const uint8_t* data = (arr->buffers != nullptr && arr->n_buffers > 2)
                            ? static_cast<const uint8_t*>(arr->buffers[2])
                            : nullptr;
    int64_t data_len = static_cast<int64_t>(byte_end) - static_cast<int64_t>(byte_start);
    return append_buffer(data ? (data + byte_start) : nullptr, data_len, err_out);
  }

  if (std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    int64_t byte_start = 0, byte_end = 0;
    if (!append_sliced_offsets(arr, start, len, &offs64_scratch_, &byte_start, &byte_end, err_out)) {
      return false;
    }
    const uint8_t* data = (arr->buffers != nullptr && arr->n_buffers > 2)
                            ? static_cast<const uint8_t*>(arr->buffers[2])
                            : nullptr;
    return append_buffer(data ? (data + byte_start) : nullptr, byte_end - byte_start, err_out);
  }

  int fw = fixed_width_bytes(fmt);
  if (fw >= 0) {
    const uint8_t* data = (arr->buffers != nullptr && arr->n_buffers > 1)
                            ? static_cast<const uint8_t*>(arr->buffers[1])
                            : nullptr;
    int64_t data_len = len * fw;
    return append_buffer((data && data_len > 0) ? (data + start * fw) : nullptr, data_len, err_out);
  }

  if (err_out) *err_out = std::string("Unsupported Arrow format in fast IPC encoder: ") + fmt;
  return false;
}

std::string RecordBatchEncoder::encode(const ArrowSchema* root_schema,
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

  // Exact Arrow IPC Message(RecordBatch) FlatBuffer layout:
  // Fixed 100-byte prefix [0x00..0x63], buffers vector at 0x64, 4 zero pad bytes,
  // nodes vector at 0x6C + num_buffers * 16.
  const uint32_t num_buffers = static_cast<uint32_t>(buffers_.size());
  const uint32_t num_nodes = static_cast<uint32_t>(nodes_.size());
  const uint32_t nodes_pos = 0x6Cu + num_buffers * 16u;
  const uint32_t meta_len = nodes_pos + 4u + num_nodes * 16u;
  const int64_t body_len = static_cast<int64_t>(body_.size());

  std::string out;
  out.resize(8u + static_cast<size_t>(meta_len) + body_.size());
  uint8_t* p = reinterpret_cast<uint8_t*>(&out[0]);
  std::memset(p, 0, 8u + static_cast<size_t>(meta_len));

  put_u32(p + 0, 0xFFFFFFFFu);
  put_i32(p + 4, static_cast<int32_t>(meta_len));

  uint8_t* fb = p + 8;
  put_u32(fb + 0x00, 0x14u);

  put_u16(fb + 0x08, 12u);
  put_u16(fb + 0x0A, 24u);
  put_u16(fb + 0x0C, 6u);
  put_u16(fb + 0x0E, 5u);
  put_u16(fb + 0x10, 8u);
  put_u16(fb + 0x12, 12u);

  put_i32(fb + 0x14, 12);
  fb[0x19] = 3u;
  put_i16(fb + 0x1A, 4);
  put_u32(fb + 0x1C, 0x1Cu);
  put_i64(fb + 0x20, body_len);

  put_u16(fb + 0x2C, 12u);
  put_u16(fb + 0x2E, 24u);
  put_u16(fb + 0x30, 16u);
  put_u16(fb + 0x32, 4u);
  put_u16(fb + 0x34, 8u);
  put_u16(fb + 0x36, use_zstd_ ? 12u : 0u);

  put_i32(fb + 0x38, 12);
  put_u32(fb + 0x3C, nodes_pos - 0x3Cu);
  put_u32(fb + 0x40, 0x64u - 0x40u);
  put_u32(fb + 0x44, use_zstd_ ? (0x5Cu - 0x44u) : 0u);
  put_i64(fb + 0x48, row_len);

  if (use_zstd_) {
    put_u16(fb + 0x56, 6u);
    put_u16(fb + 0x58, 8u);
    put_u16(fb + 0x5A, 7u);
    put_i32(fb + 0x5C, 6);
    fb[0x63] = 1u;
  }

  put_u32(fb + 0x64, num_buffers);
  uint8_t* buf_dst = fb + 0x68;
  for (uint32_t i = 0; i < num_buffers; i++) {
    put_i64(buf_dst + i * 16u + 0u, buffers_[i].offset);
    put_i64(buf_dst + i * 16u + 8u, buffers_[i].length);
  }

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

std::vector<BatchSliceSpec> plan_batch_slices(const ArrowSchema* schema,
                                              const std::vector<ArrowArray>& src_arrays,
                                              int64_t total_rows,
                                              int64_t total_uncompressed_bytes,
                                              int row_batch_size,
                                              int target_min_batches,
                                              bool align_64) {
  const int64_t max_batch_bytes = 5LL * 1024LL * 1024LL;
  double bytes_per_row = std::max(
    16.0, static_cast<double>(total_uncompressed_bytes) / static_cast<double>(std::max<int64_t>(1, total_rows)));
  int64_t max_rows_by_bytes =
    std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(max_batch_bytes) / bytes_per_row));
  int min_batches = std::max(1, target_min_batches);
  int64_t max_rows_for_stealing =
    std::max<int64_t>(1, (total_rows + min_batches - 1) / min_batches);
  int64_t step = std::min<int64_t>({
    static_cast<int64_t>(std::max(1, row_batch_size)),
    max_rows_by_bytes,
    max_rows_for_stealing
  });
  if (align_64 && step >= 64) {
    step = (step / 64) * 64;
  }

  const int64_t hard_slice_bytes = 10LL * 1024LL * 1024LL;
  std::vector<BatchSliceSpec> specs;
  for (size_t a_idx = 0; a_idx < src_arrays.size(); a_idx++) {
    int64_t alen = src_arrays[a_idx].length;
    int64_t s = 0;
    while (s < alen) {
      int64_t len = std::min<int64_t>(step, alen - s);
      while (len > 1) {
        int64_t slice_bytes = 0;
        for (int64_t j = 0; j < schema->n_children; j++) {
          slice_bytes += estimate_field_bytes(
            schema->children[j], src_arrays[a_idx].children[j], src_arrays[a_idx].offset + s, len);
        }
        if (slice_bytes <= hard_slice_bytes) break;
        len = std::max<int64_t>(1, len / 2);
      }
      specs.push_back({a_idx, s, len});
      s += len;
    }
  }
  return specs;
}

size_t strip_ipc_eos(const uint8_t* data, size_t len) {
  if (len >= 8 && fb_read_u32(data + len - 8) == 0xFFFFFFFFu && fb_read_i32(data + len - 4) == 0) {
    return len - 8;
  }
  return len;
}

bool encode_batches_parallel(const ArrowSchema* schema,
                             const std::vector<ArrowArray>& src_arrays,
                             const std::vector<BatchSliceSpec>& specs,
                             bool use_zstd,
                             int n_threads,
                             std::vector<std::string>* out_batches,
                             std::string* err_out) {
  size_t m = specs.size();
  out_batches->resize(m);
  if (m == 0) return true;

  int resolved = bqs_resolve_threads(n_threads, 16u);
  int n_workers = static_cast<int>(std::min<size_t>(static_cast<size_t>(std::max(1, resolved)), m));
  std::vector<RecordBatchEncoder> encoders;
  encoders.reserve(static_cast<size_t>(n_workers));
  for (int w = 0; w < n_workers; w++) encoders.emplace_back(use_zstd, 1);

  std::vector<std::string> worker_errs(static_cast<size_t>(n_workers));
  bqs_parallel_for(m, n_workers, [&](int w, size_t idx) -> bool {
    const BatchSliceSpec& sp = specs[idx];
    std::string err;
    std::string encoded = encoders[static_cast<size_t>(w)].encode(
      schema, &src_arrays[sp.array_idx], sp.row_start, sp.row_len, &err);
    if (!err.empty()) {
      worker_errs[static_cast<size_t>(w)] = std::move(err);
      return false;
    }
    (*out_batches)[idx] = std::move(encoded);
    return true;
  });

  for (const auto& e : worker_errs) {
    if (!e.empty()) {
      if (err_out) *err_out = e;
      return false;
    }
  }
  return true;
}

ZstdDecompressContext::ZstdDecompressContext() {
#ifdef HAVE_BQS_ZSTD
  dctx_ = ZSTD_createDCtx();
#endif
}

ZstdDecompressContext::~ZstdDecompressContext() {
#ifdef HAVE_BQS_ZSTD
  if (dctx_) ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(dctx_));
#endif
}

bool decode_ipc_record_batch(std::string&& raw_msg,
                             DecodedBatch* out,
                             ZstdDecompressContext* zctx,
                             std::string* err_out) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(raw_msg.data());
  size_t len = raw_msg.size();
  if (len < 8) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: message shorter than 8 bytes.";
    return false;
  }

  ParsedIpcFrame frame;
  if (!parse_ipc_message_frame(data, len, &frame, err_out)) {
    return false;
  }
  if (frame.is_eos || frame.header_type != 3 || !frame.hdr_pos) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: expected RecordBatch header.";
    return false;
  }

  const uint8_t* fb = frame.fb;
  const size_t meta_size = frame.meta_size;
  uint64_t rb_off64 = static_cast<uint64_t>(frame.hdr_pos) + fb_read_u32(fb + frame.hdr_pos);
  if (rb_off64 > meta_size) {
    if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: out-of-bounds RecordBatch offset.";
    return false;
  }
  uint32_t rb_off = static_cast<uint32_t>(rb_off64);
  size_t body_offset_in_msg = frame.prefix_len + meta_size;
  const uint64_t ubody_len = static_cast<uint64_t>(frame.body_len);

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

  static const uint64_t MAX_BATCH_BYTES = 2ULL * 1024ULL * 1024ULL * 1024ULL;
  if (!is_compressed) {
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
      out->body_size = static_cast<size_t>(frame.body_len);
      out->refresh_body_ptr();
      if (out->body_size == 0 || (reinterpret_cast<uintptr_t>(out->body_ptr) & 7u) == 0u) {
        return true;
      }
      raw_bufs = std::move(out->buffers);
      raw_msg = std::move(out->raw_ipc);
      data = reinterpret_cast<const uint8_t*>(raw_msg.data());
    }

    const uint8_t* src_body = data + body_offset_in_msg;
    uint64_t total_aligned64 = 0;
    for (const auto& bd : raw_bufs) {
      if (bd.length > 0) {
        total_aligned64 += (static_cast<uint64_t>(bd.length) + 7ULL) & ~7ULL;
        if (total_aligned64 > MAX_BATCH_BYTES) {
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
      if (blen <= 0) {
        out->buffers[i] = {static_cast<int64_t>(dst_pos), 0};
        continue;
      }
      size_t slen = static_cast<size_t>(blen);
      std::memcpy(dst_base + dst_pos, src_body + raw_bufs[i].offset, slen);
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

    uint64_t total_out_bytes64 = 0;
    for (size_t i = 0; i < n_bufs; i++) {
      int64_t blen = raw_bufs[i].length;
      if (blen <= 0) continue;
      if (blen < 8) {
        if (err_out) *err_out = "Invalid compressed Arrow buffer (< 8 bytes).";
        return false;
      }
      int64_t ulen = fb_read_i64(src_body + raw_bufs[i].offset);
      if (ulen < -1 || static_cast<uint64_t>(ulen > 0 ? ulen : 0) > MAX_BATCH_BYTES) {
        if (err_out) *err_out = "Invalid compressed Arrow buffer uncompressed length.";
        return false;
      }
      uint64_t actual = (ulen == -1) ? static_cast<uint64_t>(blen - 8)
                                     : static_cast<uint64_t>(std::max<int64_t>(0, ulen));
      total_out_bytes64 += (actual + 7ULL) & ~7ULL;
      if (total_out_bytes64 > MAX_BATCH_BYTES) {
        if (err_out) *err_out = "Decompressed Arrow RecordBatch exceeds 2 GiB safety limit.";
        return false;
      }
    }
    size_t total_out_bytes = static_cast<size_t>(total_out_bytes64);
    out->decompressed.reset(total_out_bytes > 0 ? new uint8_t[total_out_bytes] : nullptr);
    uint8_t* dst_base = out->decompressed.get();
    size_t dst_pos = 0;
    ZSTD_DCtx* dctx = zctx ? static_cast<ZSTD_DCtx*>(zctx->get()) : nullptr;

    for (size_t i = 0; i < n_bufs; i++) {
      int64_t blen = raw_bufs[i].length;
      if (blen <= 0) {
        out->buffers[i] = {static_cast<int64_t>(dst_pos), 0};
        continue;
      }
      int64_t boff = raw_bufs[i].offset;
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
        size_t res = dctx
                       ? ZSTD_decompressDCtx(dctx, dst_base + dst_pos, target_len, csrc, clen)
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

    std::string().swap(raw_msg);
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

void count_schema_nodes_and_buffers(const ArrowSchema* sch, size_t* n_nodes, size_t* n_bufs) {
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

bool validate_borrowed_field_buffers(const ArrowSchema* sch,
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
  if (std::strcmp(fmt, "n") == 0) return true;

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
    int64_t child_min_len = 0;
    bool ok = (std::strcmp(fmt, "+L") == 0)
                ? validate_monotonic_offsets<int64_t>(obuf, batch.body_ptr, nd.length, "list", &child_min_len, err_out)
                : validate_monotonic_offsets<int32_t>(obuf, batch.body_ptr, nd.length, "list", &child_min_len, err_out);
    return ok && validate_borrowed_field_buffers(
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
    const char* kind = (fmt[0] == 'u' || fmt[0] == 'U') ? "string" : "binary";
    int64_t last_off = 0;
    bool ok = (fmt[0] == 'U' || fmt[0] == 'Z')
                ? validate_monotonic_offsets<int64_t>(obuf, batch.body_ptr, nd.length, kind, &last_off, err_out)
                : validate_monotonic_offsets<int32_t>(obuf, batch.body_ptr, nd.length, kind, &last_off, err_out);
    if (!ok) return false;
    if (last_off > dbuf.length || (last_off > 0 && !batch.body_ptr)) {
      if (err_out) *err_out = "Invalid Arrow IPC RecordBatch: offsets exceed data buffer bounds.";
      return false;
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

void release_borrowed_array(struct ArrowArray* a) {
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

ArrowArray* build_borrowed_field_array(const ArrowSchema* sch,
                                       const DecodedBatch& batch,
                                       size_t* node_pos,
                                       size_t* buf_pos) {
  ArrowArray* arr = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
  BorrowedArrayPriv* priv = new BorrowedArrayPriv();
  const char* fmt = (sch && sch->format) ? sch->format : "";

  const FieldNodeDesc& nd = batch.nodes[(*node_pos)++];
  int n_bufs = 2;
  if (std::strcmp(fmt, "n") == 0) {
    n_bufs = 0;
  } else if (std::strcmp(fmt, "+s") == 0 || std::strncmp(fmt, "+w:", 3) == 0) {
    n_bufs = 1;
  } else if (std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "z") == 0 ||
             std::strcmp(fmt, "U") == 0 || std::strcmp(fmt, "Z") == 0) {
    n_bufs = 3;
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
    arr->children = static_cast<ArrowArray**>(
      malloc(static_cast<size_t>(sch->n_children) * sizeof(ArrowArray*)));
    for (int64_t c = 0; c < sch->n_children; c++) {
      arr->children[c] = build_borrowed_field_array(sch->children[c], batch, node_pos, buf_pos);
    }
  } else {
    arr->children = nullptr;
  }
  return arr;
}

void truncate_borrowed_field_array(ArrowArray* arr, const ArrowSchema* sch, int64_t new_len) {
  if (!arr || !sch || new_len >= arr->length) return;
  const char* fmt = sch->format ? sch->format : "";
  arr->length = new_len;
  if (std::strcmp(fmt, "n") == 0) {
    arr->null_count = new_len;
    return;
  }
  arr->null_count = (arr->null_count != 0 && arr->buffers != nullptr && arr->buffers[0] != nullptr)
                      ? count_nulls_in_bitmap(static_cast<const uint8_t*>(arr->buffers[0]), arr->offset, new_len)
                      : 0;
  if (std::strcmp(fmt, "+s") == 0) {
    for (int64_t c = 0; c < sch->n_children && c < arr->n_children; c++) {
      truncate_borrowed_field_array(arr->children[c], sch->children[c], new_len);
    }
  } else if (std::strncmp(fmt, "+w:", 3) == 0 && sch->n_children == 1 && arr->n_children == 1) {
    int64_t w = std::atoll(fmt + 3);
    truncate_borrowed_field_array(arr->children[0], sch->children[0], new_len * std::max<int64_t>(0, w));
  }
}

bool split_arrow_ipc_stream(const uint8_t* data,
                            size_t len,
                            std::string* schema_out,
                            std::vector<std::string>* batches_out,
                            std::vector<int64_t>* batch_rows_out,
                            int64_t* total_rows_out,
                            std::string* err_out) {
  int64_t total_rows = 0;
  size_t pos = 0;
  while (pos < len) {
    ParsedIpcFrame frame;
    if (!parse_ipc_message_frame(data + pos, len - pos, &frame, err_out)) {
      return false;
    }
    if (frame.is_eos) break;

    auto pack_canonical_msg = [&]() -> std::string {
      if (frame.prefix_len == 8) {
        return std::string(reinterpret_cast<const char*>(data + pos), frame.msg_len);
      }
      size_t aligned_meta = (frame.meta_size + 7u) & ~static_cast<size_t>(7u);
      std::string out(8u + aligned_meta + static_cast<size_t>(frame.body_len), '\0');
      put_u32(reinterpret_cast<uint8_t*>(&out[0]), 0xFFFFFFFFu);
      put_i32(reinterpret_cast<uint8_t*>(&out[4]), static_cast<int32_t>(aligned_meta));
      std::memcpy(&out[8], frame.fb, frame.meta_size);
      if (frame.body_len > 0) {
        std::memcpy(&out[8u + aligned_meta],
                    frame.fb + frame.meta_size,
                    static_cast<size_t>(frame.body_len));
      }
      return out;
    };

    if (frame.header_type == 1) {
      if (schema_out) *schema_out = pack_canonical_msg();
    } else if (frame.header_type == 3) {
      if (batches_out) batches_out->push_back(pack_canonical_msg());
      int64_t rb_rows = 0;
      if (frame.hdr_pos) {
        uint64_t rb_off64 = static_cast<uint64_t>(frame.hdr_pos) + fb_read_u32(frame.fb + frame.hdr_pos);
        uint32_t rb_vt_off = 0;
        uint16_t rb_vt_len = 0;
        uint32_t len_pos = 0;
        if (rb_off64 > frame.meta_size ||
            !fb_open_table(frame.fb, frame.meta_size, static_cast<uint32_t>(rb_off64), &rb_vt_off, &rb_vt_len) ||
            !fb_field_checked(frame.fb, frame.meta_size, static_cast<uint32_t>(rb_off64), rb_vt_off, rb_vt_len, 0, 8, &len_pos)) {
          if (err_out) *err_out = "Invalid Arrow IPC stream: out-of-bounds RecordBatch header.";
          return false;
        }
        if (len_pos) {
          rb_rows = fb_read_i64(frame.fb + len_pos);
          if (rb_rows < 0) {
            if (err_out) *err_out = "Invalid Arrow IPC stream: negative RecordBatch row count.";
            return false;
          }
          total_rows += rb_rows;
        }
      }
      if (batch_rows_out) batch_rows_out->push_back(rb_rows);
    }
    pos += frame.msg_len;
  }
  if (batches_out && !batches_out->empty() && schema_out && schema_out->empty()) {
    if (err_out) *err_out = "Invalid Arrow IPC stream: missing Schema message.";
    return false;
  }
  if (total_rows_out) *total_rows_out = total_rows;
  return true;
}

} // namespace bqs_ipc
