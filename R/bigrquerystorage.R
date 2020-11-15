#' @useDynLib bigrquerystorage
#' @importFrom Rcpp evalCpp
NULL

.onUnload <- function (libpath) {
  library.dynam.unload("bigrquerystorage", libpath)
}
