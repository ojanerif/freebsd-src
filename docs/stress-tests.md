# Stress Test Suite — Reference

AMD IBS / FreeBSD stress test suite for `tests/sys/amd/stress/`.  
Six ATF test programs plus four standalone stressor binaries, organized
into four resource-isolated batches by `run.sh`.

---

## Overview

Stress tests validate that IBS hardware, kernel MSR paths, and system
resources remain stable under sustained load. They are designed to
surface NMI re-arm races, MSR corruption under concurrency, and driver
latency regressions.

Stress tests run in **Phase 2** of `run.sh` — after all non-stress tests
complete at full parallelism. Within each batch, tests run in parallel;
batches themselves are sequential to avoid cross-resource interference.

---

## Execution batches

### Batch 1 — CPU (TC-CSTR, TC-ISTR, TC-FSTR, TC-STR, TC-NMISTR)

| Program | Category | Description |
|---|---|---|
| `cpu_stress_test` | TC-CSTR | CPU compute, context-switch, and FPU sustained stress (120 s) |
| `ibs_op_stress_test` | TC-ISTR | IBS Op sampling coherence under CPU / memory / disk / network load |
| `ibs_fetch_stress_test` | TC-FSTR | IBS Fetch sampling coherence under CPU / memory / disk / network load |
| `ibs_stress_test` | TC-STR | Rapid enable/disable, period changes, concurrent MSR access, 60 s run |
| `ibs_cpu_stress_test` | TC-STR | IBS MSR stability during pipeline saturation and context switches |
| `ibs_nmi_stress_test` | TC-NMISTR | NMI delivery stability and AMD Erratum #420 drain under 192-CPU load |

### Batch 2 — Memory (TC-MSTR, TC-MEMIBS)

| Program | Category | Description |
|---|---|---|
| `mem_stress_test` | TC-MSTR | Cache thrash, DRAM bandwidth saturation, and TLB pressure (130 s) |
| `ibs_mem_stress_test` | TC-MEMIBS | IBS Op sampling coherence during L2/L3/DRAM cache-thrash stress |

### Batch 3 — Disk (TC-DSTR)

| Program | Category | Description |
|---|---|---|
| `disk_stress_test` | TC-DSTR | Sequential I/O, random I/O, and fsync durability stress (120 s) |

### Batch 4 — Network (TC-NSTR)

| Program | Category | Description |
|---|---|---|
| `net_stress_test` | TC-NSTR | AF_UNIX socketpair, TCP loopback, and connection-rate stress (125 s) |

---

## Stressor binaries

Four plain C programs used as background load by the IBS stress tests.
They are not ATF programs and are not run by kyua directly.

| Binary | Description |
|---|---|
| `cpu_stressor` | Spawns N threads running compute / context-switch / FPU loops |
| `mem_stressor` | Allocates and randomly accesses a large buffer to thrash L2/L3/DRAM |
| `disk_stressor` | Sequential and random writes to a temp file with fsync |
| `net_stressor` | TCP and AF_UNIX loopback connections at configurable rate |

---

## Test case summary

### `cpu_stress_test` (TC-CSTR)

| Test case | Duration | Description |
|---|---|---|
| `cpu_stress_compute` | 120 s | Integer/FPU loop across all available CPUs |
| `cpu_stress_context_switch` | 120 s | High-frequency thread wake/sleep context switching |
| `cpu_stress_fpu` | 120 s | FPU-intensive workload targeting vector pipelines |

### `ibs_op_stress_test` (TC-ISTR)

| Test case | Duration | Description |
|---|---|---|
| `ibs_op_cpu_stress` | 120 s | IBS Op sampling while CPU stressor runs in background |
| `ibs_op_mem_stress` | 120 s | IBS Op sampling while memory stressor runs in background |
| `ibs_op_disk_stress` | 120 s | IBS Op sampling while disk stressor runs in background |
| `ibs_op_net_stress` | 120 s | IBS Op sampling while network stressor runs in background |

### `ibs_fetch_stress_test` (TC-FSTR)

| Test case | Duration | Description |
|---|---|---|
| `ibs_fetch_cpu_stress` | 120 s | IBS Fetch sampling while CPU stressor runs in background |
| `ibs_fetch_mem_stress` | 120 s | IBS Fetch sampling while memory stressor runs in background |
| `ibs_fetch_disk_stress` | 120 s | IBS Fetch sampling while disk stressor runs in background |
| `ibs_fetch_net_stress` | 120 s | IBS Fetch sampling while network stressor runs in background |

### `ibs_stress_test` (TC-STR)

| Test case | Duration | Description |
|---|---|---|
| `ibs_stress_rapid_enable_disable` | ~3 s | 1000× IBS Op enable/disable cycle; checks no MSR corruption |
| `ibs_stress_period_changes` | ~1 s | Rapid period changes while IbsOpEn=0; verifies MaxCnt preserved (known skip: FreeBSD-Tests-010) |
| `ibs_stress_concurrent_msr_access` | 22 s | 8 threads hammering IBS MSRs concurrently |
| `ibs_stress_long_running` | 60 s | IBS Op sampling for 60 s; verifies no drift or corruption |

### `ibs_cpu_stress_test` (TC-STR)

| Test case | Duration | Description |
|---|---|---|
| `ibs_cpu_stress_compute` | 120 s | IBS MSR access while CPU compute stressor saturates all cores |
| `ibs_cpu_stress_context_switch` | 120 s | IBS MSR access under high-frequency context switching |

### `ibs_nmi_stress_test` (TC-NMISTR)

| Test case | Duration | Description |
|---|---|---|
| `ibs_nmi_drain_under_load` | ~3 s | AMD Erratum #420 drain sequence 100× under 192-CPU load; asserts all iterations complete within 5000 µs |
| `ibs_nmi_high_rate_stability` | 120 s | IBS Op at max rate (period=16) for 120 s; no NMI storm or watchdog |
| `ibs_nmi_rate_limit_enforce` | — | Rate-limit sysctl enforcement (skips: sysctl not yet implemented — FreeBSD-Tests-026) |

### `mem_stress_test` (TC-MSTR)

| Test case | Duration | Description |
|---|---|---|
| `mem_stress_bandwidth` | 120 s | Streaming read/write across a 2 GB buffer to saturate DRAM bandwidth |
| `mem_stress_cache_thrash` | 120 s | Random accesses across buffer larger than L3 to evict cache lines |
| `mem_stress_tlb` | 130 s | Many small allocations across page boundaries to pressure TLB |

### `ibs_mem_stress_test` (TC-MEMIBS)

| Test case | Duration | Description |
|---|---|---|
| `ibs_mem_stress_bandwidth` | 126 s | IBS Op sampling while memory bandwidth stressor runs |
| `ibs_mem_stress_cache_thrash` | 126 s | IBS Op sampling while cache-thrash stressor runs |

### `disk_stress_test` (TC-DSTR)

| Test case | Duration | Description |
|---|---|---|
| `disk_stress_sequential` | ~0 s | Sequential write/read of a 256 MB file; verifies data integrity |
| `disk_stress_random` | 120 s | Random 4 KB writes across a 256 MB file for 120 s |
| `disk_stress_fsync` | 120 s | Repeated small writes + fsync; verifies durability under pressure |

### `net_stress_test` (TC-NSTR)

| Test case | Duration | Description |
|---|---|---|
| `net_stress_unix_socketpair` | 120 s | AF_UNIX socketpair send/recv at maximum throughput |
| `net_stress_loopback_tcp` | 125 s | TCP loopback echo with multiple concurrent connections |
| `net_stress_connection_rate` | 120 s | New TCP connection establishment rate on loopback |

---

## Known skip conditions

| Test case | Reason |
|---|---|
| `ibs_stress_period_changes` | IbsOpEn=0 race: kernel NMI handler re-arms counter; tracked as FreeBSD-Tests-010 |
| `ibs_nmi_rate_limit_enforce` | `dev.hwpmc.ibs.min_period` sysctl not implemented; tracked as FreeBSD-Tests-026 |

---

## Running stress tests

```sh
# Run full stress suite (all 4 batches, parallel within each)
sudo ./run.sh --run-all --suite STRESS --force

# Run IBS suite including stress tests
sudo ./run.sh --run-all --suite IBS --stress --force

# Run all suites (Phase 1 non-stress + Phase 2 stress batches)
sudo ./run.sh --run-all --suite ALL --force

# Run a specific stress category
sudo ./run.sh --run-all --suite STRESS --category TC-NMISTR --force
```
