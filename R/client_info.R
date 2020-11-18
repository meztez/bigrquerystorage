#' Loosely adapted from https://github.com/googleapis/python-api-core/blob/master/google/api_core/client_info.py
#' @noMd
bqs_ua <- function() {
  paste0(
    "bigrquerystorage",
    utils::packageVersion("bigrquerystorage"),
    "r/",
    R.version$major,
    ".",
    R.version$minor,
    R.version$platform,
    " grpc/",
    paste0(grpc_version(), collapse = "_")
  )
}
