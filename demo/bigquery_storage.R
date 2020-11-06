### Adapted from https://cloud.google.com/bigquery/docs/reference/storage/libraries#client-libraries-install-python
library(bigrquerystorage)
library(arrow)

# TODO(developer): Set the project_id variable.
project_id <- "labo-brunotremblay-253317"
#
# The read session is created in this project. This project can be
# different from that which contains the table.
client <- bqs_client(verbose = FALSE, credentials_file = "/mnt/c/Users/gen01914/Work/bigrquerystorage/bq-storage-dev.json")

# This example reads baby name data from the public datasets.
table <- sprintf("projects/%s/datasets/%s/tables/%s", "bigquery-public-data", "usa_names", "usa_1910_current")

requested_session <- RProtoBuf::new(RProtoBuf::P("google.cloud.bigquery.storage.v1.ReadSession"))
requested_session$table <- table
# This API can also deliver data serialized in Apache Arrow format.
# This example leverages Apache Avro.
requested_session$data_format <- RProtoBuf::P("google.cloud.bigquery.storage.v1.DataFormat")[["ARROW"]]

# We limit the output columns to a subset of those allowed in the table,
# and set a simple filter to only report names from the state of
# Washington (WA).
requested_session$read_options$selected_fields <- c("name", "number", "state")
requested_session$read_options$row_restriction <- 'state = "WA"'

# Set a snapshot time if it's been specified.
if (exists("snapshot_time")) {
  snapshot_time <- as.POSIXct(snapshot_time)
  snapshot_ts = RProtoBuf::new(RProtoBuf::P("google.protobuf.Timestamp"))
  snapshot_ts$seconds <- as.integer(snapshot_time)
  snapshot_ts$nanos <- as.integer((snapshot_time - as.integer(snapshot_time)) * 1000000000L)
  requested_session$table_modifiers$snapshot_time <- snapshot_ts
}

parent <- sprintf("projects/%s", project_id)
# We'll use only a single stream for reading data from the table. However,
# if you wanted to fan out multiple readers you could do so by having a
# reader process each individual stream.
session <- client$CreateReadSession$build(parent = parent, read_session = requested_session, max_stream_count = 1)

res <- client$CreateReadSession$call(session, c("read_session.table" = session$read_session$table))



reader <- client$ReadRows$build(read_stream = res$streams[[1]]$name)
arrow_reader <- RecordBatchStreamReader$create(res$arrow_schema$serialized_schema)
batches <- list()
no_message <- FALSE
while (!no_message) {
  tryCatch({
    stream <- client$ReadRows$call(reader, c("read_stream" = reader$read_stream))
    batch <- record_batch(stream$arrow_record_batch$serialized_record_batch, schema = arrow_reader$schema)
    batches <- append(batches, batch)
    cat("Batch ", length(batches), "Offset ", stream$row_count + reader$offset, "\n")
    reader$offset <- stream$row_count + reader$offset
    # Too slow, break early while I figure out a better implementation
    if (length(batches) > 5) {
      break
    }
  }, error = function(e) {
    if (!grepl("No response from the gRPC server", e, fixed = TRUE)) {
      stop(e)
    } else {
      no_message = TRUE
    }
  })
}


dt <- as.data.frame(do.call(Table$create,batches))
nrow(dt)
