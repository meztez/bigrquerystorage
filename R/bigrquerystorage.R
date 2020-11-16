#' @useDynLib bigrquerystorage
NULL

.onUnload <- function (libpath) {
  library.dynam.unload("bigrquerystorage", libpath)
}
