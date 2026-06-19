#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

from __future__ import annotations

import io


class TtyStringIO(io.StringIO):
    def isatty(self):
        return True


def test_extension_core_symbols():
    from freebsd_cache_hotspot import _fbcacheprobe as fb

    assert hasattr(fb, "cpuid")
    assert hasattr(fb, "rdtsc")
    assert hasattr(fb, "cache_latency_probe")
    assert hasattr(fb, "measure_chase_bucket")
    assert hasattr(fb, "cache_bw_probe")
    assert hasattr(fb, "pmc_available")
    assert fb.CACHE_LINE_SIZE == 64


def test_generation_map_family_1a_separates_zen5_zen6():
    from freebsd_cache_hotspot.topology import zen_generation

    assert zen_generation(0x19, 0x11) == "Zen 4"
    assert zen_generation(0x19, 0x21) == "Zen 3"
    assert zen_generation(0x19, 0x44) == "Zen 3+"
    assert zen_generation(0x1A, 0x44) == "Zen 5"
    assert zen_generation(0x1A, 0x54) == "Zen 6"


def test_pipeline_width_matches_generation():
    from freebsd_cache_hotspot.topology import CpuIdentity

    zen4 = CpuIdentity(0x19, 0x11, 1, "AuthenticAMD", "", 0, 0)
    zen5 = CpuIdentity(0x1A, 0x44, 1, "AuthenticAMD", "", 0, 0)
    assert zen4.pipeline_width == 6
    assert zen5.pipeline_width == 8
    far_future = CpuIdentity(0x1B, 0x00, 1, "AuthenticAMD", "", 0, 0)
    assert far_future.pipeline_width == 0


def test_zen4_events_have_explicit_unit_masks():
    from freebsd_cache_hotspot.events import events_for_cpu

    events = events_for_cpu(0x19, 0x11)
    assert events["cycles"].event_select == 0x076
    assert events["cycles"].unit_mask == 0x00
    assert events["dc_fill_far_dram"].event_select == 0x043
    assert events["dc_fill_far_dram"].unit_mask == 0x40
    assert events["backend_slots"].event_select == 0x1A0
    assert events["backend_slots"].unit_mask == 0x1E


def test_cli_parser_strips_command_separator():
    from freebsd_cache_hotspot.__main__ import _normalize_remainder, build_parser

    parser = build_parser()
    args = parser.parse_args(["count", "--events", "cycles,instructions", "--", "/bin/true"])
    assert args.events == "cycles,instructions"
    assert _normalize_remainder(args.command) == ["/bin/true"]


def test_perfevtsel_raw_encodes_extended_eventselect():
    from freebsd_cache_hotspot.events import events_for_cpu

    backend = events_for_cpu(0x19, 0x11)["backend_slots"]
    # EventSel=0x1A0, UnitMask=0x1E -> low byte 0xA0, mask in [15:8] = 0x1E,
    # extended eventsel [11:8]=0x1 in [35:32].
    expected = 0xA0 | (0x1E << 8) | (0x1 << 32)
    assert backend.perfevtsel_raw == expected


def test_ibs_event_spec_implies_l3miss_with_ldlat():
    from freebsd_cache_hotspot.collector import _ibs_event_spec

    assert _ibs_event_spec(l3miss=False, ldlat=256, opcount=False) == "ibs-op,l3miss,ldlat=256"
    assert _ibs_event_spec(l3miss=True, ldlat=0, opcount=True) == "ibs-op,l3miss,opcount"


def test_geometric_buckets_strictly_increasing():
    from freebsd_cache_hotspot.sweep import geometric_buckets

    buckets = geometric_buckets(1, 262144, 40)
    assert buckets[0] == 1
    assert buckets[-1] == 262144
    assert all(b < n for b, n in zip(buckets, buckets[1:]))


def test_ccx_groups_partition_192_thread_zen4():
    from freebsd_cache_hotspot.sweep import discover_ccx_groups
    from freebsd_cache_hotspot.topology import CpuIdentity, Topology

    topo = Topology.__new__(Topology)
    topo.cpu = CpuIdentity(0x19, 0x11, 1, "AuthenticAMD", "EPYC 9654", 48, 48)
    topo.online_cpus = 192
    topo.caches = []
    groups = discover_ccx_groups(topo)
    # 8c*SMT2 = 16 threads/CCX → 192 / 16 = 12 CCXs
    assert len(groups) == 12
    assert sum(len(g.cpus) for g in groups) == 192
    assert groups[0].representative_cpu == 0
    assert groups[1].representative_cpu == 16


def test_ccx_groups_use_cpuid_l3_shared_threads_when_available():
    from freebsd_cache_hotspot.sweep import _threads_per_ccx_group, discover_ccx_groups
    from freebsd_cache_hotspot.topology import CacheLevel, CpuIdentity, Topology

    topo = Topology.__new__(Topology)
    topo.cpu = CpuIdentity(0x19, 0x11, 1, "AuthenticAMD", "EPYC 9654", 48, 48)
    topo.online_cpus = 64
    topo.caches = [CacheLevel(3, "Unified", 32768, 64, 16, 32768, 8)]
    threads, basis = _threads_per_ccx_group(topo)
    groups = discover_ccx_groups(topo)
    assert threads == 8
    assert "CPUID L3 shared_threads=8" in basis
    assert len(groups) == 8
    assert groups[1].representative_cpu == 8


def test_count_rejects_events_with_topdown():
    import pytest
    from freebsd_cache_hotspot.__main__ import build_parser, cmd_count

    parser = build_parser()
    args = parser.parse_args(["count", "--events", "cycles", "--topdown", "--", "/bin/true"])
    with pytest.raises(SystemExit):
        cmd_count(args)


def _synthetic_sweep_result():
    from freebsd_cache_hotspot.sweep import BucketResult, CcxGroup, SweepResult

    return SweepResult(
        topo_brand="AMD <EPYC & test>",
        generation="Zen 4",
        ccx_groups=[CcxGroup(0, tuple(range(0, 16))), CcxGroup(1, tuple(range(16, 32)))],
        buckets_kb=[1, 32, 1024, 32768, 262144],
        rows=[
            BucketResult(1, 16, 262144, 130.0),
            BucketResult(0, 0, 1, 1.2),
            BucketResult(0, 0, 32, 1.8),
            BucketResult(0, 0, 1024, 5.0),
            BucketResult(0, 0, 32768, 38.0),
            BucketResult(0, 0, 262144, 120.0),
            BucketResult(1, 16, 1, 1.1),
            BucketResult(1, 16, 32, 1.9),
            BucketResult(1, 16, 1024, 5.5),
            BucketResult(1, 16, 32768, 60.0),
        ],
        started_ns=1_000_000_000,
        ended_ns=8_500_000_000,
        l1d_kb=32,
        l2_kb=1024,
        l3_kb=32768,
        cpu_family=0x19,
        cpu_model=0x11,
        cpu_stepping=1,
        grouping_basis="CPUID L3 shared_threads=16; contiguous logical CPU ranges",
    )


def test_sweep_parser_accepts_terminal_options_and_csv_export():
    from freebsd_cache_hotspot.__main__ import build_parser

    parser = build_parser()
    args = parser.parse_args([
        "sweep", "--scale", "linear", "--top-hotspots", "12",
        "--csv", "/tmp/fch.csv",
    ])
    assert args.scale == "linear"
    assert args.top_hotspots == 12
    assert args.csv == "/tmp/fch.csv"


def test_sweep_parser_rejects_unknown_output_option():
    import pytest
    from freebsd_cache_hotspot.__main__ import build_parser

    parser = build_parser()
    with pytest.raises(SystemExit):
        parser.parse_args(["sweep", "--definitely-unknown-output", "/tmp/fch.out"])


def test_sweep_rejects_non_positive_top_hotspots():
    import pytest
    from freebsd_cache_hotspot.__main__ import build_parser, cmd_sweep

    parser = build_parser()
    args = parser.parse_args(["sweep", "--top-hotspots", "0"])
    with pytest.raises(SystemExit):
        cmd_sweep(args)


def test_sweep_rejects_invalid_measurement_arguments():
    import pytest
    from freebsd_cache_hotspot.__main__ import build_parser, cmd_sweep

    parser = build_parser()
    for argv in (["sweep", "--repeat", "0"], ["sweep", "--steps", "1"], ["sweep", "--min-kb", "8", "--max-kb", "4"]):
        args = parser.parse_args(argv)
        with pytest.raises(SystemExit):
            cmd_sweep(args)


def test_sweep_to_csv_preserves_raw_columns_and_sorting():
    from freebsd_cache_hotspot.sweep import sweep_to_csv

    csv = sweep_to_csv(_synthetic_sweep_result())
    lines = csv.splitlines()
    assert lines[0] == "ccx_id,cpu,kb,ns_per_load"
    assert lines[1] == "0,0,1,1.2000"
    assert lines[-1] == "1,16,262144,130.0000"


def test_final_heatmap_no_color_has_no_ansi_and_has_hotspot_tables():
    from freebsd_cache_hotspot.sweep import render_final_heatmap

    out = io.StringIO()
    render_final_heatmap(_synthetic_sweep_result(), out=out, color=False, top=4)
    text = out.getvalue()
    assert "\x1b[" not in text
    assert "FreeBSD AMD cache-hotspot map" in text
    assert "Top same-bucket CCX hotspots" in text
    assert "Family 0x19 Model 0x11 Step 1" in text
    assert "Inferred CCX -> CPU mapping" in text
    assert "CPUID L3 shared_threads=16" in text
    assert "L1D≤32K" in text


def test_final_heatmap_honors_no_color_environment(monkeypatch):
    from freebsd_cache_hotspot.sweep import render_final_heatmap

    monkeypatch.setenv("NO_COLOR", "1")
    out = TtyStringIO()
    render_final_heatmap(_synthetic_sweep_result(), out=out, color=True, top=4)
    assert "\x1b[" not in out.getvalue()


def test_live_canvas_no_color_disables_ansi_on_tty():
    import signal
    from freebsd_cache_hotspot.sweep import CcxGroup, _LiveCanvas

    out = TtyStringIO()
    original = signal.getsignal(signal.SIGWINCH) if hasattr(signal, "SIGWINCH") else None
    canvas = _LiveCanvas(
        [CcxGroup(0, (0,))], [1],
        l1d_kb=32, l2_kb=1024, l3_kb=32768,
        out=out, color=False,
    )
    canvas.update(0, 1, 1.0)
    canvas.finalize()
    assert "\x1b[" not in out.getvalue()
    if hasattr(signal, "SIGWINCH"):
        assert signal.getsignal(signal.SIGWINCH) == original


def test_live_canvas_tty_shows_in_run_hotspot_panels(monkeypatch):
    from freebsd_cache_hotspot.sweep import CcxGroup, _LiveCanvas

    monkeypatch.delenv("NO_COLOR", raising=False)
    monkeypatch.delenv("PY_COLORS", raising=False)
    monkeypatch.setenv("TERM", "xterm-256color")
    out = TtyStringIO()
    canvas = _LiveCanvas(
        [CcxGroup(0, (0,)), CcxGroup(1, (16,))], [32, 32768],
        l1d_kb=32, l2_kb=1024, l3_kb=32768,
        out=out, color=True,
    )
    canvas.update(0, 32, 2.0)
    canvas.update(1, 32, 3.0)
    canvas.update(0, 32768, 40.0)
    canvas.update(1, 32768, 65.0)
    canvas.finalize()
    text = out.getvalue()
    assert "latest sample: CCX 1 32M" in text
    assert "Live same-bucket hotspots:" in text
    assert "Live cross-CCX spread:" in text


def test_live_canvas_restores_sigwinch_handler(monkeypatch):
    import signal
    from freebsd_cache_hotspot.sweep import CcxGroup, _LiveCanvas

    if not hasattr(signal, "SIGWINCH"):
        return

    monkeypatch.delenv("NO_COLOR", raising=False)
    monkeypatch.delenv("PY_COLORS", raising=False)
    monkeypatch.setenv("TERM", "xterm-256color")
    original = signal.getsignal(signal.SIGWINCH)
    canvas = _LiveCanvas(
        [CcxGroup(0, (0,))], [1],
        l1d_kb=32, l2_kb=1024, l3_kb=32768,
        out=TtyStringIO(), color=True,
    )
    assert signal.getsignal(signal.SIGWINCH) != original
    canvas.finalize()
    assert signal.getsignal(signal.SIGWINCH) == original


def test_build_scale_rejects_invalid_mode_with_empty_values():
    import pytest
    from freebsd_cache_hotspot.sweep import _build_scale

    with pytest.raises(ValueError):
        _build_scale([], "invalid")


def test_hotspot_analysis_identifies_slowest_same_bucket_ccx():
    from freebsd_cache_hotspot.sweep import bucket_spreads, hotspot_cells

    result = _synthetic_sweep_result()
    hot = hotspot_cells(result, 1)[0]
    assert hot.ccx_id == 1
    assert hot.kb == 32768
    spread = bucket_spreads(result, 1)[0]
    assert spread.kb == 32768
    assert spread.slowest_ccx == 1


def test_final_heatmap_labels_no_positive_cliff_as_flat():
    from freebsd_cache_hotspot.sweep import BucketResult, CcxGroup, SweepResult, render_final_heatmap

    result = SweepResult(
        topo_brand="AMD test",
        generation="Zen 4",
        ccx_groups=[CcxGroup(0, (0,))],
        buckets_kb=[1, 2, 4],
        rows=[BucketResult(0, 0, 1, 3.0), BucketResult(0, 0, 2, 2.0), BucketResult(0, 0, 4, 1.0)],
        l1d_kb=32,
        l2_kb=1024,
        l3_kb=32768,
    )
    out = io.StringIO()
    render_final_heatmap(result, out=out, color=False)
    assert "flat" in out.getvalue()


def _synthetic_topology():
    from types import SimpleNamespace

    return SimpleNamespace(
        cpu=SimpleNamespace(generation="Zen 4", brand="AMD test", family=0x19, model=0x11, stepping=1),
        online_cpus=1,
        cache_size_kb=lambda level, cache_type=None: {1: 32, 2: 1024, 3: 32768}.get(level, 0),
    )


def test_run_parallel_sweep_rejects_invalid_repeat():
    import pytest
    from freebsd_cache_hotspot.sweep import run_parallel_sweep

    with pytest.raises(ValueError):
        run_parallel_sweep(_synthetic_topology(), min_kb=1, max_kb=2, steps=2, repeat=0)


def test_run_parallel_sweep_fails_when_all_samples_missing(monkeypatch):
    import pytest
    from freebsd_cache_hotspot import sweep

    monkeypatch.setattr(sweep._fb, "measure_chase_bucket", lambda **kwargs: None)
    with pytest.raises(RuntimeError, match="no samples"):
        sweep.run_parallel_sweep(_synthetic_topology(), min_kb=1, max_kb=2, steps=2, repeat=1, out=io.StringIO())


def test_write_new_text_refuses_existing_file(tmp_path):
    import pytest
    from freebsd_cache_hotspot.__main__ import _write_new_text

    target = tmp_path / "existing.csv"
    target.write_text("original", encoding="utf-8")
    with pytest.raises(FileExistsError):
        _write_new_text(str(target), "replacement")
    assert target.read_text(encoding="utf-8") == "original"
