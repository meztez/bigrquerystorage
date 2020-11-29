#' Download table from BigQuery using BigQuery Storage API
#' @param x BigQuery table reference `{project}.{dataset}.{table_name}`
#' @param parent Used as parent for `CreateReadSession`
#' grpc method. You can set option `bigquerystorage.project`.
#' @param max_results Maximum number of results to retrieve. Use `Inf` or `-1L`
#' retrieve all rows.
#' @param access_token Access token
#' @param root_certificate The file containing the PEM encoding of the
#' server root certificates. Default to GRPC_DEFAULT_SSL_ROOTS_FILE_PATH.
#' @param snapshot_time Snapshot time
#' @param selected_fields A character vector of field to select from table.
#' @param row_restriction Restriction to apply to the table.
#'@param quiet Should information be printed to console.
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
#' @importFrom arrow RecordBatchStreamReader Table
bqs_table_download <- function(
  x,
  parent = getOption("bigquerystorage.project",""),
  max_results = -1L,
  access_token = "",
  root_certificate = Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH", grpc_mingw_root_pem_path_detect),
  snapshot_time = 0L,
  selected_fields = character(),
  row_restriction = "",
  quiet = NA) {

  # Parameters validation
  bqs_table_name <- unlist(strsplit(unlist(x), "\\.|:"))
  stopifnot(length(bqs_table_name) == 3)
  timestamp_seconds <- as.integer(snapshot_time)
  timestamp_nanos <- as.integer(as.numeric(snapshot_time - timestamp_seconds)*1000000000)
  access_token <- as.character(access_token)
  parent <- as.character(parent)

  if (max_results < 0 || max_results == Inf) {
    max_results <- -1L
  }

  if (nchar(parent) == 0) {
    parent <- bqs_table_name[1]
  }

  quiet <- if (is.na(quiet)) {
    !interactive()
  } else {
    quiet
  }

  if (!quiet) {
    bqs_set_log_verbosity(1L)
  } else {
    bqs_set_log_verbosity(2L)
  }

  raws <- bqs_ipc_stream(
    project = bqs_table_name[1],
    dataset = bqs_table_name[2],
    table = bqs_table_name[3],
    parent = parent,
    n = max_results,
    client_info = bqs_ua(),
    service_configuration = system.file(
      "bqs_config/bigquerystorage_grpc_service_config.json",
      package = "bigrquerystorage",
      mustWork = TRUE
    ),
    access_token = access_token,
    root_certificate = root_certificate,
    timestamp_seconds = timestamp_seconds,
    timestamp_nanos = timestamp_nanos,
    selected_fields = selected_fields,
    row_restriction = row_restriction
  )

  rdr <- RecordBatchStreamReader$create(unlist(raws))
  if (length(raws[[2]]) == 0L) {
    Table$create(
      setNames(
        data.frame(matrix(ncol = rdr$schema$num_fields, nrow = 0)),
        rdr$schema$names
        )
    )
  } else {
    rdr$read_table()
  }

}

#' Substitute bigrquery bq_table_download method. This is very experimental.
#' @param parent Parent project used by the API for billing.
#' @importFrom rlang env_unlock
#' @import bigrquery
#' @export
overload_bq_table_download <- function(parent) {
  utils::assignInNamespace("bq_table_download",  function(
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
    bigrquery:::convert_bigint(as.data.frame(table_data), bigint)
  }, ns = "bigrquery")
  if ("package:bigrquery" %in% search()) {
    env_unlock(environment(bq_table_download))
    namespaceExport(environment(bq_table_download), "bq_table_download")
    lockEnvironment(environment(bq_table_download), bindings = TRUE)
  }
}

grpc_mingw_root_pem_path_detect <-
  if (Sys.info()[["sysname"]] == "Windows") {
    file.path(Sys.getenv("RTOOLS40_HOME"),
              if (Sys.info()[["machine"]] == "x86-64") {"mingw64"} else {"mingw32"},
              "share",
              "grpc",
              "roots.pem")
  } else {
    ""
  }
