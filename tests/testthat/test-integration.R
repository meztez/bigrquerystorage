test_that("Test different method to see if they return correctly", {

	skip_if_not(getOption("bigquerystorage.project", FALSE))

	con <- bigrquery::dbConnect(
		bigrquery::bigquery(),
		project = "bigquery-public-data",
		dataset = "usa_names",
		billing = getOption("bigquerystorage.project"))

	# Basic reading table as data.frame
	dt <- DBI::dbGetQuery(con, "SELECT * FROM `bigquery-public-data.usa_names.usa_1910_current` LIMIT 50000")
	expect_true(inherits(dt, "data.frame"))
	expect_true(nrow(dt) == 50000)

	# Full table fetch
	dt <- DBI::dbReadTable(con, "bigquery-public-data.usa_names.usa_1910_current", quiet = TRUE, )
	expect_true(inherits(dt, "data.frame"))

	# Compare with bigrquery method
	dt <- bqs_table_download("bigquery-public-data.usa_names.usa_1910_current", max_results = 50000)
	## IPC stream cannot be cut to exact size
	dt <- as.data.frame(dt)[1:50000,]
	dt2 <- bigrquery::bq_table_download("bigquery-public-data.usa_names.usa_1910_current", max_results = 50000)
	expect_true(all.equal(dt, dt2))

	# Check if a 0 rows table can be returned
	dt <- DBI::dbGetQuery(con, "SELECT * FROM `bigquery-public-data.usa_names.usa_1910_current` LIMIT 0")
	expect_true(inherits(dt, "data.frame"))
	expect_true(nrow(dt) == 0)

	# Check other features
	dt <- bqs_table_download("bigquery-public-data:usa_names.usa_1910_current",
													 selected_fields = c("name", "number", "state"),
													 row_restriction = 'state = "WA"')
	expect_length(names(dt), 3)
	expect_identical(as.character(unique(dt$state)), "WA")
})