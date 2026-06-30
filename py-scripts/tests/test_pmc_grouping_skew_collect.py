#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: davi.chavesazevedo@amd.com

from __future__ import annotations

import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

PY_SCRIPTS = Path(__file__).resolve().parents[1]
if str(PY_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(PY_SCRIPTS))


def test_parse_pmcstat_two_counter_uses_last_numeric_row() -> None:
    from pmc_grouping_skew_collect import parse_pmcstat_two_counter

    parsed = parse_pmcstat_two_counter(
        "\n".join([
            "# pmcstat header",
            "100 110",
            "not numeric",
            "300 250 trailing fields ignored",
        ])
    )

    assert parsed.valid is True
    assert parsed.row_count == 2
    assert parsed.last_a == 300
    assert parsed.last_b == 250
    assert parsed.max_count == 300
    assert parsed.delta_abs == 50
    assert parsed.signed_delta == -50
    assert parsed.permille == pytest.approx(50 * 1000.0 / 300)
    assert parsed.a_gt_b is True
    assert parsed.b_gt_a is False
    assert parsed.equal is False


def test_parse_pmcstat_two_counter_reports_empty_and_non_numeric_output() -> None:
    from pmc_grouping_skew_collect import parse_pmcstat_two_counter

    empty = parse_pmcstat_two_counter("\n")
    non_numeric = parse_pmcstat_two_counter("# header\ncycles instructions\n")

    assert empty.valid is False
    assert empty.reason == "empty output"
    assert non_numeric.valid is False
    assert non_numeric.reason == "no numeric rows"


def test_apply_record_correction_subtracts_signed_calibration_offset() -> None:
    from pmc_grouping_skew_collect import ProbeRecord, apply_record_correction

    record = ProbeRecord(valid=True, signed_delta=120, max_count=10_000)
    invalid = ProbeRecord(valid=False, signed_delta=120, max_count=10_000)

    corrected = apply_record_correction(record, baseline_signed_offset=20.0)
    skipped = apply_record_correction(invalid, baseline_signed_offset=20.0)

    assert corrected.baseline_signed_offset == 20.0
    assert corrected.corrected_signed_delta == 100.0
    assert corrected.corrected_delta == 100.0
    assert corrected.corrected_permille == 10.0
    assert record.corrected_permille is None
    assert skipped.corrected_permille is None


def test_verdict_from_stats_reports_sample_and_cycle_failures() -> None:
    from pmc_grouping_skew_collect import ProbeRecord, RunState, verdict_from_stats

    args = SimpleNamespace(
        dry_run=False,
        verdict_percentile=0.95,
        corrected_tol=1.0,
        calibration_n=2,
        measurement_n=2,
        min_valid_samples=2,
        min_measurement_cycles=1_000_000,
        offset_stability_warn_pct=30.0,
    )
    calibration = [
        ProbeRecord(valid=True, signed_delta=100),
        ProbeRecord(valid=True, signed_delta=110),
    ]
    measurement = [
        ProbeRecord(valid=True, corrected_permille=0.5, max_count=999_999),
        ProbeRecord(valid=False),
    ]

    verdict = verdict_from_stats(
        calibration,
        measurement,
        baseline_signed_offset=105.0,
        args=args,
        state=RunState(),
    )

    assert verdict.pass_ is False
    assert verdict.status == "fail"
    assert verdict.partial is False
    assert "measurement has 1 valid samples; need 2" in verdict.reasons
    assert any(
        reason.startswith("measurement median max_count 999999 cycles")
        for reason in verdict.reasons
    )


def test_dry_run_verdict_does_not_claim_hardware_result() -> None:
    from pmc_grouping_skew_collect import RunState, verdict_from_stats

    args = SimpleNamespace(
        dry_run=True,
        verdict_percentile=0.95,
        corrected_tol=1.0,
        calibration_n=1,
        measurement_n=1,
        min_measurement_cycles=1_000_000,
    )

    verdict = verdict_from_stats(
        [],
        [],
        baseline_signed_offset=0.0,
        args=args,
        state=RunState(),
    )

    assert verdict.pass_ is None
    assert verdict.status == "dry_run"
    assert verdict.metric_value is None
    assert verdict.reasons == ["dry-run: commands were printed but PMCs were not executed"]
