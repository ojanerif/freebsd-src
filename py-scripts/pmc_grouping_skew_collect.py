#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Collect repeated FreeBSD hwpmc(4) / pmcstat(8) AMD core PMC grouping
#   skew measurements.  The collector runs the opt-in Kyua tolerance check and
#   direct pmcstat process-counting probes, then records JSON and CSV data for
#   variance, acceptance-rate, and terminal graph analysis.

from __future__ import annotations

import argparse
import copy
import csv
import json
import math
import os
import platform
import queue
import re
import signal
import statistics
import shutil
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

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
    tail_text,
    unique_tmp_path,
    utc_now,
)
from pmu_common.metrics import format_skew, percentile, permille_to_percent
from pmu_common.terminal import Terminal


DEFAULT_TOLERANCE_PERMILLE = 100
DEFAULT_SLEEP_SECONDS = 5
DEFAULT_MINUTES = 10.0
DEFAULT_KYUA_TIMEOUT_SECONDS = 120
DEFAULT_KYUA_MIN_SECONDS = 15
DEFAULT_COMMAND_GRACE_SECONDS = 5
DEFAULT_MIN_CYCLE_COUNT = 1
SCHEMA_VERSION = 3
PRODUCER = "pmc_grouping_skew_collect.py"
KYUA_TEST_NAME = "pmcstat_grouping_test:repeated_process_cycles_have_bounded_skew"

PMSTAT_DUPLICATE_EVENT = "ls_not_halted_cyc"
PMSTAT_INSTRUCTION_EVENT = "ex_ret_instr"
PMSTAT_CACHE_EVENT = "l2_pf_miss_l2_l3.all"
PMSTAT_CACHE_EVENT_CANDIDATES = (
    "l2_pf_miss_l2_l3.all",
    "l2_pf_miss_l2_l3",
    "l2_pf_miss_l2_l3.l2_hwpf",
)

SKEW_RE = re.compile(
    r"delta=(?P<delta>[0-9]+(?:\.[0-9]+)?)\s+"
    r"max=(?P<max>[0-9]+(?:\.[0-9]+)?)\s+"
    r"permille=(?P<permille>[0-9]+(?:\.[0-9]+)?)"
)

STOP_REQUESTED = False
SHUTDOWN_REASON: Optional[str] = None
TERMINAL = Terminal("pmc-grouping-skew")
COMMAND_RUNNER = CommandRunner(TERMINAL)


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def colorize(text: str, color: str) -> str:
    return TERMINAL.colorize(text, color)


def configure_terminal(args: argparse.Namespace) -> None:
    TERMINAL.configure(
        color=args.color,
        live_graph=args.live_graph,
        verbose=args.verbose,
        quiet=args.quiet,
    )


def log_info(message: str) -> None:
    TERMINAL.info(message)


def log_warn(message: str) -> None:
    TERMINAL.warn(message)


def log_err(message: str) -> None:
    TERMINAL.error(message)


def log_verbose(message: str) -> None:
    TERMINAL.verbose(message)


def die(message: str, exit_code: int = 1) -> None:
    log_err(message)
    raise SystemExit(exit_code)


def parse_positive_int(value: str, name: str) -> int:
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{name} must be an integer") from exc

    if number <= 0:
        raise argparse.ArgumentTypeError(f"{name} must be positive")

    return number


def parse_positive_float(value: str, name: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{name} must be a number") from exc

    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError(f"{name} must be positive")

    return number


# ---------------------------------------------------------------------------
# Command execution and platform probing
# ---------------------------------------------------------------------------

def run_command(
    argv: Sequence[str],
    *,
    cwd: Optional[Path] = None,
    env: Optional[Dict[str, str]] = None,
    dry_run: bool = False,
    timeout_seconds: Optional[float] = None,
    grace_seconds: float = DEFAULT_COMMAND_GRACE_SECONDS,
    progress_label: Optional[str] = None,
) -> Dict[str, Any]:
    return COMMAND_RUNNER.run(
        argv,
        cwd=cwd,
        env=env,
        dry_run=dry_run,
        timeout_seconds=timeout_seconds,
        grace_seconds=grace_seconds,
        progress_label=progress_label,
    )


def command_with_sudo(
    argv: Sequence[str],
    use_sudo: bool,
    sudo_cmd: str,
    sudo_non_interactive: bool = True,
) -> List[str]:
    return SudoConfig(
        use_sudo=use_sudo,
        sudo_cmd=sudo_cmd,
        non_interactive=sudo_non_interactive,
    ).apply(argv)


def sysctl_value(name: str) -> str:
    return simple_command_value(["sysctl", "-n", name])


def check_tool(name: str, *, required: bool = True) -> Optional[str]:
    try:
        return common_check_tool(name, required=required)
    except FileNotFoundError:
        die(f"required tool not found in PATH: {name}")


def ensure_hwpmc_loaded(args: argparse.Namespace) -> Dict[str, Any]:
    status = {
        "checked": False,
        "was_loaded": False,
        "load_attempted": False,
        "loaded_by_collector": False,
        "error": None,
    }
    if args.dry_run or args.no_hwpmc_load:
        return status

    if platform.system() != "FreeBSD":
        status["error"] = "not FreeBSD"
        return status

    status["checked"] = True

    loaded = run_command(
        ["kldstat", "-n", "hwpmc"],
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds,
    )
    if loaded["returncode"] == 0:
        status["was_loaded"] = True
        return status

    status["load_attempted"] = True
    load = run_command(
        command_with_sudo(
            ["kldload", "hwpmc"],
            args.use_sudo,
            args.sudo_cmd,
            args.sudo_non_interactive,
        ),
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds,
    )
    if load["returncode"] == 0:
        status["loaded_by_collector"] = True
    else:
        status["error"] = load.get("stderr", "kldload hwpmc failed").strip()

    return status


def collect_platform_info(args: argparse.Namespace) -> Dict[str, Any]:
    cpuid = sysctl_value("kern.hwpmc.cpuid")
    parsed_cpuid = parse_hwpmc_cpuid(cpuid) if cpuid else parse_hwpmc_cpuid("")
    info = {
        "collected_at": utc_now(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "uname": " ".join(platform.uname()),
        "hw_model": sysctl_value("hw.model"),
        "hw_ncpu": sysctl_value("hw.ncpu"),
        "hw_cpu_vendor": sysctl_value("hw.cpu_vendor"),
        "kern_hwpmc_cpuid": cpuid,
        "parsed_cpuid": parsed_cpuid,
        "pmcstat_path": shutil.which("pmcstat"),
        "kyua_path": shutil.which("kyua"),
    }
    return info


def validate_platform(info: Dict[str, Any], args: argparse.Namespace) -> None:
    if args.dry_run:
        return

    if info["system"] != "FreeBSD" and not args.allow_non_freebsd:
        die("this collector must run on FreeBSD; use --allow-non-freebsd only for dry inspection")

    cpuid = info["parsed_cpuid"]
    if cpuid.get("vendor") != "AuthenticAMD" and not args.allow_non_amd:
        die("AMD core PMC collection requires AuthenticAMD CPU and kern.hwpmc.cpuid")

    generation = cpuid.get("generation", "unknown")
    if generation in ("unknown", "unknown AMD", "future AMD") and not args.allow_unknown_generation:
        die(
            "unknown AMD generation; verify CPUID family/model/stepping and PPR, "
            "or pass --allow-unknown-generation after confirming event semantics"
        )


def resolve_kyua_dir(args: argparse.Namespace, repo_root: Path) -> Path:
    if args.kyua_dir is not None:
        return args.kyua_dir

    installed = Path("/usr/tests/sys/amd/pmc")
    if (installed / "Kyuafile").exists():
        return installed

    source = repo_root / "tests" / "sys" / "amd" / "pmc"
    return source


def collect_event_list(args: argparse.Namespace) -> List[str]:
    if args.dry_run:
        return []

    result = run_command(
        command_with_sudo(
            ["pmcstat", "-L"],
            args.use_sudo,
            args.sudo_cmd,
            args.sudo_non_interactive,
        ),
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds,
    )
    if result.get("returncode") != 0:
        args.event_list_error = result
        return []

    output = result.get("stdout", "")
    events = []

    for line in output.splitlines():
        fields = line.split()

        if fields:
            events.append(fields[0])

    return events


def check_event_availability(
    events: Iterable[str],
    required: Iterable[str],
) -> Dict[str, bool]:
    available = set(events)
    return {event: event in available for event in required}


def choose_cache_event(events: Iterable[str], requested: str) -> Optional[str]:
    available = set(events)

    if requested in available:
        return requested

    for event in PMSTAT_CACHE_EVENT_CANDIDATES:
        if event in available:
            return event

    return None


# ---------------------------------------------------------------------------
# Output parsing and summary
# ---------------------------------------------------------------------------

def parse_skew_from_text(text: str) -> Dict[str, Any]:
    matches = list(SKEW_RE.finditer(text))

    if not matches:
        return {}

    match = matches[-1]
    return {
        "delta": float(match.group("delta")),
        "max": float(match.group("max")),
        "permille": float(match.group("permille")),
    }


def header_event_count(header: str, event: str) -> int:
    count = 0

    for token in header.split():
        if token == event or token.endswith("/" + event):
            count += 1

    return count


def find_matching_header(
    headers: Sequence[str],
    event_a: str,
    event_b: str,
) -> Optional[str]:
    for header in headers:
        if event_a in header and event_b in header:
            return header

    return None


def parse_pmcstat_output(
    text: str,
    event_a: str,
    event_b: str,
    min_cycle_count: int = DEFAULT_MIN_CYCLE_COUNT,
) -> Dict[str, Any]:
    headers = [line for line in text.splitlines() if line.startswith("#")]
    rows: List[Tuple[int, int]] = []

    for line in text.splitlines():
        if line.startswith("#"):
            continue

        fields = line.split()

        if len(fields) < 2:
            continue

        if fields[0].isdigit() and fields[1].isdigit():
            rows.append((int(fields[0]), int(fields[1])))

    header = find_matching_header(headers, event_a, event_b)
    event_a_count = header_event_count(header, event_a) if header is not None else 0
    event_b_count = header_event_count(header, event_b) if header is not None else 0
    if event_a == event_b:
        header_valid = event_a_count >= 2
    else:
        header_valid = event_a_count >= 1 and event_b_count >= 1

    parsed: Dict[str, Any] = {
        "headers": headers,
        "matching_header": header,
        "event_a_count": event_a_count,
        "event_b_count": event_b_count,
        "header_valid": header_valid,
        "rows": rows,
        "row_count": len(rows),
        "positive_a": sum(1 for a, _ in rows if a > 0),
        "positive_b": sum(1 for _, b in rows if b > 0),
        "positive_pairs": sum(1 for a, b in rows if a > 0 and b > 0),
        "event_a": event_a,
        "event_b": event_b,
        "last_a": None,
        "last_b": None,
        "total_a": None,
        "total_b": None,
        "measurement_valid": False,
    }

    if rows:
        last_a, last_b = rows[-1]
        total_a = sum(a for a, _ in rows)
        total_b = sum(b for _, b in rows)
        parsed["last_a"] = last_a
        parsed["last_b"] = last_b
        parsed["total_a"] = total_a
        parsed["total_b"] = total_b
        minimum = min_cycle_count if event_a == event_b == PMSTAT_DUPLICATE_EVENT else 1
        parsed["measurement_valid"] = bool(
            header_valid and last_a >= minimum and last_b >= minimum
        )

        if event_a == event_b and max(last_a, last_b) > 0:
            delta = abs(last_a - last_b)
            maximum = max(last_a, last_b)
            total_delta = abs(total_a - total_b)
            total_maximum = max(total_a, total_b)
            parsed["delta"] = delta
            parsed["max"] = maximum
            parsed["permille"] = (delta * 1000.0) / maximum
            parsed["total_delta"] = total_delta
            parsed["total_max"] = total_maximum
            parsed["total_permille"] = (
                (total_delta * 1000.0) / total_maximum if total_maximum > 0 else None
            )
        elif event_a == PMSTAT_DUPLICATE_EVENT and event_b == PMSTAT_INSTRUCTION_EVENT:
            parsed["instructions_per_cycle"] = (last_b / last_a) if last_a > 0 else None
            parsed["total_instructions_per_cycle"] = (
                (total_b / total_a) if total_a > 0 else None
            )
        elif event_b == PMSTAT_DUPLICATE_EVENT:
            parsed["a_per_million_cycles"] = (last_a * 1000000.0 / last_b) if last_b > 0 else None
            parsed["total_a_per_million_cycles"] = (
                (total_a * 1000000.0 / total_b) if total_b > 0 else None
            )

    return parsed


def summarize_skew(
    records: Sequence[Dict[str, Any]],
    case: str,
) -> Dict[str, Any]:
    selected = [record for record in records if record.get("case") == case]
    permilles = [
        float(record["permille"])
        for record in selected
        if record.get("permille") is not None
    ]
    accepted = [record for record in selected if record.get("accepted") is True]
    rejected = [record for record in selected if record.get("accepted") is False]
    verdicts = len(accepted) + len(rejected)
    status_counts = summarize_status_counts(selected)
    tolerance_failures = [
        record
        for record in rejected
        if record.get("tolerance_pass") is False and record.get("permille") is not None
    ]
    command_failures = [
        record
        for record in selected
        if record.get("command_outcome") in ("timeout", "failed", "exec_error")
        and record.get("status") != "interrupted"
    ]
    invalid_outputs = [record for record in selected if record.get("status") == "invalid_output"]
    skipped = [record for record in selected if str(record.get("status", "")).startswith("skip_")]
    interrupted = [record for record in selected if record.get("status") == "interrupted"]
    dry_runs = [record for record in selected if record.get("status") == "dry_run"]

    mean_permille = statistics.fmean(permilles) if permilles else None
    stdev_permille = statistics.pstdev(permilles) if len(permilles) > 1 else 0.0
    p50_permille = percentile(permilles, 0.50)
    p90_permille = percentile(permilles, 0.90)
    p95_permille = percentile(permilles, 0.95)
    min_permille = min(permilles) if permilles else None
    max_permille = max(permilles) if permilles else None

    summary: Dict[str, Any] = {
        "case": case,
        "samples": len(selected),
        "samples_with_permille": len(permilles),
        "accepted": len(accepted),
        "rejected": len(rejected),
        "tolerance_failures": len(tolerance_failures),
        "command_failures": len(command_failures),
        "invalid_outputs": len(invalid_outputs),
        "skipped": len(skipped),
        "interrupted": len(interrupted),
        "dry_runs": len(dry_runs),
        "status_counts": status_counts,
        "acceptance_rate": (len(accepted) / verdicts) if verdicts else None,
        "min_permille": min_permille,
        "max_permille": max_permille,
        "mean_permille": mean_permille,
        "variance_permille": (
            statistics.pvariance(permilles) if len(permilles) > 1 else 0.0
        ),
        "stdev_permille": stdev_permille,
        "p50_permille": p50_permille,
        "p90_permille": p90_permille,
        "p95_permille": p95_permille,
        "min_percent": permille_to_percent(min_permille),
        "max_percent": permille_to_percent(max_permille),
        "mean_percent": permille_to_percent(mean_permille),
        "stdev_percent": permille_to_percent(stdev_permille),
        "p50_percent": permille_to_percent(p50_permille),
        "p90_percent": permille_to_percent(p90_permille),
        "p95_percent": permille_to_percent(p95_permille),
    }
    return summary


def summarize_status_counts(records: Sequence[Dict[str, Any]]) -> Dict[str, int]:
    summary: Dict[str, int] = {}

    for record in records:
        status = str(record.get("status", "unknown"))
        summary[status] = summary.get(status, 0) + 1

    return summary


def numeric_record_values(records: Sequence[Dict[str, Any]], key: str) -> List[float]:
    values: List[float] = []

    for record in records:
        value = record.get(key)
        if value is None:
            continue

        number = float(value)
        if math.isfinite(number):
            values.append(number)

    return values


def summarize_metric_values(values: Sequence[float]) -> Dict[str, Optional[float]]:
    return {
        "samples": len(values),
        "min": min(values) if values else None,
        "max": max(values) if values else None,
        "mean": statistics.fmean(values) if values else None,
        "p50": percentile(values, 0.50),
        "p90": percentile(values, 0.90),
        "p95": percentile(values, 0.95),
    }


def comparison_metric_for_case(case: str) -> Tuple[Optional[str], Optional[str]]:
    if case == "mixed_cycles_instructions":
        return "instructions_per_cycle", "IPC"

    if case == "mixed_cache_cycles":
        return "a_per_million_cycles", "per_Mcycle"

    return None, None


def summarize_comparison(
    records: Sequence[Dict[str, Any]],
    case: str,
) -> Dict[str, Any]:
    selected = [record for record in records if record.get("case") == case]
    accepted = [record for record in selected if record.get("accepted") is True]
    rejected = [record for record in selected if record.get("accepted") is False]
    skipped = [record for record in selected if str(record.get("status", "")).startswith("skip_")]
    interrupted = [record for record in selected if record.get("status") == "interrupted"]
    invalid_outputs = [record for record in selected if record.get("status") == "invalid_output"]
    command_failures = [
        record
        for record in selected
        if record.get("command_outcome") in ("timeout", "failed", "exec_error")
        and record.get("status") != "interrupted"
    ]
    metric_key, metric_label = comparison_metric_for_case(case)
    values = numeric_record_values(selected, metric_key) if metric_key is not None else []
    total_values = numeric_record_values(selected, f"total_{metric_key}") if metric_key else []
    latest = selected[-1] if selected else None

    return {
        "case": case,
        "records": len(selected),
        "accepted": len(accepted),
        "rejected": len(rejected),
        "skipped": len(skipped),
        "interrupted": len(interrupted),
        "invalid_outputs": len(invalid_outputs),
        "command_failures": len(command_failures),
        "status_counts": summarize_status_counts(selected),
        "metric_key": metric_key,
        "metric_label": metric_label,
        "metric": summarize_metric_values(values),
        "total_metric": summarize_metric_values(total_values),
        "latest": {
            "case": case,
            "event_a": latest.get("event_a"),
            "event_b": latest.get("event_b"),
            "row_count": latest.get("row_count"),
            "last_a": latest.get("last_a"),
            "last_b": latest.get("last_b"),
            "total_a": latest.get("total_a"),
            "total_b": latest.get("total_b"),
            "metric": latest.get(metric_key) if latest and metric_key else None,
            "total_metric": latest.get(f"total_{metric_key}") if latest and metric_key else None,
            metric_key: latest.get(metric_key) if latest and metric_key else None,
            f"total_{metric_key}": latest.get(f"total_{metric_key}") if latest and metric_key else None,
        } if latest else None,
    }


def build_summary(state: Dict[str, Any]) -> Dict[str, Any]:
    records = state.get("records", [])
    return {
        "updated_at": utc_now(),
        "schema_version": SCHEMA_VERSION,
        "iterations_completed": state.get("iterations_completed", 0),
        "records": len(records),
        "skew_cases": {
            "kyua_debug": summarize_skew(records, "kyua_debug"),
            "duplicate_cycles": summarize_skew(records, "duplicate_cycles"),
        },
        "comparison_cases": {
            "mixed_cycles_instructions": summarize_comparison(
                records,
                "mixed_cycles_instructions",
            ),
            "mixed_cache_cycles": summarize_comparison(records, "mixed_cache_cycles"),
        },
        "case_counts": summarize_case_counts(records),
    }


def summarize_case_counts(
    records: Sequence[Dict[str, Any]],
) -> Dict[str, Dict[str, int]]:
    summary: Dict[str, Dict[str, int]] = {}

    for record in records:
        case = str(record.get("case", "unknown"))
        status = str(record.get("status", "unknown"))
        case_summary = summary.setdefault(case, {})
        case_summary[status] = case_summary.get(status, 0) + 1

    return summary


# ---------------------------------------------------------------------------
# JSON / CSV emission
# ---------------------------------------------------------------------------

CSV_COLUMNS = [
    "run_id",
    "iteration",
    "case",
    "started_at",
    "ended_at",
    "elapsed_seconds",
    "returncode",
    "terminated_by_signal",
    "status",
    "command_outcome",
    "measurement_valid",
    "tolerance_pass",
    "accepted",
    "tolerance_permille",
    "event_a",
    "event_b",
    "row_count",
    "positive_a",
    "positive_b",
    "positive_pairs",
    "last_a",
    "last_b",
    "total_a",
    "total_b",
    "delta",
    "max",
    "permille",
    "total_delta",
    "total_max",
    "total_permille",
    "instructions_per_cycle",
    "total_instructions_per_cycle",
    "a_per_million_cycles",
    "total_a_per_million_cycles",
    "output_file",
    "raw_sha256",
    "command_text",
]


def record_to_csv_row(record: Dict[str, Any]) -> Dict[str, Any]:
    return {column: record.get(column, "") for column in CSV_COLUMNS}


def write_csv_records(path: Path, records: Sequence[Dict[str, Any]]) -> None:
    tmp = unique_tmp_path(path)

    try:
        with tmp.open("w", newline="", encoding="utf-8") as fp:
            writer = csv.DictWriter(fp, fieldnames=CSV_COLUMNS)
            writer.writeheader()

            for record in records:
                writer.writerow(record_to_csv_row(record))

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


def write_state(state: Dict[str, Any], json_path: Path, csv_path: Path) -> None:
    state["summary"] = build_summary(state)
    atomic_write_text(json_path, json.dumps(state, indent=2, sort_keys=True) + "\n")
    write_csv_records(csv_path, state.get("records", []))


class CheckpointWriter:
    def __init__(self, json_path: Path, csv_path: Path, threaded: bool) -> None:
        self.json_path = json_path
        self.csv_path = csv_path
        self.threaded = threaded
        self._queue: "queue.Queue[Optional[Dict[str, Any]]]" = queue.Queue(maxsize=2)
        self._thread: Optional[threading.Thread] = None
        self._error: Optional[BaseException] = None

    def start(self) -> None:
        if not self.threaded:
            return

        self._thread = threading.Thread(
            target=self._worker,
            name="pmc-skew-checkpoint-writer",
            daemon=True,
        )
        self._thread.start()

    def write(self, state: Dict[str, Any]) -> None:
        self.raise_if_failed()
        snapshot = copy.deepcopy(state)

        if not self.threaded:
            write_state(snapshot, self.json_path, self.csv_path)
            return

        while True:
            try:
                self._queue.put(snapshot, timeout=0.25)
                return
            except queue.Full:
                self.raise_if_failed()

    def close(self) -> None:
        if not self.threaded:
            self.raise_if_failed()
            return

        while True:
            try:
                self._queue.put(None, timeout=0.25)
                break
            except queue.Full:
                self.raise_if_failed()

        if self._thread is not None:
            self._thread.join()
        self.raise_if_failed()

    def raise_if_failed(self) -> None:
        if self._error is not None:
            raise RuntimeError("checkpoint writer failed") from self._error

    def _worker(self) -> None:
        latest: Optional[Dict[str, Any]] = None
        try:
            while True:
                item = self._queue.get()
                if item is None:
                    if latest is not None:
                        write_state(latest, self.json_path, self.csv_path)

                    return

                latest = item

                while True:
                    try:
                        extra = self._queue.get_nowait()
                    except queue.Empty:
                        break

                    if extra is None:
                        write_state(latest, self.json_path, self.csv_path)
                        return

                    latest = extra

                write_state(latest, self.json_path, self.csv_path)
                latest = None
        except BaseException as exc:
            self._error = exc


# ---------------------------------------------------------------------------
# Terminal graph output
# ---------------------------------------------------------------------------

def graph_bar(value: Optional[float], tolerance: int, width: int = 40) -> str:
    if value is None:
        return "?" * min(width, 8)

    if tolerance <= 0:
        tolerance = 1

    ratio = min(value / tolerance, 2.0)
    fill = int(round((ratio / 2.0) * width))
    fill = max(0, min(width, fill))
    chars = ["-"] * width
    threshold = max(0, min(width - 1, width // 2))
    chars[threshold] = "|"

    for index in range(fill):
        chars[index] = "#"

    return "".join(chars)


def color_status(status: str) -> str:
    if status == "PASS":
        return colorize(status, "green")

    if status == "FAIL":
        return colorize(status, "red")

    if status in ("SKIP", "STOP"):
        return colorize(status, "yellow")

    return colorize(status, "cyan")


def format_count(value: Any) -> str:
    if value is None:
        return "n/a"

    return f"{int(value):,}"


def format_ratio(value: Any, precision: int = 3) -> str:
    if value is None:
        return "n/a"

    return f"{float(value):.{precision}f}"


def record_display_status(record: Dict[str, Any]) -> str:
    status = str(record.get("status", ""))
    accepted = record.get("accepted")

    if accepted is True:
        return "PASS"

    if status.startswith("skip_"):
        return "SKIP"

    if status == "interrupted":
        return "STOP"

    if status == "dry_run":
        return "DRY"

    if accepted is False:
        return "FAIL"

    return "INFO"


def record_comparison_detail(record: Dict[str, Any]) -> str:
    row_count = record.get("row_count")
    last_a = record.get("last_a")
    last_b = record.get("last_b")

    if last_a is None or last_b is None:
        return ""

    case = record.get("case")
    total_a = record.get("total_a")
    total_b = record.get("total_b")
    row_text = f"rows={row_count}"

    if case in ("kyua_debug", "duplicate_cycles"):
        detail = (
            f"{row_text} last={format_count(last_a)}/{format_count(last_b)} "
            f"delta={format_count(record.get('delta'))}"
        )
        if total_a is not None and total_b is not None and row_count and int(row_count) > 1:
            detail += (
                f" total={format_count(total_a)}/{format_count(total_b)} "
                f"total_skew={format_skew(record.get('total_permille'))}"
            )
        return detail

    if case == "mixed_cycles_instructions":
        detail = (
            f"{row_text} cycles={format_count(last_a)} "
            f"instructions={format_count(last_b)} "
            f"IPC={format_ratio(record.get('instructions_per_cycle'))}"
        )
        if total_a is not None and total_b is not None and row_count and int(row_count) > 1:
            detail += (
                f" total_cycles={format_count(total_a)} "
                f"total_instructions={format_count(total_b)} "
                f"total_IPC={format_ratio(record.get('total_instructions_per_cycle'))}"
            )
        return detail

    if case == "mixed_cache_cycles":
        event_a = record.get("event_a") or "event_a"
        detail = (
            f"{row_text} {event_a}={format_count(last_a)} "
            f"cycles={format_count(last_b)} "
            f"per_Mcycle={format_ratio(record.get('a_per_million_cycles'))}"
        )
        if total_a is not None and total_b is not None and row_count and int(row_count) > 1:
            detail += (
                f" total_{event_a}={format_count(total_a)} "
                f"total_cycles={format_count(total_b)} "
                f"total_per_Mcycle={format_ratio(record.get('total_a_per_million_cycles'))}"
            )
        return detail

    return f"{row_text} last={format_count(last_a)}/{format_count(last_b)}"


def behavior_insight(case: str, data: Dict[str, Any], tolerance: int) -> str:
    samples_with_permille = int(data.get("samples_with_permille", 0) or 0)
    command_failures = int(data.get("command_failures", 0) or 0)
    invalid_outputs = int(data.get("invalid_outputs", 0) or 0)
    interrupted = int(data.get("interrupted", 0) or 0)
    skipped = int(data.get("skipped", 0) or 0)
    tolerance_failures = int(data.get("tolerance_failures", 0) or 0)

    if samples_with_permille == 0:
        if interrupted:
            return "run interrupted before a completed skew sample; partial output was checkpointed"
        if command_failures or invalid_outputs:
            return "no skew samples decoded; inspect command status and raw output"
        if skipped:
            return "no skew samples decoded; deadline guard skipped remaining probes"
        return "no skew samples decoded; check raw output or run without --dry-run"

    mean = data.get("mean_permille")
    p95 = data.get("p95_permille")
    maximum = data.get("max_permille")
    stdev = data.get("stdev_permille")

    reference = p95 if p95 is not None else maximum if maximum is not None else mean
    tolerance_ratio = None
    if reference is not None and tolerance > 0:
        tolerance_ratio = reference / tolerance

    if tolerance_failures > 0:
        verdict = "skew exceeded tolerance; independent counter read/start timing is visible"
    elif command_failures or invalid_outputs:
        verdict = "completed skew samples are in range, but some probes failed before usable output"
    elif interrupted:
        verdict = "completed skew samples are in range before operator interrupt"
    elif skipped:
        verdict = "completed skew samples are in range; remaining probes were skipped by deadline guard"
    elif tolerance_ratio is None:
        verdict = "skew decoded, but tolerance comparison is unavailable"
    elif tolerance_ratio < 0.25:
        verdict = "stable; observed skew is comfortably below tolerance"
    elif tolerance_ratio < 0.75:
        verdict = "moderate; skew is inside tolerance but large enough to track across runs"
    else:
        verdict = "near limit; still passing, but scheduler/load noise is close to tolerance"

    spread = ""
    if mean is not None and stdev is not None:
        if mean == 0:
            spread = " no measurable spread around a zero mean."
        elif stdev > mean:
            spread = " high run-to-run spread; prefer longer sleeps or more iterations."
        elif stdev > mean * 0.5:
            spread = " noticeable run-to-run spread; compare p95/max, not only mean."
        else:
            spread = " low run-to-run spread."

    if case == "duplicate_cycles":
        context = (
            " duplicate ls_not_halted_cyc counters should be close,"
            " not bit-identical in FreeBSD pmcstat."
        )
    elif case == "kyua_debug":
        context = " Kyua tolerance path reflects the installed pmcstat grouping regression case."
    else:
        context = ""

    return f"{verdict}.{spread}{context}"


def print_record_line(record: Dict[str, Any], tolerance: int) -> None:
    case = record.get("case", "unknown")
    permille = record.get("permille")
    status = record_display_status(record)
    detail = record_comparison_detail(record)

    if permille is None:
        raw_status = str(record.get("status", ""))
        suffix = f" ({raw_status})" if raw_status and raw_status.lower() != status.lower() else ""
        if detail:
            suffix += f" {detail}"
        log_info(f"iteration {record.get('iteration')} {case}: {color_status(status)}{suffix}")
        return

    bar = graph_bar(float(permille), tolerance)
    bar_color = "green" if status == "PASS" else "red" if status == "FAIL" else "cyan"

    log_info(
        f"iteration {record.get('iteration')} {case}: {color_status(status)} "
        f"permille={float(permille):.3f} tolerance={tolerance} "
        f"[{colorize(bar, bar_color)}] {detail}"
    )


def print_final_summary(state: Dict[str, Any], json_path: Path, csv_path: Path) -> None:
    summary = state.get("summary", build_summary(state))
    if not summary:
        summary = build_summary(state)
    configuration = state.get("configuration", {})
    tolerance = int(configuration.get("tolerance_permille", 0) or 0)
    log_info("=== skew summary ===")
    if state.get("shutdown_reason"):
        log_info(f"shutdown: {state['shutdown_reason']}")
    log_info(f"tolerance: {format_skew(float(tolerance))}")

    for case, data in summary.get("skew_cases", {}).items():
        records = data.get("samples", 0)
        measurements = data.get("samples_with_permille", 0)

        if records == 0:
            log_info(f"{case}: no samples")
            continue

        rate = data.get("acceptance_rate")
        rate_s = "n/a" if rate is None else f"{rate * 100.0:.2f}%"
        mean = data.get("mean_permille")
        p95 = data.get("p95_permille")
        maximum = data.get("max_permille")
        log_info(
            f"{case}: records={records} measurements={measurements} "
            f"accepted={data.get('accepted')} rejected={data.get('rejected')} "
            f"skipped={data.get('skipped')} interrupted={data.get('interrupted')} "
            f"cmdfail={data.get('command_failures')} invalid={data.get('invalid_outputs')} "
            f"acceptance={rate_s} "
            f"mean={format_skew(mean)} p95={format_skew(p95)} max={format_skew(maximum)}"
        )
        log_info(f"{case} insight: {behavior_insight(case, data, tolerance)}")

    comparison_cases = summary.get("comparison_cases", {})
    if comparison_cases:
        log_info("=== direct pmcstat comparisons ===")

    for case, data in comparison_cases.items():
        records = data.get("records", 0)
        if records == 0:
            log_info(f"{case}: no samples")
            continue

        metric = data.get("metric", {})
        total_metric = data.get("total_metric", {})
        latest = data.get("latest") or {}
        latest_text = record_comparison_detail(latest) if latest else ""
        latest_suffix = f" latest: {latest_text}" if latest_text else ""
        label = data.get("metric_label") or "metric"
        log_info(
            f"{case}: records={records} accepted={data.get('accepted')} "
            f"rejected={data.get('rejected')} skipped={data.get('skipped')} "
            f"cmdfail={data.get('command_failures')} invalid={data.get('invalid_outputs')} "
            f"{label}_mean={format_ratio(metric.get('mean'))} "
            f"{label}_p95={format_ratio(metric.get('p95'))} "
            f"total_{label}_mean={format_ratio(total_metric.get('mean'))}"
            f"{latest_suffix}"
        )

    log_info(f"JSON: {json_path}")
    log_info(f"CSV:  {csv_path}")


# ---------------------------------------------------------------------------
# Measurement cases
# ---------------------------------------------------------------------------

def flatten_case_record(
    *,
    run_id: str,
    iteration: int,
    case: str,
    command_result: Dict[str, Any],
    tolerance: int,
    event_a: Optional[str] = None,
    event_b: Optional[str] = None,
    output_file: Optional[Path] = None,
    parsed: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    parsed = parsed or {}
    rc = command_result.get("returncode")
    permille = parsed.get("permille")
    command_ok = rc == 0 and command_result.get("outcome") in ("ok", "dry_run")
    terminated_by_signal = command_result.get("terminated_by_signal")
    interrupted = terminated_by_signal in (signal.SIGINT, signal.SIGTERM)
    measurement_valid = bool(parsed.get("measurement_valid"))
    tolerance_pass: Optional[bool] = None
    accepted: Optional[bool]
    status: str

    if interrupted or (STOP_REQUESTED and not command_ok and not command_result.get("dry_run")):
        accepted = None
        status = "interrupted"
    elif command_result.get("dry_run"):
        accepted = None
        status = "dry_run"
    elif case in ("kyua_debug", "duplicate_cycles"):
        valid_for_tolerance = measurement_valid or case == "kyua_debug"
        tolerance_pass = bool(
            command_ok
            and valid_for_tolerance
            and permille is not None
            and float(permille) <= tolerance
        )
        accepted = tolerance_pass
        if not command_ok:
            status = str(command_result.get("outcome", "command_error"))
        elif permille is None or not valid_for_tolerance:
            status = "invalid_output"
        else:
            status = "pass" if tolerance_pass else "fail_tolerance"
    elif command_ok:
        rows = parsed.get("row_count")
        positive_a = int(parsed.get("positive_a", 0) or 0)
        positive_b = int(parsed.get("positive_b", 0) or 0)

        if case == "mixed_cycles_instructions":
            accepted = bool(parsed.get("header_valid", True) and positive_a and positive_b)
        elif case == "mixed_cache_cycles":
            accepted = bool(parsed.get("header_valid", True) and positive_b)
        else:
            accepted = bool(rows is None or rows > 0 and parsed.get("header_valid", True))

        status = "pass" if accepted else "invalid_output"
    else:
        accepted = False
        status = str(command_result.get("outcome", "command_error"))

    record: Dict[str, Any] = {
        "run_id": run_id,
        "iteration": iteration,
        "case": case,
        "started_at": command_result.get("started_at"),
        "ended_at": command_result.get("ended_at"),
        "elapsed_seconds": command_result.get("elapsed_seconds"),
        "returncode": rc,
        "terminated_by_signal": terminated_by_signal,
        "status": status,
        "command_outcome": command_result.get("outcome"),
        "measurement_valid": measurement_valid,
        "tolerance_pass": tolerance_pass,
        "accepted": accepted,
        "tolerance_permille": tolerance,
        "event_a": event_a,
        "event_b": event_b,
        "row_count": parsed.get("row_count"),
        "positive_a": parsed.get("positive_a"),
        "positive_b": parsed.get("positive_b"),
        "positive_pairs": parsed.get("positive_pairs"),
        "last_a": parsed.get("last_a"),
        "last_b": parsed.get("last_b"),
        "total_a": parsed.get("total_a"),
        "total_b": parsed.get("total_b"),
        "delta": parsed.get("delta"),
        "max": parsed.get("max"),
        "permille": permille,
        "total_delta": parsed.get("total_delta"),
        "total_max": parsed.get("total_max"),
        "total_permille": parsed.get("total_permille"),
        "instructions_per_cycle": parsed.get("instructions_per_cycle"),
        "total_instructions_per_cycle": parsed.get("total_instructions_per_cycle"),
        "a_per_million_cycles": parsed.get("a_per_million_cycles"),
        "total_a_per_million_cycles": parsed.get("total_a_per_million_cycles"),
        "output_file": str(output_file) if output_file is not None else None,
        "raw_sha256": sha256_file(output_file) if output_file is not None else None,
        "command_text": command_result.get("command_text"),
        "stdout_tail": tail_text(command_result.get("stdout", "")),
        "stderr_tail": tail_text(command_result.get("stderr", "")),
        "parsed": parsed,
        "command": command_result,
    }
    return record


def run_kyua_debug_case(
    args: argparse.Namespace,
    *,
    run_id: str,
    iteration: int,
    kyua_dir: Path,
) -> Dict[str, Any]:
    command = [
        "kyua",
        "-v",
        "test_suites.FreeBSD.amd.pmc.grouping.runtime=true",
        "-v",
        (
            "test_suites.FreeBSD.amd.pmc.grouping."
            f"cycle_tolerance_permille={args.tolerance_permille}"
        ),
        "debug",
        KYUA_TEST_NAME,
    ]
    result = run_command(
        command_with_sudo(
            command,
            args.use_sudo,
            args.sudo_cmd,
            args.sudo_non_interactive,
        ),
        cwd=kyua_dir,
        dry_run=args.dry_run,
        timeout_seconds=bounded_timeout(args.deadline, args.kyua_timeout),
        grace_seconds=args.command_grace_seconds,
        progress_label=f"kyua debug iteration {iteration}",
    )
    combined = f"{result.get('stdout', '')}\n{result.get('stderr', '')}"
    parsed = parse_skew_from_text(combined)
    return flatten_case_record(
        run_id=run_id,
        iteration=iteration,
        case="kyua_debug",
        command_result=result,
        tolerance=args.tolerance_permille,
        event_a=PMSTAT_DUPLICATE_EVENT,
        event_b=PMSTAT_DUPLICATE_EVENT,
        parsed=parsed,
    )


def run_pmcstat_pair_case(
    args: argparse.Namespace,
    *,
    run_id: str,
    iteration: int,
    case: str,
    event_a: str,
    event_b: str,
    raw_dir: Path,
) -> Dict[str, Any]:
    output_file = raw_dir / f"{iteration:05d}-{case}.out"
    tmp_output_file = output_file if args.dry_run else unique_tmp_path(output_file, "out")
    command = [
        "pmcstat",
        "-C",
        "-q",
        "-p",
        event_a,
        "-p",
        event_b,
        "-o",
        str(tmp_output_file),
        "--",
        "sleep",
        str(args.sleep_seconds),
    ]
    result = run_command(
        command_with_sudo(
            command,
            args.use_sudo,
            args.sudo_cmd,
            args.sudo_non_interactive,
        ),
        dry_run=args.dry_run,
        timeout_seconds=bounded_timeout(
            args.deadline,
            args.sleep_seconds + args.command_timeout_overhead,
        ),
        grace_seconds=args.command_grace_seconds,
        progress_label=f"{case} iteration {iteration}",
    )

    if tmp_output_file.exists() and result.get("returncode") == 0:
        tmp_output_file.replace(output_file)
        fsync_directory(output_file.parent)
    elif not args.dry_run:
        for stale in (tmp_output_file, output_file):
            try:
                stale.unlink()
            except OSError:
                pass

    output_text = "" if args.dry_run else read_text_file(output_file)
    parsed = parse_pmcstat_output(
        output_text,
        event_a,
        event_b,
        args.min_cycle_count,
    )

    record = flatten_case_record(
        run_id=run_id,
        iteration=iteration,
        case=case,
        command_result=result,
        tolerance=args.tolerance_permille,
        event_a=event_a,
        event_b=event_b,
        output_file=None if args.dry_run else output_file,
        parsed=parsed,
    )

    if args.embed_raw_output:
        record["pmcstat_output"] = output_text

    return record


def run_iteration(
    args: argparse.Namespace,
    *,
    run_id: str,
    iteration: int,
    kyua_dir: Path,
    raw_dir: Path,
) -> List[Dict[str, Any]]:
    records: List[Dict[str, Any]] = []

    if not args.skip_kyua and can_run_kyua(args):
        log_info(f"iteration {iteration}: kyua debug tolerance check")
        records.append(
            run_kyua_debug_case(
                args,
                run_id=run_id,
                iteration=iteration,
                kyua_dir=kyua_dir,
            )
        )
    elif not args.skip_kyua:
        records.append(
            make_skip_record(
                run_id,
                iteration,
                "kyua_debug",
                PMSTAT_DUPLICATE_EVENT,
                PMSTAT_DUPLICATE_EVENT,
                "skip_deadline",
            )
        )

    for case, event_a, event_b in direct_pmcstat_cases(args):
        if STOP_REQUESTED:
            break
        if not can_run_direct_case(args):
            records.append(
                make_skip_record(
                    run_id,
                    iteration,
                    case,
                    event_a,
                    event_b,
                    "skip_deadline",
                )
            )
            continue
        if not case_events_available(args, event_a, event_b):
            records.append(
                make_skip_record(
                    run_id,
                    iteration,
                    case,
                    event_a,
                    event_b,
                    "skip_missing_event",
                )
            )
            continue
        log_info(f"iteration {iteration}: pmcstat {event_a},{event_b}")
        records.append(
            run_pmcstat_pair_case(
                args,
                run_id=run_id,
                iteration=iteration,
                case=case,
                event_a=event_a,
                event_b=event_b,
                raw_dir=raw_dir,
            )
        )

    return records


def direct_pmcstat_cases(args: argparse.Namespace) -> List[Tuple[str, str, str]]:
    return [
        ("duplicate_cycles", PMSTAT_DUPLICATE_EVENT, PMSTAT_DUPLICATE_EVENT),
        (
            "mixed_cycles_instructions",
            PMSTAT_DUPLICATE_EVENT,
            PMSTAT_INSTRUCTION_EVENT,
        ),
        ("mixed_cache_cycles", args.cache_event, PMSTAT_DUPLICATE_EVENT),
    ]


def kyua_needed_seconds(args: argparse.Namespace) -> float:
    return min(float(args.kyua_min_seconds), float(args.kyua_timeout))


def direct_needed_seconds(args: argparse.Namespace) -> float:
    return float(args.sleep_seconds + args.command_timeout_overhead)


def can_run_kyua(args: argparse.Namespace) -> bool:
    return args.dry_run or enough_time_remains(args, kyua_needed_seconds(args))


def can_run_direct_case(args: argparse.Namespace) -> bool:
    return args.dry_run or enough_time_remains(args, direct_needed_seconds(args))


def can_start_iteration(args: argparse.Namespace) -> bool:
    if args.dry_run:
        return True

    if not args.skip_kyua and can_run_kyua(args):
        return True

    if not can_run_direct_case(args):
        return False

    return any(
        case_events_available(args, event_a, event_b)
        for _, event_a, event_b in direct_pmcstat_cases(args)
    )


def enough_time_remains(args: argparse.Namespace, needed: float) -> bool:
    remaining = remaining_seconds(args.deadline)
    if remaining is None:
        return True

    return remaining > max(1.0, needed)


def case_events_available(args: argparse.Namespace, event_a: str, event_b: str) -> bool:
    if args.dry_run:
        return True

    availability = getattr(args, "event_availability", {})

    if not availability:
        return True

    return bool(availability.get(event_a, False) and availability.get(event_b, False))


def make_skip_record(
    run_id: str,
    iteration: int,
    case: str,
    event_a: str,
    event_b: str,
    status: str,
) -> Dict[str, Any]:
    now = utc_now()
    return {
        "run_id": run_id,
        "iteration": iteration,
        "case": case,
        "started_at": now,
        "ended_at": now,
        "elapsed_seconds": 0.0,
        "returncode": None,
        "status": status,
        "command_outcome": status,
        "measurement_valid": False,
        "tolerance_pass": None,
        "accepted": None,
        "event_a": event_a,
        "event_b": event_b,
        "command_text": "",
    }


# ---------------------------------------------------------------------------
# Argument parsing and main loop
# ---------------------------------------------------------------------------

def signal_handler(signum: int, _frame: Any) -> None:
    global SHUTDOWN_REASON, STOP_REQUESTED
    STOP_REQUESTED = True
    SHUTDOWN_REASON = f"signal {signum}"
    COMMAND_RUNNER.terminate_active(signal.SIGTERM)
    log_warn(
        f"received signal {signum}; current command will finish, then the run stops"
    )


def shutdown_exit_code() -> int:
    if SHUTDOWN_REASON is None:
        return 0

    match = re.fullmatch(r"signal (\d+)", SHUTDOWN_REASON)
    if match is None:
        return 0

    return 128 + int(match.group(1))


def build_arg_parser(repo_root: Path) -> argparse.ArgumentParser:
    default_outdir = Path.cwd() / "pmu-skew-data"

    parser = argparse.ArgumentParser(
        description="Collect FreeBSD AMD pmcstat grouping skew data into JSON/CSV.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_collect.py --minutes 30\n"
            "  sudo python3 py-scripts/pmc_grouping_skew_collect.py --once --tolerance-permille 100\n"
            "  python3 py-scripts/pmc_grouping_skew_collect.py --dry-run --once\n"
        ),
    )

    parser.add_argument("--minutes", type=lambda v: parse_positive_float(v, "minutes"),
        default=DEFAULT_MINUTES,
        help=f"collection window in minutes (default: {DEFAULT_MINUTES})")
    parser.add_argument("--iterations", type=lambda v: parse_positive_int(v, "iterations"),
        default=None, help="optional maximum number of complete iterations")
    parser.add_argument("--once", action="store_true",
        help="run exactly one complete iteration")

    parser.add_argument("--sleep-seconds", type=lambda v: parse_positive_int(v, "sleep-seconds"),
        default=DEFAULT_SLEEP_SECONDS,
        help=f"sleep workload duration for direct pmcstat commands (default: {DEFAULT_SLEEP_SECONDS})")
    parser.add_argument("--tolerance-permille", type=lambda v: parse_positive_int(v, "tolerance-permille"),
        default=DEFAULT_TOLERANCE_PERMILLE,
        help=f"duplicate-cycle acceptance tolerance (default: {DEFAULT_TOLERANCE_PERMILLE})")
    parser.add_argument("--min-cycle-count", type=lambda v: parse_positive_int(v, "min-cycle-count"),
        default=DEFAULT_MIN_CYCLE_COUNT,
        help=f"minimum duplicate-cycle denominator for valid samples (default: {DEFAULT_MIN_CYCLE_COUNT})")
    parser.add_argument("--cache-event", default=PMSTAT_CACHE_EVENT,
        help=f"cache-side event paired with cycles (default: {PMSTAT_CACHE_EVENT})")
    parser.add_argument("--no-cache-event-fallback", action="store_true",
        help="use only --cache-event instead of the shell-test fallback event list")

    parser.add_argument("--outdir", type=Path, default=default_outdir,
        help="directory for JSON, CSV, and raw pmcstat outputs (default: ./pmu-skew-data)")
    parser.add_argument("--kyua-dir", type=Path, default=None,
        help="directory containing pmcstat_grouping_test Kyuafile")
    parser.add_argument("--skip-kyua", action="store_true",
        help="skip the Kyua debug command and collect only direct pmcstat samples")

    parser.add_argument("--sudo-cmd", default="sudo",
        help="sudo command used when not running as root (default: sudo)")
    parser.add_argument("--sudo-interactive", dest="sudo_non_interactive",
        action="store_false", help="allow interactive sudo prompts")
    parser.add_argument("--no-sudo", dest="use_sudo", action="store_false",
        help="do not prefix commands with sudo when not root")
    parser.set_defaults(use_sudo=True, sudo_non_interactive=True)

    parser.add_argument("--kyua-timeout", type=lambda v: parse_positive_int(v, "kyua-timeout"),
        default=DEFAULT_KYUA_TIMEOUT_SECONDS,
        help=f"maximum seconds for each Kyua debug command (default: {DEFAULT_KYUA_TIMEOUT_SECONDS})")
    parser.add_argument("--kyua-min-seconds", type=lambda v: parse_positive_int(v, "kyua-min-seconds"),
        default=DEFAULT_KYUA_MIN_SECONDS,
        help=f"minimum remaining seconds before starting Kyua debug (default: {DEFAULT_KYUA_MIN_SECONDS})")
    parser.add_argument("--preflight-timeout", type=lambda v: parse_positive_int(v, "preflight-timeout"),
        default=30, help="maximum seconds for setup/probe commands (default: 30)")
    parser.add_argument("--command-timeout-overhead", type=lambda v: parse_positive_int(v, "command-timeout-overhead"),
        default=30, help="seconds added to each pmcstat workload timeout (default: 30)")
    parser.add_argument("--command-grace-seconds", type=lambda v: parse_positive_int(v, "command-grace-seconds"),
        default=DEFAULT_COMMAND_GRACE_SECONDS,
        help=f"SIGTERM grace period before SIGKILL (default: {DEFAULT_COMMAND_GRACE_SECONDS})")
    parser.add_argument("--no-threaded-writer", dest="threaded_writer", action="store_false",
        help="write JSON/CSV checkpoints synchronously")
    parser.set_defaults(threaded_writer=True)

    parser.add_argument("--embed-raw-output", action="store_true",
        help="embed full raw pmcstat output text inside JSON records")
    parser.add_argument("--color", choices=("auto", "always", "never"),
        default="auto", help="ANSI color mode for terminal output (default: auto)")
    parser.add_argument("--live-graph", choices=("auto", "always", "never"),
        default="auto", help="live progress graph while Kyua/pmcstat commands run")
    parser.add_argument("-v", "--verbose", action="store_true",
        help="print additional command and environment diagnostics")
    parser.add_argument("--quiet", action="store_true",
        help="suppress per-case terminal graph lines")
    parser.add_argument("--dry-run", action="store_true",
        help="print commands and write an empty run skeleton without executing PMCs")

    parser.add_argument("--no-hwpmc-load", action="store_true",
        help="do not attempt to kldload hwpmc before collection")
    parser.add_argument("--allow-non-freebsd", action="store_true",
        help="allow execution on non-FreeBSD hosts for command-shape inspection")
    parser.add_argument("--allow-non-amd", action="store_true",
        help="allow execution when kern.hwpmc.cpuid does not report AuthenticAMD")
    parser.add_argument("--allow-unknown-generation", action="store_true",
        help="allow unknown/future AMD generation after manual PPR verification")

    return parser


def main() -> int:
    script_path = Path(__file__).resolve()
    repo_root = script_path.parent.parent
    parser = build_arg_parser(repo_root)
    args = parser.parse_args()
    configure_terminal(args)
    if args.once:
        args.iterations = 1
    if args.dry_run and args.iterations is None:
        args.iterations = 1
    args.outdir = args.outdir.expanduser().resolve()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    if not args.dry_run:
        check_tool("pmcstat")

        if not args.skip_kyua:
            check_tool("kyua")

        if args.use_sudo and not is_root():
            check_tool(args.sudo_cmd)

            if args.sudo_non_interactive:
                sudo_check = run_command(
                    [args.sudo_cmd, "-n", "true"],
                    timeout_seconds=args.preflight_timeout,
                    grace_seconds=args.command_grace_seconds,
                )
                if sudo_check.get("returncode") != 0:
                    die("sudo -n true failed; run as root or use --sudo-interactive")

    args.outdir.mkdir(parents=True, exist_ok=True)
    raw_dir = args.outdir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.outdir / "pmc-grouping-skew.json"
    csv_path = args.outdir / "pmc-grouping-skew.csv"

    hwpmc_status = ensure_hwpmc_loaded(args)
    if (
        hwpmc_status.get("error")
        and not args.dry_run
        and platform.system() == "FreeBSD"
    ):
        die(f"hwpmc unavailable: {hwpmc_status['error']}")
    platform_info = collect_platform_info(args)
    validate_platform(platform_info, args)
    kyua_dir = resolve_kyua_dir(args, repo_root)
    if (
        not args.skip_kyua
        and not args.dry_run
        and not (kyua_dir / "Kyuafile").exists()
    ):
        die(f"Kyuafile not found in Kyua directory: {kyua_dir}")

    cpuid = platform_info.get("parsed_cpuid", {})
    log_info(
        "target CPU: "
        f"{cpuid.get('generation')} family={cpuid.get('family_hex')} "
        f"model={cpuid.get('model_hex')} stepping={cpuid.get('stepping')} "
        f"PPR={cpuid.get('ppr')}"
    )
    if not args.skip_kyua:
        log_info(f"kyua directory: {kyua_dir}")

    events = collect_event_list(args)
    if events and not args.no_cache_event_fallback:
        chosen_cache_event = choose_cache_event(events, args.cache_event)
        if chosen_cache_event is not None:
            args.cache_event = chosen_cache_event
    required_events = [
        PMSTAT_DUPLICATE_EVENT,
        PMSTAT_INSTRUCTION_EVENT,
        args.cache_event,
    ]
    event_availability = check_event_availability(events, required_events)
    args.event_availability = event_availability
    missing = [
        event for event, available in event_availability.items()
        if not available
    ]
    if missing and not args.dry_run:
        log_warn(f"pmcstat -L does not list events: {', '.join(missing)}")

    run_id = uuid.uuid4().hex
    state: Dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "producer": PRODUCER,
        "run_id": run_id,
        "repo_root": str(repo_root),
        "started_at": utc_now(),
        "ended_at": None,
        "iterations_completed": 0,
        "configuration": {
            "minutes": args.minutes,
            "iterations": args.iterations,
            "sleep_seconds": args.sleep_seconds,
            "tolerance_permille": args.tolerance_permille,
            "min_cycle_count": args.min_cycle_count,
            "cache_event": args.cache_event,
            "cache_event_candidates": list(PMSTAT_CACHE_EVENT_CANDIDATES),
            "kyua_dir": str(kyua_dir),
            "skip_kyua": args.skip_kyua,
            "use_sudo": args.use_sudo,
            "sudo_cmd": args.sudo_cmd,
            "sudo_non_interactive": args.sudo_non_interactive,
            "kyua_timeout": args.kyua_timeout,
            "kyua_min_seconds": args.kyua_min_seconds,
            "command_timeout_overhead": args.command_timeout_overhead,
            "threaded_writer": args.threaded_writer,
            "color": args.color,
            "live_graph": args.live_graph,
            "verbose": args.verbose,
            "dry_run": args.dry_run,
        },
        "platform": platform_info,
        "hwpmc": hwpmc_status,
        "event_availability": event_availability,
        "event_list_error": getattr(args, "event_list_error", None),
        "records": [],
        "summary": {},
    }
    writer = CheckpointWriter(json_path, csv_path, args.threaded_writer)
    writer.start()
    writer.write(state)

    deadline = time.monotonic() + args.minutes * 60.0
    args.deadline = deadline
    iteration = 0
    try:
        while not STOP_REQUESTED:
            if args.iterations is not None and iteration >= args.iterations:
                break
            if time.monotonic() >= deadline:
                break
            if not can_start_iteration(args):
                log_verbose("stopping: insufficient time or runnable events for another probe")
                break

            iteration += 1
            log_info(f"starting iteration {iteration}")
            records = run_iteration(
                args,
                run_id=run_id,
                iteration=iteration,
                kyua_dir=kyua_dir,
                raw_dir=raw_dir,
            )
            state["records"].extend(records)
            state["iterations_completed"] = iteration
            writer.write(state)

            if not args.quiet:
                for record in records:
                    print_record_line(record, args.tolerance_permille)

        state["ended_at"] = utc_now()
        if SHUTDOWN_REASON is not None:
            state["shutdown_reason"] = SHUTDOWN_REASON
        writer.write(state)
        writer.close()
        print_final_summary(state, json_path, csv_path)
        return shutdown_exit_code()
    except KeyboardInterrupt:
        state["ended_at"] = utc_now()
        state["shutdown_reason"] = "keyboard interrupt"
        writer.write(state)
        writer.close()
        print_final_summary(state, json_path, csv_path)
        return 130
    except BaseException:
        state["ended_at"] = utc_now()
        state["shutdown_reason"] = "exception"
        writer.write(state)
        writer.close()
        raise

if __name__ == "__main__":
    sys.exit(main())
