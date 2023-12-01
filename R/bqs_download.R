#' Download table from BigQuery using BigQuery Storage API
#' @param x BigQuery table reference `{project}.{dataset}.{table_name}`
#' @param parent Used as parent for `CreateReadSession`
#' grpc method. You can set option `bigquerystorage.project`.
#' @param snapshot_time Snapshot time
#' @param selected_fields A character vector of field to select from table.
#' @param row_restriction Restriction to apply to the table.
#' @param n_max Maximum number of results to retrieve. Use `Inf` or `-1L`
#' retrieve all rows.
#' @param as_tibble Should data be returned as tibble. Default is to return
#' as arrow Table from raw IPC stream.
#' @param quiet Should information be printed to console.
#' @param bigint The R type that BigQuery's 64-bit integer types should be mapped to.
#'   The default is `"integer"` which returns R's `integer` type but results in `NA` for
#'   values above/below +/- 2147483647. `"integer64"` returns a [bit64::integer64],
#'   which allows the full range of 64 bit integers.
#' @param max_results Deprecated
#' @export
#' @importFrom arrow RecordBatchStreamReader Table
#' @importFrom lifecycle deprecated deprecate_warn
#' @importFrom tibble tibble
bqs_table_download <- function(
  x,
  parent = getOption("bigquerystorage.project", ""),
  snapshot_time = NA,
  selected_fields = character(),
  row_restriction = "",
  n_max = Inf,
  quiet = NA,
  as_tibble = FALSE,
  bigint = c("integer", "integer64", "numeric", "character"),
  max_results = lifecycle::deprecated()) {

  # Parameters validation
  bqs_table_name <- unlist(strsplit(unlist(x), "\\.|:"))
  assertthat::assert_that(length(bqs_table_name) >= 3)
  assertthat::assert_that(is.character(row_restriction))
  assertthat::assert_that(is.character(selected_fields))
  if (is.na(snapshot_time)) {
    snapshot_time <- 0L
  } else {
    assertthat::assert_that(inherits(snapshot_time, "POSIXct"))
  }
  timestamp_seconds <- as.integer(snapshot_time)
  timestamp_nanos <- as.integer(as.numeric(snapshot_time - timestamp_seconds)*1000000000)
  if (lifecycle::is_present(max_results)) {
    lifecycle::deprecate_warn("1.99.0", "bqs_table_download(max_results)",
                              "bqs_table_download(n_max)")
    n_max <- max_results
  }

  parent <- as.character(parent)
  if (nchar(parent) == 0) { parent <- bqs_table_name[1] }

  if (n_max < 0 || n_max == Inf) {
    n_max <- -1L
    trim_to_n <- FALSE
  } else {
    trim_to_n <- TRUE
  }

  bigint <- match.arg(bigint)

  quiet <- isTRUE(quiet)

  bqs_auth()

  raws <- bqs_ipc_stream(
    client = .global$client$ptr,
    project = bqs_table_name[1],
    dataset = bqs_table_name[2],
    table = bqs_table_name[3],
    parent = parent,
    n = n_max,
    selected_fields = selected_fields,
    row_restriction = row_restriction,
    timestamp_seconds = timestamp_seconds,
    timestamp_nanos = timestamp_nanos,
    quiet = quiet
  )

  rdr <- RecordBatchStreamReader$create(unlist(raws))
  # There is currently no way to create an Arrow Table from a
  # RecordBatchStreamReader when there is a schema but no batches.
  if (length(raws[[2]]) == 0L) {
    tb <- Table$create(
      stats::setNames(
        data.frame(matrix(ncol = rdr$schema$num_fields, nrow = 0)),
        rdr$schema$names
      )
    )
  } else {
    tb <- rdr$read_table()
  }

  if (isTRUE(as_tibble)) {
    tb <- parse_postprocess(
      tibble::tibble(
        as.data.frame(tb)
      ),
      bigint
    )
  }

  # Batches do not support a n_max so we get just enough results before
  # exiting the streaming loop.
  if (isTRUE(trim_to_n) && nrow(tb) > 0) {
    tb <- tb[1:n_max, ]
  }

  return(tb)

}

#' Initialize client
#' @export
#' @details
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
bqs_auth <- function() {

  if (!is.null(.global$client) &&
      (as.numeric(Sys.time()) - .global$client$creation < 30)) {
    return(invisible())
  } else {
    bqs_deauth()
  }

  # Recycling bigrquery credentials
  if (bigrquery::bq_has_token()) {
    if (!is.null(asNamespace("bigrquery")$.auth$cred$credentials$refresh_token)) {
      refresh_token <- c(
        type = "authorized_user",
        client_secret = asNamespace("bigrquery")$.auth$cred$app$secret,
        client_id = asNamespace("bigrquery")$.auth$cred$app$key,
        refresh_token = asNamespace("bigrquery")$.auth$cred$credentials$refresh_token
      )
      refresh_token <- paste0("{", paste0(
        '"', names(refresh_token), '":"', refresh_token, '"',
        collapse = ","), "}")
      access_token <- ""
    } else {
      access_token <- asNamespace("bigrquery")$.auth$get_cred()$credentials$access_token
      refresh_token <- ""
    }
  } else {
    access_token <- ""
    refresh_token <- ""
  }

  root_certificate = Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH", grpc_mingw_root_pem_path_detect())

  .global$client$ptr <- bqs_client(
    client_info = bqs_ua(),
    service_configuration = system.file(
      "bqs_config/bigquerystorage_grpc_service_config.json",
      package = "bigrquerystorage",
      mustWork = TRUE
    ),
    refresh_token = refresh_token,
    access_token = access_token,
    root_certificate = root_certificate
  )

  .global$client$creation <- as.numeric(Sys.time())

  invisible()

}

#' Destroy client
#' @rdname bqs_auth
#' @export
bqs_deauth <- function() {
  .global$client <- NULL
  invisible()
}

#' Substitute bigrquery bq_table_download method. This is very experimental.
#' @param parent Parent project used by the API for billing.
#' @importFrom rlang env_unlock
#' @import bigrquery
#' @export
overload_bq_table_download <- function(parent) {
  utils::assignInNamespace("bq_table_download",  function(
    x, n_max = Inf, page_size = NULL, start_index = 0L, max_connections = 6L,
    quiet = NA, bigint = c("integer", "integer64", "numeric", "character"), max_results = deprecated()) {

    x <- bigrquery::as_bq_table(x)
    if (lifecycle::is_present(max_results)) {
      lifecycle::deprecate_warn("1.4.0", "bq_table_download(max_results)",
                                "bq_table_download(n_max)")
      n_max <- max_results
    }
    assertthat::assert_that(is.numeric(n_max), length(n_max) == 1)
    assertthat::assert_that(is.numeric(start_index), length(start_index) == 1)
    bigint <- match.arg(bigint)
    table_data <- bigrquerystorage::bqs_table_download(
      x = x,
      parent = parent,
      n_max = n_max + start_index,
      as_tibble = TRUE,
      quiet = quiet,
      bigint = bigint
    )
    if (start_index > 0L) {
      table_data <- table_data[start_index:nrow(table_data), ]
    }
    return(table_data)
  }, ns = "bigrquery")
  if ("package:bigrquery" %in% search()) {
    env_unlock(environment(bq_table_download))
    namespaceExport(environment(bq_table_download), "bq_table_download")
    lockEnvironment(environment(bq_table_download), bindings = TRUE)
  }
}

grpc_mingw_root_pem_path_detect <- function() {
  if (Sys.info()[["sysname"]] == "Windows") {
    RTOOLS43_ROOT <- gsub("\\\\", "/", Sys.getenv("RTOOLS43_HOME", "c:/rtools43"))
    WIN <- if (Sys.info()[["machine"]] == "x86-64") {"64"} else {"32"}
    MINGW_PREFIX <- paste0("mingw", WIN)
    file.path(RTOOLS43_ROOT,
              MINGW_PREFIX,
              "share",
              "grpc",
              "roots.pem")
  } else {
    ""
  }
}

# BigQuery storage --------------------------------------------------------
#' @noRd
bqs_initiate <- function() {
  if (!isTRUE(Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH", TRUE))) {
    if (file.exists(grpc_mingw_root_pem_path_detect())) {
      Sys.setenv(GRPC_DEFAULT_SSL_ROOTS_FILE_PATH = grpc_mingw_root_pem_path_detect())
    }
  }
  bqs_init_logger()
  # Issue with parallel arrow as.data.frame on Windows
  if (.Platform$OS.type == "windows") {
    options("arrow.use_threads" = FALSE)
  }
}

# utils ------------------------------------------------------------------
`%||%` <- function (x, y) if (is.null(x)) y else x

parse_postprocess <- function(df, bigint) {

  if (bigint != "integer64") {
    as_bigint <- switch(
      bigint,
      integer = as.integer,
      numeric = as.numeric,
      character = as.character
    )
    df <- col_apply(df, bit64::is.integer64, as_bigint)
  }

  df
}

col_apply <- function(x, p, f) {
  if (is.list(x)) {
    x[] <- lapply(x, col_apply, p = p, f = f)
    x
  } else if (p(x)) {
    f(x)
  } else {
    x
  }
}
