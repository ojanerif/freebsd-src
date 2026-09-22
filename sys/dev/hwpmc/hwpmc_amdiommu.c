/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * AMD IOMMU Performance Counter hwpmc(4) class driver.
 *
 * Implements the pmc_classdep ops table for AMD IOMMU performance counters
 * (EFR[PCSup]=1, MMIO topology at 0x4000h).  One class is registered for
 * all counter-capable IOMMU units; rows are laid out as
 * unit0_bank0_cntr0 .. unit0_bankN_cntrM, unit1_bank0_cntr0, ...
 *
 * Modelled on hwpmc_cmn600.c (uncore 48-bit programmable).
 * Per-CPU init/fini wires pc_hwpmcs[] only; no per-CPU hardware state.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/pmc.h>
#include <sys/pmckern.h>
#include <sys/systm.h>

#include "pmc_events.h"

/* Filter enable bits for pm_amdiommu_filter */
#define	AMDIOMMU_FILTER_PASID	0x01
#define	AMDIOMMU_FILTER_DOMAIN	0x02
#define	AMDIOMMU_FILTER_DEVID	0x04

/*
 * Per-row descriptor.
 * ri = unit * nbanks * ncounters + bank * ncounters + cntr
 */
struct amdiommu_descr {
	struct pmc_descr	 pd_descr;	/* base: class, caps, name, width */
	struct pmc_hw		 pd_hw;
	uint32_t		 pd_unit;	/* IOMMU unit index */
	uint8_t			 pd_bank;
	uint8_t			 pd_cntr;
	int			 pd_numa;	/* NUMA domain (-1 = unknown) */
};

static struct amdiommu_descr	**amdiommu_pmcdesc;
static int			  amdiommu_npmc;	/* total rows */
static int			  amdiommu_ri;		/* first row index in md */
static int			  amdiommu_maxcpu;	/* pmc_cpu_max() at init */
static int			  amdiommu_class_idx = -1;	/* pmd_classdep index */

#define	AMDIOMMU_CAPS	(PMC_CAP_SYSWIDE | PMC_CAP_READ | PMC_CAP_WRITE | \
			 PMC_CAP_DOMWIDE)

static inline struct amdiommu_descr *
amdiomdesc(int ri)
{
	return (amdiommu_pmcdesc[ri]);
}

static inline struct amdiommu_unit *
amdiom_unit(int ri)
{
	const struct pmc_amdiommu_unit *u;

	/*
	 * Resolve pd_unit through the SAME enumeration that produced it in
	 * pmc_amdiommu_initialize() — the kern_pmc registry index via
	 * pmc_amdiommu_unit().  Using the driver's TAILQ order instead
	 * (pmc_amdiommu_get_unit) would be a second, independent
	 * enumeration; the two only coincide when attach order matches
	 * ascending device-unit order, so on a system that attaches
	 * counter-capable IOMMUs out of order they would point at
	 * different physical units and counter access would silently
	 * target the wrong IOMMU.
	 */
	u = pmc_amdiommu_unit(amdiomdesc(ri)->pd_unit);
	if (u == NULL)
		return (NULL);
	return ((struct amdiommu_unit *)u->pau_arg);
}

/* ── Ops ─────────────────────────────────────────────────────────────────── */

static int
amdiommu_pcpu_init(struct pmc_mdep *md __unused, int cpu)
{
	struct pmc_cpu *pc;
	int n;

	if (cpu < 0 || cpu >= amdiommu_maxcpu)
		return (0);
	pc = pmc_pcpu[cpu];
	if (pc == NULL)
		return (0);
	/*
	 * Wire AMDIOMMU hw-PMC pointers so that pmc_do_op_pmcallocate()
	 * does not dereference a NULL pc->pc_hwpmcs[] entry for SC mode.
	 * AMDIOMMU counters are uncore — all CPUs share the same descriptors.
	 */
	for (n = 0; n < amdiommu_npmc; n++)
		pc->pc_hwpmcs[n + amdiommu_ri] = &amdiommu_pmcdesc[n]->pd_hw;
	return (0);
}

static int
amdiommu_pcpu_fini(struct pmc_mdep *md __unused, int cpu)
{
	struct pmc_cpu *pc;
	int n;

	if (cpu < 0 || cpu >= amdiommu_maxcpu)
		return (0);
	pc = pmc_pcpu[cpu];
	if (pc == NULL)
		return (0);
	for (n = 0; n < amdiommu_npmc; n++)
		pc->pc_hwpmcs[n + amdiommu_ri] = NULL;
	return (0);
}

static int
amdiommu_read_pmc(int cpu __unused, int ri, struct pmc *pm __unused,
    pmc_value_t *v)
{
	struct amdiommu_descr *desc;
	struct amdiommu_unit *unit;
	uint64_t val;
	int error;

	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));

	desc = amdiomdesc(ri);
	unit = amdiom_unit(ri);
	if (unit == NULL)
		return (ENXIO);

	error = pmc_amdiommu_read(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_COUNTER, &val);
	if (error != 0)
		return (error);

	/*
	 * Spec §3.4.22.2: diagram shows ICounter[47:0]; prose line 20359 reads
	 * "47:28" — a known contradiction.  Diagram and Linux both use 47:0.
	 */
	*v = val & PMC_AMDIOMMU_COUNTER_MASK;
	PMCDBG3(MDP, REA, 2, "%s ri=%d -> %ju", __func__, ri, (uintmax_t)val);
	return (0);
}

static int
amdiommu_write_pmc(int cpu __unused, int ri, struct pmc *pm __unused,
    pmc_value_t v)
{
	struct amdiommu_descr *desc;
	struct amdiommu_unit *unit;

	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));

	desc = amdiomdesc(ri);
	unit = amdiom_unit(ri);
	if (unit == NULL)
		return (ENXIO);

	PMCDBG3(MDP, WRI, 1, "%s ri=%d v=%ju", __func__, ri, (uintmax_t)v);
	return (pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_COUNTER,
	    v & PMC_AMDIOMMU_COUNTER_MASK));
}

static int
amdiommu_config_pmc(int cpu __unused, int ri, struct pmc *pm)
{
	struct pmc_hw *phw;

	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));

	phw = &amdiomdesc(ri)->pd_hw;
	KASSERT(pm == NULL || phw->phw_pmc == NULL,
	    ("[amdiommu,%d] ri=%d phw already owned", __LINE__, ri));

	phw->phw_pmc = pm;
	return (0);
}

static int
amdiommu_get_config(int cpu __unused, int ri, struct pmc **ppm)
{
	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));

	*ppm = amdiomdesc(ri)->pd_hw.phw_pmc;
	return (0);
}

static int
amdiommu_describe(int cpu __unused, int ri, struct pmc_info *pi,
    struct pmc **ppmc)
{
	struct amdiommu_descr *desc;

	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));

	desc = amdiomdesc(ri);
	strlcpy(pi->pm_name, desc->pd_descr.pd_name, sizeof(pi->pm_name));
	pi->pm_class = desc->pd_descr.pd_class;

	if (desc->pd_hw.phw_state & PMC_PHW_FLAG_IS_ENABLED) {
		pi->pm_enabled = TRUE;
		*ppmc = desc->pd_hw.phw_pmc;
	} else {
		pi->pm_enabled = FALSE;
		*ppmc = NULL;
	}
	return (0);
}

/*
 * Map a PMC_EV_AMDIOMMU_* event to the CSource value written to the counter
 * control register (Fxn=0x10, bits [7:0]).  Events are defined in
 * pmc_events.h in the same order as the spec's CSource table (0x00..0x17).
 */
static int
amdiommu_ev2csource(enum pmc_event ev, uint8_t *csource)
{
	if (ev < PMC_EV_AMDIOMMU_FIRST || ev > PMC_EV_AMDIOMMU_LAST)
		return (EINVAL);

	/*
	 * CSource is 1-based; mapping assumes __PMC_EV_AMDIOMMU() ordering
	 * matches AMD 48882 Table 82 without gaps.  Verify on each new event.
	 */
	_Static_assert(
	    (int)PMC_EV_AMDIOMMU_LAST - (int)PMC_EV_AMDIOMMU_FIRST <= 254,
	    "AMDIOMMU event range exceeds uint8_t CSource capacity");
	*csource = (uint8_t)(ev - PMC_EV_AMDIOMMU_FIRST) + 1;
	return (0);
}

static int
amdiommu_allocate_pmc(int cpu __unused, int ri, struct pmc *pm,
    const struct pmc_op_pmcallocate *a)
{
	const struct pmc_descr *pd;
	uint8_t csource;

	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));

	pd = &amdiomdesc(ri)->pd_descr;

	if (pd->pd_class != a->pm_class)
		return (EINVAL);

	if ((a->pm_caps & ~AMDIOMMU_CAPS) != 0)
		return (EINVAL);

	if (amdiommu_ev2csource(a->pm_ev, &csource) != 0)
		return (EINVAL);

	pm->pm_md.pm_amdiommu.pm_amdiommu_csource = csource;
	return (0);
}

static int
amdiommu_release_pmc(int cpu __unused, int ri, struct pmc *pm __unused)
{
	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));
	KASSERT(amdiomdesc(ri)->pd_hw.phw_pmc == NULL,
	    ("[amdiommu,%d] ri=%d phw not cleared before release", __LINE__,
	    ri));
	return (0);
}

static int
amdiommu_start_pmc(int cpu __unused, int ri, struct pmc *pm)
{
	struct amdiommu_descr *desc;
	struct amdiommu_unit *unit;
	uint64_t ctrl;
	int error;

	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));

	desc = amdiomdesc(ri);
	unit = amdiom_unit(ri);
	if (unit == NULL)
		return (ENXIO);

	/*
	 * Verify the bank is not locked before programming any filter register.
	 * A locked bank silently ignores writes to PASID/Domain/DevID match
	 * registers (spec MMIO 0x4008/0x4010/0x4018, amd_reg.h).  The counter
	 * would then run with the stale filter values from a previous allocation,
	 * producing silent miscounts.  Return EBUSY so the caller knows the
	 * hardware state is unexpected.
	 */
	if (pmc_amdiommu_bank_is_locked(unit, desc->pd_bank)) {
		PMCDBG1(MDP, STA, 0, "amdiommu ri=%d: bank locked", ri);
		return (EBUSY);
	}

	/*
	 * Program optional filter registers before starting the counter.
	 * Filters narrow counting to a specific PASID, DMA domain, or device.
	 * Writing 0 to a filter register disables that filter (count all).
	 * Order: clear filters first, then set enabled ones, then start counter.
	 */
	/* Stale filter state is preferable to ENXIO on start. */
	(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_PASID, 0);
	(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_DOMAIN, 0);
	(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_DEVID, 0);

	if (pm->pm_md.pm_amdiommu.pm_amdiommu_filter & AMDIOMMU_FILTER_PASID)
		(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
		    PMC_AMDIOMMU_FXN_PASID,
		    pm->pm_md.pm_amdiommu.pm_amdiommu_pasid);
	if (pm->pm_md.pm_amdiommu.pm_amdiommu_filter & AMDIOMMU_FILTER_DOMAIN)
		(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
		    PMC_AMDIOMMU_FXN_DOMAIN,
		    pm->pm_md.pm_amdiommu.pm_amdiommu_domain);
	if (pm->pm_md.pm_amdiommu.pm_amdiommu_filter & AMDIOMMU_FILTER_DEVID)
		(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
		    PMC_AMDIOMMU_FXN_DEVID,
		    pm->pm_md.pm_amdiommu.pm_amdiommu_devid);

	/*
	 * Counter Source Register (Fxn=PMC_AMDIOMMU_FXN_SRC = 0x08):
	 *   bits [7:0]  CSource — 1-based event ordinal (Table 82); 0 = stop
	 *
	 * Writing a non-zero CSource starts the counter immediately.
	 * No separate CntEn bit exists in this register.
	 */
	ctrl = (uint64_t)pm->pm_md.pm_amdiommu.pm_amdiommu_csource;
	error = pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_SRC, ctrl);

	PMCDBG3(MDP, STA, 1, "%s ri=%d csource=0x%02x", __func__, ri,
	    pm->pm_md.pm_amdiommu.pm_amdiommu_csource);
	return (error);
}

static int
amdiommu_stop_pmc(int cpu __unused, int ri, struct pmc *pm __unused)
{
	struct amdiommu_descr *desc;
	struct amdiommu_unit *unit;

	KASSERT(ri >= 0 && ri < amdiommu_npmc,
	    ("[amdiommu,%d] ri %d out of range", __LINE__, ri));

	desc = amdiomdesc(ri);
	unit = amdiom_unit(ri);
	if (unit == NULL)
		return (ENXIO);

	/*
	 * Power-gating safe stop (spec §3.4.22 / Linux perf_iommu_stop):
	 * The MI layer reads the counter before calling stop_pmc for
	 * system-wide PMCs, so the final value is already captured.
	 * Writing CSource=0 after the MI read avoids the race where
	 * power-gating would block a subsequent read.
	 */
	/* Discard filter writes — stale state is preferable to ENXIO. */
	(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_PASID, 0);
	(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_DOMAIN, 0);
	(void)pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_DEVID, 0);
	return (pmc_amdiommu_write(unit, desc->pd_bank, desc->pd_cntr,
	    PMC_AMDIOMMU_FXN_SRC, 0));
}

/* ── Initialize / Finalize ───────────────────────────────────────────────── */

int
pmc_amdiommu_initialize(struct pmc_mdep *md, int nclass_idx)
{
	const struct pmc_amdiommu_unit *u;
	struct pmc_classdep *pcd;
	struct amdiommu_descr *desc;
	int nunits, ri, unit_idx;
	uint8_t nbanks, ncounters;

	KASSERT(md != NULL, ("[amdiommu,%d] md is NULL", __LINE__));

	nunits = pmc_amdiommu_nunits();
	if (nunits == 0)
		return (ENOENT);

	/* Count total rows across all units. */
	amdiommu_npmc = 0;
	amdiommu_class_idx = -1;
	for (unit_idx = 0; unit_idx < nunits; unit_idx++) {
		u = pmc_amdiommu_unit(unit_idx);
		if (u == NULL || u->pau_arg == NULL)
			continue;
		amdiommu_npmc += (int)u->pau_nbanks * u->pau_ncounters;
	}
	if (amdiommu_npmc == 0)
		return (ENOENT);

	amdiommu_pmcdesc = malloc(sizeof(*amdiommu_pmcdesc) * amdiommu_npmc,
	    M_PMC, M_WAITOK | M_ZERO);

	ri = 0;
	for (unit_idx = 0; unit_idx < nunits; unit_idx++) {
		u = pmc_amdiommu_unit(unit_idx);
		if (u == NULL || u->pau_arg == NULL)
			continue;

		pmc_amdiommu_get_topology(
		    (const struct amdiommu_unit *)u->pau_arg,
		    &nbanks, &ncounters);
		KASSERT(nbanks <= 63 && ncounters <= 15,
		    ("[amdiommu,%d] unit %d: nbanks=%u ncounters=%u exceeds spec",
		    __LINE__, unit_idx, nbanks, ncounters));

		for (uint8_t bank = 0; bank < nbanks; bank++) {
			for (uint8_t cntr = 0; cntr < ncounters; cntr++) {
				desc = malloc(sizeof(*desc), M_PMC,
				    M_WAITOK | M_ZERO);
				desc->pd_unit  = unit_idx;
				desc->pd_bank  = bank;
				desc->pd_cntr  = cntr;
				desc->pd_descr.pd_class = PMC_CLASS_AMDIOMMU;
				desc->pd_descr.pd_caps  = AMDIOMMU_CAPS;
				desc->pd_descr.pd_width = 48;
				/*
				 * pd_numa is the IOMMU unit index, which maps 1:1 to
				 * PCI segment/socket in EPYC systems.  A full
				 * bus_get_domain() would require including amd_iommu.h
				 * and breaks the separation between hwpmc and the IOMMU
				 * driver.  Expose unit_idx as the per-socket selector.
				 */
				desc->pd_numa = (int)unit_idx;
				snprintf(desc->pd_descr.pd_name,
				    sizeof(desc->pd_descr.pd_name),
				    "AMDIOMMU%d-%d-%d",
				    unit_idx, bank, cntr);
				desc->pd_hw.phw_state =
				    PMC_PHW_FLAG_IS_ENABLED;
				desc->pd_hw.phw_pmc = NULL;
				amdiommu_pmcdesc[ri++] = desc;
			}
		}
	}

	pcd = &md->pmd_classdep[nclass_idx];
	pcd->pcd_caps		= AMDIOMMU_CAPS;
	pcd->pcd_class		= PMC_CLASS_AMDIOMMU;
	pcd->pcd_num		= amdiommu_npmc;
	pcd->pcd_ri		= md->pmd_npmc;
	pcd->pcd_width		= 48;

	pcd->pcd_allocate_pmc	= amdiommu_allocate_pmc;
	pcd->pcd_config_pmc	= amdiommu_config_pmc;
	pcd->pcd_describe	= amdiommu_describe;
	pcd->pcd_get_config	= amdiommu_get_config;
	pcd->pcd_get_msr	= NULL;
	pcd->pcd_pcpu_init	= amdiommu_pcpu_init;
	pcd->pcd_pcpu_fini	= amdiommu_pcpu_fini;
	amdiommu_ri = md->pmd_npmc;
	amdiommu_maxcpu = pmc_cpu_max();
	amdiommu_class_idx = nclass_idx;
	pcd->pcd_read_pmc	= amdiommu_read_pmc;
	pcd->pcd_release_pmc	= amdiommu_release_pmc;
	pcd->pcd_start_pmc	= amdiommu_start_pmc;
	pcd->pcd_stop_pmc	= amdiommu_stop_pmc;
	pcd->pcd_write_pmc	= amdiommu_write_pmc;

	md->pmd_npmc += amdiommu_npmc;

	PMCDBG2(MDP, INI, 1, "amdiommu-initialize units=%d npmc=%d",
	    nunits, amdiommu_npmc);
	return (0);
}

void
pmc_amdiommu_finalize(struct pmc_mdep *md)
{
	struct pmc_classdep *pcd;
	int i;

	/* NULL ops before free to catch any in-flight MI calls. */
	if (md != NULL && amdiommu_class_idx >= 0) {
		pcd = &md->pmd_classdep[amdiommu_class_idx];
		pcd->pcd_allocate_pmc = NULL;
		pcd->pcd_read_pmc     = NULL;
		pcd->pcd_write_pmc    = NULL;
		pcd->pcd_start_pmc    = NULL;
		pcd->pcd_stop_pmc     = NULL;
		pcd->pcd_release_pmc  = NULL;
	}
	for (i = 0; i < amdiommu_npmc; i++)
		free(amdiommu_pmcdesc[i], M_PMC);
	free(amdiommu_pmcdesc, M_PMC);
	amdiommu_pmcdesc = NULL;
	amdiommu_npmc = 0;
	amdiommu_class_idx = -1;
	PMCDBG0(MDP, INI, 1, "amdiommu-finalize");
}
