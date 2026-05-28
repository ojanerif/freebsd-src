#!/bin/sh
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause
#
# generate-report.sh — Parse JUnit XML and write a Markdown summary
# to $GITHUB_STEP_SUMMARY.

set -eu

RESULTS_XML="${RESULTS_XML:-ibs-results.xml}"
BUILD_STATUS="${BUILD_STATUS:-unknown}"
TEST_STATUS="${TEST_STATUS:-unknown}"

build_icon() {
	case "$1" in
		success) printf '✅' ;;
		failure) printf '❌' ;;
		skipped) printf '⏭️' ;;
		*)       printf '❓' ;;
	esac
}

test_icon() {
	case "$1" in
		passed)  printf '✅' ;;
		failed)  printf '❌' ;;
		error)   printf '💥' ;;
		skipped) printf '⏭️' ;;
		*)       printf '❓' ;;
	esac
}

{
	printf '## AMD IBS CI Results\n\n'
	printf '| Stage | Status |\n'
	printf '|-------|--------|\n'
	printf '| Build | %s %s |\n' "$(build_icon "$BUILD_STATUS")" "$BUILD_STATUS"
	printf '| Tests | %s %s |\n' "$(test_icon  "$TEST_STATUS")"  "$TEST_STATUS"
	printf '\n'
} >> "$GITHUB_STEP_SUMMARY"

# Parse JUnit XML if present
if [ -f "$RESULTS_XML" ]; then
	# Extract counts using basic sh-compatible XML parsing
	# <testsuite tests="N" failures="N" errors="N" skipped="N">
	total=$(   grep -o 'tests="[0-9]*"'    "$RESULTS_XML" | head -1 | grep -o '[0-9]*' || echo 0)
	failures=$(grep -o 'failures="[0-9]*"' "$RESULTS_XML" | head -1 | grep -o '[0-9]*' || echo 0)
	errors=$(  grep -o 'errors="[0-9]*"'   "$RESULTS_XML" | head -1 | grep -o '[0-9]*' || echo 0)
	skipped=$( grep -o 'skipped="[0-9]*"'  "$RESULTS_XML" | head -1 | grep -o '[0-9]*' || echo 0)
	passed=$(( total - failures - errors - skipped ))
	pmcgroupstart_bridge_id=""
	if grep -q 'group_start_atomicity_EXPECTED_FAIL' "$RESULTS_XML"; then
		pmcgroupstart_bridge_id=$(awk '
		function attr(s, key,   pat, v) {
			pat = "(^|[[:space:]])" key "=\"[^\"]*\""
			if (match(s, pat)) {
				v = substr(s, RSTART, RLENGTH)
				sub("^[^=]*=\"", "", v)
				sub("\"$", "", v)
				return v
			}
			return ""
		}
		BEGIN {
			in_tc = 0
			target = 0
			xfail_marker = 0
			tc_class = ""
			tc_name = ""
		}
		/<testcase[[:space:]>]/ {
			in_tc = 1
			target = 0
			xfail_marker = 0
			tc_class = attr($0, "classname")
			tc_name = attr($0, "name")
			if (tc_name ~ /^group_start_atomicity_EXPECTED_FAIL($|_)/ &&
			    tc_class ~ /(^|[.\/])hwpmc_grouping_test$/)
				target = 1
		}
		in_tc && target && (/Expected failure result details/ || /PMCGROUPSTART/) {
			xfail_marker = 1
		}
		in_tc && /<\/testcase>/ {
			if (target && xfail_marker) {
				if (tc_class != "")
					print tc_class ":" tc_name
				else
					print "hwpmc_grouping_test:" tc_name
				exit 0
			}
			in_tc = 0
			target = 0
			xfail_marker = 0
			tc_class = ""
			tc_name = ""
		}
		' "$RESULTS_XML")
	fi

	{
		printf '### Test Counts\n\n'
		printf '| Result | Count |\n'
		printf '|--------|-------|\n'
		printf '| ✅ Passed  | %s |\n' "$passed"
		printf '| ❌ Failed  | %s |\n' "$failures"
		printf '| 💥 Error   | %s |\n' "$errors"
		printf '| ⏭️ Skipped | %s |\n' "$skipped"
		printf '| **Total**  | **%s** |\n' "$total"
		printf '\n'
	} >> "$GITHUB_STEP_SUMMARY"

	if [ -n "$pmcgroupstart_bridge_id" ]; then
		{
			printf '### Expected-Failure Bridge Notes\n\n'
			printf '%s%s\n' "- \`$pmcgroupstart_bridge_id\` is an intentional " \
			    'expected failure documenting missing FreeBSD `PMCGROUPSTART`.'
			printf '%s%s\n' '- Kyua JUnit records `expected_failure` details ' \
			    'in the testcase `system-err` text.'
			printf '%s\n' '- The note keeps the bridge visible without changing CI pass/fail status.'
			printf '%s%s\n' '- Action when this changes: inspect whether a real ' \
			    'group-start/global-arm ABI landed.'
			printf '%s\n' '- Otherwise replace the bridge with a measured atomic-arm assertion.'
			printf '\n'
		} >> "$GITHUB_STEP_SUMMARY"
	fi

	# List individual test cases with failures
	if [ "$failures" -gt 0 ] || [ "$errors" -gt 0 ]; then
		{
			printf '### Failed Tests\n\n'
			# Extract <testcase classname="X" name="Y"> followed by <failure
			grep -A3 '<failure' "$RESULTS_XML" | \
				grep -E 'classname|message' | \
				sed 's/.*classname="//;s/".*//' | \
				while IFS= read -r line; do
					printf '- `%s`\n' "$line"
				done || true
			printf '\n'
		} >> "$GITHUB_STEP_SUMMARY"
	fi
else
	printf '> No JUnit XML found at `%s`\n\n' "$RESULTS_XML" >> "$GITHUB_STEP_SUMMARY"
fi

printf '%s\n' "---" >> "$GITHUB_STEP_SUMMARY"
printf '*Runner: %s | FreeBSD %s*\n' \
	"$(hostname)" "$(uname -r)" >> "$GITHUB_STEP_SUMMARY"
