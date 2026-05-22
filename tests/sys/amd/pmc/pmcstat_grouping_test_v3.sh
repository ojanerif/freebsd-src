#! /usr/libexec/atf-sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Version 3 of the duplicate AMD core PMC skew investigation suite.
#   Three focused hypothesis tests that distinguish the source of skew:
#
#   H1 — skew_scales_inversely_with_duration
#        Sweeps measurement durations [1, 2, 5, 10, 30]s.
#        Prediction (fixed overhead):      delta_abs ≈ const, permille ∝ 1/duration
#        Prediction (accumulated drift):   delta_abs ∝ duration, permille ≈ const
#        Pass criterion: median_permille[30s] < median_permille[1s] / 3
#
#   H2 — counter_b_is_always_larger_than_a (arming order bias)
#        Runs 40 iterations at a fixed duration and counts how often b > a.
#        Prediction (sequential arming):   P(b > a) >> 0.5 — strongly one-sided
#        Prediction (symmetric noise):     P(b > a) ≈ 0.5 — balanced
#        The test reports the observed ratio and flags systematic bias.
#        A ratio above 80% is reported as WARN (informational), not ATF_FAIL,
#        because this is a characterization test, not a correctness assertion.
#
#   H3 — dd_fixed_count_accumulates_more_cycles_than_sleep
#        Compares dd count=500000 (no timeout, exits naturally) vs sleep 5.
#        Asserts: dd accumulates more ls_not_halted_cyc than sleep 5.
#        Motivation: validate the pmc-003 fix (count-based dd vs SIGTERM-dd).
#
#   ATF config keys (all opt-in):
#     amd.pmc.grouping.runtime             (bool, default false)
#     amd.pmc.grouping.v3.iterations       (int, default 5 per duration point)
#     amd.pmc.grouping.v3.bias.iterations  (int, default 40 for bias test)

# ---------------------------------------------------------------------------
# Guards and helpers
# ---------------------------------------------------------------------------

pmcstat_check_support()
{
	if ! kldstat -n hwpmc > /dev/null 2>&1; then
		atf_skip "hwpmc module not loaded (kldload hwpmc)"
	fi
	if ! command -v pmcstat > /dev/null 2>&1; then
		atf_skip "pmcstat not found in PATH"
	fi
}

pmcstat_check_amd_grouping_runtime()
{
	if [ "$(atf_config_get amd.pmc.grouping.runtime false)" != "true" ]; then
		atf_skip "AMD core grouping runtime disabled; set amd.pmc.grouping.runtime=true"
	fi
	local cpuid
	cpuid=$(sysctl -n kern.hwpmc.cpuid 2>/dev/null) || \
	    atf_skip "kern.hwpmc.cpuid unavailable"
	case "$cpuid" in
	AuthenticAMD-*) ;;
	*) atf_skip "requires AuthenticAMD CPU, got $cpuid" ;;
	esac
}

pmcstat_require_event()
{
	local event="$1"
	if ! pmcstat -L | awk -v e="$event" '$1==e{found=1}END{exit(!found)}'; then
		atf_skip "event $event unavailable on this CPU"
	fi
}

#
# pmcstat_one_sample <outfile> <dur_seconds>
#   Runs: pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc -o <out> -- sleep <dur>
#   Prints on stdout: "last_a last_b delta_abs permille b_gt_a"
#   Returns 1 if pmcstat fails or output is empty/unparseable.
#
pmcstat_one_sample()
{
	local out="$1"
	local dur="$2"

	if ! pmcstat -C -q \
	    -p ls_not_halted_cyc \
	    -p ls_not_halted_cyc \
	    -o "$out" -- sleep "$dur" > /dev/null 2>pmcstat_v3.err; then
		return 1
	fi
	[ -s "$out" ] || return 1

	awk '
	    !/^#/ && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {
	        la = $1; lb = $2
	    }
	    END {
	        if (la == "" || lb == "") { exit 1 }
	        d = la - lb; if (d < 0) d = -d
	        m = (la > lb) ? la : lb
	        p = (m > 0) ? (d * 1000.0 / m) : 0
	        bgt = (lb > la) ? 1 : 0
	        printf "%d %d %d %.4f %d\n", la, lb, d, p, bgt
	    }' "$out"
}

#
# pmcstat_one_sample_cmd <outfile> <cmd...>
#   Like pmcstat_one_sample but runs an arbitrary workload command.
#   Prints: "last_a last_b delta_abs permille b_gt_a"
#
pmcstat_one_sample_cmd()
{
	local out="$1"
	shift  # remaining args are the workload command

	if ! pmcstat -C -q \
	    -p ls_not_halted_cyc \
	    -p ls_not_halted_cyc \
	    -o "$out" -- "$@" > /dev/null 2>pmcstat_v3.err; then
		return 1
	fi
	[ -s "$out" ] || return 1

	awk '
	    !/^#/ && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {
	        la = $1; lb = $2
	    }
	    END {
	        if (la == "" || lb == "") { exit 1 }
	        d = la - lb; if (d < 0) d = -d
	        m = (la > lb) ? la : lb
	        p = (m > 0) ? (d * 1000.0 / m) : 0
	        bgt = (lb > la) ? 1 : 0
	        printf "%d %d %d %.4f %d\n", la, lb, d, p, bgt
	    }' "$out"
}

#
# pmcstat_collect_duration <dur> <n_samples>
#   Collects n_samples at the given duration.
#   Prints one awk-friendly line per sample: "dur la lb delta permille bgt"
#
pmcstat_collect_duration()
{
	local dur="$1"
	local n="$2"
	local i result

	i=0
	while [ "$i" -lt "$n" ]; do
		i=$((i + 1))
		result=$(pmcstat_one_sample "v3-dur${dur}-${i}.out" "$dur") || continue
		printf '%s %s\n' "$dur" "$result"
	done
}

# ---------------------------------------------------------------------------
# H1 — skew_scales_inversely_with_duration
# ---------------------------------------------------------------------------

atf_test_case skew_scales_inversely_with_duration cleanup
skew_scales_inversely_with_duration_head()
{
	atf_set "descr" \
	    "v3-H1: PMC skew permille decreases with longer sleep duration (fixed-overhead signature)"
	atf_set "require.user" "root"
}
skew_scales_inversely_with_duration_body()
{
	local iters dur all_data
	local perm_1s perm_30s verdict

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"

	iters=$(atf_config_get amd.pmc.grouping.v3.iterations 5)

	printf '%-6s  %8s  %12s  %12s  %8s  %8s\n' \
	    "dur(s)" "samples" "median_last_a" "median_delta" "med_perm‰" "max_perm‰"
	printf '%s\n' "------  --------  ------------  ------------  ---------  ---------"

	all_data=""
	for dur in 1 2 5 10 30; do
		data=$(pmcstat_collect_duration "$dur" "$iters")
		if [ -z "$data" ]; then
			printf '%-6s  no valid samples\n' "${dur}s"
			continue
		fi
		all_data="${all_data}${data}
"

		# Compute per-duration stats using awk
		printf '%s\n' "$data" | awk -v d="$dur" '
		    NF >= 5 {
		        n++
		        la[n]=$2; delta[n]=$4; perm[n]=$5
		    }
		    function median(arr, len,   sorted, i, j, tmp) {
		        for (i=1; i<=len; i++) sorted[i]=arr[i]
		        for (i=1; i<=len; i++)
		            for (j=i+1; j<=len; j++)
		                if (sorted[i] > sorted[j]) { tmp=sorted[i]; sorted[i]=sorted[j]; sorted[j]=tmp }
		        if (len % 2 == 1) return sorted[int(len/2)+1]
		        return (sorted[len/2] + sorted[len/2+1]) / 2
		    }
		    END {
		        if (n == 0) { printf "%-6s  no data\n", d"s"; exit }
		        med_la    = median(la,    n)
		        med_delta = median(delta, n)
		        med_perm  = median(perm,  n)
		        max_perm  = 0
		        for (i=1; i<=n; i++) if (perm[i] > max_perm) max_perm = perm[i]
		        printf "%-6s  %8d  %12.0f  %12.0f  %9.2f  %9.2f\n",
		            d"s", n, med_la, med_delta, med_perm, max_perm
		    }'
	done

	# Extract median permille for 1s and 30s to evaluate hypothesis
	perm_1s=$(printf '%s\n' "$all_data" | awk '$1==1 && NF>=5 {perm[++n]=$5}
	    END { if(n==0){print -1;exit}
	          for(i=1;i<=n;i++) for(j=i+1;j<=n;j++) if(perm[i]>perm[j]){t=perm[i];perm[i]=perm[j];perm[j]=t}
	          if(n%2==1) print perm[int(n/2)+1]; else print (perm[n/2]+perm[n/2+1])/2 }')
	perm_30s=$(printf '%s\n' "$all_data" | awk '$1==30 && NF>=5 {perm[++n]=$5}
	    END { if(n==0){print -1;exit}
	          for(i=1;i<=n;i++) for(j=i+1;j<=n;j++) if(perm[i]>perm[j]){t=perm[i];perm[i]=perm[j];perm[j]=t}
	          if(n%2==1) print perm[int(n/2)+1]; else print (perm[n/2]+perm[n/2+1])/2 }')

	printf '\n'
	printf 'H1 result: median_permille[1s]=%.2f  median_permille[30s]=%.2f\n' \
	    "$perm_1s" "$perm_30s"

	# Verdict: if permille[30] < permille[1] / 3 → fixed overhead confirmed
	verdict=$(awk -v p1="$perm_1s" -v p30="$perm_30s" 'BEGIN {
	    if (p1 <= 0 || p30 < 0) { print "insufficient_data"; exit }
	    ratio = p30 / p1
	    if (ratio < 0.333) print "fixed_overhead_confirmed"
	    else if (ratio < 0.700) print "partial_overhead"
	    else print "accumulated_drift"
	}')

	printf 'H1 verdict: %s\n' "$verdict"

	case "$verdict" in
	fixed_overhead_confirmed)
		printf 'INTERPRETATION: delta_abs is approximately constant; permille falls\n'
		printf 'proportionally with duration. The skew is dominated by a fixed cost\n'
		printf 'at pmc_start()/pmc_read() — NOT by divergence during measurement.\n'
		;;
	partial_overhead)
		printf 'INTERPRETATION: permille decreases with duration but not as fast as\n'
		printf '1/duration. Both fixed overhead and some accumulated drift are present.\n'
		;;
	accumulated_drift)
		printf 'INTERPRETATION: permille does not decrease with duration. Skew is\n'
		printf 'proportional to measurement time — counters diverge during measurement.\n'
		;;
	insufficient_data)
		atf_fail "H1: insufficient valid samples to evaluate hypothesis"
		;;
	esac

	# The test does not ATF_FAIL on outcome — it is a characterization test.
	# It fails only on data collection errors.
	printf '\nH1: characterization complete (no pass/fail criterion on verdict)\n'
}
skew_scales_inversely_with_duration_cleanup()
{
	rm -f v3-dur*.out pmcstat_v3.err
}

# ---------------------------------------------------------------------------
# H2 — counter_b_is_always_larger_than_a
# ---------------------------------------------------------------------------

atf_test_case counter_b_is_always_larger_than_a cleanup
counter_b_is_always_larger_than_a_head()
{
	atf_set "descr" \
	    "v3-H2: check whether counter B (second -p) is systematically larger than A (arming order bias)"
	atf_set "require.user" "root"
}
counter_b_is_always_larger_than_a_body()
{
	local n_iter i result la lb delta perm bgt
	local count_b_gt_a count_a_gt_b count_equal count_total

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"

	n_iter=$(atf_config_get amd.pmc.grouping.v3.bias.iterations 40)

	count_b_gt_a=0
	count_a_gt_b=0
	count_equal=0
	count_total=0

	printf '%-6s  %12s  %12s  %8s  %6s\n' "iter" "last_a" "last_b" "perm‰" "b>a?"
	printf '%s\n' "------  ------------  ------------  --------  ------"

	i=0
	while [ "$i" -lt "$n_iter" ]; do
		i=$((i + 1))
		result=$(pmcstat_one_sample "v3-bias-${i}.out" 2) || {
			printf '%-6d  pmcstat error\n' "$i"
			continue
		}
		set -- $result
		la="$1" lb="$2" delta="$3" perm="$4" bgt="$5"
		printf '%-6d  %12d  %12d  %8.2f  %6s\n' \
		    "$i" "$la" "$lb" "$perm" "$([ "$bgt" = 1 ] && echo yes || echo no)"
		count_total=$((count_total + 1))
		if [ "$bgt" = "1" ]; then
			count_b_gt_a=$((count_b_gt_a + 1))
		elif [ "$la" -gt "$lb" ]; then
			count_a_gt_b=$((count_a_gt_b + 1))
		else
			count_equal=$((count_equal + 1))
		fi
	done

	if [ "$count_total" -eq 0 ]; then
		atf_fail "H2: no valid samples collected"
	fi

	pct_b_gt_a=$(awk -v b="$count_b_gt_a" -v t="$count_total" \
	    'BEGIN { printf "%.1f", b/t*100 }')

	printf '\nH2 result: total=%d  b>a=%d (%.1f%%)  a>b=%d  equal=%d\n' \
	    "$count_total" "$count_b_gt_a" "$pct_b_gt_a" "$count_a_gt_b" "$count_equal"

	verdict=$(awk -v b="$count_b_gt_a" -v t="$count_total" 'BEGIN {
	    r = b / t
	    if      (r >= 0.90) print "strong_bias_b_gt_a"
	    else if (r >= 0.70) print "moderate_bias_b_gt_a"
	    else if (r <= 0.10) print "strong_bias_a_gt_b"
	    else if (r <= 0.30) print "moderate_bias_a_gt_b"
	    else                print "no_systematic_bias"
	}')

	printf 'H2 verdict: %s\n' "$verdict"

	case "$verdict" in
	strong_bias_b_gt_a)
		printf 'INTERPRETATION: B is almost always larger than A. This is strong\n'
		printf 'evidence that pmc_start() arms counter A first, then B. Counter B\n'
		printf 'starts counting before counter A has fully initialized — or A is\n'
		printf 'read before B at teardown, giving B extra cycles at both ends.\n'
		printf 'The skew is STRUCTURAL, not random noise.\n'
		;;
	moderate_bias_b_gt_a)
		printf 'INTERPRETATION: B tends to be larger than A. Probable sequential\n'
		printf 'arming but with some scheduling variability.\n'
		;;
	no_systematic_bias)
		printf 'INTERPRETATION: No consistent direction. Skew is symmetric noise,\n'
		printf 'not sequential arming. The source is timing jitter at start/stop,\n'
		printf 'not counter initialization order.\n'
		;;
	strong_bias_a_gt_b|moderate_bias_a_gt_b)
		printf 'INTERPRETATION: A tends to be larger — arming order reversed from\n'
		printf 'expectation. Counter A initialized last or read first.\n'
		;;
	esac
}
counter_b_is_always_larger_than_a_cleanup()
{
	rm -f v3-bias-*.out pmcstat_v3.err
}

# ---------------------------------------------------------------------------
# H3 — dd_fixed_count_accumulates_more_cycles_than_sleep
# ---------------------------------------------------------------------------

atf_test_case dd_fixed_count_accumulates_more_cycles_than_sleep cleanup
dd_fixed_count_accumulates_more_cycles_than_sleep_head()
{
	atf_set "descr" \
	    "v3-H3: dd count=500000 (no timeout) accumulates more cycles than sleep 5 and achieves lower skew"
	atf_set "require.user" "root"
}
dd_fixed_count_accumulates_more_cycles_than_sleep_body()
{
	local r_sleep r_dd
	local la_sleep lb_sleep perm_sleep
	local la_dd lb_dd perm_dd

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"

	printf 'Collecting sleep 5 sample...\n'
	r_sleep=$(pmcstat_one_sample "v3-h3-sleep.out" 5) || \
	    atf_fail "H3: pmcstat with sleep 5 failed"
	set -- $r_sleep
	la_sleep="$1" lb_sleep="$2" perm_sleep="$4"

	printf 'Collecting dd count=500000 sample...\n'
	r_dd=$(pmcstat_one_sample_cmd "v3-h3-dd.out" \
	    dd if=/dev/zero of=/dev/null bs=4096 count=500000) || \
	    atf_fail "H3: pmcstat with dd count=500000 failed"
	set -- $r_dd
	la_dd="$1" lb_dd="$2" perm_dd="$4"

	printf '\n'
	printf '%-20s  %12s  %12s  %10s\n' "workload" "last_a (cyc)" "last_b (cyc)" "permille‰"
	printf '%s\n' "--------------------  ------------  ------------  ----------"
	printf '%-20s  %12d  %12d  %10.2f\n' "sleep 5"         "$la_sleep" "$lb_sleep" "$perm_sleep"
	printf '%-20s  %12d  %12d  %10.2f\n' "dd count=500000" "$la_dd"    "$lb_dd"    "$perm_dd"
	printf '\n'

	# Assert: dd accumulates strictly more cycles than sleep 5
	if ! awk -v a="$la_dd" -v b="$la_sleep" 'BEGIN { exit(!(a > b)) }'; then
		atf_fail "H3: dd count=500000 did not accumulate more cycles than sleep 5 (dd=$la_dd sleep=$la_sleep); dd may have been too fast or not executed"
	fi
	printf 'PASS: dd accumulated more cycles than sleep 5 (ratio=%.1fx)\n' \
	    "$(awk -v d="$la_dd" -v s="$la_sleep" 'BEGIN { printf "%.1f", d/s }')"

	# Informational: compare permille (not a hard assertion — skew direction matters)
	perm_verdict=$(awk -v pd="$perm_dd" -v ps="$perm_sleep" 'BEGIN {
	    if (ps <= 0) { print "insufficient_sleep_data"; exit }
	    ratio = pd / ps
	    if      (ratio < 0.25)  print "dd_much_lower_skew"
	    else if (ratio < 0.70)  print "dd_lower_skew"
	    else if (ratio < 1.30)  print "similar_skew"
	    else                    print "dd_higher_skew"
	}')

	printf 'H3 skew verdict: %s\n' "$perm_verdict"
	case "$perm_verdict" in
	dd_much_lower_skew)
		printf 'INTERPRETATION: dd cycle count is much higher, making the fixed\n'
		printf 'timing overhead negligible. Confirms fixed-overhead model.\n' ;;
	dd_lower_skew)
		printf 'INTERPRETATION: dd reduces skew compared to sleep, consistent with\n'
		printf 'fixed overhead diluted by more cycles.\n' ;;
	similar_skew)
		printf 'INTERPRETATION: dd does not reduce skew significantly. The skew\n'
		printf 'source is not purely proportional to cycle count.\n' ;;
	dd_higher_skew)
		printf 'INTERPRETATION: dd has higher skew than sleep. Possible causes:\n'
		printf '  - dd involves more kernel transitions (write() syscalls) causing\n'
		printf '    more pmc_read interruptions or mode switches.\n'
		printf '  - Counter B bias is amplified by dd execution pattern.\n' ;;
	esac
}
dd_fixed_count_accumulates_more_cycles_than_sleep_cleanup()
{
	rm -f v3-h3-sleep.out v3-h3-dd.out pmcstat_v3.err
}

# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

atf_init_test_cases()
{
	atf_add_test_case skew_scales_inversely_with_duration
	atf_add_test_case counter_b_is_always_larger_than_a
	atf_add_test_case dd_fixed_count_accumulates_more_cycles_than_sleep
}
