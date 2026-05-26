#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Terminal logging, ANSI color policy, and live progress rendering helpers.

"""Terminal logger with optional ANSI color and live progress rendering."""

from __future__ import annotations

import sys
import threading
import time
from typing import Callable, Optional, Tuple


COLORS = {
    "red": "\033[0;31m",
    "green": "\033[0;32m",
    "yellow": "\033[1;33m",
    "blue": "\033[0;34m",
    "purple": "\033[0;35m",
    "cyan": "\033[0;36m",
    "bold": "\033[1m",
    "dim": "\033[2m",
    "reset": "\033[0m",
}


class Terminal:
    """Small logger/progress renderer with deterministic stdout/stderr usage."""

    def __init__(self, prefix: str) -> None:
        self.prefix = prefix
        self.color_enabled = False
        self.verbose_enabled = False
        self.live_graph_enabled = False
        self._log_lock = threading.Lock()
        self._suspend_hook: Optional[Callable[[], None]] = None
        self._resume_hook: Optional[Callable[[], None]] = None

    def set_dashboard_hooks(
        self,
        suspend: Optional[Callable[[], None]],
        resume: Optional[Callable[[], None]],
    ) -> None:
        self._suspend_hook = suspend
        self._resume_hook = resume

    def has_dashboard(self) -> bool:
        return self._suspend_hook is not None

    def _emit(self, stream, line: str) -> None:
        with self._log_lock:
            if self._suspend_hook is not None:
                self._suspend_hook()
            print(line, file=stream, flush=True)
            if self._resume_hook is not None:
                self._resume_hook()

    def configure(
        self,
        *,
        color: str,
        live_graph: str,
        verbose: bool,
        quiet: bool,
    ) -> None:
        if color == "always":
            self.color_enabled = True
        elif color == "never":
            self.color_enabled = False
        else:
            self.color_enabled = sys.stdout.isatty()

        self.verbose_enabled = verbose

        if live_graph == "always":
            self.live_graph_enabled = True
        elif live_graph == "never":
            self.live_graph_enabled = False
        else:
            self.live_graph_enabled = sys.stderr.isatty() and not quiet

    def colorize(self, text: str, color: str) -> str:
        if not self.color_enabled or color not in COLORS:
            return text

        return f"{COLORS[color]}{text}{COLORS['reset']}"

    def info(self, message: str) -> None:
        label = self.colorize("INFO:", "blue")
        self._emit(sys.stdout, f"[{self.prefix}] {label}  {message}")

    def warn(self, message: str) -> None:
        label = self.colorize("WARN:", "yellow")
        self._emit(sys.stderr, f"[{self.prefix}] {label}  {message}")

    def error(self, message: str) -> None:
        label = self.colorize("ERROR:", "red")
        self._emit(sys.stderr, f"[{self.prefix}] {label} {message}")

    def verbose(self, message: str, *, noisy: bool = False) -> None:
        if not self.verbose_enabled:
            return
        if noisy and self.has_dashboard():
            return
        label = self.colorize("VERBOSE:", "purple")
        self._emit(sys.stdout, f"[{self.prefix}] {label} {message}")

    @staticmethod
    def progress_bar(elapsed: float, timeout: Optional[float], width: int = 28) -> str:
        if timeout is not None and timeout > 0:
            ratio = max(0.0, min(1.0, elapsed / timeout))
            filled = int(round(ratio * width))
            return "#" * filled + "-" * (width - filled)

        cursor = int(elapsed * 4) % width
        chars = ["-"] * width
        chars[cursor] = "#"

        return "".join(chars)

    def _live_progress(
        self,
        label: str,
        started_mono: float,
        timeout: Optional[float],
        stop: threading.Event,
    ) -> None:
        while not stop.wait(0.25):
            elapsed = time.monotonic() - started_mono
            bar = self.progress_bar(elapsed, timeout)
            if timeout is not None and timeout > 0:
                pct = min(100.0, (elapsed * 100.0) / timeout)
                suffix = f" {elapsed:5.1f}s/{timeout:.0f}s {pct:5.1f}%"
            else:
                suffix = f" {elapsed:5.1f}s"
            line = (
                f"\r{self.colorize('[RUN]', 'cyan')} {label:<34} "
                f"[{self.colorize(bar, 'green')}]{suffix}"
            )
            sys.stderr.write(line)
            sys.stderr.flush()

        sys.stderr.write("\r\033[K")
        sys.stderr.flush()

    def start_progress(
        self,
        label: Optional[str],
        started_mono: float,
        timeout: Optional[float],
    ) -> Tuple[Optional[threading.Event], Optional[threading.Thread]]:
        if not label or not self.live_graph_enabled or self.has_dashboard():
            return None, None

        stop = threading.Event()
        thread = threading.Thread(
            target=self._live_progress,
            args=(label, started_mono, timeout, stop),
            daemon=True,
        )
        thread.start()

        return stop, thread

    @staticmethod
    def stop_progress(
        stop: Optional[threading.Event],
        thread: Optional[threading.Thread],
    ) -> None:
        if stop is not None:
            stop.set()

        if thread is not None:
            thread.join(timeout=1.0)
