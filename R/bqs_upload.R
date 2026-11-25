#' Upload table data
#'
#' This uploads rows to a BigQuery table using the BigQuery Storage Write API
#' (gRPC protocol with Arrow RecordBatch serialization).
#'
#' @param x Table reference (`{project}.{dataset}.{table_name}`, `{project}:{dataset}.{table_name}`,
#'   or a [bigrquery::bq_table]).
#' @param values Data to upload: a `data.frame`, `tibble`, `data.table`,
#'   `nanoarrow_array`, `nanoarrow_array_stream`, or any object coercible via
#'   [nanoarrow::as_nanoarrow_array_stream()].
#' @param fields Optional schema specification (`bq_fields` or list of `bq_field`s).
#'   If `NULL`, inferred from `values` or retrieved from the destination table when it already exists.
#' @param create_disposition Behavior if the destination table does not exist:
#'   `"CREATE_IF_NEEDED"` (default, creates the table) or `"CREATE_NEVER"` (errors if missing).
#' @param write_disposition Behavior if the destination table already has data:
#'   `"WRITE_EMPTY"` (default, errors if non-empty), `"WRITE_APPEND"` (appends rows),
#'   or `"WRITE_TRUNCATE"` (replaces existing rows).
#' @param stream_type Write stream mode: `"AUTO"` (default, uses high-speed
#'   parallel `"DEFAULT"` streams with automatic table cleanup on failure when
#'   creating a brand-new table, and atomic `"PENDING"` streams with
#'   `BatchCommitWriteStreams` when writing to an existing table), `"PENDING"`
#'   (always use atomic `CreateWriteStream` / `FinalizeWriteStream` /
#'   `BatchCommitWriteStreams`), or `"DEFAULT"` (always write to `_default`).
#' @param row_batch_size Maximum number of rows per `AppendRows` request when
#'   `values` is a `data.frame` (default `100000L`). Automatically scaled down for
#'   wide tables to stay well below the 20 MB gRPC message limit.
#' @param quiet Should progress information be suppressed? Default `NA` (shows progress
#'   in interactive sessions).
#' @param max_stream_count Maximum number of parallel write streams to open
#'   (`0L` automatically scales concurrency based on payload size and
#'   `options(bigrquerystorage.threads)`).
#' @param compression Arrow record batch buffer compression codec: `"auto"`
#'   (default, uses `"zstd"` when compiled with `libzstd`), `"zstd"`, or `"none"`.
#' @param ... Additional arguments passed to [bigrquery::bq_table_create()] when
#'   creating a table.
#' @details
#' More details about the BigQuery Storage Write API are available in the
#' [API Reference documentation](https://docs.cloud.google.com/bigquery/docs/write-api).
#'
#' Upstream `bigrquery` can delegate `bq_table_upload(..., api = "arrow")` directly to
#' `bqs_table_upload()`.
#'
#' @return Invisibly returns `x`.
#' @export
#' @import nanoarrow
bqs_table_upload <- function(
    x,
    values,
    fields = NULL,
    create_disposition = c("CREATE_IF_NEEDED", "CREATE_NEVER"),
    write_disposition = c("WRITE_EMPTY", "WRITE_APPEND", "WRITE_TRUNCATE"),
    stream_type = c("AUTO", "PENDING", "DEFAULT"),
    row_batch_size = 100000L,
    quiet = getOption("bigrquery.quiet", NA),
    max_stream_count = 0L,
    compression = c("auto", "zstd", "none"),
    ...) {
  ref <- bqs_parse_table_ref(x)
  project <- ref[[1]]
  dataset <- ref[[2]]
  table <- ref[[3]]

  create_disposition <- match.arg(create_disposition)
  write_disposition <- match.arg(write_disposition)
  stream_type <- match.arg(stream_type)
  compression <- match.arg(compression)
  assertthat::assert_that(is.numeric(row_batch_size), length(row_batch_size) == 1, row_batch_size >= 1)
  row_batch_size <- as.integer(row_batch_size)
  max_stream_count <- as.integer(max_stream_count)
  threads <- as.integer(getOption("bigrquerystorage.threads", 0L))

  use_zstd <- switch(compression,
    auto = bqs_has_zstd(),
    zstd = {
      if (!bqs_has_zstd()) {
        stop("bigrquerystorage was compiled without libzstd support; use compression = 'none'.", call. = FALSE)
      }
      TRUE
    },
    none = FALSE
  )

  quiet <- if (is.na(quiet)) !interactive() else isTRUE(quiet)
  if (!is.data.frame(values)) {
    values <- nanoarrow::as_nanoarrow_array_stream(values)
  }

  bqs_auth()

  if (!is.data.frame(values) || nrow(values) >= 4096L) {
    bqs_warm_write_channels(.global$client$ptr, if (max_stream_count > 0L) max_stream_count else threads)
  }

  bq_tbl <- bigrquery::bq_table(project, dataset, table)
  prep <- bqs_prepare_upload_table(
    bq_tbl = bq_tbl,
    project = project,
    dataset = dataset,
    table = table,
    values = values,
    fields = bqs_normalize_fields(fields),
    create_disposition = create_disposition,
    write_disposition = write_disposition,
    ...
  )
  target_fields <- prep$target_fields
  effective_stream_type <- if (stream_type == "AUTO") {
    if (prep$created_brand_new) "DEFAULT" else "PENDING"
  } else {
    stream_type
  }

  upload_ok <- FALSE
  if (prep$created_brand_new && effective_stream_type == "DEFAULT") {
    on.exit(
      if (!upload_ok) try(bigrquery::bq_table_delete(bq_tbl), silent = TRUE),
      add = TRUE
    )
  }

  fast_stream <- NULL
  if (is.data.frame(values)) {
    if (nrow(values) == 0L || ncol(values) == 0L) {
      upload_ok <- TRUE
      return(invisible(x))
    }
    arr <- bqs_df_to_arrow_array(values, target_fields)
    sch <- nanoarrow::infer_nanoarrow_schema(arr)
    fast_stream <- nanoarrow::basic_array_stream(list(arr), schema = sch, validate = FALSE)
  } else {
    sch <- values$get_schema()
    if (bqs_can_fast_encode(sch)) {
      fast_stream <- values
    }
  }

  if (!is.null(fast_stream)) {
    bqs_arrow_write(
      client = .global$client$ptr,
      project = project,
      dataset = dataset,
      table = table,
      stream_xptr = fast_stream,
      schema_ipc = bqs_schema_to_ipc(sch),
      row_batch_size = row_batch_size,
      stream_type = effective_stream_type,
      use_zstd = use_zstd,
      quiet = quiet,
      max_stream_count = max_stream_count,
      max_threads = threads
    )
    upload_ok <- TRUE
    return(invisible(x))
  }

  ipc_bytes <- bqs_serialize_to_ipc(values, target_fields, row_batch_size, use_zstd = use_zstd)
  if (length(ipc_bytes)) {
    bqs_ipc_write(
      client = .global$client$ptr,
      project = project,
      dataset = dataset,
      table = table,
      ipc_bytes = ipc_bytes,
      stream_type = effective_stream_type,
      quiet = quiet,
      max_stream_count = max_stream_count,
      max_threads = threads
    )
  }
  upload_ok <- TRUE
  invisible(x)
}

# Destination DDL helpers -------------------------------------------------

#' @noRd
bqs_prepare_upload_table <- function(
    bq_tbl,
    project,
    dataset,
    table,
    values,
    fields,
    create_disposition,
    write_disposition,
    ...) {
  tbl_id <- sprintf("%s.%s.%s", project, dataset, table)
  if (create_disposition == "CREATE_IF_NEEDED" && write_disposition != "WRITE_TRUNCATE") {
    create_fields <- fields %||% bqs_infer_bq_fields(values)
    created_now <- tryCatch({
      bqs_create_table(bq_tbl, create_fields, ...)
      TRUE
    }, error = function(e) {
      msg <- conditionMessage(e)
      if (identical(e$status, 409L) || grepl("Already Exists", msg, ignore.case = TRUE)) {
        FALSE
      } else if ((identical(e$status, 403L) || grepl("Access Denied|Forbidden|permission", msg, ignore.case = TRUE)) &&
                 isTRUE(tryCatch(bigrquery::bq_table_exists(bq_tbl), error = function(...) FALSE))) {
        FALSE
      } else {
        stop(e)
      }
    })
    if (created_now) {
      return(list(target_fields = create_fields, created_brand_new = TRUE))
    }
    if (write_disposition == "WRITE_EMPTY" && !bqs_table_is_empty(bq_tbl, project)) {
      stop(sprintf("Table '%s' is not empty and write_disposition is 'WRITE_EMPTY'.", tbl_id), call. = FALSE)
    }
    return(list(target_fields = fields %||% bqs_get_table_fields(bq_tbl), created_brand_new = FALSE))
  }

  if (write_disposition == "WRITE_TRUNCATE") {
    meta <- tryCatch(
      bigrquery::bq_table_meta(bq_tbl),
      error = function(e) {
        if (identical(e$status, 404L) || grepl("Not found", conditionMessage(e), ignore.case = TRUE)) NULL else stop(e)
      }
    )
    if (is.null(meta)) {
      if (create_disposition == "CREATE_NEVER") {
        stop(sprintf("Table '%s' does not exist and create_disposition is 'CREATE_NEVER'.", tbl_id), call. = FALSE)
      }
      create_fields <- fields %||% bqs_infer_bq_fields(values)
      bqs_create_table(bq_tbl, create_fields, ...)
      return(list(target_fields = create_fields, created_brand_new = TRUE))
    }
    create_fields <- fields %||% (if (length(meta$schema$fields)) bqs_normalize_fields(meta$schema$fields) else bqs_infer_bq_fields(values))
    bqs_with_ddl_retry(bigrquery::bq_table_delete(bq_tbl))
    extra <- list(...)
    extra$description <- extra$description %||% meta$description
    extra$labels <- extra$labels %||% meta$labels
    do.call(bqs_create_table, c(list(x = bq_tbl, fields = create_fields), extra))
    return(list(target_fields = create_fields, created_brand_new = FALSE))
  }

  if (!bigrquery::bq_table_exists(bq_tbl)) {
    stop(sprintf("Table '%s' does not exist and create_disposition is 'CREATE_NEVER'.", tbl_id), call. = FALSE)
  }
  if (write_disposition == "WRITE_EMPTY" && !bqs_table_is_empty(bq_tbl, project)) {
    stop(sprintf("Table '%s' is not empty and write_disposition is 'WRITE_EMPTY'.", tbl_id), call. = FALSE)
  }
  list(target_fields = fields %||% bqs_get_table_fields(bq_tbl), created_brand_new = FALSE)
}

#' @noRd
bqs_table_is_empty <- function(bq_tbl, parent) {
  if (bigrquery::bq_table_nrow(bq_tbl) > 0) {
    return(FALSE)
  }
  stream <- bqs_table_download(bq_tbl, parent = parent, n_max = 1L, type = "arrow", quiet = TRUE)
  batch <- stream$get_next()
  is.null(batch) || batch$length == 0L
}

#' @noRd
bqs_with_ddl_retry <- function(expr, max_attempts = 5L) {
  expr_q <- substitute(expr)
  env <- parent.frame()
  for (attempt in seq_len(max_attempts)) {
    res <- tryCatch(
      list(ok = TRUE, val = eval(expr_q, envir = env)),
      error = function(e) list(ok = FALSE, err = e)
    )
    if (res$ok) return(res$val)
    is_rate_limit <- grepl("rateLimitExceeded|too many table update", conditionMessage(res$err), ignore.case = TRUE)
    if (!is_rate_limit || attempt == max_attempts) stop(res$err)
    Sys.sleep(min(8, 0.5 * (2^(attempt - 1L))))
  }
}

#' @noRd
bqs_create_table <- function(x, fields, ...) {
  extra <- list(...)
  extra$billing <- NULL
  args <- if (length(fields)) {
    c(list(x = x, fields = bqs_fields_to_bq_fields(fields)), extra)
  } else {
    c(list(x = x), extra)
  }
  bqs_with_ddl_retry(do.call(bigrquery::bq_table_create, args))
}

# Arrow IPC Serialization -------------------------------------------------

#' @noRd
bqs_schema_to_ipc <- function(schema) {
  empty_stream <- nanoarrow::basic_array_stream(list(), schema = schema, validate = FALSE)
  con <- rawConnection(raw(0), "wb")
  on.exit(close(con))
  nanoarrow::write_nanoarrow(empty_stream, con)
  rawConnectionValue(con)
}

#' @noRd
bqs_serialize_to_ipc <- function(values, target_fields, row_batch_size, use_zstd = FALSE) {
  if (inherits(values, c("nanoarrow_array_stream", "nanoarrow_array"))) {
    stream <- nanoarrow::as_nanoarrow_array_stream(values)
    stream_schema <- stream$get_schema()
    first_batch <- stream$get_next()
    if (is.null(first_batch)) return(raw(0))
    first_batches <- list(first_batch)
    repeat {
      b <- stream$get_next()
      if (is.null(b)) break
      first_batches[[length(first_batches) + 1L]] <- b
    }
    if (sum(vapply(first_batches, function(b) b$length, integer(1))) == 0L) {
      return(raw(0))
    }
    replay <- nanoarrow::basic_array_stream(first_batches, schema = stream_schema, validate = FALSE)
    if (bqs_schema_has_dictionary(stream_schema)) {
      df_conv <- parse_postprocess_schema(
        bqs_convert_stream(replay, int64_ptype(stream_schema), "integer64"),
        stream_schema,
        "integer64"
      )
      return(bqs_serialize_to_ipc(df_conv, target_fields, row_batch_size, use_zstd = use_zstd))
    }
    con <- rawConnection(raw(0), "wb")
    on.exit(close(con))
    nanoarrow::write_nanoarrow(replay, con)
    return(rawConnectionValue(con))
  }

  if (!is.data.frame(values)) {
    return(bqs_serialize_to_ipc(
      nanoarrow::as_nanoarrow_array_stream(values),
      target_fields,
      row_batch_size,
      use_zstd = use_zstd
    ))
  }

  n <- nrow(values)
  if (n == 0L || ncol(values) == 0L) {
    return(raw(0))
  }

  est_bytes_per_row <- max(64, ncol(values) * 16, as.numeric(utils::object.size(values)) / n)
  batch_rows <- min(row_batch_size, max(1L, as.integer(8 * 1024 * 1024 / est_bytes_per_row)))

  arr <- bqs_df_to_arrow_array(values, target_fields)
  sch <- nanoarrow::infer_nanoarrow_schema(arr)
  bqs_encode_arrow_ipc_raw(
    schema_xptr = sch,
    array_xptr = arr,
    schema_ipc = bqs_schema_to_ipc(sch),
    row_batch_size = batch_rows,
    use_zstd = isTRUE(use_zstd),
    n_threads = as.integer(getOption("bigrquerystorage.threads", 0L))
  )
}

#' @noRd
bqs_df_to_arrow_array <- function(df, target_fields = NULL) {
  n <- nrow(df)
  col_names <- names(df)
  p <- length(col_names)
  field_map <- if (length(target_fields)) {
    f_names <- tolower(vapply(target_fields, `[[`, character(1), "name"))
    names(target_fields) <- f_names
    target_fields
  } else {
    list()
  }

  target_types <- character(p)
  target_modes <- character(p)
  for (i in seq_len(p)) {
    f <- field_map[[tolower(col_names[[i]])]]
    if (!is.null(f)) {
      target_types[[i]] <- bqs_normalize_type(f$type %||% "")
      target_modes[[i]] <- toupper(f$mode %||% "NULLABLE")
    } else {
      target_types[[i]] <- ""
      target_modes[[i]] <- "NULLABLE"
    }
  }

  threads <- as.integer(getOption("bigrquerystorage.threads", 0L))
  fast <- bqs_fast_df_columns(df, col_names, target_types, target_modes, n, threads)
  children <- fast$arrays
  child_schemas <- fast$schemas
  names(child_schemas) <- col_names

  for (i in seq_len(p)) {
    if (is.null(children[[i]])) {
      nm <- col_names[[i]]
      col <- df[[i]]
      f <- field_map[[tolower(nm)]]
      arr <- bqs_col_to_arrow_array(col, nm, f)
      sch <- nanoarrow::infer_nanoarrow_schema(arr)
      sch <- nanoarrow::nanoarrow_schema_modify(sch, list(name = nm))
      nanoarrow::nanoarrow_array_set_schema(arr, sch)
      children[[i]] <- arr
      child_schemas[[i]] <- sch
    }
  }

  root_schema <- nanoarrow::na_struct(child_schemas)
  root_arr <- nanoarrow::nanoarrow_array_modify(
    nanoarrow::nanoarrow_array_init(root_schema),
    list(
      length = n,
      null_count = 0L,
      offset = 0L,
      buffers = list(NULL),
      children = children
    )
  )
  nanoarrow::nanoarrow_array_set_schema(root_arr, root_schema)
  root_arr
}

#' @noRd
bqs_normalize_type <- function(type) {
  tp <- toupper(type %||% "")
  switch(tp,
    INTEGER = , INT = , SMALLINT = , TINYINT = , BYTEINT = , BIGINT = "INT64",
    FLOAT = , DOUBLE = "FLOAT64",
    BOOLEAN = "BOOL",
    STRUCT = "RECORD",
    tp
  )
}

#' @noRd
bqs_stop_incompatible_type <- function(name, col, target_type, detail = NULL) {
  msg <- sprintf(
    "Column '%s' of type '%s' cannot be converted to BigQuery %s",
    name,
    paste(class(col), collapse = "/"),
    target_type
  )
  if (!is.null(detail) && nzchar(detail)) {
    msg <- paste0(msg, ": ", detail)
  }
  stop(paste0(msg, "."), call. = FALSE)
}

#' @noRd
bqs_is_all_na_logical <- function(col) {
  is.logical(col) && !is.object(col) && all(is.na(col))
}

#' @noRd
bqs_double_to_integer64_strict <- function(col, name) {
  if (any(is.nan(col))) {
    bqs_stop_incompatible_type(name, col, "INT64", "NaN values cannot be converted to INT64")
  }
  non_na <- !is.na(col)
  if (any(non_na)) {
    vals <- col[non_na]
    if (!all(is.finite(vals)) || !all(trunc(vals) == vals) || any(abs(vals) >= 2^63)) {
      bqs_stop_incompatible_type(
        name, col, "INT64",
        "contains non-integer, non-finite, or out-of-range double values"
      )
    }
  }
  bit64::as.integer64(col)
}

#' @noRd
bqs_parse_date_strict <- function(col, name) {
  non_na <- !is.na(col)
  if (!any(non_na)) {
    return(as.Date(rep(NA_real_, length(col)), origin = "1970-01-01"))
  }
  parsed <- tryCatch(
    as.Date(col),
    error = function(e) bqs_stop_incompatible_type(name, col, "DATE", conditionMessage(e))
  )
  if (any(non_na & is.na(parsed))) {
    bad <- col[which(non_na & is.na(parsed))[1L]]
    bqs_stop_incompatible_type(name, col, "DATE", sprintf("cannot parse '%s'", bad))
  }
  parsed
}

#' @noRd
bqs_parse_timestamp_strict <- function(col, name, target_type) {
  non_na <- !is.na(col)
  if (!any(non_na)) {
    return(as.POSIXct(rep(NA_real_, length(col)), origin = "1970-01-01", tz = "UTC"))
  }
  parsed <- tryCatch(
    as.POSIXct(col, tz = "UTC"),
    error = function(e) bqs_stop_incompatible_type(name, col, target_type, conditionMessage(e))
  )
  if (any(non_na & is.na(parsed))) {
    bad <- col[which(non_na & is.na(parsed))[1L]]
    bqs_stop_incompatible_type(name, col, target_type, sprintf("cannot parse '%s'", bad))
  }
  parsed
}

#' @noRd
bqs_parse_time_strict <- function(col, name) {
  non_na <- !is.na(col)
  if (!any(non_na)) {
    return(structure(rep(NA_real_, length(col)), units = "secs", class = c("hms", "difftime")))
  }
  bqs_check_namespace("hms", "TIME")
  parsed <- tryCatch(
    suppressWarnings(hms::as_hms(col)),
    error = function(e) bqs_stop_incompatible_type(name, col, "TIME", conditionMessage(e))
  )
  if (any(non_na & is.na(parsed))) {
    bad <- col[which(non_na & is.na(parsed))[1L]]
    bqs_stop_incompatible_type(name, col, "TIME", sprintf("cannot parse '%s'", bad))
  }
  parsed
}

#' @noRd
bqs_col_to_arrow_array <- function(col, name, field = NULL) {
  mode <- if (!is.null(field)) toupper(field$mode %||% "NULLABLE") else "NULLABLE"
  type <- if (!is.null(field)) bqs_normalize_type(field$type %||% "") else ""

  if (mode == "REPEATED") {
    if (!(is.list(col) && !is.data.frame(col) && !inherits(col, "blob") && !inherits(col, "POSIXlt"))) {
      bqs_stop_incompatible_type(name, col, paste("REPEATED", if (nzchar(type)) type else "ARRAY"))
    }
    return(bqs_repeated_to_arrow_array(col, name, field))
  }

  if (nzchar(type)) {
    if (type == "RECORD") {
      subfields <- if (!is.null(field)) field$fields else NULL
      if (is.data.frame(col)) {
        return(bqs_df_to_arrow_array(col, subfields))
      }
      if (is.list(col) && !inherits(col, "blob") && !inherits(col, "POSIXlt")) {
        non_null <- Filter(Negate(is.null), col)
        if (length(non_null) == 0L || all(vapply(non_null, function(el) {
          is.list(el) && !is.data.frame(el) && (length(el) == 0L || rlang::is_named(el))
        }, logical(1)))) {
          col <- bqs_list_of_named_lists_to_df(col, subfields)
          return(bqs_df_to_arrow_array(col, subfields))
        }
      }
      bqs_stop_incompatible_type(name, col, "RECORD")
    }

    if (type == "INT64") {
      if (bit64::is.integer64(col) || (is.integer(col) && !is.object(col))) {
        return(nanoarrow::as_nanoarrow_array(col))
      }
      if (is.double(col) && !is.object(col)) {
        return(nanoarrow::as_nanoarrow_array(bqs_double_to_integer64_strict(col, name)))
      }
      if (bqs_is_all_na_logical(col)) {
        return(nanoarrow::as_nanoarrow_array(rep(bit64::NA_integer64_, length(col))))
      }
      bqs_stop_incompatible_type(name, col, "INT64")
    }

    if (type == "FLOAT64") {
      if (is.double(col) && !is.object(col)) {
        return(nanoarrow::as_nanoarrow_array(col))
      }
      if ((is.integer(col) && !is.object(col)) || bit64::is.integer64(col)) {
        return(nanoarrow::as_nanoarrow_array(as.double(col)))
      }
      if (bqs_is_all_na_logical(col)) {
        return(nanoarrow::as_nanoarrow_array(rep(NA_real_, length(col))))
      }
      bqs_stop_incompatible_type(name, col, "FLOAT64")
    }

    if (type == "BOOL") {
      if (is.logical(col) && !is.object(col)) {
        return(nanoarrow::as_nanoarrow_array(col))
      }
      bqs_stop_incompatible_type(name, col, "BOOL")
    }

    if (type == "STRING") {
      if ((is.character(col) && (!is.object(col) || inherits(col, "wk_wkt"))) || is.factor(col)) {
        return(nanoarrow::as_nanoarrow_array(as.character(col), schema = nanoarrow::na_string()))
      }
      if (bqs_is_all_na_logical(col)) {
        return(nanoarrow::as_nanoarrow_array(rep(NA_character_, length(col)), schema = nanoarrow::na_string()))
      }
      if (bit64::is.integer64(col) || ((is.integer(col) || is.double(col) || is.logical(col)) && !is.object(col))) {
        return(nanoarrow::as_nanoarrow_array(as.character(col), schema = nanoarrow::na_string()))
      }
      bqs_stop_incompatible_type(name, col, "STRING")
    }

    if (type == "JSON") {
      if (is.character(col) && !is.object(col)) {
        return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_string()))
      }
      if (bqs_is_all_na_logical(col)) {
        return(nanoarrow::as_nanoarrow_array(rep(NA_character_, length(col)), schema = nanoarrow::na_string()))
      }
      bqs_stop_incompatible_type(name, col, "JSON")
    }

    if (type == "GEOGRAPHY") {
      if (inherits(col, "wk_wkt") || (is.character(col) && !is.object(col))) {
        return(nanoarrow::as_nanoarrow_array(as.character(col), schema = nanoarrow::na_string()))
      }
      if (bqs_is_all_na_logical(col)) {
        return(nanoarrow::as_nanoarrow_array(rep(NA_character_, length(col)), schema = nanoarrow::na_string()))
      }
      bqs_stop_incompatible_type(name, col, "GEOGRAPHY")
    }

    if (type == "BYTES") {
      if (inherits(col, "blob")) {
        return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_binary()))
      }
      if (bqs_is_all_na_logical(col)) {
        empty_blob <- structure(
          vector("list", length(col)),
          ptype = raw(0),
          class = c("blob", "vctrs_list_of", "vctrs_vctr", "list")
        )
        return(nanoarrow::as_nanoarrow_array(empty_blob, schema = nanoarrow::na_binary()))
      }
      if (is.list(col) && !is.data.frame(col) && !inherits(col, "POSIXlt") &&
          all(vapply(col, function(el) is.null(el) || is.raw(el), logical(1)))) {
        raw_blob <- structure(
          unclass(col),
          ptype = raw(0),
          class = c("blob", "vctrs_list_of", "vctrs_vctr", "list")
        )
        return(nanoarrow::as_nanoarrow_array(raw_blob, schema = nanoarrow::na_binary()))
      }
      bqs_stop_incompatible_type(name, col, "BYTES")
    }

    if (type == "DATE") {
      if (inherits(col, "Date")) {
        return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_date32()))
      }
      if (is.character(col) && !is.object(col)) {
        return(nanoarrow::as_nanoarrow_array(bqs_parse_date_strict(col, name), schema = nanoarrow::na_date32()))
      }
      if (bqs_is_all_na_logical(col)) {
        return(nanoarrow::as_nanoarrow_array(
          as.Date(rep(NA_real_, length(col)), origin = "1970-01-01"),
          schema = nanoarrow::na_date32()
        ))
      }
      bqs_stop_incompatible_type(name, col, "DATE")
    }

    if (type == "TIME") {
      if (inherits(col, c("hms", "difftime"))) {
        return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_time64("us")))
      }
      if (is.character(col) && !is.object(col)) {
        return(nanoarrow::as_nanoarrow_array(bqs_parse_time_strict(col, name), schema = nanoarrow::na_time64("us")))
      }
      if (bqs_is_all_na_logical(col)) {
        return(nanoarrow::as_nanoarrow_array(
          structure(rep(NA_real_, length(col)), units = "secs", class = c("hms", "difftime")),
          schema = nanoarrow::na_time64("us")
        ))
      }
      bqs_stop_incompatible_type(name, col, "TIME")
    }

    if (type %in% c("DATETIME", "TIMESTAMP")) {
      tz <- if (type == "DATETIME") "" else "UTC"
      if (inherits(col, "POSIXt")) {
        return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_timestamp("us", timezone = tz)))
      }
      if (is.character(col) && !is.object(col)) {
        return(nanoarrow::as_nanoarrow_array(
          bqs_parse_timestamp_strict(col, name, type),
          schema = nanoarrow::na_timestamp("us", timezone = tz)
        ))
      }
      if (bqs_is_all_na_logical(col)) {
        return(nanoarrow::as_nanoarrow_array(
          as.POSIXct(rep(NA_real_, length(col)), origin = "1970-01-01", tz = "UTC"),
          schema = nanoarrow::na_timestamp("us", timezone = tz)
        ))
      }
      bqs_stop_incompatible_type(name, col, type)
    }

    if (type %in% c("NUMERIC", "BIGNUMERIC")) {
      if (bit64::is.integer64(col) ||
          ((is.integer(col) || is.double(col) || is.character(col)) && !is.object(col)) ||
          bqs_is_all_na_logical(col)) {
        prec <- if (type == "NUMERIC") 38L else 76L
        scale <- if (type == "BIGNUMERIC" && is.character(col)) 38L else 9L
        bitwidth <- if (type == "NUMERIC") 128L else 256L
        return(bqs_decimal_to_arrow_array(col, precision = prec, scale = scale, bitwidth = bitwidth, name = name))
      }
      bqs_stop_incompatible_type(name, col, type)
    }

    return(nanoarrow::as_nanoarrow_array(col))
  }

  if (is.data.frame(col)) {
    return(bqs_df_to_arrow_array(col, NULL))
  }

  if (is.list(col) && !inherits(col, "blob") && !inherits(col, "POSIXlt")) {
    non_null <- Filter(Negate(is.null), col)
    if (length(non_null) > 0 && is.list(non_null[[1]]) && !is.data.frame(non_null[[1]]) && rlang::is_named(non_null[[1]])) {
      col <- bqs_list_of_named_lists_to_df(col, NULL)
      return(bqs_df_to_arrow_array(col, NULL))
    }
    return(bqs_repeated_to_arrow_array(col, name, field))
  }

  if (inherits(col, "wk_wkt")) {
    return(nanoarrow::as_nanoarrow_array(as.character(col), schema = nanoarrow::na_string()))
  }

  if (is.factor(col)) {
    return(nanoarrow::as_nanoarrow_array(as.character(col), schema = nanoarrow::na_string()))
  }

  if (inherits(col, c("hms", "difftime"))) {
    return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_time64("us")))
  }

  if (inherits(col, "POSIXt")) {
    return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_timestamp("us", timezone = "UTC")))
  }

  if (inherits(col, "Date")) {
    return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_date32()))
  }

  nanoarrow::as_nanoarrow_array(col)
}

#' @noRd
bqs_decimal_to_arrow_array <- function(col, precision, scale, bitwidth, name = NULL) {
  enc <- tryCatch(
    bqs_encode_decimal(col, scale = scale, bitwidth = bitwidth),
    error = function(e) {
      if (!is.null(name) && nzchar(name)) {
        bq_type <- if (bitwidth == 128L) "NUMERIC" else "BIGNUMERIC"
        bqs_stop_incompatible_type(name, col, bq_type, conditionMessage(e))
      }
      stop(e)
    }
  )
  sch <- if (bitwidth == 128L) {
    nanoarrow::na_decimal128(precision, scale)
  } else {
    nanoarrow::na_decimal256(precision, scale)
  }
  valid_buf <- if (is.null(enc$validity)) NULL else nanoarrow::as_nanoarrow_buffer(enc$validity)
  data_buf <- nanoarrow::as_nanoarrow_buffer(enc$data)
  arr <- nanoarrow::nanoarrow_array_modify(
    nanoarrow::nanoarrow_array_init(sch),
    list(
      length = enc$length,
      null_count = enc$null_count,
      offset = 0L,
      buffers = list(valid_buf, data_buf)
    )
  )
  nanoarrow::nanoarrow_array_set_schema(arr, sch)
  arr
}

#' @noRd
bqs_repeated_to_arrow_array <- function(col, name, field = NULL) {
  n <- length(col)
  elem_field <- if (!is.null(field)) {
    ef <- field
    ef$mode <- "NULLABLE"
    ef$type <- bqs_normalize_type(ef$type %||% "")
    ef
  } else {
    NULL
  }

  non_null <- Filter(Negate(is.null), col)
  is_record <- (!is.null(elem_field) && elem_field$type == "RECORD") ||
    (length(non_null) > 0 && is.data.frame(non_null[[1]]))

  if (is_record) {
    if (length(non_null) > 0 && !all(vapply(non_null, is.data.frame, logical(1)))) {
      bqs_stop_incompatible_type(name, col, "REPEATED RECORD", "all non-NULL elements must be data.frames")
    }
    lens <- vapply(col, function(el) if (is.null(el)) 0L else nrow(el), integer(1))
    offsets <- c(0L, cumsum(lens))
    non_empty <- Filter(function(el) !is.null(el) && nrow(el) > 0L, col)
    if (length(non_empty) > 0) {
      combined <- bqs_rbind_dfs(non_empty)
    } else if (length(non_null) > 0) {
      combined <- non_null[[1]]
    } else if (!is.null(elem_field) && length(elem_field$fields)) {
      empty_cols <- lapply(elem_field$fields, bqs_empty_col_for_field)
      names(empty_cols) <- vapply(elem_field$fields, `[[`, character(1), "name")
      combined <- tibble::as_tibble(empty_cols)
    } else {
      combined <- data.frame()
    }
    item_arr <- bqs_df_to_arrow_array(combined, if (!is.null(elem_field)) elem_field$fields else NULL)
  } else {
    elem_type <- if (!is.null(elem_field)) elem_field$type else ""
    lens <- lengths(col)
    offsets <- c(0L, cumsum(lens))
    if (length(non_null) > 0) {
      combined <- bqs_concat_vectors(non_null, name = name, target_type = elem_type)
    } else {
      combined <- bqs_empty_col_for_field(elem_field)
    }
    item_arr <- bqs_col_to_arrow_array(combined, name, elem_field)
  }

  item_sch <- nanoarrow::infer_nanoarrow_schema(item_arr)
  item_sch <- nanoarrow::nanoarrow_schema_modify(item_sch, list(name = "item"))
  nanoarrow::nanoarrow_array_set_schema(item_arr, item_sch)

  list_sch <- nanoarrow::na_list(item_sch)
  list_arr <- nanoarrow::nanoarrow_array_modify(
    nanoarrow::nanoarrow_array_init(list_sch),
    list(
      length = n,
      null_count = 0L,
      offset = 0L,
      buffers = list(NULL, nanoarrow::as_nanoarrow_buffer(as.integer(offsets))),
      children = list(item_arr)
    )
  )
  nanoarrow::nanoarrow_array_set_schema(list_arr, list_sch)
  list_arr
}

#' @noRd
bqs_list_of_named_lists_to_df <- function(lst, subfields = NULL) {
  n <- length(lst)
  non_null <- Filter(Negate(is.null), lst)
  col_names <- if (length(subfields)) {
    vapply(subfields, `[[`, character(1), "name")
  } else if (length(non_null)) {
    names(non_null[[1]])
  } else {
    character(0)
  }
  sf_map <- if (length(subfields)) {
    m <- subfields
    names(m) <- tolower(col_names)
    m
  } else {
    list()
  }
  cols <- lapply(col_names, function(nm) {
    pieces <- lapply(lst, function(el) if (is.null(el) || is.null(el[[nm]])) NA else el[[nm]])
    sf <- sf_map[[tolower(nm)]]
    sf_mode <- if (!is.null(sf)) toupper(sf$mode %||% "NULLABLE") else "NULLABLE"
    sf_type <- if (!is.null(sf)) bqs_normalize_type(sf$type %||% "") else ""
    tt <- if (sf_mode != "REPEATED" && sf_type != "RECORD") sf_type else ""
    bqs_concat_vectors(pieces, name = nm, target_type = tt)
  })
  names(cols) <- col_names
  tibble::new_tibble(cols, nrow = n)
}

#' @noRd
bqs_rbind_dfs <- function(dfs) {
  if (length(dfs) == 1L) {
    return(dfs[[1]])
  }
  col_names <- names(dfs[[1]])
  total_rows <- sum(vapply(dfs, nrow, integer(1)))
  cols <- lapply(col_names, function(nm) {
    pieces <- lapply(dfs, `[[`, nm)
    if (is.data.frame(pieces[[1]])) {
      bqs_rbind_dfs(pieces)
    } else if (is.list(pieces[[1]]) && !inherits(pieces[[1]], "blob") && !inherits(pieces[[1]], "POSIXlt")) {
      unlist(pieces, recursive = FALSE, use.names = FALSE)
    } else {
      bqs_concat_vectors(pieces)
    }
  })
  names(cols) <- col_names
  tibble::new_tibble(cols, nrow = total_rows)
}

#' @noRd
bqs_is_raw_list <- function(x) {
  is.list(x) && !is.data.frame(x) && !inherits(x, "POSIXlt") &&
    all(vapply(x, function(el) is.null(el) || is.raw(el), logical(1)))
}

#' @noRd
bqs_double_to_decimal_chr <- function(x, name, target_type) {
  out <- rep(NA_character_, length(x))
  non_na <- !is.na(x)
  if (any(non_na)) {
    vals <- x[non_na]
    if (!all(is.finite(vals))) {
      bqs_stop_incompatible_type(name, x, target_type, "Cannot convert non-finite value")
    }
    out[non_na] <- sprintf("%.9f", vals)
  }
  out
}

#' @noRd
bqs_concat_vectors <- function(lst, name = "item", target_type = "") {
  if (length(lst) == 0L) {
    return(bqs_empty_col_for_field(if (nzchar(target_type)) list(type = target_type, mode = "NULLABLE") else NULL))
  }
  if (nzchar(target_type)) {
    if (target_type == "INT64") {
      if (all(vapply(lst, function(x) is.integer(x) && !is.object(x), logical(1)))) {
        return(unlist(lst, recursive = FALSE, use.names = FALSE))
      }
      if (all(vapply(lst, function(x) is.double(x) && !is.object(x), logical(1)))) {
        return(unlist(lst, recursive = FALSE, use.names = FALSE))
      }
      raw_dbl <- unlist(lapply(lst, function(x) {
        if (bit64::is.integer64(x)) {
          unclass(x)
        } else if (is.integer(x) && !is.object(x)) {
          unclass(bit64::as.integer64(x))
        } else if (is.double(x) && !is.object(x)) {
          unclass(bqs_double_to_integer64_strict(x, name))
        } else if (bqs_is_all_na_logical(x)) {
          unclass(rep(bit64::NA_integer64_, length(x)))
        } else {
          bqs_stop_incompatible_type(name, x, "INT64")
        }
      }), recursive = FALSE, use.names = FALSE)
      class(raw_dbl) <- "integer64"
      return(raw_dbl)
    }

    if (target_type == "FLOAT64") {
      if (all(vapply(lst, function(x) is.double(x) && !is.object(x), logical(1)))) {
        return(unlist(lst, recursive = FALSE, use.names = FALSE))
      }
      return(unlist(lapply(lst, function(x) {
        if (is.double(x) && !is.object(x)) {
          x
        } else if ((is.integer(x) && !is.object(x)) || bit64::is.integer64(x)) {
          as.double(x)
        } else if (bqs_is_all_na_logical(x)) {
          rep(NA_real_, length(x))
        } else {
          bqs_stop_incompatible_type(name, x, "FLOAT64")
        }
      }), recursive = FALSE, use.names = FALSE))
    }

    if (target_type == "BOOL") {
      for (x in lst) {
        if (!(is.logical(x) && !is.object(x))) {
          bqs_stop_incompatible_type(name, x, "BOOL")
        }
      }
      return(unlist(lst, recursive = FALSE, use.names = FALSE))
    }

    if (target_type == "STRING") {
      if (all(vapply(lst, function(x) is.character(x) && !is.object(x), logical(1)))) {
        return(unlist(lst, recursive = FALSE, use.names = FALSE))
      }
      return(unlist(lapply(lst, function(x) {
        if ((is.character(x) && (!is.object(x) || inherits(x, "wk_wkt"))) ||
            is.factor(x) ||
            bit64::is.integer64(x) ||
            ((is.integer(x) || is.double(x) || is.logical(x)) && !is.object(x))) {
          as.character(x)
        } else {
          bqs_stop_incompatible_type(name, x, "STRING")
        }
      }), recursive = FALSE, use.names = FALSE))
    }

    if (target_type == "JSON") {
      if (all(vapply(lst, function(x) is.character(x) && !is.object(x), logical(1)))) {
        return(unlist(lst, recursive = FALSE, use.names = FALSE))
      }
      return(unlist(lapply(lst, function(x) {
        if (is.character(x) && !is.object(x)) {
          x
        } else if (bqs_is_all_na_logical(x)) {
          rep(NA_character_, length(x))
        } else {
          bqs_stop_incompatible_type(name, x, "JSON")
        }
      }), recursive = FALSE, use.names = FALSE))
    }

    if (target_type == "GEOGRAPHY") {
      return(unlist(lapply(lst, function(x) {
        if (inherits(x, "wk_wkt") || (is.character(x) && !is.object(x))) {
          as.character(x)
        } else if (bqs_is_all_na_logical(x)) {
          rep(NA_character_, length(x))
        } else {
          bqs_stop_incompatible_type(name, x, "GEOGRAPHY")
        }
      }), recursive = FALSE, use.names = FALSE))
    }

    if (target_type == "BYTES") {
      pieces <- lapply(lst, function(x) {
        if (inherits(x, "blob")) {
          unclass(x)
        } else if (bqs_is_all_na_logical(x)) {
          vector("list", length(x))
        } else if (bqs_is_raw_list(x)) {
          unclass(x)
        } else {
          bqs_stop_incompatible_type(name, x, "BYTES")
        }
      })
      return(structure(
        unlist(pieces, recursive = FALSE, use.names = FALSE) %||% list(),
        ptype = raw(0),
        class = c("blob", "vctrs_list_of", "vctrs_vctr", "list")
      ))
    }

    if (target_type == "DATE") {
      pieces <- lapply(lst, function(x) {
        if (inherits(x, "Date")) {
          as.numeric(x)
        } else if (is.character(x) && !is.object(x)) {
          as.numeric(bqs_parse_date_strict(x, name))
        } else if (bqs_is_all_na_logical(x)) {
          rep(NA_real_, length(x))
        } else {
          bqs_stop_incompatible_type(name, x, "DATE")
        }
      })
      return(as.Date(unlist(pieces, use.names = FALSE), origin = "1970-01-01"))
    }

    if (target_type == "TIME") {
      pieces <- lapply(lst, function(x) {
        if (inherits(x, c("hms", "difftime"))) {
          as.numeric(x, units = "secs")
        } else if (is.character(x) && !is.object(x)) {
          as.numeric(bqs_parse_time_strict(x, name), units = "secs")
        } else if (bqs_is_all_na_logical(x)) {
          rep(NA_real_, length(x))
        } else {
          bqs_stop_incompatible_type(name, x, "TIME")
        }
      })
      return(structure(unlist(pieces, use.names = FALSE), units = "secs", class = c("hms", "difftime")))
    }

    if (target_type %in% c("DATETIME", "TIMESTAMP")) {
      pieces <- lapply(lst, function(x) {
        if (inherits(x, "POSIXt")) {
          as.numeric(x)
        } else if (is.character(x) && !is.object(x)) {
          as.numeric(bqs_parse_timestamp_strict(x, name, target_type))
        } else if (bqs_is_all_na_logical(x)) {
          rep(NA_real_, length(x))
        } else {
          bqs_stop_incompatible_type(name, x, target_type)
        }
      })
      return(as.POSIXct(unlist(pieces, use.names = FALSE), origin = "1970-01-01", tz = "UTC"))
    }

    if (target_type %in% c("NUMERIC", "BIGNUMERIC")) {
      has_chr <- FALSE
      has_i64 <- FALSE
      has_dbl <- FALSE
      for (x in lst) {
        if (bit64::is.integer64(x)) {
          has_i64 <- TRUE
        } else if (is.character(x) && !is.object(x)) {
          has_chr <- TRUE
        } else if (is.double(x) && !is.object(x)) {
          has_dbl <- TRUE
        } else if (!((is.integer(x) && !is.object(x)) || bqs_is_all_na_logical(x))) {
          bqs_stop_incompatible_type(name, x, target_type)
        }
      }
      if (!has_chr && !has_i64) {
        return(unlist(lst, recursive = FALSE, use.names = FALSE))
      }
      if (!has_chr && !has_dbl) {
        raw_dbl <- unlist(lapply(lst, function(x) {
          if (bit64::is.integer64(x)) {
            unclass(x)
          } else if (is.integer(x) && !is.object(x)) {
            unclass(bit64::as.integer64(x))
          } else {
            unclass(rep(bit64::NA_integer64_, length(x)))
          }
        }), recursive = FALSE, use.names = FALSE)
        class(raw_dbl) <- "integer64"
        return(raw_dbl)
      }
      pieces <- lapply(lst, function(x) {
        if (is.character(x) && !is.object(x)) {
          x
        } else if ((is.integer(x) && !is.object(x)) || bit64::is.integer64(x)) {
          as.character(x)
        } else if (is.double(x) && !is.object(x)) {
          bqs_double_to_decimal_chr(x, name, target_type)
        } else {
          rep(NA_character_, length(x))
        }
      })
      return(unlist(pieces, use.names = FALSE))
    }
  }

  non_empty <- Filter(function(x) length(x) > 0L, lst)
  first <- if (length(non_empty)) non_empty[[1]] else lst[[1]]
  if (any(vapply(non_empty, bit64::is.integer64, logical(1)))) {
    if (any(vapply(non_empty, function(x) is.character(x) || is.factor(x), logical(1)))) {
      return(unlist(lapply(lst, as.character), use.names = FALSE))
    }
    has_non_int_dbl <- any(vapply(non_empty, function(x) {
      if (!(is.double(x) && !is.object(x))) return(FALSE)
      if (any(is.nan(x))) return(TRUE)
      vals <- x[!is.na(x)]
      length(vals) > 0L && (!all(is.finite(vals)) || !all(trunc(vals) == vals) || any(abs(vals) >= 2^63))
    }, logical(1)))
    if (has_non_int_dbl) {
      return(unlist(lapply(lst, as.double), use.names = FALSE))
    }
    raw_dbl <- unlist(lapply(lst, function(x) {
      if (bit64::is.integer64(x)) {
        unclass(x)
      } else if (is.integer(x) && !is.object(x)) {
        unclass(bit64::as.integer64(x))
      } else if (is.double(x) && !is.object(x)) {
        unclass(bqs_double_to_integer64_strict(x, name))
      } else if (bqs_is_all_na_logical(x)) {
        unclass(rep(bit64::NA_integer64_, length(x)))
      } else {
        bqs_stop_incompatible_type(name, x, "INT64")
      }
    }), recursive = FALSE, use.names = FALSE)
    class(raw_dbl) <- "integer64"
    return(raw_dbl)
  }
  if (inherits(first, "blob")) {
    pieces <- lapply(lst, function(x) {
      if (inherits(x, "blob")) {
        unclass(x)
      } else if (bqs_is_all_na_logical(x)) {
        vector("list", length(x))
      } else if (bqs_is_raw_list(x)) {
        unclass(x)
      } else {
        bqs_stop_incompatible_type(name, x, "BYTES")
      }
    })
    return(blob::as_blob(unlist(pieces, recursive = FALSE, use.names = FALSE) %||% list()))
  }
  if (inherits(first, "wk_wkt")) {
    pieces <- lapply(lst, function(x) {
      if (inherits(x, "wk_wkt") || (is.character(x) && !is.object(x))) {
        as.character(x)
      } else if (bqs_is_all_na_logical(x)) {
        rep(NA_character_, length(x))
      } else {
        bqs_stop_incompatible_type(name, x, "GEOGRAPHY")
      }
    })
    return(wk::wkt(unlist(pieces, use.names = FALSE)))
  }
  if (inherits(first, "Date")) {
    pieces <- lapply(lst, function(x) {
      if (inherits(x, "Date")) {
        as.numeric(x)
      } else if (bqs_is_all_na_logical(x)) {
        rep(NA_real_, length(x))
      } else {
        bqs_stop_incompatible_type(name, x, "DATE")
      }
    })
    return(as.Date(unlist(pieces, use.names = FALSE), origin = "1970-01-01"))
  }
  if (inherits(first, "POSIXt")) {
    tz <- attr(first, "tzone") %||% "UTC"
    pieces <- lapply(lst, function(x) {
      if (inherits(x, "POSIXt")) {
        as.numeric(x)
      } else if (bqs_is_all_na_logical(x)) {
        rep(NA_real_, length(x))
      } else {
        bqs_stop_incompatible_type(name, x, "TIMESTAMP")
      }
    })
    return(as.POSIXct(unlist(pieces, use.names = FALSE), origin = "1970-01-01", tz = tz))
  }
  if (inherits(first, c("hms", "difftime"))) {
    pieces <- lapply(lst, function(x) {
      if (inherits(x, c("hms", "difftime"))) {
        as.numeric(x, units = "secs")
      } else if (bqs_is_all_na_logical(x)) {
        rep(NA_real_, length(x))
      } else {
        bqs_stop_incompatible_type(name, x, "TIME")
      }
    })
    return(structure(unlist(pieces, use.names = FALSE), units = "secs", class = c("hms", "difftime")))
  }
  if (any(vapply(non_empty, is.factor, logical(1))) &&
      any(vapply(non_empty, is.character, logical(1)))) {
    return(unlist(lapply(lst, as.character), use.names = FALSE))
  }
  unlist(lst, recursive = FALSE, use.names = FALSE)
}
