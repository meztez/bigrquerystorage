// [[Rcpp::depends(nanoarrow)]]
#include "bqs_arrow_ipc.h"
#include "bqs_parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* const EXT_DATETIME = "google:sqlType:datetime";
const char* const EXT_GEOGRAPHY = "google:sqlType:geography";

struct RCallbacks {
  static Rcpp::Function ns_fn(const char* pkg, const char* name) {
    SEXP pkg_sexp = PROTECT(Rf_mkString(pkg));
    SEXP ns = PROTECT(R_FindNamespace(pkg_sexp));
    SEXP val = PROTECT(Rf_eval(Rf_install(name), ns));
    Rcpp::Function fn(val);
    UNPROTECT(3);
    return fn;
  }

  static Rcpp::Function as_bigint_fn(const std::string& bigint) {
    if (bigint == "integer64") return ns_fn("bit64", "as.integer64");
    const char* nm = (bigint == "integer") ? "as.integer"
                   : (bigint == "numeric") ? "as.numeric"
                                           : "as.character";
    SEXP val = PROTECT(Rf_eval(Rf_install(nm), R_BaseNamespace));
    Rcpp::Function fn(val);
    UNPROTECT(1);
    return fn;
  }
};

void noop_release_schema(struct ArrowSchema* s) { s->release = nullptr; }

void release_projected_struct_array(struct ArrowArray* a) {
  if (!a || !a->release) return;
  if (a->children) free(a->children);
  a->children = nullptr;
  a->release = nullptr;
}

// Reusable C ArrowArrayStream wrapper for collected, projected, and fallback streams.
struct VecArrowArrayStream {
  bool use_schema_stream = false;
  ArrowArrayStream schema_source{};
  ArrowSchema borrowed_schema{};
  std::vector<ArrowSchema*> child_schema_ptrs;
  std::vector<ArrowArray> batches;
  size_t idx = 0;
  std::string last_error;

  ~VecArrowArrayStream() {
    for (size_t i = idx; i < batches.size(); i++) {
      if (batches[i].release) batches[i].release(&batches[i]);
    }
    if (use_schema_stream && schema_source.release) {
      schema_source.release(&schema_source);
    }
  }

  static int get_schema(struct ArrowArrayStream* s, struct ArrowSchema* out) {
    auto* self = static_cast<VecArrowArrayStream*>(s->private_data);
    if (self->use_schema_stream) {
      int rc = self->schema_source.get_schema(&self->schema_source, out);
      if (rc != 0) {
        const char* err = self->schema_source.get_last_error(&self->schema_source);
        self->last_error = err ? err : "Failed to get schema";
      }
      return rc;
    }
    *out = self->borrowed_schema;
    if (!self->child_schema_ptrs.empty()) {
      out->children = self->child_schema_ptrs.data();
    }
    out->release = noop_release_schema;
    return 0;
  }

  static int get_next(struct ArrowArrayStream* s, struct ArrowArray* out) {
    auto* self = static_cast<VecArrowArrayStream*>(s->private_data);
    if (self->idx >= self->batches.size()) {
      out->release = nullptr;
      return 0;
    }
    *out = self->batches[self->idx];
    self->batches[self->idx].release = nullptr;
    self->idx++;
    return 0;
  }

  static const char* get_last_error(struct ArrowArrayStream* s) {
    auto* self = static_cast<VecArrowArrayStream*>(s->private_data);
    return self->last_error.empty() ? nullptr : self->last_error.c_str();
  }

  static void release(struct ArrowArrayStream* s) {
    delete static_cast<VecArrowArrayStream*>(s->private_data);
    s->private_data = nullptr;
    s->release = nullptr;
  }
};

SEXP wrap_vec_arrow_stream(VecArrowArrayStream* impl) {
  SEXP xptr = PROTECT(nanoarrow_array_stream_owning_xptr());
  auto* stream = nanoarrow_output_array_stream_from_xptr(xptr);
  stream->get_schema = VecArrowArrayStream::get_schema;
  stream->get_next = VecArrowArrayStream::get_next;
  stream->get_last_error = VecArrowArrayStream::get_last_error;
  stream->release = VecArrowArrayStream::release;
  stream->private_data = impl;
  UNPROTECT(1);
  return xptr;
}

void move_stream(SEXP xptr, ArrowArrayStream* dst) {
  ArrowArrayStream* src = nanoarrow_array_stream_from_xptr(xptr);
  std::memcpy(dst, src, sizeof(ArrowArrayStream));
  src->release = nullptr;
}

bool parse_decimal128_scale(const char* fmt, int* scale_out) {
  if (!fmt || fmt[0] != 'd' || fmt[1] != ':') return false;
  const char* comma = std::strchr(fmt + 2, ',');
  if (!comma) return false;
  const char* comma2 = std::strchr(comma + 1, ',');
  if (comma2 && std::atoi(comma2 + 1) != 128) return false;
  *scale_out = std::atoi(comma + 1);
  return true;
}

inline bool is_bit_set(const uint8_t* valid, int64_t idx) {
  return (valid[idx >> 3] >> (idx & 7)) & 1;
}

SEXP make_int64_sexp(const int64_t* vals,
                     const uint8_t* valid,
                     int64_t null_count,
                     int64_t offset,
                     int64_t len,
                     SEXP int64_class) {
  SEXP vec = PROTECT(Rf_allocVector(REALSXP, len));
  int64_t* dst = reinterpret_cast<int64_t*>(REAL(vec));
  if (null_count == 0 || valid == nullptr) {
    if (len > 0) std::memcpy(dst, vals + offset, static_cast<size_t>(len) * sizeof(int64_t));
  } else {
    for (int64_t i = 0; i < len; i++) {
      int64_t idx = offset + i;
      dst[i] = is_bit_set(valid, idx) ? vals[idx] : static_cast<int64_t>(1ULL << 63);
    }
  }
  Rf_setAttrib(vec, R_ClassSymbol, int64_class);
  UNPROTECT(1);
  return vec;
}

SEXP convert_list_int64_batches(const std::vector<ArrowArray*>& col_batches, int64_t total_rows) {
  SEXP int64_class = PROTECT(Rf_mkString("integer64"));
  SEXP out = PROTECT(Rf_allocVector(VECSXP, total_rows));
  int64_t pos = 0;
  for (ArrowArray* col : col_batches) {
    int64_t len = col->length;
    int64_t offset = col->offset;
    const uint8_t* valid = static_cast<const uint8_t*>(col->buffers[0]);
    const int32_t* offsets = static_cast<const int32_t*>(col->buffers[1]);
    ArrowArray* item = col->children[0];
    const uint8_t* item_valid = static_cast<const uint8_t*>(item->buffers[0]);
    const int64_t* item_vals = static_cast<const int64_t*>(item->buffers[1]);

    for (int64_t i = 0; i < len; i++) {
      int64_t idx = offset + i;
      if (col->null_count > 0 && valid != nullptr && !is_bit_set(valid, idx)) {
        SET_VECTOR_ELT(out, pos + i, R_NilValue);
      } else {
        int32_t start = offsets[idx];
        int32_t end = offsets[idx + 1];
        SET_VECTOR_ELT(out, pos + i, make_int64_sexp(
          item_vals, item_valid, item->null_count, item->offset + start, end - start, int64_class));
      }
    }
    pos += len;
  }
  UNPROTECT(2);
  return out;
}

bool schema_has_int64_under_list(const ArrowSchema* sch, bool under_list) {
  if (!sch || !sch->format) return false;
  if (std::strcmp(sch->format, "l") == 0) return under_list;
  bool is_list = std::strcmp(sch->format, "+l") == 0;
  for (int64_t j = 0; j < sch->n_children; j++) {
    if (schema_has_int64_under_list(sch->children[j], under_list || is_list)) return true;
  }
  return false;
}

SEXP sanitize_ptype_int64_under_list(SEXP ptype, const ArrowSchema* sch, bool under_list) {
  if (!sch || !sch->format) return ptype;
  if (std::strcmp(sch->format, "l") == 0) {
    return under_list ? Rf_allocVector(REALSXP, 0) : ptype;
  }
  if (!schema_has_int64_under_list(sch, under_list)) return ptype;
  if (std::strcmp(sch->format, "+s") == 0 && TYPEOF(ptype) == VECSXP &&
      XLENGTH(ptype) == sch->n_children) {
    SEXP cloned = PROTECT(Rf_shallow_duplicate(ptype));
    for (int64_t j = 0; j < sch->n_children; j++) {
      SET_VECTOR_ELT(cloned, j, sanitize_ptype_int64_under_list(VECTOR_ELT(cloned, j), sch->children[j], under_list));
    }
    UNPROTECT(1);
    return cloned;
  }
  if (std::strcmp(sch->format, "+l") == 0 && sch->n_children == 1) {
    SEXP sym_ptype = Rf_install("ptype");
    SEXP inner = Rf_getAttrib(ptype, sym_ptype);
    if (inner != R_NilValue) {
      SEXP cloned = PROTECT(Rf_shallow_duplicate(ptype));
      SEXP new_inner = PROTECT(sanitize_ptype_int64_under_list(inner, sch->children[0], true));
      Rf_setAttrib(cloned, sym_ptype, new_inner);
      UNPROTECT(2);
      return cloned;
    }
  }
  return ptype;
}

void repair_int64_value(SEXP parent, R_xlen_t idx, const ArrowArray* arr,
                        const ArrowSchema* sch, int64_t start, int64_t len,
                        SEXP int64_class);

void repair_int64_list_elements(SEXP r_list, R_xlen_t r_pos, const ArrowArray* arr,
                                const ArrowSchema* sch, int64_t start, int64_t len,
                                SEXP int64_class) {
  const uint8_t* valid = arr->null_count != 0 ? static_cast<const uint8_t*>(arr->buffers[0]) : nullptr;
  const int32_t* offsets = static_cast<const int32_t*>(arr->buffers[1]);
  for (int64_t i = 0; i < len; i++) {
    int64_t p = arr->offset + start + i;
    if (valid != nullptr && !is_bit_set(valid, p)) continue;
    if (VECTOR_ELT(r_list, r_pos + i) == R_NilValue) continue;
    repair_int64_value(r_list, r_pos + i, arr->children[0], sch->children[0],
                       offsets[p], offsets[p + 1] - offsets[p], int64_class);
  }
}

void repair_int64_value(SEXP parent, R_xlen_t idx, const ArrowArray* arr,
                        const ArrowSchema* sch, int64_t start, int64_t len,
                        SEXP int64_class) {
  const char* fmt = sch->format ? sch->format : "";
  SEXP obj = VECTOR_ELT(parent, idx);
  if (std::strcmp(fmt, "l") == 0) {
    SET_VECTOR_ELT(parent, idx, make_int64_sexp(
      static_cast<const int64_t*>(arr->buffers[1]),
      static_cast<const uint8_t*>(arr->buffers[0]),
      arr->null_count, arr->offset + start, len, int64_class));
    return;
  }
  if (!schema_has_int64_under_list(sch, true)) return;
  if (std::strcmp(fmt, "+s") == 0 && TYPEOF(obj) == VECSXP && XLENGTH(obj) == sch->n_children) {
    for (int64_t j = 0; j < sch->n_children; j++) {
      repair_int64_value(obj, j, arr->children[j], sch->children[j], arr->offset + start, len, int64_class);
    }
  } else if (std::strcmp(fmt, "+l") == 0 && TYPEOF(obj) == VECSXP && XLENGTH(obj) == len) {
    repair_int64_list_elements(obj, 0, arr, sch, start, len, int64_class);
  }
}

void repair_int64_column(SEXP parent, R_xlen_t idx,
                         const std::vector<const ArrowArray*>& arrs,
                         const std::vector<int64_t>& starts,
                         const std::vector<int64_t>& lens,
                         const ArrowSchema* sch, SEXP int64_class) {
  const char* fmt = sch->format ? sch->format : "";
  SEXP obj = VECTOR_ELT(parent, idx);
  if (std::strcmp(fmt, "+l") == 0 && TYPEOF(obj) == VECSXP) {
    R_xlen_t pos = 0;
    for (size_t b = 0; b < arrs.size(); b++) {
      repair_int64_list_elements(obj, pos, arrs[b], sch, starts[b], lens[b], int64_class);
      pos += lens[b];
    }
  } else if (std::strcmp(fmt, "+s") == 0 && TYPEOF(obj) == VECSXP && XLENGTH(obj) == sch->n_children) {
    for (int64_t j = 0; j < sch->n_children; j++) {
      if (!schema_has_int64_under_list(sch->children[j], false)) continue;
      std::vector<const ArrowArray*> child_arrs(arrs.size());
      std::vector<int64_t> child_starts(arrs.size());
      for (size_t b = 0; b < arrs.size(); b++) {
        child_arrs[b] = arrs[b]->children[j];
        child_starts[b] = arrs[b]->offset + starts[b];
      }
      repair_int64_column(obj, j, child_arrs, child_starts, lens, sch->children[j], int64_class);
    }
  }
}

SEXP compact_row_names(int64_t n) {
  if (n <= 0) return Rf_allocVector(INTSXP, 0);
  SEXP rn = Rf_allocVector(INTSXP, 2);
  INTEGER(rn)[0] = NA_INTEGER;
  INTEGER(rn)[1] = -static_cast<int>(n);
  return rn;
}

SEXP make_tbl_class() {
  SEXP cls = PROTECT(Rf_allocVector(STRSXP, 3));
  SET_STRING_ELT(cls, 0, Rf_mkChar("tbl_df"));
  SET_STRING_ELT(cls, 1, Rf_mkChar("tbl"));
  SET_STRING_ELT(cls, 2, Rf_mkChar("data.frame"));
  UNPROTECT(1);
  return cls;
}

std::string schema_extension_name(const ArrowSchema* sch) {
  if (!sch || !sch->metadata) return std::string();
  const char* md = sch->metadata;
  int32_t n_pairs = bqs_ipc::load_unaligned<int32_t>(md);
  md += sizeof(int32_t);
  for (int32_t i = 0; i < n_pairs; i++) {
    int32_t key_len = bqs_ipc::load_unaligned<int32_t>(md);
    md += sizeof(int32_t);
    const char* key = md;
    md += key_len;
    int32_t val_len = bqs_ipc::load_unaligned<int32_t>(md);
    md += sizeof(int32_t);
    const char* val = md;
    md += val_len;
    if (key_len == 20 && std::memcmp(key, "ARROW:extension:name", 20) == 0) {
      return std::string(val, static_cast<size_t>(val_len));
    }
  }
  return std::string();
}

inline const char* schema_format(const ArrowSchema* sch) {
  return (sch && sch->format) ? sch->format : "";
}

bool needs_postprocess(const ArrowSchema* sch, const std::string& ext) {
  const char* fmt = schema_format(sch);
  return std::strcmp(fmt, "l") == 0 || std::strcmp(fmt, "z") == 0 ||
         std::strcmp(fmt, "tsu:") == 0 || std::strcmp(fmt, "+s") == 0 ||
         std::strcmp(fmt, "+l") == 0 || ext == EXT_DATETIME || ext == EXT_GEOGRAPHY;
}

enum BigintMode { BIGINT_INTEGER, BIGINT_INTEGER64, BIGINT_NUMERIC, BIGINT_CHARACTER };

struct PostContext {
  BigintMode mode;
  std::vector<std::pair<SEXP, R_xlen_t>> int64_slots;
  SEXP sym_tzone;
  SEXP sym_ptype;
  SEXP utc;
  SEXP cls_tbl;
  SEXP cls_wk;
  SEXP cls_blob;
  SEXP raw0;
};

inline SEXP own_slot(SEXP parent, R_xlen_t i) {
  SEXP x = VECTOR_ELT(parent, i);
  if (MAYBE_SHARED(x)) {
    x = Rf_shallow_duplicate(x);
    SET_VECTOR_ELT(parent, i, x);
  }
  return x;
}

void walk_slot(SEXP parent, R_xlen_t i, const ArrowSchema* sch,
               const std::string& inherited_ext, PostContext& ctx) {
  SEXP col = VECTOR_ELT(parent, i);
  if (col == R_NilValue) return;
  const char* fmt = schema_format(sch);
  std::string ext = schema_extension_name(sch);
  if (ext.empty()) ext = inherited_ext;

  if (std::strcmp(fmt, "+s") == 0) {
    bool to_tibble = Rf_inherits(col, "data.frame") && !Rf_inherits(col, "tbl_df");
    bool walk_children = TYPEOF(col) == VECSXP && XLENGTH(col) == sch->n_children;
    if (to_tibble || walk_children) col = own_slot(parent, i);
    if (to_tibble) Rf_setAttrib(col, R_ClassSymbol, ctx.cls_tbl);
    if (walk_children) {
      for (int64_t j = 0; j < sch->n_children; j++) {
        const ArrowSchema* child = sch->children[j];
        if (needs_postprocess(child, schema_extension_name(child))) {
          walk_slot(col, j, child, std::string(), ctx);
        }
      }
    }
    return;
  }

  if (std::strcmp(fmt, "+l") == 0) {
    bool strip = Rf_inherits(col, "arrow_list") || Rf_inherits(col, "vctrs_list_of");
    const ArrowSchema* child = sch->n_children > 0 ? sch->children[0] : nullptr;
    std::string child_ext = schema_extension_name(child);
    if (child_ext.empty()) child_ext = ext;
    bool descend = child != nullptr && TYPEOF(col) == VECSXP && needs_postprocess(child, child_ext);
    if (strip || descend) col = own_slot(parent, i);
    if (strip) {
      Rf_setAttrib(col, R_ClassSymbol, R_NilValue);
      Rf_setAttrib(col, ctx.sym_ptype, R_NilValue);
    }
    if (descend) {
      R_xlen_t n = XLENGTH(col);
      for (R_xlen_t k = 0; k < n; k++) walk_slot(col, k, child, child_ext, ctx);
    }
    return;
  }

  if (std::strcmp(fmt, "l") == 0) {
    if (ctx.mode == BIGINT_INTEGER && TYPEOF(col) == INTSXP) return;
    if (ctx.mode == BIGINT_INTEGER64 && Rf_inherits(col, "integer64")) return;
    ctx.int64_slots.emplace_back(parent, i);
    return;
  }

  if (ext == EXT_DATETIME || std::strcmp(fmt, "tsu:") == 0) {
    col = own_slot(parent, i);
    Rf_setAttrib(col, ctx.sym_tzone, ctx.utc);
  } else if (ext == EXT_GEOGRAPHY) {
    col = own_slot(parent, i);
    Rf_setAttrib(col, R_ClassSymbol, ctx.cls_wk);
  } else if (std::strcmp(fmt, "z") == 0 && !Rf_inherits(col, "blob")) {
    col = own_slot(parent, i);
    Rf_setAttrib(col, R_ClassSymbol, ctx.cls_blob);
    Rf_setAttrib(col, ctx.sym_ptype, ctx.raw0);
  }
}

void convert_int64_slots(PostContext& ctx, const std::string& bigint) {
  if (ctx.int64_slots.empty()) return;
  Rcpp::Function as_bigint = RCallbacks::as_bigint_fn(bigint);

  const R_xlen_t gather_max_len = 65536;
  std::vector<std::pair<SEXP, R_xlen_t>> gathered;
  R_xlen_t total = 0;
  for (auto& slot : ctx.int64_slots) {
    SEXP v = VECTOR_ELT(slot.first, slot.second);
    if (ctx.int64_slots.size() > 1 && TYPEOF(v) == REALSXP && Rf_inherits(v, "integer64") &&
        XLENGTH(v) <= gather_max_len) {
      gathered.push_back(slot);
      total += XLENGTH(v);
    } else {
      SET_VECTOR_ELT(slot.first, slot.second, as_bigint(v));
    }
  }
  if (gathered.empty()) return;

  SEXP combined = PROTECT(Rf_allocVector(REALSXP, total));
  double* dst = REAL(combined);
  for (auto& slot : gathered) {
    SEXP v = VECTOR_ELT(slot.first, slot.second);
    R_xlen_t len = XLENGTH(v);
    if (len > 0) std::memcpy(dst, REAL(v), static_cast<size_t>(len) * sizeof(double));
    dst += len;
  }
  Rf_setAttrib(combined, R_ClassSymbol, Rf_mkString("integer64"));
  SEXP res = PROTECT(as_bigint(combined));
  if (XLENGTH(res) != total) {
    UNPROTECT(2);
    Rcpp::stop("Unexpected length returned by INT64 conversion.");
  }
  SEXPTYPE res_type = TYPEOF(res);
  R_xlen_t pos = 0;
  for (auto& slot : gathered) {
    R_xlen_t len = XLENGTH(VECTOR_ELT(slot.first, slot.second));
    SEXP piece = PROTECT(Rf_allocVector(res_type, len));
    switch (res_type) {
      case INTSXP:
        if (len > 0) std::memcpy(INTEGER(piece), INTEGER(res) + pos, static_cast<size_t>(len) * sizeof(int));
        break;
      case REALSXP:
        if (len > 0) std::memcpy(REAL(piece), REAL(res) + pos, static_cast<size_t>(len) * sizeof(double));
        break;
      case STRSXP:
        for (R_xlen_t k = 0; k < len; k++) SET_STRING_ELT(piece, k, STRING_ELT(res, pos + k));
        break;
      case LGLSXP:
        if (len > 0) std::memcpy(LOGICAL(piece), LOGICAL(res) + pos, static_cast<size_t>(len) * sizeof(int));
        break;
      default:
        for (R_xlen_t k = 0; k < len; k++) SET_VECTOR_ELT(piece, k, VECTOR_ELT(res, pos + k));
        break;
    }
    Rf_copyMostAttrib(res, piece);
    SET_VECTOR_ELT(slot.first, slot.second, piece);
    UNPROTECT(1);
    pos += len;
  }
  UNPROTECT(2);
}

void schema_flags(const ArrowSchema* sch, const std::string& inherited_ext, bool* geography, bool* bytes) {
  if (!sch) return;
  std::string ext = schema_extension_name(sch);
  if (ext.empty()) ext = inherited_ext;
  if (ext == EXT_GEOGRAPHY) *geography = true;
  if (std::strcmp(schema_format(sch), "z") == 0) *bytes = true;
  bool is_list = std::strcmp(schema_format(sch), "+l") == 0;
  for (int64_t j = 0; j < sch->n_children; j++) {
    schema_flags(sch->children[j], is_list ? ext : std::string(), geography, bytes);
  }
}

enum DirectColKind {
  DCOL_FALLBACK = 0,
  DCOL_INT64_TO_INT32,
  DCOL_INT64_TO_INT64,
  DCOL_INT64_TO_DBL,
  DCOL_INT32,
  DCOL_FLOAT64,
  DCOL_FLOAT32,
  DCOL_BOOL,
  DCOL_DATE32,
  DCOL_TIME64_US,
  DCOL_TIMESTAMP_US,
  DCOL_DECIMAL128,
  DCOL_UTF8,
  DCOL_LARGE_UTF8
};

struct DecodedBatchSlice {
  const bqs_ipc::DecodedBatch* batch;
  int64_t row_len;
  int64_t dst_offset;
};

struct DirectColSpec {
  int col_idx = 0;
  DirectColKind kind = DCOL_FALLBACK;
  size_t node_idx = 0;
  size_t buf_idx = 0;
  double dec_scale_mult = 1.0;
  bool is_geography = false;
  void* out_ptr = nullptr;
};

struct NumericSliceTask {
  const DirectColSpec* col = nullptr;
  const DecodedBatchSlice* slices = nullptr;
  size_t b_start = 0;
  size_t b_end = 0;
  uint64_t na_real_bits = 0;
  bool overflow = false;
};

template <typename T>
inline void copy_exact_slice(const uint8_t* raw,
                             const uint8_t* valid,
                             int64_t len,
                             T* dst,
                             T na_val) {
  if (!valid) {
    std::memcpy(dst, raw, static_cast<size_t>(len) * sizeof(T));
  } else {
    for (int64_t i = 0; i < len; i++) {
      dst[i] = is_bit_set(valid, i) ? bqs_ipc::load_unaligned<T>(raw + i * sizeof(T)) : na_val;
    }
  }
}

template <typename SrcT, typename ConvFn>
inline void convert_to_double_slice(const uint8_t* raw,
                                    const uint8_t* valid,
                                    int64_t len,
                                    double* dst,
                                    uint64_t na_real_bits,
                                    ConvFn&& conv) {
  if (!valid) {
    for (int64_t i = 0; i < len; i++) {
      dst[i] = conv(bqs_ipc::load_unaligned<SrcT>(raw + i * sizeof(SrcT)));
    }
  } else {
    for (int64_t i = 0; i < len; i++) {
      if (is_bit_set(valid, i)) {
        dst[i] = conv(bqs_ipc::load_unaligned<SrcT>(raw + i * sizeof(SrcT)));
      } else {
        std::memcpy(dst + i, &na_real_bits, sizeof(uint64_t));
      }
    }
  }
}

void run_numeric_slice_task(NumericSliceTask& task) {
  const DirectColSpec& c = *task.col;
  const size_t n_idx = c.node_idx;
  const size_t b_idx = c.buf_idx;

  for (size_t s = task.b_start; s < task.b_end; s++) {
    const DecodedBatchSlice& sl = task.slices[s];
    const bqs_ipc::DecodedBatch& batch = *sl.batch;
    const int64_t len = sl.row_len;
    if (len <= 0) continue;
    const int64_t dst_off = sl.dst_offset;

    const bqs_ipc::FieldNodeDesc& nd = batch.nodes[n_idx];
    const bqs_ipc::BufferDesc& vbuf = batch.buffers[b_idx];
    const bqs_ipc::BufferDesc& dbuf = batch.buffers[b_idx + 1];
    const uint8_t* valid = (nd.null_count > 0 && vbuf.length > 0) ? (batch.body_ptr + vbuf.offset) : nullptr;
    const uint8_t* raw = (dbuf.length > 0) ? (batch.body_ptr + dbuf.offset) : nullptr;

    switch (c.kind) {
      case DCOL_FLOAT64:
        copy_exact_slice<uint64_t>(
          raw, valid, len, static_cast<uint64_t*>(c.out_ptr) + dst_off, task.na_real_bits);
        break;
      case DCOL_FLOAT32:
        convert_to_double_slice<float>(
          raw, valid, len, static_cast<double*>(c.out_ptr) + dst_off, task.na_real_bits,
          [](float v) { return static_cast<double>(v); });
        break;
      case DCOL_INT64_TO_INT32: {
        int* dst = static_cast<int*>(c.out_ptr) + dst_off;
        bool local_ovf = false;
        for (int64_t i = 0; i < len; i++) {
          if (valid && !is_bit_set(valid, i)) {
            dst[i] = NA_INTEGER;
          } else {
            int64_t v = bqs_ipc::load_unaligned<int64_t>(raw + i * sizeof(int64_t));
            if (v < -2147483647LL || v > 2147483647LL) {
              dst[i] = NA_INTEGER;
              local_ovf = true;
            } else {
              dst[i] = static_cast<int>(v);
            }
          }
        }
        if (local_ovf) task.overflow = true;
        break;
      }
      case DCOL_INT64_TO_INT64:
        copy_exact_slice<int64_t>(
          raw, valid, len, static_cast<int64_t*>(c.out_ptr) + dst_off, static_cast<int64_t>(1ULL << 63));
        break;
      case DCOL_INT64_TO_DBL:
        convert_to_double_slice<int64_t>(
          raw, valid, len, static_cast<double*>(c.out_ptr) + dst_off, task.na_real_bits,
          [](int64_t v) { return static_cast<double>(v); });
        break;
      case DCOL_INT32:
        copy_exact_slice<int32_t>(
          raw, valid, len, static_cast<int32_t*>(c.out_ptr) + dst_off, NA_INTEGER);
        break;
      case DCOL_BOOL: {
        int* dst = static_cast<int*>(c.out_ptr) + dst_off;
        if (!valid) {
          int64_t i = 0;
          for (; i + 8 <= len; i += 8) {
            uint8_t byte = raw[i >> 3];
            for (int b = 0; b < 8; b++) dst[i + b] = (byte >> b) & 1;
          }
          for (; i < len; i++) dst[i] = (raw[i >> 3] >> (i & 7)) & 1;
        } else {
          for (int64_t i = 0; i < len; i++) {
            dst[i] = is_bit_set(valid, i) ? ((raw[i >> 3] >> (i & 7)) & 1) : NA_LOGICAL;
          }
        }
        break;
      }
      case DCOL_DATE32:
        convert_to_double_slice<int32_t>(
          raw, valid, len, static_cast<double*>(c.out_ptr) + dst_off, task.na_real_bits,
          [](int32_t v) { return static_cast<double>(v); });
        break;
      case DCOL_TIME64_US:
      case DCOL_TIMESTAMP_US:
        convert_to_double_slice<int64_t>(
          raw, valid, len, static_cast<double*>(c.out_ptr) + dst_off, task.na_real_bits,
          [](int64_t v) { return static_cast<double>(v) * 1e-6; });
        break;
      case DCOL_DECIMAL128: {
#if defined(__SIZEOF_INT128__)
        const double mult = c.dec_scale_mult;
        convert_to_double_slice<__int128>(
          raw, valid, len, static_cast<double*>(c.out_ptr) + dst_off, task.na_real_bits,
          [mult](__int128 v) { return static_cast<double>(v) * mult; });
#endif
        break;
      }
      default:
        break;
    }
  }
}

inline uint64_t fast_hash_str(const char* p, int64_t len) {
  if (len <= 0) return 0x9e3779b97f4a7c15ULL;
  uint64_t h = static_cast<uint64_t>(len) * 0x9e3779b97f4a7c15ULL;
  if (len <= 8) {
    uint64_t v = 0;
    if (len >= 4) {
      uint32_t a = bqs_ipc::load_unaligned<uint32_t>(p);
      uint32_t b = bqs_ipc::load_unaligned<uint32_t>(p + len - 4);
      v = (static_cast<uint64_t>(a) << 32) | b;
    } else {
      v = (static_cast<uint64_t>(static_cast<uint8_t>(p[0])) << 16) |
          (static_cast<uint64_t>(static_cast<uint8_t>(p[len >> 1])) << 8) |
          static_cast<uint64_t>(static_cast<uint8_t>(p[len - 1]));
    }
    h ^= v;
    h *= 0xbf58476d1ce4e5b9ULL;
    return h ^ (h >> 32);
  }
  int64_t i = 0;
  for (; i + 8 <= len; i += 8) {
    h ^= bqs_ipc::load_unaligned<uint64_t>(p + i);
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= (h >> 31);
  }
  h ^= bqs_ipc::load_unaligned<uint64_t>(p + len - 8);
  h *= 0x94d049bb133111ebULL;
  return h ^ (h >> 32);
}

struct StrDictEntry {
  uint64_t hash;
  const char* ptr;
  int32_t len;
};

struct StringChunkTask {
  const DirectColSpec* col = nullptr;
  const DecodedBatchSlice* slices = nullptr;
  size_t b_start = 0;
  size_t b_end = 0;
  int64_t dst_offset = 0;
  int64_t total_rows = 0;
  bool use_dict = true;
  bool has_nulls = false;
  std::vector<StrDictEntry> entries;
  std::vector<uint32_t> codes;
};

template <typename OffsetT>
void run_string_chunk_task_t(StringChunkTask& task) {
  const DirectColSpec& c = *task.col;
  const size_t n_idx = c.node_idx;
  const size_t b_idx = c.buf_idx;

  task.codes.resize(static_cast<size_t>(task.total_rows));
  uint32_t* codes = task.codes.data();

  static constexpr uint32_t EMPTY_SLOT = 0xFFFFFFFFu;
  uint32_t cap = 4096u;
  std::vector<uint32_t> slots(cap, EMPTY_SLOT);
  uint32_t mask = cap - 1u;
  task.entries.reserve(1024);

  auto rehash = [&]() {
    cap <<= 1;
    mask = cap - 1u;
    slots.assign(cap, EMPTY_SLOT);
    for (uint32_t id = 0; id < static_cast<uint32_t>(task.entries.size()); id++) {
      uint32_t pos = static_cast<uint32_t>(task.entries[id].hash) & mask;
      while (slots[pos] != EMPTY_SLOT) pos = (pos + 1u) & mask;
      slots[pos] = id;
    }
  };

  auto intern_local = [&](const char* ptr, int32_t slen) -> uint32_t {
    uint64_t h = fast_hash_str(ptr, slen);
    uint32_t pos = static_cast<uint32_t>(h) & mask;
    while (true) {
      uint32_t id = slots[pos];
      if (id == EMPTY_SLOT) {
        uint32_t new_id = static_cast<uint32_t>(task.entries.size());
        task.entries.push_back({h, ptr, slen});
        slots[pos] = new_id;
        if (task.entries.size() * 2u >= cap) rehash();
        return new_id;
      }
      const StrDictEntry& e = task.entries[id];
      if (e.hash == h && e.len == slen && (slen == 0 || std::memcmp(e.ptr, ptr, static_cast<size_t>(slen)) == 0)) {
        return id;
      }
      pos = (pos + 1u) & mask;
    }
  };

  auto bail_dict = [&]() {
    task.use_dict = false;
    std::vector<StrDictEntry>().swap(task.entries);
    std::vector<uint32_t>().swap(task.codes);
  };

  int64_t out_pos = 0;
  bool checked_cardinality = false;

  for (size_t s = task.b_start; s < task.b_end; s++) {
    const DecodedBatchSlice& sl = task.slices[s];
    const bqs_ipc::DecodedBatch& batch = *sl.batch;
    const int64_t len = sl.row_len;
    if (len <= 0) continue;

    const bqs_ipc::FieldNodeDesc& nd = batch.nodes[n_idx];
    const bqs_ipc::BufferDesc& vbuf = batch.buffers[b_idx];
    const bqs_ipc::BufferDesc& obuf = batch.buffers[b_idx + 1];
    const bqs_ipc::BufferDesc& dbuf = batch.buffers[b_idx + 2];

    const uint8_t* valid = (nd.null_count > 0 && vbuf.length > 0) ? (batch.body_ptr + vbuf.offset) : nullptr;
    const char* data = (dbuf.length > 0) ? reinterpret_cast<const char*>(batch.body_ptr + dbuf.offset) : "";
    const uint8_t* op = batch.body_ptr + obuf.offset;
    if (valid) task.has_nulls = true;

    for (int64_t i = 0; i < len; i++) {
      if (valid && !is_bit_set(valid, i)) {
        codes[out_pos++] = EMPTY_SLOT;
      } else {
        OffsetT start = bqs_ipc::load_unaligned<OffsetT>(op + i * static_cast<int64_t>(sizeof(OffsetT)));
        OffsetT slen = bqs_ipc::load_unaligned<OffsetT>(op + (i + 1) * static_cast<int64_t>(sizeof(OffsetT))) - start;
        if constexpr (sizeof(OffsetT) == 8) {
          if (slen > 2147483647LL) {
            bail_dict();
            return;
          }
        }
        codes[out_pos++] = intern_local(data + start, static_cast<int32_t>(slen));
      }
      if (!checked_cardinality && out_pos >= 2048) {
        checked_cardinality = true;
        if (static_cast<int64_t>(task.entries.size()) > (out_pos * 3) / 4) {
          bail_dict();
          return;
        }
      }
    }
  }
}

void run_string_chunk_task(StringChunkTask& task) {
  if (task.col->kind == DCOL_LARGE_UTF8) run_string_chunk_task_t<int64_t>(task);
  else run_string_chunk_task_t<int32_t>(task);
}

template <typename OffsetT>
void fill_high_card_strings(SEXP str_vec,
                            const DirectColSpec& sp,
                            const std::vector<DecodedBatchSlice>& slices,
                            size_t b_start,
                            size_t b_end) {
  for (size_t s = b_start; s < b_end; s++) {
    const DecodedBatchSlice& sl = slices[s];
    const bqs_ipc::DecodedBatch& batch = *sl.batch;
    const int64_t len = sl.row_len;
    const int64_t dst_off = sl.dst_offset;
    const bqs_ipc::FieldNodeDesc& nd = batch.nodes[sp.node_idx];
    const bqs_ipc::BufferDesc& vbuf = batch.buffers[sp.buf_idx];
    const bqs_ipc::BufferDesc& obuf = batch.buffers[sp.buf_idx + 1];
    const bqs_ipc::BufferDesc& dbuf = batch.buffers[sp.buf_idx + 2];
    const uint8_t* valid = (nd.null_count > 0 && vbuf.length > 0) ? (batch.body_ptr + vbuf.offset) : nullptr;
    const char* data = (dbuf.length > 0) ? reinterpret_cast<const char*>(batch.body_ptr + dbuf.offset) : "";
    const uint8_t* op = batch.body_ptr + obuf.offset;

    for (int64_t i = 0; i < len; i++) {
      if (valid && !is_bit_set(valid, i)) {
        SET_STRING_ELT(str_vec, dst_off + i, NA_STRING);
      } else {
        OffsetT st = bqs_ipc::load_unaligned<OffsetT>(op + i * static_cast<int64_t>(sizeof(OffsetT)));
        OffsetT slen = bqs_ipc::load_unaligned<OffsetT>(op + (i + 1) * static_cast<int64_t>(sizeof(OffsetT))) - st;
        if (slen > 0) {
          SET_STRING_ELT(str_vec, dst_off + i, Rf_mkCharLenCE(data + st, static_cast<int>(slen), CE_UTF8));
        }
      }
    }
  }
}

struct GlobalCharEntry {
  uint64_t hash;
  const char* ptr;
  int32_t len;
  SEXP charsxp;
};

class GlobalCharTable {
public:
  explicit GlobalCharTable(size_t reserve_hint = 1024) {
    size_t c = 2048;
    while (c < reserve_hint * 2) c <<= 1;
    cap_ = c;
    mask_ = cap_ - 1;
    slots_.assign(cap_, 0xFFFFFFFFu);
    entries_.reserve(reserve_hint);
  }

  SEXP intern(uint64_t h, const char* ptr, int32_t len, SEXP guard, R_xlen_t* guard_pos) {
    if (len == 0) return R_BlankString;
    size_t pos = static_cast<size_t>(h) & mask_;
    while (true) {
      uint32_t id = slots_[pos];
      if (id == 0xFFFFFFFFu) {
        SEXP cs = Rf_mkCharLenCE(ptr, len, CE_UTF8);
        SET_STRING_ELT(guard, (*guard_pos)++, cs);
        uint32_t new_id = static_cast<uint32_t>(entries_.size());
        entries_.push_back({h, ptr, len, cs});
        slots_[pos] = new_id;
        if (entries_.size() * 2 >= cap_) rehash();
        return cs;
      }
      const GlobalCharEntry& e = entries_[id];
      if (e.hash == h && e.len == len && std::memcmp(e.ptr, ptr, static_cast<size_t>(len)) == 0) {
        return e.charsxp;
      }
      pos = (pos + 1) & mask_;
    }
  }

private:
  size_t cap_;
  size_t mask_;
  std::vector<uint32_t> slots_;
  std::vector<GlobalCharEntry> entries_;

  void rehash() {
    cap_ <<= 1;
    mask_ = cap_ - 1;
    slots_.assign(cap_, 0xFFFFFFFFu);
    for (uint32_t id = 0; id < static_cast<uint32_t>(entries_.size()); id++) {
      size_t pos = static_cast<size_t>(entries_[id].hash) & mask_;
      while (slots_[pos] != 0xFFFFFFFFu) pos = (pos + 1) & mask_;
      slots_[pos] = id;
    }
  }
};

std::vector<std::pair<size_t, size_t>> group_slices_by_rows(
    const std::vector<DecodedBatchSlice>& slices, int64_t target_rows) {
  std::vector<std::pair<size_t, size_t>> groups;
  size_t gs = 0;
  int64_t acc = 0;
  for (size_t s = 0; s < slices.size(); s++) {
    acc += slices[s].row_len;
    if (acc >= target_rows || s + 1 == slices.size()) {
      groups.emplace_back(gs, s + 1);
      gs = s + 1;
      acc = 0;
    }
  }
  return groups;
}

} // namespace

// [[Rcpp::export(rng=false)]]
SEXP bqs_convert_stream(SEXP stream_xptr, SEXP ptype_df, std::string bigint) {
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
  std::vector<int> list_int64_cols;
  std::vector<int> repair_cols;
  std::vector<int> other_cols;

  for (int64_t j = 0; j < n_cols; j++) {
    ArrowSchema* ch = schema.children[j];
    const char* fmt = ch->format;
    int scale = 0;
    if (
#if defined(__SIZEOF_INT128__)
        parse_decimal128_scale(fmt, &scale)
#else
        false
#endif
    ) {
      dec_cols.push_back(static_cast<int>(j));
      dec_scales.push_back(std::pow(10.0, -scale));
    } else if (bigint == "integer" && fmt && std::strcmp(fmt, "l") == 0) {
      int32_cols.push_back(static_cast<int>(j));
    } else if (fmt && std::strcmp(fmt, "+l") == 0 && ch->n_children == 1 &&
               ch->children[0] && ch->children[0]->format &&
               std::strcmp(ch->children[0]->format, "l") == 0) {
      list_int64_cols.push_back(static_cast<int>(j));
    } else {
      if (schema_has_int64_under_list(ch, false)) repair_cols.push_back(static_cast<int>(j));
      other_cols.push_back(static_cast<int>(j));
    }
  }

  if (dec_cols.empty() && int32_cols.empty() && list_int64_cols.empty() && repair_cols.empty()) {
    schema.release(&schema);
    Rcpp::Function na_convert = RCallbacks::ns_fn("nanoarrow", "convert_array_stream");
    return na_convert(stream_xptr, ptype_df);
  }

  std::vector<ArrowArray> batches;
  int64_t total_rows = 0;
  while (true) {
    ArrowArray arr;
    arr.release = nullptr;
    if (stream->get_next(stream, &arr) != 0) {
      const char* err = stream->get_last_error ? stream->get_last_error(stream) : "Failed to read next batch from ArrowArrayStream";
      std::string err_str = err ? err : "Failed to read next batch from ArrowArrayStream";
      for (auto& b : batches) b.release(&b);
      schema.release(&schema);
      Rcpp::stop(err_str.c_str());
    }
    if (arr.release == nullptr) break;
    total_rows += arr.length;
    batches.push_back(arr);
  }

  Rcpp::List out_cols(n_cols);

#if defined(__SIZEOF_INT128__)
  for (size_t k = 0; k < dec_cols.size(); k++) {
    int col_idx = dec_cols[k];
    double scale_mult = dec_scales[k];
    Rcpp::NumericVector col_vec(total_rows);
    double* out_ptr = REAL(col_vec);
    int64_t pos = 0;
    for (auto& b : batches) {
      ArrowArray* child = b.children[col_idx];
      int64_t len = child->length;
      int64_t offset = child->offset;
      const uint8_t* valid = child->null_count != 0 ? static_cast<const uint8_t*>(child->buffers[0]) : nullptr;
      const uint8_t* raw_vals = static_cast<const uint8_t*>(child->buffers[1]);
      for (int64_t i = 0; i < len; i++) {
        int64_t idx = offset + i;
        if (!valid || is_bit_set(valid, idx)) {
          __extension__ __int128 v = bqs_ipc::load_unaligned<__int128>(raw_vals + idx * 16);
          out_ptr[pos + i] = static_cast<double>(v) * scale_mult;
        } else {
          out_ptr[pos + i] = NA_REAL;
        }
      }
      pos += len;
    }
    out_cols[col_idx] = col_vec;
  }
#endif

  bool any_int_overflow = false;
  for (int col_idx : int32_cols) {
    Rcpp::IntegerVector col_vec(total_rows);
    int* out_ptr = INTEGER(col_vec);
    int64_t pos = 0;
    for (auto& b : batches) {
      ArrowArray* child = b.children[col_idx];
      int64_t len = child->length;
      int64_t offset = child->offset;
      const uint8_t* valid = child->null_count != 0 ? static_cast<const uint8_t*>(child->buffers[0]) : nullptr;
      const int64_t* vals = static_cast<const int64_t*>(child->buffers[1]);
      for (int64_t i = 0; i < len; i++) {
        int64_t idx = offset + i;
        if (valid && !is_bit_set(valid, idx)) {
          out_ptr[pos + i] = NA_INTEGER;
        } else {
          int64_t v = vals[idx];
          if (v < -2147483647LL || v > 2147483647LL) {
            out_ptr[pos + i] = NA_INTEGER;
            any_int_overflow = true;
          } else {
            out_ptr[pos + i] = static_cast<int>(v);
          }
        }
      }
      pos += len;
    }
    out_cols[col_idx] = col_vec;
  }
  if (any_int_overflow) Rf_warning("NAs produced by integer overflow");

  for (int col_idx : list_int64_cols) {
    std::vector<ArrowArray*> col_batches(batches.size());
    for (size_t b = 0; b < batches.size(); b++) col_batches[b] = batches[b].children[col_idx];
    out_cols[col_idx] = convert_list_int64_batches(col_batches, total_rows);
  }

  if (!other_cols.empty()) {
    Rcpp::List full_ptype(ptype_df);
    Rcpp::List sub_ptype(other_cols.size());
    Rcpp::CharacterVector full_names = full_ptype.names();
    Rcpp::CharacterVector sub_names(other_cols.size());
    for (size_t j = 0; j < other_cols.size(); j++) {
      int col_idx = other_cols[j];
      SEXP col_ptype = full_ptype[col_idx];
      if (schema_has_int64_under_list(schema.children[col_idx], false)) {
        col_ptype = sanitize_ptype_int64_under_list(col_ptype, schema.children[col_idx], false);
      }
      sub_ptype[j] = col_ptype;
      sub_names[j] = full_names[col_idx];
    }
    sub_ptype.attr("names") = sub_names;
    sub_ptype.attr("class") = "data.frame";
    sub_ptype.attr("row.names") = Rcpp::IntegerVector::create(NA_INTEGER, 0);

    auto* sub_stream_impl = new VecArrowArrayStream();
    sub_stream_impl->child_schema_ptrs.resize(other_cols.size());
    for (size_t j = 0; j < other_cols.size(); j++) {
      sub_stream_impl->child_schema_ptrs[j] = schema.children[other_cols[j]];
    }
    sub_stream_impl->borrowed_schema = schema;
    sub_stream_impl->borrowed_schema.n_children = static_cast<int64_t>(other_cols.size());
    sub_stream_impl->borrowed_schema.release = noop_release_schema;
    sub_stream_impl->batches.resize(batches.size());
    for (size_t b = 0; b < batches.size(); b++) {
      ArrowArray& src = batches[b];
      ArrowArray& proj = sub_stream_impl->batches[b];
      size_t k = other_cols.size();
      auto** children = static_cast<ArrowArray**>(malloc(k * sizeof(ArrowArray*)));
      for (size_t j = 0; j < k; j++) children[j] = src.children[other_cols[j]];
      proj.length = src.length;
      proj.null_count = src.null_count;
      proj.offset = src.offset;
      proj.n_buffers = src.n_buffers;
      proj.n_children = static_cast<int64_t>(k);
      proj.buffers = src.buffers;
      proj.children = children;
      proj.dictionary = nullptr;
      proj.private_data = nullptr;
      proj.release = release_projected_struct_array;
    }

    SEXP sub_stream_xptr = PROTECT(wrap_vec_arrow_stream(sub_stream_impl));
    Rcpp::Function convert_fn = repair_cols.empty()
      ? RCallbacks::ns_fn("nanoarrow", "convert_array_stream")
      : RCallbacks::ns_fn("bigrquerystorage", "convert_stream_no_warnings");
    SEXP sub_res_sexp = PROTECT(convert_fn(sub_stream_xptr, sub_ptype, static_cast<double>(total_rows), R_PosInf));
    Rcpp::List sub_res(sub_res_sexp);
    UNPROTECT(2);

    for (size_t j = 0; j < other_cols.size(); j++) {
      out_cols[other_cols[j]] = sub_res[j];
    }

    if (!repair_cols.empty()) {
      SEXP int64_class = PROTECT(Rf_mkString("integer64"));
      std::vector<const ArrowArray*> arrs(batches.size());
      std::vector<int64_t> starts(batches.size());
      std::vector<int64_t> lens(batches.size());
      for (int col_idx : repair_cols) {
        for (size_t b = 0; b < batches.size(); b++) {
          arrs[b] = batches[b].children[col_idx];
          starts[b] = batches[b].offset;
          lens[b] = batches[b].length;
        }
        repair_int64_column(out_cols, col_idx, arrs, starts, lens, schema.children[col_idx], int64_class);
      }
      UNPROTECT(1);
    }
  }

  for (auto& b : batches) b.release(&b);
  schema.release(&schema);

  Rcpp::List full_ptype(ptype_df);
  out_cols.attr("names") = full_ptype.names();
  out_cols.attr("class") = "data.frame";
  out_cols.attr("row.names") = compact_row_names(total_rows);
  return out_cols;
}

// [[Rcpp::export(rng=false)]]
Rcpp::LogicalVector bqs_schema_flags(SEXP schema_xptr) {
  struct ArrowSchema* schema = nanoarrow_schema_from_xptr(schema_xptr);
  bool geography = false, bytes = false;
  schema_flags(schema, std::string(), &geography, &bytes);
  return Rcpp::LogicalVector::create(
    Rcpp::Named("geography") = geography,
    Rcpp::Named("bytes") = bytes
  );
}

// [[Rcpp::export(rng=false)]]
SEXP bqs_postprocess(SEXP df, SEXP schema_xptr, std::string bigint) {
  struct ArrowSchema* schema = nanoarrow_schema_from_xptr(schema_xptr);

  PostContext ctx;
  ctx.mode = (bigint == "integer")   ? BIGINT_INTEGER
           : (bigint == "integer64") ? BIGINT_INTEGER64
           : (bigint == "numeric")   ? BIGINT_NUMERIC
                                     : BIGINT_CHARACTER;
  ctx.sym_tzone = Rf_install("tzone");
  ctx.sym_ptype = Rf_install("ptype");
  ctx.utc = PROTECT(Rf_mkString("UTC"));
  ctx.cls_tbl = PROTECT(make_tbl_class());
  ctx.cls_wk = PROTECT(Rf_allocVector(STRSXP, 2));
  SET_STRING_ELT(ctx.cls_wk, 0, Rf_mkChar("wk_wkt"));
  SET_STRING_ELT(ctx.cls_wk, 1, Rf_mkChar("wk_vctr"));
  ctx.cls_blob = PROTECT(Rf_allocVector(STRSXP, 4));
  SET_STRING_ELT(ctx.cls_blob, 0, Rf_mkChar("blob"));
  SET_STRING_ELT(ctx.cls_blob, 1, Rf_mkChar("vctrs_list_of"));
  SET_STRING_ELT(ctx.cls_blob, 2, Rf_mkChar("vctrs_vctr"));
  SET_STRING_ELT(ctx.cls_blob, 3, Rf_mkChar("list"));
  ctx.raw0 = PROTECT(Rf_allocVector(RAWSXP, 0));

  SEXP holder = PROTECT(Rf_allocVector(VECSXP, 1));
  SET_VECTOR_ELT(holder, 0, df);
  walk_slot(holder, 0, schema, std::string(), ctx);
  convert_int64_slots(ctx, bigint);
  SEXP out = VECTOR_ELT(holder, 0);
  UNPROTECT(6);
  return out;
}

// [[Rcpp::export(rng=false)]]
SEXP bqs_collect_streams(Rcpp::List readers, SEXP schema_reader, int n_threads) {
  size_t n_readers = static_cast<size_t>(readers.size());
  std::vector<ArrowArrayStream> streams(n_readers);
  for (size_t i = 0; i < n_readers; i++) {
    move_stream(readers[static_cast<R_xlen_t>(i)], &streams[i]);
  }

  std::vector<std::vector<ArrowArray>> batches(n_readers);
  std::vector<std::string> errors(n_readers);

  int resolved = bqs_resolve_threads(n_threads, 16u);
  bqs_parallel_for(n_readers, resolved, [&](int, size_t idx) -> bool {
    ArrowArrayStream* s = &streams[idx];
    while (true) {
      ArrowArray arr;
      arr.release = nullptr;
      if (s->get_next(s, &arr) != 0) {
        const char* err = s->get_last_error(s);
        errors[idx] = std::string("Failed to decode Arrow record batch: ") + (err ? err : "unknown error");
        return false;
      }
      if (arr.release == nullptr) break;
      batches[idx].push_back(arr);
    }
    return true;
  });

  for (auto& s : streams) {
    if (s.release) s.release(&s);
  }

  std::string first_error;
  for (const auto& e : errors) {
    if (!e.empty()) {
      first_error = e;
      break;
    }
  }

  auto* collected = new VecArrowArrayStream();
  collected->use_schema_stream = true;
  move_stream(schema_reader, &collected->schema_source);
  size_t n_batches = 0;
  for (const auto& b : batches) n_batches += b.size();
  collected->batches.reserve(n_batches);
  for (auto& b : batches) {
    collected->batches.insert(collected->batches.end(), b.begin(), b.end());
  }

  SEXP out = wrap_vec_arrow_stream(collected);
  if (!first_error.empty()) Rcpp::stop(first_error);
  return out;
}

SEXP bqs_parse_schema_ipc(const std::string& schema_bytes) {
  Rcpp::RawVector schema_raw(schema_bytes.size());
  if (!schema_bytes.empty()) {
    std::memcpy(RAW(schema_raw), schema_bytes.data(), schema_bytes.size());
  }
  Rcpp::Function read_nanoarrow = RCallbacks::ns_fn("nanoarrow", "read_nanoarrow");
  SEXP schema_reader_xptr = PROTECT(read_nanoarrow(schema_raw));
  struct ArrowArrayStream* sch_stream = nanoarrow_array_stream_from_xptr(schema_reader_xptr);

  SEXP schema_xptr = PROTECT(nanoarrow_schema_owning_xptr());
  struct ArrowSchema* out_schema = nanoarrow_output_schema_from_xptr(schema_xptr);
  if (sch_stream->get_schema(sch_stream, out_schema) != 0) {
    const char* err = sch_stream->get_last_error ? sch_stream->get_last_error(sch_stream) : "Failed to parse ArrowSchema";
    UNPROTECT(2);
    Rcpp::stop(err ? err : "Failed to parse ArrowSchema");
  }
  UNPROTECT(2);
  return schema_xptr;
}

SEXP bqs_convert_decoded_streams(
    SEXP schema_xptr,
    std::vector<std::vector<bqs_ipc::DecodedBatch>>* stream_batches,
    int64_t n_max,
    const std::string& bigint,
    bool as_data_frame,
    int n_threads) {
  struct ArrowSchema* schema = nanoarrow_schema_from_xptr(schema_xptr);

  bool has_geo = false, has_bytes = false;
  schema_flags(schema, std::string(), &has_geo, &has_bytes);
  if (has_geo || has_bytes) {
    Rcpp::Function check_pkg = RCallbacks::ns_fn("bigrquerystorage", "bqs_check_namespace");
    if (has_geo) check_pkg("wk", "GEOGRAPHY");
    if (has_bytes) check_pkg("blob", "BYTES");
  }

  std::vector<DecodedBatchSlice> slices;
  int64_t total_rows = 0;
  for (auto& sb : *stream_batches) {
    for (auto& batch : sb) {
      if (n_max >= 0 && total_rows >= n_max) break;
      if (batch.length <= 0) continue;
      int64_t take = (n_max >= 0) ? std::min<int64_t>(batch.length, n_max - total_rows) : batch.length;
      slices.push_back({&batch, take, total_rows});
      total_rows += take;
    }
  }

  const int64_t n_cols = schema->n_children;

  if (total_rows == 0) {
    for (auto& sb : *stream_batches) sb.clear();
    auto* empty_impl = new VecArrowArrayStream();
    empty_impl->child_schema_ptrs.resize(static_cast<size_t>(n_cols));
    for (int64_t j = 0; j < n_cols; j++) {
      empty_impl->child_schema_ptrs[static_cast<size_t>(j)] = schema->children[j];
    }
    empty_impl->borrowed_schema = *schema;
    empty_impl->borrowed_schema.release = noop_release_schema;

    SEXP empty_stream_xptr = PROTECT(wrap_vec_arrow_stream(empty_impl));
    Rcpp::Function int64_ptype = RCallbacks::ns_fn("bigrquerystorage", "int64_ptype");
    SEXP ptype = PROTECT(int64_ptype(schema_xptr));
    SEXP df = PROTECT(bqs_convert_stream(empty_stream_xptr, ptype, bigint));
    SEXP tb = PROTECT(bqs_postprocess(df, schema_xptr, bigint));
    if (as_data_frame) {
      Rf_setAttrib(tb, R_ClassSymbol, Rf_mkString("data.frame"));
    } else if (!Rf_inherits(tb, "tbl_df")) {
      SEXP cls_tbl = PROTECT(make_tbl_class());
      Rf_setAttrib(tb, R_ClassSymbol, cls_tbl);
      UNPROTECT(1);
    }
    UNPROTECT(4);
    return tb;
  }

  std::vector<size_t> col_node_pos(static_cast<size_t>(n_cols), 0);
  std::vector<size_t> col_buf_pos(static_cast<size_t>(n_cols), 0);
  size_t running_nodes = 0;
  size_t running_bufs = 0;
  for (int64_t j = 0; j < n_cols; j++) {
    col_node_pos[static_cast<size_t>(j)] = running_nodes;
    col_buf_pos[static_cast<size_t>(j)] = running_bufs;
    bqs_ipc::count_schema_nodes_and_buffers(schema->children[j], &running_nodes, &running_bufs);
  }

  for (const auto& sl : slices) {
    if (sl.batch->nodes.size() != running_nodes || sl.batch->buffers.size() != running_bufs) {
      Rcpp::stop("Arrow RecordBatch node/buffer count mismatch against session schema.");
    }
  }

  Rcpp::List out_cols(n_cols);
  Rcpp::CharacterVector col_names(n_cols);
  std::vector<DirectColSpec> direct_specs(static_cast<size_t>(n_cols));
  std::vector<int> numeric_cols;
  std::vector<int> string_cols;
  std::vector<int> fallback_cols;

  SEXP cls_int64 = PROTECT(Rf_mkString("integer64"));
  SEXP cls_date = PROTECT(Rf_mkString("Date"));
  SEXP cls_posix = PROTECT(Rf_allocVector(STRSXP, 2));
  SET_STRING_ELT(cls_posix, 0, Rf_mkChar("POSIXct"));
  SET_STRING_ELT(cls_posix, 1, Rf_mkChar("POSIXt"));
  SEXP cls_hms = PROTECT(Rf_allocVector(STRSXP, 2));
  SET_STRING_ELT(cls_hms, 0, Rf_mkChar("hms"));
  SET_STRING_ELT(cls_hms, 1, Rf_mkChar("difftime"));
  SEXP cls_wk = PROTECT(Rf_allocVector(STRSXP, 2));
  SET_STRING_ELT(cls_wk, 0, Rf_mkChar("wk_wkt"));
  SET_STRING_ELT(cls_wk, 1, Rf_mkChar("wk_vctr"));
  SEXP str_utc = PROTECT(Rf_mkString("UTC"));
  SEXP str_secs = PROTECT(Rf_mkString("secs"));
  SEXP sym_tzone = Rf_install("tzone");
  SEXP sym_units = Rf_install("units");

  auto alloc_col = [&](int64_t j, SEXPTYPE rtype, DirectColSpec& sp) -> SEXP {
    SEXP v = PROTECT(Rf_allocVector(rtype, total_rows));
    sp.out_ptr = (rtype == REALSXP) ? static_cast<void*>(REAL(v))
               : (rtype == INTSXP)  ? static_cast<void*>(INTEGER(v))
                                    : static_cast<void*>(LOGICAL(v));
    out_cols[j] = v;
    UNPROTECT(1);
    numeric_cols.push_back(static_cast<int>(j));
    return v;
  };

  for (int64_t j = 0; j < n_cols; j++) {
    ArrowSchema* ch = schema->children[j];
    col_names[j] = ch->name ? Rf_mkCharCE(ch->name, CE_UTF8) : Rf_mkChar("");
    const char* fmt = schema_format(ch);
    std::string ext = schema_extension_name(ch);

    DirectColSpec& sp = direct_specs[static_cast<size_t>(j)];
    sp.col_idx = static_cast<int>(j);
    sp.node_idx = col_node_pos[static_cast<size_t>(j)];
    sp.buf_idx = col_buf_pos[static_cast<size_t>(j)];
    sp.is_geography = (ext == EXT_GEOGRAPHY);

    int dec_scale = 0;
    if (ch->dictionary != nullptr) {
      sp.kind = DCOL_FALLBACK;
      fallback_cols.push_back(static_cast<int>(j));
    } else if (std::strcmp(fmt, "g") == 0 && ext.empty()) {
      sp.kind = DCOL_FLOAT64;
      alloc_col(j, REALSXP, sp);
    } else if (std::strcmp(fmt, "f") == 0 && ext.empty()) {
      sp.kind = DCOL_FLOAT32;
      alloc_col(j, REALSXP, sp);
    } else if (std::strcmp(fmt, "l") == 0 && ext.empty() && bigint != "character") {
      if (bigint == "integer") {
        sp.kind = DCOL_INT64_TO_INT32;
        alloc_col(j, INTSXP, sp);
      } else if (bigint == "integer64") {
        sp.kind = DCOL_INT64_TO_INT64;
        Rf_setAttrib(alloc_col(j, REALSXP, sp), R_ClassSymbol, cls_int64);
      } else {
        sp.kind = DCOL_INT64_TO_DBL;
        alloc_col(j, REALSXP, sp);
      }
    } else if (std::strcmp(fmt, "i") == 0 && ext.empty()) {
      sp.kind = DCOL_INT32;
      alloc_col(j, INTSXP, sp);
    } else if (std::strcmp(fmt, "b") == 0 && ext.empty()) {
      sp.kind = DCOL_BOOL;
      alloc_col(j, LGLSXP, sp);
    } else if (std::strcmp(fmt, "tdD") == 0 && ext.empty()) {
      sp.kind = DCOL_DATE32;
      Rf_setAttrib(alloc_col(j, REALSXP, sp), R_ClassSymbol, cls_date);
    } else if (std::strcmp(fmt, "ttu") == 0 && ext.empty()) {
      sp.kind = DCOL_TIME64_US;
      SEXP v = alloc_col(j, REALSXP, sp);
      Rf_setAttrib(v, sym_units, str_secs);
      Rf_setAttrib(v, R_ClassSymbol, cls_hms);
    } else if (std::strncmp(fmt, "tsu:", 4) == 0 && (ext.empty() || ext == EXT_DATETIME)) {
      sp.kind = DCOL_TIMESTAMP_US;
      SEXP v = alloc_col(j, REALSXP, sp);
      Rf_setAttrib(v, R_ClassSymbol, cls_posix);
      Rf_setAttrib(v, sym_tzone, str_utc);
    } else if (
#if defined(__SIZEOF_INT128__)
        ext.empty() && parse_decimal128_scale(fmt, &dec_scale)
#else
        false
#endif
    ) {
      sp.kind = DCOL_DECIMAL128;
      sp.dec_scale_mult = std::pow(10.0, -dec_scale);
      alloc_col(j, REALSXP, sp);
    } else if ((std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "U") == 0) &&
               (ext.empty() || ext == EXT_GEOGRAPHY)) {
      sp.kind = (std::strcmp(fmt, "U") == 0) ? DCOL_LARGE_UTF8 : DCOL_UTF8;
      string_cols.push_back(static_cast<int>(j));
    } else {
      sp.kind = DCOL_FALLBACK;
      fallback_cols.push_back(static_cast<int>(j));
    }
  }

  for (int64_t j = 0; j < n_cols; j++) {
    const DirectColSpec& sp = direct_specs[static_cast<size_t>(j)];
    bool is_fb = (sp.kind == DCOL_FALLBACK);
    for (const auto& sl : slices) {
      const bqs_ipc::DecodedBatch& batch = *sl.batch;
      if (!is_fb && sl.row_len <= 0) continue;
      size_t npos = sp.node_idx;
      size_t bpos = sp.buf_idx;
      std::string val_err;
      if (!bqs_ipc::validate_borrowed_field_buffers(
            schema->children[j], batch, is_fb ? batch.length : sl.row_len, is_fb, &npos, &bpos, &val_err)) {
        UNPROTECT(7);
        Rcpp::stop(val_err.c_str());
      }
    }
  }

  uint64_t na_real_bits = bqs_ipc::load_unaligned<uint64_t>(&NA_REAL);
  auto num_groups = group_slices_by_rows(slices, 131072);

  std::vector<NumericSliceTask> num_tasks;
  num_tasks.reserve(numeric_cols.size() * num_groups.size());
  for (int cidx : numeric_cols) {
    for (const auto& grp : num_groups) {
      num_tasks.push_back({
        &direct_specs[static_cast<size_t>(cidx)],
        slices.data(),
        grp.first,
        grp.second,
        na_real_bits,
        false
      });
    }
  }

  std::vector<std::vector<StringChunkTask>> str_col_tasks(string_cols.size());
  std::vector<StringChunkTask*> flat_str_tasks;
  if (!string_cols.empty()) {
    auto str_groups = group_slices_by_rows(slices, 196608);
    for (size_t sc = 0; sc < string_cols.size(); sc++) {
      int cidx = string_cols[sc];
      auto& vec = str_col_tasks[sc];
      vec.resize(str_groups.size());
      for (size_t g = 0; g < str_groups.size(); g++) {
        size_t bs = str_groups[g].first;
        size_t be = str_groups[g].second;
        int64_t chunk_rows = 0;
        for (size_t u = bs; u < be; u++) chunk_rows += slices[u].row_len;
        vec[g].col = &direct_specs[static_cast<size_t>(cidx)];
        vec[g].slices = slices.data();
        vec[g].b_start = bs;
        vec[g].b_end = be;
        vec[g].dst_offset = slices[bs].dst_offset;
        vec[g].total_rows = chunk_rows;
        flat_str_tasks.push_back(&vec[g]);
      }
    }
  }

  size_t total_tasks = num_tasks.size() + flat_str_tasks.size();
  if (total_tasks > 0) {
    int resolved = bqs_resolve_threads(n_threads, 24u);
    int workers = (total_rows >= 8192 && total_tasks > 1) ? resolved : 1;
    bqs_parallel_for(total_tasks, workers, [&](int, size_t idx) {
      if (idx < flat_str_tasks.size()) run_string_chunk_task(*flat_str_tasks[idx]);
      else run_numeric_slice_task(num_tasks[idx - flat_str_tasks.size()]);
    });
  }

  for (const auto& t : num_tasks) {
    if (t.overflow) {
      Rf_warning("NAs produced by integer overflow");
      break;
    }
  }

  for (size_t sc = 0; sc < string_cols.size(); sc++) {
    int cidx = string_cols[sc];
    const DirectColSpec& sp = direct_specs[static_cast<size_t>(cidx)];
    auto& chunks = str_col_tasks[sc];

    SEXP str_vec = PROTECT(Rf_allocVector(STRSXP, total_rows));
    if (sp.is_geography) Rf_setAttrib(str_vec, R_ClassSymbol, cls_wk);

    size_t max_guard_slots = 0;
    for (const auto& ch : chunks) {
      if (ch.use_dict) max_guard_slots += ch.entries.size();
    }

    SEXP guard = PROTECT(Rf_allocVector(STRSXP, static_cast<R_xlen_t>(max_guard_slots)));
    R_xlen_t guard_pos = 0;
    GlobalCharTable gtab(std::min<size_t>(max_guard_slots + 16, 65536));
    std::vector<SEXP> resolved;

    for (auto& ch : chunks) {
      if (ch.use_dict) {
        size_t n_ent = ch.entries.size();
        resolved.resize(n_ent);
        for (size_t u = 0; u < n_ent; u++) {
          const StrDictEntry& e = ch.entries[u];
          resolved[u] = gtab.intern(e.hash, e.ptr, e.len, guard, &guard_pos);
        }
        const uint32_t* codes = ch.codes.data();
        const int64_t base = ch.dst_offset;
        const int64_t nrows = ch.total_rows;
        if (!ch.has_nulls) {
          for (int64_t i = 0; i < nrows; i++) {
            SEXP val = resolved[codes[i]];
            if (val != R_BlankString) SET_STRING_ELT(str_vec, base + i, val);
          }
        } else {
          for (int64_t i = 0; i < nrows; i++) {
            uint32_t code = codes[i];
            if (code == 0xFFFFFFFFu) SET_STRING_ELT(str_vec, base + i, NA_STRING);
            else {
              SEXP val = resolved[code];
              if (val != R_BlankString) SET_STRING_ELT(str_vec, base + i, val);
            }
          }
        }
        std::vector<StrDictEntry>().swap(ch.entries);
        std::vector<uint32_t>().swap(ch.codes);
      } else if (sp.kind == DCOL_LARGE_UTF8) {
        fill_high_card_strings<int64_t>(str_vec, sp, slices, ch.b_start, ch.b_end);
      } else {
        fill_high_card_strings<int32_t>(str_vec, sp, slices, ch.b_start, ch.b_end);
      }
    }

    out_cols[cidx] = str_vec;
    UNPROTECT(2);
  }

  if (!fallback_cols.empty()) {
    size_t nf = fallback_cols.size();
    std::vector<ArrowSchema*> fb_child_schemas(nf);
    for (size_t k = 0; k < nf; k++) {
      fb_child_schemas[k] = schema->children[fallback_cols[k]];
    }
    auto* fb_impl = new VecArrowArrayStream();
    fb_impl->child_schema_ptrs = fb_child_schemas;
    fb_impl->borrowed_schema = *schema;
    fb_impl->borrowed_schema.n_children = static_cast<int64_t>(nf);
    fb_impl->borrowed_schema.release = noop_release_schema;

    fb_impl->batches.resize(slices.size());
    for (size_t s = 0; s < slices.size(); s++) {
      const DecodedBatchSlice& sl = slices[s];
      ArrowArray& root_a = fb_impl->batches[s];
      auto* rpriv = new bqs_ipc::BorrowedArrayPriv();
      root_a.length = sl.row_len;
      root_a.null_count = 0;
      root_a.offset = 0;
      root_a.n_buffers = 1;
      root_a.n_children = static_cast<int64_t>(nf);
      root_a.buffers = rpriv->buffers;
      root_a.dictionary = nullptr;
      root_a.private_data = rpriv;
      root_a.release = bqs_ipc::release_borrowed_array;
      root_a.children = static_cast<ArrowArray**>(malloc(nf * sizeof(ArrowArray*)));
      for (size_t k = 0; k < nf; k++) {
        int col_idx = fallback_cols[k];
        size_t npos = col_node_pos[static_cast<size_t>(col_idx)];
        size_t bpos = col_buf_pos[static_cast<size_t>(col_idx)];
        ArrowArray* ch_a = bqs_ipc::build_borrowed_field_array(
          schema->children[col_idx], *sl.batch, &npos, &bpos);
        bqs_ipc::truncate_borrowed_field_array(ch_a, schema->children[col_idx], sl.row_len);
        root_a.children[k] = ch_a;
      }
    }

    SEXP sub_schema_xptr = PROTECT(nanoarrow_schema_owning_xptr());
    struct ArrowSchema* sub_sch_ptr = nanoarrow_output_schema_from_xptr(sub_schema_xptr);
    *sub_sch_ptr = fb_impl->borrowed_schema;
    sub_sch_ptr->children = fb_child_schemas.data();
    sub_sch_ptr->release = noop_release_schema;

    Rcpp::Function int64_ptype = RCallbacks::ns_fn("bigrquerystorage", "int64_ptype");
    SEXP sub_ptype = PROTECT(int64_ptype(sub_schema_xptr));
    SEXP fb_stream_xptr = PROTECT(wrap_vec_arrow_stream(fb_impl));
    SEXP fb_df = PROTECT(bqs_convert_stream(fb_stream_xptr, sub_ptype, bigint));
    SEXP fb_post = PROTECT(bqs_postprocess(fb_df, sub_schema_xptr, bigint));
    Rcpp::List fb_list(fb_post);
    for (size_t k = 0; k < nf; k++) out_cols[fallback_cols[k]] = fb_list[k];
    UNPROTECT(5);
  }

  for (auto& sb : *stream_batches) {
    for (auto& b : sb) b.clear_memory();
    std::vector<bqs_ipc::DecodedBatch>().swap(sb);
  }

  out_cols.attr("names") = col_names;
  if (as_data_frame) {
    out_cols.attr("class") = "data.frame";
  } else {
    SEXP cls_tbl = PROTECT(make_tbl_class());
    Rf_setAttrib(out_cols, R_ClassSymbol, cls_tbl);
    UNPROTECT(1);
  }
  Rf_setAttrib(out_cols, R_RowNamesSymbol, compact_row_names(total_rows));

  UNPROTECT(7);
  return out_cols;
}

// [[Rcpp::export(rng=false)]]
SEXP bqs_decode_arrow_ipc_raw(Rcpp::RawVector ipc_bytes,
                              double n_max,
                              std::string bigint,
                              bool as_data_frame,
                              int n_threads) {
  std::string schema_bytes;
  std::vector<std::string> raw_batches;
  int64_t total_rows = 0;
  std::string split_err;
  if (!bqs_ipc::split_arrow_ipc_stream(
        RAW(ipc_bytes),
        static_cast<size_t>(ipc_bytes.size()),
        &schema_bytes,
        &raw_batches,
        nullptr,
        &total_rows,
        &split_err)) {
    Rcpp::stop(split_err.c_str());
  }
  if (schema_bytes.empty()) {
    Rcpp::stop("Invalid Arrow IPC stream: missing Schema message.");
  }

  int64_t n = (std::isfinite(n_max) && n_max >= 0.0) ? static_cast<int64_t>(n_max) : -1;
  std::vector<std::vector<bqs_ipc::DecodedBatch>> stream_batches(1);
  if (n != 0 && !raw_batches.empty()) {
    bqs_ipc::ZstdDecompressContext zctx;
    int64_t acc_rows = 0;
    for (size_t i = 0; i < raw_batches.size(); i++) {
      if (n > 0 && acc_rows >= n) break;
      bqs_ipc::DecodedBatch dec;
      std::string dec_err;
      if (!bqs_ipc::decode_ipc_record_batch(std::move(raw_batches[i]), &dec, &zctx, &dec_err)) {
        Rcpp::stop(dec_err.c_str());
      }
      acc_rows += dec.length;
      stream_batches[0].push_back(std::move(dec));
    }
  }

  SEXP schema_xptr = PROTECT(bqs_parse_schema_ipc(schema_bytes));
  SEXP res = PROTECT(bqs_convert_decoded_streams(
    schema_xptr, &stream_batches, n, bigint, as_data_frame, n_threads));
  UNPROTECT(2);
  return res;
}
