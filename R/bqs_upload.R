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
#' @param stream_type Write stream mode: `"PENDING"` (default, atomic commit via
#'   `CreateWriteStream` / `FinalizeWriteStream` / `BatchCommitWriteStreams`) or
#'   `"DEFAULT"` (writes to the table's `_default` stream, immediately committed).
#' @param row_batch_size Maximum number of rows per `AppendRows` request when
#'   `values` is a `data.frame` (default `100000L`). Automatically scaled down for
#'   wide tables to stay well below the 20 MB gRPC message limit.
#' @param quiet Should progress information be suppressed? Default `NA` (shows progress
#'   in interactive sessions).
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
    stream_type = c("PENDING", "DEFAULT"),
    row_batch_size = 100000L,
    quiet = getOption("bigrquery.quiet", NA),
    ...) {
  if (inherits(x, "bq_table")) {
    project <- x$project
    dataset <- x$dataset
    table <- x$table
  } else {
    bqs_table_name <- unlist(strsplit(unlist(x), "\\.|:"))
    assertthat::assert_that(length(bqs_table_name) >= 3)
    project <- bqs_table_name[1]
    dataset <- bqs_table_name[2]
    table <- bqs_table_name[3]
  }

  create_disposition <- match.arg(create_disposition)
  write_disposition <- match.arg(write_disposition)
  stream_type <- match.arg(stream_type)
  assertthat::assert_that(is.numeric(row_batch_size), length(row_batch_size) == 1, row_batch_size >= 1)
  row_batch_size <- as.integer(row_batch_size)

  if (is.na(quiet)) {
    quiet <- !interactive()
  } else {
    quiet <- isTRUE(quiet)
  }

  bqs_auth()

  bq_tbl <- bigrquery::bq_table(project, dataset, table)
  tbl_exists <- bigrquery::bq_table_exists(bq_tbl)

  if (!is.null(fields)) {
    fields <- bqs_normalize_fields(fields)
  }

  if (!tbl_exists) {
    if (create_disposition == "CREATE_NEVER") {
      stop(sprintf("Table '%s.%s.%s' does not exist and create_disposition is 'CREATE_NEVER'.", project, dataset, table), call. = FALSE)
    }
    create_fields <- if (!is.null(fields)) fields else bqs_infer_bq_fields(values)
    bqs_create_table(bq_tbl, create_fields, ...)
    target_fields <- create_fields
  } else {
    if (write_disposition == "WRITE_EMPTY") {
      if (!bqs_table_is_empty(bq_tbl, project)) {
        stop(sprintf("Table '%s.%s.%s' is not empty and write_disposition is 'WRITE_EMPTY'.", project, dataset, table), call. = FALSE)
      }
      target_fields <- if (!is.null(fields)) fields else bqs_get_table_fields(bq_tbl)
    } else if (write_disposition == "WRITE_TRUNCATE") {
      meta <- bigrquery::bq_table_meta(bq_tbl)
      create_fields <- if (!is.null(fields)) {
        fields
      } else if (length(meta$schema$fields)) {
        bqs_normalize_fields(meta$schema$fields)
      } else {
        bqs_infer_bq_fields(values)
      }
      bigrquery::bq_table_delete(bq_tbl)
      extra <- list(...)
      if (is.null(extra$description) && !is.null(meta$description)) {
        extra$description <- meta$description
      }
      if (is.null(extra$labels) && !is.null(meta$labels)) {
        extra$labels <- meta$labels
      }
      do.call(bqs_create_table, c(list(x = bq_tbl, fields = create_fields), extra))
      target_fields <- create_fields
    } else {
      target_fields <- if (!is.null(fields)) fields else bqs_get_table_fields(bq_tbl)
    }
  }

  ipc_bytes <- bqs_serialize_to_ipc(values, target_fields, row_batch_size)
  if (!length(ipc_bytes)) {
    return(invisible(x))
  }

  bqs_ipc_write(
    client = .global$client$ptr,
    project = project,
    dataset = dataset,
    table = table,
    ipc_bytes = ipc_bytes,
    stream_type = stream_type,
    quiet = quiet
  )

  invisible(x)
}

# Table & Schema helpers --------------------------------------------------

#' @noRd
bqs_normalize_fields <- function(fields) {
  if (is.null(fields)) return(NULL)
  lapply(fields, function(f) {
    type <- toupper(f$type %||% "STRING")
    type <- switch(type,
      INTEGER = "INT64",
      INT = "INT64",
      SMALLINT = "INT64",
      TINYINT = "INT64",
      BYTEINT = "INT64",
      BIGINT = "INT64",
      FLOAT = "FLOAT64",
      DOUBLE = "FLOAT64",
      BOOLEAN = "BOOL",
      STRUCT = "RECORD",
      type
    )
    mode <- toupper(f$mode %||% "NULLABLE")
    subfields <- if (length(f$fields)) bqs_normalize_fields(f$fields) else list()
    out <- list(
      name = as.character(f$name),
      type = type,
      mode = mode
    )
    if (length(subfields)) {
      out$fields <- subfields
    }
    if (!is.null(f$description) && nzchar(f$description)) {
      out$description <- as.character(f$description)
    }
    if (!is.null(f$rangeElementType)) {
      out$rangeElementType <- f$rangeElementType
    }
    out
  })
}

#' @noRd
`%||%` <- function(x, y) if (is.null(x)) y else x

#' @noRd
bqs_get_table_fields <- function(bq_tbl) {
  meta <- bigrquery::bq_table_meta(bq_tbl, fields = "schema")
  bqs_normalize_fields(meta$schema$fields)
}

#' @noRd
bqs_table_is_empty <- function(bq_tbl, parent) {
  if (bigrquery::bq_table_nrow(bq_tbl) > 0) {
    return(FALSE)
  }
  # Storage Write API commits may not immediately update REST `numRows` metadata;
  # verify via a 1-row Storage Read check.
  stream <- bqs_table_download(bq_tbl, parent = parent, n_max = 1L, type = "arrow", quiet = TRUE)
  batch <- stream$get_next()
  is.null(batch) || batch$length == 0L
}

#' @noRd
bqs_create_table <- function(x, fields, ...) {
  extra <- list(...)
  extra$billing <- NULL
  if (length(fields)) {
    bq_fields_list <- bqs_fields_to_bq_fields(fields)
    do.call(bigrquery::bq_table_create, c(list(x = x, fields = bq_fields_list), extra))
  } else {
    do.call(bigrquery::bq_table_create, c(list(x = x), extra))
  }
}

#' @noRd
bqs_fields_to_bq_fields <- function(fields) {
  bigrquery::bq_fields(unname(lapply(fields, function(f) {
    subfields <- if (length(f$fields)) bqs_fields_to_bq_fields(f$fields) else list()
    bigrquery::bq_field(
      name = f$name,
      type = f$type,
      mode = f$mode %||% "NULLABLE",
      fields = subfields,
      description = f$description
    )
  })))
}

#' @noRd
bqs_infer_bq_fields <- function(values) {
  if (is.data.frame(values)) {
    return(unname(Map(bqs_infer_bq_field_from_r, values, names(values))))
  }
  sch <- nanoarrow::infer_nanoarrow_schema(values)
  unname(lapply(sch$children, bqs_infer_bq_field_from_arrow))
}

#' @noRd
bqs_infer_bq_field_from_r <- function(col, name) {
  if (is.data.frame(col)) {
    return(list(
      name = name,
      type = "RECORD",
      mode = "NULLABLE",
      fields = unname(Map(bqs_infer_bq_field_from_r, col, names(col)))
    ))
  }
  if (inherits(col, "blob")) {
    return(list(name = name, type = "BYTES", mode = "NULLABLE"))
  }
  if (inherits(col, "wk_wkt")) {
    return(list(name = name, type = "GEOGRAPHY", mode = "NULLABLE"))
  }
  if (inherits(col, "POSIXt")) {
    tz <- attr(col, "tzone")
    return(list(name = name, type = "TIMESTAMP", mode = "NULLABLE"))
  }
  if (inherits(col, c("hms", "difftime"))) {
    return(list(name = name, type = "TIME", mode = "NULLABLE"))
  }
  if (inherits(col, "Date")) {
    return(list(name = name, type = "DATE", mode = "NULLABLE"))
  }
  if (bit64::is.integer64(col) || is.integer(col)) {
    return(list(name = name, type = "INT64", mode = "NULLABLE"))
  }
  if (is.factor(col) || is.character(col)) {
    return(list(name = name, type = "STRING", mode = "NULLABLE"))
  }
  if (is.logical(col)) {
    return(list(name = name, type = "BOOL", mode = "NULLABLE"))
  }
  if (is.double(col)) {
    return(list(name = name, type = "FLOAT64", mode = "NULLABLE"))
  }
  if (is.list(col)) {
    ptype <- attr(col, "ptype")
    if (is.null(ptype)) {
      non_null <- Filter(Negate(is.null), col)
      if (length(non_null)) {
        first_el <- non_null[[1]]
        if (is.data.frame(first_el)) {
          # Find the first data.frame element with > 0 rows if possible so nested
          # list columns inside a REPEATED RECORD retain a sample element for type inference
          non_empty_df <- Filter(function(d) is.data.frame(d) && nrow(d) > 0L, non_null)
          ptype <- if (length(non_empty_df)) non_empty_df[[1]] else first_el
        } else if (is.list(first_el) && rlang::is_named(first_el)) {
          return(list(
            name = name,
            type = "RECORD",
            mode = "NULLABLE",
            fields = unname(Map(bqs_infer_bq_field_from_r, first_el, names(first_el)))
          ))
        } else {
          non_empty_vec <- Filter(function(v) length(v) > 0L, non_null)
          ptype <- if (length(non_empty_vec)) non_empty_vec[[1]][0] else first_el[0]
        }
      } else {
        ptype <- character()
      }
    }
    child_field <- bqs_infer_bq_field_from_r(ptype, name)
    child_field$mode <- "REPEATED"
    return(child_field)
  }
  stop(sprintf("Unsupported column type '%s' for column '%s'.", paste(class(col), collapse = "/"), name), call. = FALSE)
}

#' @noRd
bqs_infer_bq_field_from_arrow <- function(sch) {
  parsed <- nanoarrow::nanoarrow_schema_parse(sch)
  ext <- sch$metadata[["ARROW:extension:name"]]
  name <- sch$name %||% ""
  if (identical(parsed$type, "list") || identical(parsed$type, "large_list")) {
    child <- bqs_infer_bq_field_from_arrow(sch$children[[1]])
    child$name <- name
    child$mode <- "REPEATED"
    return(child)
  }
  if (identical(parsed$type, "struct")) {
    return(list(
      name = name,
      type = "RECORD",
      mode = "NULLABLE",
      fields = unname(lapply(sch$children, bqs_infer_bq_field_from_arrow))
    ))
  }
  if (identical(ext, "google:sqlType:geography")) {
    return(list(name = name, type = "GEOGRAPHY", mode = "NULLABLE"))
  }
  if (identical(ext, "google:sqlType:datetime")) {
    return(list(name = name, type = "DATETIME", mode = "NULLABLE"))
  }
  type <- switch(parsed$type,
    int8 = , int16 = , int32 = , int64 = , uint8 = , uint16 = , uint32 = , uint64 = "INT64",
    float = , double = , half_float = "FLOAT64",
    bool = "BOOL",
    string = , large_string = "STRING",
    binary = , large_binary = , fixed_size_binary = "BYTES",
    date32 = , date64 = "DATE",
    time32 = , time64 = "TIME",
    timestamp = if (nzchar(parsed$timezone %||% "")) "TIMESTAMP" else "DATETIME",
    decimal128 = "NUMERIC",
    decimal256 = "BIGNUMERIC",
    "STRING"
  )
  list(name = name, type = type, mode = "NULLABLE")
}

# Arrow IPC Serialization -------------------------------------------------

#' @noRd
bqs_serialize_to_ipc <- function(values, target_fields, row_batch_size) {
  if (inherits(values, c("nanoarrow_array_stream", "nanoarrow_array"))) {
    stream <- nanoarrow::as_nanoarrow_array_stream(values)
    stream_schema <- stream$get_schema()
    first_batch <- stream$get_next()
    if (is.null(first_batch)) {
      return(raw(0))
    }
    first_batches <- list(first_batch)
    repeat {
      b <- stream$get_next()
      if (is.null(b)) break
      first_batches[[length(first_batches) + 1L]] <- b
    }
    total_rows <- sum(vapply(first_batches, function(b) b$length, integer(1)))
    if (total_rows == 0L) {
      return(raw(0))
    }
    replay <- nanoarrow::basic_array_stream(first_batches, schema = stream_schema, validate = FALSE)
    con <- rawConnection(raw(0), "wb")
    on.exit(close(con))
    nanoarrow::write_nanoarrow(replay, con)
    return(rawConnectionValue(con))
  }

  if (!is.data.frame(values)) {
    stream <- nanoarrow::as_nanoarrow_array_stream(values)
    return(bqs_serialize_to_ipc(stream, target_fields, row_batch_size))
  }

  n <- nrow(values)
  if (n == 0L || ncol(values) == 0L) {
    return(raw(0))
  }

  # Scale batch size for wide tables to keep each serialized batch well under 20 MB
  est_bytes_per_row <- max(64L, ncol(values) * 16L)
  max_rows_by_size <- max(1000L, as.integer(8L * 1024L * 1024L / est_bytes_per_row))
  batch_rows <- min(row_batch_size, max_rows_by_size)

  if (n <= batch_rows) {
    arr <- bqs_df_to_arrow_array(values, target_fields)
    sch <- nanoarrow::infer_nanoarrow_schema(arr)
    stream <- nanoarrow::basic_array_stream(list(arr), schema = sch, validate = FALSE)
  } else {
    starts <- seq.int(1L, n, by = batch_rows)
    batches <- lapply(starts, function(s) {
      e <- min(n, s + batch_rows - 1L)
      chunk <- bqs_slice_df(values, s:e)
      bqs_df_to_arrow_array(chunk, target_fields)
    })
    sch <- nanoarrow::infer_nanoarrow_schema(batches[[1]])
    stream <- nanoarrow::basic_array_stream(batches, schema = sch, validate = FALSE)
  }

  con <- rawConnection(raw(0), "wb")
  on.exit(close(con))
  nanoarrow::write_nanoarrow(stream, con)
  rawConnectionValue(con)
}

#' @noRd
bqs_slice_df <- function(df, idx) {
  out <- lapply(df, function(col) {
    if (is.data.frame(col)) {
      bqs_slice_df(col, idx)
    } else {
      col[idx]
    }
  })
  attr(out, "names") <- names(df)
  attr(out, "class") <- "data.frame"
  attr(out, "row.names") <- .set_row_names(length(idx))
  out
}

#' @noRd
bqs_df_to_arrow_array <- function(df, target_fields = NULL) {
  n <- nrow(df)
  col_names <- names(df)
  field_map <- if (length(target_fields)) {
    f_names <- tolower(vapply(target_fields, `[[`, character(1), "name"))
    names(target_fields) <- f_names
    target_fields
  } else {
    list()
  }

  children <- vector("list", length(col_names))
  child_schemas <- vector("list", length(col_names))
  names(child_schemas) <- col_names

  for (i in seq_along(col_names)) {
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
bqs_col_to_arrow_array <- function(col, name, field = NULL) {
  mode <- if (!is.null(field)) toupper(field$mode %||% "NULLABLE") else "NULLABLE"
  type <- if (!is.null(field)) toupper(field$type %||% "") else ""

  if (mode == "REPEATED") {
    return(bqs_repeated_to_arrow_array(col, name, field))
  }

  if (is.data.frame(col) || type %in% c("RECORD", "STRUCT")) {
    if (!is.data.frame(col) && is.list(col)) {
      col <- bqs_list_of_named_lists_to_df(col, if (!is.null(field)) field$fields else NULL)
    }
    subfields <- if (!is.null(field)) field$fields else NULL
    return(bqs_df_to_arrow_array(col, subfields))
  }

  if (is.list(col) && !inherits(col, "blob") && !inherits(col, "POSIXlt")) {
    non_null <- Filter(Negate(is.null), col)
    if (length(non_null) > 0 && is.list(non_null[[1]]) && !is.data.frame(non_null[[1]]) && rlang::is_named(non_null[[1]])) {
      col <- bqs_list_of_named_lists_to_df(col, NULL)
      return(bqs_df_to_arrow_array(col, NULL))
    }
    return(bqs_repeated_to_arrow_array(col, name, field))
  }

  if (inherits(col, "wk_wkt") || type == "GEOGRAPHY") {
    return(nanoarrow::as_nanoarrow_array(as.character(col), schema = nanoarrow::na_string()))
  }

  if (is.factor(col)) {
    return(nanoarrow::as_nanoarrow_array(as.character(col), schema = nanoarrow::na_string()))
  }

  if (inherits(col, c("hms", "difftime")) || type == "TIME") {
    if (!inherits(col, "difftime")) {
      col <- hms::as_hms(col)
    }
    return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_time64("us")))
  }

  if (type == "DATETIME") {
    if (is.character(col)) {
      col <- as.POSIXct(col, tz = "UTC")
    }
    return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_timestamp("us", timezone = "")))
  }

  if (inherits(col, "POSIXt") || type == "TIMESTAMP") {
    if (is.character(col)) {
      col <- as.POSIXct(col, tz = "UTC")
    }
    return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_timestamp("us", timezone = "UTC")))
  }

  if (inherits(col, "Date") || type == "DATE") {
    if (is.character(col)) {
      col <- as.Date(col)
    }
    return(nanoarrow::as_nanoarrow_array(col, schema = nanoarrow::na_date32()))
  }

  if (type == "NUMERIC") {
    return(bqs_decimal_to_arrow_array(col, precision = 38L, scale = 9L, bitwidth = 128L))
  }

  if (type == "BIGNUMERIC") {
    scale <- if (is.character(col)) 38L else 9L
    return(bqs_decimal_to_arrow_array(col, precision = 76L, scale = scale, bitwidth = 256L))
  }

  if (type == "INT64" && !bit64::is.integer64(col) && !is.integer(col)) {
    col <- bit64::as.integer64(col)
  }

  if (type == "FLOAT64" && is.integer(col)) {
    col <- as.double(col)
  }

  if (type == "STRING" && !is.character(col)) {
    col <- as.character(col)
  }

  nanoarrow::as_nanoarrow_array(col)
}

#' @noRd
bqs_decimal_to_arrow_array <- function(col, precision, scale, bitwidth) {
  enc <- bqs_encode_decimal(col, scale = scale, bitwidth = bitwidth)
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
    ef
  } else {
    NULL
  }

  non_null <- Filter(Negate(is.null), col)
  is_record <- (!is.null(elem_field) && elem_field$type %in% c("RECORD", "STRUCT")) ||
    (length(non_null) > 0 && is.data.frame(non_null[[1]]))

  if (is_record) {
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
    lens <- lengths(col)
    offsets <- c(0L, cumsum(lens))
    if (length(non_null) > 0) {
      combined <- bqs_concat_vectors(non_null)
    } else {
      combined <- bqs_empty_col_for_field(elem_field)
    }
    item_arr <- bqs_col_to_arrow_array(combined, "item", elem_field)
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
  cols <- lapply(col_names, function(nm) {
    pieces <- lapply(lst, function(el) if (is.null(el) || is.null(el[[nm]])) NA else el[[nm]])
    bqs_concat_vectors(pieces)
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
bqs_empty_col_for_field <- function(field) {
  if (is.null(field)) return(character(0))
  mode <- toupper(field$mode %||% "NULLABLE")
  if (mode == "REPEATED") return(list())
  type <- toupper(field$type %||% "STRING")
  if (type %in% c("RECORD", "STRUCT")) {
    if (!length(field$fields)) return(tibble::tibble())
    sub <- lapply(field$fields, bqs_empty_col_for_field)
    names(sub) <- vapply(field$fields, `[[`, character(1), "name")
    return(tibble::new_tibble(sub, nrow = 0L))
  }
  switch(type,
    INT64 = , INTEGER = , INT = , SMALLINT = , TINYINT = , BYTEINT = , BIGINT = bit64::integer64(0),
    FLOAT64 = , FLOAT = , DOUBLE = , NUMERIC = , BIGNUMERIC = double(0),
    BOOL = , BOOLEAN = logical(0),
    BYTES = list(),
    DATE = as.Date(character(0)),
    TIME = structure(numeric(0), units = "secs", class = c("hms", "difftime")),
    DATETIME = , TIMESTAMP = as.POSIXct(numeric(0), origin = "1970-01-01", tz = "UTC"),
    character(0)
  )
}

#' @noRd
bqs_concat_vectors <- function(lst) {
  non_empty <- Filter(function(x) length(x) > 0L, lst)
  first <- if (length(non_empty)) non_empty[[1]] else lst[[1]]
  if (bit64::is.integer64(first)) {
    raw_dbl <- unlist(lapply(lst, function(x) {
      if (!bit64::is.integer64(x)) x <- bit64::as.integer64(x)
      unclass(x)
    }), recursive = FALSE, use.names = FALSE)
    class(raw_dbl) <- "integer64"
    return(raw_dbl)
  }
  if (inherits(first, "blob")) {
    raw_list <- unlist(lapply(lst, unclass), recursive = FALSE, use.names = FALSE)
    return(blob::as_blob(raw_list))
  }
  if (inherits(first, "wk_wkt")) {
    return(wk::wkt(unlist(lapply(lst, as.character), use.names = FALSE)))
  }
  if (inherits(first, "Date")) {
    return(as.Date(unlist(lapply(lst, as.numeric), use.names = FALSE), origin = "1970-01-01"))
  }
  if (inherits(first, "POSIXt")) {
    tz <- attr(first, "tzone") %||% "UTC"
    return(as.POSIXct(unlist(lapply(lst, as.numeric), use.names = FALSE), origin = "1970-01-01", tz = tz))
  }
  if (inherits(first, "hms")) {
    return(hms::as_hms(unlist(lapply(lst, as.numeric), use.names = FALSE)))
  }
  unlist(lst, recursive = FALSE, use.names = FALSE)
}
