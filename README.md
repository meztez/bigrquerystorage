
<!-- README.md is generated from README.Rmd. Please edit that file -->

# bigrquerystorage

<!-- badges: start -->

<!-- badges: end -->

The goal of bigrquerystorage is to provide acces to the BigQuery Storage
API from R.

Currently it supports v1 version with very basic table mass download
capacity.

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
# install.packages("devtools")
devtools::install_github("meztez/bigrquerystorage")
```

## Example

This is a basic example which shows you how to solve a common problem:

``` r
## Auth is done automagically using Application Default Credentials
library(bigrquerystorage)
## basic example code
bqs_table_download("bigquery-public-data.usa_names.usa_1910_current", "labo-brunotremblay-253317")
```
