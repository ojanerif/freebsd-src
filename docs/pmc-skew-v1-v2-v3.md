# PMC Grouping Skew — v1 / v2 / v3 Investigation Summary

**Date:** 2026-05-22  
**Author:** agent_claude (sess_2026-05-22_0900), requested by Osvaldo J. Filho  
**Test under investigation:** `pmcstat_grouping_test:repeated_process_cycles_have_bounded_skew`  
**Hardware:** AMD EPYC Zen 4 (family 0x19 model 0x11)

---

## 1. Overview

Three test versions were created to investigate and characterize skew in the
`repeated_process_cycles_have_bounded_skew` ATF case.

| Version | File | Status | Purpose |
|---|---|---|---|
| v1 | `pmcstat_grouping_test.sh` | Production (Makefile/Kyuafile) | Original test; 2h empirical baseline collected |
| v2 | `pmcstat_grouping_test_v2.sh` | Research artifact | CPU-bound workload + CPU pinning fix attempt |
| v3 | `pmcstat_grouping_test_v3.sh` | Research artifact | Structured hypothesis testing (H1/H2/H3) |

---

## 2. Empirical Data

### 2.1 Historical baseline (pre-investigation, tolerance 100 ‰)

| Metric | Value |
|---|---|
| Total runs | 481 |
| Pass | 417 (86.7 %) |
| Fail | 64 (13.3 %) |
| p90 | 100.7 ‰ |
| p95 | 103.9 ‰ |
| Max observed | 122.6 ‰ |
| Tolerance used | 100 ‰ |

### 2.2 2-hour collection — v1 vs v2

| Metric | v1 — `sleep 5` / tol 100 ‰ | v2 — `cpuset+dd` / tol 50 ‰ | v2 valid (≥ 10 M cycles) |
|---|---|---|---|
| Samples | 475 | 1 404 | 74 |
| Fail rate | 0.0 % | 95.3 % | 29.7 % |
| mean ‰ | 42.0 | 48.3 | 24.4 |
| p95 ‰ | 90.0 | 80.3 | 54.9 |
| max ‰ | 97.7 | 109.9 | 69.2 |
| b > a | 94.1 % | 100 % | 100 % |
| low\_count\_warnings | 0 | 94.7 % | — |

### 2.3 v3 empirical results (this session)

**H1 — Duration sweep** (30 samples × 5 durations = 150 sweep samples):

| dur | n | med\_last\_a | med\_delta\_abs | med\_perm ‰ | p95 ‰ | max ‰ | b > a |
|---|---|---|---|---|---|---|---|
| 1 s | 30 | 3.68 M | **199.2 K** | 50.43 | 55.44 | 98.72 | 90 % |
| 2 s | 30 | 3.83 M | **199.5 K** | 48.75 | 93.09 | 95.64 | 93 % |
| 5 s | 30 | 3.90 M | **197.8 K** | 45.28 | 95.93 | 102.02 | 80 % |
| 10 s | 30 | 4.00 M | **198.1 K** | 47.77 | 90.72 | 98.85 | 90 % |
| 30 s | 30 | 3.94 M | **198.3 K** | 48.07 | 91.64 | 93.50 | 83 % |

**H2 — Counter position bias** (100 samples at 5 s):

| Metric | Value |
|---|---|
| n | 100 |
| b > a | **94** (94.0 %) |
| a > b | 6 (6.0 %) |
| mean ‰ | 43.72 |
| median ‰ | 46.48 |
| p90 ‰ | 91.60 |
| p95 ‰ | 93.97 |
| max ‰ | 96.16 |
| stdev ‰ | 31.83 |
| med\_delta\_abs | 198.9 K |
| med\_last\_a | 3.97 M |

**H3 — `dd count=500000` vs `sleep 5`** (30 samples each):

| Metric | sleep 5 | dd count=500000 | ratio |
|---|---|---|---|
| med\_cycles | 3.88 M | **467 M** | 120.4× |
| med\_perm ‰ | 47.31 | **0.42** | 0.009× |
| p95 ‰ | 93.33 | **0.43** | 0.005× |
| max ‰ | 96.95 | **0.44** | 0.005× |
| b > a | 87 % | 90 % | — |

---

## 3. Root Cause Analysis — Final Model

### 3.1 H1 finding: the delta\_abs is a constant ~199 K cycles

The duration sweep reveals something striking: `med_delta_abs` is **nearly identical
across all durations** — 199.2 K at 1 s, 199.5 K at 2 s, 197.8 K at 5 s, 198.1 K at
10 s, 198.3 K at 30 s. The range is only 1.7 K cycles across a 30× span of measurement
duration.

This is the signature of a **fixed overhead**, not accumulated drift. The same ~199 K
cycle gap appears regardless of whether the workload runs for 1 second or 30 seconds.

However, the **permille does not drop** as expected from a pure fixed-overhead model.
At 30 s, the median permille is 48 ‰, nearly the same as 50 ‰ at 1 s. This seems
contradictory — until we look at `med_last_a`:

- 1 s: `med_last_a = 3.68 M` — the `sleep` workload accumulates almost the same total
  cycles regardless of duration. `sleep 1` and `sleep 30` both yield ~3.7–4.0 M
  `ls_not_halted_cyc`.

This is the key insight: **`ls_not_halted_cyc` only counts non-halted cycles**. A
`sleep N` process spends virtually all its time in `nanosleep()`, which is halted.
The cycle count reflects only the tiny active windows (syscall entry/exit, signal
delivery, pmcstat overhead) — and those do not grow with duration. So the denominator
stays at ~3.7–4 M regardless of sleep duration, and the ratio stays at ~50 ‰.

The H1 verdict `accumulated_drift_dominant` from the automated analysis is therefore
a **classifier artifact**: the ratio did not drop because the denominator did not grow,
not because drift grew proportionally. The underlying delta_abs data proves fixed overhead.

### 3.2 H2 finding: 94 % b > a — structural sequential arming

Counter B (second `-p ls_not_halted_cyc`) is larger than counter A in **94 of 100**
runs. This matches the v1/v2 pre-collection analysis (94.1 % and 100 %) and is
consistent across all experiments:

- H1 sweep: 80–93 % b > a across all durations
- H2 bias: 94.0 %
- H3 sleep: 87 %; H3 dd: 90 %

The ~6 % where a > b are the cases where scheduler noise or interrupt delivery reverses
the usual order. The bias is not 100 % (not a hard lock), but 94 % is far beyond random
noise (which would be 50 %).

**Mechanism:** `pmcstat -C` allocates and arms each `-p` counter in separate `pmc_start()`
syscalls. The kernel dispatches them sequentially: A first, then B. Counter B is
therefore armed slightly later — but at teardown, B is also read slightly later, meaning
B counts cycles during the window between the two reads. Net effect: B accumulates
~199 K extra cycles on average. At a median cycle count of ~3.9 M, this is ~50 ‰.

### 3.3 H3 finding: dd count=N reduces skew by 113× — the definitive fix

`dd count=500000` accumulates **467 M cycles** vs `sleep 5` at 3.88 M — a 120× larger
denominator. The delta_abs stays at roughly the same fixed ~199 K cycles (same syscall
jitter at arm/teardown), so the permille drops from 47.31 ‰ to **0.42 ‰** — a 113×
reduction. p95 drops from 93.33 ‰ to **0.43 ‰**.

This definitively confirms the fixed-overhead model. The gap between the two counters
is a constant ~199 K cycles regardless of workload. With a large enough denominator,
this becomes negligible.

The sign bias persists (90 % b > a for dd), confirming the structural arming order is
unchanged — but the magnitude is irrelevant at 0.42 ‰.

### 3.4 Root cause summary

```
skew_permille = delta_abs / max(last_a, last_b) × 1000

delta_abs ≈ 199 K cycles  (constant, determined by syscall jitter at pmc_start / pmc_read)

max(last_a, last_b) depends entirely on the workload:
  sleep N → ~3.7–4.0 M cycles  (idle; count is of syscall overhead, not sleep time)
  dd count=500000 → ~467 M cycles  (CPU-bound; count scales with actual work)

skew with sleep:   199K / 3.9M  × 1000 ≈ 51 ‰
skew with dd:      199K / 467M  × 1000 ≈ 0.43 ‰
```

The problem is **not** CPU migration, **not** accumulated measurement drift, and
**not** random noise. It is a **fixed ~199 K cycle offset** from sequential
`pmc_start()` / `pmc_read()` calls divided by a tiny denominator from an idle workload.

---

## 4. Bugs

### Bug pmc-001 — `pmcstat -C` does not synchronize start/read of multiple rows

**Status:** Open (FreeBSD base, `usr.sbin/pmcstat/`)  
**Symptom:** Two PMC rows allocated with separate `pmc_start()` and `pmc_read()` syscalls
produce a constant ~199 K cycle gap. The second counter always runs ahead of the first
(94 % of the time) due to deterministic sequential arming order.  
**Impact:** Any test comparing two identical counters in `-C` mode will see ~199 K cycle
skew regardless of duration. With an idle workload (~4 M total cycles), this is ~50 ‰.  
**Workaround:** Use a CPU-bound workload (`dd count=N`) to accumulate hundreds of millions
of cycles, making the fixed offset negligible (<1 ‰).  
**Long-term fix direction:** Investigate atomic group-read ioctl in hwpmc that arms/reads
multiple PMC rows in a single kernel entry (analogous to `perf_event_open` group fds
on Linux). Filed as upstream improvement opportunity.

---

### Bug pmc-002 — `DEFAULT_TOLERANCE_PERMILLE = 100` below empirical max in v1 collector

**Status:** Open  
**File:** `py-scripts/pmc_grouping_skew_collect.py:59`  
**Symptom:** Python collector marks samples as FAIL when skew is between 100–122.7 ‰,
causing ~13 % false failure rate. The ATF shell test uses 250 ‰ by default and is safe.  
**Fix:** Raise `DEFAULT_TOLERANCE_PERMILLE` from 100 to 250 (align with ATF default).
If kept at 100 as a research threshold, document the empirical baseline explicitly.  
**Files:** `py-scripts/pmc_grouping_skew_collect.py:59`, line 1565 (`--tolerance-permille` help)

---

### Bug pmc-003 — v2 `timeout 5 dd` terminated by SIGTERM before accumulating cycles

**Status:** Open (v2 design flaw)  
**Files:** `py-scripts/pmc_grouping_skew_v2_collect.py`, `pmcstat_grouping_test_v2.sh`  
**Symptom:** 94.7 % of v2 samples have `low_count_warning` (< 10 M cycles). dd killed
by `timeout` via SIGTERM before accumulating meaningful cycles.  
**Root cause:** With `cpuset -l 0`, dd runs on one core but 5 s of wall time is not 5 s
of CPU time under scheduler contention. The process frequently doesn't get enough
dispatch time to consume 10 M non-halted cycles.  
**Fix (confirmed by H3):** Use `dd count=500000 if=/dev/zero of=/dev/null bs=4096`
(exits by exhaustion at ~467 M cycles, no SIGTERM, no timeout needed).

---

## 5. Test Design — What Each Version Tests

### v1 — `pmcstat_grouping_test.sh` (production)

- Workload: `sleep 5`
- Tolerance: 250 ‰ (ATF default)
- Status: **safe at 250 ‰** (empirically: max observed 97.7 ‰ in 475 samples)
- Problem: `sleep` accumulates only ~3.9 M cycles regardless of duration, making the
  fixed 199 K cycle offset dominate (~50 ‰ median)

### v2 — `pmcstat_grouping_test_v2.sh` (research)

- Workload: `cpuset -l 0 timeout 5 dd if=/dev/zero of=/dev/null bs=4096`
- Tolerance: 50 ‰
- Hypothesis tested: CPU pinning + CPU-bound workload reduce skew
- Finding: 94.7 % of samples triggered `low_count_warning` (pmc-003); only 74 valid
  samples; mean dropped 42 → 24.4 ‰ but p95 still 54.9 ‰
- Not added to Makefile/Kyuafile

### v3 — `pmcstat_grouping_test_v3.sh` (research, 3 test cases)

Three ATF test cases targeting specific hypotheses:

**H1 — `skew_scales_inversely_with_duration`**: Duration sweep [1, 2, 5, 10, 30] s.
Revealed that delta_abs is constant (~199 K) but permille does not drop because
`sleep N` cycle counts are also constant (~3.9 M) regardless of duration.
ATF pass criterion: `median_permille[30s] < median_permille[1s] / 3` — **not met** with
sleep workload, but for the correct reason (idle denominator, not accumulated drift).

**H2 — `counter_b_is_always_larger_than_a`**: 100 iterations at 5 s. b > a in 94 %
of runs. Confirms structural sequential arming — informational, does not `atf_fail`.

**H3 — `dd_fixed_count_accumulates_more_cycles_than_sleep`**: `dd count=500000` vs
`sleep 5`. dd accumulates 467 M cycles (120× more) and produces 0.42 ‰ skew (113×
lower). **Hard assertion passes**: dd > sleep in all 30/30 runs.

---

## 6. Recommendations

| Priority | Action | File | Expected outcome |
|---|---|---|---|
| **1 — immediate** | Raise `DEFAULT_TOLERANCE_PERMILLE` 100 → 250 in v1 collector | `py-scripts/pmc_grouping_skew_collect.py:59` | Eliminates 13 % false failures; aligns with ATF default |
| **2 — v2 fix** | Replace `timeout 5 dd` with `dd count=500000` in v2 | `pmcstat_grouping_test_v2.sh`, `pmc_grouping_skew_v2_collect.py` | Fixes pmc-003; workload now accumulates ~467 M cycles consistently |
| **3 — v2 retest** | After pmc-003 fix, retest v2 at tolerance 5–10 ‰ | — | Expected: nearly all runs pass at 1–2 ‰; validates fixed-overhead model |
| **4 — ATF v2 revise** | Change v2 test workload to `dd count=500000`, lower tol to 5 ‰ | `pmcstat_grouping_test_v2.sh` | Creates a genuinely tight duplicate-PMC skew test |
| **5 — long term** | Investigate atomic group-read ioctl for `pmcstat -C` | FreeBSD `usr.sbin/pmcstat/` | Eliminates pmc-001 at the root; fixed gap → 0 |

---

## 7. Key Numbers at a Glance

| Metric | sleep workload | dd count=500000 |
|---|---|---|
| Cycle count | ~3.9 M | ~467 M |
| Fixed delta\_abs | ~199 K cycles | ~199 K cycles (same) |
| Median skew | ~48 ‰ | **~0.42 ‰** |
| p95 skew | ~93 ‰ | **~0.43 ‰** |
| b > a rate | 87–94 % | 90 % |
| Required ATF tolerance | 250 ‰ | **5 ‰** |

The ~199 K cycle fixed offset is the single number that explains the entire investigation.
It is the cost of two un-synchronized `pmc_start()` / `pmc_read()` syscall pairs on an
AMD EPYC Zen 4 at ~3–4 GHz. Everything else follows from dividing it by the workload
cycle count.

---

## 8. Files Produced

| File | Description |
|---|---|
| `tests/sys/amd/pmc/pmcstat_grouping_test_v2.sh` | ATF v2 (cpuset+dd, tol 50 ‰) — research |
| `tests/sys/amd/pmc/pmcstat_grouping_test_v3.sh` | ATF v3 (H1/H2/H3) — research |
| `py-scripts/pmc_grouping_skew_v2_collect.py` | Python v2 collector (--workload-cmd) |
| `py-scripts/pmc_grouping_skew_v3_collect.py` | Python v3 collector (sweep/bias/dd) |
| `docs/pmc-skew-v1-vs-v2.md` | v1 vs v2 detailed comparison |
| `docs/pmc-skew-v1-v2-v3.md` | This document |
| `/tmp/pmc-skew-v1/pmc-grouping-skew.{json,csv}` | 475 samples v1 (2h) |
| `/tmp/pmc-skew-v2/pmc-grouping-skew-v2.{json,csv}` | 1 404 samples v2 (2h) |
| `/tmp/pmc-skew-v3/pmc-grouping-skew-v3.{json,csv}` | 310 samples v3 (1 pass: 150 sweep + 100 bias + 60 h3) |
