#' Create stub object from protobuf spec
#'
#' @param files the spec file
#' @param protoPath Search path for proto file imports.
#' @param ... Additional parameters for `readProtoFiles` and `readProtoFiles2`.
#' @details Will use [RProtoBuf::readProtoFiles2] when `protoPath` is provided.
#' @return a stub data structure
#' @noRd
proto_skeleton <- function(files, protoPath = NULL, ...){
  SERVICE = "service"
  RPC = "rpc"
  RETURNS = "returns"
  STREAM = "stream"
  PACKAGE = "package"

  services <- list()
  pkg <- ""

  bracket_counter <- function(token, counter) {
    if (token == '{') {
      if (counter == -1L) {
        counter <- 0L
      }
      counter <- counter + 1L
    } else if (token == '}') {
      counter <- counter - 1L
    }
    return(counter)
  }

  doServices <- function(i){
    service_name <- tokens[i+1]
    # services[[service_name]] <<- list()

    opened_brackets <- -1L
    while(tokens[i] != '}' || opened_brackets > 1L) {
      opened_brackets <- bracket_counter(tokens[i], opened_brackets)
      if(tokens[i] == RPC){
        i <- doRPC(i, service_name)
      }
      i <- i + 1
    }

    return(i)
  }

  doRPC <- function(i, service_name) {
    rpc_name = tokens[i+1]
    fn <- list(f=I)

    w <- "RequestType"
    opened_brackets <- -1L
    parsed <- logical(1)
    while(tokens[i] != '}' || opened_brackets > 1L){
      opened_brackets <- bracket_counter(tokens[i], opened_brackets)
      if (tokens[i] == "{") {
        parsed <- TRUE
      }
      if(tokens[i] == '(' && !parsed){
        i <- i + 1
        isStream <- tokens[i] == STREAM
        if(isStream){
          i <- i + 1
        }

        fn[[w]] <- list(name=tokens[i], stream=isStream, proto=sprintf("%s.%s", pkg, tokens[i]))
        w <- "ResponseType"
      }

      i <- i + 1
    }
    fn$name <- sprintf("/%s.%s/%s",pkg, service_name, rpc_name)
    services[[rpc_name]] <<- fn
    return(i)
  }

  if (!is.null(protoPath)) {
    RProtoBuf::readProtoFiles2(files = files, protoPath = protoPath, ...)
    files <- file.path(protoPath, files)
  } else {
    RProtoBuf::readProtoFiles(files = files, ...)
  }

  lines <- unlist(lapply(files, readLines))

  tokens <- Filter(f=nchar, unlist(strsplit(lines, '(^//.*$|\\s+|(?=[{}();]))', perl=TRUE)))

  i <- 1
  while(i <= length(tokens)){
    if(tokens[i] == PACKAGE) {
      pkg <- tokens[i+1];
    }
    else if(tokens[i] == SERVICE){
      i <- doServices(i)
    }

    i <- i + 1
  }

  services
}
