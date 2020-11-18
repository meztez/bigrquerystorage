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
inst_path <- gsub(".*scripts/googleapis/", "./inst/protos/", fpath)
for (path in inst_path) {
  if (!dir.exists(dirname(path))) {
    dir.create(dirname(path), recursive = TRUE, showWarnings = FALSE)
  }
}
file.copy(fpath, inst_path, overwrite = TRUE)

system(sprintf("protoc -I=/usr/local/include/ -I=./inst/protos/ --cpp_out=./src %s", paste(inst_path, collapse = " ")))
system(sprintf("protoc -I=/usr/local/include/ -I=./inst/protos/ --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` --grpc_out=./src %s", inst_path[8]))
