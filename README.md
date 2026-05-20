# freebsd-ci-actions

FreeBSD CI integration and ATF test suite for AMD performance monitoring —
IBS (Instruction-Based Sampling), Uncore PMC (L3, DF, UMC), and
miscellaneous PMC / hwpmc API tests.

Validated on **AMD EPYC 9654 (Zen 4, 192 CPUs)**, FreeBSD 16.0-CURRENT amd64.

---

## Repository layout

```
freebsd-ci-actions/
├── run.sh                    # Test suite manager — build, run, report, commit
├── ci/
│   └── tools/
│       ├── ci.conf           # VM image configuration for FreeBSD CI builds
│       └── freebsdci         # RC script: first-boot test orchestration in CI VMs
├── docs/
│   ├── ibs-tests.md          # IBS test reference: all test IDs, status, expected results
│   ├── stress-tests.md       # Stress suite reference: batches, categories, stressors
│   └── TODO.md               # Planned Uncore PMC and Misc PMC test cases
└── tests/
    └── sys/
        └── amd/
            ├── ibs/          # IBS ATF test suite (36 programs)
            ├── umcdf/        # UMC / Data Fabric ATF test suite (10 programs)
            ├── pmc/          # hwpmc / pmcstat ATF test suite (5 programs)
            ├── l3/           # L3 cache PMU ATF test suite (3 programs)
            └── stress/       # Stress ATF test suite (6 programs + 4 stressor binaries)
```

---

## Test suites

### IBS — Instruction-Based Sampling (`tests/sys/amd/ibs/`)

36 test programs covering AMD IBS hardware via `cpuctl.ko` and `hwpmc.ko`,
plus pure software decode helpers that require no hardware access.

| Program | Cat | Description |
|---|---|---|
| `ibs_detect_test` | TC-DET | IBS feature detection via CPUID and Zen generation identification |
| `ibs_cpu_test` | TC-DET | AMD CPU family/model detection for IBS-capable processors |
| `ibs_msr_test` | TC-MSR | IBS MSR basic read/write operations via cpuctl |
| `ibs_period_test` | TC-MSR | IBS Fetch and Op period encoding, min/max/rollover validation |
| `ibs_routing_test` | TC-INT | IBS enable/disable, Op/Fetch cnt-ctl, and global IBSCTL routing |
| `ibs_interrupt_test` | TC-INT | NMI delivery, VAL bit observation, and spurious interrupt handling |
| `ibs_api_test` | TC-API | IBS MSR round-trip and reserved-bit isolation via userspace API |
| `ibs_swfilt_test` | TC-API | Software filter control bits in IBS Fetch and Op control MSRs |
| `ibs_ioctl_test` | TC-API | IBS ioctl API (placeholder — not yet implemented in kernel) |
| `ibs_data_accuracy_test` | TC-DATA | IBS sample data field extraction, DataSrc encodings, dc/fetch addresses |
| `ibs_l3miss_test` | TC-DATA | L3MissOnly filter detection and behavior on Zen 4+ processors |
| `ibs_smp_test` | TC-SMP | Per-core IBS MSR isolation and CPU migration on multi-core systems |
| `ibs_hwpmc_alloc_test` | TC-HWPMC | IBS PMC allocation, rejection of bad rate/qualifier/mode via libpmc |
| `ibs_hwpmc_caps_test` | TC-HWPMC | PMC capability width and event queries for IBS class events |
| `ibs_hwpmc_info_test` | TC-HWPMC | IBS class name and event visibility in hwpmc/libpmc |
| `ibs_hwpmc_runtime_test` | TC-HWPMC | IBS Fetch PMC lifecycle: allocate, attach, start, stop, detach |
| `ibs_cpuctl_access_test` | TC-DRV | cpuctl driver device enumeration, CPUID, and open mode validation |
| `ibs_robustness_test` | TC-CONC | Kernel survival under adversarial IBS usage, affinity switching, fork |
| `ibs_concurrency_test` | TC-CONC | Multiprocess concurrent MSR access and signal-storm under sampling |
| `ibs_access_control_test` | TC-SEC | MSR access privilege enforcement for unprivileged users |
| `ibs_invalid_input_test` | TC-SEC | Out-of-range MSR and invalid ioctl inputs without kernel panic |
| `ibs_unit_field_masks_test` | TC-UNIT | Field mask constants verified against AMD PPR (no hardware) |
| `ibs_unit_helpers_test` | TC-UNIT | MaxCnt get/set/clamp and period conversion helper unit tests |
| `ibs_unit_datasrc_test` | TC-UNIT | DataSrc field extraction from MSR_IBS_OP_DATA2 unit tests |
| `ibs_unit_cpuid_parse_test` | TC-UNIT | CPU family/model/stepping parsing for Zen 1–5 unit tests |
| `ibs_unit_op_ext_maxcnt_test` | TC-UNIT | Zen 2+ extended Op MaxCnt (23-bit) field unit tests |
| `ibs_unit_feature_flags_test` | TC-UNIT | IBS CPUID feature flag bit positions and accessor unit tests |
| `ibs_unit_fetch_ctl_fields_test` | TC-UNIT | IBS Fetch CTL multi-bit field encoding/decoding unit tests |
| `ibs_unit_op_data_fields_test` | TC-UNIT | IBS Op Data 1–3 field bit layout and extraction unit tests |
| `ibs_unit_msr_range_test` | TC-UNIT | IBS MSR address range, offsets, and uniqueness unit tests |
| `ibs_unit_ldlat_test` | TC-UNIT | Load latency threshold field encoding and overlap unit tests |
| `ibs_unit_zen3_errata_test` | TC-UNIT | Zen 3 IBS errata decode policy model unit tests (no hardware) |
| `ibs_stress_test` | TC-STR | IBS rapid enable/disable, period changes, concurrent MSR access, long-running |
| `ibs_cpu_stress_test` | TC-STR | IBS MSR stability under CPU pipeline saturation and context switches |
| `ibs_mem_stress_test` | TC-MEMIBS | IBS coherence under L2/L3/DRAM bandwidth and cache-thrash stress |
| `ibs_nmi_stress_test` | TC-NMISTR | NMI delivery stability and AMD Erratum #420 drain under 192-CPU load |

**Shared headers:** `ibs_utils.h` (MSR I/O, hardware accessors), `ibs_decode.h` and `ibs_zen3_errata_decode.h` (pure field/policy helpers, no I/O).

---

### UMCDF — UMC and Data Fabric PMU (`tests/sys/amd/umcdf/`)

10 test programs covering AMD Unified Memory Controller and Data Fabric PMU detection and event enumeration.

| Program | Cat | Description |
|---|---|---|
| `umcdf_cpuid_test` | TC-UMCDET | CPU generation classification from CPUID via UMCDF decode |
| `umcdf_df_test` | TC-UMCPMC | DF PMU capability probe, event row count, and PMU metadata |
| `umcdf_umc_test` | TC-UMCPMC | UMC capability probe and metadata contract validation |
| `umcdf_unit_capabilities_test` | TC-UMCUNIT | UMC/DF capability predicate function unit tests (no hardware) |
| `umcdf_unit_df_config_dispatch_test` | TC-UMCUNIT | DF encoding dispatch logic correctness unit tests |
| `umcdf_unit_df_encoding_test` | TC-UMCUNIT | DF event/unit mask encoding macro unit tests |
| `umcdf_unit_perfmonv2_test` | TC-UMCUNIT | CPUID PerfMonV2 (Fn80000022) field parsing unit tests |
| `umcdf_unit_vendor_test` | TC-UMCUNIT | AMD vendor identification constant unit tests |
| `umcdf_unit_zen_map_test` | TC-UMCUNIT | Zen generation mapping function unit tests |
| `umcdf_unit_zen_name_test` | TC-UMCUNIT | Zen generation name string function unit tests |

**Shared headers:** `amd_umcdf_common.h`, `amd_umcdf_decode.h`.

---

### PMC — hwpmc / pmcstat (`tests/sys/amd/pmc/`)

5 test programs covering AMD core PMC negative-path error handling, grouping,
and pmcstat(8) offline decode behavior.

| Program | Cat | Description |
|---|---|---|
| `hwpmc_exterr_test` | TC-PMCAPI | Extended error text for PMC_OP_PMCALLOCATE / PMCATTACH / PMCRW failures (14 cases) |
| `hwpmc_grouping_test` | TC-PMCAPI | hwpmc(4) event grouping behavior for Zen core PMCs via libpmc |
| `pmcstat_grouping_test` | TC-PMCSTAT | pmcstat(8) grouped system-wide counting output validation (shell) |
| `pmcstat_ibs_errata_test` | TC-PMCSTAT | pmcstat(8) offline IBS Fetch erratum #1238 decode validation using synthetic pmclog input |
| `pmcstat_tsc_test` | TC-PMCSTAT | pmcstat TSC column presence and frequency integration smoke test (shell) |

---

### L3 — L3 Cache PMU (`tests/sys/amd/l3/`)

3 test programs covering AMD L3 cache PMU detection and event validation.

| Program | Cat | Description |
|---|---|---|
| `l3_detect_test` | — | L3 cache PMU presence detection and capability probing |
| `l3_hit_test` | — | L3 cache hit counter event enumeration and metadata validation |
| `l3_miss_test` | — | L3 cache miss counter event enumeration and metadata validation |

---

### STRESS — Stress tests (`tests/sys/amd/stress/`)

6 ATF test programs plus 4 standalone stressor binaries. Stress tests run in
resource-isolated batches (see [Execution model](#execution-model)).

| Program | Cat | Description |
|---|---|---|
| `cpu_stress_test` | TC-CSTR | CPU compute, context-switch, and FPU stress workloads |
| `ibs_op_stress_test` | TC-ISTR | IBS Op sampling coherence under CPU/memory/disk/network stress |
| `ibs_fetch_stress_test` | TC-FSTR | IBS Fetch sampling coherence under CPU/memory/disk/network stress |
| `ibs_stress_test` | TC-STR | IBS rapid enable/disable, period changes, and long-running stability |
| `ibs_cpu_stress_test` | TC-STR | IBS MSR stability under CPU pipeline saturation and context switches |
| `mem_stress_test` | TC-MSTR | Memory cache thrash, bandwidth saturation, and TLB pressure |
| `ibs_mem_stress_test` | TC-MEMIBS | IBS coherence during L2/L3/DRAM bandwidth and cache-thrash stress |
| `disk_stress_test` | TC-DSTR | Sequential and random disk I/O with fsync integrity validation |
| `net_stress_test` | TC-NSTR | AF_UNIX socketpair, TCP loopback, and connection-rate stress |
| `ibs_nmi_stress_test` | TC-NMISTR | NMI delivery and AMD Erratum #420 drain under 192-CPU load |

Stressor binaries (`cpu_stressor`, `mem_stressor`, `disk_stressor`, `net_stressor`) are
plain C programs spawned as background load by the IBS stress tests.

---

## Test categories

| Category | Suite | Tier | Description |
|---|---|---|---|
| TC-DET | IBS | E2E | CPU/IBS feature detection |
| TC-MSR | IBS | Integration | MSR read/write and period encoding |
| TC-INT | IBS | E2E | Interrupt delivery and routing |
| TC-API | IBS | Integration | Userspace API round-trips |
| TC-DATA | IBS | E2E | Sample data field accuracy |
| TC-SMP | IBS | E2E | SMP per-CPU isolation |
| TC-HWPMC | IBS | Integration | libpmc / hwpmc PMC API |
| TC-DRV | IBS | Integration | cpuctl driver interface |
| TC-CONC | IBS | E2E | Concurrency and robustness |
| TC-SEC | IBS | Integration | Privilege / access control |
| TC-UNIT | IBS | Unit | Pure C helpers, no hardware |
| TC-STR | IBS/STRESS | Stress | CPU/MSR stability stress — Batch 1 (CPU) |
| TC-NMISTR | IBS | Stress | NMI drain stress — Batch 1 (CPU) |
| TC-MEMIBS | IBS | Stress | IBS memory stress — Batch 2 (Memory) |
| TC-CSTR | STRESS | Stress | CPU compute stress — Batch 1 (CPU) |
| TC-ISTR | STRESS | Stress | IBS Op stress — Batch 1 (CPU) |
| TC-FSTR | STRESS | Stress | IBS Fetch stress — Batch 1 (CPU) |
| TC-MSTR | STRESS | Stress | Memory stress — Batch 2 (Memory) |
| TC-DSTR | STRESS | Stress | Disk stress — Batch 3 (Disk) |
| TC-NSTR | STRESS | Stress | Network stress — Batch 4 (Network) |
| TC-UMCDET | UMCDF | Unit | UMC/DF CPU detection |
| TC-UMCPMC | UMCDF | Integration | UMC/DF PMC lifecycle |
| TC-UMCUNIT | UMCDF | Unit | UMC/DF pure decode helpers |
| TC-PMCAPI | PMC | Integration | hwpmc API and error-path validation |
| TC-PMCSTAT | PMC | Integration | pmcstat command and pmclog decode paths |

---

## Execution model

`run.sh` uses a **two-phase execution model**:

**Phase 1 — Non-stress tests** run at full parallelism (`nproc` workers by default).
All TC-DET, TC-MSR, TC-INT, TC-API, TC-DATA, TC-SMP, TC-HWPMC, TC-DRV,
TC-CONC, TC-SEC, TC-UNIT, TC-UMCDET, TC-UMCPMC, TC-UMCUNIT, TC-PMCAPI,
and TC-PMCSTAT tests run here.

**Phase 2 — Stress tests** run in four sequential resource-isolated batches.
Tests *within* each batch still run in parallel:

| Batch | Categories | Resource |
|---|---|---|
| 1 | TC-CSTR TC-ISTR TC-FSTR TC-STR TC-NMISTR | CPU |
| 2 | TC-MSTR TC-MEMIBS | Memory |
| 3 | TC-DSTR | Disk |
| 4 | TC-NSTR | Network |

---

## Requirements

- FreeBSD amd64 with ATF (Automated Test Framework; included in FreeBSD base)
- Hardware IBS/MSR tests: AMD CPU with IBS support, `cpuctl.ko`, and root
  privileges for MSR access
- hwpmc/libpmc runtime tests: `hwpmc.ko` and root privileges when the test
  allocates or samples real PMCs
- Offline decode and pure unit tests: no live IBS MSR access and no AMD target
  CPU requirement; they operate on synthetic CPUID or pmclog data

Zen 4+ runtime tests require the matching AMD family/model and skip gracefully
on older hardware.

---

## Building

```sh
# IBS suite
cd tests/sys/amd/ibs && make

# UMCDF suite
cd tests/sys/amd/umcdf && make

# PMC suite
cd tests/sys/amd/pmc && make

# Stress suite
cd tests/sys/amd/stress && make
```

---

## Running

```sh
# Interactive menu
sudo ./run.sh

# Compile and install all suites
sudo ./run.sh --compile --suite ALL

# Run all suites (two-phase: non-stress parallel + stress batches)
sudo ./run.sh --run-all --suite ALL --force

# Run a single suite
sudo ./run.sh --run-all --suite IBS --force

# Run pmcstat decode tests
sudo ./run.sh --run-all --suite PMC --category TC-PMCSTAT --force

# Run a specific category only
sudo ./run.sh --run-all --suite IBS --category TC-UNIT --force

# Run with stress tests included
sudo ./run.sh --run-all --suite IBS --stress --force

# Fully automated: build kernel → reboot → test → email report
sudo ./run.sh --auto --suite IBS --kernconf GENERIC --email you@amd.com

# Run a single test case directly
kyua test -k 'ibs_detect_test:ibs_detect'
```

### Cron example

```sh
# Run nightly at 02:00 — idempotent, skips if source unchanged
0 2 * * * root /usr/home/osvaldo/freebsd-ci-actions/run.sh \
    --auto --suite ALL --kernconf GENERIC --force \
    --email ojanerif@amd.com >> /var/log/ibs-autotest-cron.log 2>&1
```

---

## CI integration

`ci/tools/ci.conf` is consumed by FreeBSD release engineering VM build scripts.
It configures `loader.conf`, `kyua.conf`, `rc.conf`, and `sysctl.conf` inside
the VM image for unattended test runs.

`ci/tools/freebsdci` is the RC script run on first boot inside the VM:

1. Extracts CI metadata from the tar device (`/dev/vtbd1`)
2. Runs a smoke pass or the full `kyua test` suite
3. Saves results as JUnit XML and plain text
4. Shuts down the VM

---

## References

- [docs/ibs-tests.md](docs/ibs-tests.md) — Full IBS test reference (IDs, categories, expected results)
- [docs/stress-tests.md](docs/stress-tests.md) — Stress suite reference (batches, stressors, categories)
- [docs/TODO.md](docs/TODO.md) — Planned Uncore PMC, L3, and Misc PMC test cases

---

## License

BSD 2-Clause — see individual file headers.
