#!/usr/bin/env Rscript
# ==============================================================================
# Benchmark: bigrquerystorage (bigrquery Arrow API) vs Contenders in R
# ==============================================================================
# Contenders:
#   1. bigrquerystorage (`bigrquery` with `api = "arrow"`, PR r-dbi/bigrquery#695)
#   2. bigrquery (`api = "json"`, REST JSON API without Arrow)
#   3. DuckDB (`duckdb` + `bigquery` community extension via Storage Read/Write API)
#   4. ADBC (`adbcdrivermanager` + `bigquery` ADBC driver)
#
# Metrics:
#   - Download: Time to Tibble (TTT, seconds) & Transfer Speed (MB/s, M rows/s)
#   - Upload:   Time to Table  (TTT, seconds) & Transfer Speed (MB/s, M rows/s)
#
# Public Datasets Used:
#   - `bigquery-public-data.usa_names.usa_1910_current` (6.31M rows x 5 cols)
#   - `bigquery-public-data.samples.gsod` (year >= 2009: 4.58M rows x 31 cols)
#
# Environment Variables (same convention as bigrquerystorage tests/testthat):
#   - BIGQUERY_TEST_PROJECT: GCP project ID used for billing & temporary dataset
#   - GCP_SERVICE_ACCOUNT:   Base64-encoded GCP service account JSON key
#
# Outputs (when run from repository root `Rscript scripts/benchmark_bigrquerystorage.R`):
#   - scripts/benchmark_bigrquerystorage_results.csv
#   - scripts/benchmark_bigrquerystorage_summary.csv
#   - scripts/benchmark_bigrquerystorage_readme.md
#   - man/figures/benchmark_bigrquerystorage.png
# ==============================================================================

suppressPackageStartupMessages({
  library(bigrquery)
  library(bigrquerystorage)
  library(DBI)
  library(duckdb)
  library(adbcdrivermanager)
  library(tibble)
  library(dplyr)
})

# ------------------------------------------------------------------------------
# 1. Authentication & Environment Setup (following bigrquerystorage testthat)
# ------------------------------------------------------------------------------
ca_bundle <- "/etc/pki/tls/certs/ca-bundle.crt"
if (file.exists(ca_bundle) && !nzchar(Sys.getenv("GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"))) {
  Sys.setenv(GRPC_DEFAULT_SSL_ROOTS_FILE_PATH = ca_bundle)
}

project_id <- Sys.getenv("BIGQUERY_TEST_PROJECT")
gcp_sa_b64 <- Sys.getenv("GCP_SERVICE_ACCOUNT")

if (!nzchar(project_id) || !nzchar(gcp_sa_b64)) {
  stop(
    "Missing required environment variables `BIGQUERY_TEST_PROJECT` and/or ",
    "`GCP_SERVICE_ACCOUNT` in `.Renviron` or environment.",
    call. = FALSE
  )
}

sa_json_path <- tempfile(pattern = "gcp_sa_", fileext = ".json")
on.exit(unlink(sa_json_path, force = TRUE), add = TRUE)
writeBin(base64enc::base64decode(gcp_sa_b64), sa_json_path)
Sys.setenv(GOOGLE_APPLICATION_CREDENTIALS = sa_json_path)

# Authenticate bigrquery and bigrquerystorage using the service account JSON
bigrquery::bq_auth(path = sa_json_path)
bigrquerystorage::bqs_auth()

cat("======================================================================\n")
cat("BigQuery R Benchmark Suite — bigrquerystorage vs Contenders\n")
cat("Billing / Test Project:", project_id, "\n")
cat("======================================================================\n\n")

# Create an isolated temporary dataset in US multi-region for benchmark tables
bench_dataset_name <- sprintf("bqs_benchmark_%s", format(Sys.time(), "%Y%m%d_%H%M%S"))
bench_ds <- bigrquery::bq_dataset(project_id, bench_dataset_name)
bigrquery::bq_dataset_create(bench_ds, location = "US")
on.exit(
  try(bigrquery::bq_dataset_delete(bench_ds, delete_contents = TRUE), silent = TRUE),
  add = TRUE
)

# ------------------------------------------------------------------------------
# 2. Initialize Persistent DuckDB and ADBC Connections
# ------------------------------------------------------------------------------
con_duck <- DBI::dbConnect(
  duckdb::duckdb(config = list(allow_unsigned_extensions = "true"))
)
on.exit(try(DBI::dbDisconnect(con_duck, shutdown = TRUE), silent = TRUE), add = TRUE)
invisible(DBI::dbExecute(con_duck, "LOAD bigquery;"))
invisible(DBI::dbExecute(con_duck, "SET preserve_insertion_order = false;"))
invisible(DBI::dbExecute(
  con_duck,
  sprintf("ATTACH 'project=%s dataset=%s' AS bq (TYPE bigquery);", project_id, bench_dataset_name)
))

drv_adbc <- adbcdrivermanager::adbc_driver("bigquery")
db_adbc <- adbcdrivermanager::adbc_database_init(
  drv_adbc,
  `bigquery.project_id` = project_id,
  `bigquery.dataset_id` = bench_dataset_name,
  `bigquery.auth_type` = "json_credential_file",
  `bigquery.auth.credentials` = sa_json_path
)
con_adbc <- adbcdrivermanager::adbc_connection_init(db_adbc)
on.exit({
  try(adbcdrivermanager::adbc_connection_release(con_adbc), silent = TRUE)
  try(adbcdrivermanager::adbc_database_release(db_adbc), silent = TRUE)
}, add = TRUE)

# ------------------------------------------------------------------------------
# 3. Prepare Public Dataset Benchmark Tables
# ------------------------------------------------------------------------------
cat("[1/4] Staging public dataset tables in", bench_dataset_name, "...\n")

# Download Benchmark 1: USA Names (6.31M rows x 5 cols)
tb_usa_dl <- bigrquery::bq_table(bench_ds, "dl_usa_names")
bigrquery::bq_project_query(
  project_id,
  "SELECT * FROM `bigquery-public-data.usa_names.usa_1910_current`",
  destination_table = tb_usa_dl,
  quiet = TRUE
)

# Download Benchmark 2: GSOD Weather (year >= 2009: 4.58M rows x 18 cols)
tb_gsod_dl <- bigrquery::bq_table(bench_ds, "dl_gsod_2009_2010")
bigrquery::bq_project_query(
  project_id,
  "SELECT * FROM `bigquery-public-data.samples.gsod` WHERE year >= 2009",
  destination_table = tb_gsod_dl,
  quiet = TRUE
)

# Prepare in-memory R tibbles from public datasets for Upload Benchmarks
cat("[2/4] Preparing in-memory public dataset tibbles for upload benchmarks...\n")
df_upload_usa <- bigrquery::bq_table_download(
  tb_usa_dl,
  n_max = 1000000L,
  api = "arrow",
  billing = project_id,
  quiet = TRUE
)

df_upload_gsod <- bigrquery::bq_table_download(
  tb_gsod_dl,
  n_max = 1000000L,
  api = "arrow",
  billing = project_id,
  quiet = TRUE
)

# Helper to measure elapsed wall-clock seconds cleanly
time_op <- function(expr) {
  gc(verbose = FALSE)
  t0 <- proc.time()[["elapsed"]]
  force(expr)
  proc.time()[["elapsed"]] - t0
}

n_iterations <- as.integer(Sys.getenv("BQS_BENCH_ITERATIONS", "3"))
results_list <- list()

# ------------------------------------------------------------------------------
# 4. Run Download Benchmarks (Time to Tibble - TTT)
# ------------------------------------------------------------------------------
cat(sprintf("[3/4] Running Download (Time to Tibble) benchmarks (%d iterations)...\n", n_iterations))

download_workloads <- list(
  list(
    workload_id = "dl_usa_names_6.3m",
    dataset_label = "`usa_names.usa_1910_current` (6.31M x 5)",
    bq_table = tb_usa_dl,
    full_table_id = sprintf("%s.%s.dl_usa_names", project_id, bench_dataset_name)
  ),
  list(
    workload_id = "dl_gsod_4.6m",
    dataset_label = "`samples.gsod` `year >= 2009` (4.58M x 31)",
    bq_table = tb_gsod_dl,
    full_table_id = sprintf("%s.%s.dl_gsod_2009_2010", project_id, bench_dataset_name)
  )
)

for (wl in download_workloads) {
  cat("\n  Workload (Download):", wl$dataset_label, "\n")

  # Reference download to get exact dimensions and in-memory tibble size
  ref_df <- bigrquery::bq_table_download(wl$bq_table, api = "arrow", billing = project_id, quiet = TRUE)
  n_rows <- nrow(ref_df)
  n_cols <- ncol(ref_df)
  mem_mb <- as.numeric(utils::object.size(ref_df)) / (1024^2)
  rm(ref_df)
  gc(verbose = FALSE)

  methods_dl <- list(
    "bigrquerystorage (Arrow API)" = function() {
      res <- bigrquery::bq_table_download(
        wl$bq_table,
        api = "arrow",
        billing = project_id,
        quiet = TRUE
      )
      stopifnot(nrow(res) == n_rows)
      invisible(NULL)
    },
    "bigrquery (JSON API)" = function() {
      res <- bigrquery::bq_table_download(
        wl$bq_table,
        api = "json",
        billing = project_id,
        quiet = TRUE
      )
      stopifnot(nrow(res) == n_rows)
      invisible(NULL)
    },
    "duckdb (bigquery ext)" = function() {
      sql <- sprintf(
        "SELECT * FROM bigquery_scan('%s', billing_project = '%s')",
        wl$full_table_id, project_id
      )
      res <- tibble::as_tibble(DBI::dbGetQuery(con_duck, sql))
      stopifnot(nrow(res) == n_rows)
      invisible(NULL)
    },
    "adbc (bigquery driver)" = function() {
      sql <- sprintf("SELECT * FROM `%s`", wl$full_table_id)
      stream <- adbcdrivermanager::read_adbc(con_adbc, sql)
      res <- tibble::as_tibble(stream)
      stopifnot(nrow(res) == n_rows)
      invisible(NULL)
    }
  )

  for (m_name in names(methods_dl)) {
    for (iter in seq_len(n_iterations)) {
      elapsed_s <- time_op(methods_dl[[m_name]]())
      cat(sprintf("    - %-30s [iter %d/%d]: %6.2f s (%6.1f MB/s)\n",
                  m_name, iter, n_iterations, elapsed_s, mem_mb / elapsed_s))
      results_list[[length(results_list) + 1L]] <- tibble::tibble(
        operation = "Download (Time to Tibble)",
        workload_id = wl$workload_id,
        dataset = wl$dataset_label,
        rows = n_rows,
        cols = n_cols,
        size_mb = round(mem_mb, 2),
        method = m_name,
        iteration = iter,
        ttt_seconds = round(elapsed_s, 3),
        mb_per_sec = round(mem_mb / elapsed_s, 2),
        mrows_per_sec = round((n_rows / 1e6) / elapsed_s, 3)
      )
    }
  }
}

# ------------------------------------------------------------------------------
# 5. Run Upload Benchmarks (Time to Table - TTT)
# ------------------------------------------------------------------------------
cat(sprintf("\n[4/4] Running Upload (Time to Table) benchmarks (%d iterations)...\n", n_iterations))

upload_workloads <- list(
  list(
    workload_id = "ul_usa_names_1m",
    dataset_label = "`usa_names` slice (1.00M x 5)",
    data = df_upload_usa
  ),
  list(
    workload_id = "ul_gsod_1m",
    dataset_label = "`samples.gsod` slice (1.00M x 31)",
    data = df_upload_gsod
  )
)

for (wl in upload_workloads) {
  cat("\n  Workload (Upload):", wl$dataset_label, "\n")
  df_in <- wl$data
  n_rows <- nrow(df_in)
  n_cols <- ncol(df_in)
  mem_mb <- as.numeric(utils::object.size(df_in)) / (1024^2)

  for (iter in seq_len(n_iterations)) {
    suffix <- sprintf("%s_i%d_%d", wl$workload_id, iter, sample(1000:9999, 1))

    # 1. bigrquerystorage (Arrow API via bigrquery PR #695)
    tb_arrow <- bigrquery::bq_table(bench_ds, paste0("up_arrow_", suffix))
    t_arrow <- time_op(
      bigrquery::bq_table_upload(tb_arrow, values = df_in, api = "arrow", quiet = TRUE)
    )
    cat(sprintf("    - %-30s [iter %d/%d]: %6.2f s (%6.1f MB/s)\n",
                "bigrquerystorage (Arrow API)", iter, n_iterations, t_arrow, mem_mb / t_arrow))
    results_list[[length(results_list) + 1L]] <- tibble::tibble(
      operation = "Upload (Time to Table)",
      workload_id = wl$workload_id,
      dataset = wl$dataset_label,
      rows = n_rows,
      cols = n_cols,
      size_mb = round(mem_mb, 2),
      method = "bigrquerystorage (Arrow API)",
      iteration = iter,
      ttt_seconds = round(t_arrow, 3),
      mb_per_sec = round(mem_mb / t_arrow, 2),
      mrows_per_sec = round((n_rows / 1e6) / t_arrow, 3)
    )

    # 2. bigrquery (JSON API without Arrow)
    tb_json <- bigrquery::bq_table(bench_ds, paste0("up_json_", suffix))
    t_json <- time_op(
      bigrquery::bq_table_upload(tb_json, values = df_in, api = "json", quiet = TRUE)
    )
    cat(sprintf("    - %-30s [iter %d/%d]: %6.2f s (%6.1f MB/s)\n",
                "bigrquery (JSON API)", iter, n_iterations, t_json, mem_mb / t_json))
    results_list[[length(results_list) + 1L]] <- tibble::tibble(
      operation = "Upload (Time to Table)",
      workload_id = wl$workload_id,
      dataset = wl$dataset_label,
      rows = n_rows,
      cols = n_cols,
      size_mb = round(mem_mb, 2),
      method = "bigrquery (JSON API)",
      iteration = iter,
      ttt_seconds = round(t_json, 3),
      mb_per_sec = round(mem_mb / t_json, 2),
      mrows_per_sec = round((n_rows / 1e6) / t_json, 3)
    )

    # 3. DuckDB (bigquery extension)
    tbl_duck_name <- paste0("up_duck_", suffix)
    t_duck <- time_op({
      duckdb::duckdb_register(con_duck, "df_bench_up", df_in)
      DBI::dbExecute(
        con_duck,
        sprintf("CREATE OR REPLACE TABLE bq.%s.%s AS SELECT * FROM df_bench_up;",
                bench_dataset_name, tbl_duck_name)
      )
      duckdb::duckdb_unregister(con_duck, "df_bench_up")
    })
    cat(sprintf("    - %-30s [iter %d/%d]: %6.2f s (%6.1f MB/s)\n",
                "duckdb (bigquery ext)", iter, n_iterations, t_duck, mem_mb / t_duck))
    results_list[[length(results_list) + 1L]] <- tibble::tibble(
      operation = "Upload (Time to Table)",
      workload_id = wl$workload_id,
      dataset = wl$dataset_label,
      rows = n_rows,
      cols = n_cols,
      size_mb = round(mem_mb, 2),
      method = "duckdb (bigquery ext)",
      iteration = iter,
      ttt_seconds = round(t_duck, 3),
      mb_per_sec = round(mem_mb / t_duck, 2),
      mrows_per_sec = round((n_rows / 1e6) / t_duck, 3)
    )

    # 4. ADBC (bigquery driver)
    tbl_adbc_name <- paste0("up_adbc_", suffix)
    t_adbc <- time_op(
      adbcdrivermanager::write_adbc(df_in, con_adbc, tbl_adbc_name, mode = "create")
    )
    cat(sprintf("    - %-30s [iter %d/%d]: %6.2f s (%6.1f MB/s)\n",
                "adbc (bigquery driver)", iter, n_iterations, t_adbc, mem_mb / t_adbc))
    results_list[[length(results_list) + 1L]] <- tibble::tibble(
      operation = "Upload (Time to Table)",
      workload_id = wl$workload_id,
      dataset = wl$dataset_label,
      rows = n_rows,
      cols = n_cols,
      size_mb = round(mem_mb, 2),
      method = "adbc (bigquery driver)",
      iteration = iter,
      ttt_seconds = round(t_adbc, 3),
      mb_per_sec = round(mem_mb / t_adbc, 2),
      mrows_per_sec = round((n_rows / 1e6) / t_adbc, 3)
    )
  }
}

# ------------------------------------------------------------------------------
# 6. Aggregate Results & Compute {0-9}X Relative Speedups
# ------------------------------------------------------------------------------
raw_results <- dplyr::bind_rows(results_list)

summary_results <- raw_results |>
  dplyr::group_by(operation, workload_id, dataset, rows, cols, size_mb, method) |>
  dplyr::summarise(
    median_ttt_s = round(stats::median(ttt_seconds), 2),
    mean_ttt_s = round(mean(ttt_seconds), 2),
    min_ttt_s = round(min(ttt_seconds), 2),
    max_ttt_s = round(max(ttt_seconds), 2),
    median_mb_s = round(stats::median(mb_per_sec), 1),
    median_mrows_s = round(stats::median(mrows_per_sec), 2),
    .groups = "drop"
  ) |>
  dplyr::group_by(operation, workload_id) |>
  dplyr::mutate(
    bqs_ttt_s = median_ttt_s[method == "bigrquerystorage (Arrow API)"][1],
    json_ttt_s = median_ttt_s[method == "bigrquery (JSON API)"][1],
    vs_bigquery_json = round(json_ttt_s / median_ttt_s, 1),
    vs_bigrquerystorage = round(median_ttt_s / bqs_ttt_s, 1),
    relative_to_bqs_label = dplyr::case_when(
      method == "bigrquerystorage (Arrow API)" ~ "1.0x (baseline)",
      median_ttt_s >= bqs_ttt_s ~ sprintf("%.1fx slower", median_ttt_s / bqs_ttt_s),
      TRUE ~ sprintf("%.1fx faster", bqs_ttt_s / median_ttt_s)
    )
  ) |>
  dplyr::ungroup()

# Order methods consistently
method_levels <- c(
  "bigrquerystorage (Arrow API)",
  "duckdb (bigquery ext)",
  "adbc (bigquery driver)",
  "bigrquery (JSON API)"
)
summary_results <- summary_results |>
  dplyr::mutate(method = factor(method, levels = method_levels)) |>
  dplyr::arrange(operation, workload_id, method) |>
  dplyr::mutate(method = as.character(method))

# Save CSV outputs in scripts/ (or current working directory if run inside scripts/)
out_dir <- if (dir.exists("scripts")) "scripts" else "."
fig_dir <- if (dir.exists("man")) file.path("man", "figures") else out_dir
dir.create(fig_dir, recursive = TRUE, showWarnings = FALSE)

utils::write.csv(raw_results, file.path(out_dir, "benchmark_bigrquerystorage_results.csv"), row.names = FALSE)
utils::write.csv(summary_results, file.path(out_dir, "benchmark_bigrquerystorage_summary.csv"), row.names = FALSE)

# ------------------------------------------------------------------------------
# 7. Build Templated Summary Sentence & README-Ready Markdown Output
# ------------------------------------------------------------------------------
dl_summary <- summary_results |> dplyr::filter(operation == "Download (Time to Tibble)")
ul_summary <- summary_results |> dplyr::filter(operation == "Upload (Time to Table)")

dl_vs_json_min <- min(round(dl_summary$json_ttt_s / dl_summary$bqs_ttt_s, 0))
dl_vs_json_max <- max(round(dl_summary$json_ttt_s / dl_summary$bqs_ttt_s, 0))
ul_vs_json_min <- min(round(ul_summary$json_ttt_s / ul_summary$bqs_ttt_s, 0))
ul_vs_json_max <- max(round(ul_summary$json_ttt_s / ul_summary$bqs_ttt_s, 0))

format_bqs_speed_phrase <- function(df_sub, target_method) {
  ratios <- df_sub |>
    dplyr::filter(method == target_method) |>
    dplyr::transmute(ratio = median_ttt_s / bqs_ttt_s) |>
    dplyr::pull(ratio)
  if (all(ratios >= 1)) {
    r <- round(range(ratios), 1)
    if (abs(r[1] - r[2]) < 0.15) {
      sprintf("%.1fX times faster", r[1])
    } else {
      sprintf("%.1fX to %.1fX times faster", r[1], r[2])
    }
  } else if (all(ratios < 1)) {
    inv <- round(range(1 / ratios), 1)
    if (abs(inv[1] - inv[2]) < 0.15) {
      sprintf("%.1fX times slower", inv[1])
    } else {
      sprintf("%.1fX to %.1fX times slower", inv[1], inv[2])
    }
  } else {
    slower_x <- round(1 / min(ratios), 1)
    faster_x <- round(max(ratios), 1)
    sprintf("%.1fX times slower to %.1fX times faster", slower_x, faster_x)
  }
}

templated_sentence <- sprintf(
  paste0(
    "In R benchmarks on BigQuery public datasets, **`bigrquerystorage`** (`bigrquery` with `api = \"arrow\"`) is ",
    "**%dX to %dX times faster** on download (*Time to Tibble*) and **%dX to %dX times faster** on upload (*Time to Table*) ",
    "than `bigrquery` without the Arrow API (`api = \"json\"`), while performing **%s** on download ",
    "(and **%s** on upload) than `duckdb` (`bigquery` extension), and **%s** on download ",
    "(and **%s** on upload) than the `adbc` BigQuery driver."
  ),
  dl_vs_json_min, dl_vs_json_max,
  ul_vs_json_min, ul_vs_json_max,
  format_bqs_speed_phrase(dl_summary, "duckdb (bigquery ext)"),
  format_bqs_speed_phrase(ul_summary, "duckdb (bigquery ext)"),
  format_bqs_speed_phrase(dl_summary, "adbc (bigquery driver)"),
  format_bqs_speed_phrase(ul_summary, "adbc (bigquery driver)")
)

# Format Markdown table rows
make_md_table <- function(df) {
  header <- c(
    "| Dataset | Dimensions (In-Memory) | Method | Median TTT (s) | Transfer Speed (MB/s) | Throughput (M rows/s) | vs `bigrquery` (JSON) | vs `bigrquerystorage` (Arrow) |",
    "|:---|:---|:---|---:|---:|---:|---:|:---|"
  )
  rows <- vapply(seq_len(nrow(df)), function(i) {
    r <- df[i, ]
    vs_json_label <- if (r$method == "bigrquery (JSON API)") {
      "1.0x (baseline)"
    } else {
      sprintf("**%.1fX faster**", r$vs_bigquery_json)
    }
    sprintf(
      "| %s | %sM x %d (%.0f MB) | **%s** | %.2f s | %.1f MB/s | %.2f M/s | %s | %s |",
      r$dataset,
      format(round(r$rows / 1e6, 2), nsmall = 2),
      r$cols,
      r$size_mb,
      r$method,
      r$median_ttt_s,
      r$median_mb_s,
      r$median_mrows_s,
      vs_json_label,
      r$relative_to_bqs_label
    )
  }, character(1))
  paste(c(header, rows), collapse = "\n")
}

readme_md <- paste0(
  "### Performance (`bigrquerystorage` Arrow API vs `bigrquery` JSON, `duckdb`, and `adbc`)\n\n",
  templated_sentence, "\n\n",
  "![BigQuery R Benchmarks: Time to Tibble (Download) & Time to Table (Upload)](man/figures/benchmark_bigrquerystorage.png)\n\n",
  "#### Download Benchmark — Time to Tibble (TTT)\n\n",
  make_md_table(dl_summary), "\n\n",
  "#### Upload Benchmark — Time to Table (TTT)\n\n",
  make_md_table(ul_summary), "\n\n",
  "*Benchmarks executed in R (", R.version.string, ") over ", n_iterations,
  " iterations per workload using `r-dbi/bigrquery#695` (`api = \"arrow\"` vs `api = \"json\"`), ",
  "`duckdb` (`bigquery` extension), and `adbcdrivermanager` (`bigquery` driver); see ",
  "[`scripts/benchmark_bigrquerystorage.R`](https://github.com/meztez/bigrquerystorage/blob/main/scripts/benchmark_bigrquerystorage.R).*\n"
)

writeLines(readme_md, file.path(out_dir, "benchmark_bigrquerystorage_readme.md"))

# Optional: Generate clean visual chart if ggplot2 is installed
if (requireNamespace("ggplot2", quietly = TRUE)) {
  library(ggplot2)
  plot_df <- summary_results |>
    dplyr::mutate(
      workload_title = paste0(operation, "\n", gsub("`", "", dataset)),
      method = factor(method, levels = rev(method_levels))
    )

  p <- ggplot(plot_df, aes(x = method, y = median_ttt_s, fill = method)) +
    geom_col(width = 0.65) +
    geom_text(
      aes(label = sprintf("%.2fs (%s)", median_ttt_s, relative_to_bqs_label)),
      hjust = -0.05,
      size = 3.3
    ) +
    coord_flip() +
    facet_wrap(~workload_title, scales = "free_x", ncol = 2) +
    scale_y_continuous(expand = expansion(mult = c(0, 0.38))) +
    scale_fill_manual(values = c(
      "bigrquerystorage (Arrow API)" = "#1b9e77",
      "duckdb (bigquery ext)" = "#7570b3",
      "adbc (bigquery driver)" = "#e7298a",
      "bigrquery (JSON API)" = "#d95f02"
    )) +
    labs(
      title = "BigQuery R Benchmarks: Time to Tibble (Download) & Time to Table (Upload)",
      subtitle = "Lower is better (median wall-clock seconds across iterations)",
      x = NULL,
      y = "Median Time (seconds)",
      fill = "Method"
    ) +
    theme_minimal(base_size = 11) +
    theme(
      legend.position = "none",
      strip.text = element_text(face = "bold", size = 10),
      panel.grid.minor = element_blank()
    )

  ggplot2::ggsave(
    file.path(fig_dir, "benchmark_bigrquerystorage.png"),
    p,
    width = 12,
    height = 7,
    dpi = 150
  )
}

cat("\n======================================================================\n")
cat("README Snippet Generated (benchmark_bigrquerystorage_readme.md):\n")
cat("======================================================================\n\n")
cat(readme_md)
