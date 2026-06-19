# pyc_compute_freebsd

FreeBSD AMD Zen cache-hotspot localization using the same Python + native C shape as `pyc_compute`: Python owns orchestration and reporting, while C owns CPUID, fenced TSC timing, CPU pinning, cache probes, and libpmc calls.

This port targets the FreeBSD server first: **AMD Zen 4, Family 19h Model 10h-1Fh/A0h-AFh class EPYC**, 192 logical CPUs.  The tool still probes CPUID before selecting event semantics; Family 1Ah is not treated as Zen 5 without checking the model range.

## Layers

- **Silicon:** CPUID `0x00000001`, `0x8000001B`, and `0x80000022`; AMD core PMCs (`PMCx076`, `PMCx0C0`, `PMCx043`, `PMCx064`, `PMCx1A0`); IBS Op through FreeBSD `hwpmc(4)`/`pmcstat(8)`.
- **FreeBSD kernel:** `hwpmc(4)` and `libpmc(3)` process/system counting modes; `PMC_MODE_TC` for process counting and `PMC_MODE_SC` for CPU system counting.
- **Userland tooling:** Python CLI, native CPython extension `_fbcacheprobe`, `pmcstat -S ibs-op` for IBS pmclog collection/decoding in this first FreeBSD path.
- **Firmware/platform policy:** NUMA and CPUID masking are reported, not assumed; use `cpuset(1)`/first-touch policy externally when proving far-DRAM behavior.

## Build on FreeBSD

```sh
python3 setup.py build_ext --inplace
```

Host-tuned Zen 4 build:

```sh
FCH_MARCH=-march=znver4 python3 setup.py build_ext --inplace
```

The FreeBSD build links the C extension with `-lpmc`.  Load `hwpmc(4)` before counting or IBS sampling:

```sh
sudo kldload hwpmc
kldstat -n hwpmc
```

## CPU/generation detection

Always start with topology:

```sh
python3 -m freebsd_cache_hotspot topo
cpucontrol -i 0x00000001 /dev/cpuctl0
cpucontrol -i 0x8000001b /dev/cpuctl0
cpucontrol -i 0x80000022 /dev/cpuctl0
```

On Zen 4, expect Family `0x19`, model range `0x10-0x1f`, `0x60-0x7f`, or `0xa0-0xaf`; dispatch width is 6 slots.  `CPUID Fn80000022_EAX[0]` reports PerfMonV2, and `Fn80000022_EBX[3:0]` reports core PMC count.

## Cache probes

Run the C pointer-chase and sequential-read probes pinned to CPU 0:

```sh
python3 -m freebsd_cache_hotspot probe --cpu 0 --min-kb 1 --max-kb 262144 --steps 50 --repeat 5 --bandwidth
```

For 192-way machines, run per NUMA domain by choosing one CPU from each domain and controlling memory placement externally with FreeBSD `cpuset(1)`/first-touch policy.

## Live terminal per-CCX hotspot sweep

On the EPYC 9654 target (`Family 19h Model 11h Step 1`, Zen 4), start with the
parallel CCX sweep.  It runs one native pointer-chase thread per inferred Zen 4
CCX group, pins each thread in C, and updates a live terminal heatmap.  The
primary view is the CLI dashboard while the sweep is running:

```sh
python3 -m freebsd_cache_hotspot sweep \
  --min-kb 1 --max-kb 262144 --steps 40 --repeat 3 \
  --scale robust --top-hotspots 12
```

The terminal view is optimized for immediate triage: rows are CCX groups, columns
are pointer-chase footprints, and heat is `ns/load`.  While the sweep is still
running, the CLI dashboard also shows the latest sample, the current worst
same-bucket CCX outliers, and the current buckets with the largest cross-CCX
spread.  The final terminal report repeats those rankings with the completed
data set.  CSV is only a raw sample export:

```sh
outdir=$(mktemp -d /tmp/fch-sweep.XXXXXX)
python3 -m freebsd_cache_hotspot sweep \
  --min-kb 1 --max-kb 262144 --steps 40 --repeat 3 \
  --scale robust --top-hotspots 12 \
  --csv "$outdir/fch-sweep.csv"
printf 'CSV samples: %s/fch-sweep.csv\n' "$outdir"
```

The `make sweep` convenience target runs the same canonical sweep and writes the
optional CSV into a private temporary directory.

For Zen 4 interpretation, treat the built-in cache bands as measurement landmarks,
not conclusions: L1D is around 32 KiB, L2 is around 1 MiB, and each EPYC Genoa
CCX/CCD exposes a 32 MiB L3 slice.  High latency beyond the L3 slice is expected;
same-bucket CCX outliers are the useful cache-hotspot candidates to follow up with
PMC counts and IBS Op sampling.

Useful variants:

```sh
# Exact min/max scale instead of p5/p95 clipping.
python3 -m freebsd_cache_hotspot sweep --scale linear

# Log-safe output without ANSI color.
python3 -m freebsd_cache_hotspot sweep --no-color
```

`sweep` should run unprivileged when possible.  The PMC/IBS commands below may
require `hwpmc(4)` privileges; any workload passed after `--` runs with the
privileges of the Python process, so do not pass untrusted commands to a
`sudo`-wrapped invocation.

## Core PMC counting

Process-scope counting (`PMC_MODE_TC`) around a command:

```sh
python3 -m freebsd_cache_hotspot count -- /bin/true
```

Zen 4 cache-source set:

```sh
python3 -m freebsd_cache_hotspot count \
  --events cycles,instructions,dc_fill_l3,dc_fill_near_dram,dc_fill_far_dram,l2_miss \
  -- ./your_workload
```

Zen 4 dispatch-slot approximation:

```sh
python3 -m freebsd_cache_hotspot count --topdown -- ./your_workload
```

System-scope counting on CPU 0 (`PMC_MODE_SC`) is available for controlled pinned workloads, but it counts whatever runs on that CPU:

```sh
python3 -m freebsd_cache_hotspot count --system --cpu 0 --events cycles,instructions -- ./your_workload
```

The default Zen 4 event specs are FreeBSD PMU JSON/libpmc names:

| Tool key | FreeBSD event spec | Silicon event | UnitMask |
|---|---|---:|---:|
| `cycles` | `ls_not_halted_cyc` | `PMCx076` | `0x00` |
| `instructions` | `ex_ret_instr` | `PMCx0C0` | `0x00` |
| `dc_fill_l3` | `ls_dmnd_fills_from_sys.local_ccx` | `PMCx043` | `0x02` |
| `dc_fill_near_dram` | `ls_dmnd_fills_from_sys.dram_io_near` | `PMCx043` | `0x08` |
| `dc_fill_far_dram` | `ls_dmnd_fills_from_sys.dram_io_far` | `PMCx043` | `0x40` |
| `l2_miss` | `l2_cache_req_stat.ls_rd_blk_c` | `PMCx064` | `0x08` |
| `frontend_slots` | `de_no_dispatch_per_slot.no_ops_from_frontend` | `PMCx1A0` | `0x01` |
| `backend_slots` | `de_no_dispatch_per_slot.backend_stalls` | `PMCx1A0` | `0x1E` |
| `smt_slots` | `de_no_dispatch_per_slot.smt_contention` | `PMCx1A0` | `0x60` |

## IBS Op sampling

FreeBSD IBS is exposed through `hwpmc(4)` as sampling PMCs and decoded by `pmcstat(8)`.  This first FreeBSD port drives the native FreeBSD path instead of Linux `perf_event` rings:

```sh
python3 -m freebsd_cache_hotspot ibs --period 65536 -- ./your_workload
python3 -m freebsd_cache_hotspot ibs --period 65536 --l3miss -- ./your_workload
python3 -m freebsd_cache_hotspot ibs --period 65536 --l3miss --ldlat 256 -- ./your_workload
```

FreeBSD `hwpmc_ibs.h` uses a minimum IBS Op/Fetch rate of `65536`; do not carry Linux `perf_event` examples such as `1024` or `5120` into this port.

## Measurement hygiene

- Identify CPU family/model/stepping first: `python3 -m freebsd_cache_hotspot topo` plus `cpucontrol` CPUID leaves.
- Do at least five runs, discard the first, and report mean/stddev.
- Do not oversubscribe six Zen 4 core PMC pairs without documenting sequential-start skew or running separate passes.
- For SMT, remember system-scope core PMCs count sibling-thread activity on that core.
- For NUMA, pin CPU and memory placement; do not call far DRAM a silicon problem until first-touch policy is controlled.
