library(testthat)
library(bigrquery)
library(bigrquerystorage)

if (bigrquery::bq_authable() &&
		inherits(try(bigrquery::bq_test_project()), "character") &&
		nzchar(Sys.getenv("GCP_SERVICE_ACCOUNT"))) {
	tmp <- tempfile(pattern = ".json")
	writeLines(base64enc::base64decode(Sys.getenv("GCP_SERVICE_ACCOUNT")), tmp)
  bq_auth(path = tmp)
  test_check("bigrquerystorage")
}
