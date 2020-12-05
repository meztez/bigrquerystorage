#' @useDynLib bigrquerystorage
NULL

.onLoad <- function(libname, pkgname) {
	# Setup grpc execution environment
	bqs_initiate()
}
