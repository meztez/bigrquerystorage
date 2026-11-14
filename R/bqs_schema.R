# Schema normalization & field inference ----------------------------------

#' @noRd
bqs_normalize_fields <- function(fields) {
  if (is.null(fields)) return(NULL)
  lapply(fields, function(f) {
    type <- switch(toupper(f$type %||% "STRING"),
      INTEGER = , INT = , SMALLINT = , TINYINT = , BYTEINT = , BIGINT = "INT64",
      FLOAT = , DOUBLE = "FLOAT64",
      BOOLEAN = "BOOL",
      STRUCT = "RECORD",
      toupper(f$type %||% "STRING")
    )
    out <- list(
      name = as.character(f$name),
      type = type,
      mode = toupper(f$mode %||% "NULLABLE")
    )
    if (length(f$fields)) {
      out$fields <- bqs_normalize_fields(f$fields)
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
bqs_get_table_fields <- function(bq_tbl) {
  meta <- bigrquery::bq_table_meta(bq_tbl, fields = "schema")
  bqs_normalize_fields(meta$schema$fields)
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
  type <- if (inherits(col, "blob")) {
    "BYTES"
  } else if (inherits(col, "wk_wkt")) {
    "GEOGRAPHY"
  } else if (inherits(col, "POSIXt")) {
    "TIMESTAMP"
  } else if (inherits(col, c("hms", "difftime"))) {
    "TIME"
  } else if (inherits(col, "Date")) {
    "DATE"
  } else if (bit64::is.integer64(col) || is.integer(col)) {
    "INT64"
  } else if (is.factor(col) || is.character(col)) {
    "STRING"
  } else if (is.logical(col)) {
    "BOOL"
  } else if (is.double(col)) {
    "FLOAT64"
  } else {
    NULL
  }
  if (!is.null(type)) {
    return(list(name = name, type = type, mode = "NULLABLE"))
  }

  if (is.list(col)) {
    ptype <- attr(col, "ptype")
    if (is.null(ptype)) {
      non_null <- Filter(Negate(is.null), col)
      if (length(non_null)) {
        first_el <- non_null[[1]]
        if (is.data.frame(first_el)) {
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
bqs_schema_has_dictionary <- function(sch) {
  !is.null(sch$dictionary) || any(vapply(sch$children, bqs_schema_has_dictionary, logical(1)))
}

#' @noRd
bqs_infer_bq_field_from_arrow <- function(sch) {
  ext <- sch$metadata[["ARROW:extension:name"]]
  name <- sch$name %||% ""
  if (!is.null(sch$dictionary)) {
    out <- bqs_infer_bq_field_from_arrow(sch$dictionary)
    out$name <- name
    if (identical(ext, "google:sqlType:geography")) out$type <- "GEOGRAPHY"
    if (identical(ext, "google:sqlType:datetime")) out$type <- "DATETIME"
    return(out)
  }
  parsed <- nanoarrow::nanoarrow_schema_parse(sch)
  if (parsed$type %in% c("list", "large_list")) {
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

#' @noRd
bqs_empty_col_for_field <- function(field) {
  if (is.null(field)) return(character(0))
  if (toupper(field$mode %||% "NULLABLE") == "REPEATED") return(list())
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
