# Arrow -> R conversion (offline, no BigQuery access needed) ---------------

repeated_record_array <- function() {
  big <- bit64::as.integer64(c("9223372036854775295", "2", "3", "-9007199254740993", "5", "6"))
  item_schema <- na_struct(list(a = na_int64(), b = na_string()))
  item_arr <- as_nanoarrow_array(
    data.frame(a = big, b = c("x", "y", "z", "p", "q", "r")),
    schema = item_schema
  )
  list_arr <- nanoarrow_array_modify(
    nanoarrow_array_init(na_list(item_schema)),
    list(
      length = 3L,
      null_count = 0L,
      offset = 0L,
      buffers = list(NULL, as_nanoarrow_buffer(c(0L, 2L, 3L, 6L))),
      children = list(item_arr)
    )
  )
  schema <- na_struct(list(id = na_int64(), rr = na_list(item_schema)))
  arr <- nanoarrow_array_modify(
    nanoarrow_array_init(schema),
    list(
      length = 3L,
      null_count = 0L,
      offset = 0L,
      buffers = list(NULL),
      children = list(as_nanoarrow_array(bit64::as.integer64(1:3), schema = na_int64()), list_arr)
    )
  )
  nanoarrow_array_set_schema(arr, schema)
  list(array = arr, values = big)
}

convert_download <- function(arr, bigint) {
  stream <- basic_array_stream(list(arr))
  schema <- stream$get_schema()
  df <- bqs_convert_stream(stream, int64_ptype(schema), bigint, nanoarrow::convert_array_stream, convert_stream_no_warnings)
  parse_postprocess_schema(df, schema, bigint)
}

test_that("INT64 fields of REPEATED RECORD columns are lossless and honor bigint", {
  rr <- repeated_record_array()
  big <- rr$values

  df64 <- convert_download(rr$array, "integer64")
  expect_null(attr(df64$rr, "class"))
  expect_s3_class(df64$rr[[1]], "tbl_df")
  expect_identical(df64$rr[[1]]$a, big[1:2])
  expect_identical(df64$rr[[2]]$a, big[3])
  expect_identical(df64$rr[[3]]$a, big[4:6])
  expect_identical(df64$rr[[3]]$b, c("p", "q", "r"))

  dfchr <- convert_download(rr$array, "character")
  expect_identical(dfchr$rr[[1]]$a, c("9223372036854775295", "2"))
  expect_identical(dfchr$rr[[3]]$a, c("-9007199254740993", "5", "6"))

  expect_warning(
    dfint <- convert_download(rr$array, "integer"),
    "NAs produced by integer overflow"
  )
  expect_identical(dfint$id, 1:3)
  expect_identical(dfint$rr[[1]]$a, c(NA_integer_, 2L))
  expect_identical(dfint$rr[[2]]$a, 3L)
  expect_identical(dfint$rr[[3]]$a, c(NA_integer_, 5L, 6L))
})

test_that("parse_postprocess_schema does not modify a shared input", {
  rr <- repeated_record_array()
  stream <- basic_array_stream(list(rr$array))
  schema <- stream$get_schema()
  df <- bqs_convert_stream(stream, int64_ptype(schema), "integer64", nanoarrow::convert_array_stream, convert_stream_no_warnings)
  before <- unserialize(serialize(df, NULL))
  tb <- parse_postprocess_schema(df, schema, "integer64")
  expect_s3_class(tb, "tbl_df")
  expect_s3_class(tb$rr[[1]], "tbl_df")
  expect_identical(df, before)
  expect_false(inherits(df, "tbl_df"))
  expect_false(inherits(df$rr[[1]], "tbl_df"))
})

test_that("bqs_collect_streams decodes several IPC buffers in reader order", {
  skip_if_not(exists("write_nanoarrow", asNamespace("nanoarrow")))
  to_ipc <- function(df) {
    con <- rawConnection(raw(0), "wb")
    on.exit(close(con))
    nanoarrow::write_nanoarrow(df, con)
    rawConnectionValue(con)
  }
  parts <- list(
    data.frame(x = 1:3, y = c("a", "b", "c")),
    data.frame(x = integer(), y = character()),
    data.frame(x = 4:5, y = c("d", "e")),
    data.frame(x = 6L, y = "f")
  )
  buffers <- lapply(parts, to_ipc)
  readers <- lapply(buffers, read_nanoarrow)
  stream <- bqs_collect_streams(readers, read_nanoarrow(buffers[[1]]), 3L)
  expect_s3_class(stream, "nanoarrow_array_stream")
  expect_identical(
    convert_array_stream(stream),
    data.frame(x = 1:6, y = c("a", "b", "c", "d", "e", "f"))
  )
})

test_that("bqs_infer_bq_fields and bqs_df_to_arrow_array handle nested and special types", {
  df <- tibble::tibble(
    int_col = c(1L, NA_integer_),
    int64_col = bit64::as.integer64(c("9223372036854775295", NA)),
    dbl_col = c(1.5, NA_real_),
    lgl_col = c(TRUE, NA),
    chr_col = c("abc", NA_character_),
    fct_col = factor(c("x", NA)),
    date_col = as.Date(c("2024-01-01", NA)),
    time_col = hms::hms(hours = c(1, NA)),
    ts_col = as.POSIXct(c("2024-01-01 01:02:03", NA), tz = "UTC"),
    struct_col = tibble::tibble(a = 1:2, b = c("u", "v")),
    rep_int_col = list(1:3, integer(0)),
    rep_struct_col = list(
      tibble::tibble(x = 1:2, y = c("p", "q")),
      tibble::tibble(x = integer(0), y = character(0))
    )
  )

  fields <- bqs_infer_bq_fields(df)
  types <- stats::setNames(
    vapply(fields, `[[`, character(1), "type"),
    vapply(fields, `[[`, character(1), "name")
  )
  modes <- stats::setNames(
    vapply(fields, `[[`, character(1), "mode"),
    vapply(fields, `[[`, character(1), "name")
  )

  expect_identical(types[["int_col"]], "INT64")
  expect_identical(types[["int64_col"]], "INT64")
  expect_identical(types[["dbl_col"]], "FLOAT64")
  expect_identical(types[["lgl_col"]], "BOOL")
  expect_identical(types[["chr_col"]], "STRING")
  expect_identical(types[["fct_col"]], "STRING")
  expect_identical(types[["date_col"]], "DATE")
  expect_identical(types[["time_col"]], "TIME")
  expect_identical(types[["ts_col"]], "TIMESTAMP")
  expect_identical(types[["struct_col"]], "RECORD")
  expect_identical(types[["rep_int_col"]], "INT64")
  expect_identical(modes[["rep_int_col"]], "REPEATED")
  expect_identical(types[["rep_struct_col"]], "RECORD")
  expect_identical(modes[["rep_struct_col"]], "REPEATED")

  arr <- bqs_df_to_arrow_array(df, fields)
  sch <- infer_nanoarrow_schema(arr)
  expect_identical(sch$children$fct_col$format, "u")
  expect_identical(sch$children$time_col$format, "ttu")
  expect_identical(sch$children$ts_col$format, "tsu:UTC")
  expect_identical(sch$children$rep_int_col$format, "+l")
  expect_identical(sch$children$rep_struct_col$format, "+l")
  expect_identical(sch$children$rep_struct_col$children$item$format, "+s")

  ipc <- bqs_serialize_to_ipc(df, fields, row_batch_size = 1L)
  expect_true(length(ipc) > 0)
  reader <- read_nanoarrow(ipc)
  b1 <- reader$get_next()
  b2 <- reader$get_next()
  expect_equal(b1$length, 1L)
  expect_equal(b2$length, 1L)
  expect_null(reader$get_next())
})

test_that("bqs_encode_decimal encodes Decimal128 and Decimal256 with NAs", {
  num_fields <- list(
    list(name = "num", type = "NUMERIC", mode = "NULLABLE"),
    list(name = "bnum", type = "BIGNUMERIC", mode = "NULLABLE"),
    list(name = "dt", type = "DATETIME", mode = "NULLABLE")
  )
  df <- tibble::tibble(
    num = c(12345.6789, -67.89, NA_real_),
    bnum = c("12345678901234567890.123456789", "-0.5", NA_character_),
    dt = as.POSIXct(c("2024-01-01 01:02:03", "2024-01-02 04:05:06", NA), tz = "UTC")
  )
  arr <- bqs_df_to_arrow_array(df, num_fields)
  sch <- infer_nanoarrow_schema(arr)
  expect_identical(sch$children$num$format, "d:38,9")
  expect_identical(sch$children$bnum$format, "d:76,38,256")
  expect_identical(sch$children$dt$format, "tsu:")

  res <- convert_download(arr, "integer")
  expect_equal(res$num, c(12345.6789, -67.89, NA_real_))
})

test_that("nested INT64 in STRUCT, REPEATED RECORD, and plain data.frames serialize losslessly", {
  big_vals <- bit64::as.integer64(c("9223372036854775295", "-9007199254740993", NA))

  inner_df <- data.frame(big = big_vals, txt = c("a", "b", "c"))
  outer_df <- data.frame(id = 1:3)
  outer_df$s <- inner_df
  outer_df$rep_s <- list(
    data.frame(
      a = bit64::as.integer64(c("9223372036854775295", "2")),
      b = c("x", "y")
    ),
    data.frame(a = bit64::as.integer64(character(0)), b = character(0)),
    data.frame(
      a = bit64::as.integer64(c("-9007199254740993", NA)),
      b = c("z", "w")
    )
  )

  fields <- bqs_infer_bq_fields(outer_df)
  ipc <- bqs_serialize_to_ipc(outer_df, fields, row_batch_size = 2L)
  stream <- read_nanoarrow(ipc)
  sch <- stream$get_schema()
  out <- parse_postprocess_schema(
    bqs_convert_stream(stream, int64_ptype(sch), "integer64", nanoarrow::convert_array_stream, convert_stream_no_warnings),
    sch,
    "integer64"
  )
  expect_identical(out$s$big, big_vals)
  expect_identical(out$rep_s[[1]]$a, bit64::as.integer64(c("9223372036854775295", "2")))
  expect_identical(out$rep_s[[2]]$a, bit64::as.integer64(character(0)))
  expect_identical(out$rep_s[[3]]$a, bit64::as.integer64(c("-9007199254740993", NA)))

  # Deeply nested REPEATED RECORD with nested STRUCT and nested REPEATED INT64
  df_deep <- tibble::tibble(
    rep_s = list(
      tibble::tibble(
        a = bit64::as.integer64(character(0)),
        sub = tibble::tibble(k = bit64::as.integer64(character(0))),
        rep_b = list()
      ),
      tibble::tibble(
        a = bit64::as.integer64(c("9223372036854775295", "-9007199254740993")),
        sub = tibble::tibble(k = bit64::as.integer64(c("9223372036854775295", "10"))),
        rep_b = list(bit64::as.integer64(c("9223372036854775295", "11")), bit64::as.integer64("-9007199254740993"))
      )
    )
  )
  fields_deep <- bqs_infer_bq_fields(df_deep)
  arr_deep <- bqs_df_to_arrow_array(df_deep, fields_deep)
  out_deep <- convert_download(arr_deep, "integer64")
  expect_identical(out_deep$rep_s[[2]]$a, df_deep$rep_s[[2]]$a)
  expect_identical(out_deep$rep_s[[2]]$sub$k, df_deep$rep_s[[2]]$sub$k)
  expect_identical(out_deep$rep_s[[2]]$rep_b, df_deep$rep_s[[2]]$rep_b)
})

test_that("fast C++ column builder and ZSTD Arrow IPC encoder preserve values across non-aligned batch slices", {
  n <- 205L
  df <- tibble::tibble(
    i = ifelse(seq_len(n) %% 7L == 0L, NA_integer_, seq_len(n)),
    i64 = bit64::as.integer64(ifelse(seq_len(n) %% 11L == 0L, NA_character_, as.character(9007199254740990 + seq_len(n)))),
    d = ifelse(seq_len(n) %% 5L == 0L, NA_real_, seq_len(n) * 0.25),
    b = ifelse(seq_len(n) %% 9L == 0L, NA, seq_len(n) %% 2L == 0L),
    s = ifelse(seq_len(n) %% 6L == 0L, NA_character_, paste0("val_", seq_len(n))),
    f = factor(ifelse(seq_len(n) %% 8L == 0L, NA_character_, paste0("lvl_", seq_len(n) %% 4L))),
    dt = as.Date("2024-01-01") + ifelse(seq_len(n) %% 10L == 0L, NA_integer_, seq_len(n)),
    ts = as.POSIXct("2024-01-01 00:00:00", tz = "UTC") + ifelse(seq_len(n) %% 13L == 0L, NA_real_, seq_len(n))
  )
  fields <- bqs_infer_bq_fields(df)

  # Non-byte-aligned row_batch_size (37 rows) exercises bit-shifted validity & bool slices
  ipc_raw <- bqs_serialize_to_ipc(df, fields, row_batch_size = 37L, use_zstd = FALSE)
  stream_raw <- read_nanoarrow(ipc_raw)
  sch_raw <- stream_raw$get_schema()
  out_raw <- parse_postprocess_schema(
    bqs_convert_stream(stream_raw, int64_ptype(sch_raw), "integer64", nanoarrow::convert_array_stream, convert_stream_no_warnings),
    sch_raw,
    "integer64"
  )
  expect_identical(out_raw$i64, df$i64)
  expect_equal(out_raw$d, df$d)
  expect_identical(out_raw$b, df$b)
  expect_identical(out_raw$s, df$s)
  expect_identical(out_raw$f, as.character(df$f))
  expect_identical(out_raw$dt, df$dt)
  expect_equal(out_raw$ts, df$ts)

  if (bqs_has_zstd() && isTRUE(nanoarrow::nanoarrow_with_zstd())) {
    ipc_zstd <- bqs_serialize_to_ipc(df, fields, row_batch_size = 37L, use_zstd = TRUE)
    expect_lt(length(ipc_zstd), length(ipc_raw))
    stream_z <- read_nanoarrow(ipc_zstd)
    sch_z <- stream_z$get_schema()
    out_z <- parse_postprocess_schema(
      bqs_convert_stream(stream_z, int64_ptype(sch_z), "integer64", nanoarrow::convert_array_stream, convert_stream_no_warnings),
      sch_z,
      "integer64"
    )
    expect_identical(out_z, out_raw)
  }
})

