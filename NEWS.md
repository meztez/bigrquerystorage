# bigrquerystorage 1.1.0.9000

* Now uses nanoarrow instead of arrow. This lightens the dependency chain quite a bit (@hadleywickham).
* With `use_tibble = TRUE`
  * Convert BigQuery SQL types BYTES, GEOGRAPHY to blob, wk_wkt
  * Set timezone of BigQuery SQL type DATETIME to UTC
* Fix nested list parse post processing.
* Fix returning more rows than actual rows in source table when n_max > nrows.

# bigrquerystorage 1.1.0

* Fix n_max truncation and check for interrupt every 100 messages.

# bigrquerystorage 1.0.0

* Initial CRAN submission.
