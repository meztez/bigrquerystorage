#' Download table from BigQuery using BigQuery Storage API
#' @param x BigQuery table reference `{project}.{dataset}.{table_name}`
#' @param billing Billing project. Used as parent for `CreateReadSession` grpc method.
#' @param as_data_frame Transform to a data.frame after arrow processing.
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
  as_data_frame = TRUE,
  config = system.file("bqs_config/bigquerystorage_grpc_service_config.json",
                       package = "bigrquerystorage",
                       mustWork = TRUE)) {

  bq_table <- strsplit(x, ".", fixed = TRUE)[[1]]

  stopifnot(length(bq_table)==3)

  if (missing(billing)) {
    billing = bq_table[1]
  }

  ipc_stream <- bqs_ipc_stream(billing, bq_table[1], bq_table[2], bq_table[3],
                               client_info = bqs_ua(), service_configuration = config)

  out <- RecordBatchStreamReader$create(ipc_stream)$read_table()

  if (as_data_frame) {
    out <- as.data.frame(out)
  }

  out
}
