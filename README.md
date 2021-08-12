
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
use a C++ generated client with `arrow` R package. It makes it 2 to 4
times faster than `bigrquery::bq_table_download` method on GCE virtual
machines.

More insidious; no more truncated results when
`bigrquery::bq_table_download` page size produce `json` files greater
than 10MiB. Update : This should be fixed in the lastest bigrquery package release. Have not tested.

## Installation (w64 support only)

``` r
remotes::install_github("meztez/bigrquerystorage", INSTALL_opts = "--no-multiarch")
```

### System requirements:

  - [gRPC](https://github.com/grpc/grpc)
  - [protoc](https://github.com/protocolbuffers/protobuf)

#### Debian/Ubuntu

``` sh
# install protoc and grpc
apt-get install build-essential autoconf libtool pkg-config
git clone -b v1.33.2 https://github.com/grpc/grpc
cd grpc
git submodule update --init
./test/distrib/cpp/run_distrib_test_cmake_module_install_pkgconfig.sh
cd ..
rm -R grpc
```

#### Windows

If it detects `Rtools40`, it should be able to install dependencies from
CRAN or bintray.

## Example

This is a basic example which shows you how to solve a common problem.
BigQuery Storage API requires a billing project as there is no free tier
to the service.

``` r
## Auth is done automagically using Application Default Credentials.
## Use the following command once to set it up :
## gcloud auth application-default login --billing-project={project}
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
  # , as_tibble = TRUE # FALSE : arrow, TRUE : arrow->as.data.frame
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

About the same.

### Compared to `bigrquery::bq_table_download`

When `bigrquery::bq_table_download` does not hit a quota or a rate
limit, 2 to 4 times faster. The bigger the table, the faster this will
be compared to the standard REST API. Best results is obtained on GCE
virtual machines close to the data.

## Authentification

Done using Google Application Default Credentials (ADC) or by recycling
`bigrquery` authentification. Auth will be done automatically the first
time a request is made.

``` r
bqs_auth()
bqs_deauth()
```

## Stability

Does not support AVRO output format. Report any issues to the project
[issue
tracker](https://github.com/meztez/bigrquerystorage/issues/new/choose).
