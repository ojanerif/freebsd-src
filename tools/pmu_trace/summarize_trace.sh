#!/bin/sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Summarize trace_alloc_start.d CSV with POSIX sh + awk for CI logs.

set -eu

if [ "$#" -ne 1 ]; then
	printf 'usage: %s trace.csv\n' "$0" >&2
	exit 64
fi

trace_file="$1"
if [ ! -r "$trace_file" ]; then
	printf 'trace file is not readable: %s\n' "$trace_file" >&2
	exit 66
fi

awk -F, '
BEGIN {
    start_count = 0;
    pair_count = 0;
}
$1 == "start" {
    start_count++;
    if (last_ts != "" && $3 != last_ri && ($5 + 0) > last_ts) {
        delta = ($5 + 0) - last_ts;
        pair_count++;
        deltas[pair_count] = delta;
        sum += delta;
        if (min == "" || delta < min)
            min = delta;
        if (delta > max)
            max = delta;
    }
    last_ri = $3;
    last_ts = $5 + 0;
}
END {
    if (pair_count == 0) {
        printf("start_count=%d pair_count=0\n", start_count);
        exit(1);
    }
    for (i = 1; i <= pair_count; i++) {
        for (j = i + 1; j <= pair_count; j++) {
            if (deltas[j] < deltas[i]) {
                tmp = deltas[i];
                deltas[i] = deltas[j];
                deltas[j] = tmp;
            }
        }
    }
    if (pair_count % 2 == 0)
        median = (deltas[pair_count / 2] + deltas[pair_count / 2 + 1]) / 2;
    else
        median = deltas[int(pair_count / 2) + 1];
    printf("start_count=%d pair_count=%d min_ns=%.0f median_ns=%.0f mean_ns=%.1f max_ns=%.0f\n",
        start_count, pair_count, min, median, sum / pair_count, max);
}' "$trace_file"
