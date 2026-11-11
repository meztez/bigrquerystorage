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
      __extension__ const __int128* vals = static_cast<const __int128*>(child->buffers[1]);
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

