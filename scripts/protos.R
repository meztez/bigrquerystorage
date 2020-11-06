#* Get latest version required proto from googleapis
#*
#*
unlink(c("./inst/proto", "./scripts/repos"), recursive = TRUE, force = TRUE)

dir.create("./inst/proto", showWarnings = FALSE)
dir.create("./scripts/repos", showWarnings = FALSE)

system("git clone --depth 1 git@github.com:googleapis/googleapis.git ./scripts/repos/googleapis")
system("git clone --depth 1 git@github.com:protocolbuffers/protobuf.git ./scripts/repos/protobuf")

fpath <- grep("bigquery/storage", dir("./scripts/repos/googleapis", "storage.proto$", full.names = TRUE, recursive = TRUE), value = TRUE)

import_path <- function(fpath) {
  imports <- lapply(fpath, function(path) { grep("^import", readLines(path), value = TRUE) })
  imports <- gsub("import \"|\";", "", sort(unique(unlist(imports))))
  if (length(imports) > 0) {
    imports <- c(paste0("./scripts/repos/googleapis/", imports), paste0("./scripts/repos/protobuf/src/", imports))
    imports <- imports[file.exists(imports)]
    return(sort(unique(c(imports, import_path(imports)))))
  }
  return()
}

fpath <- c(fpath, import_path(fpath))
tpath <- gsub("scripts/repos/googleapis|scripts/repos/protobuf/src", "inst/proto", fpath)
sapply(unique(dirname(tpath)), dir.create, recursive = TRUE, showWarnings = FALSE)
copied <- file.copy(fpath, tpath, overwrite = TRUE)
fpath[!copied]

unlink("./scripts/repos", recursive = TRUE, force = TRUE)
