# int64 handling ---------------------------------------------------------

test_that("int64_ptype maps int64 to integer64, including struct fields", {
  schema <- na_struct(list(
    a = na_int64(),
    b = na_string(),
    c = na_struct(list(d = na_int64()))
  ))
  ptype <- int64_ptype(schema)
  expect_s3_class(ptype$a, "integer64")
  expect_type(ptype$b, "character")
  expect_s3_class(ptype$c$d, "integer64")
})

test_that("int64_ptype maps int64 to integer64 in list fields (REPEATED columns)", {
  schema <- na_struct(list(a = na_list(na_int64())))
  ptype <- int64_ptype(schema)
  expect_s3_class(attr(ptype$a, "ptype"), "integer64")
})

test_that("int64 conversion is lossless (r-dbi/bigrquery#689)", {
  big <- bit64::as.integer64("9223372036854775295")
  arr <- as_nanoarrow_array(
    data.frame(x = big),
    schema = na_struct(list(x = na_int64()))
  )
  fields <- list(list(name = "x", type = "INT64", mode = "NULLABLE"))

  convert <- function(bigint) {
    stream <- basic_array_stream(list(arr))
    df <- convert_array_stream(stream, to = int64_ptype(stream$get_schema()))
    parse_postprocess(tibble::tibble(df), bigint, fields)
  }

  expect_identical(convert("integer64")$x, big)
  expect_identical(convert("character")$x, "9223372036854775295")
  expect_identical(suppressWarnings(convert("integer")$x), NA_integer_)
  expect_identical(
    suppressWarnings(convert("numeric")$x),
    suppressWarnings(as.numeric(big))
  )
})

test_that("int64_ptype leaves non-nested vctrs_list_of ptypes untouched (e.g. blob/BYTES)", {
  schema <- na_binary()
  ptype <- int64_ptype(schema)
  expect_s3_class(ptype, "blob")
})

test_that("repeated int64 columns honor bigint and are lossless", {
  big <- bit64::as.integer64("9223372036854775295")
  # Construct a struct(x = list<int64>) array without relying on
  # as_nanoarrow_array.list() on integer64 (which in nanoarrow <= 0.9.0 used
  # unlist() and stripped the integer64 class).
  item_arr <- as_nanoarrow_array(
    c(big, bit64::as.integer64("2"), bit64::as.integer64("3")),
    schema = na_int64()
  )
  list_arr <- nanoarrow_array_init(na_list(na_int64()))
  list_arr <- nanoarrow_array_modify(
    list_arr,
    list(
      length = 2L,
      null_count = 0L,
      offset = 0L,
      buffers = list(NULL, as_nanoarrow_buffer(c(0L, 2L, 3L))),
      children = list(item_arr)
    )
  )
  struct_schema <- na_struct(list(x = na_list(na_int64())))
  struct_arr <- nanoarrow_array_init(struct_schema)
  struct_arr <- nanoarrow_array_modify(
    struct_arr,
    list(
      length = 2L,
      null_count = 0L,
      offset = 0L,
      buffers = list(NULL),
      children = list(list_arr)
    )
  )
  nanoarrow_array_set_schema(struct_arr, struct_schema)

  convert_repeated <- function(mode) {
    stream <- basic_array_stream(list(struct_arr))
    schema <- stream$get_schema()
    to <- int64_ptype(schema)
    df <- bqs_convert_stream(stream, to, mode, nanoarrow::convert_array_stream, convert_stream_no_warnings)
    parse_postprocess_schema(df, schema, mode)
  }

  df64 <- convert_repeated("integer64")
  expect_s3_class(df64$x[[1]], "integer64")
  expect_identical(df64$x[[1]], c(big, bit64::as.integer64("2")))
  expect_identical(df64$x[[2]], bit64::as.integer64("3"))

  dfint <- suppressWarnings(convert_repeated("integer"))
  expect_identical(dfint$x[[1]], c(NA_integer_, 2L))
  expect_identical(dfint$x[[2]], 3L)

  dfchr <- convert_repeated("character")
  expect_identical(dfchr$x[[1]], c("9223372036854775295", "2"))
  expect_identical(dfchr$x[[2]], "3")
})

test_that("bqs_convert_stream and parse_postprocess_schema handle Decimal128 and bigint modes", {
  big <- bit64::as.integer64("9223372036854775295")
  arr <- as_nanoarrow_array(
    data.frame(x = c(big, bit64::as.integer64("42"))),
    schema = na_struct(list(x = na_int64()))
  )

  for (mode in c("integer", "integer64", "numeric", "character")) {
    stream <- basic_array_stream(list(arr))
    schema <- stream$get_schema()
    to <- int64_ptype(schema)
    df <- suppressWarnings(bqs_convert_stream(stream, to, mode, nanoarrow::convert_array_stream, convert_stream_no_warnings))
    tb <- suppressWarnings(parse_postprocess_schema(df, schema, mode))
    expect_s3_class(tb, "tbl_df")
    if (mode == "integer") {
      expect_identical(tb$x, c(NA_integer_, 42L))
    } else if (mode == "integer64") {
      expect_identical(tb$x, c(big, bit64::as.integer64("42")))
    } else if (mode == "character") {
      expect_identical(tb$x, c("9223372036854775295", "42"))
    }
  }
})

