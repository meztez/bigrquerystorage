
<!-- README.md is generated from README.Rmd. Please edit that file -->

# bigrquerystorage

<!-- badges: start -->

<!-- badges: end -->

The goal of bigrquerystorage is to provide access to the BigQuery
Storage API from R.

The main motivation is to replace `bigrquery::bq_table_download` in some
workload.

Currently it supports v1 version (except AVRO format).

## Benefits

BigQuery Storage API is not rate limited nor has per project quota. No
need to manage `bigrquery::bq_table_download` page size anymore.

BigQuery Storage API is based on gRPC. This particular implementation
use a C++ generated client with the excellent `arrow` R package. It
makes it 2 to 4 times faster than `bigrquery::bq_table_download` method.

More insidious; no more truncated results when
`bigrquery::bq_table_download` page size produce `json` files greater
than 10MiB.

## Installation

System requirements:  
\- C++11  
\- [gRPC C++](https://github.com/grpc/grpc/blob/master/BUILDING.md) I
used [this
procedure](https://github.com/grpc/grpc/blob/master/test/distrib/cpp/run_distrib_test_cmake_module_install_pkgconfig.sh)
after cloning the repo.  
\- [protoc
C++](https://github.com/protocolbuffers/protobuf/tree/master/src)

You can install the development version of bigrquerystorage from
[GitHub](https://github.com/meztez/bigrquerystorage) with:

``` r
mkdir bqs
cd bqs
wget https://raw.githubusercontent.com/meztez/bigrquerystorage/master/scripts/install_debian_ubuntu.sh
chmod 755 ./install_debian_ubuntu.sh
sudo ./install_debian_ubuntu.sh
```

This will also install dependencies and build gRPC correcly.

## Example

This is a basic example which shows you how to solve a common problem.
BigQuery Storage API requires a billing project as there is no free tier
to the service.

``` r
## Auth is done automagically using Application Default Credentials
library(bigrquerystorage)

# TODO(developer): Set the project_id variable.
# project_id <- 'your-project-id'
#
# The read session is created in this project. This project can be
# different from that which contains the table.

rows <- bqs_table_download(
  x = "bigquery-public-data:usa_names.usa_1910_current"
  , parent = project_id
  # , snapshot_time = Sys.time() # a POSIX time
  , selected_fields = c("name", "number", "state"),
  , row_restriction = 'state = "WA"'
  # , access_token = {your token} # Alternative to ADC
  # , as_data_frame = TRUE # FALSE : arrow, TRUE : arrow->as.data.frame
)

sprintf("Got %d unique names in states: %s",
        length(unique(rows$name)),
        paste(unique(rows$state), collapse = " "))

# Replace bigrquery::bq_download_table
library(bigrquery)
rows <- bigrquery::bq_table_download("bigquery-public-data.usa_names.usa_1910_current")
# Downloading 6,122,890 rows in 613 pages.
overload_bq_table_download(project_id)
rows <- bigrquery::bq_table_download("bigquery-public-data.usa_names.usa_1910_current")
# Streamed 6122890 rows in 5980 messages.
```

## Performance

### Compared to Python Client for BigQuery Storage API

Currently about 25% Slower if you consider conversion to data frame.
About the same without the conversion.

### Compared to `bigrquery::bq_table_download`

When `bigrquery::bq_table_download` does not hit a quota or a rate
limit, 2 to 4 times faster. The bigger the table, the faster this will
be compared to the standard REST API.

## Stability

Windows is not supported at the moment.  
Does not support AVRO output format.
