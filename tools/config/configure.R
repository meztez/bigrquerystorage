# Prepare your package for installation here.
# Use 'define()' to define configuration variables.
# Use 'configure_file()' to substitute configuration values.

# Check OS ----------------------------------------------------------------
win <- .Platform$OS.type == "windows"
mac <- Sys.info()[["sysname"]] == "Darwin"

sys <- function(cmd, intern = TRUE, ...) {
  suppressWarnings(system(cmd, intern = intern, ...))
}

# System requirements -----------------------------------------------------

done <- FALSE

# flags specified explicitly
cflags <- Sys.getenv("BQS_CFLAGS")
ldflags <- Sys.getenv("BQS_LDFLAGS")
done <- nzchar(cflags) || nzchar(ldflags)

# working pkg-config finds the required libs
if (!done && Sys.getenv("AUTOBREW_FORCE") == "" &&
	nzchar(Sys.which("pkg-config"))) {
  cflags <- sys("pkg-config --cflags --silence-errors grpc++ protobuf")
  ldflags <- sys("pkg-config --cflags --libs grpc++ protobuf")
  done <- nzchar(cflags) || nzchar(ldflags)
}

# autobrew on mac
if (!done && mac && Sys.getenv("DISABLE_AUTOBREW") == "") {
  deps <- c(
    "grpc-static-1.59.3",
    "re2-static-20231101",
    "protobuf-static-25.1",
    "openssl-static-3.1.1",
    "jsoncpp-static-1.9.5",
    "c-ares-static-1.22.1",
    "abseil-static-20230802.1"
  )
  plfm <- if (R.Version()$arch == "aarch64") "arm64_big_sur" else "big_sur"

  repo <- "https://github.com/gaborcsardi/homebrew-cran"
  urls <- sprintf(
	"%s/releases/download/%s/%s.%s.bottle.tar.gz",
	repo, deps, deps, plfm
  )
  for (url in urls) {
	tgt <- file.path(".deps", basename(url))
	dir.create(dirname(tgt), showWarnings = FALSE, recursive = TRUE)
	if (file.exists(tgt)) next
	download.file(url, tgt, quiet = TRUE)
	untar(tgt, exdir = ".deps")
  }
}

# TODO: download static libs on windows

# give up
# TODO: better error message, suggest solutions
if (!done) {
  stop("Could not find system requirements. :(")
}

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

# fix pragmas
gle_src <- dir(
  file.path("src", "google"),
  pattern = "[.]pb[.]h$",
  recursive = TRUE
)
for (src_ in gle_src) {
  src <- file.path("src", "google", src_)
  lns <- readLines(src)
  lns <- sub("^#pragma ", "# pragma ", lns)
  writeLines(lns, src)
}

# Prepare makevars variables ----------------------------------------------

# other package sources
pkg_sources <- sort(dir("./src", ".cpp$|.c$"), decreasing = TRUE)

# compiler flags
cxxflags <- "-I."

fix_flags <- function(x) {
  x <- gsub("-Wno-float-conversion ", "", x, fixed = TRUE)
  x <- gsub("-Wno-implicit-float-conversion ", "", x, fixed = TRUE)
  x <- gsub("-Wno-implicit-int-float-conversion ", "", x, fixed = TRUE)
  x <- gsub("-Wno-unknown-warning-option ", "", x, fixed = TRUE)
  x <- gsub("-Wno-unused-command-line-argument ", "", x, fixed = TRUE)

  if (grepl("-DNOMINMAX ", x, fixed = TRUE)) {
	x <- gsub("-DNOMINMAX ", "", x, fixed = TRUE)
	x <- paste("-DNOMINMAX", x)
  }

  x
}

# define variable for template
define(CPPF = fix_flags(paste(cflags, "-DSTRING_R_HEADERS")))
define(CXXF = cxxflags)
define(LIBS = fix_flags(ldflags))
define(TARGETS = paste(c(
  gsub(".proto$", ".pb.o", protos),
  gsub(".proto$", ".grpc.pb.o", services),
  gsub(".cpp$|.c$", ".o", pkg_sources)
), collapse = " "))
