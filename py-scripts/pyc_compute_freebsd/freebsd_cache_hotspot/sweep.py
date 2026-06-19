#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

from __future__ import annotations

"""Parallel per-CCX cache-latency sweep with live terminal reports."""

import math
import os
import signal
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Sequence

from freebsd_cache_hotspot import _fbcacheprobe as _fb
from freebsd_cache_hotspot.topology import Topology, fmt_kb


CCX_CORES_ZEN4 = 8
CCX_CORES_ZEN3 = 8
CCX_CORES_ZEN2 = 4
CCX_CORES_ZEN1 = 4

DEFAULT_L1D_KB = 32
DEFAULT_L2_KB = 1024
DEFAULT_L3_KB = 32 * 1024

# A dark-background thermal ramp: deep blue -> cyan -> green -> yellow -> orange -> red -> white.
THERMAL_STOPS: tuple[tuple[float, tuple[int, int, int]], ...] = (
    (0.00, (23, 37, 84)),
    (0.16, (29, 78, 137)),
    (0.33, (0, 156, 184)),
    (0.50, (89, 205, 144)),
    (0.67, (255, 231, 86)),
    (0.82, (245, 121, 58)),
    (0.94, (206, 42, 51)),
    (1.00, (255, 245, 230)),
)

GLYPHS = "·▁▂▃▄▅▆▇█"

@dataclass(frozen=True, slots=True)
class CcxGroup:
    ccx_id: int
    cpus: tuple[int, ...]

    @property
    def representative_cpu(self) -> int:
        return self.cpus[0]


@dataclass(slots=True)
class BucketResult:
    ccx_id: int
    cpu: int
    kb: int
    ns: float


@dataclass(frozen=True, slots=True)
class HeatScale:
    mode: str
    lo: float
    hi: float
    observed_lo: float
    observed_hi: float

    def normalize(self, value: float) -> float:
        if self.hi <= self.lo:
            return 1.0
        return _clamp((value - self.lo) / (self.hi - self.lo))


@dataclass(frozen=True, slots=True)
class RowStats:
    ccx_id: int
    cpu: int
    p50_ns: float
    p95_ns: float
    max_ns: float
    max_kb: int
    cliff_from_kb: int
    cliff_to_kb: int
    cliff_delta_ns: float


@dataclass(frozen=True, slots=True)
class HotspotCell:
    ccx_id: int
    cpu: int
    kb: int
    band: str
    ns: float
    bucket_median_ns: float

    @property
    def delta_ns(self) -> float:
        return self.ns - self.bucket_median_ns

    @property
    def ratio(self) -> float:
        return self.ns / self.bucket_median_ns if self.bucket_median_ns > 0.0 else 1.0


@dataclass(frozen=True, slots=True)
class BucketSpread:
    kb: int
    band: str
    min_ns: float
    median_ns: float
    max_ns: float
    slowest_ccx: int

    @property
    def spread_ns(self) -> float:
        return self.max_ns - self.min_ns

    @property
    def ratio(self) -> float:
        return self.max_ns / self.median_ns if self.median_ns > 0.0 else 1.0


@dataclass(slots=True)
class SweepResult:
    topo_brand: str
    generation: str
    ccx_groups: list[CcxGroup]
    buckets_kb: list[int]
    rows: list[BucketResult] = field(default_factory=list)
    started_ns: int = 0
    ended_ns: int = 0
    l1d_kb: int = DEFAULT_L1D_KB
    l2_kb: int = DEFAULT_L2_KB
    l3_kb: int = DEFAULT_L3_KB
    cpu_family: int = 0
    cpu_model: int = 0
    cpu_stepping: int = 0
    grouping_basis: str = "inferred contiguous logical CPU ranges"

    def matrix(self) -> list[list[float | None]]:
        idx = {kb: i for i, kb in enumerate(self.buckets_kb)}
        grid: list[list[float | None]] = [
            [None] * len(self.buckets_kb) for _ in self.ccx_groups
        ]
        ccx_idx = {g.ccx_id: i for i, g in enumerate(self.ccx_groups)}
        for row in self.rows:
            r = ccx_idx.get(row.ccx_id)
            c = idx.get(row.kb)
            if r is not None and c is not None:
                grid[r][c] = row.ns
        return grid

    @property
    def elapsed_seconds(self) -> float:
        if self.ended_ns <= self.started_ns:
            return 0.0
        return (self.ended_ns - self.started_ns) / 1e9


def _ccx_size_for(generation: str) -> int:
    if generation in {"Zen 3", "Zen 3+", "Zen 4", "Zen 5", "Zen 6"}:
        return CCX_CORES_ZEN4
    if generation == "Zen 2":
        return CCX_CORES_ZEN2
    if generation == "Zen 1 / Zen+":
        return CCX_CORES_ZEN1
    return CCX_CORES_ZEN4


def _smt_threads_per_core() -> int:
    try:
        out = subprocess.check_output(
            ["sysctl", "-n", "kern.smp.threads_per_core"],
            stderr=subprocess.DEVNULL, timeout=2,
        ).decode().strip()
        if out.isdigit() and int(out) > 0:
            return int(out)
    except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        pass
    return 2


def _l3_shared_threads(topo: Topology) -> int:
    # Tests construct topo via SimpleNamespace and Topology.__new__ without
    # always setting .caches; tolerate that rather than tightening here.
    for cache in getattr(topo, "caches", ()):
        if cache.level == 3 and cache.shared_threads > 0:
            return cache.shared_threads
    return 0


def _threads_per_ccx_group(topo: Topology) -> tuple[int, str]:
    shared = _l3_shared_threads(topo)
    if shared > 0:
        return shared, f"CPUID L3 shared_threads={shared}; contiguous logical CPU ranges"
    cores_per_ccx = _ccx_size_for(topo.cpu.generation)
    smt = _smt_threads_per_core()
    threads = cores_per_ccx * smt
    return threads, (
        f"generation fallback {cores_per_ccx} cores/CCX × SMT{smt}; "
        "contiguous logical CPU ranges"
    )


def discover_ccx_groups(topo: Topology, threads_per_ccx: int | None = None) -> list[CcxGroup]:
    if threads_per_ccx is None:
        threads_per_ccx, _ = _threads_per_ccx_group(topo)
    if threads_per_ccx <= 0 or topo.online_cpus <= 0:
        return [CcxGroup(0, (0,))]
    groups: list[CcxGroup] = []
    for ccx_id, base in enumerate(range(0, topo.online_cpus, threads_per_ccx)):
        cpus = tuple(range(base, min(base + threads_per_ccx, topo.online_cpus)))
        if cpus:
            groups.append(CcxGroup(ccx_id, cpus))
    return groups


def geometric_buckets(min_kb: int, max_kb: int, steps: int) -> list[int]:
    if steps < 2 or min_kb < 1 or max_kb < min_kb:
        raise ValueError("invalid bucket parameters")
    ratio = (max_kb / min_kb) ** (1.0 / (steps - 1))
    seen: list[int] = []
    last = 0
    for step in range(steps):
        kb = max_kb if step == steps - 1 else max(1, int(min_kb * (ratio ** step)))
        if kb != last:
            seen.append(kb)
            last = kb
    return seen


def _clamp(value: float) -> float:
    return max(0.0, min(1.0, value))


def _percentile_sorted(ordered: Sequence[float], pct: float) -> float:
    if not ordered:
        return 0.0
    if len(ordered) == 1:
        return ordered[0]
    pos = _clamp(pct / 100.0) * (len(ordered) - 1)
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] * (1.0 - (pos - lo)) + ordered[hi] * (pos - lo)


def _percentile(values: Sequence[float], pct: float) -> float:
    return _percentile_sorted(sorted(values), pct) if values else 0.0


def _median(values: Sequence[float]) -> float:
    return _percentile(values, 50.0)


def _build_scale(values: Sequence[float], mode: str = "robust") -> HeatScale:
    if mode not in {"robust", "linear"}:
        raise ValueError("scale must be 'robust' or 'linear'")
    if not values:
        return HeatScale(mode, 0.0, 1.0, 0.0, 1.0)
    ordered = sorted(values)
    observed_lo, observed_hi = ordered[0], ordered[-1]
    if mode == "linear":
        return HeatScale(mode, observed_lo, observed_hi, observed_lo, observed_hi)
    lo = _percentile_sorted(ordered, 5.0)
    hi = _percentile_sorted(ordered, 95.0)
    if hi <= lo:
        lo, hi = observed_lo, observed_hi
    return HeatScale(mode, lo, hi, observed_lo, observed_hi)


def _rgb_for(value: float) -> tuple[int, int, int]:
    value = _clamp(value)
    prev_p, prev_rgb = THERMAL_STOPS[0]
    for next_p, next_rgb in THERMAL_STOPS[1:]:
        if value <= next_p:
            span = next_p - prev_p
            local = 0.0 if span <= 0 else (value - prev_p) / span
            return tuple(
                int(round(prev_rgb[i] + (next_rgb[i] - prev_rgb[i]) * local))
                for i in range(3)
            )
        prev_p, prev_rgb = next_p, next_rgb
    return THERMAL_STOPS[-1][1]


def _ansi_bg(rgb: tuple[int, int, int]) -> str:
    return f"\x1b[48;2;{rgb[0]};{rgb[1]};{rgb[2]}m"


def _ansi_fg_for_bg(rgb: tuple[int, int, int]) -> str:
    luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
    return "\x1b[38;2;11;18;32m" if luminance > 150 else "\x1b[38;2;250;250;250m"


def _should_color(out, requested: bool) -> bool:
    if not requested:
        return False
    if os.environ.get("NO_COLOR") or os.environ.get("PY_COLORS") == "0":
        return False
    if os.environ.get("TERM") == "dumb":
        return False
    return hasattr(out, "isatty") and out.isatty()


def _glyph_for(value: float) -> str:
    idx = max(0, min(len(GLYPHS) - 1, int(value * (len(GLYPHS) - 1))))
    return GLYPHS[idx]


def _axis_label(kb: int) -> str:
    # Heatmap columns are 5 chars wide; fmt_kb can emit 6 (e.g. "185.9M").
    # Drop the decimal when the decimal form would overflow the column.
    label = fmt_kb(kb)
    if len(label) <= 5:
        return label
    if kb >= 1024 * 1024:
        return f"{round(kb / (1024 * 1024))}G"
    if kb >= 1024:
        return f"{round(kb / 1024)}M"
    return f"{round(kb)}K"


def _boxed(title: str, content: list[str]) -> list[str]:
    inner = max((len(line) for line in content), default=0)
    inner = max(inner, len(title) + 1)
    fill = inner - 1 - len(title)
    return [
        f"╭─ {title} " + "─" * fill + "╮",
        *(f"│ {line:<{inner}} │" for line in content),
        "╰" + "─" * (inner + 2) + "╯",
    ]


def _terminal_cell(ns: float | None, scale: HeatScale, color: bool, width: int = 3) -> str:
    if ns is None:
        return f" {'·':^{width}} "
    t = scale.normalize(ns)
    glyph = _glyph_for(t)
    text = glyph * width
    if not color:
        return f" {text} "
    rgb = _rgb_for(t)
    return f" {_ansi_bg(rgb)}{_ansi_fg_for_bg(rgb)}{text}\x1b[0m "


def _cache_band(kb: int, result: SweepResult) -> str:
    if kb <= result.l1d_kb:
        return "L1D"
    if kb <= result.l2_kb:
        return "L2"
    if kb <= result.l3_kb:
        return "L3"
    return "DRAM"


def _cache_band_line(result: SweepResult) -> str:
    return (
        f"L1D≤{fmt_kb(result.l1d_kb)}  "
        f"L2≤{fmt_kb(result.l2_kb)}  "
        f"L3-slice≤{fmt_kb(result.l3_kb)}  "
        f"DRAM>{fmt_kb(result.l3_kb)}"
    )


def _silicon_label(result: SweepResult) -> str:
    if result.cpu_family:
        return (
            f"Family {result.cpu_family:#x} Model {result.cpu_model:#x} "
            f"Step {result.cpu_stepping} ({result.generation})"
        )
    return result.generation


def _grouping_note(result: SweepResult) -> str:
    return f"Grouping: {result.grouping_basis}; validate against FreeBSD CPU/cache topology on target hardware."


def _sample_values(matrix: list[list[float | None]]) -> list[float]:
    return [v for row in matrix for v in row if v is not None]


def _bucket_view_indices(bucket_count: int, cols: int, summary_width: int) -> list[int]:
    max_cols = max(1, min(bucket_count, (cols - 20 - summary_width) // 5))
    stride = max(1, math.ceil(bucket_count / max_cols))
    return list(range(0, bucket_count, stride))


def row_stats(result: SweepResult, matrix: list[list[float | None]] | None = None) -> list[RowStats]:
    matrix = matrix if matrix is not None else result.matrix()
    stats: list[RowStats] = []
    for row, group in zip(matrix, result.ccx_groups, strict=False):
        pairs = [(kb, ns) for kb, ns in zip(result.buckets_kb, row, strict=False) if ns is not None]
        if not pairs:
            continue
        ordered = sorted(ns for _, ns in pairs)
        max_kb, max_ns = max(pairs, key=lambda item: item[1])
        cliff_from = cliff_to = pairs[0][0]
        cliff_delta = 0.0
        for (prev_kb, prev_ns), (next_kb, next_ns) in zip(pairs, pairs[1:], strict=False):
            delta = next_ns - prev_ns
            if delta > cliff_delta:
                cliff_from, cliff_to = prev_kb, next_kb
                cliff_delta = delta
        stats.append(RowStats(
            ccx_id=group.ccx_id,
            cpu=group.representative_cpu,
            p50_ns=_percentile_sorted(ordered, 50.0),
            p95_ns=_percentile_sorted(ordered, 95.0),
            max_ns=max_ns,
            max_kb=max_kb,
            cliff_from_kb=cliff_from,
            cliff_to_kb=cliff_to,
            cliff_delta_ns=cliff_delta,
        ))
    return stats


def hotspot_cells(
    result: SweepResult, limit: int = 10,
    matrix: list[list[float | None]] | None = None,
) -> list[HotspotCell]:
    matrix = matrix if matrix is not None else result.matrix()
    medians: dict[int, float] = {}
    for col, kb in enumerate(result.buckets_kb):
        col_values = [row[col] for row in matrix if row[col] is not None]
        medians[kb] = _median(col_values) if col_values else 0.0

    cells: list[HotspotCell] = []
    for row, group in zip(matrix, result.ccx_groups, strict=False):
        for kb, ns in zip(result.buckets_kb, row, strict=False):
            if ns is None:
                continue
            cells.append(HotspotCell(
                ccx_id=group.ccx_id,
                cpu=group.representative_cpu,
                kb=kb,
                band=_cache_band(kb, result),
                ns=ns,
                bucket_median_ns=medians.get(kb, 0.0),
            ))
    cells.sort(key=lambda cell: (cell.ratio, cell.delta_ns, cell.ns), reverse=True)
    return cells[:limit]


def bucket_spreads(
    result: SweepResult, limit: int = 8,
    matrix: list[list[float | None]] | None = None,
) -> list[BucketSpread]:
    matrix = matrix if matrix is not None else result.matrix()
    spreads: list[BucketSpread] = []
    for col, kb in enumerate(result.buckets_kb):
        values: list[tuple[int, float]] = []
        for row, group in zip(matrix, result.ccx_groups, strict=False):
            ns = row[col]
            if ns is not None:
                values.append((group.ccx_id, ns))
        if len(values) < 2:
            continue
        slowest_ccx, max_ns = max(values, key=lambda item: item[1])
        min_ns = min(ns for _, ns in values)
        median_ns = _median([ns for _, ns in values])
        spreads.append(BucketSpread(
            kb=kb,
            band=_cache_band(kb, result),
            min_ns=min_ns,
            median_ns=median_ns,
            max_ns=max_ns,
            slowest_ccx=slowest_ccx,
        ))
    spreads.sort(key=lambda spread: (spread.spread_ns, spread.ratio), reverse=True)
    return spreads[:limit]


_LIVE_REDRAW_MIN_INTERVAL = 0.1  # seconds; cap live redraw to ~10 Hz


class _LiveCanvas:
    """Threadsafe in-place renderer of the per-CCX latency grid."""

    def __init__(
        self,
        groups: list[CcxGroup],
        buckets: list[int],
        *,
        l1d_kb: int,
        l2_kb: int,
        l3_kb: int,
        out=sys.stdout,
        color: bool = True,
        scale_mode: str = "robust",
    ):
        self.groups = groups
        self.buckets = buckets
        self.out = out
        self.interactive = _should_color(out, color)
        self.scale_mode = scale_mode
        self.lock = threading.Lock()
        self.render_lock = threading.Lock()
        self.grid: list[list[float | None]] = [
            [None] * len(buckets) for _ in groups
        ]
        self.row_index = {g.ccx_id: i for i, g in enumerate(groups)}
        self.bucket_index = {kb: i for i, kb in enumerate(buckets)}
        self.completed = 0
        self.failed = 0
        self.total = len(groups) * len(buckets)
        self.start_ns = time.monotonic_ns()
        self.printed_lines = 0
        self.l1d_kb = l1d_kb
        self.l2_kb = l2_kb
        self.l3_kb = l3_kb
        self.last_sample: tuple[int, int, float] | None = None
        self._cached_cols = shutil.get_terminal_size((120, 40)).columns
        self._next_draw_at = 0.0
        self._rendering = False
        self._resize_signal = getattr(signal, "SIGWINCH", None)
        self._previous_resize_handler = None
        if self.interactive and self._resize_signal is not None:
            try:
                self._previous_resize_handler = signal.getsignal(self._resize_signal)
                signal.signal(self._resize_signal, self._on_resize)
            except (ValueError, OSError):
                self._resize_signal = None

    def _restore_resize_handler(self) -> None:
        if self._resize_signal is None:
            return
        try:
            if signal.getsignal(self._resize_signal) == self._on_resize:
                signal.signal(self._resize_signal, self._previous_resize_handler)
        except (ValueError, OSError):
            pass

    def _on_resize(self, *_args) -> None:
        try:
            self._cached_cols = shutil.get_terminal_size((120, 40)).columns
        except OSError:
            pass

    def update(self, ccx_id: int, kb: int, ns: float | None) -> None:
        draw_now = False
        with self.lock:
            row = self.row_index.get(ccx_id)
            col = self.bucket_index.get(kb)
            if row is None or col is None:
                return
            self.completed += 1
            if ns is None:
                self.failed += 1
            else:
                self.grid[row][col] = ns
                self.last_sample = (ccx_id, kb, ns)
            now = time.monotonic()
            if self.interactive and not self._rendering and now >= self._next_draw_at:
                self._rendering = True
                draw_now = True
        if draw_now:
            try:
                self._redraw()
            finally:
                with self.lock:
                    self._rendering = False
                    self._next_draw_at = time.monotonic() + _LIVE_REDRAW_MIN_INTERVAL

    def finalize(self) -> None:
        if self.interactive:
            self._redraw(final=True)
            self.out.write("\n")
        self.out.flush()
        self._restore_resize_handler()

    def _redraw(self, final: bool = False) -> None:
        with self.render_lock:
            with self.lock:
                if not self.interactive and not final:
                    return
                grid = [row[:] for row in self.grid]
                completed = self.completed
                failed = self.failed
                last_sample = self.last_sample
                printed_lines = self.printed_lines
                cols = self._cached_cols

            values = _sample_values(grid)
            scale = _build_scale(values, self.scale_mode)
            summary_width = 32
            bucket_view = _bucket_view_indices(len(self.buckets), cols, summary_width)

            lines: list[str] = []
            elapsed = (time.monotonic_ns() - self.start_ns) / 1e9
            pct = (completed / self.total * 100.0) if self.total else 100.0
            bar_w = max(10, min(42, cols - 70))
            filled = int(bar_w * completed / self.total) if self.total else bar_w
            bar = "█" * filled + "░" * (bar_w - filled)
            lines.append(
                f"FreeBSD AMD cache sweep  {bar}  {completed}/{self.total} "
                f"({pct:5.1f}%)  elapsed {elapsed:6.2f}s"
            )
            lines.append(
                f"scale={scale.mode} {scale.lo:5.1f}..{scale.hi:5.1f} ns "
                f"(observed {scale.observed_lo:5.1f}..{scale.observed_hi:5.1f})  "
                f"cache bands: L1D≤{fmt_kb(self.l1d_kb)} L2≤{fmt_kb(self.l2_kb)} "
                f"L3≤{fmt_kb(self.l3_kb)}"
            )
            if last_sample is None:
                lines.append("latest sample: warming native pointer-chase workers...")
            else:
                ccx_id, kb, ns = last_sample
                lines.append(
                    f"latest sample: CCX {ccx_id} {fmt_kb(kb)} {ns:6.2f} ns/load  "
                    "live panels rank same-bucket CCX outliers"
                )
            header = "CCX CPU "
            for ci in bucket_view:
                header += f"{_axis_label(self.buckets[ci]):>5}"
            header += " |    p50    max hotspot"
            lines.append(header)

            for r, group in enumerate(self.groups):
                row = grid[r]
                present: list[float] = []
                max_ns = -1.0
                max_kb = self.buckets[0] if self.buckets else 0
                for i, value in enumerate(row):
                    if value is None:
                        continue
                    present.append(value)
                    if value > max_ns:
                        max_ns = value
                        max_kb = self.buckets[i]
                if present:
                    trailer = f" | {_median(present):6.1f} {max_ns:6.1f} {fmt_kb(max_kb):>7}"
                else:
                    trailer = " |      -      -       -"
                cells = "".join(_terminal_cell(row[ci], scale, self.interactive) for ci in bucket_view)
                lines.append(f"{group.ccx_id:>3} {group.representative_cpu:>3} {cells}{trailer}")
            lines.extend(self._live_summary_lines(grid))
            if failed:
                lines.append(f"warning: {failed} bucket sample(s) failed; failed cells are shown as ···")

            if printed_lines:
                self.out.write(f"\x1b[{printed_lines}A")
            for line in lines:
                self.out.write("\x1b[2K" if self.interactive else "")
                self.out.write(line + "\n")
            with self.lock:
                self.printed_lines = len(lines)
            self.out.flush()

    def _live_summary_lines(self, grid: list[list[float | None]]) -> list[str]:
        hotspots: list[tuple[float, float, float, int, int]] = []
        spreads: list[tuple[float, float, int, int]] = []
        for col, kb in enumerate(self.buckets):
            values: list[tuple[int, float]] = []
            for row, group in zip(grid, self.groups, strict=False):
                ns = row[col]
                if ns is not None:
                    values.append((group.ccx_id, ns))
            if len(values) < 2:
                continue
            median = _median([ns for _, ns in values])
            if median <= 0.0:
                continue
            slowest_ccx, max_ns = max(values, key=lambda item: item[1])
            min_ns = min(ns for _, ns in values)
            spreads.append((max_ns - min_ns, max_ns / median, kb, slowest_ccx))
            for ccx_id, ns in values:
                delta = ns - median
                if delta > 0.0:
                    hotspots.append((ns / median, delta, ns, ccx_id, kb))

        lines: list[str] = []
        if hotspots:
            hotspots.sort(reverse=True)
            parts = [
                f"CCX {ccx_id}@{fmt_kb(kb)} {ns:.1f}ns {ratio:.2f}×"
                for ratio, _delta, ns, ccx_id, kb in hotspots[:3]
            ]
            lines.append("Live same-bucket hotspots: " + "  |  ".join(parts))
        else:
            lines.append("Live same-bucket hotspots: collecting paired CCX samples...")

        if spreads:
            spreads.sort(reverse=True)
            parts = [
                f"{fmt_kb(kb)} Δ{spread:.1f}ns slow=CCX {slowest_ccx} ({ratio:.2f}×med)"
                for spread, ratio, kb, slowest_ccx in spreads[:3]
            ]
            lines.append("Live cross-CCX spread: " + "  |  ".join(parts))
        else:
            lines.append("Live cross-CCX spread: collecting paired CCX samples...")
        return lines


def _worker(group: CcxGroup, buckets: list[int], repeat: int, superpage: bool,
            canvas: _LiveCanvas, sink: list[BucketResult], errors: list[str],
            sink_lock: threading.Lock, stop_evt: threading.Event) -> None:
    cpu = group.representative_cpu
    for kb in buckets:
        if stop_evt.is_set():
            return
        try:
            sample = _fb.measure_chase_bucket(
                kb=kb, repeat=repeat, cpu=cpu, superpage=1 if superpage else 0,
            )
        except Exception as exc:
            with sink_lock:
                errors.append(f"CCX {group.ccx_id} CPU {cpu} bucket {kb}K: {exc}")
            stop_evt.set()
            canvas.update(group.ccx_id, kb, None)
            return
        if sample is None:
            canvas.update(group.ccx_id, kb, None)
            continue
        kb_real, ns = sample
        canvas.update(group.ccx_id, kb_real, ns)
        with sink_lock:
            sink.append(BucketResult(group.ccx_id, cpu, int(kb_real), float(ns)))


def run_parallel_sweep(
    topo: Topology,
    *,
    min_kb: int = 1,
    max_kb: int = 262144,
    steps: int = 40,
    repeat: int = 3,
    superpage: bool = True,
    color: bool = True,
    scale_mode: str = "robust",
    out=sys.stdout,
) -> SweepResult:
    if min_kb < 1 or max_kb < min_kb:
        raise ValueError("--min-kb must be >= 1 and <= --max-kb")
    if steps < 2:
        raise ValueError("--steps must be >= 2")
    if repeat < 1 or repeat > 256:
        raise ValueError("--repeat must be between 1 and 256")
    if scale_mode not in {"robust", "linear"}:
        raise ValueError("scale must be 'robust' or 'linear'")
    threads_per_ccx, grouping_basis = _threads_per_ccx_group(topo)
    groups = discover_ccx_groups(topo, threads_per_ccx=threads_per_ccx)
    buckets = geometric_buckets(min_kb, max_kb, steps)
    l1d_kb = topo.cache_size_kb(1, "Data") or DEFAULT_L1D_KB
    l2_kb = topo.cache_size_kb(2) or DEFAULT_L2_KB
    l3_kb = topo.cache_size_kb(3) or DEFAULT_L3_KB
    canvas = _LiveCanvas(
        groups, buckets,
        l1d_kb=l1d_kb, l2_kb=l2_kb, l3_kb=l3_kb,
        out=out, color=color, scale_mode=scale_mode,
    )
    sink: list[BucketResult] = []
    errors: list[str] = []
    sink_lock = threading.Lock()
    stop_evt = threading.Event()
    threads: list[threading.Thread] = []

    started = time.monotonic_ns()
    try:
        for group in groups:
            thread = threading.Thread(
                target=_worker,
                args=(group, buckets, repeat, superpage, canvas, sink, errors, sink_lock, stop_evt),
                name=f"ccx-{group.ccx_id}",
                daemon=True,
            )
            thread.start()
            threads.append(thread)
        for thread in threads:
            thread.join()
    except KeyboardInterrupt:
        stop_evt.set()
        for thread in threads:
            thread.join(timeout=2.0)
        raise
    finally:
        canvas.finalize()

    if errors:
        head = errors[0]
        extra = f" (+{len(errors) - 1} more)" if len(errors) > 1 else ""
        raise RuntimeError(f"cache sweep worker failed: {head}{extra}")
    if not sink:
        raise RuntimeError("cache sweep collected no samples")

    ended = time.monotonic_ns()
    return SweepResult(
        topo_brand=topo.cpu.brand or "AMD",
        generation=topo.cpu.generation,
        ccx_groups=groups,
        buckets_kb=buckets,
        rows=sink,
        started_ns=started,
        ended_ns=ended,
        l1d_kb=l1d_kb,
        l2_kb=l2_kb,
        l3_kb=l3_kb,
        cpu_family=topo.cpu.family,
        cpu_model=topo.cpu.model,
        cpu_stepping=topo.cpu.stepping,
        grouping_basis=grouping_basis,
    )


def _legend(color: bool) -> str:
    labels = ["fast", "L1/L2", "L3", "DRAM", "hot"]
    pieces: list[str] = []
    for i, label in enumerate(labels):
        t = i / (len(labels) - 1)
        if color:
            rgb = _rgb_for(t)
            pieces.append(f"{_ansi_bg(rgb)}  \x1b[0m {label}")
        else:
            pieces.append(f"{_glyph_for(t)} {label}")
    return "  ".join(pieces)


def render_final_heatmap(
    result: SweepResult,
    out=sys.stdout,
    color: bool = True,
    scale_mode: str = "robust",
    top: int = 8,
) -> None:
    color = _should_color(out, color)
    matrix = result.matrix()
    flat = _sample_values(matrix)
    if not flat:
        out.write("no samples collected\n")
        return
    scale = _build_scale(flat, scale_mode)
    cols = shutil.get_terminal_size((120, 40)).columns
    bucket_count = len(result.buckets_kb)
    view = _bucket_view_indices(bucket_count, cols, summary_width=42)
    stats_by_ccx = {stats.ccx_id: stats for stats in row_stats(result, matrix=matrix)}
    hot = hotspot_cells(result, top, matrix=matrix)
    spreads = bucket_spreads(result, min(top, 8), matrix=matrix)

    out.write("\n")
    box = _boxed("FreeBSD AMD cache-hotspot map", [
        f"{result.topo_brand} | {_silicon_label(result)} | "
        f"{len(result.ccx_groups)} inferred CCX groups × {bucket_count} buckets | "
        f"{result.elapsed_seconds:.2f}s wall",
        f"scale={scale.mode} {scale.lo:.2f}..{scale.hi:.2f} ns/load "
        f"(observed {scale.observed_lo:.2f}..{scale.observed_hi:.2f})",
        f"cache bands: {_cache_band_line(result)}",
        _grouping_note(result),
    ])
    out.write("\n".join(box) + "\n")
    out.write(f"Legend: {_legend(color)}\n\n")

    header = "CCX CPU "
    for ci in view:
        header += f"{_axis_label(result.buckets_kb[ci]):>5}"
    header += " |    p50    p95    max hotspot      cliff"
    out.write(header + "\n")
    for row, group in zip(matrix, result.ccx_groups, strict=False):
        line = f"{group.ccx_id:>3} {group.representative_cpu:>3} "
        for ci in view:
            line += _terminal_cell(row[ci], scale, color)
        stats = stats_by_ccx.get(group.ccx_id)
        if stats is None:
            line += " |      -      -      -       -          -"
        else:
            cliff = (
                "flat" if stats.cliff_delta_ns <= 0.0
                else f"{fmt_kb(stats.cliff_from_kb)}→{fmt_kb(stats.cliff_to_kb)}"
            )
            line += (
                f" | {stats.p50_ns:6.1f} {stats.p95_ns:6.1f} {stats.max_ns:6.1f} "
                f"{fmt_kb(stats.max_kb):>7} {cliff:>10}"
            )
        out.write(line + "\n")

    out.write("\nTop same-bucket CCX hotspots (relative to median for that bucket):\n")
    out.write(" rank  CCX  CPU bucket band    ns/load  bucket-med   delta   ratio\n")
    for rank, cell in enumerate(hot, 1):
        out.write(
            f" {rank:>4} {cell.ccx_id:>4} {cell.cpu:>4} {fmt_kb(cell.kb):>6} "
            f"{cell.band:<5} {cell.ns:8.2f} {cell.bucket_median_ns:10.2f} "
            f"{cell.delta_ns:7.2f} {cell.ratio:6.2f}×\n"
        )

    out.write("\nBuckets with largest cross-CCX spread:\n")
    out.write(" rank bucket band      min      med      max   spread slowest\n")
    for rank, spread in enumerate(spreads, 1):
        out.write(
            f" {rank:>4} {fmt_kb(spread.kb):>6} {spread.band:<5} "
            f"{spread.min_ns:8.2f} {spread.median_ns:8.2f} {spread.max_ns:8.2f} "
            f"{spread.spread_ns:8.2f} CCX {spread.slowest_ccx}\n"
        )

    out.write("\nInferred CCX -> CPU mapping (representative thread of each group):\n")
    out.write(f"  {_grouping_note(result)}\n")
    for group in result.ccx_groups:
        cpu_range = f"{group.cpus[0]}..{group.cpus[-1]}" if len(group.cpus) > 1 else f"{group.cpus[0]}"
        out.write(f"  CCX {group.ccx_id:>2}: cpus {cpu_range}  (representative={group.representative_cpu})\n")

    elapsed = result.elapsed_seconds
    out.write(
        f"\nWall clock: {elapsed:.2f}s parallel across {len(result.ccx_groups)} CCXs "
        f"(serial estimate ~{elapsed * len(result.ccx_groups):.0f}s)\n"
    )
    out.write(
        "Interpretation: absolute DRAM-band heat is expected beyond the per-CCX L3 slice; "
        "same-bucket CCX outliers are the first candidates for cache/locality hotspots.\n"
    )


def sweep_to_csv(result: SweepResult) -> str:
    lines = ["ccx_id,cpu,kb,ns_per_load"]
    for row in sorted(result.rows, key=lambda item: (item.ccx_id, item.kb)):
        lines.append(f"{row.ccx_id},{row.cpu},{row.kb},{row.ns:.4f}")
    return "\n".join(lines) + "\n"
