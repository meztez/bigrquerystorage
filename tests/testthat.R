library(testthat)
library(bigrquery)
library(bigrquerystorage)
bq_auth()

test_check("bigrquerystorage")
