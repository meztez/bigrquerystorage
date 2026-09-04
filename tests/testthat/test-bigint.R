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
  arr <- suppressWarnings(as_nanoarrow_array(
    list(c(big, bit64::as.integer64("2")), bit64::as.integer64("3")),
    schema = na_list(na_int64())
  ))
  stream <- basic_array_stream(list(arr))
  x <- convert_array_stream(stream, to = int64_ptype(stream$get_schema()))
  fields <- list(list(name = "x", type = "INT64", mode = "REPEATED"))

  df64 <- parse_postprocess(tibble::tibble(x = x), "integer64", fields)
  expect_s3_class(df64$x[[1]], "integer64")
  expect_identical(df64$x[[1]], c(big, bit64::as.integer64("2")))
  expect_identical(df64$x[[2]], bit64::as.integer64("3"))

  dfint <- suppressWarnings(parse_postprocess(tibble::tibble(x = x), "integer", fields))
  expect_identical(dfint$x[[1]], c(NA_integer_, 2L))
  expect_identical(dfint$x[[2]], 3L)
})
