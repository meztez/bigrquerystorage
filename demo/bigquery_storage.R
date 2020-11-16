library(bigrquery)
library(bigrquerystorage)

#Test bigrquery speed
system.time({
  con <- bigrquery::dbConnect(
    bigrquery::bigquery(),
    project = "bigquery-public-data",
    dataset = "usa_names",
    billing = "labo-brunotremblay-253317",
    bigint = "numeric",
    quiet = FALSE)
  dta <- bigrquery::bq_table_download(bigrquery::as_bq_table("bigquery-public-data.usa_names.usa_1910_current"))
})

# Downloading 6,122,890 rows in 613 pages.
# user  system elapsed
# 16.510   3.608  23.138

#Test bigrquerystorage
system.time({
  dtb <- bigrquerystorage::bqs_table_download("bigquery-public-data.usa_names.usa_1910_current", "labo-brunotremblay-253317", TRUE)
})
# user  system elapsed
# 12.357   0.486  20.739

#Compare table
all.equal(dta, dtb)
# [1] TRUE
