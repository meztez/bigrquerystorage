.onLoad <- function(libname, pkgname) {
  # On Windows, we might have an embedded SSL root cert file.
  # Externally env var, set by the user, takes priority.
  if (.Platform$OS.type == "windows") {
    pem <- system.file(package = .packageName, "roots.pem")
    if (pem != "" && file.exists(pem) &&
        Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH") == "") {
      Sys.setenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH" = normalizePath(pem))
    }
  }
}
