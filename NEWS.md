# bigrquerystorage (development version)

## New features

* Add `bqs_table_upload()` implementing the `BigQueryWrite` gRPC interface using
  Arrow `RecordBatch` serialization (`AppendRows` with `ArrowData`):
  * Supports `PENDING` streams (atomic multi-batch commit via
    `CreateWriteStream`, `FinalizeWriteStream`, and `BatchCommitWriteStreams`)
    and `DEFAULT` streams (`_default`).
  * Supports `create_disposition = c("CREATE_IF_NEEDED", "CREATE_NEVER")` and
    `write_disposition = c("WRITE_APPEND", "WRITE_EMPTY", "WRITE_TRUNCATE")` so
    upstream `bigrquery::bq_table_upload()` can delegate directly to
    `bqs_table_upload()`.
  * Accepts `data.frame`, `tibble`, `data.table`, `nanoarrow_array`, and
    `nanoarrow_array_stream` inputs, with full support for `INT64`
    (`bit64::integer64`), `NUMERIC` (`Decimal128`), `BIGNUMERIC` (`Decimal256`),
    `DATE`, `TIME` (`hms`), `DATETIME`, `TIMESTAMP`, `BYTES` (`blob`),
    `GEOGRAPHY` (`wk::wkt`), nested `STRUCT` / `RECORD`, and `REPEATED` columns.
* `bqs_table_download()` now accepts a `bigrquery::bq_table` object directly in
  `x` in addition to character table references.

# bigrquerystorage 1.3.0

## Performance

* `bqs_table_download()` is much faster than in 1.2.3 (median of two runs on a
  4-core Linux server):
  * Nested table `google_analytics_sample.ga_sessions_20170801` (2,556 rows):
    120 s to 2.3 s.
  * `samples.gsod`, 18 columns, `year >= 2007` (11.3M rows): 55 s to 5.5 s.
  * `usa_names.usa_1910_current` (6.3M rows): 3.7 s to 1.9 s.
* Download:
  * Requests `ZSTD` Arrow buffer compression by default when supported by
    `nanoarrow` (`compression = "auto"`), cutting the transfer size of
    `usa_names` by 3.6x.
  * Reads the streams of a `ReadSession` in parallel, each worker on its own
    gRPC channel (HTTP/2 connection). Up to 16 streams are read concurrently
    by default; set `options(bigrquerystorage.threads = n)` to change it.
  * `CreateReadSession` requests `preferred_min_stream_count` so the server
    returns enough streams to read in parallel.
  * Reads are also parallel when `n_max` is set, and stop as soon as enough
    rows have been received.
  * Parallel downloads can be interrupted and show a progress bar.
  * Moves the serialized Arrow record batches out of the Protobuf responses
    instead of copying them.
  * Reuses the gRPC client for 30 minutes instead of 30 seconds, avoiding new
    TLS/HTTP2 handshakes.
* Conversion to R:
  * Decodes and decompresses the Arrow record batches of each stream in
    parallel instead of on the R thread.
  * Converts `NUMERIC` (`Decimal128`) columns, and `INT64` columns when
    `bigint = "integer"`, directly in C++.
  * Post-processes `STRUCT` / `RECORD` and `REPEATED` columns in C++ instead of
    evaluating R code for every row, and converts all nested `INT64` values
    with a single `bigint` conversion (an overflow warning is reported once,
    not once per row).
  * Derives the post-processing metadata (`DATETIME`, `GEOGRAPHY`, `BYTES`,
    `INT64`, `STRUCT`, `ARRAY`) from the Arrow schema of the `ReadSession`
    instead of calling `bigrquery::bq_table_fields()` on every download.

## New features

* `bqs_table_download()` gains `type = c("tibble", "data.frame", "data.table",
  "arrow")`, `max_stream_count` and `compression = c("auto", "zstd", "lz4",
  "none")` arguments.
* Update the bundled Google BigQuery Storage v1 and Google API `.proto`
  definitions (raises the maximum `ReadRows` response size to 128 MB).

## Bug fixes

* `REPEATED INT64` columns are read directly as `bit64::integer64` instead of
  going through R's double, which could lose precision above 2^53 (#86).
* Fix wrong values for `INT64` fields of `REPEATED RECORD` columns, and any
  `INT64` nested under a `REPEATED` field: `nanoarrow` <= 0.9.0 ignores list
  offsets when converting them to `bit64::integer64`, which returned shifted
  or garbage values. They are now rebuilt losslessly from the Arrow buffers.

# bigrquerystorage 1.2.3

* Fix RANGE conversion.
* Replace `Rf_error` with `Rcpp::stop` in `RProgress.h` (#84).
* `bqs_table_download()` no longer converts INT64 columns through R's double,
  which silently lost precision for values above 2^53 before the `bigint`
  argument was applied. INT64 columns (including fields nested in RECORD
  columns) are now read directly as `bit64::integer64`, so
  `bigint = "integer64"` and `bigint = "character"` are lossless over the full
  64-bit range (r-dbi/bigrquery#689). REPEATED INT64 columns still go through
  double due to a conversion bug in nanoarrow, but now honor `bigint` (#86, #87).

# bigrquerystorage 1.2.2

* Fix logging for new version of gRPC.
* Fix service authentication when using `gcloud auth application-default` database.
* Fix FLOAT conversion.

# bigrquerystorage 1.2.1

* Increment `nanoarrow` version requirement.

# bigrquerystorage 1.2.0

* Now uses nanoarrow instead of arrow. This lightens the dependency chain quite a bit (@hadleywickham).
* Deprecated `use_tibble` in `bqs_table_download`, always convert to tibble from the new nanoarrow stream reader.
* Convert BigQuery SQL types BYTES, GEOGRAPHY to blob, wk_wkt
* Set timezone of BigQuery SQL type DATETIME to UTC
* Fix nested list parse post processing.
* Fix returning more rows than actual rows in source table when n_max > nrows.
* Drop DBI methods.

# bigrquerystorage 1.1.0

* Fix n_max truncation and check for interrupt every 100 messages.

# bigrquerystorage 1.0.0

* Initial CRAN submission.
