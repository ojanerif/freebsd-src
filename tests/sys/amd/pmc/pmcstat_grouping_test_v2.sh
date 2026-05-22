#! /usr/libexec/atf-sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Version 2 of the duplicate AMD core PMC skew regression test.
#
#   v1 (pmcstat_grouping_test.sh) used `sleep N` as the monitored workload.
#   Because sleep is idle-heavy, the absolute cycle count is small (~1-5 M),
#   so a constant ~30 µs timing jitter between the two independent pmc_start()
#   calls produces a large *relative* skew (~50 permille typical, up to ~123
#   permille in the tail), causing ~13% false-failure rate at tolerance=100.
#
#   v2 fixes the two structural root causes:
#
#   1. CPU-bound workload (cpuset -l 0 timeout 5 dd if=/dev/zero of=/dev/null):
#      Replaces the idle sleep with a CPU-saturating dd loop pinned to a single
#      core.  This accumulates 10-100x more cycles (~500 M vs ~2 M), making the
#      same absolute timing jitter negligible in relative terms.
#
#   2. CPU affinity pinning (cpuset -l 0):
#      Pins the monitored process to CPU 0, eliminating inter-core migration and
#      per-CCX frequency-scaling divergence as sources of skew.
#
#   Expected outcome: skew drops from ~50 permille typical / ~123 max to well
#   under 10 permille, allowing a tight tolerance of 50 permille.
#
#   ATF config keys:
#     amd.pmc.grouping.runtime            (bool, default false) — opt-in gate
#     amd.pmc.grouping.cycle_v2_tolerance_permille (int, default 50)

pmcstat_check_support()
{
	if ! kldstat -n hwpmc > /dev/null 2>&1; then
		atf_skip "hwpmc module not loaded (kldload hwpmc)"
	fi
	if ! command -v pmcstat > /dev/null 2>&1; then
		atf_skip "pmcstat not found in PATH"
	fi
}

pmcstat_check_grouping_runtime_enabled()
{
	if [ "$(atf_config_get amd.pmc.grouping.runtime false)" != "true" ]; then
		atf_skip "AMD core grouping runtime disabled by default; set amd.pmc.grouping.runtime=true"
	fi
}

pmcstat_check_amd_cpuid()
{
	local cpuid

	cpuid=$(sysctl -n kern.hwpmc.cpuid 2>/dev/null) || \
	    atf_skip "kern.hwpmc.cpuid is unavailable"
	case "$cpuid" in
	AuthenticAMD-*)
		;;
	*)
		atf_skip "AMD core grouping runtime requires AuthenticAMD CPU, got $cpuid"
		;;
	esac
}

pmcstat_check_amd_grouping_runtime()
{
	pmcstat_check_grouping_runtime_enabled
	pmcstat_check_amd_cpuid
}

pmcstat_v2_cycle_tolerance_permille()
{
	local tolerance

	tolerance=$(atf_config_get amd.pmc.grouping.cycle_v2_tolerance_permille 50)
	case "$tolerance" in
	*[!0-9]*|'')
		atf_fail "invalid amd.pmc.grouping.cycle_v2_tolerance_permille=$tolerance"
		;;
	esac
	printf '%s\n' "$tolerance"
}

pmcstat_event_available()
{
	local event="$1"

	pmcstat -L | awk -v event="$event" '
	    $1 == event { found = 1 }
	    END { exit(found ? 0 : 1) }'
}

pmcstat_require_event()
{
	local event="$1"

	if ! pmcstat_event_available "$event"; then
		atf_skip "pmcstat event $event is unavailable on this CPU"
	fi
}

pmcstat_check_cpuset()
{
	if ! command -v cpuset > /dev/null 2>&1; then
		atf_skip "cpuset not found in PATH; required for CPU affinity pinning in v2"
	fi
	if ! command -v timeout > /dev/null 2>&1; then
		atf_skip "timeout not found in PATH; required for bounded dd workload in v2"
	fi
}

#
# pmcstat_capture_process_two_events_v2 <output> <event_a> <event_b> [duration]
#
# Like pmcstat_capture_process_two_events from v1, but the monitored workload
# is replaced with a CPU-bound dd loop pinned to CPU 0 via cpuset(1).
#
# workload: cpuset -l 0 timeout <duration> dd if=/dev/zero of=/dev/null bs=4096
#
# The dd loop saturates one CPU for the full duration, accumulating 10-100x
# more cycles than an equivalent sleep.  CPU pinning eliminates migration skew.
# dd stderr (progress output) is redirected to /dev/null.
#
pmcstat_capture_process_two_events_v2()
{
	local duration
	local err
	local event_a="$2"
	local event_b="$3"
	local out="$1"

	duration="${4:-5}"

	# The workload runs as: cpuset -l 0 timeout <dur> dd if=/dev/zero ...
	# cpuset must be the direct child of pmcstat so the cpuset constraint
	# is inherited by dd.  dd stderr is silenced to avoid mixing with the
	# ATF output.
	if ! pmcstat -C -q -p "$event_a" -p "$event_b" -o "$out" -- \
	    cpuset -l 0 timeout "$duration" \
	    dd if=/dev/zero of=/dev/null bs=4096 > /dev/null 2>pmcstat.err; then
		err=$(cat pmcstat.err)
		atf_fail "pmcstat v2 failed with $event_a,$event_b: $err"
	fi
	if [ ! -s "$out" ]; then
		atf_fail "pmcstat v2 produced an empty process-counting output file"
	fi
}

pmcstat_require_header_event_count()
{
	local count
	local event="$1"
	local expected="$2"
	local header="$3"

	count=$(printf '%s\n' "$header" | awk -v event="$event" '
	    {
	        for (i = 1; i <= NF; i++) {
	            if ($i == event)
	                n++;
	            else if (length($i) > length(event) &&
	                substr($i, length($i) - length(event) + 1) == event &&
	                substr($i, length($i) - length(event), 1) == "/")
	                n++;
	        }
	    }
	    END { print n + 0 }')
	if [ "$count" -ne "$expected" ]; then
		atf_fail "header has $count copies of $event, expected $expected: $header"
	fi
}

pmcstat_two_counter_stats()
{
	local file="$1"

	awk '
	    !/^#/ && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {
	        rows++;
	        if ($1 > 0)
	            positive_a++;
	        if ($2 > 0)
	            positive_b++;
	        last_a = $1;
	        last_b = $2;
	    }
	    END { print rows + 0, positive_a + 0, positive_b + 0,
	        last_a + 0, last_b + 0 }' "$file"
}

pmcstat_require_bounded_delta()
{
	local a="$1"
	local b="$2"
	local limit_permille="$3"
	local context="$4"
	local delta_report

	delta_report=$(awk -v a="$a" -v b="$b" '
	    BEGIN {
	        d = a - b;
	        if (d < 0)
	            d = -d;
	        m = (a > b) ? a : b;
	        if (m <= 0) {
	            print "invalid";
	            exit 2;
	        }
	        printf "delta=%0.f max=%0.f permille=%.3f", d, m, (d * 1000) / m;
	    }')
	if ! awk -v a="$a" -v b="$b" -v limit="$limit_permille" '
	    BEGIN {
	        d = a - b;
	        if (d < 0)
	            d = -d;
	        m = (a > b) ? a : b;
	        exit(!(m > 0 && d * 1000 <= limit * m));
	    }'; then
		atf_fail "$context counters diverged beyond ${limit_permille}/1000: $delta_report"
	fi
	printf '%s observed bounded skew: %s\n' "$context" "$delta_report"
}

# ---------------------------------------------------------------------------
# Test case: repeated_process_cycles_have_bounded_skew_v2
#
# Identical assertion to v1 (two identical PMCs must agree within tolerance)
# but uses CPU-bound workload + CPU pinning to lower the expected skew from
# ~50 permille to well under 10 permille.
#
# Tolerance default: 50 permille (vs 250 permille in v1).
# ATF config key:    amd.pmc.grouping.cycle_v2_tolerance_permille
# ---------------------------------------------------------------------------

atf_test_case repeated_process_cycles_have_bounded_skew_v2 cleanup
repeated_process_cycles_have_bounded_skew_v2_head()
{
	atf_set "descr" \
	    "v2: duplicate AMD cycle PMCs measured against CPU-bound workload (cpuset+dd) must agree within 50 permille"
	atf_set "require.user" "root"
}
repeated_process_cycles_have_bounded_skew_v2_body()
{
	local cycles_a cycles_b header positive_a positive_b rows stats tolerance

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"
	pmcstat_check_cpuset

	pmcstat_capture_process_two_events_v2 "pmcstat-cycles-skew-v2.out" \
	    "ls_not_halted_cyc" "ls_not_halted_cyc" 5

	header=$(grep '^#' pmcstat-cycles-skew-v2.out | grep 'ls_not_halted_cyc' |
	    head -1)
	if [ -z "$header" ]; then
		atf_fail "pmcstat v2 output is missing the duplicate-cycle header"
	fi
	pmcstat_require_header_event_count "ls_not_halted_cyc" 2 "$header"

	stats=$(pmcstat_two_counter_stats pmcstat-cycles-skew-v2.out)
	set -- $stats
	rows=$1
	positive_a=$2
	positive_b=$3
	cycles_a=$4
	cycles_b=$5

	if [ "$rows" -eq 0 ]; then
		atf_fail "pmcstat v2 produced no numeric rows for duplicate cycle PMCs"
	fi
	if [ "$positive_a" -eq 0 ] || [ "$positive_b" -eq 0 ]; then
		atf_fail "v2 duplicate cycle PMCs did not both produce positive counts"
	fi

	# Sanity: CPU-bound workload should accumulate far more cycles than an
	# idle sleep.  Warn (but do not fail) if counts look suspiciously small
	# — this may indicate dd was not actually running or pinning failed.
	if [ "$cycles_a" -lt 10000000 ]; then
		printf 'WARNING: cycles_a=%d is unexpectedly low for a CPU-bound ' \
		    "$cycles_a"
		printf 'workload; dd may not have run for the full duration\n'
	fi

	tolerance=$(pmcstat_v2_cycle_tolerance_permille)
	pmcstat_require_bounded_delta "$cycles_a" "$cycles_b" "$tolerance" \
	    "v2 duplicate ls_not_halted_cyc (cpuset+dd)"
}
repeated_process_cycles_have_bounded_skew_v2_cleanup()
{
	rm -f pmcstat-cycles-skew-v2.out pmcstat.err
}

atf_init_test_cases()
{
	atf_add_test_case repeated_process_cycles_have_bounded_skew_v2
}
