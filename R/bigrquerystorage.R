#' @aliases NULL
#' @aliases bigrquerystorage-package
#' @importFrom Rcpp sourceCpp
#' @importFrom bit64 is.integer64
#' @importFrom rlang %||% check_installed is_named
#' @useDynLib bigrquerystorage, .registration = TRUE
"_PACKAGE"

.global <- new.env(parent = emptyenv())

.onLoad <- function(libname, pkgname) {
  if (.Platform$OS.type == "windows" && !nzchar(Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"))) {
    pem <- system.file("roots.pem", package = pkgname, lib.loc = libname)
    if (!nzchar(pem)) {
      pem <- system.file("roots.pem", package = pkgname)
    }
    if (nzchar(pem) && file.exists(pem)) {
      Sys.setenv(GRPC_DEFAULT_SSL_ROOTS_FILE_PATH = normalizePath(pem))
    }
  }
  bqs_initiate()
}

#' @noRd
bqs_initiate <- function() {
  bqs_init_logger()
  if (.Platform$OS.type == "windows" && !nzchar(Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"))) {
    warning("On Windows, GRPC_DEFAULT_SSL_ROOTS_FILE_PATH should be set to the PEM file path to load SSL roots from.")
  }
}

#' Loosely adapted from https://github.com/googleapis/python-api-core/blob/master/google/api_core/client_info.py
#' @noRd
bqs_ua <- function() {
  paste0(
    "bigrquerystorage",
    utils::packageVersion("bigrquerystorage"),
    "r/",
    R.version$major, ".", R.version$minor,
    R.version$platform,
    " grpc/",
    paste0(grpc_version(), collapse = "_")
  )
}

#' @noRd
bqs_parse_table_ref <- function(x) {
  parts <- if (inherits(x, "bq_table")) {
    c(x$project, x$dataset, x$table)
  } else {
    unlist(strsplit(unlist(x), "\\.|:"))
  }
  assertthat::assert_that(length(parts) >= 3L)
  parts[1:3]
}

#' @noRd
bqs_as_bigint_fn <- function(bigint) {
  switch(bigint,
    integer = as.integer,
    integer64 = bit64::as.integer64,
    numeric = as.numeric,
    character = as.character
  )
}

#' @noRd
bqs_check_namespace <- function(pkg, bqs_type) {
  rlang::check_installed(pkg, sprintf("to parse BigQueryStorage '%s' fields.", bqs_type))
}

# work around R CMD check false positives
dummy <- function() {
  Rcpp::compileAttributes
}

