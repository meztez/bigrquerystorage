### Performance (`bigrquerystorage` Arrow API vs `bigrquery` JSON, `duckdb`, and `adbc`)

In R benchmarks on BigQuery public datasets, **`bigrquerystorage`** (`bigrquery` with `api = "arrow"`) is **23X to 46X times faster** on download (*Time to Tibble*) and **5X to 16X times faster** on upload (*Time to Table*) than `bigrquery` without the Arrow API (`api = "json"`), while performing **1.8X to 2.0X times faster** on download (and **3.0X to 5.1X times faster** on upload) than `duckdb` (`bigquery` extension), and **3.5X to 7.3X times faster** on download (and **1.7X to 2.1X times faster** on upload) than the `adbc` BigQuery driver.

![BigQuery R Benchmarks: Time to Tibble (Download) & Time to Table (Upload)](man/figures/benchmark_bigrquerystorage.png)

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

*Benchmarks executed in R (R version 4.6.0 (2026-04-24)) over 3 iterations per workload using `r-dbi/bigrquery#695` (`api = "arrow"` vs `api = "json"`), `duckdb` (`bigquery` extension), and `adbcdrivermanager` (`bigquery` driver); see [`scripts/benchmark_bigrquerystorage.R`](https://github.com/meztez/bigrquerystorage/blob/main/scripts/benchmark_bigrquerystorage.R).*

