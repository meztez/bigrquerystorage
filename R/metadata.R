#' Loosely adapted from https://github.com/googleapis/python-api-core/blob/master/google/api_core/client_info.py
#' @noMd
bqs_ua <- function() {
  METRICS_METADATA_KEY <- "x-goog-api-client"
  c(METRICS_METADATA_KEY,
    paste0(
      "bigrquerystorage ",
      "gl-r/",
      R.version$major,
      ".",
      R.version$minor,
      R.version$platform,
      " grpc/",
      paste0(grpc_version(), collapse = "_"),
      " gccl/",
      utils::packageVersion("bigrquerystorage")
    ))
}

#' @noRd
to_grpc_metadata <- function(metadata) {
  ROUTING_METADATA_KEY <- "x-goog-request-params"
  m <- vapply(metadata, utils::URLencode, character(1))
  m <- paste(names(m), m, sep = "=")
  mk <- rep_len(ROUTING_METADATA_KEY, length(m)*2)
  mk[seq_len(length(m))*2] <- m
  c(bqs_ua(), mk)
}
