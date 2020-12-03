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

		data <- bqs_table_download(res@bq_table,
															 tryCatch(res@billing, error = function(e) {getOption("bigquerystorage.project","")}),
															 max_results = n + res@cursor$cur(),
															 as_tibble = TRUE,
															 quiet = res@quiet,
															 bigint = res@bigint,
															 ...
		)

		if (res@cursor$cur() > 0L) {
			data <- data[res@cursor$cur():nrow(data), ]
		}

		res@cursor$adv(n)

		return(data)
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
											         as_tibble = TRUE,
											         quiet = conn@quiet,
											         bigint = conn@bigint,
											         ...
		)
	})
