#' Download table from BigQuery using BigQuery Storage API
#' @param x BigQuery table reference `{project}.{dataset}.{table_name}`
#' @param billing Billing project. Used as parent for `CreateReadSession` grpc method.
#' @param as_df Transform to a data.frame after arrow processing.
#' @param config Path to BigQuery Storage grpc service configuration file.
#' @details
#'
#' About Crendentials
#'
#' If your application runs inside a Google Cloud environment that has
#' a default service account, your application can retrieve the service
#' account credentials to call Google Cloud APIs. Such environments
#' include Compute Engine, Google Kubernetes Engine, App Engine,
#' Cloud Run, and Cloud Functions. We recommend using this strategy
#' because it is more convenient and secure than manually passing credentials.
#'
#' Additionally, we recommend you use Google Cloud Client Libraries for
#' your application. Google Cloud Client Libraries use a library called
#' Application Default Credentials (ADC) to automatically find your service
#' account credentials. ADC looks for service account credentials
#' in the following order:
#'
#' 1. If the environment variable GOOGLE_APPLICATION_CREDENTIALS is set,
#' ADC uses the service account file that the variable points to.
#' 2. If the environment variable GOOGLE_APPLICATION_CREDENTIALS isn't
#' set, ADC uses the default service account that Compute Engine,
#' Google Kubernetes Engine, App Engine, Cloud Run, and Cloud
#' Functions provide.
#' 3. If ADC can't use either of the above credentials, an error occurs.
#' @export
#' @importFrom arrow record_batch Table RecordBatchStreamReader
bqs_table_download <- function(
  x,
  billing,
  as_df = FALSE,
  config = system.file("bqs_config/bigquerystorage_grpc_service_config.json",
                       package = "bigrquerystorage",
                       mustWork = TRUE)) {
  bq_table <- strsplit(x, ".", fixed = TRUE)[[1]]
  stopifnot(length(bq_table)==3)
  if (missing(billing)) {
    billing = bq_table[1]
  }
  raw <- bqs_dl_arrow_batches(
    billing,
    bq_table[1],
    bq_table[2],
    bq_table[3],
    clientInfo = bqs_ua(),
    config = config)
  arrow_reader <- RecordBatchStreamReader$create(raw$schema)

  arrow_dt <- do.call(
    Table$create,
    lapply(
      raw$arrow_batches[lengths(raw$arrow_batches) != 0],
      record_batch,
      schema = arrow_reader$schema))
  if (as_df) {
    as.data.frame(arrow_dt)
  }
}
