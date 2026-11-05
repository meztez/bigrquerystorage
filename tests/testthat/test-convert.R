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
