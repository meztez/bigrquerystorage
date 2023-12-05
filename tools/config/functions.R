# -------------------------------------------------------------------------

as_string <- function(x) {
  paste(x, collapse = "")
}

sys <- function(cmd, intern = TRUE, ...) {
  as_string(suppressWarnings(system(cmd, intern = intern, ...)))
}

add_to_path <- function(path) {
  paths <- strsplit(
    Sys.getenv("PATH"),
    .Platform$path.sep,
    fixed = TRUE
  )[[1]]
  paths <- c(normalizePath(path), paths)
  Sys.setenv(PATH = paste(paths, collapse = .Platform$path.sep))
}

# -------------------------------------------------------------------------

# autobrew
rtmp <- Sys.getenv("RUNNER_TEMP")
arch <- R.Version()$arch
autobrew_path <- if (rtmp != "") {
  if (.Platform$OS.type == "windows") {
    file.path(rtmp, "deps-win")
  } else {
    file.path(rtmp, if (arch == "aarch64") "deps" else "deps-arm64")
  }
} else {
  if (.Platform$OS.type == "windows") {
    ".deps-win"
  } else {
    if (arch == "aarch64") ".deps" else ".deps-arm64"
  }
}
autobrew_proto_include_path <- file.path(
  autobrew_path,
  "protobuf-static",
  "25.1",
  "include"
)

# autobrew on mac, download
download_autobrew <- function() {
  path <- autobrew_path
  deps <- c(
    "pkg-config-0.29.2_3",
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
    repo, sub("_[0-9]+$", "", deps), deps, plfm
  )
  for (url in urls) {
    tgt <- file.path(path, basename(url))
    dir.create(dirname(tgt), showWarnings = FALSE, recursive = TRUE)
    if (file.exists(tgt)) next
    download.file(url, tgt, quiet = TRUE)
    untar(tgt, exdir = path)
  }
}

# autobrew on mac, configure
configure_autobrew <- function() {
  path <- autobrew_path
  # e.g. .deps/protobuf-static/25.1/
  pkg_dirs <- file.path(
    path,
    Filter(function(x) {
      file.info(file.path(path, x))$isdir
    }, setdiff(dir(path), c("pkgconfig", "bin")))
  )

  # e.g. .deps/protobuf-static/25.1/INSTALL_RECEIPT.
  # ocntains the files that have @@HOMEBREW_*@@ substitutions
  rcpt <- file.path(
    pkg_dirs,
    vapply(pkg_dirs, dir, character(1)),
    "INSTALL_RECEIPT.json"
  )
  # we only need to replace @@HOMEBREW_CELLAR@@
  cellar <- file.path(getwd(), path)
  for (js in rcpt) {
    lns <- readLines(js, warn = FALSE)
    from <- grep("changed_files", lns)[1] + 1L
    if (is.na(from)) next
    lns <- lns[from:length(lns)]
    to <- grep("^\\s*],\\s*$", lns)[1] - 1L
    if (is.na(to)) next
    lns <- sub("\",?$", "", sub("^\"", "", trimws(lns[1:to])))
    cfs <- file.path(dirname(js), lns)
    for (cf in cfs) {
      cflns <- readLines(cf, warn = FALSE)
      cflns <- gsub("@@HOMEBREW_CELLAR@@", cellar, cflns, fixed = TRUE)
      Sys.chmod(cf, "0664")
      writeLines(cflns, cf)
    }
  }

  # copy pkg-config .pc files to a single directory, then only
  # one directory is on PKG_CONFIG_PATH, and easier to make sure that
  # `normalizePath()` works.
  pcdir <- file.path(path, "pkgconfig")
  unlink(pcdir, recursive = TRUE, force = TRUE)
  dir.create(pcdir, recursive = TRUE)
  pcdirs <- file.path(dirname(rcpt), "lib", "pkgconfig")
  for (pc1 in pcdirs) {
    file.copy(file.path(pc1, dir(pc1)), pcdir)
  }
  Sys.setenv(PKG_CONFIG_PATH = normalizePath(pcdir))

  # copy all binaries to a single directory and put that at the
  # beginning pf PATH, so our version of `protoc` is found first
  bindir <- file.path(path, "bin")
  unlink(bindir, recursive = TRUE, force = TRUE)
  dir.create(bindir, recursive = TRUE)
  bindirs <- file.path(dirname(rcpt), "bin")
  for (b1 in bindirs) {
    file.copy(file.path(b1, dir(b1)), bindir)
  }
  add_to_path(bindir)

  list(
    cflags = sys(
      "pkg-config --cflags --silence-errors grpc++ protobuf"),
    ldflags = sys(
      "pkg-config --libs --static --silence-errors grpc++ protobuf"
    )
  )
}

# -------------------------------------------------------------------------

winlib_path <- autobrew_path
winlib_root <- file.path(winlib_path, "x86_64-w64-mingw32.static.posix")
winlib_proto_include_path <- file.path(winlib_root, "include")
winlib_pkg_config_path <- file.path(winlib_root, "lib", "pkgconfig")
winlib_bin <- file.path(winlib_root, "bin")
winlib_pem <- file.path(winlib_root, "share", "grpc", "roots.pem")

download_win <- function() {
  url <- "https://github.com/gaborcsardi/r-dev-web/releases/download/grpc-1.59.3/grpc-1.59.3.tar.gz"
  dir.create(winlib_path, recursive = TRUE, showWarnings = FALSE)
  tgt <- file.path(winlib_path, basename(url))
  if (!file.exists(tgt)) {
    download.file(url, tgt, quiet = TRUE)
    untar(tgt, exdir = winlib_path, tar = "internal")
  }
  if (!file.copy(winlib_pem, "inst", overwrite = TRUE)) {
    warning("Could not copy grpc root certs from ", winlib_pem)
  }
}

configure_win <- function() {
  Sys.setenv("PKG_CONFIG_PATH" = normalizePath(winlib_pkg_config_path))
  add_to_path(winlib_bin)
  list(
    cflags = sys(
      "pkgconf --cflags --silence-errors grpc++ protobuf"
    ),
    ldflags = sys(
      "pkgconf --libs --silence-errors --static grpc++ protobuf"
    )
  )
}
