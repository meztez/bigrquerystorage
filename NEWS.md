# bigrquerystorage (development version)

* REPEATED INT64 columns are now also read directly as `bit64::integer64`
  (instead of going through R's double, which could lose precision for
  values above 2^53). This relies on a nanoarrow fix for
  apache/arrow-nanoarrow#932 (list<int64> conversion corrupted values);
  until that fix is merged and released, this package temporarily depends
  on a patched nanoarrow fork/branch (see `DESCRIPTION`'s `Remotes` field).
  This dev version will not be submitted to CRAN until nanoarrow ships a
  release with the fix (#86).

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
