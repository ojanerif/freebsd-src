#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

from __future__ import annotations

"""FreeBSD hwpmc collection orchestration."""

import os
import shutil
import signal
import stat
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

from freebsd_cache_hotspot import _fbcacheprobe as _fb
from freebsd_cache_hotspot.analyzer import CountResult
from freebsd_cache_hotspot.events import FreeBsdAmdEvent, default_events, events_for_cpu
from freebsd_cache_hotspot.topology import Topology


@dataclass(slots=True)
class ProfileResult:
    command: list[str]
    mode: str
    cpu: int
    pid: int = 0
    duration_ns: int = 0
    counts: list[CountResult] = field(default_factory=list)
    skipped_events: list[str] = field(default_factory=list)


@dataclass(slots=True)
class IbsTextResult:
    command: list[str]
    event_spec: str
    period: int
    log_path: str
    decoded_output: str
    op_samples: int = 0
    load_samples: int = 0
    miss_samples: int = 0
    latency_lines: int = 0


class FreeBsdProfiler:
    def __init__(self, topo: Topology | None = None):
        self.topo = topo or Topology()
        if not _fb.pmc_available():
            raise RuntimeError("FreeBSD libpmc backend is not available; build and run this project on FreeBSD")
        _fb.pmc_init()

    @staticmethod
    def _check_exec_target_safety(cmd: Sequence[str]) -> None:
        if not cmd:
            raise ValueError("empty command")
        target = cmd[0] if "/" in cmd[0] else shutil.which(cmd[0])
        if not target:
            return
        try:
            st = os.stat(target)
        except OSError:
            return
        if st.st_mode & (stat.S_ISUID | stat.S_ISGID):
            raise PermissionError(f"refusing to profile privilege-transition executable: {target}")

    @staticmethod
    def _spawn_stopped_exec(cmd: Sequence[str]) -> int:
        pid = os.fork()
        if pid == 0:
            try:
                os.kill(os.getpid(), signal.SIGSTOP)
                os.execvp(cmd[0], list(cmd))
            except BaseException:
                os._exit(127)
        waited, status = os.waitpid(pid, os.WUNTRACED)
        if waited != pid or not os.WIFSTOPPED(status):
            raise RuntimeError(f"child {pid} did not stop before exec")
        return pid

    @staticmethod
    def _wait_child(pid: int) -> int:
        while True:
            waited, status = os.waitpid(pid, 0)
            if waited != pid:
                continue
            if os.WIFEXITED(status):
                return os.WEXITSTATUS(status)
            if os.WIFSIGNALED(status):
                return -os.WTERMSIG(status)

    @staticmethod
    def _raise_for_returncode(cmd: Sequence[str], returncode: int) -> None:
        if returncode == 0:
            return
        rendered = " ".join(cmd)
        if returncode < 0:
            raise RuntimeError(f"profiled command {rendered} terminated by signal {-returncode}")
        raise RuntimeError(f"profiled command {rendered} exited with status {returncode}")

    def _resolve_events(self, event_names: Sequence[str]) -> tuple[list[FreeBsdAmdEvent], list[str]]:
        event_map = events_for_cpu(self.topo.cpu.family, self.topo.cpu.model)
        by_spec = {event.freebsd_spec: event for event in event_map.values()}
        resolved: list[FreeBsdAmdEvent] = []
        skipped: list[str] = []
        for name in event_names:
            event = event_map.get(name) or by_spec.get(name)
            if event is None:
                skipped.append(name)
                continue
            resolved.append(event)
        return resolved, skipped

    @staticmethod
    def _release_all(pmcids: Sequence[int]) -> None:
        for pmcid in pmcids:
            try:
                _fb.pmc_release(pmcid)
            except OSError:
                pass

    def count_command(
        self,
        cmd: Sequence[str],
        event_names: Sequence[str] | None = None,
        *,
        mode: str = "TC",
        cpu: int = -1,
        topdown: bool = False,
    ) -> ProfileResult:
        self._check_exec_target_safety(cmd)
        if event_names is None:
            event_names = default_events(self.topo.cpu.family, self.topo.cpu.model, topdown=topdown)
        events, skipped = self._resolve_events(event_names)
        result = ProfileResult(command=list(cmd), mode=mode.upper(), cpu=cpu, skipped_events=skipped)
        if not events:
            return result
        if len(events) > (self.topo.pmc.core_counters or 6):
            result.skipped_events.append(
                f"requested {len(events)} events with {self.topo.pmc.core_counters or 6} core PMCs; FreeBSD run is sequential-start, not Linux perf multiplexed"
            )

        pmcids: list[int] = []
        pid = 0
        try:
            if mode.upper() == "TC":
                pid = self._spawn_stopped_exec(cmd)
                result.pid = pid
                for event in events:
                    pmcid = int(_fb.pmc_allocate(event.freebsd_spec, mode="TC", cpu=-1, count=0))
                    pmcids.append(pmcid)
                    _fb.pmc_attach(pmcid, pid)
                for pmcid in pmcids:
                    _fb.pmc_start(pmcid)
                start = time.monotonic_ns()
                os.kill(pid, signal.SIGCONT)
                rc = self._wait_child(pid)
                stop = time.monotonic_ns()
                pid = 0
                for pmcid in pmcids:
                    try:
                        _fb.pmc_stop(pmcid)
                    except OSError:
                        pass
                self._raise_for_returncode(cmd, rc)
            elif mode.upper() == "SC":
                for event in events:
                    pmcid = int(_fb.pmc_allocate(event.freebsd_spec, mode="SC", cpu=cpu, count=0))
                    pmcids.append(pmcid)
                for pmcid in pmcids:
                    _fb.pmc_start(pmcid)
                start = time.monotonic_ns()
                completed = subprocess.run(list(cmd), check=False)
                stop = time.monotonic_ns()
                for pmcid in pmcids:
                    try:
                        _fb.pmc_stop(pmcid)
                    except OSError:
                        pass
                self._raise_for_returncode(cmd, completed.returncode)
            else:
                raise ValueError("mode must be TC or SC for counting")

            result.duration_ns = stop - start
            for event, pmcid in zip(events, pmcids, strict=False):
                value = int(_fb.pmc_read(pmcid))
                result.counts.append(CountResult(event.key, event.freebsd_spec, value, event.description))
            return result
        finally:
            if pid:
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
                try:
                    os.waitpid(pid, 0)
                except OSError:
                    pass
            self._release_all(pmcids)


def _ibs_event_spec(*, l3miss: bool, ldlat: int, opcount: bool) -> str:
    parts = ["ibs-op"]
    if l3miss or ldlat:
        parts.append("l3miss")
    if ldlat:
        parts.append(f"ldlat={ldlat}")
    if opcount:
        parts.append("opcount")
    return ",".join(parts)


def _summarize_ibs_text(text: str) -> tuple[int, int, int, int]:
    op_samples = load_samples = miss_samples = latency_lines = 0
    for line in text.splitlines():
        if not line.startswith("ibs-op"):
            continue
        op_samples += 1
        fields = set(line.replace(",", " ").split())
        if "load" in fields:
            load_samples += 1
        if "miss" in fields:
            miss_samples += 1
        if "Latency" in fields:
            latency_lines += 1
    return op_samples, load_samples, miss_samples, latency_lines


def run_pmcstat_ibs(
    cmd: Sequence[str],
    *,
    period: int = 65536,
    l3miss: bool = False,
    ldlat: int = 0,
    opcount: bool = False,
    top: int = 30,
    outdir: str | None = None,
) -> IbsTextResult:
    if not cmd:
        raise ValueError("empty command")
    if period < 65536:
        raise ValueError("FreeBSD IBS period must be >= 65536")
    if ldlat and (ldlat < 128 or ldlat > 2048 or ldlat % 128 != 0):
        raise ValueError("IBS ldlat must be 128..2048 cycles in 128-cycle steps")
    event_spec = _ibs_event_spec(l3miss=l3miss, ldlat=ldlat, opcount=opcount)
    if outdir is None:
        base = Path(tempfile.mkdtemp(
            prefix=time.strftime("freebsd-cache-hotspot-%Y%m%dT%H%M%SZ-", time.gmtime())
        ))
    else:
        base = Path(outdir)
        base.mkdir(parents=True, exist_ok=True)
    log_path = base / "ibs-op.pmc"
    run_cmd = ["pmcstat", "-S", event_spec, "-n", str(period), "-O", str(log_path), "--", *cmd]
    raw = subprocess.run(run_cmd, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if raw.returncode != 0:
        raise RuntimeError(raw.stdout.strip() or f"pmcstat exited with {raw.returncode}")
    decode_cmd = ["pmcstat", "-R", str(log_path), "-z", str(top), "-G"]
    decoded = subprocess.run(decode_cmd, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if decoded.returncode != 0:
        raise RuntimeError(decoded.stdout.strip() or f"pmcstat -R exited with {decoded.returncode}")
    op_samples, load_samples, miss_samples, latency_lines = _summarize_ibs_text(decoded.stdout)
    return IbsTextResult(
        command=list(cmd),
        event_spec=event_spec,
        period=period,
        log_path=str(log_path),
        decoded_output=decoded.stdout,
        op_samples=op_samples,
        load_samples=load_samples,
        miss_samples=miss_samples,
        latency_lines=latency_lines,
    )
