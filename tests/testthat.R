library(testthat)
library(bigrquery)
library(bigrquerystorage)

if (nzchar(Sys.getenv("BIGQUERY_TEST_PROJECT")) &&
		nzchar(Sys.getenv("GCP_SERVICE_ACCOUNT"))) {

  test_check("bigrquerystorage")

}
