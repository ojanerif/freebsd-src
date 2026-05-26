#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Reusable command execution, timeout, sudo argv, and tool lookup helpers.

"""Command helpers for argv execution, sudo wrapping, and pgid-safe timeouts."""

from __future__ import annotations

import math
import os
import shlex
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Mapping, Optional, Sequence

from .fs import utc_now
from .terminal import Terminal


def command_to_string(argv: Sequence[str]) -> str:
    return " ".join(shlex.quote(str(arg)) for arg in argv)


def is_root() -> bool:
    return hasattr(os, "geteuid") and os.geteuid() == 0


def finite_timeout(value: Optional[float]) -> Optional[float]:
    if value is None or value <= 0 or not math.isfinite(value):
        return None

    return value


def remaining_seconds(deadline: Optional[float]) -> Optional[float]:
    if deadline is None:
        return None

    return max(0.0, deadline - time.monotonic())


def bounded_timeout(deadline: Optional[float], preferred: Optional[float]) -> Optional[float]:
    preferred = finite_timeout(preferred)
    remaining = remaining_seconds(deadline)
    if remaining is None:
        return preferred

    if preferred is None:
        return remaining

    return min(remaining, preferred)


@dataclass(frozen=True)
class SudoConfig:
    """Build sudo-prefixed argv lists without using shell strings."""

    use_sudo: bool = True
    sudo_cmd: str = "sudo"
    non_interactive: bool = True

    def apply(self, argv: Sequence[str]) -> list[str]:
        """Return argv with sudo prepended when requested and not already root."""
        if not self.use_sudo or is_root():
            return [str(arg) for arg in argv]

        sudo_argv = [self.sudo_cmd]

        if self.non_interactive:
            sudo_argv.append("-n")

        return sudo_argv + [str(arg) for arg in argv]


class CommandRunner:
    """Run commands with timeouts, live progress hooks, and pgid cleanup."""

    def __init__(self, terminal: Terminal) -> None:
        self.terminal = terminal
        self.active_pgid: Optional[int] = None

    def run(
        self,
        argv: Sequence[str],
        *,
        cwd: Optional[Path] = None,
        env: Optional[Mapping[str, str]] = None,
        dry_run: bool = False,
        timeout_seconds: Optional[float] = None,
        grace_seconds: float = 5,
        progress_label: Optional[str] = None,
        inherit_stdin: bool = False,
    ) -> Dict[str, Any]:
        argv_list = [str(arg) for arg in argv]
        started_mono = time.monotonic()
        result: Dict[str, Any] = {
            "command": argv_list,
            "command_text": command_to_string(argv_list),
            "cwd": str(cwd) if cwd is not None else None,
            "started_at": utc_now(),
            "ended_at": None,
            "elapsed_seconds": None,
            "returncode": None,
            "stdout": "",
            "stderr": "",
            "timeout_seconds": timeout_seconds,
            "timed_out": False,
            "terminated_by_signal": None,
            "dry_run": dry_run,
            "outcome": "not_run" if dry_run else "unknown",
        }

        timeout_seconds = finite_timeout(timeout_seconds)
        grace_seconds = finite_timeout(grace_seconds) or 0.0
        result["timeout_seconds"] = timeout_seconds

        if dry_run:
            self.terminal.info(f"dry-run: {result['command_text']}")
            result["returncode"] = 0
            result["outcome"] = "dry_run"
            result["ended_at"] = utc_now()
            result["elapsed_seconds"] = 0.0
            return result

        proc: Optional[subprocess.Popen[str]] = None
        progress_stop = None
        progress_thread = None
        try:
            self.terminal.verbose(f"exec: {result['command_text']}", noisy=True)
            proc = subprocess.Popen(
                argv_list,
                cwd=str(cwd) if cwd is not None else None,
                env=dict(env) if env is not None else None,
                close_fds=True,
                encoding="utf-8",
                errors="replace",
                stdin=None if inherit_stdin else subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )

            try:
                self.active_pgid = os.getpgid(proc.pid)
            except OSError:
                self.active_pgid = None

            progress_stop, progress_thread = self.terminal.start_progress(
                progress_label,
                started_mono,
                timeout_seconds,
            )

            try:
                stdout, stderr = proc.communicate(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                result["timed_out"] = True
                self._terminate_process_group(proc, signal.SIGTERM)

                try:
                    stdout, stderr = proc.communicate(timeout=grace_seconds)
                except subprocess.TimeoutExpired:
                    self._terminate_process_group(proc, signal.SIGKILL)
                    stdout, stderr = proc.communicate()

            result["stdout"] = stdout or ""
            result["stderr"] = stderr or ""
            result["returncode"] = proc.returncode

            if result["timed_out"]:
                result["outcome"] = "timeout"
            elif proc.returncode == 0:
                result["outcome"] = "ok"
            else:
                result["outcome"] = "failed"

            if proc.returncode is not None and proc.returncode < 0:
                result["terminated_by_signal"] = -proc.returncode
        except OSError as exc:
            result["returncode"] = 127
            result["stderr"] = str(exc)
            result["outcome"] = "exec_error"
        finally:
            self.terminal.stop_progress(progress_stop, progress_thread)
            self.active_pgid = None
            result["ended_at"] = utc_now()
            result["elapsed_seconds"] = round(time.monotonic() - started_mono, 6)
        return result

    def terminate_active(self, sig: int = signal.SIGTERM) -> None:
        if self.active_pgid is None:
            return

        try:
            os.killpg(self.active_pgid, sig)
        except OSError:
            pass

    @staticmethod
    def _terminate_process_group(proc: subprocess.Popen[str], sig: int) -> None:
        try:
            os.killpg(os.getpgid(proc.pid), sig)
        except OSError:
            try:
                proc.send_signal(sig)
            except OSError:
                pass


def simple_command_value(
    argv: Sequence[str],
    *,
    sudo: bool = False,
    sudo_cmd: str = "sudo",
    sudo_non_interactive: bool = True,
    timeout_seconds: Optional[float] = 10.0,
) -> str:
    """Run a small command and return stripped stdout, or an empty string.

    This helper is intentionally conservative: no shell, bounded by default,
    stderr discarded, and failures collapsed to "" for sysctl-style probes.
    """
    command = SudoConfig(
        use_sudo=sudo,
        sudo_cmd=sudo_cmd,
        non_interactive=sudo_non_interactive,
    ).apply(argv)
    try:
        completed = subprocess.run(
            command,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            check=False,
            timeout=finite_timeout(timeout_seconds),
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""

    if completed.returncode != 0:
        return ""

    return completed.stdout.strip()


def check_tool(name: str, *, required: bool = True) -> Optional[str]:
    path = shutil.which(name)

    if path is None and required:
        raise FileNotFoundError(f"required tool not found in PATH: {name}")

    return path
