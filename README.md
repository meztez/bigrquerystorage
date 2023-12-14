
<!-- README.md is generated from README.Rmd. Please edit that file -->

# bigrquerystorage

<!-- badges: start -->

[![R-CMD-check](https://github.com/meztez/bigrquerystorage/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/meztez/bigrquerystorage/actions/workflows/R-CMD-check.yaml)
<!-- badges: end -->

<figure>
<img src="./docs/bigrquerystorage.gif"
alt="Comparing bq_table_download from bigrquery to bgs_table_download from bigrquerystorage" />
<figcaption aria-hidden="true">Comparing bq_table_download from
bigrquery to bgs_table_download from bigrquerystorage</figcaption>
</figure>

Use [BigQuery Storage
API](https://cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1)
from R.

The main utility is to replace `bigrquery::bq_table_download` method.

It supports [BigQueryRead
interface](https://cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#bigqueryread).
Support for [BigQueryWrite
interface](https://cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#bigquerywrite)
may be added in a future release.

## Advantages over BigQuery REST API

BigQuery Storage API is not rate limited and per project quota do not
apply. It is an rpc protocol and provides faster downloads for big
results sets.

## Details

This implementation use a C++ generated client combined with the `arrow`
R package to transform the raw stream into an R object.

`bqs_table_download` is the main function of this package. Other
functions are helpers to facilitate authentication and debugging.

The package also includes DBI methods for `dbFetch` and `dbReadTable`.
It should be loaded after `bigrquery`. Alternatively, use
`overload_bq_table_download` to replace `bigrquery::bq_table_download`
directly in `bigrquery` namespace.

## Installation

``` r
# CRAN
install.packages("bigrquerystorage")
# github (main)
remotes::install_github("meztez/bigrquerystorage")
```

### System requirements:

- [gRPC](https://github.com/grpc/grpc)
- [protoc](https://github.com/protocolbuffers/protobuf)

#### Debian 11 & 12 / Ubuntu 22.04

``` sh
# install protoc and grpc
apt-get install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc \
                   pkg-config
```

#### Fedora 36 & 37 & 38 / Rocky Linux 9

``` sh
# install grpc, protoc is automatically installed
dnf install -y grpc-devel pkgconf
```

<details>
<summary>
Other Linux distributions
</summary>

Please [let us
know](https://github.com/meztez/bigrquerystorage/issues/new/choose) if
these instructions do not work any more.

##### Alpine Linux

``` sh
apk add g++ gcc make openssl openssl-dev git cmake bash linux-headers
```

Alpine Linux 3.19 and Edge do not work currently, because the
installation of the arrow package fails.

##### Debian 10

Needs the buster-backports repository.

``` sh
echo "deb https://deb.debian.org/debian buster-backports main" >> \
    /etc/apt/sources.list.d/backports.list && \
    apt-get update && \
apt-get install -y 'libgrpc\+\+-dev'/buster-backports \
    protobuf-compiler-grpc/buster-backports \
    libprotobuf-dev/buster-backports \
    protobuf-compiler/buster-backports pkg-config
```

##### OpenSUSE

In OpenSUSE 15.4 and 15.5 the version of the grpc package is tool old,
so installation fails. You can potentially compile a newer version of
grpc from source.

##### Ubuntu 20.04

In Ubuntu 20.04 the version of the grpc package is tool old, so
installation fails. You can potentially compile a newer version of grpc
from source.

##### CentOS 7 & 8 / RHEL 7 & 8

These distros do not have a grpc package. You can potentially compile
grpc from source.

</details>

#### macOS

If you use Homebrew you may install the `grpc` package, plus
`pkg-config`. If you don’t have Homebrew installed, the package will
download static builds of the system dependencies during installation.
This works with macOS Big Sur, or later, on Intel and Arm64 machines.

``` sh
brew install grpc pkg-config
```

#### Windows

The package will automatically download a static build of the system
requirements during installation. This works on R 4.2.x (with Rtools40
or Rtools42), R 4.3.x (with Rtools43) or later currently.

## Example

This is a basic example which shows you how to solve a common problem.
BigQuery Storage API requires a billing project.

``` r

# Auth is done automagically using Application Default Credentials.
# or reusing bigrquery auth.

# Use the following command once to set it up :
# gcloud auth application-default login --billing-project={project}

library(bigrquery)
library(bigrquerystorage)

# TODO: (developer): Set the project_id variable to your billing project.
# The read session will bill this project. This project can be
# different from the one that contains the table.
project_id <- 'your-project-id'

rows <- bqs_table_download(
  x = "bigquery-public-data:usa_names.usa_1910_current",
  parent = project_id
  # , snapshot_time = Sys.time() # a POSIXct time
  , selected_fields = c("name", "number", "state"),
  row_restriction = 'state = "WA"'
  # , sample_percentage = 50
  # , as_tibble = TRUE
)

sprintf(
  "Got %d unique names in states: %s",
  length(unique(rows$name)),
  paste(unique(rows$state), collapse = " ")
)

# Replace bigrquery::bq_download_table
rows <- bigrquery::bq_table_download("bigquery-public-data.usa_names.usa_1910_current")
# Downloading 6,122,890 rows in 613 pages.
overload_bq_table_download(project_id)
rows <- bigrquery::bq_table_download("bigquery-public-data.usa_names.usa_1910_current")
# Streamed 6122890 rows in 5980 messages.
```

## Authentication

Done using Google Application Default Credentials (ADC) or by recycling
`bigrquery` authentication. Auth will be done automatically the first
time a request is made.

``` r
bqs_auth()
bqs_deauth()
```

## Stability

Does not support AVRO output format. Report any issues to the project
[issue
tracker](https://github.com/meztez/bigrquerystorage/issues/new/choose).

Full gRPC debug trace with
`bigrquerystorage:::bqs_set_log_verbosity(0)`.
