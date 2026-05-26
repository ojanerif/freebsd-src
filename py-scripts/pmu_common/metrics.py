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

"""Small numeric helpers for PMU collection summaries."""

from __future__ import annotations

import math
from typing import Optional, Sequence


def percentile(values: Sequence[float], pct: float) -> Optional[float]:
    """Return the interpolated percentile for a numeric sequence.

    ``pct`` is a fraction in the inclusive range [0.0, 1.0].  ``None`` is
    returned for an empty sequence so callers can distinguish no data from a
    legitimate zero percentile.
    """
    if not math.isfinite(pct) or pct < 0.0 or pct > 1.0:
        raise ValueError("pct must be in the range [0.0, 1.0]")
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
