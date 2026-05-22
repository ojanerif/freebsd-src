#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   v3 collector — structured hypothesis test for the source of duplicate PMC skew.
#
#   Three experiments, all running pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc:
#
#   EXPERIMENT 1 — Duration sweep (H1: fixed overhead vs accumulated drift)
#     For each duration in [1, 2, 5, 10, 30]s (configurable):
#       Run N iterations with sleep workload.
#       Record: last_a, last_b, delta_abs, permille, sign(b-a).
#     Expected result (fixed overhead):
#       delta_abs ≈ constant across durations, permille ∝ 1/duration.
#     Expected result (accumulated drift):
#       delta_abs ∝ duration, permille ≈ constant.
#
#   EXPERIMENT 2 — Counter position bias (H2: sequential arming)
#     Run N iterations at a fixed duration.
#     Track P(b > a) — if >> 0.5, counter B is consistently armed AFTER A
#     and reads more cycles (start earlier / stop later than A).
#
#   EXPERIMENT 3 — Fixed-count dd vs sleep (H3: pmc-003 fix validation)
#     Run N iterations each: sleep 5 vs dd count=500000.
#     Compares cycle counts and permille to quantify the workload effect.
#
#   Output:
#     JSON  — full records + per-experiment summaries
#     CSV   — flat record table (case, duration, iteration, last_a, last_b, delta, permille, b_gt_a)
#     Text  — printed sweep table and hypothesis verdicts

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import platform
import signal
import shlex
import shutil
import statistics
import sys
import time
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from pmu_common.amd_zen import parse_hwpmc_cpuid
from pmu_common.command import (
    CommandRunner,
    SudoConfig,
    bounded_timeout,
    check_tool as common_check_tool,
    is_root,
    remaining_seconds,
    simple_command_value,
)
from pmu_common.fs import (
    atomic_write_text,
    fsync_directory,
    read_text_file,
    sha256_file,
    unique_tmp_path,
    utc_now,
)
from pmu_common.metrics import percentile
from pmu_common.terminal import Terminal


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SCHEMA_VERSION = 3
PRODUCER = "pmc_grouping_skew_v3_collect.py"
EVENT = "ls_not_halted_cyc"

DEFAULT_DURATIONS: List[int] = [1, 2, 5, 10, 30]
DEFAULT_SWEEP_ITERATIONS = 20       # per duration point
DEFAULT_BIAS_ITERATIONS = 100       # for H2 counter position
DEFAULT_DD_ITERATIONS = 30          # for H3 dd vs sleep comparison
DEFAULT_BIAS_DURATION = 5           # seconds for H2
DEFAULT_DD_COUNT = 500_000          # dd count for H3
DEFAULT_COMMAND_TIMEOUT_OVERHEAD = 30
DEFAULT_COMMAND_GRACE_SECONDS = 10
DEFAULT_MINUTES = 60.0

STOP_REQUESTED = False
SHUTDOWN_REASON: Optional[str] = None
TERMINAL = Terminal("pmc-skew-v3")
COMMAND_RUNNER = CommandRunner(TERMINAL)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def log_info(m: str) -> None:    TERMINAL.info(m)
def log_warn(m: str) -> None:    TERMINAL.warn(m)
def log_err(m: str) -> None:     TERMINAL.error(m)
def log_verbose(m: str) -> None: TERMINAL.verbose(m)


def die(msg: str, code: int = 1) -> None:
    log_err(msg)
    raise SystemExit(code)


def parse_pos_int(v: str, name: str) -> int:
    try:
        n = int(v, 10)
    except ValueError as e:
        raise argparse.ArgumentTypeError(f"{name} must be an integer") from e
    if n <= 0:
        raise argparse.ArgumentTypeError(f"{name} must be positive")
    return n


def parse_pos_float(v: str, name: str) -> float:
    try:
        n = float(v)
    except ValueError as e:
        raise argparse.ArgumentTypeError(f"{name} must be a number") from e
    if not math.isfinite(n) or n <= 0:
        raise argparse.ArgumentTypeError(f"{name} must be positive")
    return n


def sysctl_value(name: str) -> str:
    return simple_command_value(["sysctl", "-n", name])


def check_tool(name: str) -> Optional[str]:
    try:
        return common_check_tool(name, required=True)
    except FileNotFoundError:
        die(f"required tool not found in PATH: {name}")


def run_cmd(
    argv: Sequence[str],
    args: argparse.Namespace,
    *,
    timeout_s: Optional[float] = None,
    label: Optional[str] = None,
) -> Dict[str, Any]:
    return COMMAND_RUNNER.run(
        SudoConfig(args.use_sudo, args.sudo_cmd, args.sudo_non_interactive).apply(argv),
        dry_run=args.dry_run,
        timeout_seconds=timeout_s,
        grace_seconds=DEFAULT_COMMAND_GRACE_SECONDS,
        progress_label=label,
    )


# ---------------------------------------------------------------------------
# pmcstat output parsing
# ---------------------------------------------------------------------------

def parse_pmcstat_two_counter(text: str) -> Optional[Dict[str, Any]]:
    """
    Parse the last numeric row from a pmcstat -C two-counter output file.
    Returns dict with: last_a, last_b, delta_abs, permille, b_gt_a
    Returns None if no valid row found.
    """
    last_a = last_b = None
    for line in text.splitlines():
        if line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) >= 2 and fields[0].isdigit() and fields[1].isdigit():
            last_a, last_b = int(fields[0]), int(fields[1])

    if last_a is None or last_b is None:
        return None

    delta = abs(last_a - last_b)
    maximum = max(last_a, last_b)
    permille = (delta * 1000.0 / maximum) if maximum > 0 else 0.0
    b_gt_a = last_b > last_a

    return {
        "last_a": last_a,
        "last_b": last_b,
        "delta_abs": delta,
        "permille": permille,
        "b_gt_a": b_gt_a,
        "a_gt_b": last_a > last_b,
        "equal": last_a == last_b,
    }


# ---------------------------------------------------------------------------
# Single probe runner
# ---------------------------------------------------------------------------

def run_probe(
    args: argparse.Namespace,
    *,
    case: str,
    duration: Optional[int],
    iteration: int,
    workload: List[str],
    raw_dir: Path,
    run_id: str,
) -> Dict[str, Any]:
    label = f"{case} dur={duration}s iter={iteration}" if duration else f"{case} iter={iteration}"
    out_name = f"{case}-dur{duration or 'x'}-{iteration:04d}.out"
    output_file = raw_dir / out_name
    tmp_out = output_file if args.dry_run else unique_tmp_path(output_file, "out")

    command = [
        "pmcstat", "-C", "-q",
        "-p", EVENT,
        "-p", EVENT,
        "-o", str(tmp_out),
        "--",
    ] + workload

    dur_for_timeout = duration or 10
    timeout_s = bounded_timeout(
        args.deadline,
        dur_for_timeout + DEFAULT_COMMAND_TIMEOUT_OVERHEAD,
    )

    result = run_cmd(command, args, timeout_s=timeout_s, label=label)
    rc = result.get("returncode")

    if tmp_out.exists() and rc == 0:
        tmp_out.replace(output_file)
        fsync_directory(output_file.parent)
    elif not args.dry_run:
        for p in (tmp_out, output_file):
            try:
                p.unlink()
            except OSError:
                pass

    output_text = "" if args.dry_run else read_text_file(output_file)
    parsed = parse_pmcstat_two_counter(output_text) if output_text else None
    terminated_by_signal = result.get("terminated_by_signal")
    interrupted = terminated_by_signal in (signal.SIGINT, signal.SIGTERM)

    record: Dict[str, Any] = {
        "run_id": run_id,
        "case": case,
        "duration_seconds": duration,
        "iteration": iteration,
        "workload": workload,
        "started_at": result.get("started_at"),
        "ended_at": result.get("ended_at"),
        "elapsed_seconds": result.get("elapsed_seconds"),
        "returncode": rc,
        "terminated_by_signal": terminated_by_signal,
        "interrupted": interrupted,
        "command_outcome": result.get("outcome"),
        "last_a": parsed["last_a"] if parsed else None,
        "last_b": parsed["last_b"] if parsed else None,
        "delta_abs": parsed["delta_abs"] if parsed else None,
        "permille": parsed["permille"] if parsed else None,
        "b_gt_a": parsed["b_gt_a"] if parsed else None,
        "a_gt_b": parsed["a_gt_b"] if parsed else None,
        "equal": parsed["equal"] if parsed else None,
        "valid": bool(parsed and rc == 0),
        "output_file": str(output_file) if not args.dry_run else None,
        "raw_sha256": sha256_file(output_file) if (not args.dry_run and output_file.exists()) else None,
        "stderr_tail": (result.get("stderr") or "")[-400:],
    }
    return record


# ---------------------------------------------------------------------------
# Per-experiment collection
# ---------------------------------------------------------------------------

def run_sweep(args: argparse.Namespace, run_id: str, raw_dir: Path) -> List[Dict[str, Any]]:
    """H1: duration sweep with sleep workload."""
    records = []
    for dur in args.durations:
        if STOP_REQUESTED:
            break
        workload = ["sleep", str(dur)]
        log_info(f"H1 sweep: duration={dur}s x{args.sweep_iterations} iterations")
        for it in range(1, args.sweep_iterations + 1):
            if STOP_REQUESTED:
                break
            if not enough_time(args, dur + DEFAULT_COMMAND_TIMEOUT_OVERHEAD):
                log_warn(f"H1: deadline reached at duration={dur}s iter={it}")
                break
            rec = run_probe(args, case="sweep", duration=dur, iteration=it,
                            workload=workload, raw_dir=raw_dir, run_id=run_id)
            records.append(rec)
            status = "ok" if rec["valid"] else "invalid"
            p = f"{rec['permille']:.2f}‰" if rec["permille"] is not None else "n/a"
            b_gt = "b>a" if rec.get("b_gt_a") else ("a>b" if rec.get("a_gt_b") else "eq")
            log_verbose(f"  H1 dur={dur}s it={it}: {status} permille={p} {b_gt}")
    return records


def run_bias(args: argparse.Namespace, run_id: str, raw_dir: Path) -> List[Dict[str, Any]]:
    """H2: counter position bias at fixed duration."""
    records = []
    dur = args.bias_duration
    workload = ["sleep", str(dur)]
    log_info(f"H2 bias: duration={dur}s x{args.bias_iterations} iterations")
    for it in range(1, args.bias_iterations + 1):
        if STOP_REQUESTED:
            break
        if not enough_time(args, dur + DEFAULT_COMMAND_TIMEOUT_OVERHEAD):
            log_warn(f"H2: deadline reached at iter={it}")
            break
        rec = run_probe(args, case="bias", duration=dur, iteration=it,
                        workload=workload, raw_dir=raw_dir, run_id=run_id)
        records.append(rec)
        if rec["valid"]:
            b_gt = "b>a" if rec["b_gt_a"] else ("a>b" if rec["a_gt_b"] else "eq")
            log_verbose(f"  H2 it={it}: permille={rec['permille']:.2f}‰ {b_gt}")
    return records


def run_dd_comparison(args: argparse.Namespace, run_id: str, raw_dir: Path) -> List[Dict[str, Any]]:
    """H3: dd count=N vs sleep 5."""
    records = []
    sleep_workload = ["sleep", "5"]
    dd_workload = ["dd", "if=/dev/zero", "of=/dev/null", "bs=4096",
                   f"count={args.dd_count}"]

    log_info(f"H3 dd-vs-sleep: x{args.dd_iterations} iterations each")
    for it in range(1, args.dd_iterations + 1):
        if STOP_REQUESTED:
            break

        # sleep probe
        if enough_time(args, 5 + DEFAULT_COMMAND_TIMEOUT_OVERHEAD):
            rec = run_probe(args, case="h3_sleep", duration=5, iteration=it,
                            workload=sleep_workload, raw_dir=raw_dir, run_id=run_id)
            records.append(rec)
        # dd probe
        if enough_time(args, 60):  # dd count=500000 should finish in <60s
            rec = run_probe(args, case="h3_dd", duration=None, iteration=it,
                            workload=dd_workload, raw_dir=raw_dir, run_id=run_id)
            records.append(rec)

    return records


def enough_time(args: argparse.Namespace, needed_s: float) -> bool:
    if args.dry_run:
        return True
    rem = remaining_seconds(args.deadline)
    return rem is None or rem > max(1.0, needed_s)


# ---------------------------------------------------------------------------
# Analysis and reporting
# ---------------------------------------------------------------------------

def stats_for(records: List[Dict[str, Any]], *, valid_only: bool = True) -> Dict[str, Any]:
    recs = [r for r in records if r.get("valid")] if valid_only else records
    permilles = [r["permille"] for r in recs if r.get("permille") is not None]
    deltas = [r["delta_abs"] for r in recs if r.get("delta_abs") is not None]
    last_as = [r["last_a"] for r in recs if r.get("last_a") is not None]
    b_gt_a_count = sum(1 for r in recs if r.get("b_gt_a"))
    a_gt_b_count = sum(1 for r in recs if r.get("a_gt_b"))
    equal_count = sum(1 for r in recs if r.get("equal"))
    n = len(recs)

    def s(lst: list) -> Dict[str, Any]:
        if not lst:
            return {"n": 0}
        return {
            "n": len(lst),
            "min": min(lst),
            "mean": statistics.fmean(lst),
            "median": statistics.median(lst),
            "p90": percentile(lst, 0.90),
            "p95": percentile(lst, 0.95),
            "max": max(lst),
            "stdev": statistics.pstdev(lst) if len(lst) > 1 else 0.0,
        }

    return {
        "n": n,
        "b_gt_a": b_gt_a_count,
        "a_gt_b": a_gt_b_count,
        "equal": equal_count,
        "pct_b_gt_a": (b_gt_a_count / n * 100) if n else None,
        "permille": s(permilles),
        "delta_abs": s(deltas),
        "last_a": s(last_as),
    }


def sweep_table(records: List[Dict[str, Any]], durations: List[int]) -> str:
    lines = []
    lines.append(
        f"{'dur(s)':<7} {'n':>5} {'med_last_a':>14} {'med_delta_abs':>14} "
        f"{'med_perm‰':>10} {'max_perm‰':>10} {'stdev_perm':>10} {'pct_b>a':>8}"
    )
    lines.append("-" * 82)
    for dur in durations:
        recs = [r for r in records if r.get("case") == "sweep"
                and r.get("duration_seconds") == dur and r.get("valid")]
        if not recs:
            lines.append(f"{dur+'s':<7} no valid samples")
            continue
        permilles = [r["permille"] for r in recs]
        deltas = [r["delta_abs"] for r in recs]
        last_as = [r["last_a"] for r in recs]
        b_count = sum(1 for r in recs if r.get("b_gt_a"))
        pct_b = b_count / len(recs) * 100
        lines.append(
            f"{str(dur)+'s':<7} {len(recs):>5} "
            f"{statistics.median(last_as):>14.0f} "
            f"{statistics.median(deltas):>14.0f} "
            f"{statistics.median(permilles):>10.2f} "
            f"{max(permilles):>10.2f} "
            f"{(statistics.pstdev(permilles) if len(permilles)>1 else 0.0):>10.2f} "
            f"{pct_b:>7.1f}%"
        )
    return "\n".join(lines)


def h1_verdict(records: List[Dict[str, Any]], durations: List[int]) -> str:
    """Return (verdict_str, detail_str)."""
    def med_perm(dur: int) -> Optional[float]:
        recs = [r for r in records if r.get("case") == "sweep"
                and r.get("duration_seconds") == dur and r.get("valid")
                and r.get("permille") is not None]
        if not recs:
            return None
        return statistics.median(r["permille"] for r in recs)

    def med_delta(dur: int) -> Optional[float]:
        recs = [r for r in records if r.get("case") == "sweep"
                and r.get("duration_seconds") == dur and r.get("valid")
                and r.get("delta_abs") is not None]
        if not recs:
            return None
        return statistics.median(r["delta_abs"] for r in recs)

    p_short = med_perm(durations[0])
    p_long = med_perm(durations[-1])
    d_short = med_delta(durations[0])
    d_long = med_delta(durations[-1])

    if p_short is None or p_long is None:
        return "insufficient_data", "not enough valid samples at both endpoints"

    ratio = p_long / p_short if p_short > 0 else 1.0
    delta_ratio = (d_long / d_short) if (d_short and d_short > 0) else None

    if ratio < 0.15:
        verdict = "fixed_overhead_confirmed"
        detail = (f"permille dropped {1/ratio:.1f}x from {durations[0]}s to {durations[-1]}s "
                  f"(expected {durations[-1]//durations[0]}x if purely fixed overhead). "
                  f"delta_abs ratio={delta_ratio:.2f} (near 1.0 = constant offset)."
                  if delta_ratio else "")
    elif ratio < 0.40:
        verdict = "strong_fixed_overhead"
        detail = f"permille ratio={ratio:.2f}; mostly fixed overhead with minor drift."
    elif ratio < 0.70:
        verdict = "mixed_overhead_and_drift"
        detail = f"permille ratio={ratio:.2f}; both fixed overhead and accumulated drift present."
    else:
        verdict = "accumulated_drift_dominant"
        detail = f"permille ratio={ratio:.2f}; permille does not fall with duration. Drift dominates."
    return verdict, detail


def h2_verdict(records: List[Dict[str, Any]]) -> Tuple[str, str]:
    recs = [r for r in records if r.get("case") == "bias" and r.get("valid")]
    if not recs:
        return "insufficient_data", "no valid bias samples"
    n = len(recs)
    b_count = sum(1 for r in recs if r.get("b_gt_a"))
    pct = b_count / n * 100
    if pct >= 90:
        verdict = "strong_sequential_arming_b_after_a"
        detail = (f"b>a in {pct:.1f}% of {n} runs. Counter B (second -p) is always armed "
                  "AFTER counter A. B accumulates extra cycles at startup before A "
                  "is fully active, and A stops counting before B at teardown. "
                  "The skew is STRUCTURAL and deterministic, not random noise.")
    elif pct >= 70:
        verdict = "probable_sequential_arming"
        detail = f"b>a in {pct:.1f}% of {n} runs. Probable sequential arming with scheduling variability."
    elif pct >= 30:
        verdict = "no_systematic_bias"
        detail = f"b>a in {pct:.1f}% of {n} runs. Symmetric noise, no arming order effect."
    else:
        verdict = "a_armed_after_b"
        detail = f"b>a in only {pct:.1f}% of {n} runs. Counter A is actually the trailing counter."
    return verdict, detail


def h3_verdict(records: List[Dict[str, Any]]) -> Tuple[str, str]:
    sleep_recs = [r for r in records if r.get("case") == "h3_sleep" and r.get("valid")]
    dd_recs = [r for r in records if r.get("case") == "h3_dd" and r.get("valid")]
    if not sleep_recs or not dd_recs:
        return "insufficient_data", "need both sleep and dd valid samples"

    med_cycles_sleep = statistics.median(r["last_a"] for r in sleep_recs)
    med_cycles_dd = statistics.median(r["last_a"] for r in dd_recs)
    med_perm_sleep = statistics.median(r["permille"] for r in sleep_recs)
    med_perm_dd = statistics.median(r["permille"] for r in dd_recs)

    cycle_ratio = med_cycles_dd / med_cycles_sleep if med_cycles_sleep > 0 else 0
    perm_ratio = med_perm_dd / med_perm_sleep if med_perm_sleep > 0 else 1.0

    if cycle_ratio < 1.0:
        verdict = "dd_fewer_cycles_than_sleep"
        detail = (f"dd count={500000} accumulated fewer cycles than sleep 5 "
                  f"(dd={med_cycles_dd:.0f} sleep={med_cycles_sleep:.0f}). "
                  "dd exits too quickly — increase count.")
    elif perm_ratio < 0.3:
        verdict = "dd_dramatically_reduces_skew"
        detail = (f"dd cycles={med_cycles_dd:.0f} ({cycle_ratio:.1f}x sleep). "
                  f"Permille: dd={med_perm_dd:.2f}‰ vs sleep={med_perm_sleep:.2f}‰ "
                  f"({perm_ratio:.2f}x). Fixed overhead model strongly confirmed.")
    elif perm_ratio < 0.7:
        verdict = "dd_reduces_skew_as_expected"
        detail = (f"dd cycles={med_cycles_dd:.0f} ({cycle_ratio:.1f}x sleep). "
                  f"Permille: dd={med_perm_dd:.2f}‰ vs sleep={med_perm_sleep:.2f}‰. "
                  "Consistent with fixed overhead diluted by more cycles.")
    else:
        verdict = "dd_does_not_reduce_skew"
        detail = (f"dd cycles={med_cycles_dd:.0f} ({cycle_ratio:.1f}x sleep). "
                  f"Permille: dd={med_perm_dd:.2f}‰ vs sleep={med_perm_sleep:.2f}‰. "
                  "Skew not proportional to cycle count — overhead is not simply fixed.")
    return verdict, detail


def build_summary(
    records: List[Dict[str, Any]],
    args: argparse.Namespace,
) -> Dict[str, Any]:
    sweep_recs = [r for r in records if r.get("case") == "sweep"]
    bias_recs = [r for r in records if r.get("case") == "bias"]
    h3_sleep = [r for r in records if r.get("case") == "h3_sleep"]
    h3_dd = [r for r in records if r.get("case") == "h3_dd"]

    per_duration = {}
    for dur in args.durations:
        recs_dur = [r for r in sweep_recs if r.get("duration_seconds") == dur]
        per_duration[str(dur)] = stats_for(recs_dur)

    h1_v, h1_d = h1_verdict(records, args.durations)
    h2_v, h2_d = h2_verdict(records)
    h3_v, h3_d = h3_verdict(records)

    return {
        "updated_at": utc_now(),
        "schema_version": SCHEMA_VERSION,
        "producer": PRODUCER,
        "records_total": len(records),
        "h1_sweep": {
            "verdict": h1_v,
            "detail": h1_d,
            "per_duration": per_duration,
        },
        "h2_bias": {
            "verdict": h2_v,
            "detail": h2_d,
            "stats": stats_for(bias_recs),
        },
        "h3_dd_vs_sleep": {
            "verdict": h3_v,
            "detail": h3_d,
            "sleep_stats": stats_for(h3_sleep),
            "dd_stats": stats_for(h3_dd),
        },
    }


def print_summary(summary: Dict[str, Any], records: List[Dict[str, Any]], args: argparse.Namespace) -> None:
    log_info("=" * 70)
    log_info("=== v3 hypothesis test results ===")
    log_info("=" * 70)

    log_info("\n--- H1: Duration sweep (fixed overhead vs accumulated drift) ---")
    log_info(sweep_table(records, args.durations))
    h1 = summary["h1_sweep"]
    log_info(f"\nH1 verdict: {h1['verdict']}")
    log_info(f"H1 detail:  {h1['detail']}")

    log_info("\n--- H2: Counter position bias ---")
    h2 = summary["h2_bias"]
    bias_s = h2["stats"]
    if bias_s.get("n", 0) > 0:
        log_info(
            f"n={bias_s['n']} b>a={h2['stats']['b_gt_a']} "
            f"({h2['stats'].get('pct_b_gt_a', 0):.1f}%) "
            f"a>b={h2['stats']['a_gt_b']}"
        )
    log_info(f"H2 verdict: {h2['verdict']}")
    log_info(f"H2 detail:  {h2['detail']}")

    log_info("\n--- H3: dd count vs sleep ---")
    h3 = summary["h3_dd_vs_sleep"]
    ss = h3["sleep_stats"]
    ds = h3["dd_stats"]
    if ss.get("n", 0) > 0 and ds.get("n", 0) > 0:
        sp = ss["permille"]
        dp = ds["permille"]
        sla = ss["last_a"]
        dla = ds["last_a"]
        log_info(f"sleep: n={ss['n']} med_cycles={sla.get('median', 0):.0f} med_perm={sp.get('median', 0):.2f}‰")
        log_info(f"dd:    n={ds['n']} med_cycles={dla.get('median', 0):.0f} med_perm={dp.get('median', 0):.2f}‰")
    log_info(f"H3 verdict: {h3['verdict']}")
    log_info(f"H3 detail:  {h3['detail']}")


# ---------------------------------------------------------------------------
# CSV output
# ---------------------------------------------------------------------------

CSV_COLUMNS = [
    "run_id", "case", "duration_seconds", "iteration",
    "started_at", "ended_at", "elapsed_seconds",
    "returncode", "valid",
    "last_a", "last_b", "delta_abs", "permille",
    "b_gt_a", "a_gt_b", "equal",
    "workload", "output_file", "raw_sha256",
]


def write_csv(path: Path, records: Sequence[Dict[str, Any]]) -> None:
    tmp = unique_tmp_path(path)
    try:
        with tmp.open("w", newline="", encoding="utf-8") as fp:
            w = csv.DictWriter(fp, fieldnames=CSV_COLUMNS)
            w.writeheader()
            for r in records:
                row = {c: r.get(c, "") for c in CSV_COLUMNS}
                if isinstance(row.get("workload"), list):
                    row["workload"] = shlex.join(row["workload"])
                w.writerow(row)
            fp.flush()
            os.fsync(fp.fileno())
        tmp.replace(path)
        fsync_directory(path.parent)
    except BaseException:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise


# ---------------------------------------------------------------------------
# Signal handling
# ---------------------------------------------------------------------------

def signal_handler(signum: int, _frame: Any) -> None:
    global SHUTDOWN_REASON, STOP_REQUESTED
    STOP_REQUESTED = True
    SHUTDOWN_REASON = f"signal {signum}"
    COMMAND_RUNNER.terminate_active(signal.SIGTERM)
    log_warn(f"received signal {signum}; stopping after current probe")


def shutdown_code() -> int:
    if SHUTDOWN_REASON is None:
        return 0
    import re
    m = re.fullmatch(r"signal (\d+)", SHUTDOWN_REASON)
    return (128 + int(m.group(1))) if m else 0


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "v3 PMC skew collector — structured hypothesis tests.\n"
            "H1: duration sweep (fixed overhead?)\n"
            "H2: counter position bias (sequential arming?)\n"
            "H3: dd count=N vs sleep (cycle count effect?)"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_v3_collect.py\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_v3_collect.py --sweep-only\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_v3_collect.py --once\n"
            "  python3 py-scripts/pmc_grouping_skew_v3_collect.py --dry-run --once\n"
        ),
    )

    p.add_argument("--durations", nargs="+", type=int,
        default=DEFAULT_DURATIONS,
        metavar="S",
        help=f"sleep durations for H1 sweep in seconds (default: {DEFAULT_DURATIONS})")
    p.add_argument("--sweep-iterations", type=lambda v: parse_pos_int(v, "sweep-iterations"),
        default=DEFAULT_SWEEP_ITERATIONS,
        help=f"H1 iterations per duration point (default: {DEFAULT_SWEEP_ITERATIONS})")
    p.add_argument("--bias-iterations", type=lambda v: parse_pos_int(v, "bias-iterations"),
        default=DEFAULT_BIAS_ITERATIONS,
        help=f"H2 counter position bias iterations (default: {DEFAULT_BIAS_ITERATIONS})")
    p.add_argument("--bias-duration", type=lambda v: parse_pos_int(v, "bias-duration"),
        default=DEFAULT_BIAS_DURATION,
        help=f"H2 sleep duration in seconds (default: {DEFAULT_BIAS_DURATION})")
    p.add_argument("--dd-count", type=lambda v: parse_pos_int(v, "dd-count"),
        default=DEFAULT_DD_COUNT,
        help=f"H3 dd block count (default: {DEFAULT_DD_COUNT:,})")
    p.add_argument("--dd-iterations", type=lambda v: parse_pos_int(v, "dd-iterations"),
        default=DEFAULT_DD_ITERATIONS,
        help=f"H3 iterations per workload (default: {DEFAULT_DD_ITERATIONS})")

    p.add_argument("--sweep-only", action="store_true",
        help="run only H1 (duration sweep); skip H2 and H3")
    p.add_argument("--bias-only", action="store_true",
        help="run only H2 (counter position bias)")
    p.add_argument("--dd-only", action="store_true",
        help="run only H3 (dd vs sleep comparison)")

    p.add_argument("--minutes", type=lambda v: parse_pos_float(v, "minutes"),
        default=DEFAULT_MINUTES,
        help=f"maximum collection time in minutes (default: {DEFAULT_MINUTES})")
    p.add_argument("--once", action="store_true",
        help="run all experiments exactly once (one pass)")

    p.add_argument("--outdir", type=Path,
        default=Path.cwd() / "pmu-skew-data-v3",
        help="output directory (default: ./pmu-skew-data-v3)")

    p.add_argument("--sudo-cmd", default="sudo")
    p.add_argument("--sudo-interactive", dest="sudo_non_interactive", action="store_false")
    p.add_argument("--no-sudo", dest="use_sudo", action="store_false")
    p.set_defaults(use_sudo=True, sudo_non_interactive=True)

    p.add_argument("--preflight-timeout", type=int, default=30)
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--color", choices=("auto", "always", "never"), default="auto")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--no-hwpmc-load", action="store_true")
    p.add_argument("--allow-non-freebsd", action="store_true")
    p.add_argument("--allow-non-amd", action="store_true")

    return p


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    TERMINAL.configure(
        color=args.color,
        live_graph=False,
        verbose=args.verbose,
        quiet=args.quiet,
    )

    if args.dry_run and not args.once:
        args.once = True

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    if not args.dry_run:
        check_tool("pmcstat")
        if args.use_sudo and not is_root():
            check_tool(args.sudo_cmd)

    args.outdir = args.outdir.expanduser().resolve()
    args.outdir.mkdir(parents=True, exist_ok=True)
    raw_dir = args.outdir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.outdir / "pmc-grouping-skew-v3.json"
    csv_path = args.outdir / "pmc-grouping-skew-v3.csv"

    # Platform info
    cpuid_raw = sysctl_value("kern.hwpmc.cpuid") if not args.dry_run else ""
    parsed_cpuid = parse_hwpmc_cpuid(cpuid_raw) if cpuid_raw else {}
    platform_info = {
        "system": platform.system(),
        "hw_model": sysctl_value("hw.model") if not args.dry_run else "",
        "kern_hwpmc_cpuid": cpuid_raw,
        "parsed_cpuid": parsed_cpuid,
    }
    if not args.dry_run and platform_info["system"] != "FreeBSD" and not args.allow_non_freebsd:
        die("requires FreeBSD; use --allow-non-freebsd for offline inspection")

    gen = parsed_cpuid.get("generation", "unknown")
    log_info(f"CPU: {gen} family={parsed_cpuid.get('family_hex')} model={parsed_cpuid.get('model_hex')}")
    log_info(f"H1 sweep durations: {args.durations}s  x{args.sweep_iterations} iterations each")
    log_info(f"H2 bias: {args.bias_iterations} iterations @ {args.bias_duration}s")
    log_info(f"H3 dd: count={args.dd_count:,} x{args.dd_iterations} vs sleep 5 x{args.dd_iterations}")

    run_id = uuid.uuid4().hex
    deadline = time.monotonic() + args.minutes * 60.0
    args.deadline = deadline

    all_records: List[Dict[str, Any]] = []

    try:
        do_h1 = not args.bias_only and not args.dd_only
        do_h2 = not args.sweep_only and not args.dd_only
        do_h3 = not args.sweep_only and not args.bias_only

        pass_num = 0
        while not STOP_REQUESTED:
            pass_num += 1
            log_info(f"\n=== pass {pass_num} ===")

            if do_h1 and not STOP_REQUESTED:
                all_records.extend(run_sweep(args, run_id, raw_dir))
            if do_h2 and not STOP_REQUESTED:
                all_records.extend(run_bias(args, run_id, raw_dir))
            if do_h3 and not STOP_REQUESTED:
                all_records.extend(run_dd_comparison(args, run_id, raw_dir))

            if args.once or time.monotonic() >= deadline:
                break

        summary = build_summary(all_records, args)
        state = {
            "schema_version": SCHEMA_VERSION,
            "producer": PRODUCER,
            "run_id": run_id,
            "platform": platform_info,
            "configuration": {
                "durations": args.durations,
                "sweep_iterations": args.sweep_iterations,
                "bias_iterations": args.bias_iterations,
                "bias_duration": args.bias_duration,
                "dd_count": args.dd_count,
                "dd_iterations": args.dd_iterations,
                "minutes": args.minutes,
            },
            "records_total": len(all_records),
            "summary": summary,
        }

        atomic_write_text(json_path, json.dumps(state, indent=2, sort_keys=True) + "\n")
        write_csv(csv_path, all_records)

        print_summary(summary, all_records, args)
        log_info(f"\nJSON: {json_path}")
        log_info(f"CSV:  {csv_path}")

        return shutdown_code()

    except KeyboardInterrupt:
        log_warn("interrupted by user")
        return 130


if __name__ == "__main__":
    sys.exit(main())
