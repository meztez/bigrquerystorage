#' Build a client handle
#'
#' @param verbose Default `FALSE`. When `TRUE`, equivalent to setting
#' environment variable `GRPC_VERBOSITY` to `DEBUG` but redirecting
#' `stdout` to R console.
#' @param credentials A valid access token. (WIP Not implemented)
#' @param credentials_file A path to a service account file.
#' @details `credentials` and `credentials_file` are mutually exclusive.
#' Environment variable `GOOGLE_APPLICATION_CREDENTIALS` will be set with
#' `credentials_file` value when provided.
#'
#' If your application runs inside a Google Cloud environment that has
#' a default service account, your application can retrieve the service
#' account credentials to call Google Cloud APIs. Such environments
#' include Compute Engine, Google Kubernetes Engine, App Engine,
#' Cloud Run, and Cloud Functions. We recommend using this strategy
#' because it is more convenient and secure than manually passing credentials.
#'
#' Additionally, we recommend you use Google Cloud Client Libraries for
#' your application. Google Cloud Client Libraries use a library called
#' Application Default Credentials (ADC) to automatically find your service
#' account credentials. ADC looks for service account credentials
#' in the following order:
#'
#' 1. If the environment variable GOOGLE_APPLICATION_CREDENTIALS is set,
#' ADC uses the service account file that the variable points to.
#' 2. If the environment variable GOOGLE_APPLICATION_CREDENTIALS isn't
#' set, ADC uses the default service account that Compute Engine,
#' Google Kubernetes Engine, App Engine, Cloud Run, and Cloud
#' Functions provide.
#' 3. If ADC can't use either of the above credentials, an error occurs.
#' @return client handle
#' @export
bqs_client <- function(verbose = FALSE, credentials, credentials_file) {

  if (!missing(credentials) && !missing(credentials_file)) {
    stop("`credentials` and `credentials_file` are mutually exclusive")
  }

  if (!missing(credentials_file) && file.exists(credentials_file)) {
    Sys.setenv(GOOGLE_APPLICATION_CREDENTIALS=credentials_file)
  }

  target <- "bigquerystorage.googleapis.com:443"
  protoPath <- system.file("proto/", package = .packageName, mustWork = TRUE)
  spec <- "google/cloud/bigquery/storage/v1/storage.proto"
  impl <- proto_skeleton(spec, protoPath)
  rpc_init(verbose)

  lapply(impl, function(fn)
    {
      RequestDescriptor <- RProtoBuf::P(fn[["RequestType"]]$proto)
      ResponseDescriptor <- RProtoBuf::P(fn[["ResponseType"]]$proto)

      list(
        call = function(x, metadata=character(0), ...) {
          RProtoBuf::read(ResponseDescriptor,
                          rpc(target,
                              fn$name,
                              RProtoBuf::serialize(x, NULL),
                              to_grpc_metadata(metadata),
                              ...))
          },
        build = function(...) {
          RProtoBuf::new(RequestDescriptor, ...)
        }
      )
    })
}


