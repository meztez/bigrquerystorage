#' Download table from BigQuery using BigQuery Storage API
#' @param x BigQuery table reference `{project}.{dataset}.{table_name}`
#' @param parent Used as parent for `CreateReadSession`
#' grpc method.
#' @param as_data_frame Transform to a data.frame after arrow processing.
#' @param access_token Access token
#' @param snapshot_time Snapshot time
#' @param selected_fields A character vector of field to select from table.
#' @param row_restriction Restriction to apply to the table.
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
#' @importFrom arrow RecordBatchStreamReader
bqs_table_download <- function(
  x,
  parent,
  as_data_frame = TRUE,
  access_token = "",
  snapshot_time = 0L,
  selected_fields = character(),
  row_restriction = "") {

  # Parameters validation
  stopifnot(is.character(x))
  bq_table <- strsplit(x, ".", fixed = TRUE)[[1]]
  stopifnot(length(bq_table) == 3)
  timestamp_seconds <- as.integer(snapshot_time)
  timestamp_nanos <- as.integer(as.numeric(snapshot_time-timestamp_seconds)*1000000000)

  if (missing(parent)) {
    parent <- bq_table[1]
  }

  ipc_stream <- bqs_ipc_stream(
    project= bq_table[1],
    dataset = bq_table[2],
    table = bq_table[3],
    parent = parent,
    client_info = bqs_ua(),
    service_configuration = system.file(
      "bqs_config/bigquerystorage_grpc_service_config.json",
      package = "bigrquerystorage",
      mustWork = TRUE
    ),
    access_token = access_token,
    timestamp_seconds = timestamp_seconds,
    timestamp_nanos = timestamp_nanos,
    selected_fields = selected_fields,
    row_restriction = row_restriction
  )

  out <- RecordBatchStreamReader$create(ipc_stream)$read_table()

  if (as_data_frame) {
    out <- as.data.frame(out)
  }

  out
}

#' Substitute bigrquery bq_table_download method. This is very experimental.
#' @param parent Parent project used by the API for billing.
#' @importFrom rlang env_unlock
#' @import bigrquery
#' @export
overload_bq_table_download <- function(parent) {
  if (!"package:bigrquery" %in% search()) {
    stop("bigrquery library not loaded")
  }
  assignInNamespace("bq_table_download",  function(
    x, max_results = Inf, page_size = 10000, start_index = 0L, max_connections = 6L,
    quiet = NA, bigint = c("integer", "integer64", "numeric", "character")) {
      x <- bigrquery::as_bq_table(x)
      assertthat::assert_that(is.numeric(max_results), length(max_results) == 1)
      assertthat::assert_that(is.numeric(start_index), length(start_index) == 1)
      bigint <- match.arg(bigint)
      table_data <- bigrquerystorage::bqs_table_download(
        x = paste(x, collapse = "."),
        parent = parent,
        access_token = bigrquery:::.auth$cred$credentials$access_token,
      )
      bigrquery:::convert_bigint(table_data, bigint)
  }, ns = "bigrquery")
  env_unlock(environment(bq_table_download))
  namespaceExport(environment(bq_table_download), "bq_table_download")
  lockEnvironment(environment(bq_table_download), bindings = TRUE)
}
