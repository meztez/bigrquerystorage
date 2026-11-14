### Performance (`bigrquerystorage` Arrow API vs `bigrquery` JSON, `duckdb`, and `adbc`)

In R benchmarks on BigQuery public datasets, **`bigrquerystorage`** (`bigrquery` with `api = "arrow"`) is **21X to 47X times faster** on download (*Time to Tibble*) and **10X to 20X times faster** on upload (*Time to Table*) than `bigrquery` without the Arrow API (`api = "json"`), while performing **1.6X to 2.3X times faster** on download (and **4.1X to 5.6X times faster** on upload) than `duckdb` (`bigquery` extension), and **2.6X to 7.1X times faster** on download (and **3.1X to 3.3X times faster** on upload) than the `adbc` BigQuery driver.

![BigQuery R Benchmarks: Time to Tibble (Download) & Time to Table (Upload)](man/figures/benchmark_bigrquerystorage.png)

#### Download Benchmark — Time to Tibble (TTT)

| Dataset | Dimensions (In-Memory) | Method | Median TTT (s) | Transfer Speed (MB/s) | Throughput (M rows/s) | vs `bigrquery` (JSON) | vs `bigrquerystorage` (Arrow) |
|:---|:---|:---|---:|---:|---:|---:|:---|
| `samples.gsod` `year >= 2009` (4.58M x 31) | 4.58M x 31 (752 MB) | **bigrquerystorage (Arrow API)** | 1.20 s | 624.8 MB/s | 3.81 M/s | **46.8X faster** | 1.0x (baseline) |
| `samples.gsod` `year >= 2009` (4.58M x 31) | 4.58M x 31 (752 MB) | **duckdb (bigquery ext)** | 2.74 s | 274.3 MB/s | 1.67 M/s | **20.5X faster** | 2.3x slower |
| `samples.gsod` `year >= 2009` (4.58M x 31) | 4.58M x 31 (752 MB) | **adbc (bigquery driver)** | 8.53 s | 88.1 MB/s | 0.54 M/s | **6.6X faster** | 7.1x slower |
| `samples.gsod` `year >= 2009` (4.58M x 31) | 4.58M x 31 (752 MB) | **bigrquery (JSON API)** | 56.12 s | 13.4 MB/s | 0.08 M/s | 1.0x (baseline) | 46.8x slower |
| `usa_names.usa_1910_current` (6.31M x 5) | 6.31M x 5 (194 MB) | **bigrquerystorage (Arrow API)** | 1.10 s | 176.4 MB/s | 5.73 M/s | **20.8X faster** | 1.0x (baseline) |
| `usa_names.usa_1910_current` (6.31M x 5) | 6.31M x 5 (194 MB) | **duckdb (bigquery ext)** | 1.73 s | 112.6 MB/s | 3.66 M/s | **13.2X faster** | 1.6x slower |
| `usa_names.usa_1910_current` (6.31M x 5) | 6.31M x 5 (194 MB) | **adbc (bigquery driver)** | 2.86 s | 67.9 MB/s | 2.20 M/s | **8.0X faster** | 2.6x slower |
| `usa_names.usa_1910_current` (6.31M x 5) | 6.31M x 5 (194 MB) | **bigrquery (JSON API)** | 22.90 s | 8.5 MB/s | 0.28 M/s | 1.0x (baseline) | 20.8x slower |

#### Upload Benchmark — Time to Table (TTT)

| Dataset | Dimensions (In-Memory) | Method | Median TTT (s) | Transfer Speed (MB/s) | Throughput (M rows/s) | vs `bigrquery` (JSON) | vs `bigrquerystorage` (Arrow) |
|:---|:---|:---|---:|---:|---:|---:|:---|
| `samples.gsod` slice (1.00M x 31) | 1.00M x 31 (164 MB) | **bigrquerystorage (Arrow API)** | 2.04 s | 80.6 MB/s | 0.49 M/s | **20.2X faster** | 1.0x (baseline) |
| `samples.gsod` slice (1.00M x 31) | 1.00M x 31 (164 MB) | **duckdb (bigquery ext)** | 11.42 s | 14.4 MB/s | 0.09 M/s | **3.6X faster** | 5.6x slower |
| `samples.gsod` slice (1.00M x 31) | 1.00M x 31 (164 MB) | **adbc (bigquery driver)** | 6.26 s | 26.2 MB/s | 0.16 M/s | **6.6X faster** | 3.1x slower |
| `samples.gsod` slice (1.00M x 31) | 1.00M x 31 (164 MB) | **bigrquery (JSON API)** | 41.16 s | 4.0 MB/s | 0.02 M/s | 1.0x (baseline) | 20.2x slower |
| `usa_names` slice (1.00M x 5) | 1.00M x 5 (32 MB) | **bigrquerystorage (Arrow API)** | 1.30 s | 24.3 MB/s | 0.77 M/s | **10.0X faster** | 1.0x (baseline) |
| `usa_names` slice (1.00M x 5) | 1.00M x 5 (32 MB) | **duckdb (bigquery ext)** | 5.33 s | 5.9 MB/s | 0.19 M/s | **2.4X faster** | 4.1x slower |
| `usa_names` slice (1.00M x 5) | 1.00M x 5 (32 MB) | **adbc (bigquery driver)** | 4.34 s | 7.3 MB/s | 0.23 M/s | **3.0X faster** | 3.3x slower |
| `usa_names` slice (1.00M x 5) | 1.00M x 5 (32 MB) | **bigrquery (JSON API)** | 12.97 s | 2.4 MB/s | 0.08 M/s | 1.0x (baseline) | 10.0x slower |

#### Benchmark Environment

- **Software & Driver Versions** (3 iterations per workload; see [`scripts/benchmark_bigrquerystorage.R`](https://github.com/meztez/bigrquerystorage/blob/main/scripts/benchmark_bigrquerystorage.R)):
  - **R**: `4.6.0 (2026-04-24)`
  - **`bigrquerystorage`**: `1.3.0.9000` (`nanoarrow 0.9.0`, `gRPC 1.46.7`, `Protobuf 3.14.0`, `libzstd 1.5.1`)
  - **`bigrquery`**: `1.6.2.9000` (`r-dbi/bigrquery#695`, `api = "arrow"` vs `api = "json"`)
  - **`duckdb`**: `2.0.0` + `bigquery` community extension `v0.13.0` (`preserve_insertion_order = false`)
  - **`adbc`**: `adbcdrivermanager 0.25.0` + ADBC Driver Foundry `bigquery` driver `v1.15.0`
- **Hardware & OS Profile**: 2x Intel(R) Xeon(R) Platinum 8375C CPU @ 2.90GHz (64 physical cores / 128 vCPUs), 3.9 TiB RAM, Red Hat Enterprise Linux 9.5 (Plow) (`Linux 5.14.0-503.29.1.el9_5.x86_64`, `x86_64`), BigQuery `US` multi-region.

