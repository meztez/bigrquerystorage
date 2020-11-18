#' @useDynLib bigrquerystorage
NULL

.onLoad <- function(libname, pkgname){
  bqs_init_logger()
}

.onUnload <- function(libpath) {
  library.dynam.unload("bigrquerystorage", libpath)
}
