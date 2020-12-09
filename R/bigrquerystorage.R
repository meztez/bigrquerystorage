#' @useDynLib bigrquerystorage
NULL

.onLoad <- function(libname, pkgname) {
	# Setup grpc execution environment
	bqs_initiate()
}

.onAttach <- function(libname, pkgname) {
	# Setup grpc execution environment
	bqs_initiate()
}

.global <- new.env()
