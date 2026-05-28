#!/bin/sh
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause
#
# run-tests.sh — Load hwpmc.ko, run Kyua test suite, emit JUnit XML + HTML.
#                Retries failing tests up to 3 total attempts (§8.3 flaky
#                test detection).
#
# Environment variables:
#   TESTS_DIR        path to directory containing Kyuafile
#   TIMEOUT_MINUTES  max minutes for the test run (default 15)
#   KYUA_ARGS        extra kyua global arguments before "test" (optional)

set -eu

log_info() { printf '[run-atf-tests] INFO:  %s\n' "$*"; }
log_err()  { printf '[run-atf-tests] ERROR: %s\n' "$*" >&2; }

TESTS_DIR="${TESTS_DIR:?TESTS_DIR not set}"
TIMEOUT_MINUTES="${TIMEOUT_MINUTES:-15}"
KYUA_ARGS="${KYUA_ARGS:-}"
RESULTS_XML="ibs-results.xml"
KYUADB="kyua.db"
MAX_ATTEMPTS=3

if [ ! -f "${TESTS_DIR}/Kyuafile" ]; then
	log_err "Kyuafile not found in: $TESTS_DIR"
	printf 'test_status=error\n'             >> "$GITHUB_OUTPUT"
	printf 'results_path=%s\n' "$RESULTS_XML" >> "$GITHUB_OUTPUT"
	exit 1
fi

# ---------------------------------------------------------------------------
# Load hwpmc module (requires sudo, configured in sudoers)
# ---------------------------------------------------------------------------
log_info "Loading hwpmc kernel module..."
if kldstat -n hwpmc 2>/dev/null; then
	log_info "hwpmc already loaded"
else
	sudo kldload hwpmc || { log_err "Failed to load hwpmc.ko"; exit 1; }
fi

hwpmc_loaded=1
cleanup() {
	if [ "${hwpmc_loaded:-0}" -eq 1 ]; then
		log_info "Unloading hwpmc..."
		sudo kldunload hwpmc 2>/dev/null || true
	fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Attempt 1 — run full suite
# ---------------------------------------------------------------------------
log_info "Attempt 1/${MAX_ATTEMPTS}: running full suite in $TESTS_DIR (timeout: ${TIMEOUT_MINUTES}m)"
mkdir -p kyua-report

kyua_exit=0
timeout "${TIMEOUT_MINUTES}m" \
	kyua ${KYUA_ARGS} test \
		--kyuafile "${TESTS_DIR}/Kyuafile" \
		--results-file "$KYUADB" \
	|| kyua_exit=$?

if [ "$kyua_exit" -eq 124 ]; then
	log_err "Test run timed out after ${TIMEOUT_MINUTES} minutes"
	test_status="error"
	printf 'test_status=%s\n'   "$test_status" >> "$GITHUB_OUTPUT"
	printf 'results_path=%s\n'  "$RESULTS_XML"  >> "$GITHUB_OUTPUT"
	kyua report-junit --results-file "$KYUADB" --output "$RESULTS_XML" 2>/dev/null || true
	kyua report-html  --results-file "$KYUADB" --output kyua-report   2>/dev/null || true
	exit 0
elif [ "$kyua_exit" -ne 0 ]; then
	log_info "Some tests failed in attempt 1 (kyua exit: $kyua_exit)"
	test_status="failed"
else
	log_info "All tests passed on attempt 1"
	test_status="passed"
fi

# ---------------------------------------------------------------------------
# Retry loop (attempts 2 and 3) — only for failing/broken tests
# ---------------------------------------------------------------------------
flaky_tests=""
truly_failed=""

if [ "$test_status" = "failed" ]; then
	# Extract failing test IDs from the kyua database.
	# Output format: "  test_name:case_name  ->  failed: ..."
	# We extract just "test_name:case_name".
	failing=$(kyua report --results-file "$KYUADB" --results-filter failed,broken 2>/dev/null | \
		grep ' -> ' | sed 's/^[[:space:]]*//; s/[[:space:]]*->.*$//' || true)

	if [ -z "$failing" ]; then
		# kyua exited non-zero but report shows no failures (e.g., broken env)
		log_info "No individual test failures found in report; treating as error"
		test_status="error"
	else
		attempt=2
		remaining="$failing"
		while [ "$attempt" -le "$MAX_ATTEMPTS" ] && [ -n "$remaining" ]; do
			log_info "Retry attempt ${attempt}/${MAX_ATTEMPTS} for failing test(s):"
			echo "$remaining" | while IFS= read -r tc; do
				[ -n "$tc" ] && log_info "  retrying: $tc"
			done

			RETRY_STILL_FAILING=""
			while IFS= read -r tc; do
				[ -z "$tc" ] && continue
				retry_db="kyua-retry${attempt}-$(echo "$tc" | tr ':/' '--').db"
				retry_exit=0
				timeout "${TIMEOUT_MINUTES}m" \
					kyua ${KYUA_ARGS} test \
						--kyuafile "${TESTS_DIR}/Kyuafile" \
						--results-file "$retry_db" \
						"$tc" \
					2>/dev/null || retry_exit=$?

				if [ "$retry_exit" -eq 0 ]; then
					log_info "FLAKY (passed on attempt ${attempt}): $tc"
					flaky_tests="${flaky_tests}${tc}
"
				else
					RETRY_STILL_FAILING="${RETRY_STILL_FAILING}${tc}
"
				fi
			done <<EOF
$remaining
EOF
			remaining="$RETRY_STILL_FAILING"
			attempt=$((attempt + 1))
		done

		truly_failed="$remaining"
		if [ -n "$truly_failed" ]; then
			test_status="failed"
			log_info "Tests that failed all ${MAX_ATTEMPTS} attempts:"
			echo "$truly_failed" | while IFS= read -r tc; do
				[ -n "$tc" ] && log_err "  FAIL: $tc"
			done
		elif [ -n "$flaky_tests" ]; then
			test_status="passed"
			log_info "All failures recovered via retry — flaky tests detected:"
			echo "$flaky_tests" | while IFS= read -r tc; do
				[ -n "$tc" ] && log_info "  FLAKY: $tc"
			done
		else
			test_status="passed"
		fi
	fi
fi

# ---------------------------------------------------------------------------
# Generate JUnit XML report (from initial run — preserves original results)
# ---------------------------------------------------------------------------
log_info "Generating JUnit XML: $RESULTS_XML"
kyua report-junit \
	--results-file "$KYUADB" \
	--output "$RESULTS_XML" \
	2>/dev/null || true

# ---------------------------------------------------------------------------
# Generate HTML report
# ---------------------------------------------------------------------------
log_info "Generating HTML report: kyua-report/"
kyua report-html \
	--results-file "$KYUADB" \
	--output kyua-report \
	2>/dev/null || true

# ---------------------------------------------------------------------------
# Print summary to log
# ---------------------------------------------------------------------------
log_info "=== Test run summary ==="
kyua report --results-file "$KYUADB" 2>/dev/null || true

if [ -n "$flaky_tests" ]; then
	log_info "=== Flaky tests (passed in retry, failed on attempt 1) ==="
	echo "$flaky_tests" | while IFS= read -r tc; do
		[ -n "$tc" ] && log_info "  FLAKY: $tc"
	done
fi
if [ -n "$truly_failed" ]; then
	log_info "=== Truly failed tests (failed all ${MAX_ATTEMPTS} attempts) ==="
	echo "$truly_failed" | while IFS= read -r tc; do
		[ -n "$tc" ] && log_err "  FAIL: $tc"
	done
fi
log_info "Final test_status: $test_status"

printf 'test_status=%s\n'   "$test_status" >> "$GITHUB_OUTPUT"
printf 'results_path=%s\n'  "$RESULTS_XML"  >> "$GITHUB_OUTPUT"

[ "$test_status" = "passed" ] || [ "$test_status" = "failed" ]
