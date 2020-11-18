# library(bigrquerystorage)
# system.time({
# dt <- bqs_table_download(
#     x = "bigquery-public-data.usa_names.usa_1910_current",
#     billing = "labo-brunotremblay-253317")})
#
#
# library(bigrquery)
# system.time({
# dt2 <- bq_table_download("bigquery-public-data.usa_names.usa_1910_current")
# })
# overload_bq_table_download("labo-brunotremblay-253317")
# library(bigrquery)
#
# system.time({
#   dt2 <- bq_table_download("bigquery-public-data.usa_names.usa_1910_current")
# })
#
# library(bigrquerystorage)
# overload_bq_table_download("labo-brunotremblay-253317")
# system.time({
# con <- bigrquery::dbConnect(
#   bigrquery::bigquery(),
#   project = "bigquery-public-data",
#   dataset = "usa_names",
#   billing = "labo-brunotremblay-253317",
#   bigint = "numeric",
#   quiet = FALSE
# )
# dt3 <- dbReadTable(con, "usa_1910_current")
# })
# all.equal(dt2, dt3)

# library(bigrquery)
# library(bigrquerystorage)
#
# # Test bigrquery speed
# system.time({
#   con <- bigrquery::dbConnect(
#     bigrquery::bigquery(),
#     project = "bigquery-public-data",
#     dataset = "usa_names",
#     billing = "labo-brunotremblay-253317",
#     bigint = "numeric",
#     quiet = FALSE
#   )
#   dta <- bigrquery::bq_table_download(bigrquery::as_bq_table("bigquery-public-data.usa_names.usa_1910_current"))
# })
#
# # Downloading 6,122,890 rows in 613 pages.
# # user  system elapsed
# # 16.510   3.608  23.138
#
# # Test bigrquerystorage
# system.time({
#   dtb <- bigrquerystorage::bqs_table_download("bigquery-public-data.usa_names.usa_1910_current", "labo-brunotremblay-253317", FALSE)
# })
# # user  system elapsed
# # 5.436   0.576  13.168
#
# # Compare table
# all.equal(dta, dtb)
# # [1] TRUE

test_that("Everything's good here", {
  expect_true(TRUE)
})
