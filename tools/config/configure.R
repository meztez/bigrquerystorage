# Prepare your package for installation here.
# Use 'define()' to define configuration variables.
# Use 'configure_file()' to substitute configuration values.

# Check OS ----------------------------------------------------------------
win <- .Platform$OS.type == "windows"
mac <- Sys.info()[["sysname"]] == "Darwin"

# Autogenerate sources from proto files -----------------------------------

# binary locator, fail if not available
detect_binary <- function(binary) {
  message(sprintf("*** searching for %s ...", binary), appendLF = FALSE)
  path <- Sys.which(binary)
  if (path == "") {
    if (binary == "pkg-config") {
      install_with_pacman("pkgconf", RTOOLS43_ROOT, WIN)
      path <- Sys.which(binary)
    } else {
      message(" Failed")
      stop("Could not find ", binary)
    }
  } else {
    message(" OK")
  }
  path
}

# locate codegen binaries
protoc <- detect_binary("protoc")
grpc_cpp_plugin <- detect_binary("grpc_cpp_plugin")

# locate packages proto files
base_proto_path <- "./inst/protos/"
protos <- dir(base_proto_path, ".proto", recursive = TRUE)

# identify service protos
services <- character()
for (proto in protos) {
  if (any(grepl(
    "^service ",
    readLines(file.path(base_proto_path, proto))
  ))) {
    services <- c(services, proto)
  }
}

# determine proto compile order
compile_order <- function(pbpath, bpath) {
  import_order <- function(pbpath) {
    imports <- lapply(
      pbpath,
      function(path) {
        grep("^import", readLines(path), value = TRUE)
      }
    )
    imports <- gsub("import \"|\";", "", unique(unlist(imports)))
    if (length(imports) > 0) {
      imports <- file.path(bpath, imports)
      imports <- imports[file.exists(imports)]
      return(unique(c(import_order(imports), imports, pbpath)))
    }
    return()
  }
  p <- gsub(paste0("^", bpath), "", import_order(file.path(bpath, pbpath)))
  gsub("^[\\/]", "", p)
}

# protos list
protos <- compile_order(services, base_proto_path)

# protos include path (to locate google/protobuf/*.proto)
ipath <- base_proto_path

# compile proto files to generate basic grpc client
message("*** compiling proto files ...")
system(
  sprintf(
    "%s %s --cpp_out=./src --experimental_allow_proto3_optional %s",
    protoc,
    paste0("-I=", ipath, collapse = " "),
    paste(protos, collapse = " ")
  )
)
system(
  sprintf(
    "%s %s --plugin=protoc-gen-grpc=%s --grpc_out=./src %s",
    protoc,
    paste0("-I=", ipath, collapse = " "),
    grpc_cpp_plugin, paste(services, collapse = " ")
  )
)

# fix OPTIONAL conflict in field_behavior.pb.h enum
field_behavior <- "src/google/api/field_behavior.pb.h"
if (file.exists(field_behavior)) {
  lines <- readLines(field_behavior)
  x <- grep("^enum FieldBehavior", lines)
  linesx <- c(
    lines[1:(x - 1)],
    "#undef OPTIONAL",
    lines[x:length(lines)]
  )
  writeLines(linesx, field_behavior)
}

# Prepare makevars variables ----------------------------------------------

# locate pkg-config
pkg_config <- detect_binary("pkg-config")

# other package sources
pkg_sources <- sort(dir("./src", ".cpp$|.c$"), decreasing = TRUE)

# compiler flags
comp_flags <- "-I."

# define variable for template
define(CPPF = paste(
  system(sprintf("%s --cflags grpc", pkg_config), intern = TRUE),
  "-DSTRICT_R_HEADERS"
))
define(CXXF = comp_flags)
define(CF = comp_flags)
define(LIBS = linker_libs)
define(TARGETS = paste(
  c(
    gsub(".proto$", ".pb.o", protos),
    gsub(".proto$", ".grpc.pb.o", services),
    gsub(".cpp$|.c$", ".o", pkg_sources)
  ),
  collapse = " "
))
