#' @useDynLib bigrquerystorage
NULL

.onLoad <- function(libname, pkgname){
	if (!isTRUE(Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH", TRUE))) {
		if (file.exists(grpc_mingw_root_pem_path_detect)) {
			Sys.setenv(GRPC_DEFAULT_SSL_ROOTS_FILE_PATH = grpc_mingw_root_pem_path_detect)
		}
	}
  bqs_init_logger()
  # Issue with parallel arrow as.data.frame on Windows
  if (Sys.info()[["sysname"]] == "Windows") {
  	options("arrow.use_threads" = FALSE)
  }
}

.onUnload <- function(libpath) {
  library.dynam.unload("bigrquerystorage", libpath)
}
