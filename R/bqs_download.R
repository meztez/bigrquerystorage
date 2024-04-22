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
#' @param as_tibble Should data be returned as tibble. Default (FALSE) is to return
#' as arrow Table from raw IPC stream.
#' @param bigint The R type that BigQuery's 64-bit integer types should be mapped to.
#'   The default is `"integer"` which returns R's `integer` type but results in `NA` for
#'   values above/below +/- 2147483647. `"integer64"` returns a [bit64::integer64],
#'   which allows the full range of 64 bit integers.
#' @param max_results Deprecated
#' @details
#' More details about table modifiers and table options are available from the
#' API Reference documentation. (See [TableModifiers](https://cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#tablemodifiers) and
#' [TableReadOptions](https://cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#tablereadoptions))
#' @return This method returns a [arrow::Table] Table or optionally a tibble.
#' If you need a `data.frame`, leave parameter as_tibble to FALSE and coerce
#' the results with [as.data.frame()].
#' @export
#' @importFrom arrow RecordBatchStreamReader Table
#' @importFrom lifecycle deprecated deprecate_warn
#' @importFrom tibble tibble
#' @importFrom rlang is_missing
bqs_table_download <- function(
    x,
    parent = getOption("bigquerystorage.project", ""),
    snapshot_time = NA,
    selected_fields = character(),
    row_restriction = "",
    sample_percentage,
    n_max = Inf,
    quiet = NA,
    as_tibble = FALSE,
    bigint = c("integer", "integer64", "numeric", "character"),
    max_results = lifecycle::deprecated()) {
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
    quiet = quiet
  )

  rdr <- RecordBatchStreamReader$create(unlist(raws))
  # There is currently no way to create an Arrow Table from a
  # RecordBatchStreamReader when there is a schema but no batches.
  if (length(raws[[2]]) == 0L) {
    tb <- Table$create(
      stats::setNames(
        data.frame(matrix(ncol = rdr$schema$num_fields, nrow = 0)),
        rdr$schema$names
      )
    )
  } else {
    tb <- rdr$read_table()
  }

  if (isTRUE(as_tibble)) {
  	fields <- select_fields(bigrquery::bq_table_fields(x), selected_fields)
    tb <- parse_postprocess(
      tibble::tibble(
        as.data.frame(tb)
      ),
      bigint,
      fields
    )
  }

  # Batches do not support a n_max so we get just enough results before
  # exiting the streaming loop.
  if (isTRUE(trim_to_n) && nrow(tb) > n_max) {
    tb <- tb[1:n_max, ]
  }

  return(tb)
}

#' Initialize bigrquerystorage client
#' @export
#' @details
#' Will attempt to reuse `bigrquery` credentials.
#'
#' About Crendentials
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
  if (!is.null(.global$client) &&
    (as.numeric(Sys.time()) - .global$client$creation < 30)) {
    return(invisible())
  } else {
    bqs_deauth()
  }

  # Recycling bigrquery credentials
  if (bigrquery::bq_has_token()) {
    .authcred <- asNamespace("bigrquery")[[".auth"]][["cred"]]
    if (!is.null(refresh_token <- .authcred[["credentials"]][["refresh_token"]])) {
      .authclient <- .authcred[["client"]]
      access_token <- ""
      refresh_token <- c(
        type = "authorized_user",
        client_secret = .authclient[["secret"]],
        client_id = .authclient[["key"]],
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

#' Overload `bigrquery::bq_table_download`
#' @description
#' `r lifecycle::badge("experimental")`
#' Replace bigrquery bq_table_download method in bigrquery namespace.
#' @param parent Parent project used by the API for billing.
#' @importFrom rlang env_unlock
#' @importFrom lifecycle badge
#' @import bigrquery
#' @return No return value, called for side effects.
#' @export
overload_bq_table_download <- function(parent) {
  utils::assignInNamespace("bq_table_download", function(
      x, n_max = Inf, page_size = NULL, start_index = 0L, max_connections = 6L,
      quiet = NA, bigint = c("integer", "integer64", "numeric", "character"), max_results = deprecated()) {
    x <- bigrquery::as_bq_table(x)
    if (lifecycle::is_present(max_results)) {
      lifecycle::deprecate_warn(
        "1.4.0", "bq_table_download(max_results)",
        "bq_table_download(n_max)"
      )
      n_max <- max_results
    }
    assertthat::assert_that(is.numeric(n_max), length(n_max) == 1)
    assertthat::assert_that(is.numeric(start_index), length(start_index) == 1)
    bigint <- match.arg(bigint)
    table_data <- bigrquerystorage::bqs_table_download(
      x = x,
      parent = parent,
      n_max = n_max + start_index,
      as_tibble = TRUE,
      quiet = quiet,
      bigint = bigint
    )
    if (start_index > 0L) {
      table_data <- table_data[start_index:nrow(table_data), ]
    }
    return(table_data)
  }, ns = "bigrquery")
  if ("package:bigrquery" %in% search()) {
    env_unlock(environment(bq_table_download))
    namespaceExport(environment(bq_table_download), "bq_table_download")
    lockEnvironment(environment(bq_table_download), bindings = TRUE)
  }
}


# BigQuery storage --------------------------------------------------------
#' @noRd
bqs_initiate <- function() {
  bqs_init_logger()
  if (.Platform$OS.type == "windows") {
    if (Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH") == "") {
      warning("On Windows, GRPC_DEFAULT_SSL_ROOTS_FILE_PATH should be set to the PEM file path to load SSL roots from.")
    }
    # Issue with parallel arrow as.data.frame on Windows
    options("arrow.use_threads" = FALSE)
  }
}

# utils ------------------------------------------------------------------

#' @noRd
parse_postprocess <- function(df, bigint, fields) {
  tests <- list()
  if (bigint != "integer64") {
    as_bigint <- switch(bigint,
      integer = as.integer,
      numeric = as.numeric,
      character = as.character
    )
    tests[["bigint"]] <- list(
    	"test" = \(x,y) bit64::is.integer64(x),
    	"func" = \(x) as_bigint(x)
    )
  }
	if (has_type(fields, "DATETIME")) {
		tests[["DATETIME"]] <- list(
			"test" = \(x,y) {y[["type"]] %in% "DATETIME"},
			"func" = \(x) {attr(x, "tzone") <- "UTC"; x}
		)
	}
	if (has_type(fields, "GEOGRAPHY")) {
	  bqs_check_namespace("wk", "GEOGRAPHY")
		tests[["GEOGRAPHY"]] <- list(
			"test" = \(x,y) y[["type"]] %in% "GEOGRAPHY",
			"func" = \(x) {attr(x, "class") <- c("wk_wkt", "wk_vctr");x}
		)
	}
	if (has_type(fields, "BYTES")) {
		bqs_check_namespace("blob", "BYTES")
		tests[["BYTES"]] <- list(
			"test" = \(x,y) y[["type"]] %in% "BYTES",
			"func" = \(x) {
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
		if (inherits(x, "arrow_list")) {
			x <- as.list(x)
		}
		if (rlang::is_named(x)) {
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
select_fields <- function(fields, selected_fields) {
	if (length(selected_fields)) {
		selected_fields <- strsplit(selected_fields, ".", fixed = TRUE)
		nm <- vapply(fields, `[[`, character(1), "name")
		snm <- vapply(selected_fields, head, character(1), 1)
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

