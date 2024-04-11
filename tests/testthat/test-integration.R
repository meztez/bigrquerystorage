# BigQueryStorage -------------------------------------------------------
auth_fn <- function() {
  tmp <- tempfile(pattern = ".json")
  on.exit(unlink(tmp))
  writeBin(base64enc::base64decode(Sys.getenv("GCP_SERVICE_ACCOUNT")), tmp)
  bigrquery::bq_auth(path = tmp)
}


test_that("BigQuery json and BigQuery return the same results", {
  auth_fn()

  # Compare with bigrquery method
  dt <- bqs_table_download("bigquery-public-data.usa_names.usa_1910_current", bigrquery::bq_test_project(), n_max = 50000, as_tibble = TRUE, quiet = TRUE)
  dt2 <- bq_table_download("bigquery-public-data.usa_names.usa_1910_current", n_max = 50000, quiet = TRUE)
  expect_equal(dt, dt2)
})

test_that("Method dispatch to BigQuery Storage", {
  auth_fn()

  con <- bigrquery::dbConnect(
    bigrquery::bigquery(),
    project = "bigquery-public-data",
    dataset = "usa_names",
    billing = bigrquery::bq_test_project(),
    quiet = TRUE
  )

  # Basic reading table as data.frame
  dt <- DBI::dbGetQuery(con, "SELECT * FROM `bigquery-public-data.usa_names.usa_1910_current` LIMIT 50000")
  expect_true(inherits(dt, "data.frame"))
  expect_true(nrow(dt) == 50000)

  # Full table fetch
  dt <- DBI::dbReadTable(con, "bigquery-public-data.usa_names.usa_1910_current")
  expect_true(inherits(dt, "data.frame"))
})

test_that("0 rows table can be returned", {
  auth_fn()

  con <- bigrquery::dbConnect(
    bigrquery::bigquery(),
    project = "bigquery-public-data",
    dataset = "usa_names",
    billing = bigrquery::bq_test_project(),
    quiet = TRUE
  )

  # Check if a 0 rows table can be returned
  dt <- DBI::dbGetQuery(con, "SELECT * FROM `bigquery-public-data.usa_names.usa_1910_current` LIMIT 0")
  expect_true(inherits(dt, "data.frame"))
  expect_true(nrow(dt) == 0)
})

test_that("Optional BigQuery Storage API parameters work", {
  auth_fn()

  # Check other features
  dt <- bqs_table_download("bigquery-public-data:usa_names.usa_1910_current",
    bigrquery::bq_test_project(),
    selected_fields = c("name", "number", "state"),
    row_restriction = 'state = "WA"',
    quiet = TRUE
  )
  expect_length(names(dt), 3)
  expect_identical(as.character(unique(dt$state)), "WA")

  # BigQuery tables are organized into data blocks. The TABLESAMPLE clause works by randomly
  # selecting a percentage of data blocks from the table and reading all of the rows in the
  # selected blocks. The sampling granularity is limited by the number of data blocks.
  #
  # Typically, BigQuery splits tables or table partitions into blocks if they are larger than
  # about 1 GB. Smaller tables might consist of a single data block. In that case, the TABLESAMPLE
  # clause reads the entire table. If the sampling percentage is greater than zero and the table
  # is not empty, then table sampling always returns some results.
  dt <- bqs_table_download(
    "bigquery-public-data:usa_names.usa_1910_current",
    bigrquery::bq_test_project(),
    selected_fields = "number",
    sample_percentage = 0,
    quiet = TRUE
  )
  expect_true(nrow(dt) == 0)
})

# types -------------------------------------------------------------------

test_that("can read utf-8 strings", {
  auth_fn()

  sql <- "SELECT '\U0001f603' as x"
  tb <- bq_project_query(bigrquery::bq_test_project(), sql, quiet = TRUE)
  df <- bqs_table_download(tb, bigrquery::bq_test_project(), as_tibble = TRUE, quiet = TRUE)
  x <- df$x[[1]]

  expect_equal(Encoding(x), "UTF-8")
  expect_equal(x, "\U0001f603")
})

# https://cloud.google.com/bigquery/docs/reference/storage/#arrow_schema_details
# DATETIME does not a have a timezone
test_that("can convert date time types", {
  auth_fn()

  sql <- "SELECT
    datetime,
    CAST (datetime as DATE) as date,
    CAST (datetime as TIME) as time,
    CAST (datetime as TIMESTAMP) as timestamp
    FROM (SELECT DATETIME '2000-01-02 03:04:05.67' as datetime)
  "

  tb <- bq_project_query(bigrquery::bq_test_project(), sql, quiet = TRUE)
  df <- bqs_table_download(tb, bigrquery::bq_test_project(), as_tibble = TRUE, quiet = TRUE)
  df2 <- bq_table_download(tb, quiet = TRUE)

  base <- ISOdatetime(2000, 1, 2, 3, 4, 5.67, tz = "UTC")
  attr(df$datetime, "tzone") <- attr(base, "tzone")

  expect_equal(df, df2, tolerance = 0.67)
  expect_equal(df$datetime, base)
  expect_equal(df$timestamp, base)
  expect_equal(df$date, as.Date(base))
  expect_equal(df$time, hms::hms(hours = 3, minutes = 4, seconds = 5.67))
})

test_that("correctly parse logical values", {
  auth_fn()

  query <- "SELECT TRUE as x"
  tb <- bq_project_query(bigrquery::bq_test_project(), query)
  df <- bqs_table_download(tb, bigrquery::bq_test_project(), as_tibble = TRUE, quiet = TRUE)

  expect_true(df$x)
})

test_that("the return type of integer columns is set by the bigint argument", {
  auth_fn()

  x <- c("-2147483648", "-2147483647", "-1", "0", "1", "2147483647", "2147483648")
  sql <- paste0("SELECT * FROM UNNEST ([", paste0(x, collapse = ","), "]) AS x")
  qry <- bq_project_query(bigrquery::bq_test_project(), sql)

  expect_warning(
    out_int <- bqs_table_download(qry, bigrquery::bq_test_project(), as_tibble = TRUE, bigint = "integer", quiet = TRUE)$x,
    "integer overflow"
  )
  expect_identical(out_int, suppressWarnings(as.integer(x)))

  out_int64 <- bqs_table_download(qry, bigrquery::bq_test_project(), as_tibble = TRUE, bigint = "integer64", quiet = TRUE)$x
  expect_identical(out_int64, bit64::as.integer64(x))

  out_dbl <- bqs_table_download(qry, bigrquery::bq_test_project(), as_tibble = TRUE, bigint = "numeric", quiet = TRUE)$x
  expect_identical(out_dbl, as.double(x))

  out_chr <- bqs_table_download(qry, bigrquery::bq_test_project(), as_tibble = TRUE, bigint = "character", quiet = TRUE)$x
  expect_identical(out_chr, x)
})

test_that("n_max returns no more rows than actual originaly in table", {
  auth_fn()
  query <- "SELECT TRUE as x"
  tb <- bq_project_query(bigrquery::bq_test_project(), query)
  df <- bqs_table_download(tb, bigrquery::bq_test_project(), as_tibble = TRUE, quiet = TRUE, n_max = 50)
  expect_equal(nrows(df), 1)
})

# Geography is mapped to an utf8 string in input,
# it would have to be converted to a geography by the user
test_that("can convert geography type", {
  auth_fn()

  skip_if_not_installed("wk")
  sql <- "SELECT ST_GEOGFROMTEXT('POINT (30 10)') as geography"
  tb <- bq_project_query(bigrquery::bq_test_project(), sql, quiet = TRUE)
  df <- bqs_table_download(tb, bigrquery::bq_test_project(), as_tibble = TRUE, quiet = TRUE)

  expect_identical(wk::wkt(df$geography), wk::wkt("POINT(30 10)"))
})

test_that("can convert bytes type", {
  auth_fn()

  sql <- "SELECT ST_ASBINARY(ST_GEOGFROMTEXT('POINT (30 10)')) as bytes"
  tb <- bq_project_query(bq_test_project(), sql, quiet = TRUE)
  df <- bqs_table_download(tb, bigrquery::bq_test_project(), as_tibble = TRUE, quiet = TRUE)

  expect_identical(
    unlist(df$bytes),
    as.raw(c(
      0x01, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0x3d, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24,
      0x40
    ))
  )
})

