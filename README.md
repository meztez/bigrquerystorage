
<!-- README.md is generated from README.Rmd. Please edit that file -->

# bigrquerystorage

<!-- badges: start -->

[![CRAN_Status_Badge](https://www.r-pkg.org/badges/version/bigrquerystorage)](https://cran.r-project.org/package=bigrquerystorage)
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

The main utility is to replace `bigrquery::bq_table_download` and
`bigrquery::bq_table_upload` methods.

It supports both
[BigQueryRead](https://cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#bigqueryread)
and
[BigQueryWrite](https://cloud.google.com/bigquery/docs/reference/storage/rpc/google.cloud.bigquery.storage.v1#bigquerywrite)
interfaces.

## Advantages over BigQuery REST API

BigQuery Storage API is not rate limited and per project quota do not
apply. It is an rpc protocol and provides faster downloads and uploads
using native binary Apache Arrow serialization instead of JSON.

### Performance (`bigrquerystorage` Arrow API vs `bigrquery` JSON, `duckdb`, and `adbc`)

In R benchmarks on BigQuery public datasets, **`bigrquerystorage`**
(`bigrquery` with `api = "arrow"`) is **23X to 46X times faster** on
download (*Time to Tibble*) and **5X to 16X times faster** on upload
(*Time to Table*) than `bigrquery` without the Arrow API
(`api = "json"`), while performing **1.8X to 2.0X times faster** on
download (and **3.0X to 5.1X times faster** on upload) than `duckdb`
(`bigquery` extension), and **3.5X to 7.3X times faster** on download
(and **1.7X to 2.1X times faster** on upload) than the `adbc` BigQuery
driver.

#### Download Benchmark — Time to Tibble (TTT)

| Dataset | Dimensions (In-Memory) | Method | Median TTT (s) | Transfer Speed (MB/s) | Throughput (M rows/s) | vs `bigrquery` (JSON) | vs `bigrquerystorage` (Arrow) |
|:---|:---|:---|---:|---:|---:|---:|:---|
| `samples.gsod` `year >= 2009` (4.58M x 31) | 4.58M x 31 (752 MB) | **bigrquerystorage (Arrow API)** | 1.30 s | 577.3 MB/s | 3.52 M/s | **45.7X faster** | 1.0x (baseline) |
| `samples.gsod` `year >= 2009` (4.58M x 31) | 4.58M x 31 (752 MB) | **duckdb (bigquery ext)** | 2.33 s | 322.9 MB/s | 1.97 M/s | **25.5X faster** | 1.8x slower |
| `samples.gsod` `year >= 2009` (4.58M x 31) | 4.58M x 31 (752 MB) | **adbc (bigquery driver)** | 9.55 s | 78.7 MB/s | 0.48 M/s | **6.2X faster** | 7.3x slower |
| `samples.gsod` `year >= 2009` (4.58M x 31) | 4.58M x 31 (752 MB) | **bigrquery (JSON API)** | 59.39 s | 12.7 MB/s | 0.08 M/s | 1.0x (baseline) | 45.7x slower |
| `usa_names.usa_1910_current` (6.31M x 5) | 6.31M x 5 (194 MB) | **bigrquerystorage (Arrow API)** | 1.00 s | 194.8 MB/s | 6.32 M/s | **22.7X faster** | 1.0x (baseline) |
| `usa_names.usa_1910_current` (6.31M x 5) | 6.31M x 5 (194 MB) | **duckdb (bigquery ext)** | 2.05 s | 94.9 MB/s | 3.08 M/s | **11.1X faster** | 2.0x slower |
| `usa_names.usa_1910_current` (6.31M x 5) | 6.31M x 5 (194 MB) | **adbc (bigquery driver)** | 3.45 s | 56.3 MB/s | 1.83 M/s | **6.6X faster** | 3.5x slower |
| `usa_names.usa_1910_current` (6.31M x 5) | 6.31M x 5 (194 MB) | **bigrquery (JSON API)** | 22.66 s | 8.6 MB/s | 0.28 M/s | 1.0x (baseline) | 22.7x slower |

#### Upload Benchmark — Time to Table (TTT)

| Dataset | Dimensions (In-Memory) | Method | Median TTT (s) | Transfer Speed (MB/s) | Throughput (M rows/s) | vs `bigrquery` (JSON) | vs `bigrquerystorage` (Arrow) |
|:---|:---|:---|---:|---:|---:|---:|:---|
| `samples.gsod` slice (1.00M x 31) | 1.00M x 31 (164 MB) | **bigrquerystorage (Arrow API)** | 2.40 s | 68.4 MB/s | 0.42 M/s | **15.7X faster** | 1.0x (baseline) |
| `samples.gsod` slice (1.00M x 31) | 1.00M x 31 (164 MB) | **duckdb (bigquery ext)** | 12.18 s | 13.5 MB/s | 0.08 M/s | **3.1X faster** | 5.1x slower |
| `samples.gsod` slice (1.00M x 31) | 1.00M x 31 (164 MB) | **adbc (bigquery driver)** | 5.01 s | 32.7 MB/s | 0.20 M/s | **7.5X faster** | 2.1x slower |
| `samples.gsod` slice (1.00M x 31) | 1.00M x 31 (164 MB) | **bigrquery (JSON API)** | 37.71 s | 4.3 MB/s | 0.03 M/s | 1.0x (baseline) | 15.7x slower |
| `usa_names` slice (1.00M x 5) | 1.00M x 5 (32 MB) | **bigrquerystorage (Arrow API)** | 2.32 s | 13.7 MB/s | 0.43 M/s | **5.4X faster** | 1.0x (baseline) |
| `usa_names` slice (1.00M x 5) | 1.00M x 5 (32 MB) | **duckdb (bigquery ext)** | 6.97 s | 4.6 MB/s | 0.14 M/s | **1.8X faster** | 3.0x slower |
| `usa_names` slice (1.00M x 5) | 1.00M x 5 (32 MB) | **adbc (bigquery driver)** | 3.91 s | 8.1 MB/s | 0.26 M/s | **3.2X faster** | 1.7x slower |
| `usa_names` slice (1.00M x 5) | 1.00M x 5 (32 MB) | **bigrquery (JSON API)** | 12.44 s | 2.5 MB/s | 0.08 M/s | 1.0x (baseline) | 5.4x slower |

*Benchmarks executed in R (R version 4.6.0) over 3 iterations per
workload using `r-dbi/bigrquery#695` (`api = "arrow"` vs
`api = "json"`), `duckdb` (`bigquery` extension), and
`adbcdrivermanager` (`bigquery` driver).*

## Details

This implementation uses a C++ gRPC client combined with the `nanoarrow`
R package to transform Arrow IPC streams to and from R `tibble`,
`data.frame`, `data.table`, or `nanoarrow_array_stream`.

`bqs_table_download` and `bqs_table_upload` are the main functions of
this package. Other functions are helpers to facilitate authentication
and debugging.

## Installation

#### CRAN

``` r
install.packages("bigrquerystorage")
```

#### Github

``` r
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
apk add grpc-dev protobuf-dev re2-dev c-ares-dev
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

In OpenSUSE 15.4 and 15.5 the version of the grpc package is too old, so
installation fails. You can potentially compile a newer version of grpc
from source.

##### Ubuntu 20.04

In Ubuntu 20.04 the version of the grpc package is too old, so
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

From Rtools43, grpc is included in the toolchain.

The package used to automatically download static builds of the system
requirements during installation but this was removed per CRAN policy.
Only, R 4.3.x (with Rtools43) or later is currently supported.

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
)

sprintf(
  "Got %d unique names in states: %s",
  length(unique(rows$name)),
  paste(unique(rows$state), collapse = " ")
)

# Upload a data frame using the BigQuery Storage Write API
bqs_table_upload(
  x = paste0(project_id, ".my_dataset.my_table"),
  values = mtcars,
  create_disposition = "CREATE_IF_NEEDED",
  write_disposition = "WRITE_TRUNCATE"
)
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
