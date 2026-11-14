// Arrow -> R conversion for bigrquerystorage.
//
// This file is independent from gRPC so it only needs Rcpp and the nanoarrow
// C data interface helpers (nanoarrow/r.h).
// [[Rcpp::depends(nanoarrow)]]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <Rcpp.h>
#include <nanoarrow/r.h>
#include "bqs_arrow_ipc.h"

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

static inline bool is_bit_set(const uint8_t* valid, int64_t idx) {
  return (valid[idx >> 3] >> (idx & 7)) & 1;
}

static SEXP make_int64_sexp(const int64_t* vals,
                            const uint8_t* valid,
                            int64_t null_count,
                            int64_t offset,
                            int64_t len,
                            SEXP int64_class) {
  SEXP vec = PROTECT(Rf_allocVector(REALSXP, len));
  int64_t* dst = reinterpret_cast<int64_t*>(REAL(vec));
  if (null_count == 0 || valid == nullptr) {
    if (len > 0) {
      std::memcpy(dst, vals + offset, len * sizeof(int64_t));
    }
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

// Convert a list<int64> (+l with child l) column across batches to an R list of integer64 vectors
static SEXP convert_list_int64_batches(const std::vector<ArrowArray*>& col_batches,
                                       int64_t total_rows) {
  SEXP int64_class = PROTECT(Rf_mkString("integer64"));
  SEXP out = PROTECT(Rf_allocVector(VECSXP, total_rows));
  int64_t pos = 0;
  for (size_t b = 0; b < col_batches.size(); b++) {
    ArrowArray* col = col_batches[b];
    int64_t len = col->length;
    int64_t offset = col->offset;
    int64_t null_count = col->null_count;
    const uint8_t* valid = static_cast<const uint8_t*>(col->buffers[0]);
    const int32_t* offsets = static_cast<const int32_t*>(col->buffers[1]);

    ArrowArray* item = col->children[0];
    int64_t item_offset = item->offset;
    int64_t item_null_count = item->null_count;
    const uint8_t* item_valid = static_cast<const uint8_t*>(item->buffers[0]);
    const int64_t* item_vals = static_cast<const int64_t*>(item->buffers[1]);

    for (int64_t i = 0; i < len; i++) {
      int64_t idx = offset + i;
      if (null_count > 0 && valid != nullptr && !is_bit_set(valid, idx)) {
        SET_VECTOR_ELT(out, pos + i, R_NilValue);
      } else {
        int32_t start = offsets[idx];
        int32_t end = offsets[idx + 1];
        SET_VECTOR_ELT(out, pos + i, make_int64_sexp(
          item_vals, item_valid, item_null_count,
          item_offset + start, end - start, int64_class
        ));
      }
    }
    pos += len;
  }
  UNPROTECT(2);
  return out;
}

// nanoarrow <= 0.9.0 does not apply list offsets when converting int64 to
// bit64::integer64, so INT64 values under a REPEATED field (list<int64>,
// list<struct<int64>>, ...) come back shifted or as garbage. Such columns are
// converted by nanoarrow with a double ptype and every int64 leaf is then
// rebuilt losslessly from the Arrow buffers.
static bool schema_has_int64_under_list(const ArrowSchema* sch, bool under_list) {
  if (!sch || !sch->format) return false;
  if (std::strcmp(sch->format, "l") == 0) return under_list;
  bool is_list = std::strcmp(sch->format, "+l") == 0;
  for (int64_t j = 0; j < sch->n_children; j++) {
    if (schema_has_int64_under_list(sch->children[j], under_list || is_list)) return true;
  }
  return false;
}

static SEXP sanitize_ptype_int64_under_list(SEXP ptype, const ArrowSchema* sch, bool under_list) {
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

static void repair_int64_value(SEXP parent, R_xlen_t idx, const ArrowArray* arr,
                               const ArrowSchema* sch, int64_t start, int64_t len,
                               SEXP int64_class);

// r_list[r_pos + i] was converted from logical row start + i of the list array `arr`
static void repair_int64_list_elements(SEXP r_list, R_xlen_t r_pos, const ArrowArray* arr,
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

// VECTOR_ELT(parent, idx) was converted from logical rows [start, start + len)
// of `arr` and sits under a list: rebuild its int64 leaves.
static void repair_int64_value(SEXP parent, R_xlen_t idx, const ArrowArray* arr,
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

// VECTOR_ELT(parent, idx) is a column spanning all batches: batch b covers
// logical rows [starts[b], starts[b] + lens[b]) of arrs[b].
static void repair_int64_column(SEXP parent, R_xlen_t idx,
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
// Same row.names as tibble::new_tibble(x, nrow = n)
static SEXP compact_row_names(int64_t n) {
  if (n <= 0) return Rf_allocVector(INTSXP, 0);
  SEXP rn = Rf_allocVector(INTSXP, 2);
  INTEGER(rn)[0] = NA_INTEGER;
  INTEGER(rn)[1] = -static_cast<int>(n);
  return rn;
}

// Fast hybrid ArrowArrayStream converter:
// Directly converts top-level Decimal128 (NUMERIC/BIGNUMERIC -> double),
// top-level Int64 (when bigint == "integer" -> integer32 with overflow warning),
// and top-level list<int64> in C++. Remaining columns are delegated to
// nanoarrow::convert_array_stream; int64 values nested under a list anywhere in
// those columns (REPEATED INT64 in a STRUCT, INT64 fields of a REPEATED RECORD)
// are then rebuilt losslessly as integer64 from the Arrow buffers.
// [[Rcpp::export(rng=false)]]
SEXP bqs_convert_stream(SEXP stream_xptr,
                        SEXP ptype_df,
                        std::string bigint,
                        Rcpp::Function na_convert_stream,
                        Rcpp::Function na_convert_stream_quiet) {
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
      dec_cols.push_back(j);
      dec_scales.push_back(std::pow(10.0, -scale));
    } else if (bigint == "integer" && fmt && std::strcmp(fmt, "l") == 0) {
      int32_cols.push_back(j);
    } else if (fmt && std::strcmp(fmt, "+l") == 0 && ch->n_children == 1 &&
               ch->children[0] && ch->children[0]->format &&
               std::strcmp(ch->children[0]->format, "l") == 0) {
      list_int64_cols.push_back(j);
    } else {
      if (schema_has_int64_under_list(ch, false)) {
        repair_cols.push_back(j);
      }
      other_cols.push_back(j);
    }
  }

  if (dec_cols.empty() && int32_cols.empty() && list_int64_cols.empty() && repair_cols.empty()) {
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
      const uint8_t* raw_vals = static_cast<const uint8_t*>(child->buffers[1]);
      if (null_count == 0 || valid == nullptr) {
        for (int64_t i = 0; i < len; i++) {
          __extension__ __int128 v;
          std::memcpy(&v, raw_vals + (offset + i) * 16, 16);
          out_ptr[pos + i] = static_cast<double>(v) * scale_mult;
        }
      } else {
        for (int64_t i = 0; i < len; i++) {
          int64_t idx = offset + i;
          if ((valid[idx >> 3] >> (idx & 7)) & 1) {
            __extension__ __int128 v;
            std::memcpy(&v, raw_vals + idx * 16, 16);
            out_ptr[pos + i] = static_cast<double>(v) * scale_mult;
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

  // 2b. Convert top-level list<int64> columns directly in C++
  for (size_t k = 0; k < list_int64_cols.size(); k++) {
    int col_idx = list_int64_cols[k];
    std::vector<ArrowArray*> col_batches(batches.size());
    for (size_t b = 0; b < batches.size(); b++) {
      col_batches[b] = batches[b].children[col_idx];
    }
    out_cols[col_idx] = convert_list_int64_batches(col_batches, total_rows);
  }

  // 3. Delegate remaining columns to nanoarrow
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

    Rcpp::Function& convert_fn = repair_cols.empty()
      ? na_convert_stream
      : na_convert_stream_quiet;
    SEXP sub_res_sexp = PROTECT(convert_fn(sub_stream_xptr, sub_ptype, static_cast<double>(total_rows), R_PosInf));
    Rcpp::List sub_res(sub_res_sexp);
    UNPROTECT(2);

    for (size_t j = 0; j < other_cols.size(); j++) {
      out_cols[other_cols[j]] = sub_res[j];
    }

    // Rebuild int64 values under REPEATED fields losslessly from the Arrow buffers
    if (!repair_cols.empty()) {
      SEXP int64_class = PROTECT(Rf_mkString("integer64"));
      std::vector<const ArrowArray*> arrs(batches.size());
      std::vector<int64_t> starts(batches.size());
      std::vector<int64_t> lens(batches.size());
      for (size_t k = 0; k < repair_cols.size(); k++) {
        int col_idx = repair_cols[k];
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

// -- Post-processing ----------------------------------------------------------
//
// Walks the converted data.frame alongside its Arrow schema and applies the
// BigQuery specific R classes (tibble for STRUCT/RECORD, bare lists for
// REPEATED, UTC for DATETIME, wk_wkt for GEOGRAPHY, blob for BYTES, and the
// requested `bigint` type for INT64). Objects are modified in place unless
// shared, and every INT64 leaf is converted with a single vectorised call to
// `as_bigint` instead of one R call per row.

namespace {

std::string schema_extension_name(const ArrowSchema* sch) {
  if (!sch || !sch->metadata) return std::string();
  const char* md = sch->metadata;
  int32_t n_pairs;
  std::memcpy(&n_pairs, md, sizeof(int32_t));
  md += sizeof(int32_t);
  for (int32_t i = 0; i < n_pairs; i++) {
    int32_t key_len, val_len;
    std::memcpy(&key_len, md, sizeof(int32_t));
    md += sizeof(int32_t);
    const char* key = md;
    md += key_len;
    std::memcpy(&val_len, md, sizeof(int32_t));
    md += sizeof(int32_t);
    const char* val = md;
    md += val_len;
    if (key_len == 20 && std::memcmp(key, "ARROW:extension:name", 20) == 0) {
      return std::string(val, val_len);
    }
  }
  return std::string();
}

const char* const EXT_DATETIME = "google:sqlType:datetime";
const char* const EXT_GEOGRAPHY = "google:sqlType:geography";

inline const char* schema_format(const ArrowSchema* sch) {
  return (sch && sch->format) ? sch->format : "";
}

// Mirrors needs_postprocess_schema(): which Arrow fields can be modified by the walker.
bool needs_postprocess(const ArrowSchema* sch, const std::string& ext) {
  const char* fmt = schema_format(sch);
  return std::strcmp(fmt, "l") == 0 || std::strcmp(fmt, "z") == 0 ||
    std::strcmp(fmt, "tsu:") == 0 || std::strcmp(fmt, "+s") == 0 ||
    std::strcmp(fmt, "+l") == 0 || ext == EXT_DATETIME || ext == EXT_GEOGRAPHY;
}

enum BigintMode { BIGINT_INTEGER, BIGINT_INTEGER64, BIGINT_NUMERIC, BIGINT_CHARACTER };

struct PostContext {
  BigintMode mode;
  // (owning container, index) of every INT64 leaf that must go through as_bigint
  std::vector<std::pair<SEXP, R_xlen_t>> int64_slots;
  SEXP sym_tzone;
  SEXP sym_ptype;
  SEXP utc;
  SEXP cls_tbl;
  SEXP cls_wk;
  SEXP cls_blob;
  SEXP raw0;
};

// Return VECTOR_ELT(parent, i) after making sure it is safe to modify in place
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
      for (R_xlen_t k = 0; k < n; k++) {
        walk_slot(col, k, child, child_ext, ctx);
      }
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
    return;
  }

  if (ext == EXT_GEOGRAPHY) {
    col = own_slot(parent, i);
    Rf_setAttrib(col, R_ClassSymbol, ctx.cls_wk);
    return;
  }

  if (std::strcmp(fmt, "z") == 0) {
    if (!Rf_inherits(col, "blob")) {
      col = own_slot(parent, i);
      Rf_setAttrib(col, R_ClassSymbol, ctx.cls_blob);
      Rf_setAttrib(col, ctx.sym_ptype, ctx.raw0);
    }
    return;
  }
}

// Convert the collected INT64 leaves: small ones (REPEATED values, nested
// fields) are concatenated and converted with a single call to as_bigint()
void convert_int64_slots(PostContext& ctx, Rcpp::Function& as_bigint) {
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
    if (len > 0) std::memcpy(dst, REAL(v), len * sizeof(double));
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
      if (len > 0) std::memcpy(INTEGER(piece), INTEGER(res) + pos, len * sizeof(int));
      break;
    case REALSXP:
      if (len > 0) std::memcpy(REAL(piece), REAL(res) + pos, len * sizeof(double));
      break;
    case STRSXP:
      for (R_xlen_t k = 0; k < len; k++) SET_STRING_ELT(piece, k, STRING_ELT(res, pos + k));
      break;
    case LGLSXP:
      if (len > 0) std::memcpy(LOGICAL(piece), LOGICAL(res) + pos, len * sizeof(int));
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

} // namespace

// Which optional packages the post-processing of `schema` will need.
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
SEXP bqs_postprocess(SEXP df, SEXP schema_xptr, std::string bigint, Rcpp::Function as_bigint) {
  struct ArrowSchema* schema = nanoarrow_schema_from_xptr(schema_xptr);

  PostContext ctx;
  if (bigint == "integer") {
    ctx.mode = BIGINT_INTEGER;
  } else if (bigint == "integer64") {
    ctx.mode = BIGINT_INTEGER64;
  } else if (bigint == "numeric") {
    ctx.mode = BIGINT_NUMERIC;
  } else {
    ctx.mode = BIGINT_CHARACTER;
  }
  ctx.sym_tzone = Rf_install("tzone");
  ctx.sym_ptype = Rf_install("ptype");
  ctx.utc = PROTECT(Rf_mkString("UTC"));
  ctx.cls_tbl = PROTECT(Rf_allocVector(STRSXP, 3));
  SET_STRING_ELT(ctx.cls_tbl, 0, Rf_mkChar("tbl_df"));
  SET_STRING_ELT(ctx.cls_tbl, 1, Rf_mkChar("tbl"));
  SET_STRING_ELT(ctx.cls_tbl, 2, Rf_mkChar("data.frame"));
  ctx.cls_wk = PROTECT(Rf_allocVector(STRSXP, 2));
  SET_STRING_ELT(ctx.cls_wk, 0, Rf_mkChar("wk_wkt"));
  SET_STRING_ELT(ctx.cls_wk, 1, Rf_mkChar("wk_vctr"));
  ctx.cls_blob = PROTECT(Rf_allocVector(STRSXP, 4));
  SET_STRING_ELT(ctx.cls_blob, 0, Rf_mkChar("blob"));
  SET_STRING_ELT(ctx.cls_blob, 1, Rf_mkChar("vctrs_list_of"));
  SET_STRING_ELT(ctx.cls_blob, 2, Rf_mkChar("vctrs_vctr"));
  SET_STRING_ELT(ctx.cls_blob, 3, Rf_mkChar("list"));
  ctx.raw0 = PROTECT(Rf_allocVector(RAWSXP, 0));

  // Hold the root in a list so it is processed like any other slot
  SEXP holder = PROTECT(Rf_allocVector(VECSXP, 1));
  SET_VECTOR_ELT(holder, 0, df);
  walk_slot(holder, 0, schema, std::string(), ctx);
  convert_int64_slots(ctx, as_bigint);
  SEXP out = VECTOR_ELT(holder, 0);
  UNPROTECT(6);
  return out;
}

// -- Parallel IPC decoding ----------------------------------------------------
//
// Each BigQuery read stream is handed to R as its own IPC buffer and opened with
// nanoarrow::read_nanoarrow() on the main thread. Pulling batches from those
// readers (IPC decoding + ZSTD/LZ4 decompression) only runs nanoarrow C code on
// in-memory buffers, so it is done concurrently here, one reader per worker at a
// time. All R API calls (creating, releasing readers) stay on the main thread.

namespace {

struct CollectedStream {
  ArrowArrayStream schema_source;
  std::vector<ArrowArray> arrays;
  size_t next = 0;
  std::string last_error;

  ~CollectedStream() {
    for (size_t i = next; i < arrays.size(); i++) {
      if (arrays[i].release != NULL) arrays[i].release(&arrays[i]);
    }
    if (schema_source.release != NULL) schema_source.release(&schema_source);
  }

  static int get_schema(struct ArrowArrayStream* s, struct ArrowSchema* out) {
    CollectedStream* self = static_cast<CollectedStream*>(s->private_data);
    int rc = self->schema_source.get_schema(&self->schema_source, out);
    if (rc != 0) {
      const char* err = self->schema_source.get_last_error(&self->schema_source);
      self->last_error = err ? err : "Failed to get schema";
    }
    return rc;
  }

  static int get_next(struct ArrowArrayStream* s, struct ArrowArray* out) {
    CollectedStream* self = static_cast<CollectedStream*>(s->private_data);
    if (self->next >= self->arrays.size()) {
      out->release = NULL;
      return 0;
    }
    *out = self->arrays[self->next];
    self->arrays[self->next].release = NULL;
    self->next++;
    return 0;
  }

  static const char* get_last_error(struct ArrowArrayStream* s) {
    CollectedStream* self = static_cast<CollectedStream*>(s->private_data);
    return self->last_error.empty() ? NULL : self->last_error.c_str();
  }

  static void release(struct ArrowArrayStream* s) {
    delete static_cast<CollectedStream*>(s->private_data);
    s->release = NULL;
  }
};

// Take ownership of the ArrowArrayStream held by a nanoarrow_array_stream xptr
void move_stream(SEXP xptr, ArrowArrayStream* dst) {
  ArrowArrayStream* src = nanoarrow_array_stream_from_xptr(xptr);
  std::memcpy(dst, src, sizeof(ArrowArrayStream));
  src->release = NULL;
}

} // namespace

// Pull every batch of `readers` (a list of nanoarrow_array_stream sharing the
// schema of `schema_reader`) using up to `n_threads` threads and return a
// single nanoarrow_array_stream with all batches in reader order.
// [[Rcpp::export(rng=false)]]
SEXP bqs_collect_streams(Rcpp::List readers, SEXP schema_reader, int n_threads) {
  R_xlen_t n_readers = readers.size();
  std::vector<ArrowArrayStream> streams(n_readers);
  for (R_xlen_t i = 0; i < n_readers; i++) {
    move_stream(readers[i], &streams[i]);
  }

  std::vector<std::vector<ArrowArray>> batches(n_readers);
  std::vector<std::string> errors(n_readers);
  std::atomic<R_xlen_t> next_reader(0);
  std::atomic<bool> failed(false);

  auto work = [&]() {
    while (!failed.load(std::memory_order_relaxed)) {
      R_xlen_t idx = next_reader.fetch_add(1);
      if (idx >= n_readers) break;
      ArrowArrayStream* s = &streams[idx];
      while (true) {
        ArrowArray arr;
        arr.release = NULL;
        int rc = s->get_next(s, &arr);
        if (rc != 0) {
          const char* err = s->get_last_error(s);
          errors[idx] = std::string("Failed to decode Arrow record batch: ") + (err ? err : "unknown error");
          failed.store(true);
          break;
        }
        if (arr.release == NULL) break;
        batches[idx].push_back(arr);
      }
    }
  };

  if (n_threads <= 0) {
    unsigned int hw = std::thread::hardware_concurrency();
    n_threads = hw > 0 ? static_cast<int>(std::min(hw, 16u)) : 4;
  }
  int n_workers = static_cast<int>(std::min<R_xlen_t>(std::max(1, n_threads), n_readers));
  if (n_workers <= 1) {
    work();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(n_workers - 1);
    for (int t = 0; t < n_workers - 1; t++) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
  }

  // Readers are released on the main thread: they own R memory (the IPC buffers).
  for (auto& s : streams) {
    if (s.release != NULL) s.release(&s);
  }

  std::string first_error;
  for (auto& e : errors) {
    if (!e.empty()) {
      first_error = e;
      break;
    }
  }

  CollectedStream* collected = new CollectedStream();
  move_stream(schema_reader, &collected->schema_source);
  size_t n_batches = 0;
  for (auto& b : batches) n_batches += b.size();
  collected->arrays.reserve(n_batches);
  for (auto& b : batches) {
    collected->arrays.insert(collected->arrays.end(), b.begin(), b.end());
  }

  SEXP out = PROTECT(nanoarrow_array_stream_owning_xptr());
  ArrowArrayStream* out_stream = nanoarrow_output_array_stream_from_xptr(out);
  out_stream->get_schema = CollectedStream::get_schema;
  out_stream->get_next = CollectedStream::get_next;
  out_stream->get_last_error = CollectedStream::get_last_error;
  out_stream->release = CollectedStream::release;
  out_stream->private_data = collected;
  UNPROTECT(1);

  if (!first_error.empty()) {
    Rcpp::stop(first_error);
  }
  return out;
}

// -- Decimal128 / Decimal256 encoding for upload ------------------------------

namespace {

static bool parse_decimal_string(const char* s,
                                 int scale,
                                 int bitwidth,
                                 uint8_t* out_bytes) {
  int byte_width = bitwidth / 8;
  std::memset(out_bytes, 0, byte_width);
  if (!s) return false;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') return false;

  bool neg = false;
  if (*s == '-') {
    neg = true;
    s++;
  } else if (*s == '+') {
    s++;
  }

  // Accumulate decimal digits into a little-endian uint32 limb array (8 limbs = 256 bits)
  uint32_t limbs[8] = {0};
  auto mul10_add = [&](uint32_t digit) {
    uint64_t carry = digit;
    for (int i = 0; i < 8; i++) {
      uint64_t cur = static_cast<uint64_t>(limbs[i]) * 10ULL + carry;
      limbs[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFULL);
      carry = cur >> 32;
    }
  };

  bool seen_dot = false;
  bool seen_digit = false;
  int frac_digits = 0;
  for (; *s != '\0'; s++) {
    char c = *s;
    if (c == '.') {
      if (seen_dot) return false;
      seen_dot = true;
      continue;
    }
    if (c < '0' || c > '9') break;
    seen_digit = true;
    if (seen_dot) {
      if (frac_digits < scale) {
        mul10_add(static_cast<uint32_t>(c - '0'));
        frac_digits++;
      } else if (frac_digits == scale) {
        // Round half away from zero on next digit
        if (c >= '5') {
          uint64_t carry = 1;
          for (int i = 0; i < 8 && carry; i++) {
            uint64_t cur = static_cast<uint64_t>(limbs[i]) + carry;
            limbs[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFULL);
            carry = cur >> 32;
          }
        }
        frac_digits++;
      }
    } else {
      mul10_add(static_cast<uint32_t>(c - '0'));
    }
  }
  if (!seen_digit) return false;
  while (frac_digits < scale) {
    mul10_add(0);
    frac_digits++;
  }

  if (neg) {
    // Two's complement negate across all 8 limbs
    uint64_t carry = 1;
    for (int i = 0; i < 8; i++) {
      uint64_t cur = static_cast<uint64_t>(~limbs[i]) + carry;
      limbs[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFULL);
      carry = cur >> 32;
    }
  }

  std::memcpy(out_bytes, limbs, byte_width);
  return true;
}

static void encode_double_to_decimal(double v,
                                     int scale,
                                     int bitwidth,
                                     uint8_t* out_bytes) {
  int byte_width = bitwidth / 8;
  if (!R_finite(v)) {
    std::memset(out_bytes, 0, byte_width);
    return;
  }
#if defined(__SIZEOF_INT128__)
  if (scale <= 9) {
    double scaled = std::round(v * std::pow(10.0, scale));
    __extension__ __int128 val = static_cast<__int128>(scaled);
    std::memcpy(out_bytes, &val, 16);
    if (byte_width == 32) {
      std::memset(out_bytes + 16, val < 0 ? 0xFF : 0x00, 16);
    }
    return;
  }
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", std::min(scale, 15), v);
  parse_decimal_string(buf, scale, bitwidth, out_bytes);
}

} // namespace

// Convert an R numeric, integer, integer64, or character vector to a
// little-endian Decimal128 (bitwidth=128) or Decimal256 (bitwidth=256) buffer
// and accompanying validity bitmap buffer.
// [[Rcpp::export(rng=false)]]
Rcpp::List bqs_encode_decimal(SEXP x, int scale = 9, int bitwidth = 128) {
  if (bitwidth != 128 && bitwidth != 256) {
    Rcpp::stop("bitwidth must be 128 or 256.");
  }
  int byte_width = bitwidth / 8;
  R_xlen_t n = XLENGTH(x);
  Rcpp::RawVector data_buf(n * byte_width);
  uint8_t* dst = RAW(data_buf);

  R_xlen_t valid_bytes = (n + 7) / 8;
  Rcpp::RawVector valid_buf(valid_bytes);
  uint8_t* valid = RAW(valid_buf);
  std::memset(valid, 0, valid_bytes);
  R_xlen_t null_count = 0;

  bool is_i64 = Rf_inherits(x, "integer64") && TYPEOF(x) == REALSXP;

  if (is_i64) {
    const int64_t* vals = reinterpret_cast<const int64_t*>(REAL(x));
    for (R_xlen_t i = 0; i < n; i++) {
      int64_t v = vals[i];
      if (v == static_cast<int64_t>(1ULL << 63)) {
        null_count++;
      } else {
        valid[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        char s[32];
        std::snprintf(s, sizeof(s), "%lld", static_cast<long long>(v));
        parse_decimal_string(s, scale, bitwidth, dst + i * byte_width);
      }
    }
  } else if (TYPEOF(x) == REALSXP) {
    const double* vals = REAL(x);
    for (R_xlen_t i = 0; i < n; i++) {
      double v = vals[i];
      if (ISNA(v) || ISNAN(v)) {
        null_count++;
      } else {
        valid[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        encode_double_to_decimal(v, scale, bitwidth, dst + i * byte_width);
      }
    }
  } else if (TYPEOF(x) == INTSXP) {
    const int* vals = INTEGER(x);
    for (R_xlen_t i = 0; i < n; i++) {
      int v = vals[i];
      if (v == NA_INTEGER) {
        null_count++;
      } else {
        valid[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        encode_double_to_decimal(static_cast<double>(v), scale, bitwidth, dst + i * byte_width);
      }
    }
  } else if (TYPEOF(x) == STRSXP) {
    for (R_xlen_t i = 0; i < n; i++) {
      SEXP elt = STRING_ELT(x, i);
      if (elt == NA_STRING) {
        null_count++;
      } else if (parse_decimal_string(CHAR(elt), scale, bitwidth, dst + i * byte_width)) {
        valid[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
      } else {
        null_count++;
      }
    }
  } else {
    Rcpp::stop("Unsupported vector type for NUMERIC/BIGNUMERIC conversion.");
  }

  return Rcpp::List::create(
    Rcpp::Named("data") = data_buf,
    Rcpp::Named("validity") = null_count > 0 ? SEXP(valid_buf) : R_NilValue,
    Rcpp::Named("null_count") = static_cast<int>(null_count),
    Rcpp::Named("length") = static_cast<int>(n)
  );
}

// -- Fast R data.frame -> Arrow C Data Interface & IPC Encoding ---------------

namespace {

struct OwnedSchemaPriv {
  std::string format;
  std::string name;
};

static void release_owned_schema(struct ArrowSchema* s) {
  if (!s || !s->release) return;
  if (s->children) {
    for (int64_t i = 0; i < s->n_children; i++) {
      if (s->children[i]) {
        if (s->children[i]->release) s->children[i]->release(s->children[i]);
        free(s->children[i]);
      }
    }
    free(s->children);
  }
  if (s->dictionary) {
    if (s->dictionary->release) s->dictionary->release(s->dictionary);
    free(s->dictionary);
  }
  delete static_cast<OwnedSchemaPriv*>(s->private_data);
  s->release = nullptr;
}

static SEXP make_leaf_schema_xptr(const char* fmt, const char* nm) {
  SEXP xptr = PROTECT(nanoarrow_schema_owning_xptr());
  struct ArrowSchema* s = nanoarrow_output_schema_from_xptr(xptr);
  OwnedSchemaPriv* priv = new OwnedSchemaPriv{fmt, nm};
  s->format = priv->format.c_str();
  s->name = priv->name.c_str();
  s->metadata = nullptr;
  s->flags = ARROW_FLAG_NULLABLE;
  s->n_children = 0;
  s->children = nullptr;
  s->dictionary = nullptr;
  s->private_data = priv;
  s->release = release_owned_schema;
  UNPROTECT(1);
  return xptr;
}

struct OwnedArrayPriv {
  const void* buffers[3] = {nullptr, nullptr, nullptr};
  std::vector<uint8_t> validity;
  std::vector<uint8_t> buf1;
  std::vector<uint8_t> buf2;
};

static void release_owned_array(struct ArrowArray* a) {
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
  delete static_cast<OwnedArrayPriv*>(a->private_data);
  a->release = nullptr;
}

enum FastColKind {
  FCOL_UNSUPPORTED = 0,
  FCOL_INT32_ZC,
  FCOL_INT64_ZC,
  FCOL_FLOAT64_ZC,
  FCOL_FLOAT64_FROM_INT,
  FCOL_BOOL,
  FCOL_DATE_INT_ZC,
  FCOL_DATE_DBL,
  FCOL_POSIXCT_TS,
  FCOL_POSIXCT_DT,
  FCOL_UTF8,
  FCOL_FACTOR
};

struct FastColTask {
  int col_idx = 0;
  FastColKind kind = FCOL_UNSUPPORTED;
  const void* r_ptr = nullptr;
  int64_t n = 0;
  int64_t null_count = 0;
  OwnedArrayPriv* priv = nullptr;
};

static void execute_numeric_col_task(FastColTask& t) {
  const int64_t n = t.n;
  OwnedArrayPriv* priv = t.priv;

  switch (t.kind) {
    case FCOL_INT32_ZC:
    case FCOL_DATE_INT_ZC: {
      const int* src = static_cast<const int*>(t.r_ptr);
      int64_t nulls = 0;
      for (int64_t i = 0; i < n; i++) {
        if (src[i] == NA_INTEGER) nulls++;
      }
      t.null_count = nulls;
      if (nulls > 0) {
        priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
        uint8_t* v = priv->validity.data();
        for (int64_t i = 0; i < n; i++) {
          if (src[i] != NA_INTEGER) v[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
        priv->buffers[0] = v;
      }
      priv->buffers[1] = src;
      break;
    }
    case FCOL_INT64_ZC: {
      const int64_t* src = static_cast<const int64_t*>(t.r_ptr);
      const int64_t na_i64 = static_cast<int64_t>(1ULL << 63);
      int64_t nulls = 0;
      for (int64_t i = 0; i < n; i++) {
        if (src[i] == na_i64) nulls++;
      }
      t.null_count = nulls;
      if (nulls > 0) {
        priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
        uint8_t* v = priv->validity.data();
        for (int64_t i = 0; i < n; i++) {
          if (src[i] != na_i64) v[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
        priv->buffers[0] = v;
      }
      priv->buffers[1] = src;
      break;
    }
    case FCOL_FLOAT64_ZC: {
      const double* src = static_cast<const double*>(t.r_ptr);
      int64_t nulls = 0;
      for (int64_t i = 0; i < n; i++) {
        if (std::isnan(src[i]) && R_IsNA(src[i])) nulls++;
      }
      t.null_count = nulls;
      if (nulls > 0) {
        priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
        uint8_t* v = priv->validity.data();
        for (int64_t i = 0; i < n; i++) {
          if (!(std::isnan(src[i]) && R_IsNA(src[i]))) {
            v[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
          }
        }
        priv->buffers[0] = v;
      }
      priv->buffers[1] = src;
      break;
    }
    case FCOL_FLOAT64_FROM_INT: {
      const int* src = static_cast<const int*>(t.r_ptr);
      priv->buf1.resize(static_cast<size_t>(n) * sizeof(double));
      double* dst = reinterpret_cast<double*>(priv->buf1.data());
      int64_t nulls = 0;
      for (int64_t i = 0; i < n; i++) {
        if (src[i] == NA_INTEGER) {
          dst[i] = 0.0;
          nulls++;
        } else {
          dst[i] = static_cast<double>(src[i]);
        }
      }
      t.null_count = nulls;
      if (nulls > 0) {
        priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
        uint8_t* v = priv->validity.data();
        for (int64_t i = 0; i < n; i++) {
          if (src[i] != NA_INTEGER) v[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
        priv->buffers[0] = v;
      }
      priv->buffers[1] = dst;
      break;
    }
    case FCOL_BOOL: {
      const int* src = static_cast<const int*>(t.r_ptr);
      size_t nbytes = static_cast<size_t>((n + 7) >> 3);
      priv->buf1.assign(nbytes, 0);
      uint8_t* dbits = priv->buf1.data();
      int64_t nulls = 0;
      for (int64_t i = 0; i < n; i++) {
        int val = src[i];
        if (val == NA_LOGICAL) {
          nulls++;
        } else if (val != 0) {
          dbits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
      }
      t.null_count = nulls;
      if (nulls > 0) {
        priv->validity.assign(nbytes, 0);
        uint8_t* v = priv->validity.data();
        for (int64_t i = 0; i < n; i++) {
          if (src[i] != NA_LOGICAL) v[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
        priv->buffers[0] = v;
      }
      priv->buffers[1] = dbits;
      break;
    }
    case FCOL_DATE_DBL: {
      const double* src = static_cast<const double*>(t.r_ptr);
      priv->buf1.resize(static_cast<size_t>(n) * sizeof(int32_t));
      int32_t* dst = reinterpret_cast<int32_t*>(priv->buf1.data());
      int64_t nulls = 0;
      for (int64_t i = 0; i < n; i++) {
        double v = src[i];
        if (!std::isfinite(v) || v < -2147483648.0 || v > 2147483647.0) {
          dst[i] = 0;
          nulls++;
        } else {
          dst[i] = static_cast<int32_t>(std::floor(v));
        }
      }
      t.null_count = nulls;
      if (nulls > 0) {
        priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
        uint8_t* v = priv->validity.data();
        for (int64_t i = 0; i < n; i++) {
          double val = src[i];
          if (std::isfinite(val) && val >= -2147483648.0 && val <= 2147483647.0) {
            v[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
          }
        }
        priv->buffers[0] = v;
      }
      priv->buffers[1] = dst;
      break;
    }
    case FCOL_POSIXCT_TS:
    case FCOL_POSIXCT_DT: {
      const double* src = static_cast<const double*>(t.r_ptr);
      priv->buf1.resize(static_cast<size_t>(n) * sizeof(int64_t));
      int64_t* dst = reinterpret_cast<int64_t*>(priv->buf1.data());
      int64_t nulls = 0;
      for (int64_t i = 0; i < n; i++) {
        double v = src[i];
        if (!std::isfinite(v) || v < -9.223372036854e12 || v > 9.223372036854e12) {
          dst[i] = 0;
          nulls++;
        } else {
          dst[i] = static_cast<int64_t>(std::llround(v * 1000000.0));
        }
      }
      t.null_count = nulls;
      if (nulls > 0) {
        priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
        uint8_t* v = priv->validity.data();
        for (int64_t i = 0; i < n; i++) {
          double val = src[i];
          if (std::isfinite(val) && val >= -9.223372036854e12 && val <= 9.223372036854e12) {
            v[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
          }
        }
        priv->buffers[0] = v;
      }
      priv->buffers[1] = dst;
      break;
    }
    default:
      break;
  }
}

static bool build_utf8_col_main_thread(SEXP col,
                                       int64_t n,
                                       OwnedArrayPriv* priv,
                                       int64_t* null_count_out,
                                       std::string* fmt_out) {
  int64_t total_bytes = 0;
  int64_t nulls = 0;
  bool any_needs_translate = false;

  for (int64_t i = 0; i < n; i++) {
    SEXP elt = STRING_ELT(col, i);
    if (elt == NA_STRING) {
      nulls++;
    } else {
      cetype_t ce = Rf_getCharCE(elt);
      if (ce == CE_UTF8 || ce == CE_BYTES) {
        total_bytes += XLENGTH(elt);
      } else {
        const void* vmax = vmaxget();
        const char* u = Rf_translateCharUTF8(elt);
        if (u == CHAR(elt)) {
          total_bytes += XLENGTH(elt);
        } else {
          any_needs_translate = true;
          total_bytes += static_cast<int64_t>(std::strlen(u));
        }
        vmaxset(vmax);
      }
    }
  }

  *null_count_out = nulls;
  if (nulls > 0) {
    priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
  }
  uint8_t* vbits = nulls > 0 ? priv->validity.data() : nullptr;

  bool use_large = total_bytes > 2147483647LL;
  *fmt_out = use_large ? "U" : "u";
  priv->buf2.resize(static_cast<size_t>(total_bytes));
  uint8_t* data_dst = priv->buf2.data();

  if (!use_large) {
    priv->buf1.resize(static_cast<size_t>(n + 1) * sizeof(int32_t));
    int32_t* offs = reinterpret_cast<int32_t*>(priv->buf1.data());
    int32_t pos = 0;
    offs[0] = 0;
    if (!any_needs_translate) {
      for (int64_t i = 0; i < n; i++) {
        SEXP elt = STRING_ELT(col, i);
        if (elt != NA_STRING) {
          if (vbits) vbits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
          int32_t len = static_cast<int32_t>(XLENGTH(elt));
          if (len > 0) {
            std::memcpy(data_dst + pos, CHAR(elt), static_cast<size_t>(len));
            pos += len;
          }
        }
        offs[i + 1] = pos;
      }
    } else {
      for (int64_t i = 0; i < n; i++) {
        SEXP elt = STRING_ELT(col, i);
        if (elt != NA_STRING) {
          if (vbits) vbits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
          const void* vmax = vmaxget();
          const char* u = Rf_translateCharUTF8(elt);
          int32_t len = (u == CHAR(elt)) ? static_cast<int32_t>(XLENGTH(elt))
                                         : static_cast<int32_t>(std::strlen(u));
          if (len > 0) {
            std::memcpy(data_dst + pos, u, static_cast<size_t>(len));
            pos += len;
          }
          vmaxset(vmax);
        }
        offs[i + 1] = pos;
      }
    }
  } else {
    priv->buf1.resize(static_cast<size_t>(n + 1) * sizeof(int64_t));
    int64_t* offs = reinterpret_cast<int64_t*>(priv->buf1.data());
    int64_t pos = 0;
    offs[0] = 0;
    for (int64_t i = 0; i < n; i++) {
      SEXP elt = STRING_ELT(col, i);
      if (elt != NA_STRING) {
        if (vbits) vbits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        const void* vmax = vmaxget();
        const char* u = Rf_translateCharUTF8(elt);
        int64_t len = (u == CHAR(elt)) ? static_cast<int64_t>(XLENGTH(elt))
                                       : static_cast<int64_t>(std::strlen(u));
        if (len > 0) {
          std::memcpy(data_dst + pos, u, static_cast<size_t>(len));
          pos += len;
        }
        vmaxset(vmax);
      }
      offs[i + 1] = pos;
    }
  }

  priv->buffers[0] = vbits;
  priv->buffers[1] = priv->buf1.data();
  priv->buffers[2] = priv->buf2.data();
  return true;
}

static bool build_factor_col_main_thread(SEXP col,
                                         int64_t n,
                                         OwnedArrayPriv* priv,
                                         int64_t* null_count_out,
                                         std::string* fmt_out) {
  SEXP levels = Rf_getAttrib(col, R_LevelsSymbol);
  if (TYPEOF(levels) != STRSXP) return false;
  int n_levels = Rf_length(levels);
  std::vector<std::string> lvl_strs(static_cast<size_t>(n_levels));
  std::vector<uint8_t> lvl_is_na(static_cast<size_t>(n_levels), 0);
  for (int k = 0; k < n_levels; k++) {
    SEXP elt = STRING_ELT(levels, k);
    if (elt == NA_STRING) {
      lvl_is_na[static_cast<size_t>(k)] = 1;
    } else {
      const void* vmax = vmaxget();
      const char* u = Rf_translateCharUTF8(elt);
      lvl_strs[static_cast<size_t>(k)] = std::string(u);
      vmaxset(vmax);
    }
  }

  const int* codes = INTEGER(col);
  int64_t total_bytes = 0;
  int64_t nulls = 0;
  for (int64_t i = 0; i < n; i++) {
    int c = codes[i];
    if (c == NA_INTEGER || c < 1 || c > n_levels || lvl_is_na[static_cast<size_t>(c - 1)] != 0) {
      nulls++;
    } else {
      total_bytes += static_cast<int64_t>(lvl_strs[static_cast<size_t>(c - 1)].size());
    }
  }

  if (total_bytes > 2147483647LL) return false;
  *fmt_out = "u";
  *null_count_out = nulls;
  if (nulls > 0) {
    priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
  }
  uint8_t* vbits = nulls > 0 ? priv->validity.data() : nullptr;

  priv->buf1.resize(static_cast<size_t>(n + 1) * sizeof(int32_t));
  priv->buf2.resize(static_cast<size_t>(total_bytes));
  int32_t* offs = reinterpret_cast<int32_t*>(priv->buf1.data());
  uint8_t* data_dst = priv->buf2.data();
  int32_t pos = 0;
  offs[0] = 0;

  for (int64_t i = 0; i < n; i++) {
    int c = codes[i];
    if (c != NA_INTEGER && c >= 1 && c <= n_levels && lvl_is_na[static_cast<size_t>(c - 1)] == 0) {
      if (vbits) vbits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
      const std::string& s = lvl_strs[static_cast<size_t>(c - 1)];
      int32_t len = static_cast<int32_t>(s.size());
      if (len > 0) {
        std::memcpy(data_dst + pos, s.data(), static_cast<size_t>(len));
        pos += len;
      }
    }
    offs[i + 1] = pos;
  }

  priv->buffers[0] = vbits;
  priv->buffers[1] = priv->buf1.data();
  priv->buffers[2] = priv->buf2.data();
  return true;
}

} // namespace

// [[Rcpp::export(rng=false)]]
bool bqs_has_zstd() {
  return bqs_ipc::has_zstd_support();
}

// [[Rcpp::export(rng=false)]]
bool bqs_can_fast_encode(SEXP schema_xptr) {
  struct ArrowSchema* sch = nanoarrow_schema_from_xptr(schema_xptr);
  return bqs_ipc::can_fast_encode_schema(sch);
}

// Fast zero-copy + multi-threaded conversion of standard flat data.frame columns
// to nanoarrow_array + nanoarrow_schema external pointers. Unsupported/nested
// columns return R_NilValue so R falls back cleanly to bqs_col_to_arrow_array().
// [[Rcpp::export(rng=false)]]
Rcpp::List bqs_fast_df_columns(Rcpp::List df,
                               Rcpp::CharacterVector col_names,
                               Rcpp::CharacterVector target_types,
                               Rcpp::CharacterVector target_modes,
                               int n_rows,
                               int n_threads = 0) {
  int p = df.size();
  Rcpp::List out_arrays(p);
  Rcpp::List out_schemas(p);
  if (p == 0) {
    return Rcpp::List::create(
      Rcpp::Named("arrays") = out_arrays,
      Rcpp::Named("schemas") = out_schemas
    );
  }

  std::vector<FastColTask> numeric_tasks;
  numeric_tasks.reserve(static_cast<size_t>(p));
  std::vector<SEXP> zc_sexps(static_cast<size_t>(p), R_NilValue);
  std::vector<std::string> col_formats(static_cast<size_t>(p));
  std::vector<int> n_bufs_vec(static_cast<size_t>(p), 2);
  std::vector<OwnedArrayPriv*> privs(static_cast<size_t>(p), nullptr);
  std::vector<int64_t> null_counts(static_cast<size_t>(p), 0);

  for (int j = 0; j < p; j++) {
    SEXP col = df[j];
    std::string mode = Rcpp::as<std::string>(target_modes[j]);
    std::string type = Rcpp::as<std::string>(target_types[j]);
    if (mode == "REPEATED" || TYPEOF(col) == VECSXP || Rf_inherits(col, "data.frame")) {
      out_arrays[j] = R_NilValue;
      out_schemas[j] = R_NilValue;
      continue;
    }

    FastColKind kind = FCOL_UNSUPPORTED;
    std::string fmt;
    int n_bufs = 2;
    bool zero_copy = false;

    if (TYPEOF(col) == INTSXP && Rf_isFactor(col) && (type.empty() || type == "STRING")) {
      OwnedArrayPriv* priv = new OwnedArrayPriv();
      int64_t nc = 0;
      if (build_factor_col_main_thread(col, n_rows, priv, &nc, &fmt)) {
        privs[static_cast<size_t>(j)] = priv;
        null_counts[static_cast<size_t>(j)] = nc;
        col_formats[static_cast<size_t>(j)] = fmt;
        n_bufs_vec[static_cast<size_t>(j)] = 3;
        continue;
      }
      delete priv;
    } else if (TYPEOF(col) == STRSXP &&
               (!Rf_isObject(col) || Rf_inherits(col, "wk_wkt")) &&
               (type.empty() || type == "STRING" || type == "GEOGRAPHY")) {
      OwnedArrayPriv* priv = new OwnedArrayPriv();
      int64_t nc = 0;
      build_utf8_col_main_thread(col, n_rows, priv, &nc, &fmt);
      privs[static_cast<size_t>(j)] = priv;
      null_counts[static_cast<size_t>(j)] = nc;
      col_formats[static_cast<size_t>(j)] = fmt;
      n_bufs_vec[static_cast<size_t>(j)] = 3;
      continue;
    } else if (TYPEOF(col) == INTSXP && Rf_inherits(col, "Date") &&
               (type.empty() || type == "DATE")) {
      kind = FCOL_DATE_INT_ZC;
      fmt = "tdD";
      zero_copy = true;
    } else if (TYPEOF(col) == REALSXP && Rf_inherits(col, "Date") &&
               (type.empty() || type == "DATE")) {
      kind = FCOL_DATE_DBL;
      fmt = "tdD";
    } else if (TYPEOF(col) == REALSXP && Rf_inherits(col, "POSIXct") &&
               (type.empty() || type == "TIMESTAMP" || type == "DATETIME")) {
      kind = (type == "DATETIME") ? FCOL_POSIXCT_DT : FCOL_POSIXCT_TS;
      fmt = (type == "DATETIME") ? "tsu:" : "tsu:UTC";
    } else if (TYPEOF(col) == REALSXP && Rf_inherits(col, "integer64") &&
               (type.empty() || type == "INT64")) {
      kind = FCOL_INT64_ZC;
      fmt = "l";
      zero_copy = true;
    } else if (TYPEOF(col) == INTSXP && !Rf_isObject(col)) {
      if (type.empty() || type == "INT64") {
        kind = FCOL_INT32_ZC;
        fmt = "i";
        zero_copy = true;
      } else if (type == "FLOAT64") {
        kind = FCOL_FLOAT64_FROM_INT;
        fmt = "g";
      }
    } else if (TYPEOF(col) == REALSXP && !Rf_isObject(col) &&
               (type.empty() || type == "FLOAT64")) {
      kind = FCOL_FLOAT64_ZC;
      fmt = "g";
      zero_copy = true;
    } else if (TYPEOF(col) == LGLSXP && !Rf_isObject(col) &&
               (type.empty() || type == "BOOL")) {
      kind = FCOL_BOOL;
      fmt = "b";
    }

    if (kind == FCOL_UNSUPPORTED) {
      out_arrays[j] = R_NilValue;
      out_schemas[j] = R_NilValue;
      continue;
    }

    OwnedArrayPriv* priv = new OwnedArrayPriv();
    privs[static_cast<size_t>(j)] = priv;
    col_formats[static_cast<size_t>(j)] = fmt;
    n_bufs_vec[static_cast<size_t>(j)] = n_bufs;
    if (zero_copy) zc_sexps[static_cast<size_t>(j)] = col;

    FastColTask t;
    t.col_idx = j;
    t.kind = kind;
    t.n = n_rows;
    t.priv = priv;
    if (TYPEOF(col) == INTSXP) t.r_ptr = INTEGER(col);
    else if (TYPEOF(col) == REALSXP) t.r_ptr = REAL(col);
    else if (TYPEOF(col) == LGLSXP) t.r_ptr = LOGICAL(col);
    numeric_tasks.push_back(t);
  }

  // Run numeric/bitmap scans & conversions in parallel across C++ threads
  if (!numeric_tasks.empty()) {
    if (n_threads <= 0) {
      unsigned int hw = std::thread::hardware_concurrency();
      n_threads = hw > 0 ? static_cast<int>(std::min(hw, 16u)) : 4;
    }
    size_t m = numeric_tasks.size();
    int n_workers = (static_cast<int64_t>(n_rows) * static_cast<int64_t>(m) >= 65536)
                      ? static_cast<int>(std::min<size_t>(static_cast<size_t>(std::max(1, n_threads)), m))
                      : 1;
    if (n_workers <= 1) {
      for (size_t k = 0; k < m; k++) execute_numeric_col_task(numeric_tasks[k]);
    } else {
      std::atomic<size_t> next_k{0};
      auto worker = [&]() {
        while (true) {
          size_t k = next_k.fetch_add(1);
          if (k >= m) break;
          execute_numeric_col_task(numeric_tasks[k]);
        }
      };
      std::vector<std::thread> pool;
      pool.reserve(static_cast<size_t>(n_workers - 1));
      for (int w = 1; w < n_workers; w++) pool.emplace_back(worker);
      worker();
      for (auto& th : pool) th.join();
    }
    for (const auto& t : numeric_tasks) {
      null_counts[static_cast<size_t>(t.col_idx)] = t.null_count;
    }
  }

  // Wrap completed columns into nanoarrow_schema + nanoarrow_array XPtrs on main thread
  for (int j = 0; j < p; j++) {
    OwnedArrayPriv* priv = privs[static_cast<size_t>(j)];
    if (!priv) continue;

    std::string nm = Rcpp::as<std::string>(col_names[j]);
    SEXP sch_xptr = PROTECT(make_leaf_schema_xptr(col_formats[static_cast<size_t>(j)].c_str(), nm.c_str()));
    SEXP arr_xptr = PROTECT(nanoarrow_array_owning_xptr());
    struct ArrowArray* arr = nanoarrow_output_array_from_xptr(arr_xptr);

    arr->length = n_rows;
    arr->null_count = null_counts[static_cast<size_t>(j)];
    arr->offset = 0;
    arr->n_buffers = n_bufs_vec[static_cast<size_t>(j)];
    arr->n_children = 0;
    arr->buffers = priv->buffers;
    arr->children = nullptr;
    arr->dictionary = nullptr;
    arr->private_data = priv;
    arr->release = release_owned_array;

    if (zc_sexps[static_cast<size_t>(j)] != R_NilValue) {
      R_SetExternalPtrProtected(arr_xptr, zc_sexps[static_cast<size_t>(j)]);
    }
    R_SetExternalPtrTag(arr_xptr, sch_xptr);

    out_arrays[j] = arr_xptr;
    out_schemas[j] = sch_xptr;
    UNPROTECT(2);
  }

  return Rcpp::List::create(
    Rcpp::Named("arrays") = out_arrays,
    Rcpp::Named("schemas") = out_schemas
  );
}

// Encode a single root ArrowArray + ArrowSchema + serialized Schema IPC message
// into a complete Arrow IPC stream RawVector in C++ (parallel across batches).
// [[Rcpp::export(rng=false)]]
Rcpp::RawVector bqs_encode_arrow_ipc_raw(SEXP schema_xptr,
                                         SEXP array_xptr,
                                         Rcpp::RawVector schema_ipc,
                                         int row_batch_size,
                                         bool use_zstd = false,
                                         int n_threads = 0) {
  struct ArrowSchema* schema = nanoarrow_schema_from_xptr(schema_xptr);
  struct ArrowArray* arr = nanoarrow_array_from_xptr(array_xptr);
  if (arr->length <= 0) {
    return Rcpp::RawVector(0);
  }

  std::vector<ArrowArray> src_arrays(1);
  src_arrays[0] = *arr; // Shallow view; not released here

  const int64_t n = arr->length;
  const int64_t total_uncompressed_bytes = bqs_ipc::estimate_batch_bytes(schema, arr);
  const int64_t max_batch_bytes = 5LL * 1024LL * 1024LL;
  double bytes_per_row = std::max(16.0, static_cast<double>(total_uncompressed_bytes) / static_cast<double>(n));
  int64_t max_rows_by_bytes = std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(max_batch_bytes) / bytes_per_row));
  int64_t step = std::min<int64_t>(std::max<int64_t>(1, row_batch_size), max_rows_by_bytes);

  const int64_t hard_slice_bytes = 10LL * 1024LL * 1024LL;
  std::vector<bqs_ipc::BatchSliceSpec> specs;
  int64_t s = 0;
  while (s < n) {
    int64_t len = std::min<int64_t>(step, n - s);
    while (len > 1) {
      int64_t slice_bytes = 0;
      for (int64_t j = 0; j < schema->n_children; j++) {
        slice_bytes += bqs_ipc::estimate_field_bytes(
          schema->children[j], arr->children[j], arr->offset + s, len);
      }
      if (slice_bytes <= hard_slice_bytes) break;
      len = std::max<int64_t>(1, len / 2);
    }
    specs.push_back({0u, s, len});
    s += len;
  }

  std::vector<std::string> batches;
  std::string err;
  if (!bqs_ipc::encode_batches_parallel(schema, src_arrays, specs, use_zstd, n_threads, &batches, &err)) {
    Rcpp::stop(err.c_str());
  }

  // Strip trailing 8-byte EOS marker from schema_ipc if present
  size_t sch_len = static_cast<size_t>(schema_ipc.size());
  if (sch_len >= 8) {
    uint32_t cont = 0;
    int32_t mlen = 0;
    std::memcpy(&cont, RAW(schema_ipc) + sch_len - 8, 4);
    std::memcpy(&mlen, RAW(schema_ipc) + sch_len - 4, 4);
    if (cont == 0xFFFFFFFFu && mlen == 0) {
      sch_len -= 8;
    }
  }

  size_t total_size = sch_len + 8; // +8 for EOS marker
  for (const auto& b : batches) total_size += b.size();

  Rcpp::RawVector out(total_size);
  uint8_t* dst = RAW(out);
  if (sch_len > 0) {
    std::memcpy(dst, RAW(schema_ipc), sch_len);
    dst += sch_len;
  }
  for (const auto& b : batches) {
    std::memcpy(dst, b.data(), b.size());
    dst += b.size();
  }
  const uint32_t eos_cont = 0xFFFFFFFFu;
  const int32_t eos_len = 0;
  std::memcpy(dst, &eos_cont, 4);
  std::memcpy(dst + 4, &eos_len, 4);
  return out;
}

// -- Parallel Direct C++ DecodedBatches -> R Tibble/DataFrame Converter -------

namespace {

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

static inline uint64_t fast_hash_str(const char* p, int64_t len) {
  if (len <= 0) return 0x9e3779b97f4a7c15ULL;
  uint64_t h = static_cast<uint64_t>(len) * 0x9e3779b97f4a7c15ULL;
  if (len <= 8) {
    uint64_t v = 0;
    if (len >= 4) {
      uint32_t a, b;
      std::memcpy(&a, p, 4);
      std::memcpy(&b, p + len - 4, 4);
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
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h ^= w;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= (h >> 31);
  }
  uint64_t tail;
  std::memcpy(&tail, p + len - 8, 8);
  h ^= tail;
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

static void run_numeric_slice_task(NumericSliceTask& task) {
  const DirectColSpec& c = *task.col;
  const size_t n_idx = c.node_idx;
  const size_t b_idx = c.buf_idx;
  double na_dbl = 0.0;
  std::memcpy(&na_dbl, &task.na_real_bits, sizeof(double));

  for (size_t s = task.b_start; s < task.b_end; s++) {
    const DecodedBatchSlice& sl = task.slices[s];
    const bqs_ipc::DecodedBatch& batch = *sl.batch;
    const int64_t len = sl.row_len;
    if (len <= 0) continue;
    const int64_t dst_off = sl.dst_offset;

    const bqs_ipc::FieldNodeDesc& nd = batch.nodes[n_idx];
    const bqs_ipc::BufferDesc& vbuf = batch.buffers[b_idx];
    const bqs_ipc::BufferDesc& dbuf = batch.buffers[b_idx + 1];
    const uint8_t* valid = (nd.null_count > 0 && vbuf.length > 0)
                             ? (batch.body_ptr + vbuf.offset)
                             : nullptr;
    const uint8_t* raw = (dbuf.length > 0) ? (batch.body_ptr + dbuf.offset) : nullptr;

    switch (c.kind) {
      case DCOL_FLOAT64: {
        double* dst = static_cast<double*>(c.out_ptr) + dst_off;
        if (!valid) {
          std::memcpy(dst, raw, static_cast<size_t>(len) * sizeof(double));
        } else {
          for (int64_t i = 0; i < len; i++) {
            if ((valid[i >> 3] >> (i & 7)) & 1) {
              dst[i] = bqs_ipc::load_unaligned<double>(raw + i * sizeof(double));
            } else {
              std::memcpy(dst + i, &task.na_real_bits, sizeof(uint64_t));
            }
          }
        }
        break;
      }
      case DCOL_FLOAT32: {
        double* dst = static_cast<double*>(c.out_ptr) + dst_off;
        if (!valid) {
          for (int64_t i = 0; i < len; i++) {
            dst[i] = static_cast<double>(bqs_ipc::load_unaligned<float>(raw + i * sizeof(float)));
          }
        } else {
          for (int64_t i = 0; i < len; i++) {
            if ((valid[i >> 3] >> (i & 7)) & 1) {
              dst[i] = static_cast<double>(bqs_ipc::load_unaligned<float>(raw + i * sizeof(float)));
            } else {
              std::memcpy(dst + i, &task.na_real_bits, sizeof(uint64_t));
            }
          }
        }
        break;
      }
      case DCOL_INT64_TO_INT32: {
        int* dst = static_cast<int*>(c.out_ptr) + dst_off;
        bool local_ovf = false;
        if (!valid) {
          for (int64_t i = 0; i < len; i++) {
            int64_t v = bqs_ipc::load_unaligned<int64_t>(raw + i * sizeof(int64_t));
            if (v < -2147483647LL || v > 2147483647LL) {
              dst[i] = NA_INTEGER;
              local_ovf = true;
            } else {
              dst[i] = static_cast<int>(v);
            }
          }
        } else {
          for (int64_t i = 0; i < len; i++) {
            if ((valid[i >> 3] >> (i & 7)) & 1) {
              int64_t v = bqs_ipc::load_unaligned<int64_t>(raw + i * sizeof(int64_t));
              if (v < -2147483647LL || v > 2147483647LL) {
                dst[i] = NA_INTEGER;
                local_ovf = true;
              } else {
                dst[i] = static_cast<int>(v);
              }
            } else {
              dst[i] = NA_INTEGER;
            }
          }
        }
        if (local_ovf) task.overflow = true;
        break;
      }
      case DCOL_INT64_TO_INT64: {
        int64_t* dst = static_cast<int64_t*>(c.out_ptr) + dst_off;
        const int64_t na_i64 = static_cast<int64_t>(1ULL << 63);
        if (!valid) {
          std::memcpy(dst, raw, static_cast<size_t>(len) * sizeof(int64_t));
        } else {
          for (int64_t i = 0; i < len; i++) {
            dst[i] = ((valid[i >> 3] >> (i & 7)) & 1)
                       ? bqs_ipc::load_unaligned<int64_t>(raw + i * sizeof(int64_t))
                       : na_i64;
          }
        }
        break;
      }
      case DCOL_INT64_TO_DBL: {
        double* dst = static_cast<double*>(c.out_ptr) + dst_off;
        if (!valid) {
          for (int64_t i = 0; i < len; i++) {
            dst[i] = static_cast<double>(bqs_ipc::load_unaligned<int64_t>(raw + i * sizeof(int64_t)));
          }
        } else {
          for (int64_t i = 0; i < len; i++) {
            if ((valid[i >> 3] >> (i & 7)) & 1) {
              dst[i] = static_cast<double>(bqs_ipc::load_unaligned<int64_t>(raw + i * sizeof(int64_t)));
            } else {
              std::memcpy(dst + i, &task.na_real_bits, sizeof(uint64_t));
            }
          }
        }
        break;
      }
      case DCOL_INT32: {
        int* dst = static_cast<int*>(c.out_ptr) + dst_off;
        if (!valid) {
          std::memcpy(dst, raw, static_cast<size_t>(len) * sizeof(int32_t));
        } else {
          for (int64_t i = 0; i < len; i++) {
            dst[i] = ((valid[i >> 3] >> (i & 7)) & 1)
                       ? bqs_ipc::load_unaligned<int32_t>(raw + i * sizeof(int32_t))
                       : NA_INTEGER;
          }
        }
        break;
      }
      case DCOL_BOOL: {
        int* dst = static_cast<int*>(c.out_ptr) + dst_off;
        if (!valid) {
          int64_t i = 0;
          for (; i + 8 <= len; i += 8) {
            uint8_t byte = raw[i >> 3];
            dst[i + 0] = (byte >> 0) & 1;
            dst[i + 1] = (byte >> 1) & 1;
            dst[i + 2] = (byte >> 2) & 1;
            dst[i + 3] = (byte >> 3) & 1;
            dst[i + 4] = (byte >> 4) & 1;
            dst[i + 5] = (byte >> 5) & 1;
            dst[i + 6] = (byte >> 6) & 1;
            dst[i + 7] = (byte >> 7) & 1;
          }
          for (; i < len; i++) {
            dst[i] = (raw[i >> 3] >> (i & 7)) & 1;
          }
        } else {
          for (int64_t i = 0; i < len; i++) {
            if ((valid[i >> 3] >> (i & 7)) & 1) {
              dst[i] = (raw[i >> 3] >> (i & 7)) & 1;
            } else {
              dst[i] = NA_LOGICAL;
            }
          }
        }
        break;
      }
      case DCOL_DATE32: {
        double* dst = static_cast<double*>(c.out_ptr) + dst_off;
        if (!valid) {
          for (int64_t i = 0; i < len; i++) {
            dst[i] = static_cast<double>(bqs_ipc::load_unaligned<int32_t>(raw + i * sizeof(int32_t)));
          }
        } else {
          for (int64_t i = 0; i < len; i++) {
            if ((valid[i >> 3] >> (i & 7)) & 1) {
              dst[i] = static_cast<double>(bqs_ipc::load_unaligned<int32_t>(raw + i * sizeof(int32_t)));
            } else {
              std::memcpy(dst + i, &task.na_real_bits, sizeof(uint64_t));
            }
          }
        }
        break;
      }
      case DCOL_TIME64_US:
      case DCOL_TIMESTAMP_US: {
        double* dst = static_cast<double*>(c.out_ptr) + dst_off;
        const double inv_us = 1e-6;
        if (!valid) {
          for (int64_t i = 0; i < len; i++) {
            dst[i] = static_cast<double>(bqs_ipc::load_unaligned<int64_t>(raw + i * sizeof(int64_t))) * inv_us;
          }
        } else {
          for (int64_t i = 0; i < len; i++) {
            if ((valid[i >> 3] >> (i & 7)) & 1) {
              dst[i] = static_cast<double>(bqs_ipc::load_unaligned<int64_t>(raw + i * sizeof(int64_t))) * inv_us;
            } else {
              std::memcpy(dst + i, &task.na_real_bits, sizeof(uint64_t));
            }
          }
        }
        break;
      }
      case DCOL_DECIMAL128: {
#if defined(__SIZEOF_INT128__)
        double* dst = static_cast<double*>(c.out_ptr) + dst_off;
        const double mult = c.dec_scale_mult;
        if (!valid) {
          for (int64_t i = 0; i < len; i++) {
            __extension__ __int128 v;
            std::memcpy(&v, raw + i * 16, 16);
            dst[i] = static_cast<double>(v) * mult;
          }
        } else {
          for (int64_t i = 0; i < len; i++) {
            if ((valid[i >> 3] >> (i & 7)) & 1) {
              __extension__ __int128 v;
              std::memcpy(&v, raw + i * 16, 16);
              dst[i] = static_cast<double>(v) * mult;
            } else {
              std::memcpy(dst + i, &task.na_real_bits, sizeof(uint64_t));
            }
          }
        }
#else
        (void)na_dbl;
#endif
        break;
      }
      default:
        break;
    }
  }
}

static void run_string_chunk_task(StringChunkTask& task) {
  const DirectColSpec& c = *task.col;
  const size_t n_idx = c.node_idx;
  const size_t b_idx = c.buf_idx;
  const bool is_large = (c.kind == DCOL_LARGE_UTF8);

  task.codes.resize(static_cast<size_t>(task.total_rows));
  uint32_t* codes = task.codes.data();

  static const uint32_t EMPTY_SLOT = 0xFFFFFFFFu;
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

    const uint8_t* valid = (nd.null_count > 0 && vbuf.length > 0)
                             ? (batch.body_ptr + vbuf.offset)
                             : nullptr;
    const char* data = (dbuf.length > 0)
                         ? reinterpret_cast<const char*>(batch.body_ptr + dbuf.offset)
                         : "";

    if (valid) task.has_nulls = true;

    if (!is_large) {
      const uint8_t* op = batch.body_ptr + obuf.offset;
      if (!valid) {
        for (int64_t i = 0; i < len; i++) {
          int32_t start = bqs_ipc::load_unaligned<int32_t>(op + i * 4);
          int32_t slen = bqs_ipc::load_unaligned<int32_t>(op + (i + 1) * 4) - start;
          codes[out_pos++] = intern_local(data + start, slen);
          if (!checked_cardinality && out_pos >= 2048) {
            checked_cardinality = true;
            if (static_cast<int64_t>(task.entries.size()) > (out_pos * 3) / 4) {
              task.use_dict = false;
              std::vector<StrDictEntry>().swap(task.entries);
              std::vector<uint32_t>().swap(task.codes);
              return;
            }
          }
        }
      } else {
        for (int64_t i = 0; i < len; i++) {
          if ((valid[i >> 3] >> (i & 7)) & 1) {
            int32_t start = bqs_ipc::load_unaligned<int32_t>(op + i * 4);
            int32_t slen = bqs_ipc::load_unaligned<int32_t>(op + (i + 1) * 4) - start;
            codes[out_pos++] = intern_local(data + start, slen);
          } else {
            codes[out_pos++] = 0xFFFFFFFFu;
          }
          if (!checked_cardinality && out_pos >= 2048) {
            checked_cardinality = true;
            if (static_cast<int64_t>(task.entries.size()) > (out_pos * 3) / 4) {
              task.use_dict = false;
              std::vector<StrDictEntry>().swap(task.entries);
              std::vector<uint32_t>().swap(task.codes);
              return;
            }
          }
        }
      }
    } else {
      const uint8_t* op = batch.body_ptr + obuf.offset;
      for (int64_t i = 0; i < len; i++) {
        if (!valid || ((valid[i >> 3] >> (i & 7)) & 1)) {
          int64_t start = bqs_ipc::load_unaligned<int64_t>(op + i * 8);
          int64_t slen64 = bqs_ipc::load_unaligned<int64_t>(op + (i + 1) * 8) - start;
          if (slen64 > 2147483647LL) {
            task.use_dict = false;
            std::vector<StrDictEntry>().swap(task.entries);
            std::vector<uint32_t>().swap(task.codes);
            return;
          }
          codes[out_pos++] = intern_local(data + start, static_cast<int32_t>(slen64));
        } else {
          codes[out_pos++] = 0xFFFFFFFFu;
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

struct FallbackStreamData {
  ArrowSchema root_schema;
  std::vector<ArrowSchema*> child_schema_ptrs;
  std::vector<ArrowArray> batches;
  size_t batch_idx = 0;

  ~FallbackStreamData() {
    for (size_t i = batch_idx; i < batches.size(); i++) {
      if (batches[i].release) batches[i].release(&batches[i]);
    }
  }

  static int get_schema(struct ArrowArrayStream* s, struct ArrowSchema* out) {
    FallbackStreamData* self = static_cast<FallbackStreamData*>(s->private_data);
    *out = self->root_schema;
    out->release = noop_release_schema;
    return 0;
  }

  static int get_next(struct ArrowArrayStream* s, struct ArrowArray* out) {
    FallbackStreamData* self = static_cast<FallbackStreamData*>(s->private_data);
    if (self->batch_idx >= self->batches.size()) {
      out->release = nullptr;
      return 0;
    }
    *out = self->batches[self->batch_idx];
    self->batches[self->batch_idx].release = nullptr;
    self->batch_idx++;
    return 0;
  }

  static const char* get_last_error(struct ArrowArrayStream*) { return nullptr; }
  static void release(struct ArrowArrayStream* s) { s->release = nullptr; }
};

} // namespace

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
    Rcpp::Function as_bigint) {
  struct ArrowSchema* schema = nanoarrow_schema_from_xptr(schema_xptr);

  bool has_geo = false, has_bytes = false;
  schema_flags(schema, std::string(), &has_geo, &has_bytes);
  if (has_geo) check_pkg_fn("wk", "GEOGRAPHY");
  if (has_bytes) check_pkg_fn("blob", "BYTES");

  // Flatten non-empty batches in stream order, honoring n_max
  std::vector<DecodedBatchSlice> slices;
  int64_t total_rows = 0;
  for (size_t s = 0; s < stream_batches->size(); s++) {
    auto& sb = (*stream_batches)[s];
    for (size_t b = 0; b < sb.size(); b++) {
      if (n_max >= 0 && total_rows >= n_max) break;
      int64_t blen = sb[b].length;
      if (blen <= 0) continue;
      int64_t take = (n_max >= 0) ? std::min<int64_t>(blen, n_max - total_rows) : blen;
      slices.push_back({&sb[b], take, total_rows});
      total_rows += take;
    }
  }

  const int64_t n_cols = schema->n_children;

  // Handle 0-row edge case via existing schema ptype path
  if (total_rows == 0) {
    for (auto& sb : *stream_batches) sb.clear();
    FallbackStreamData empty_data;
    empty_data.child_schema_ptrs.resize(static_cast<size_t>(n_cols));
    for (int64_t j = 0; j < n_cols; j++) {
      empty_data.child_schema_ptrs[static_cast<size_t>(j)] = schema->children[j];
    }
    empty_data.root_schema = *schema;
    empty_data.root_schema.release = noop_release_schema;

    SEXP empty_stream_xptr = PROTECT(nanoarrow_array_stream_owning_xptr());
    struct ArrowArrayStream* es = reinterpret_cast<struct ArrowArrayStream*>(R_ExternalPtrAddr(empty_stream_xptr));
    es->get_schema = FallbackStreamData::get_schema;
    es->get_next = FallbackStreamData::get_next;
    es->get_last_error = FallbackStreamData::get_last_error;
    es->release = FallbackStreamData::release;
    es->private_data = &empty_data;

    SEXP ptype = PROTECT(int64_ptype_fn(schema_xptr));
    SEXP df = PROTECT(bqs_convert_stream(empty_stream_xptr, ptype, bigint, na_convert_stream, na_convert_stream_quiet));
    SEXP tb = PROTECT(bqs_postprocess(df, schema_xptr, bigint, as_bigint));
    if (as_data_frame) {
      Rf_setAttrib(tb, R_ClassSymbol, Rf_mkString("data.frame"));
    } else if (!Rf_inherits(tb, "tbl_df")) {
      SEXP cls_tbl = PROTECT(Rf_allocVector(STRSXP, 3));
      SET_STRING_ELT(cls_tbl, 0, Rf_mkChar("tbl_df"));
      SET_STRING_ELT(cls_tbl, 1, Rf_mkChar("tbl"));
      SET_STRING_ELT(cls_tbl, 2, Rf_mkChar("data.frame"));
      Rf_setAttrib(tb, R_ClassSymbol, cls_tbl);
      UNPROTECT(1);
    }
    UNPROTECT(4);
    return tb;
  }

  // Compute exact pre-order FieldNode and Buffer index for every top-level column
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

  // Classify columns and allocate output R vectors on the main thread
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
    } else if (std::strcmp(fmt, "g") == 0 && ext.empty()) {
      sp.kind = DCOL_FLOAT64;
      SEXP v = PROTECT(Rf_allocVector(REALSXP, total_rows));
      sp.out_ptr = REAL(v);
      out_cols[j] = v;
      UNPROTECT(1);
      numeric_cols.push_back(static_cast<int>(j));
    } else if (std::strcmp(fmt, "f") == 0 && ext.empty()) {
      sp.kind = DCOL_FLOAT32;
      SEXP v = PROTECT(Rf_allocVector(REALSXP, total_rows));
      sp.out_ptr = REAL(v);
      out_cols[j] = v;
      UNPROTECT(1);
      numeric_cols.push_back(static_cast<int>(j));
    } else if (std::strcmp(fmt, "l") == 0 && ext.empty() && bigint != "character") {
      if (bigint == "integer") {
        sp.kind = DCOL_INT64_TO_INT32;
        SEXP v = PROTECT(Rf_allocVector(INTSXP, total_rows));
        sp.out_ptr = INTEGER(v);
        out_cols[j] = v;
        UNPROTECT(1);
      } else if (bigint == "integer64") {
        sp.kind = DCOL_INT64_TO_INT64;
        SEXP v = PROTECT(Rf_allocVector(REALSXP, total_rows));
        Rf_setAttrib(v, R_ClassSymbol, cls_int64);
        sp.out_ptr = REAL(v);
        out_cols[j] = v;
        UNPROTECT(1);
      } else {
        sp.kind = DCOL_INT64_TO_DBL;
        SEXP v = PROTECT(Rf_allocVector(REALSXP, total_rows));
        sp.out_ptr = REAL(v);
        out_cols[j] = v;
        UNPROTECT(1);
      }
      numeric_cols.push_back(static_cast<int>(j));
    } else if (std::strcmp(fmt, "i") == 0 && ext.empty()) {
      sp.kind = DCOL_INT32;
      SEXP v = PROTECT(Rf_allocVector(INTSXP, total_rows));
      sp.out_ptr = INTEGER(v);
      out_cols[j] = v;
      UNPROTECT(1);
      numeric_cols.push_back(static_cast<int>(j));
    } else if (std::strcmp(fmt, "b") == 0 && ext.empty()) {
      sp.kind = DCOL_BOOL;
      SEXP v = PROTECT(Rf_allocVector(LGLSXP, total_rows));
      sp.out_ptr = LOGICAL(v);
      out_cols[j] = v;
      UNPROTECT(1);
      numeric_cols.push_back(static_cast<int>(j));
    } else if (std::strcmp(fmt, "tdD") == 0 && ext.empty()) {
      sp.kind = DCOL_DATE32;
      SEXP v = PROTECT(Rf_allocVector(REALSXP, total_rows));
      Rf_setAttrib(v, R_ClassSymbol, cls_date);
      sp.out_ptr = REAL(v);
      out_cols[j] = v;
      UNPROTECT(1);
      numeric_cols.push_back(static_cast<int>(j));
    } else if (std::strcmp(fmt, "ttu") == 0 && ext.empty()) {
      sp.kind = DCOL_TIME64_US;
      SEXP v = PROTECT(Rf_allocVector(REALSXP, total_rows));
      Rf_setAttrib(v, sym_units, str_secs);
      Rf_setAttrib(v, R_ClassSymbol, cls_hms);
      sp.out_ptr = REAL(v);
      out_cols[j] = v;
      UNPROTECT(1);
      numeric_cols.push_back(static_cast<int>(j));
    } else if (std::strncmp(fmt, "tsu:", 4) == 0 && (ext.empty() || ext == EXT_DATETIME)) {
      sp.kind = DCOL_TIMESTAMP_US;
      SEXP v = PROTECT(Rf_allocVector(REALSXP, total_rows));
      Rf_setAttrib(v, R_ClassSymbol, cls_posix);
      Rf_setAttrib(v, sym_tzone, str_utc);
      sp.out_ptr = REAL(v);
      out_cols[j] = v;
      UNPROTECT(1);
      numeric_cols.push_back(static_cast<int>(j));
    } else if (
#if defined(__SIZEOF_INT128__)
        ext.empty() && parse_decimal128_scale(fmt, &dec_scale)
#else
        false
#endif
    ) {
      sp.kind = DCOL_DECIMAL128;
      sp.dec_scale_mult = std::pow(10.0, -dec_scale);
      SEXP v = PROTECT(Rf_allocVector(REALSXP, total_rows));
      sp.out_ptr = REAL(v);
      out_cols[j] = v;
      UNPROTECT(1);
      numeric_cols.push_back(static_cast<int>(j));
    } else if ((std::strcmp(fmt, "u") == 0 || std::strcmp(fmt, "U") == 0) &&
               (ext.empty() || ext == EXT_GEOGRAPHY)) {
      sp.kind = (std::strcmp(fmt, "U") == 0) ? DCOL_LARGE_UTF8 : DCOL_UTF8;
      string_cols.push_back(static_cast<int>(j));
    } else {
      sp.kind = DCOL_FALLBACK;
      fallback_cols.push_back(static_cast<int>(j));
    }
  }

  // Validate per-column buffer capacities and monotonic offsets across all slices before launching C++ workers
  for (int64_t j = 0; j < n_cols; j++) {
    const DirectColSpec& sp = direct_specs[static_cast<size_t>(j)];
    if (sp.kind == DCOL_FALLBACK) {
      for (const auto& sl : slices) {
        const bqs_ipc::DecodedBatch& batch = *sl.batch;
        size_t npos = sp.node_idx;
        size_t bpos = sp.buf_idx;
        std::string val_err;
        if (!bqs_ipc::validate_borrowed_field_buffers(
              schema->children[j], batch, batch.length, true, &npos, &bpos, &val_err)) {
          UNPROTECT(7);
          Rcpp::stop(val_err.c_str());
        }
      }
      continue;
    }
    for (const auto& sl : slices) {
      const bqs_ipc::DecodedBatch& batch = *sl.batch;
      const int64_t len = sl.row_len;
      if (len <= 0) continue;
      const bqs_ipc::FieldNodeDesc& nd = batch.nodes[sp.node_idx];
      if (nd.length < len || nd.null_count > nd.length) {
        UNPROTECT(7);
        Rcpp::stop("Invalid Arrow IPC RecordBatch: inconsistent FieldNode length/null_count.");
      }
      const bqs_ipc::BufferDesc& vbuf = batch.buffers[sp.buf_idx];
      if (nd.null_count > 0 && (vbuf.length < ((len + 7) >> 3) || !batch.body_ptr)) {
        UNPROTECT(7);
        Rcpp::stop("Invalid Arrow IPC RecordBatch: validity bitmap shorter than row count.");
      }
      if (sp.kind == DCOL_UTF8 || sp.kind == DCOL_LARGE_UTF8) {
        const bqs_ipc::BufferDesc& obuf = batch.buffers[sp.buf_idx + 1];
        const bqs_ipc::BufferDesc& dbuf = batch.buffers[sp.buf_idx + 2];
        int64_t elem_sz = (sp.kind == DCOL_LARGE_UTF8) ? 8 : 4;
        if (obuf.length < (len + 1) * elem_sz || !batch.body_ptr) {
          UNPROTECT(7);
          Rcpp::stop("Invalid Arrow IPC RecordBatch: string offset buffer shorter than row count.");
        }
        const uint8_t* op = batch.body_ptr + obuf.offset;
        int64_t last_off = 0;
        if (sp.kind == DCOL_UTF8) {
          int32_t prev = bqs_ipc::load_unaligned<int32_t>(op);
          if (prev < 0) {
            UNPROTECT(7);
            Rcpp::stop("Invalid Arrow IPC RecordBatch: negative string offset.");
          }
          for (int64_t i = 1; i <= len; i++) {
            int32_t cur = bqs_ipc::load_unaligned<int32_t>(op + i * 4);
            if (cur < prev) {
              UNPROTECT(7);
              Rcpp::stop("Invalid Arrow IPC RecordBatch: non-monotonic string offsets.");
            }
            prev = cur;
          }
          last_off = prev;
        } else {
          int64_t prev = bqs_ipc::load_unaligned<int64_t>(op);
          if (prev < 0) {
            UNPROTECT(7);
            Rcpp::stop("Invalid Arrow IPC RecordBatch: negative string offset.");
          }
          for (int64_t i = 1; i <= len; i++) {
            int64_t cur = bqs_ipc::load_unaligned<int64_t>(op + i * 8);
            if (cur < prev) {
              UNPROTECT(7);
              Rcpp::stop("Invalid Arrow IPC RecordBatch: non-monotonic string offsets.");
            }
            prev = cur;
          }
          last_off = prev;
        }
        if (last_off > dbuf.length || (last_off > 0 && !batch.body_ptr)) {
          UNPROTECT(7);
          Rcpp::stop("Invalid Arrow IPC RecordBatch: string offsets out of data buffer bounds.");
        }
      } else {
        const bqs_ipc::BufferDesc& dbuf = batch.buffers[sp.buf_idx + 1];
        int64_t req_bytes = 0;
        if (sp.kind == DCOL_BOOL) req_bytes = (len + 7) >> 3;
        else if (sp.kind == DCOL_INT32 || sp.kind == DCOL_FLOAT32 || sp.kind == DCOL_DATE32) req_bytes = len * 4;
        else if (sp.kind == DCOL_DECIMAL128) req_bytes = len * 16;
        else req_bytes = len * 8;
        if (dbuf.length < req_bytes || (req_bytes > 0 && !batch.body_ptr)) {
          UNPROTECT(7);
          Rcpp::stop("Invalid Arrow IPC RecordBatch: data buffer shorter than row count.");
        }
      }
    }
  }

  // Build fine-grained parallel tasks for numeric/bool/temporal/decimal AND string columns
  uint64_t na_real_bits = 0;
  double r_na = NA_REAL;
  std::memcpy(&na_real_bits, &r_na, sizeof(uint64_t));

  // Partition `slices` into ~128K-row groups for numeric tasks and ~200K-row groups for string tasks
  std::vector<std::pair<size_t, size_t>> num_groups;
  {
    size_t gs = 0;
    int64_t acc = 0;
    for (size_t s = 0; s < slices.size(); s++) {
      acc += slices[s].row_len;
      if (acc >= 131072 || s + 1 == slices.size()) {
        num_groups.emplace_back(gs, s + 1);
        gs = s + 1;
        acc = 0;
      }
    }
  }

  std::vector<NumericSliceTask> num_tasks;
  num_tasks.reserve(numeric_cols.size() * num_groups.size());
  for (int cidx : numeric_cols) {
    for (const auto& grp : num_groups) {
      NumericSliceTask t;
      t.col = &direct_specs[static_cast<size_t>(cidx)];
      t.slices = slices.data();
      t.b_start = grp.first;
      t.b_end = grp.second;
      t.na_real_bits = na_real_bits;
      num_tasks.push_back(t);
    }
  }

  std::vector<std::vector<StringChunkTask>> str_col_tasks(string_cols.size());
  std::vector<StringChunkTask*> flat_str_tasks;
  if (!string_cols.empty()) {
    std::vector<std::pair<size_t, size_t>> str_groups;
    size_t gs = 0;
    int64_t acc = 0;
    for (size_t s = 0; s < slices.size(); s++) {
      acc += slices[s].row_len;
      if (acc >= 196608 || s + 1 == slices.size()) {
        str_groups.emplace_back(gs, s + 1);
        gs = s + 1;
        acc = 0;
      }
    }
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

  // Execute ALL numeric and string hashing/dict tasks concurrently across C++ worker pool
  size_t total_tasks = num_tasks.size() + flat_str_tasks.size();
  if (total_tasks > 0) {
    if (n_threads <= 0) {
      unsigned int hw = std::thread::hardware_concurrency();
      n_threads = hw > 0 ? static_cast<int>(std::min(hw, 24u)) : 4;
    }
    int n_workers = (total_rows >= 8192 && total_tasks > 1)
                      ? static_cast<int>(std::min<size_t>(static_cast<size_t>(std::max(1, n_threads)), total_tasks))
                      : 1;
    std::atomic<size_t> next_t{0};
    auto worker_fn = [&]() {
      while (true) {
        size_t idx = next_t.fetch_add(1);
        if (idx >= total_tasks) break;
        if (idx < flat_str_tasks.size()) {
          run_string_chunk_task(*flat_str_tasks[idx]);
        } else {
          run_numeric_slice_task(num_tasks[idx - flat_str_tasks.size()]);
        }
      }
    };

    if (n_workers <= 1) {
      worker_fn();
    } else {
      std::vector<std::thread> pool;
      pool.reserve(static_cast<size_t>(n_workers - 1));
      for (int w = 1; w < n_workers; w++) pool.emplace_back(worker_fn);
      worker_fn();
      for (auto& th : pool) th.join();
    }
  }

  bool any_int_overflow = false;
  for (const auto& t : num_tasks) {
    if (t.overflow) {
      any_int_overflow = true;
      break;
    }
  }
  if (any_int_overflow) {
    Rf_warning("NAs produced by integer overflow");
  }

  // Finalize STRSXP columns on the main R thread using unique CHARSXP interning
  for (size_t sc = 0; sc < string_cols.size(); sc++) {
    int cidx = string_cols[sc];
    const DirectColSpec& sp = direct_specs[static_cast<size_t>(cidx)];
    auto& chunks = str_col_tasks[sc];

    SEXP str_vec = PROTECT(Rf_allocVector(STRSXP, total_rows));
    if (sp.is_geography) {
      Rf_setAttrib(str_vec, R_ClassSymbol, cls_wk);
    }

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
            if (code == 0xFFFFFFFFu) {
              SET_STRING_ELT(str_vec, base + i, NA_STRING);
            } else {
              SEXP val = resolved[code];
              if (val != R_BlankString) SET_STRING_ELT(str_vec, base + i, val);
            }
          }
        }
        std::vector<StrDictEntry>().swap(ch.entries);
        std::vector<uint32_t>().swap(ch.codes);
      } else {
        // High-cardinality direct fallback for this chunk
        const bool is_large = (sp.kind == DCOL_LARGE_UTF8);
        for (size_t s = ch.b_start; s < ch.b_end; s++) {
          const DecodedBatchSlice& sl = slices[s];
          const bqs_ipc::DecodedBatch& batch = *sl.batch;
          const int64_t len = sl.row_len;
          const int64_t dst_off = sl.dst_offset;
          const bqs_ipc::FieldNodeDesc& nd = batch.nodes[sp.node_idx];
          const bqs_ipc::BufferDesc& vbuf = batch.buffers[sp.buf_idx];
          const bqs_ipc::BufferDesc& obuf = batch.buffers[sp.buf_idx + 1];
          const bqs_ipc::BufferDesc& dbuf = batch.buffers[sp.buf_idx + 2];
          const uint8_t* valid = (nd.null_count > 0 && vbuf.length > 0)
                                   ? (batch.body_ptr + vbuf.offset)
                                   : nullptr;
          const char* data = (dbuf.length > 0)
                               ? reinterpret_cast<const char*>(batch.body_ptr + dbuf.offset)
                               : "";
          if (!is_large) {
            const uint8_t* op = batch.body_ptr + obuf.offset;
            for (int64_t i = 0; i < len; i++) {
              if (valid && !((valid[i >> 3] >> (i & 7)) & 1)) {
                SET_STRING_ELT(str_vec, dst_off + i, NA_STRING);
              } else {
                int32_t st = bqs_ipc::load_unaligned<int32_t>(op + i * 4);
                int32_t slen = bqs_ipc::load_unaligned<int32_t>(op + (i + 1) * 4) - st;
                if (slen > 0) {
                  SET_STRING_ELT(str_vec, dst_off + i, Rf_mkCharLenCE(data + st, slen, CE_UTF8));
                }
              }
            }
          } else {
            const uint8_t* op = batch.body_ptr + obuf.offset;
            for (int64_t i = 0; i < len; i++) {
              if (valid && !((valid[i >> 3] >> (i & 7)) & 1)) {
                SET_STRING_ELT(str_vec, dst_off + i, NA_STRING);
              } else {
                int64_t st = bqs_ipc::load_unaligned<int64_t>(op + i * 8);
                int64_t slen = bqs_ipc::load_unaligned<int64_t>(op + (i + 1) * 8) - st;
                if (slen > 0) {
                  SET_STRING_ELT(str_vec, dst_off + i, Rf_mkCharLenCE(data + st, static_cast<int>(slen), CE_UTF8));
                }
              }
            }
          }
        }
      }
    }

    out_cols[cidx] = str_vec;
    UNPROTECT(2); // guard, str_vec
  }

  // Handle any nested / special fallback columns zero-copy via bqs_convert_stream + bqs_postprocess
  if (!fallback_cols.empty()) {
    size_t nf = fallback_cols.size();
    FallbackStreamData fb_data;
    fb_data.child_schema_ptrs.resize(nf);
    for (size_t k = 0; k < nf; k++) {
      fb_data.child_schema_ptrs[k] = schema->children[fallback_cols[k]];
    }
    fb_data.root_schema = *schema;
    fb_data.root_schema.n_children = static_cast<int64_t>(nf);
    fb_data.root_schema.children = fb_data.child_schema_ptrs.data();
    fb_data.root_schema.release = noop_release_schema;

    fb_data.batches.resize(slices.size());
    for (size_t s = 0; s < slices.size(); s++) {
      const DecodedBatchSlice& sl = slices[s];
      ArrowArray& root_a = fb_data.batches[s];
      bqs_ipc::BorrowedArrayPriv* rpriv = new bqs_ipc::BorrowedArrayPriv();
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
    *sub_sch_ptr = fb_data.root_schema;
    sub_sch_ptr->release = noop_release_schema;

    SEXP sub_ptype = PROTECT(int64_ptype_fn(sub_schema_xptr));

    SEXP fb_stream_xptr = PROTECT(nanoarrow_array_stream_owning_xptr());
    struct ArrowArrayStream* fb_s = reinterpret_cast<struct ArrowArrayStream*>(R_ExternalPtrAddr(fb_stream_xptr));
    fb_s->get_schema = FallbackStreamData::get_schema;
    fb_s->get_next = FallbackStreamData::get_next;
    fb_s->get_last_error = FallbackStreamData::get_last_error;
    fb_s->release = FallbackStreamData::release;
    fb_s->private_data = &fb_data;

    SEXP fb_df = PROTECT(bqs_convert_stream(
      fb_stream_xptr, sub_ptype, bigint, na_convert_stream, na_convert_stream_quiet));
    SEXP fb_post = PROTECT(bqs_postprocess(fb_df, sub_schema_xptr, bigint, as_bigint));
    Rcpp::List fb_list(fb_post);
    for (size_t k = 0; k < nf; k++) {
      out_cols[fallback_cols[k]] = fb_list[k];
    }
    UNPROTECT(5);
  }

  // Free all C++ batch buffers before returning to R
  for (auto& sb : *stream_batches) {
    for (auto& b : sb) b.clear_memory();
    std::vector<bqs_ipc::DecodedBatch>().swap(sb);
  }

  out_cols.attr("names") = col_names;
  if (as_data_frame) {
    out_cols.attr("class") = "data.frame";
  } else {
    SEXP cls_tbl = PROTECT(Rf_allocVector(STRSXP, 3));
    SET_STRING_ELT(cls_tbl, 0, Rf_mkChar("tbl_df"));
    SET_STRING_ELT(cls_tbl, 1, Rf_mkChar("tbl"));
    SET_STRING_ELT(cls_tbl, 2, Rf_mkChar("data.frame"));
    Rf_setAttrib(out_cols, R_ClassSymbol, cls_tbl);
    UNPROTECT(1);
  }
  Rf_setAttrib(out_cols, R_RowNamesSymbol, compact_row_names(total_rows));

  UNPROTECT(7); // cls_int64, cls_date, cls_posix, cls_hms, cls_wk, str_utc, str_secs
  return out_cols;
}

// Decode an in-memory Arrow IPC stream RawVector directly through the in-flight
// RecordBatch decoder + parallel C++ Arrow-to-R converter (offline-testable).
// [[Rcpp::export(rng=false)]]
SEXP bqs_decode_arrow_ipc_raw(Rcpp::RawVector ipc_bytes,
                              double n_max,
                              std::string bigint,
                              bool as_data_frame,
                              int n_threads,
                              Rcpp::Function read_nanoarrow_fn,
                              Rcpp::Function int64_ptype_fn,
                              Rcpp::Function check_pkg_fn,
                              Rcpp::Function na_convert_stream,
                              Rcpp::Function na_convert_stream_quiet,
                              Rcpp::Function as_bigint) {
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

  Rcpp::RawVector schema_raw(schema_bytes.size());
  std::memcpy(RAW(schema_raw), schema_bytes.data(), schema_bytes.size());
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
    n_threads,
    int64_ptype_fn,
    check_pkg_fn,
    na_convert_stream,
    na_convert_stream_quiet,
    as_bigint));
  UNPROTECT(3);
  return res;
}


