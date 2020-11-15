#' Download table from BigQuery using BigQuery Storage API
#' @param x BigQuery table reference `{project}.{dataset}.{table_name}`
#' @param billing Billing project. Used as parent for `CreateReadSession` grpc method.
#' @export
#' @importFrom bigrquery as_bq_table
bqs_table_download <- function(
  x,
  billing,
  config = system.file("bqs_config/bigquerystorage_grpc_service_config.json",
                       package = "bigrquerystorage",
                       mustWork = TRUE)) {
  bq_table <- strsplit(x, ".", fixed = TRUE)[[1]]
  stopifnot(length(bq_table)==3)
  if (missing(billing)) {
    billing = bq_table[1]
  }
  bqs_dl_arrow_batches(billing,
                       bq_table[1],
                       bq_table[2],
                       bq_table[3],
                       client = bqs_ua(),
                       config = config)
}
