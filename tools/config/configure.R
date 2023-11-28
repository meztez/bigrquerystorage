# Prepare your package for installation here.
# Use 'define()' to define configuration variables.
# Use 'configure_file()' to substitute configuration values.

# Check OS ---------------------------------------------------------------------
win <- .Platform$OS.type == "windows"
mac <- Sys.info()[["sysname"]] == "Darwin"

# find RTOOLS43 for Windows, define pacman installation

install_with_pacman <- function(pkg, rtools43, win) {
	arch <- switch(win, "64" = "x86_64", "32" = "i686")
	# try cran mirrors
	pacman <- function() {
		system(sprintf("%s/usr/bin/pacman -Syu", rtools43))
		system(
			sprintf(
				"%s/usr/bin/pacman -S --noconfirm mingw-w64-%s-%s",
				rtools43,
				arch,
				pkg
			)
		)
	}
	if (pacman() != 0) {
	  stop("Cloud not install mingw-w64-", arch, "-", pkg, " with pacman")
	}
}

if (win) {
	message("*** searching for RTOOLS43 mingw binaries ...", appendLF = FALSE)
	RTOOLS43_ROOT <- gsub("\\\\", "/", Sys.getenv("RTOOLS43_HOME", "c:/rtools43"))
	WIN <- if (.Platform$r_arch == "x64") {"64"} else {"32"}
	MINGW_PREFIX <- paste0("/mingw", WIN)
	BINPREF <- Sys.getenv("BINPREF", paste0(RTOOLS43_ROOT, MINGW_PREFIX, "/bin/"))
	if (dir.exists(BINPREF)) {
		Sys.setenv(PATH = paste(BINPREF, Sys.getenv("PATH"), sep = ";"))
		if (Sys.which("grpc_cpp_plugin") == "") {
			# attempt to install grpc and protoc using msys2
			install_with_pacman("grpc", RTOOLS43_ROOT, WIN)
		}
		message(" OK")
	} else {
		message(" Failed")
	}
}

# Autogenerate sources from proto files ----------------------------------------

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
	if (
		any(
			grepl("^service ",
						readLines(
							file.path(base_proto_path, proto)
						)
			)
		)
	) {
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
		imports <- gsub("import \"|\";",
										"",
										unique(unlist(imports)))
		if (length(imports) > 0) {
			imports <- file.path(bpath, imports)
			imports <- imports[file.exists(imports)]
			return(unique(c(import_order(imports), imports, pbpath)))
		}
		return()
	}
	p <- gsub(paste0("^", bpath),"", import_order(file.path(bpath, pbpath)))
	gsub("^[\\/]", "", p)
}

# protos list
protos <- compile_order(services, base_proto_path)

# protos include path (to locate google/protobuf/*.proto)
ipath <- base_proto_path
debian_local <- "/usr/local/include/"
if (dir.exists(debian_local)) {
	ipath <- c(debian_local, base_proto_path)
}

# compile proto files to generate basic grpc client
message("*** compiling proto files ...")
system(
	sprintf(
		"%s %s --cpp_out=./src --experimental_allow_proto3_optional %s",
		protoc,
		paste0("-I=", ipath, collapse = " "),
		paste(protos, collapse = " ")))
system(
	sprintf(
		"%s %s --plugin=protoc-gen-grpc=%s --grpc_out=./src %s",
		protoc,
		paste0("-I=", ipath, collapse = " "),
		grpc_cpp_plugin, paste(services, collapse = " ")))

# fix OPTIONAL conflict in field_behavior.pb.h enum
field_behavior <- "src/google/api/field_behavior.pb.h"
if (file.exists(field_behavior)) {
	lines <- readLines(field_behavior)
	x <- grep("^enum FieldBehavior", lines)
	linesx <- c(lines[1:(x - 1)],
							"#undef OPTIONAL",
							lines[x:length(lines)])
	writeLines(linesx, field_behavior)
}

# Prepare makevars variables ---------------------------------------------------

# locate pkg-config
pkg_config <- detect_binary("pkg-config")

# openssl pkgconfig location have to be specified for homebrew on MacOS
if (mac) {
	if (dir.exists("/usr/local/opt/openssl@1.1/lib/pkgconfig")) {
		Sys.setenv(PKG_CONFIG_PATH = "/usr/local/opt/openssl@1.1/lib/pkgconfig")
	}
}

# other package sources
pkg_sources <- sort(dir("./src", ".cpp$|.c$"), decreasing = TRUE)

# linker libraries
linker_libs <- system(sprintf("%s --libs protobuf grpc++ openssl", pkg_config), intern = TRUE)

# abseil no longer contains a dynamic_annotations library. brew install
# separatly, from Abseil LTS 20200923, Patch 1
if (mac) {
	if (dir.exists("/usr/local/Cellar/abseil")) {
		if (any(as.numeric(dir("/usr/local/Cellar/abseil")) >= 20200923.1)) {
			linker_libs <- gsub("-labsl_dynamic_annotations", "", linker_libs, fixed = TRUE)
		}
	}
}

# some windows lib needs to be added manually for building with static openssl
if (isTRUE(win)) {
	linker_libs <- paste(linker_libs, "-lcrypt32 -lws2_32 -limagehlp")
} else {
	linker_libs <- paste(linker_libs, "-lupb_collections_lib -lupb_json_lib -lupb_textformat_lib -lupb -lutf8_range_lib -lre2 -lcares -laddress_sorting -lsystemd")
}

# compiler flags
comp_flags <- "-I."
if (win) {
	comp_flags <- paste(comp_flags, "-D_WIN32_WINNT=0x600","-DPROTOBUF_USE_DLLS")
}

# define variable for template
define(CPPF = paste(system(sprintf("%s --cflags grpc", pkg_config), intern = TRUE), "-DSTRICT_R_HEADERS"))
define(CXXF = comp_flags)
define(CF = comp_flags)
define(LIBS = linker_libs)
define(TARGETS = paste(
	c(gsub(".proto$", ".pb.o", protos),
		gsub(".proto$", ".grpc.pb.o", services),
		gsub(".cpp$|.c$", ".o", pkg_sources)),
	collapse = " "))
