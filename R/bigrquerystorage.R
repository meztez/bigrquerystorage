#' @aliases NULL
#' @aliases bigrquerystorage-package
#' @importFrom Rcpp sourceCpp
#' @importFrom bit64 is.integer64
#' @useDynLib bigrquerystorage, .registration = TRUE
"_PACKAGE"

.onLoad <- function(libname, pkgname) {
  # Setup grpc execution environment
  bqs_initiate()
}

.onAttach <- function(libname, pkgname) {
  # Setup grpc execution environment
  bqs_initiate()
}

.global <- new.env()

# work around R CMD check false positives

dummy <- function() {
  Rcpp::compileAttributes
}
