#' DBI methods
#'
#' Implementations of pure virtual functions defined in the `DBI` package.
#' @name DBI
#' @keywords internal
NULL

#' @rdname DBI
#' @inheritParams DBI::dbFetch
#' @export
setMethod(
	"dbFetch", "BigQueryResult",
	function(res, n = -1, ...) {
		stopifnot(length(n) == 1, is.numeric(n))
		stopifnot(n == round(n), !is.na(n), n >= -1)

		if (n == -1 || n == Inf) {
			n <- res@cursor$left()
		}

		if (res@cursor$cur() > 0L) {
			stop("BigQueryReadClient implementation does not support reading from offset greater than 0.")
		}

		data <- bqs_table_download(res@bq_table,
															 tryCatch(res@billing, error = function(e) {getOption("bigquerystorage.project","")}),
															 max_results = n,
															 access_token = bigrquery:::.auth$cred$credentials$access_token,
															 ...
		)
		res@cursor$adv(n)

		bigrquery:::convert_bigint(as.data.frame(data), res@bigint)
	})

#' @rdname DBI
#' @inheritParams DBI::dbReadTable
#' @export
setMethod(
	"dbReadTable", c("BigQueryConnection", "character"),
	function(conn, name, ...) {
		tb <- as_bq_table(conn, name)
		data <- bqs_table_download(tb,
											 conn@billing,
											 access_token = bigrquery:::.auth$cred$credentials$access_token,
											 ...
		)
		as.data.frame(data)
	})
