library(testthat)
library(bigrquery)
library(bigrquerystorage)

if (bigrquery::bq_authable()) {
  bq_auth()
  test_check("bigrquerystorage")
}
