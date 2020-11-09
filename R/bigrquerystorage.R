#' @useDynLib bigrquerystorage
#' @importFrom Rcpp evalCpp
#' @exportPattern "^[[:alpha:]]+"
NULL

.onUnload <- function (libpath) {
  library.dynam.unload("bigrquerystorage", libpath)
}
