#' Download table data
#'
#' This retrieves rows block in a stream using a grpc protocol.
#' It is most suitable for results of larger queries (>100 MB, say).
#'
#' @param x Table reference `{project}.{dataset}.{table_name}`
#' @param parent Used as parent for `CreateReadSession`.
#' grpc method. Default is to use option `bigquerystorage.project` value.
#' @param snapshot_time Table modifier `snapshot time` as `POSIXct`.
#' @param selected_fields Table read option `selected_fields`. A character vector of field to select from table.
#' @param row_restriction Table read option `row_restriction`. A character. SQL text filtering statement.
#' @param sample_percentage Table read option `sample_percentage`. A numeric `0 <= sample_percentage <= 100`. Not compatible with `row_restriction`.
#' @param n_max Maximum number of results to retrieve. Use `Inf` or `-1L`
#' retrieve all rows.
#' @param quiet Should information be printed to console.
#' @param as_tibble Should data be returned as tibble. Deprecated; use `type` instead.
#' @param bigint The R type that BigQuery's 64-bit integer types should be mapped to.
#'   The default is `"integer"` which returns R's `integer` type but results in `NA` for
#'   values above/below +/- 2147483647. `"integer64"` returns a [bit64::integer64],
#'   which allows the full range of 64 bit integers.
#' @param max_results Deprecated
#' @param type The output container type: `"tibble"` (default), `"data.frame"`,
#'   `"data.table"`, or `"arrow"` (returns a `nanoarrow_array_stream`).
#' @param max_stream_count Maximum number of streams to request from BigQuery Storage
#'   (`0L` lets the server choose an optimal number of streams).
#' @param compression Arrow record batch compression codec: `"auto"` (default, uses
#'   `"zstd"` when supported by `nanoarrow`), `"zstd"`, `"lz4"`, or `"none"`.
#' @details
#' More details about table modifiers and table options are available from the
#' API Reference documentation. (See [TableModifiers](https://docs.cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#tablemodifiers) and
#' [TableReadOptions](https://docs.cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#tablereadoptions))
#' @return By default (`type = "tibble"`), a [tibble::tibble]. Alternatively a
#'   `data.frame`, `data.table::data.table`, or `nanoarrow_array_stream` according to `type`.
#' @export
#' @importFrom lifecycle deprecated deprecate_warn
#' @importFrom tibble tibble new_tibble
#' @importFrom rlang is_missing local_options
#' @import nanoarrow
bqs_table_download <- function(
    x,
    parent = getOption("bigquerystorage.project", ""),
    snapshot_time = NA,
    selected_fields = character(),
    row_restriction = "",
    sample_percentage,
    n_max = Inf,
    quiet = NA,
    as_tibble = lifecycle::deprecated(),
    bigint = c("integer", "integer64", "numeric", "character"),
    max_results = lifecycle::deprecated(),
    type = c("tibble", "data.frame", "data.table", "arrow"),
    max_stream_count = 0L,
    compression = c("auto", "zstd", "lz4", "none")) {
  # Parameters validation
  bqs_table_name <- unlist(strsplit(unlist(x), "\\.|:"))
  assertthat::assert_that(length(bqs_table_name) >= 3)
  assertthat::assert_that(is.character(row_restriction), length(row_restriction) == 1)
  assertthat::assert_that(is.character(selected_fields))
  if (is.na(snapshot_time)) {
    snapshot_time <- 0L
  } else {
    assertthat::assert_that(inherits(snapshot_time, "POSIXct"))
  }
  timestamp_seconds <- as.integer(snapshot_time)
  timestamp_nanos <- as.integer(as.numeric(snapshot_time - timestamp_seconds) * 1000000000)
  if (lifecycle::is_present(max_results)) {
    lifecycle::deprecate_warn(
      "1.0.0", "bqs_table_download(max_results)",
      "bqs_table_download(n_max)"
    )
    n_max <- max_results
  }
  if (!rlang::is_missing(sample_percentage)) {
    assertthat::assert_that(
      is.numeric(sample_percentage),
      sample_percentage >= 0,
      sample_percentage <= 100
    )
    if (nchar(row_restriction)) {
      stop("Parameters `row_restriction` and `sample_percentage` cannot be use in the same query.")
    }
  } else {
    sample_percentage <- -1L
  }

  parent <- as.character(parent)
  if (!nchar(parent)) {
    parent <- bqs_table_name[1]
  }

  if (n_max < 0 || n_max == Inf) {
    n_max <- -1L
    trim_to_n <- FALSE
  } else {
    trim_to_n <- TRUE
  }

  bigint <- match.arg(bigint)
  type <- match.arg(type)
  compression <- match.arg(compression)

  if (type == "data.table") {
    bqs_check_namespace("data.table", "data.table output")
  }

  compression_codec <- switch(compression,
    auto = if (isTRUE(nanoarrow::nanoarrow_with_zstd())) 2L else if (isTRUE(nanoarrow::nanoarrow_with_lz4())) 1L else 0L,
    zstd = 2L,
    lz4 = 1L,
    none = 0L
  )

  quiet <- isTRUE(quiet)

  bqs_auth()

  raws <- bqs_ipc_stream(
    client = .global$client$ptr,
    project = bqs_table_name[1],
    dataset = bqs_table_name[2],
    table = bqs_table_name[3],
    parent = parent,
    n = n_max,
    selected_fields = selected_fields,
    row_restriction = row_restriction,
    sample_percentage = sample_percentage,
    timestamp_seconds = timestamp_seconds,
    timestamp_nanos = timestamp_nanos,
    quiet = quiet,
    max_stream_count = as.integer(max_stream_count),
    compression_codec = compression_codec
  )

  rlang::local_options(nanoarrow.warn_unregistered_extension = FALSE)
  stream <- nanoarrow::read_nanoarrow(raws)
  if (type == "arrow") {
    return(stream)
  }

  schema <- stream$get_schema()
  to <- int64_ptype(schema)
  df <- bqs_convert_stream(stream, to, bigint, nanoarrow::convert_array_stream)
  tb <- parse_postprocess_schema(df, schema, bigint)

  # Batches do not support a n_max so we get just enough results before
  # exiting the streaming loop.
  if (isTRUE(trim_to_n) && nrow(tb) > n_max) {
    tb <- tb[seq_len(n_max), , drop = FALSE]
  }

  switch(type,
    tibble = tb,
    "data.frame" = as.data.frame(tb),
    "data.table" = data.table::setDT(as.data.frame(tb))
  )
}

#' Initialize bigrquerystorage client
#' @export
#' @details
#' Will attempt to reuse `bigrquery` credentials.
#'
#' About Credentials
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
#' @return No return value, called for side effects.
bqs_auth <- function() {

  rlang::check_installed("bigrquery", "`bigrquery` have to be available to use `bigrquerystorage`.")

  if (!is.null(.global$client) &&
    (as.numeric(Sys.time()) - .global$client$creation < 1800)) {
    return(invisible())
  } else {
    bqs_deauth()
  }

  # Recycling bigrquery credentials
  if (bigrquery::bq_has_token()) {
    .authcred <- asNamespace("bigrquery")[[".auth"]][["cred"]]
    if (!is.null(refresh_token <- .authcred[["credentials"]][["refresh_token"]])) {
      .authsource <- c(.authcred[["client"]], .authcred[["app"]])
      access_token <- ""
      refresh_token <- c(
        type = "authorized_user",
        client_secret = .authsource[["secret"]],
        client_id = .authsource[["key"]],
        refresh_token = refresh_token
      )
      refresh_token <- paste0("{", paste0(
        '"', names(refresh_token), '":"', refresh_token, '"',
        collapse = ","
      ), "}")
    } else {
      access_token <- .authcred[["credentials"]][["access_token"]]
      refresh_token <- ""
    }
  } else {
    access_token <- ""
    refresh_token <- ""
  }

  root_certificate <- Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH")

  .global$client$ptr <- bqs_client(
    client_info = bqs_ua(),
    service_configuration = system.file(
      "bqs_config/bigquerystorage_grpc_service_config.json",
      package = "bigrquerystorage",
      mustWork = TRUE
    ),
    refresh_token = refresh_token,
    access_token = access_token,
    root_certificate = root_certificate
  )

  .global$client$creation <- as.numeric(Sys.time())

  invisible()
}

#' Close bigrquerystorage client
#' @rdname bqs_auth
#' @export
bqs_deauth <- function() {
  if (!is.null(.global[["client"]])) {
    rm("client", envir = .global)
  }
  invisible()
}

# BigQuery storage --------------------------------------------------------
#' @noRd
bqs_initiate <- function() {
  bqs_init_logger()
  if (.Platform$OS.type == "windows") {
    if (Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH") == "") {
      warning("On Windows, GRPC_DEFAULT_SSL_ROOTS_FILE_PATH should be set to the PEM file path to load SSL roots from.")
    }
  }
}

# utils ------------------------------------------------------------------

#' Build a conversion target for a nanoarrow stream where every int64 column,
#' including those nested in structs and lists (REPEATED columns), becomes a
#' bit64::integer64 instead of the lossy double that the default conversion
#' produces. See https://github.com/r-dbi/bigrquery/issues/689.
#' @noRd
int64_ptype <- function(schema, ptype = nanoarrow::infer_nanoarrow_ptype(schema)) {
  if (identical(schema$format, "l")) {
    bit64::integer64()
  } else if (is.data.frame(ptype)) {
    ptype[] <- mapply(int64_ptype, schema$children, ptype, SIMPLIFY = FALSE)
    ptype
  } else if (inherits(ptype, "vctrs_list_of") && length(schema$children)) {
    attr(ptype, "ptype") <- int64_ptype(schema$children[[1]], attr(ptype, "ptype"))
    ptype
  } else {
    ptype
  }
}

#' Post-process converted R data.frame using the Arrow schema directly
#' (eliminates the redundant REST API call to `bigrquery::bq_table_fields()`).
#' @noRd
parse_postprocess_schema <- function(df, schema, bigint) {
  as_bigint <- switch(bigint,
    integer = as.integer,
    integer64 = bit64::as.integer64,
    numeric = as.numeric,
    character = as.character
  )

  walk_col <- function(col, sch) {
    fmt <- sch$format
    ext <- sch$metadata[["ARROW:extension:name"]]
    if (identical(fmt, "+s")) {
      if (is.data.frame(col) && !inherits(col, "tbl_df")) {
        col <- tibble::new_tibble(col, nrow = nrow(col))
      }
      if (is.list(col) && length(col) == length(sch$children)) {
        col[] <- mapply(walk_col, col, sch$children, SIMPLIFY = FALSE)
      }
      return(col)
    }
    if (identical(fmt, "+l")) {
      if (inherits(col, c("arrow_list", "vctrs_list_of"))) {
        col <- as.list(col)
      }
      child_sch <- sch$children[[1]]
      if (!is.null(ext) && is.null(child_sch$metadata[["ARROW:extension:name"]])) {
        child_sch$metadata[["ARROW:extension:name"]] <- ext
      }
      if (needs_postprocess_schema(child_sch, bigint)) {
        col[] <- lapply(col, walk_col, sch = child_sch)
      }
      return(col)
    }
    if (identical(fmt, "l")) {
      if (bigint == "integer" && is.integer(col)) {
        return(col)
      }
      if (bigint == "integer64" && inherits(col, "integer64")) {
        return(col)
      }
      return(as_bigint(col))
    }
    if (identical(ext, "google:sqlType:datetime") || identical(fmt, "tsu:")) {
      attr(col, "tzone") <- "UTC"
      return(col)
    }
    if (identical(ext, "google:sqlType:geography")) {
      bqs_check_namespace("wk", "GEOGRAPHY")
      attr(col, "class") <- c("wk_wkt", "wk_vctr")
      return(col)
    }
    if (identical(fmt, "z")) {
      bqs_check_namespace("blob", "BYTES")
      if (!inherits(col, "blob")) {
        attr(col, "class") <- c("blob", "vctrs_list_of", "vctrs_vctr", "list")
        attr(col, "ptype") <- raw(0)
      }
      return(col)
    }
    col
  }

  walk_col(df, schema)
}

#' @noRd
needs_postprocess_schema <- function(sch, bigint) {
  fmt <- sch$format
  ext <- sch$metadata[["ARROW:extension:name"]]
  if (identical(fmt, "l") || identical(fmt, "z") || identical(fmt, "tsu:")) {
    return(TRUE)
  }
  if (identical(ext, "google:sqlType:datetime") || identical(ext, "google:sqlType:geography")) {
    return(TRUE)
  }
  if (identical(fmt, "+s") || identical(fmt, "+l")) {
    return( TRUE )
  }
  FALSE
}

#' @noRd
parse_postprocess <- function(df, bigint, fields) {
  tests <- list()
  as_bigint <- switch(bigint,
    integer = as.integer,
    integer64 = bit64::as.integer64,
    numeric = as.numeric,
    character = as.character
  )
  tests[["bigint"]] <- list(
  	"test" = function(x,y) y[["type"]] %in% c("INT", "SMALLINT", "INTEGER", "BIGINT", "TINYINT", "BYTEINT", "INT64"),
  	"func" = function(x) as_bigint(x)
  )
	if (has_type(fields, "DATETIME")) {
		tests[["DATETIME"]] <- list(
			"test" = function(x,y) {y[["type"]] %in% "DATETIME"},
			"func" = function(x) {attr(x, "tzone") <- "UTC"; x}
		)
	}
	if (has_type(fields, "GEOGRAPHY")) {
	  bqs_check_namespace("wk", "GEOGRAPHY")
		tests[["GEOGRAPHY"]] <- list(
			"test" = function(x,y) y[["type"]] %in% "GEOGRAPHY",
			"func" = function(x) {attr(x, "class") <- c("wk_wkt", "wk_vctr");x}
		)
	}
	if (has_type(fields, "BYTES")) {
		bqs_check_namespace("blob", "BYTES")
		tests[["BYTES"]] <- list(
			"test" = function(x,y) y[["type"]] %in% "BYTES",
			"func" = function(x) {
				attr(x, "class") <- c("blob", "vctrs_list_of", "vctrs_vctr", "list")
				attr(x, "ptype") <- raw(0)
				x
			}
		)
	}
  if (length(tests)) {
	  df <- col_mapply(df, list("fields" = fields), tests)
  }
  df
}

#' @noRd
#' @importFrom rlang is_named
col_mapply <- function(x, y, tests) {
	if (is.list(x)) {
		if (inherits(x, c("arrow_list", "vctrs_list_of"))) {
			x <- as.list(x)
		}
		if (inherits(x, "data.frame") && !inherits(x, "tbl_df")) {
			x <- tibble::tibble(x)
		}
		if (rlang::is_named(x) && length(x) == length(y[["fields"]])) {
			x[] <- mapply(col_mapply, x, y[["fields"]], MoreArgs = list(tests = tests), SIMPLIFY = FALSE)
			return(x)
		} else if (y[["type"]] %in% "RECORD" && y[["mode"]] %in% "REPEATED") {
			x[] <- lapply(x, col_mapply, y = y, tests = tests)
			return(x)
		}
	}
	for (t in tests) {
		if (t[["test"]](x, y)) {
			if (y[["mode"]] %in% "REPEATED") {
				x <- lapply(x, t[["func"]])
			} else {
				x <- t[["func"]](x)
			}
			break
		}
	}
	x
}

#' @noRd
#' @importFrom rlang check_installed
bqs_check_namespace <- function(pkg, bqs_type) {
	rlang::check_installed(pkg, sprintf("to parse BigQueryStorage '%s' fields.", bqs_type))
}

#' @noRd
#' @importFrom utils head
select_fields <- function(fields, selected_fields) {
	if (length(selected_fields)) {
		selected_fields <- strsplit(selected_fields, ".", fixed = TRUE)
		nm <- vapply(fields, `[[`, character(1), "name")
		snm <- vapply(selected_fields, utils::head, character(1), 1)
		for (i in rev(seq_len(length(nm)))) {
			m <- match(tolower(nm[i]), tolower(snm))
			if (is.na(m)) {
				fields[[i]] <- NULL
			} else {
				if (length(f <- fields[[i]][["fields"]]) && length(sf <- selected_fields[[m]][-1])) {
					fields[[i]][["fields"]] <- select_fields(f,	paste0(sf, collapse = "."))
				}
			}
		}
	}
	return(fields)
}

#' @noRd
has_type <- function(fields, bqs_type) {
	f <- unlist(fields)
	w <- which(grepl("type$", names(f)))
	bqs_type %in% unique(f[w])
}
