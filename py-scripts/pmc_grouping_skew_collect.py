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
import hashlib
import json
import math
import os
import platform
import queue
import re
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


DEFAULT_TOLERANCE_PERMILLE = 100
DEFAULT_SLEEP_SECONDS = 5
DEFAULT_MINUTES = 10.0
DEFAULT_KYUA_TIMEOUT_SECONDS = 120
DEFAULT_COMMAND_GRACE_SECONDS = 5
DEFAULT_MIN_CYCLE_COUNT = 1
SCHEMA_VERSION = 2
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

CPUID_RE = re.compile(r"^(?P<vendor>[^-]+)-(?P<family>[0-9]+)-(?P<model>[0-9A-Fa-f]+)-(?P<stepping>[0-9]+)$")
SKEW_RE = re.compile(
    r"delta=(?P<delta>[0-9]+(?:\.[0-9]+)?)\s+"
    r"max=(?P<max>[0-9]+(?:\.[0-9]+)?)\s+"
    r"permille=(?P<permille>[0-9]+(?:\.[0-9]+)?)"
)

STOP_REQUESTED = False
SHUTDOWN_REASON: Optional[str] = None
ACTIVE_PGID: Optional[int] = None


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def log_info(message: str) -> None:
    print(f"[pmc-grouping-skew] INFO:  {message}", flush=True)


def log_warn(message: str) -> None:
    print(f"[pmc-grouping-skew] WARN:  {message}", file=sys.stderr, flush=True)


def log_err(message: str) -> None:
    print(f"[pmc-grouping-skew] ERROR: {message}", file=sys.stderr, flush=True)


def die(message: str, exit_code: int = 1) -> None:
    log_err(message)
    raise SystemExit(exit_code)


def command_to_string(argv: Sequence[str]) -> str:
    return shlex.join([str(arg) for arg in argv])


def is_root() -> bool:
    geteuid = getattr(os, "geteuid", None)
    return bool(geteuid is not None and geteuid() == 0)


def read_text_file(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def fsync_directory(path: Path) -> None:
    try:
        fd = os.open(str(path), os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_write_text(path: Path, data: str) -> None:
    tmp = unique_tmp_path(path)
    with tmp.open("w", encoding="utf-8") as fp:
        fp.write(data)
        fp.flush()
        os.fsync(fp.fileno())
    tmp.replace(path)
    fsync_directory(path.parent)


def sha256_file(path: Path) -> Optional[str]:
    if not path.exists():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tail_text(text: str, limit: int = 4096) -> str:
    if len(text) <= limit:
        return text
    return text[-limit:]


def unique_tmp_path(path: Path, suffix: str = "tmp") -> Path:
    return path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.{suffix}")


def finite_timeout(value: Optional[float]) -> Optional[float]:
    if value is None:
        return None
    if value <= 0 or not math.isfinite(value):
        return None
    return value


def remaining_seconds(deadline: Optional[float]) -> Optional[float]:
    if deadline is None:
        return None
    return max(0.0, deadline - time.monotonic())


def bounded_timeout(deadline: Optional[float], preferred: Optional[float]) -> Optional[float]:
    remaining = remaining_seconds(deadline)
    preferred = finite_timeout(preferred)
    if remaining is None:
        return preferred
    if preferred is None:
        return remaining
    return min(remaining, preferred)


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
# AMD generation detection
# ---------------------------------------------------------------------------

def map_amd_generation(family: int, model: int) -> Tuple[str, str, str, int]:
    """Return (generation, codename, ppr, pipeline_width)."""

    if family == 0x17:
        if 0x01 <= model <= 0x0F:
            return ("Zen 1 / Zen+", "Naples/Summit Ridge/Pinnacle Ridge", "54945", 6)
        if 0x11 <= model <= 0x1F:
            return ("Zen 1 / Zen+ APU", "Raven Ridge/Picasso", "55570", 6)
        if 0x31 <= model <= 0x3F:
            return ("Zen 2", "Rome/Castle Peak", "55898", 6)
        if 0x60 <= model <= 0x6F:
            return ("Zen 2", "Renoir", "56243", 6)
        if 0x71 <= model <= 0x7F:
            return ("Zen 2", "Matisse", "56243", 6)
        if 0x90 <= model <= 0x9F:
            return ("Zen 2", "Van Gogh", "unknown", 6)
        if 0xA0 <= model <= 0xAF:
            return ("Zen 2", "Mendocino", "unknown", 6)
    elif family == 0x19:
        if 0x00 <= model <= 0x0F:
            return ("Zen 3", "Milan", "56569", 6)
        if 0x10 <= model <= 0x1F:
            return ("Zen 4", "Genoa/Bergamo/Siena", "57228", 6)
        if 0x20 <= model <= 0x2F:
            return ("Zen 3", "Vermeer", "56214", 6)
        if 0x40 <= model <= 0x4F:
            return ("Zen 3+", "Rembrandt", "56243", 6)
        if 0x50 <= model <= 0x5F:
            return ("Zen 3", "Cezanne", "56243", 6)
        if 0x60 <= model <= 0x6F:
            return ("Zen 4", "Raphael", "56713", 6)
        if 0x70 <= model <= 0x7F:
            return ("Zen 4", "Phoenix", "56713", 6)
        if 0xA0 <= model <= 0xAF:
            return ("Zen 4", "Genoa SP5/SP6", "57228", 6)
    elif family == 0x1A:
        if 0x00 <= model <= 0x0F:
            return ("Zen 5", "Turin", "57238/58550", 8)
        if 0x10 <= model <= 0x1F:
            return ("Zen 5c", "Turin Dense", "58730", 8)
        if 0x20 <= model <= 0x2F:
            return ("Zen 5", "Strix Point", "57883", 8)
        if 0x40 <= model <= 0x4F:
            return ("Zen 5", "Eldora/Granite Ridge", "57930", 8)
        if 0x50 <= model <= 0x5F:
            return ("Zen 6", "Venice", "69163", 8)
        if 0x60 <= model <= 0x6F:
            return ("Zen 5", "Krackan Point", "unknown", 8)
        if 0x70 <= model <= 0x7F:
            return ("Zen 5", "Strix Halo", "unknown", 8)
        if 0x80 <= model <= 0xAF:
            return ("Zen 6", "future server", "unknown", 8)
        if 0xC0 <= model <= 0xCF:
            return ("Zen 6", "future", "unknown", 8)

    if family > 0x1A:
        return ("future AMD", "unknown", "consult PPR", 0)
    return ("unknown AMD", "unknown", "consult PPR", 0)


def parse_hwpmc_cpuid(cpuid: str) -> Dict[str, Any]:
    match = CPUID_RE.match(cpuid.strip())
    if match is None:
        return {
            "raw": cpuid.strip(),
            "valid": False,
            "vendor": "unknown",
            "family": None,
            "model": None,
            "stepping": None,
            "generation": "unknown",
            "codename": "unknown",
            "ppr": "unknown",
            "pipeline_width": 0,
        }

    vendor = match.group("vendor")
    family = int(match.group("family"), 10)
    model = int(match.group("model"), 16)
    stepping = int(match.group("stepping"), 10)

    if vendor == "AuthenticAMD":
        generation, codename, ppr, pipeline_width = map_amd_generation(family, model)
    else:
        generation = "non-AMD"
        codename = "unknown"
        ppr = "not applicable"
        pipeline_width = 0

    return {
        "raw": cpuid.strip(),
        "valid": True,
        "vendor": vendor,
        "family": family,
        "family_hex": f"0x{family:02x}",
        "model": model,
        "model_hex": f"0x{model:02x}",
        "stepping": stepping,
        "generation": generation,
        "codename": codename,
        "ppr": ppr,
        "pipeline_width": pipeline_width,
    }


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
) -> Dict[str, Any]:
    global ACTIVE_PGID

    started = utc_now()
    started_mono = time.monotonic()
    argv_list = [str(arg) for arg in argv]
    result: Dict[str, Any] = {
        "command": argv_list,
        "command_text": command_to_string(argv_list),
        "cwd": str(cwd) if cwd is not None else None,
        "started_at": started,
        "ended_at": None,
        "elapsed_seconds": None,
        "returncode": None,
        "stdout": "",
        "stderr": "",
        "dry_run": dry_run,
        "timeout_seconds": timeout_seconds,
        "timed_out": False,
        "terminated_by_signal": None,
        "outcome": "not_run" if dry_run else "unknown",
    }

    if dry_run:
        log_info(f"dry-run: {result['command_text']}")
        result["ended_at"] = utc_now()
        result["elapsed_seconds"] = 0.0
        result["returncode"] = 0
        result["outcome"] = "dry_run"
        return result

    proc: Optional[subprocess.Popen[str]] = None
    try:
        proc = subprocess.Popen(
            argv_list,
            cwd=str(cwd) if cwd is not None else None,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            ACTIVE_PGID = os.getpgid(proc.pid)
        except OSError:
            ACTIVE_PGID = None
        try:
            stdout, stderr = proc.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            result["timed_out"] = True
            result["outcome"] = "timeout"
            terminate_process_group(proc, signal.SIGTERM)
            try:
                stdout, stderr = proc.communicate(timeout=grace_seconds)
            except subprocess.TimeoutExpired:
                terminate_process_group(proc, signal.SIGKILL)
                stdout, stderr = proc.communicate()
        result["returncode"] = proc.returncode
        result["stdout"] = stdout or ""
        result["stderr"] = stderr or ""
        if result["outcome"] == "unknown":
            result["outcome"] = "ok" if proc.returncode == 0 else "failed"
        if proc.returncode is not None and proc.returncode < 0:
            result["terminated_by_signal"] = -proc.returncode
    except OSError as exc:
        result["returncode"] = 127
        result["stderr"] = str(exc)
        result["outcome"] = "exec_error"
    finally:
        ACTIVE_PGID = None
        result["ended_at"] = utc_now()
        result["elapsed_seconds"] = round(time.monotonic() - started_mono, 6)
    return result


def terminate_process_group(proc: subprocess.Popen[str], sig: int) -> None:
    try:
        pgid = os.getpgid(proc.pid)
    except OSError:
        try:
            proc.send_signal(sig)
        except OSError:
            pass
        return
    try:
        os.killpg(pgid, sig)
    except OSError:
        pass


def command_with_sudo(argv: Sequence[str], use_sudo: bool, sudo_cmd: str,
    sudo_non_interactive: bool = True) -> List[str]:
    if not use_sudo or is_root():
        return [str(arg) for arg in argv]
    sudo_argv = [sudo_cmd]
    if sudo_non_interactive:
        sudo_argv.append("-n")
    return sudo_argv + [str(arg) for arg in argv]


def simple_command_value(argv: Sequence[str], *, sudo: bool = False,
    sudo_cmd: str = "sudo", sudo_non_interactive: bool = True) -> str:
    command = command_with_sudo(argv, sudo, sudo_cmd, sudo_non_interactive)
    try:
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return ""
    if completed.returncode != 0:
        return ""
    return completed.stdout.strip()


def sysctl_value(name: str) -> str:
    return simple_command_value(["sysctl", "-n", name])


def check_tool(name: str, *, required: bool = True) -> Optional[str]:
    path = shutil.which(name)
    if path is None and required:
        die(f"required tool not found in PATH: {name}")
    return path


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

    loaded = run_command(["kldstat", "-n", "hwpmc"],
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds)
    if loaded["returncode"] == 0:
        status["was_loaded"] = True
        return status

    status["load_attempted"] = True
    load = run_command(command_with_sudo(["kldload", "hwpmc"], args.use_sudo,
        args.sudo_cmd, args.sudo_non_interactive),
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds)
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
    result = run_command(command_with_sudo(["pmcstat", "-L"], args.use_sudo,
        args.sudo_cmd, args.sudo_non_interactive),
        timeout_seconds=args.preflight_timeout,
        grace_seconds=args.command_grace_seconds)
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


def check_event_availability(events: Iterable[str], required: Iterable[str]) -> Dict[str, bool]:
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


def find_matching_header(headers: Sequence[str], event_a: str, event_b: str) -> Optional[str]:
    for header in headers:
        if event_a in header and event_b in header:
            return header
    return None


def parse_pmcstat_output(text: str, event_a: str, event_b: str,
    min_cycle_count: int = DEFAULT_MIN_CYCLE_COUNT) -> Dict[str, Any]:
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
        "event_a": event_a,
        "event_b": event_b,
        "last_a": None,
        "last_b": None,
        "measurement_valid": False,
    }
    if rows:
        last_a, last_b = rows[-1]
        parsed["last_a"] = last_a
        parsed["last_b"] = last_b
        minimum = min_cycle_count if event_a == event_b == PMSTAT_DUPLICATE_EVENT else 1
        parsed["measurement_valid"] = bool(header_valid and last_a >= minimum and last_b >= minimum)

        if event_a == event_b and max(last_a, last_b) > 0:
            delta = abs(last_a - last_b)
            maximum = max(last_a, last_b)
            parsed["delta"] = delta
            parsed["max"] = maximum
            parsed["permille"] = (delta * 1000.0) / maximum
        elif event_a == PMSTAT_DUPLICATE_EVENT and event_b == PMSTAT_INSTRUCTION_EVENT:
            parsed["instructions_per_cycle"] = (last_b / last_a) if last_a > 0 else None
        elif event_b == PMSTAT_DUPLICATE_EVENT:
            parsed["a_per_million_cycles"] = (last_a * 1000000.0 / last_b) if last_b > 0 else None
    return parsed


def percentile(values: Sequence[float], pct: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * pct
    low = math.floor(pos)
    high = math.ceil(pos)
    if low == high:
        return ordered[int(pos)]
    return ordered[low] * (high - pos) + ordered[high] * (pos - low)


def summarize_skew(records: Sequence[Dict[str, Any]], case: str) -> Dict[str, Any]:
    selected = [record for record in records if record.get("case") == case]
    permilles = [float(record["permille"]) for record in selected
        if record.get("permille") is not None]
    accepted = [record for record in selected if record.get("accepted") is True]
    rejected = [record for record in selected if record.get("accepted") is False]
    verdicts = len(accepted) + len(rejected)

    summary: Dict[str, Any] = {
        "case": case,
        "samples": len(selected),
        "samples_with_permille": len(permilles),
        "accepted": len(accepted),
        "rejected": len(rejected),
        "acceptance_rate": (len(accepted) / verdicts) if verdicts else None,
        "min_permille": min(permilles) if permilles else None,
        "max_permille": max(permilles) if permilles else None,
        "mean_permille": statistics.fmean(permilles) if permilles else None,
        "variance_permille": statistics.pvariance(permilles) if len(permilles) > 1 else 0.0,
        "stdev_permille": statistics.pstdev(permilles) if len(permilles) > 1 else 0.0,
        "p50_permille": percentile(permilles, 0.50),
        "p90_permille": percentile(permilles, 0.90),
        "p95_permille": percentile(permilles, 0.95),
    }
    return summary


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
        "case_counts": summarize_case_counts(records),
    }


def summarize_case_counts(records: Sequence[Dict[str, Any]]) -> Dict[str, Dict[str, int]]:
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
    "status",
    "command_outcome",
    "measurement_valid",
    "tolerance_pass",
    "accepted",
    "tolerance_permille",
    "event_a",
    "event_b",
    "row_count",
    "last_a",
    "last_b",
    "delta",
    "max",
    "permille",
    "instructions_per_cycle",
    "a_per_million_cycles",
    "output_file",
    "raw_sha256",
    "command_text",
]


def record_to_csv_row(record: Dict[str, Any]) -> Dict[str, Any]:
    return {column: record.get(column, "") for column in CSV_COLUMNS}


def write_csv_records(path: Path, records: Sequence[Dict[str, Any]]) -> None:
    tmp = unique_tmp_path(path)
    with tmp.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=CSV_COLUMNS)
        writer.writeheader()
        for record in records:
            writer.writerow(record_to_csv_row(record))
        fp.flush()
        os.fsync(fp.fileno())
    tmp.replace(path)
    fsync_directory(path.parent)


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
        self._thread = threading.Thread(target=self._worker,
            name="pmc-skew-checkpoint-writer", daemon=True)
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
        self._queue.put(None)
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


def print_record_line(record: Dict[str, Any], tolerance: int) -> None:
    case = record.get("case", "unknown")
    permille = record.get("permille")
    accepted = record.get("accepted")
    status = "PASS" if accepted is True else "FAIL" if accepted is False else "INFO"
    if permille is None:
        log_info(f"iteration {record.get('iteration')} {case}: {status}")
        return
    bar = graph_bar(float(permille), tolerance)
    log_info(
        f"iteration {record.get('iteration')} {case}: {status} "
        f"permille={float(permille):.3f} tolerance={tolerance} [{bar}]"
    )


def print_final_summary(state: Dict[str, Any], json_path: Path, csv_path: Path) -> None:
    summary = state.get("summary", build_summary(state))
    if not summary:
        summary = build_summary(state)
    log_info("=== skew summary ===")
    for case, data in summary.get("skew_cases", {}).items():
        samples = data.get("samples", 0)
        if samples == 0:
            log_info(f"{case}: no samples")
            continue
        rate = data.get("acceptance_rate")
        rate_s = "n/a" if rate is None else f"{rate * 100.0:.2f}%"
        mean = data.get("mean_permille")
        p95 = data.get("p95_permille")
        maximum = data.get("max_permille")
        mean_s = "n/a" if mean is None else f"{mean:.3f}"
        p95_s = "n/a" if p95 is None else f"{p95:.3f}"
        max_s = "n/a" if maximum is None else f"{maximum:.3f}"
        log_info(
            f"{case}: samples={samples} accepted={data.get('accepted')} "
            f"rejected={data.get('rejected')} acceptance={rate_s} "
            f"mean={mean_s} p95={p95_s} max={max_s}"
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
    measurement_valid = bool(parsed.get("measurement_valid"))
    tolerance_pass: Optional[bool] = None
    accepted: Optional[bool]
    status: str
    if command_result.get("dry_run"):
        accepted = None
        status = "dry_run"
    elif case in ("kyua_debug", "duplicate_cycles"):
        valid_for_tolerance = measurement_valid or case == "kyua_debug"
        tolerance_pass = bool(command_ok and valid_for_tolerance and permille is not None and
            float(permille) <= tolerance)
        accepted = tolerance_pass
        if not command_ok:
            status = str(command_result.get("outcome", "command_error"))
        elif permille is None or not valid_for_tolerance:
            status = "invalid_output"
        else:
            status = "pass" if tolerance_pass else "fail_tolerance"
    elif command_ok:
        rows = parsed.get("row_count")
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
        "status": status,
        "command_outcome": command_result.get("outcome"),
        "measurement_valid": measurement_valid,
        "tolerance_pass": tolerance_pass,
        "accepted": accepted,
        "tolerance_permille": tolerance,
        "event_a": event_a,
        "event_b": event_b,
        "row_count": parsed.get("row_count"),
        "last_a": parsed.get("last_a"),
        "last_b": parsed.get("last_b"),
        "delta": parsed.get("delta"),
        "max": parsed.get("max"),
        "permille": permille,
        "instructions_per_cycle": parsed.get("instructions_per_cycle"),
        "a_per_million_cycles": parsed.get("a_per_million_cycles"),
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
        f"test_suites.FreeBSD.amd.pmc.grouping.cycle_tolerance_permille={args.tolerance_permille}",
        "debug",
        KYUA_TEST_NAME,
    ]
    result = run_command(
        command_with_sudo(command, args.use_sudo, args.sudo_cmd,
            args.sudo_non_interactive),
        cwd=kyua_dir,
        dry_run=args.dry_run,
        timeout_seconds=bounded_timeout(args.deadline, args.kyua_timeout),
        grace_seconds=args.command_grace_seconds,
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
        command_with_sudo(command, args.use_sudo, args.sudo_cmd,
            args.sudo_non_interactive),
        dry_run=args.dry_run,
        timeout_seconds=bounded_timeout(args.deadline,
            args.sleep_seconds + args.command_timeout_overhead),
        grace_seconds=args.command_grace_seconds,
    )
    if tmp_output_file.exists() and result.get("returncode") == 0:
        tmp_output_file.replace(output_file)
        fsync_directory(output_file.parent)
    elif output_file.exists() and not args.dry_run:
        output_file.unlink()
    output_text = read_text_file(output_file)
    parsed = parse_pmcstat_output(output_text, event_a, event_b,
        args.min_cycle_count)
    record = flatten_case_record(
        run_id=run_id,
        iteration=iteration,
        case=case,
        command_result=result,
        tolerance=args.tolerance_permille,
        event_a=event_a,
        event_b=event_b,
        output_file=output_file,
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

    if not args.skip_kyua and enough_time_remains(args, 1.0):
        log_info(f"iteration {iteration}: kyua debug tolerance check")
        records.append(run_kyua_debug_case(args, run_id=run_id,
            iteration=iteration, kyua_dir=kyua_dir))
    elif not args.skip_kyua:
        records.append(make_skip_record(run_id, iteration, "kyua_debug",
            PMSTAT_DUPLICATE_EVENT, PMSTAT_DUPLICATE_EVENT,
            "skip_deadline"))

    direct_cases = [
        ("duplicate_cycles", PMSTAT_DUPLICATE_EVENT, PMSTAT_DUPLICATE_EVENT),
        ("mixed_cycles_instructions", PMSTAT_DUPLICATE_EVENT,
            PMSTAT_INSTRUCTION_EVENT),
        ("mixed_cache_cycles", args.cache_event, PMSTAT_DUPLICATE_EVENT),
    ]
    for case, event_a, event_b in direct_cases:
        if STOP_REQUESTED:
            break
        if not enough_time_remains(args, args.sleep_seconds + args.command_timeout_overhead):
            records.append(make_skip_record(run_id, iteration, case, event_a,
                event_b, "skip_deadline"))
            continue
        if not case_events_available(args, event_a, event_b):
            records.append(make_skip_record(run_id, iteration, case, event_a,
                event_b, "skip_missing_event"))
            continue
        log_info(f"iteration {iteration}: pmcstat {event_a},{event_b}")
        records.append(run_pmcstat_pair_case(args, run_id=run_id,
            iteration=iteration, case=case, event_a=event_a, event_b=event_b,
            raw_dir=raw_dir))

    return records


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


def make_skip_record(run_id: str, iteration: int, case: str, event_a: str,
    event_b: str, status: str) -> Dict[str, Any]:
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
    if ACTIVE_PGID is not None:
        try:
            os.killpg(ACTIVE_PGID, signal.SIGTERM)
        except OSError:
            pass
    log_warn(f"received signal {signum}; current command will finish, then the run stops")


def build_arg_parser(repo_root: Path) -> argparse.ArgumentParser:
    default_outdir = repo_root / "work" / f"pmc-grouping-skew-{datetime.now().strftime('%Y%m%d-%H%M%S-%f')}"
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
        default=DEFAULT_MINUTES, help=f"collection window in minutes (default: {DEFAULT_MINUTES})")
    parser.add_argument("--iterations", type=lambda v: parse_positive_int(v, "iterations"),
        default=None, help="optional maximum number of complete iterations")
    parser.add_argument("--once", action="store_true",
        help="run exactly one complete iteration")
    parser.add_argument("--sleep-seconds", type=lambda v: parse_positive_int(v, "sleep-seconds"),
        default=DEFAULT_SLEEP_SECONDS,
        help=f"sleep workload duration for each direct pmcstat command (default: {DEFAULT_SLEEP_SECONDS})")
    parser.add_argument("--tolerance-permille", type=lambda v: parse_positive_int(v, "tolerance-permille"),
        default=DEFAULT_TOLERANCE_PERMILLE,
        help=f"duplicate-cycle acceptance tolerance (default: {DEFAULT_TOLERANCE_PERMILLE})")
    parser.add_argument("--min-cycle-count", type=lambda v: parse_positive_int(v, "min-cycle-count"),
        default=DEFAULT_MIN_CYCLE_COUNT,
        help=f"minimum duplicate-cycle denominator for valid skew samples (default: {DEFAULT_MIN_CYCLE_COUNT})")
    parser.add_argument("--cache-event", default=PMSTAT_CACHE_EVENT,
        help=f"cache-side event paired with cycles (default: {PMSTAT_CACHE_EVENT})")
    parser.add_argument("--no-cache-event-fallback", action="store_true",
        help="use only --cache-event instead of the shell-test fallback event list")
    parser.add_argument("--outdir", type=Path, default=default_outdir,
        help="directory for JSON, CSV, and raw pmcstat outputs")
    parser.add_argument("--kyua-dir", type=Path, default=None,
        help="directory containing pmcstat_grouping_test Kyuafile (default: /usr/tests/sys/amd/pmc if present)")
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
    parser.add_argument("--no-hwpmc-load", action="store_true",
        help="do not attempt to kldload hwpmc before collection")
    parser.add_argument("--allow-non-freebsd", action="store_true",
        help="allow execution on non-FreeBSD hosts for command-shape inspection")
    parser.add_argument("--allow-non-amd", action="store_true",
        help="allow execution when kern.hwpmc.cpuid does not report AuthenticAMD")
    parser.add_argument("--allow-unknown-generation", action="store_true",
        help="allow execution on unknown/future AMD generation after manual PPR verification")
    parser.add_argument("--quiet", action="store_true",
        help="suppress per-case terminal graph lines")
    parser.add_argument("--dry-run", action="store_true",
        help="print commands and write an empty run skeleton without executing PMCs")
    return parser


def main() -> int:
    script_path = Path(__file__).resolve()
    repo_root = script_path.parent.parent
    parser = build_arg_parser(repo_root)
    args = parser.parse_args()
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
                sudo_check = run_command([args.sudo_cmd, "-n", "true"],
                    timeout_seconds=args.preflight_timeout,
                    grace_seconds=args.command_grace_seconds)
                if sudo_check.get("returncode") != 0:
                    die("sudo -n true failed; run as root or use --sudo-interactive")

    args.outdir.mkdir(parents=True, exist_ok=True)
    raw_dir = args.outdir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.outdir / "pmc-grouping-skew.json"
    csv_path = args.outdir / "pmc-grouping-skew.csv"

    hwpmc_status = ensure_hwpmc_loaded(args)
    if hwpmc_status.get("error") and not args.dry_run and platform.system() == "FreeBSD":
        die(f"hwpmc unavailable: {hwpmc_status['error']}")
    platform_info = collect_platform_info(args)
    validate_platform(platform_info, args)
    kyua_dir = resolve_kyua_dir(args, repo_root)
    if not args.skip_kyua and not args.dry_run and not (kyua_dir / "Kyuafile").exists():
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
    required_events = [PMSTAT_DUPLICATE_EVENT, PMSTAT_INSTRUCTION_EVENT, args.cache_event]
    event_availability = check_event_availability(events, required_events)
    args.event_availability = event_availability
    missing = [event for event, available in event_availability.items() if not available]
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
            "command_timeout_overhead": args.command_timeout_overhead,
            "threaded_writer": args.threaded_writer,
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

            iteration += 1
            log_info(f"starting iteration {iteration}")
            records = run_iteration(args, run_id=run_id, iteration=iteration,
                kyua_dir=kyua_dir, raw_dir=raw_dir)
            state["records"].extend(records)
            state["iterations_completed"] = iteration
            writer.write(state)
            if not args.quiet:
                for record in records:
                    print_record_line(record, args.tolerance_permille)

        state["ended_at"] = utc_now()
        writer.write(state)
        writer.close()
        print_final_summary(state, json_path, csv_path)
    except KeyboardInterrupt:
        state["ended_at"] = utc_now()
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

    return 0


if __name__ == "__main__":
    sys.exit(main())
