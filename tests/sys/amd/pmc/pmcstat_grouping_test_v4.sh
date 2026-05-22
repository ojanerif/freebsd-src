#! /usr/libexec/atf-sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Version 4 of the duplicate AMD core PMC skew investigation suite.
#   Removes the ~199 K cycle fixed arming offset via calibration subtraction
#   and signed-delta noise analysis.
#
#   Background (from v3):
#     pmcstat -C arms two PMC rows via sequential pmc_start() syscalls.
#     Counter B is always armed after A, producing a constant ~199 K cycle gap
#     (94 % of the time b > a).  This offset is independent of workload duration.
#     With a sleep workload (~3.9 M cycles) it appears as ~50 permille.
#     With dd count=500000 (~467 M cycles) it drops to ~0.42 permille.
#
#   v4 approach — calibration subtraction + signed-delta noise floor:
#
#   PHASE 1 — calibration
#     Run N samples of: pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc -- true
#     `true` exits in microseconds; any measured delta is pure arming overhead.
#     baseline_offset = median(calibration_delta_abs)  (expected ~199 K)
#
#   PHASE 2 — measurement
#     Run N samples of: pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc
#                        -- dd count=500000 if=/dev/zero of=/dev/null bs=4096
#     corrected_delta = measured_delta_abs - baseline_offset
#     corrected_permille = corrected_delta / total_cycles * 1000
#
#   PHASE 3 — signed noise floor
#     signed_delta = last_b - last_a  (positive when b > a, negative otherwise)
#     true_noise_stdev = stdev(signed_delta) across all measurement samples
#     This is the actual measurement precision independent of structural bias.
#
#   ATF test cases:
#     T1 — calibrated_skew_is_near_zero
#          Hard assertion: corrected_permille < 1.0 permille
#          (after removing the arming offset the counters should agree within <1 permille)
#
#     T2 — arming_offset_is_stable
#          Characterization: stdev of calibration signed_delta < 30 % of median offset.
#          Informs upstream: the offset is consistent, making calibration reliable.
#          Informational only — does not atf_fail.
#
#   ATF config keys:
#     amd.pmc.grouping.runtime             (bool, default false)
#     amd.pmc.grouping.v4.calibration_n    (int, default 30)
#     amd.pmc.grouping.v4.measurement_n    (int, default 30)
#     amd.pmc.grouping.v4.dd_count         (int, default 500000)
#     amd.pmc.grouping.v4.corrected_tol    (float permille, default 1.0)

# ---------------------------------------------------------------------------
# Guards
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

# ---------------------------------------------------------------------------
# Core sample helper
# ---------------------------------------------------------------------------

#
# pmcstat_one_sample_cmd <outfile> <cmd...>
#   Runs: pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc -o <out> -- <cmd>
#   On success prints: "last_a last_b delta_abs signed_delta permille b_gt_a"
#   Returns 1 if pmcstat fails or output unparseable.
#
pmcstat_one_sample_cmd()
{
	local out="$1"
	shift

	if ! pmcstat -C -q \
	    -p ls_not_halted_cyc \
	    -p ls_not_halted_cyc \
	    -o "$out" -- "$@" > /dev/null 2>pmcstat_v4.err; then
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
	        sd = lb - la
	        m = (la > lb) ? la : lb
	        p = (m > 0) ? (d * 1000.0 / m) : 0
	        bgt = (lb > la) ? 1 : 0
	        printf "%d %d %d %d %.6f %d\n", la, lb, d, sd, p, bgt
	    }' "$out"
}

# ---------------------------------------------------------------------------
# Phase helpers
# ---------------------------------------------------------------------------

#
# pmcstat_collect_phase <phase_name> <n> <cmd...>
#   Collects n samples running <cmd>.
#   Prints one line per valid sample:
#     "last_a last_b delta_abs signed_delta permille b_gt_a"
#
pmcstat_collect_phase()
{
	local phase="$1"
	local n="$2"
	shift 2
	local i=0 result

	while [ "$i" -lt "$n" ]; do
		i=$((i + 1))
		result=$(pmcstat_one_sample_cmd "v4-${phase}-${i}.out" "$@") || continue
		printf '%s\n' "$result"
	done
}

#
# awk_median <col> — reads lines from stdin, prints median of column <col>
#
awk_median()
{
	local col="$1"
	awk -v c="$col" '
	    NF >= c { vals[NR] = $c }
	    END {
	        n = NR
	        if (n == 0) { print "0"; exit }
	        # bubble sort
	        for (i = 1; i <= n; i++)
	            for (j = i+1; j <= n; j++)
	                if (vals[i] > vals[j]) { t=vals[i]; vals[i]=vals[j]; vals[j]=t }
	        if (n % 2 == 1) print vals[(n+1)/2]
	        else printf "%.2f\n", (vals[n/2] + vals[n/2+1]) / 2
	    }'
}

#
# awk_stats <col> — reads lines from stdin, prints:
#   "n min mean median p90 p95 max stdev"
#
awk_stats()
{
	local col="$1"
	awk -v c="$col" '
	    NF >= c { vals[NR] = $c; sum += $c; n++ }
	    END {
	        if (n == 0) { print "0 0 0 0 0 0 0 0"; exit }
	        mean = sum / n
	        ss = 0
	        for (i = 1; i <= n; i++) ss += (vals[i] - mean)^2
	        stdev = (n > 1) ? sqrt(ss / n) : 0
	        # sort
	        for (i = 1; i <= n; i++)
	            for (j = i+1; j <= n; j++)
	                if (vals[i] > vals[j]) { t=vals[i]; vals[i]=vals[j]; vals[j]=t }
	        p90 = vals[int(n*0.90)+1]
	        p95 = vals[int(n*0.95)+1]
	        med = (n%2==1) ? vals[(n+1)/2] : (vals[n/2]+vals[n/2+1])/2
	        printf "%d %.2f %.4f %.4f %.4f %.4f %.4f %.4f\n",
	            n, vals[1], mean, med, p90, p95, vals[n], stdev
	    }'
}

# ---------------------------------------------------------------------------
# T1 — calibrated_skew_is_near_zero
# ---------------------------------------------------------------------------

atf_test_case calibrated_skew_is_near_zero cleanup
calibrated_skew_is_near_zero_head()
{
	atf_set "descr" \
	    "v4-T1: after subtracting the arming offset, corrected skew < 1 permille"
	atf_set "require.user" "root"
}
calibrated_skew_is_near_zero_body()
{
	local cal_n meas_n dd_count tol
	local cal_data meas_data
	local baseline_offset corrected_perm

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"

	cal_n=$(atf_config_get amd.pmc.grouping.v4.calibration_n 30)
	meas_n=$(atf_config_get amd.pmc.grouping.v4.measurement_n 30)
	dd_count=$(atf_config_get amd.pmc.grouping.v4.dd_count 500000)
	tol=$(atf_config_get amd.pmc.grouping.v4.corrected_tol 1.0)

	# ---- Phase 1: calibration (true workload) ----
	printf '=== Phase 1: calibration (workload: true, n=%s) ===\n' "$cal_n"
	cal_data=$(pmcstat_collect_phase "cal" "$cal_n" true)

	local cal_valid
	cal_valid=$(printf '%s\n' "$cal_data" | awk 'NF>=5' | wc -l | tr -d ' ')
	if [ "$cal_valid" -lt 5 ]; then
		atf_fail "calibration: only $cal_valid valid samples (need >= 5)"
	fi

	baseline_offset=$(printf '%s\n' "$cal_data" | awk_median 3)
	printf 'calibration: %s valid samples\n' "$cal_valid"
	printf 'baseline_offset (median delta_abs): %.0f cycles\n' "$baseline_offset"

	# calibration stdev of signed_delta
	local cal_signed_stats cal_stdev
	cal_signed_stats=$(printf '%s\n' "$cal_data" | awk_stats 4)
	cal_stdev=$(printf '%s\n' "$cal_signed_stats" | awk '{print $8}')
	printf 'calibration signed_delta: mean=%.0f stdev=%.0f\n' \
	    "$(printf '%s\n' "$cal_signed_stats" | awk '{print $3}')" "$cal_stdev"

	printf '\n'

	# ---- Phase 2: measurement (dd workload) ----
	printf '=== Phase 2: measurement (workload: dd count=%s, n=%s) ===\n' \
	    "$dd_count" "$meas_n"
	meas_data=$(pmcstat_collect_phase "meas" "$meas_n" \
	    dd "if=/dev/zero" "of=/dev/null" "bs=4096" "count=$dd_count")

	local meas_valid
	meas_valid=$(printf '%s\n' "$meas_data" | awk 'NF>=5' | wc -l | tr -d ' ')
	if [ "$meas_valid" -lt 5 ]; then
		atf_fail "measurement: only $meas_valid valid samples (need >= 5)"
	fi

	local meas_delta_stats meas_delta_med meas_cycles_med
	meas_delta_stats=$(printf '%s\n' "$meas_data" | awk_stats 3)
	meas_delta_med=$(printf '%s\n' "$meas_delta_stats" | awk '{print $4}')
	meas_cycles_med=$(printf '%s\n' "$meas_data" | awk_median 1)

	printf 'measurement: %s valid samples\n' "$meas_valid"
	printf 'measurement: med_delta_abs=%.0f cycles  med_total_cycles=%.0f\n' \
	    "$meas_delta_med" "$meas_cycles_med"

	# ---- Phase 3: corrected permille ----
	printf '\n=== Phase 3: corrected skew ===\n'
	corrected_perm=$(awk -v base="$baseline_offset" \
	    -v cycles="$meas_cycles_med" \
	    -v mdelta="$meas_delta_med" \
	    'BEGIN {
	        cd = mdelta - base
	        if (cd < 0) cd = 0
	        p = (cycles > 0) ? (cd * 1000.0 / cycles) : 0
	        printf "%.4f\n", p
	    }')

	local corrected_delta
	corrected_delta=$(awk -v base="$baseline_offset" -v md="$meas_delta_med" \
	    'BEGIN { cd = md - base; if (cd<0) cd=0; printf "%.0f\n", cd }')

	# signed delta stdev for measurement (true noise floor)
	local meas_signed_stats meas_noise_stdev
	meas_signed_stats=$(printf '%s\n' "$meas_data" | awk_stats 4)
	meas_noise_stdev=$(printf '%s\n' "$meas_signed_stats" | awk '{print $8}')

	printf 'baseline_offset:         %.0f cycles\n' "$baseline_offset"
	printf 'measured_delta_median:   %.0f cycles\n' "$meas_delta_med"
	printf 'corrected_delta:         %.0f cycles\n' "$corrected_delta"
	printf 'corrected_permille:      %s ‰\n' "$corrected_perm"
	printf 'true_noise_stdev:        %.0f cycles (signed delta stdev)\n' \
	    "$meas_noise_stdev"
	printf 'tolerance:               %s ‰\n' "$tol"

	printf '\n'

	# ---- Verdict ----
	local pass
	pass=$(awk -v cp="$corrected_perm" -v tol="$tol" \
	    'BEGIN { print (cp <= tol) ? "1" : "0" }')

	if [ "$pass" = "1" ]; then
		printf 'PASS: corrected_permille (%s) <= tolerance (%s)\n' \
		    "$corrected_perm" "$tol"
	else
		atf_fail "corrected_permille $corrected_perm ‰ exceeds tolerance $tol ‰ after arming offset subtraction"
	fi
}
calibrated_skew_is_near_zero_cleanup()
{
	rm -f v4-cal-*.out v4-meas-*.out pmcstat_v4.err
}

# ---------------------------------------------------------------------------
# T2 — arming_offset_is_stable
# ---------------------------------------------------------------------------

atf_test_case arming_offset_is_stable cleanup
arming_offset_is_stable_head()
{
	atf_set "descr" \
	    "v4-T2: calibration arming offset is stable (stdev < 30 % of median) — informational"
	atf_set "require.user" "root"
}
arming_offset_is_stable_body()
{
	local cal_n
	cal_n=$(atf_config_get amd.pmc.grouping.v4.calibration_n 30)

	pmcstat_check_support
	pmcstat_check_amd_grouping_runtime
	pmcstat_require_event "ls_not_halted_cyc"

	printf '=== Arming offset stability (workload: true, n=%s) ===\n' "$cal_n"
	local cal_data
	cal_data=$(pmcstat_collect_phase "t2" "$cal_n" true)

	local cal_valid
	cal_valid=$(printf '%s\n' "$cal_data" | awk 'NF>=5' | wc -l | tr -d ' ')
	if [ "$cal_valid" -lt 5 ]; then
		atf_skip "only $cal_valid valid calibration samples; need >= 5"
	fi

	local stats n min mean med p90 p95 max stdev
	stats=$(printf '%s\n' "$cal_data" | awk_stats 3)
	read -r n min mean med p90 p95 max stdev << EOF
$(printf '%s\n' "$stats")
EOF

	printf '%-12s %10s %10s %10s %10s %10s %10s %10s\n' \
	    "n" "min" "mean" "median" "p90" "p95" "max" "stdev"
	printf '%-12s %10.0f %10.0f %10.0f %10.0f %10.0f %10.0f %10.0f\n' \
	    "$n" "$min" "$mean" "$med" "$p90" "$p95" "$max" "$stdev"

	local pct_stdev
	pct_stdev=$(awk -v s="$stdev" -v m="$med" \
	    'BEGIN { printf "%.1f", (m>0) ? s/m*100 : 0 }')
	printf '\nstdev/median = %s %%\n' "$pct_stdev"

	local b_gt
	b_gt=$(printf '%s\n' "$cal_data" | awk 'NF>=6 && $6==1 {c++} END {print c+0}')
	printf 'b > a: %s / %s (%.0f %%)\n' \
	    "$b_gt" "$cal_valid" \
	    "$(awk -v b="$b_gt" -v n="$cal_valid" 'BEGIN{printf "%.1f",b/n*100}')"

	if awk -v pct="$pct_stdev" 'BEGIN{exit (pct<30)?0:1}'; then
		printf '\nINFO: offset is stable (stdev < 30 %% of median) — calibration reliable\n'
	else
		printf '\nWARN: offset stdev (%s %%) >= 30 %% of median — calibration noisy\n' \
		    "$pct_stdev"
	fi
	# informational only — never atf_fail
}
arming_offset_is_stable_cleanup()
{
	rm -f v4-t2-*.out pmcstat_v4.err
}

# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

atf_init_test_cases()
{
	atf_add_test_case calibrated_skew_is_near_zero
	atf_add_test_case arming_offset_is_stable
}
