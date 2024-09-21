library(testthat)
library(bigrquery)
library(bigrquerystorage)

if (nzchar(Sys.getenv("BIGQUERY_TEST_PROJECT")) &&
  nzchar(Sys.getenv("GCP_SERVICE_ACCOUNT"))) {
	options(nanoarrow.warn_unregistered_extension = FALSE)
  test_check("bigrquerystorage")
}
