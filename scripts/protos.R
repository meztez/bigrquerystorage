#* Get latest version required proto from googleapis
# git clone --depth 1 https://github.com/googleapis/googleapis ./scripts/googleapis")

fpath <- grep("bigquery/storage/v1/", dir("./scripts/googleapis", "storage.proto$", full.names = TRUE, recursive = TRUE), value = TRUE)

import_path <- function(fpath) {
  imports <- lapply(fpath, function(path) { grep("^import", readLines(path), value = TRUE) })
  imports <- gsub("import \"|\";", "", sort(unique(unlist(imports))))
  if (length(imports) > 0) {
    imports <- paste0("./scripts/googleapis/", imports)
    imports <- imports[file.exists(imports)]
    return(sort(unique(c(imports, import_path(imports)))))
  }
  return()
}

fpath <- c(fpath, import_path(fpath))
short_path <- gsub(".*scripts/googleapis/", "", fpath)
config_path <- grep("/v1/", dir(pattern = "bigquerystorage_grpc_service_config.json", full.names = TRUE, recursive = TRUE), value = TRUE)
system(sprintf("protoc -I=/usr/local/include/ -I=./scripts/googleapis/ --cpp_out=./src %s", paste(short_path, collapse = " ")))
system(sprintf("protoc -I=/usr/local/include/ -I=./scripts/googleapis/ --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` --grpc_out=./src %s", short_path[1]))
file.copy(config_path, file.path("src", basename(config_path)), overwrite = TRUE)

