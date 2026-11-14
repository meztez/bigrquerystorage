// [[Rcpp::depends(nanoarrow)]]
#include "bqs_arrow_ipc.h"
#include "bqs_parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool parse_decimal_string(const char* s, int scale, int bitwidth, uint8_t* out_bytes) {
  int byte_width = bitwidth / 8;
  std::memset(out_bytes, 0, static_cast<size_t>(byte_width));
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
    uint64_t carry = 1;
    for (int i = 0; i < 8; i++) {
      uint64_t cur = static_cast<uint64_t>(~limbs[i]) + carry;
      limbs[i] = static_cast<uint32_t>(cur & 0xFFFFFFFFULL);
      carry = cur >> 32;
    }
  }

  std::memcpy(out_bytes, limbs, static_cast<size_t>(byte_width));
  return true;
}

void encode_double_to_decimal(double v, int scale, int bitwidth, uint8_t* out_bytes) {
  int byte_width = bitwidth / 8;
  if (!R_finite(v)) {
    std::memset(out_bytes, 0, static_cast<size_t>(byte_width));
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

struct OwnedSchemaPriv {
  std::string format;
  std::string name;
};

void release_owned_schema(struct ArrowSchema* s) {
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

SEXP make_leaf_schema_xptr(const char* fmt, const char* nm) {
  SEXP xptr = PROTECT(nanoarrow_schema_owning_xptr());
  struct ArrowSchema* s = nanoarrow_output_schema_from_xptr(xptr);
  auto* priv = new OwnedSchemaPriv{fmt, nm};
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

void release_owned_array(struct ArrowArray* a) {
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
  FCOL_POSIXCT_DT
};

struct FastColTask {
  int col_idx = 0;
  FastColKind kind = FCOL_UNSUPPORTED;
  const void* r_ptr = nullptr;
  int64_t n = 0;
  int64_t null_count = 0;
  OwnedArrayPriv* priv = nullptr;
};

template <typename SrcT, typename IsValidFn>
inline void populate_validity_bitmap(const SrcT* src,
                                     int64_t n,
                                     int64_t nulls,
                                     OwnedArrayPriv* priv,
                                     IsValidFn&& is_valid) {
  if (nulls <= 0) return;
  priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
  uint8_t* v = priv->validity.data();
  for (int64_t i = 0; i < n; i++) {
    if (is_valid(src[i])) v[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
  }
  priv->buffers[0] = v;
}

template <typename SrcT, typename IsValidFn>
inline int64_t scan_zero_copy_col(const SrcT* src,
                                  int64_t n,
                                  OwnedArrayPriv* priv,
                                  IsValidFn&& is_valid) {
  int64_t nulls = 0;
  for (int64_t i = 0; i < n; i++) {
    if (!is_valid(src[i])) nulls++;
  }
  populate_validity_bitmap(src, n, nulls, priv, is_valid);
  priv->buffers[1] = src;
  return nulls;
}

template <typename SrcT, typename DstT, typename IsValidFn, typename ConvFn>
inline int64_t convert_numeric_col(const SrcT* src,
                                   int64_t n,
                                   OwnedArrayPriv* priv,
                                   IsValidFn&& is_valid,
                                   ConvFn&& conv) {
  priv->buf1.resize(static_cast<size_t>(n) * sizeof(DstT));
  DstT* dst = reinterpret_cast<DstT*>(priv->buf1.data());
  int64_t nulls = 0;
  for (int64_t i = 0; i < n; i++) {
    if (is_valid(src[i])) {
      dst[i] = conv(src[i]);
    } else {
      dst[i] = DstT(0);
      nulls++;
    }
  }
  populate_validity_bitmap(src, n, nulls, priv, is_valid);
  priv->buffers[1] = dst;
  return nulls;
}

void execute_numeric_col_task(FastColTask& t) {
  const int64_t n = t.n;
  OwnedArrayPriv* priv = t.priv;

  switch (t.kind) {
    case FCOL_INT32_ZC:
    case FCOL_DATE_INT_ZC: {
      t.null_count = scan_zero_copy_col(
        static_cast<const int*>(t.r_ptr), n, priv, [](int v) { return v != NA_INTEGER; });
      break;
    }
    case FCOL_INT64_ZC: {
      const int64_t na_i64 = static_cast<int64_t>(1ULL << 63);
      t.null_count = scan_zero_copy_col(
        static_cast<const int64_t*>(t.r_ptr), n, priv, [na_i64](int64_t v) { return v != na_i64; });
      break;
    }
    case FCOL_FLOAT64_ZC: {
      t.null_count = scan_zero_copy_col(
        static_cast<const double*>(t.r_ptr), n, priv,
        [](double v) { return !(std::isnan(v) && R_IsNA(v)); });
      break;
    }
    case FCOL_FLOAT64_FROM_INT: {
      t.null_count = convert_numeric_col<int, double>(
        static_cast<const int*>(t.r_ptr), n, priv,
        [](int v) { return v != NA_INTEGER; },
        [](int v) { return static_cast<double>(v); });
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
        if (val == NA_LOGICAL) nulls++;
        else if (val != 0) dbits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
      }
      t.null_count = nulls;
      populate_validity_bitmap(src, n, nulls, priv, [](int v) { return v != NA_LOGICAL; });
      priv->buffers[1] = dbits;
      break;
    }
    case FCOL_DATE_DBL: {
      t.null_count = convert_numeric_col<double, int32_t>(
        static_cast<const double*>(t.r_ptr), n, priv,
        [](double v) { return std::isfinite(v) && v >= -2147483648.0 && v <= 2147483647.0; },
        [](double v) { return static_cast<int32_t>(std::floor(v)); });
      break;
    }
    case FCOL_POSIXCT_TS:
    case FCOL_POSIXCT_DT: {
      t.null_count = convert_numeric_col<double, int64_t>(
        static_cast<const double*>(t.r_ptr), n, priv,
        [](double v) { return std::isfinite(v) && v >= -9.223372036854e12 && v <= 9.223372036854e12; },
        [](double v) { return static_cast<int64_t>(std::llround(v * 1000000.0)); });
      break;
    }
    default:
      break;
  }
}

template <typename OffsetT>
void fill_utf8_buffers(SEXP col,
                       int64_t n,
                       int64_t total_bytes,
                       bool any_needs_translate,
                       uint8_t* vbits,
                       OwnedArrayPriv* priv) {
  priv->buf1.resize(static_cast<size_t>(n + 1) * sizeof(OffsetT));
  priv->buf2.resize(static_cast<size_t>(total_bytes));
  OffsetT* offs = reinterpret_cast<OffsetT*>(priv->buf1.data());
  uint8_t* data_dst = priv->buf2.data();
  OffsetT pos = 0;
  offs[0] = 0;

  if (!any_needs_translate) {
    for (int64_t i = 0; i < n; i++) {
      SEXP elt = STRING_ELT(col, i);
      if (elt != NA_STRING) {
        if (vbits) vbits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        OffsetT len = static_cast<OffsetT>(XLENGTH(elt));
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
        OffsetT len = (u == CHAR(elt)) ? static_cast<OffsetT>(XLENGTH(elt))
                                       : static_cast<OffsetT>(std::strlen(u));
        if (len > 0) {
          std::memcpy(data_dst + pos, u, static_cast<size_t>(len));
          pos += len;
        }
        vmaxset(vmax);
      }
      offs[i + 1] = pos;
    }
  }
}

void build_utf8_col_main_thread(SEXP col,
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
  if (nulls > 0) priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
  uint8_t* vbits = nulls > 0 ? priv->validity.data() : nullptr;

  bool use_large = total_bytes > 2147483647LL;
  *fmt_out = use_large ? "U" : "u";
  if (use_large) {
    fill_utf8_buffers<int64_t>(col, n, total_bytes, any_needs_translate, vbits, priv);
  } else {
    fill_utf8_buffers<int32_t>(col, n, total_bytes, any_needs_translate, vbits, priv);
  }
  priv->buffers[0] = vbits;
  priv->buffers[1] = priv->buf1.data();
  priv->buffers[2] = priv->buf2.data();
}

bool build_factor_col_main_thread(SEXP col,
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
      lvl_strs[static_cast<size_t>(k)] = std::string(Rf_translateCharUTF8(elt));
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
  if (nulls > 0) priv->validity.assign(static_cast<size_t>((n + 7) >> 3), 0);
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
  std::memset(valid, 0, static_cast<size_t>(valid_bytes));
  R_xlen_t null_count = 0;

  if (Rf_inherits(x, "integer64") && TYPEOF(x) == REALSXP) {
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
      if (elt != NA_STRING && parse_decimal_string(CHAR(elt), scale, bitwidth, dst + i * byte_width)) {
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

// [[Rcpp::export(rng=false)]]
bool bqs_has_zstd() {
  return bqs_ipc::has_zstd_support();
}

// [[Rcpp::export(rng=false)]]
bool bqs_can_fast_encode(SEXP schema_xptr) {
  return bqs_ipc::can_fast_encode_schema(nanoarrow_schema_from_xptr(schema_xptr));
}

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
    return Rcpp::List::create(Rcpp::Named("arrays") = out_arrays, Rcpp::Named("schemas") = out_schemas);
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

    size_t ju = static_cast<size_t>(j);
    if (TYPEOF(col) == INTSXP && Rf_isFactor(col) && (type.empty() || type == "STRING")) {
      auto* priv = new OwnedArrayPriv();
      int64_t nc = 0;
      std::string fmt;
      if (build_factor_col_main_thread(col, n_rows, priv, &nc, &fmt)) {
        privs[ju] = priv;
        null_counts[ju] = nc;
        col_formats[ju] = std::move(fmt);
        n_bufs_vec[ju] = 3;
        continue;
      }
      delete priv;
    } else if (TYPEOF(col) == STRSXP &&
               (!Rf_isObject(col) || Rf_inherits(col, "wk_wkt")) &&
               (type.empty() || type == "STRING" || type == "GEOGRAPHY")) {
      auto* priv = new OwnedArrayPriv();
      int64_t nc = 0;
      std::string fmt;
      build_utf8_col_main_thread(col, n_rows, priv, &nc, &fmt);
      privs[ju] = priv;
      null_counts[ju] = nc;
      col_formats[ju] = std::move(fmt);
      n_bufs_vec[ju] = 3;
      continue;
    }

    FastColKind kind = FCOL_UNSUPPORTED;
    std::string fmt;
    bool zero_copy = false;

    if (TYPEOF(col) == INTSXP && Rf_inherits(col, "Date") && (type.empty() || type == "DATE")) {
      kind = FCOL_DATE_INT_ZC;
      fmt = "tdD";
      zero_copy = true;
    } else if (TYPEOF(col) == REALSXP && Rf_inherits(col, "Date") && (type.empty() || type == "DATE")) {
      kind = FCOL_DATE_DBL;
      fmt = "tdD";
    } else if (TYPEOF(col) == REALSXP && Rf_inherits(col, "POSIXct") &&
               (type.empty() || type == "TIMESTAMP" || type == "DATETIME")) {
      kind = (type == "DATETIME") ? FCOL_POSIXCT_DT : FCOL_POSIXCT_TS;
      fmt = (type == "DATETIME") ? "tsu:" : "tsu:UTC";
    } else if (TYPEOF(col) == REALSXP && Rf_inherits(col, "integer64") && (type.empty() || type == "INT64")) {
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
    } else if (TYPEOF(col) == REALSXP && !Rf_isObject(col) && (type.empty() || type == "FLOAT64")) {
      kind = FCOL_FLOAT64_ZC;
      fmt = "g";
      zero_copy = true;
    } else if (TYPEOF(col) == LGLSXP && !Rf_isObject(col) && (type.empty() || type == "BOOL")) {
      kind = FCOL_BOOL;
      fmt = "b";
    }

    if (kind == FCOL_UNSUPPORTED) {
      out_arrays[j] = R_NilValue;
      out_schemas[j] = R_NilValue;
      continue;
    }

    auto* priv = new OwnedArrayPriv();
    privs[ju] = priv;
    col_formats[ju] = std::move(fmt);
    if (zero_copy) zc_sexps[ju] = col;

    const void* r_ptr = nullptr;
    if (TYPEOF(col) == INTSXP) r_ptr = INTEGER(col);
    else if (TYPEOF(col) == REALSXP) r_ptr = REAL(col);
    else if (TYPEOF(col) == LGLSXP) r_ptr = LOGICAL(col);
    numeric_tasks.push_back({j, kind, r_ptr, n_rows, 0, priv});
  }

  if (!numeric_tasks.empty()) {
    size_t m = numeric_tasks.size();
    int resolved = bqs_resolve_threads(n_threads, 16u);
    int workers = (static_cast<int64_t>(n_rows) * static_cast<int64_t>(m) >= 65536) ? resolved : 1;
    bqs_parallel_for(m, workers, [&](int, size_t k) {
      execute_numeric_col_task(numeric_tasks[k]);
    });
    for (const auto& t : numeric_tasks) {
      null_counts[static_cast<size_t>(t.col_idx)] = t.null_count;
    }
  }

  for (int j = 0; j < p; j++) {
    size_t ju = static_cast<size_t>(j);
    OwnedArrayPriv* priv = privs[ju];
    if (!priv) continue;

    std::string nm = Rcpp::as<std::string>(col_names[j]);
    SEXP sch_xptr = PROTECT(make_leaf_schema_xptr(col_formats[ju].c_str(), nm.c_str()));
    SEXP arr_xptr = PROTECT(nanoarrow_array_owning_xptr());
    struct ArrowArray* arr = nanoarrow_output_array_from_xptr(arr_xptr);

    arr->length = n_rows;
    arr->null_count = null_counts[ju];
    arr->offset = 0;
    arr->n_buffers = n_bufs_vec[ju];
    arr->n_children = 0;
    arr->buffers = priv->buffers;
    arr->children = nullptr;
    arr->dictionary = nullptr;
    arr->private_data = priv;
    arr->release = release_owned_array;

    if (zc_sexps[ju] != R_NilValue) {
      R_SetExternalPtrProtected(arr_xptr, zc_sexps[ju]);
    }
    R_SetExternalPtrTag(arr_xptr, sch_xptr);

    out_arrays[j] = arr_xptr;
    out_schemas[j] = sch_xptr;
    UNPROTECT(2);
  }

  return Rcpp::List::create(Rcpp::Named("arrays") = out_arrays, Rcpp::Named("schemas") = out_schemas);
}

// [[Rcpp::export(rng=false)]]
Rcpp::RawVector bqs_encode_arrow_ipc_raw(SEXP schema_xptr,
                                         SEXP array_xptr,
                                         Rcpp::RawVector schema_ipc,
                                         int row_batch_size,
                                         bool use_zstd = false,
                                         int n_threads = 0) {
  struct ArrowSchema* schema = nanoarrow_schema_from_xptr(schema_xptr);
  struct ArrowArray* arr = nanoarrow_array_from_xptr(array_xptr);
  if (arr->length <= 0) return Rcpp::RawVector(0);

  std::vector<ArrowArray> src_arrays{*arr};
  int64_t total_uncompressed_bytes = bqs_ipc::estimate_batch_bytes(schema, arr);
  std::vector<bqs_ipc::BatchSliceSpec> specs = bqs_ipc::plan_batch_slices(
    schema, src_arrays, arr->length, total_uncompressed_bytes, row_batch_size, 1, false);

  std::vector<std::string> batches;
  std::string err;
  if (!bqs_ipc::encode_batches_parallel(schema, src_arrays, specs, use_zstd, n_threads, &batches, &err)) {
    Rcpp::stop(err.c_str());
  }

  size_t sch_len = bqs_ipc::strip_ipc_eos(RAW(schema_ipc), static_cast<size_t>(schema_ipc.size()));
  size_t total_size = sch_len + 8;
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
