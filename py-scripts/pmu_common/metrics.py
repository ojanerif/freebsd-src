#!/usr/bin/env python3
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Small statistical and display helpers for PMU skew summaries.

"""PMU skew metric helpers."""

from __future__ import annotations

import math
from typing import Optional, Sequence


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


def permille_to_percent(value: Optional[float]) -> Optional[float]:
    if value is None:
        return None

    return value / 10.0


def format_skew(value: Optional[float]) -> str:
    if value is None:
        return "n/a"

    return f"{permille_to_percent(value):.3f}% ({value:.3f}‰)"
