/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, Ali Jose Mashtizadeh
 * All rights reserved.
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
 */

/* Support for the AMD IBS */

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/pmc.h>
#include <sys/pmckern.h>
#include <sys/pmclog.h>
#include <sys/smp.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/specialreg.h>

#include "hwpmc_ibs.h"

#define	IBS_STOP_ITER		50 /* Stopping iterations */

/* AMD IBS PMCs */
struct ibs_descr {
	struct pmc_descr pm_descr;  /* "base class" */
};

/*
 * Globals
 */
static uint64_t ibs_features;

/*
 * IBS sysctl context and state.
 * This structure holds configuration values accessible via sysctl.
 */
static struct ibs_sysctl {
	struct sysctl_ctx_list	isc_ctx;
	struct sysctl_oid	*isc_tree;

	/* Capability flags (read-only, derived from ibs_features) */
	int			isc_fetch_cap;
	int			isc_op_cap;
	int			isc_zen4_extensions;
	int			isc_l3miss_filter;
	int			isc_load_latency_filter;

	/* Configuration (read/write) */
	int			isc_fetch_enable;
	int			isc_op_enable;
	uint64_t		isc_fetch_period;
	uint64_t		isc_op_period;
} ibs_sysctl_ctx;

/*
 * Per-processor information
 */
#define	IBS_CPU_RUNNING		1
#define	IBS_CPU_STOPPING	2
#define	IBS_CPU_STOPPED		3

struct ibs_cpu {
	int		pc_status;
	struct pmc_hw	pc_ibspmcs[IBS_NPMCS];
};
static struct ibs_cpu **ibs_pcpu;

/*
 * Read a PMC value from the MSR.
 */
static int
ibs_read_pmc(int cpu, int ri, struct pmc *pm, pmc_value_t *v)
{

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] illegal CPU value %d", __LINE__, cpu));
	KASSERT(ri >= 0 && ri < IBS_NPMCS,
	    ("[ibs,%d] illegal row-index %d", __LINE__, ri));
	KASSERT(ibs_pcpu[cpu],
	    ("[ibs,%d] null per-cpu, cpu %d", __LINE__, cpu));

	/* read the IBS ctl */
	switch (ri) {
	case IBS_PMC_FETCH:
		*v = rdmsr(IBS_FETCH_CTL);
		break;
	case IBS_PMC_OP:
		*v = rdmsr(IBS_OP_CTL);
		break;
	}

	PMCDBG2(MDP, REA, 2, "ibs-read id=%d -> %jd", ri, *v);

	return (0);
}

/*
 * Write a PMC MSR.
 */
static int
ibs_write_pmc(int cpu, int ri, struct pmc *pm, pmc_value_t v)
{

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] illegal CPU value %d", __LINE__, cpu));
	KASSERT(ri >= 0 && ri < IBS_NPMCS,
	    ("[ibs,%d] illegal row-index %d", __LINE__, ri));

	PMCDBG3(MDP, WRI, 1, "ibs-write cpu=%d ri=%d v=%jx", cpu, ri, v);

	/* write the IBS ctl */
	switch (ri) {
	case IBS_PMC_FETCH:
		wrmsr(IBS_FETCH_CTL, v);
		break;
	case IBS_PMC_OP:
		wrmsr(IBS_OP_CTL, v);
		break;
	}

	return (0);
}

/*
 * Configure hardware PMC according to the configuration recorded in 'pm'.
 */
static int
ibs_config_pmc(int cpu, int ri, struct pmc *pm)
{
	struct pmc_hw *phw;

	PMCDBG3(MDP, CFG, 1, "cpu=%d ri=%d pm=%p", cpu, ri, pm);

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] illegal CPU value %d", __LINE__, cpu));
	KASSERT(ri >= 0 && ri < IBS_NPMCS,
	    ("[ibs,%d] illegal row-index %d", __LINE__, ri));

	phw = &ibs_pcpu[cpu]->pc_ibspmcs[ri];

	KASSERT(pm == NULL || phw->phw_pmc == NULL,
	    ("[ibs,%d] pm=%p phw->pm=%p hwpmc not unconfigured",
		__LINE__, pm, phw->phw_pmc));

	phw->phw_pmc = pm;

	return (0);
}

/*
 * Retrieve a configured PMC pointer from hardware state.
 */
static int
ibs_get_config(int cpu, int ri, struct pmc **ppm)
{

	*ppm = ibs_pcpu[cpu]->pc_ibspmcs[ri].phw_pmc;

	return (0);
}

/*
 * Check if a given PMC allocation is feasible.
 */
static int
ibs_allocate_pmc(int cpu __unused, int ri, struct pmc *pm,
    const struct pmc_op_pmcallocate *a)
{
	uint64_t caps, config, config2;

	KASSERT(ri >= 0 && ri < IBS_NPMCS,
	    ("[ibs,%d] illegal row index %d", __LINE__, ri));

	/* check class match */
	if (a->pm_class != PMC_CLASS_IBS)
		return (EINVAL);
	if (a->pm_md.pm_ibs.ibs_type != ri)
		return (EINVAL);

	caps = pm->pm_caps;

	PMCDBG2(MDP, ALL, 1, "ibs-allocate ri=%d caps=0x%x", ri, caps);

	if ((caps & PMC_CAP_SYSTEM) == 0)
		return (EINVAL);

	config = a->pm_md.pm_ibs.ibs_ctl;
	config2 = a->pm_md.pm_ibs.ibs_ctl2;

	/*
	 * Validate Zen 4+ extended features.
	 * L3MissOnly filter is available on Zen 4+ for both fetch and op.
	 * ITLB refill latency configuration requires IBSFETCHCTLEXTD.
	 */
	if ((config2 & IBS_FETCH_CTL_L3MISSONLY) != 0 &&
	    (ibs_features & CPUID_IBSID_ZEN4IBSEXTENSIONS) == 0) {
		PMCDBG0(MDP, ALL, 2,
		    "ibs-allocate L3MissOnly not supported");
		return (EINVAL);
	}

	pm->pm_md.pm_ibs.ibs_ctl = config;
	pm->pm_md.pm_ibs.ibs_ctl2 = config2;

	PMCDBG3(MDP, ALL, 2, "ibs-allocate ri=%d -> config=0x%x config2=0x%x",
	    ri, config, config2);

	return (0);
}

/*
 * Release machine dependent state associated with a PMC.  This is a
 * no-op on this architecture.
 */
static int
ibs_release_pmc(int cpu, int ri, struct pmc *pmc __unused)
{
	struct pmc_hw *phw __diagused;

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] illegal CPU value %d", __LINE__, cpu));
	KASSERT(ri >= 0 && ri < IBS_NPMCS,
	    ("[ibs,%d] illegal row-index %d", __LINE__, ri));

	PMCDBG1(MDP, ALL, 1, "ibs-release ri=%d", ri);

	phw = &ibs_pcpu[cpu]->pc_ibspmcs[ri];

	KASSERT(phw->phw_pmc == NULL,
	    ("[ibs,%d] PHW pmc %p non-NULL", __LINE__, phw->phw_pmc));

	return (0);
}

/*
 * Start a PMC.
 */
static int
ibs_start_pmc(int cpu __diagused, int ri, struct pmc *pm)
{
	uint64_t config;

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] illegal CPU value %d", __LINE__, cpu));
	KASSERT(ri >= 0 && ri < IBS_NPMCS,
	    ("[ibs,%d] illegal row-index %d", __LINE__, ri));

	PMCDBG2(MDP, STA, 1, "ibs-start cpu=%d ri=%d", cpu, ri);

	/*
	 * This is used to handle spurious NMIs.  All that matters is that it
	 * is not in the stopping state.
	 */
	atomic_store_int(&ibs_pcpu[cpu]->pc_status, IBS_CPU_RUNNING);

	/*
	 * Turn on the ENABLE bit.  Zeroing out the control register eliminates
	 * stale valid bits from spurious NMIs and it resets the counter.
	 * For Zen 4+, also configure extended MSRs if available.
	 */
	switch (ri) {
	case IBS_PMC_FETCH:
		wrmsr(IBS_FETCH_CTL, 0);
		config = pm->pm_md.pm_ibs.ibs_ctl | IBS_FETCH_CTL_ENABLE;
		wrmsr(IBS_FETCH_CTL, config);

		/* Configure Zen 4+ extended fetch control */
		if ((ibs_features & CPUID_IBSID_IBSFETCHCTLEXTD) != 0 &&
		    pm->pm_md.pm_ibs.ibs_ctl2 != 0) {
			wrmsr(IBS_FETCH_EXTCTL, pm->pm_md.pm_ibs.ibs_ctl2);
		}
		break;
	case IBS_PMC_OP:
		wrmsr(IBS_OP_CTL, 0);
		config = pm->pm_md.pm_ibs.ibs_ctl | IBS_OP_CTL_ENABLE;

		/* Add L3MissOnly filter for Zen 4+ */
		if ((ibs_features & CPUID_IBSID_ZEN4IBSEXTENSIONS) != 0 &&
		    (pm->pm_md.pm_ibs.ibs_ctl2 & IBS_OP_CTL_L3MISSONLY) != 0) {
			config |= IBS_OP_CTL_L3MISSONLY;
		}
		wrmsr(IBS_OP_CTL, config);
		break;
	}

	return (0);
}

/*
 * Stop a PMC.
 */
static int
ibs_stop_pmc(int cpu __diagused, int ri, struct pmc *pm)
{
	int i;
	uint64_t config;

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] illegal CPU value %d", __LINE__, cpu));
	KASSERT(ri >= 0 && ri < IBS_NPMCS,
	    ("[ibs,%d] illegal row-index %d", __LINE__, ri));

	PMCDBG1(MDP, STO, 1, "ibs-stop ri=%d", ri);

	/*
	 * Turn off the ENABLE bit, but unfortunately there are a few quirks
	 * that generate excess NMIs.  Workaround #420 in the Revision Guide
	 * for AMD Family 10h Processors 41322 Rev. 3.92 March 2012. requires
	 * that we clear the count before clearing enable.
	 *
	 * Even after clearing the counter spurious NMIs are still possible so
	 * we use a per-CPU atomic variable to notify the interrupt handler we
	 * are stopping and discard spurious NMIs.  We then retry clearing the
	 * control register for 50us.  This gives us enough time and ensures
	 * that the valid bit is not accidently stuck after a spurious NMI.
	 */
	config = pm->pm_md.pm_ibs.ibs_ctl;

	atomic_store_int(&ibs_pcpu[cpu]->pc_status, IBS_CPU_STOPPING);

	switch (ri) {
	case IBS_PMC_FETCH:
		wrmsr(IBS_FETCH_CTL, config & ~IBS_FETCH_CTL_MAXCNTMASK);
		DELAY(1);
		config &= ~IBS_FETCH_CTL_ENABLE;
		wrmsr(IBS_FETCH_CTL, config);
		break;
	case IBS_PMC_OP:
		wrmsr(IBS_FETCH_CTL, config & ~IBS_FETCH_CTL_MAXCNTMASK);
		DELAY(1);
		config &= ~IBS_OP_CTL_ENABLE;
		wrmsr(IBS_OP_CTL, config);
		break;
	}

	for (i = 0; i < IBS_STOP_ITER; i++) {
		DELAY(1);

		switch (ri) {
		case IBS_PMC_FETCH:
			wrmsr(IBS_FETCH_CTL, 0);
			break;
		case IBS_PMC_OP:
			wrmsr(IBS_OP_CTL, 0);
			break;
		}
	}

	atomic_store_int(&ibs_pcpu[cpu]->pc_status, IBS_CPU_STOPPED);

	return (0);
}

static void
pmc_ibs_process_fetch(struct pmc *pm, struct trapframe *tf, uint64_t config)
{
	struct pmc_multipart mpd;

	if (pm == NULL)
		return;

	if (pm->pm_state != PMC_STATE_RUNNING)
		return;

	memset(&mpd, 0, sizeof(mpd));

	mpd.pl_type = PMC_CC_MULTIPART_IBS_FETCH;
	mpd.pl_length = 4;
	mpd.pl_mpdata[PMC_MPIDX_FETCH_CTL] = config;
	if ((ibs_features & CPUID_IBSID_IBSFETCHCTLEXTD) != 0) {
		mpd.pl_mpdata[PMC_MPIDX_FETCH_EXTCTL] =
		    rdmsr(IBS_FETCH_EXTCTL);
		mpd.pl_length = 5;
	}
	mpd.pl_mpdata[PMC_MPIDX_FETCH_LINADDR] = rdmsr(IBS_FETCH_LINADDR);
	if ((config & IBS_FETCH_CTL_PHYSADDRVALID) != 0) {
		mpd.pl_mpdata[PMC_MPIDX_FETCH_PHYSADDR] =
		    rdmsr(IBS_FETCH_PHYSADDR);
	}

	pmc_process_interrupt_mp(PMC_HR, pm, tf, &mpd);
}

static void
pmc_ibs_process_op(struct pmc *pm, struct trapframe *tf, uint64_t config)
{
	struct pmc_multipart mpd;

	if (pm == NULL)
		return;

	if (pm->pm_state != PMC_STATE_RUNNING)
		return;

	memset(&mpd, 0, sizeof(mpd));

	mpd.pl_type = PMC_CC_MULTIPART_IBS_OP;
	mpd.pl_length = 7;
	mpd.pl_mpdata[PMC_MPIDX_OP_CTL] = config;
	mpd.pl_mpdata[PMC_MPIDX_OP_RIP] = rdmsr(IBS_OP_RIP);
	mpd.pl_mpdata[PMC_MPIDX_OP_DATA] = rdmsr(IBS_OP_DATA);
	mpd.pl_mpdata[PMC_MPIDX_OP_DATA2] = rdmsr(IBS_OP_DATA2);
	mpd.pl_mpdata[PMC_MPIDX_OP_DATA3] = rdmsr(IBS_OP_DATA3);
	mpd.pl_mpdata[PMC_MPIDX_OP_DC_LINADDR] = rdmsr(IBS_OP_DC_LINADDR);
	mpd.pl_mpdata[PMC_MPIDX_OP_DC_PHYSADDR] = rdmsr(IBS_OP_DC_PHYSADDR);

	/* Zen 4+ extended MSR: IBS Op Data 4 */
	if ((ibs_features & CPUID_IBSID_IBSOPDATA4) != 0) {
		mpd.pl_mpdata[PMC_MPIDX_OP_DATA4] = rdmsr(IBS_OP_DATA4);
		mpd.pl_length = 8;
	}

	pmc_process_interrupt_mp(PMC_HR, pm, tf, &mpd);

	wrmsr(IBS_OP_CTL, pm->pm_md.pm_ibs.ibs_ctl | IBS_OP_CTL_ENABLE);
}

/*
 * Interrupt handler.  This function needs to return '1' if the
 * interrupt was this CPU's PMCs or '0' otherwise.  It is not allowed
 * to sleep or do anything a 'fast' interrupt handler is not allowed
 * to do.
 */
int
pmc_ibs_intr(struct trapframe *tf)
{
	struct ibs_cpu *pac;
	struct pmc *pm;
	int retval, cpu;
	uint64_t config;

	cpu = curcpu;
	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] out of range CPU %d", __LINE__, cpu));

	PMCDBG3(MDP, INT, 1, "cpu=%d tf=%p um=%d", cpu, tf, TRAPF_USERMODE(tf));

	retval = 0;

	pac = ibs_pcpu[cpu];

	config = rdmsr(IBS_FETCH_CTL);
	if ((config & IBS_FETCH_CTL_VALID) != 0) {
		pm = pac->pc_ibspmcs[IBS_PMC_FETCH].phw_pmc;

		retval = 1;

		pmc_ibs_process_fetch(pm, tf, config);
	}

	config = rdmsr(IBS_OP_CTL);
	if ((retval == 0) && ((config & IBS_OP_CTL_VALID) != 0)) {
		pm = pac->pc_ibspmcs[IBS_PMC_OP].phw_pmc;

		retval = 1;

		pmc_ibs_process_op(pm, tf, config);
	}

	if (retval == 0) {
		// Lets check for a stray NMI when stopping
		if (atomic_load_int(&pac->pc_status) == IBS_CPU_STOPPING) {
			return (1);
		}
	}


	if (retval)
		counter_u64_add(pmc_stats.pm_intr_processed, 1);
	else
		counter_u64_add(pmc_stats.pm_intr_ignored, 1);

	PMCDBG1(MDP, INT, 2, "retval=%d", retval);

	return (retval);
}

/*
 * Describe a PMC.
 */
static int
ibs_describe(int cpu, int ri, struct pmc_info *pi, struct pmc **ppmc)
{
	struct pmc_hw *phw;

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] illegal CPU %d", __LINE__, cpu));
	KASSERT(ri >= 0 && ri < IBS_NPMCS,
	    ("[ibs,%d] row-index %d out of range", __LINE__, ri));

	phw = &ibs_pcpu[cpu]->pc_ibspmcs[ri];

	if (ri == IBS_PMC_FETCH) {
		strlcpy(pi->pm_name, "IBS-FETCH", sizeof(pi->pm_name));
		pi->pm_class = PMC_CLASS_IBS;
		pi->pm_enabled = true;
		*ppmc          = phw->phw_pmc;
	} else {
		strlcpy(pi->pm_name, "IBS-OP", sizeof(pi->pm_name));
		pi->pm_class = PMC_CLASS_IBS;
		pi->pm_enabled = true;
		*ppmc          = phw->phw_pmc;
	}

	return (0);
}

/*
 * Processor-dependent initialization.
 */
static int
ibs_pcpu_init(struct pmc_mdep *md, int cpu)
{
	struct ibs_cpu *pac;
	struct pmc_cpu *pc;
	struct pmc_hw  *phw;
	int first_ri, n;

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] insane cpu number %d", __LINE__, cpu));

	PMCDBG1(MDP, INI, 1, "ibs-init cpu=%d", cpu);

	ibs_pcpu[cpu] = pac = malloc(sizeof(struct ibs_cpu), M_PMC,
	    M_WAITOK | M_ZERO);

	/*
	 * Set the content of the hardware descriptors to a known
	 * state and initialize pointers in the MI per-cpu descriptor.
	 */
	pc = pmc_pcpu[cpu];
	first_ri = md->pmd_classdep[PMC_MDEP_CLASS_INDEX_IBS].pcd_ri;

	KASSERT(pc != NULL, ("[ibs,%d] NULL per-cpu pointer", __LINE__));

	for (n = 0, phw = pac->pc_ibspmcs; n < IBS_NPMCS; n++, phw++) {
		phw->phw_state = PMC_PHW_FLAG_IS_ENABLED |
		    PMC_PHW_CPU_TO_STATE(cpu) | PMC_PHW_INDEX_TO_STATE(n);
		phw->phw_pmc = NULL;
		pc->pc_hwpmcs[n + first_ri] = phw;
	}

	return (0);
}

/*
 * Processor-dependent cleanup prior to the KLD being unloaded.
 */
static int
ibs_pcpu_fini(struct pmc_mdep *md, int cpu)
{
	struct ibs_cpu *pac;
	struct pmc_cpu *pc;
	int first_ri, i;

	KASSERT(cpu >= 0 && cpu < pmc_cpu_max(),
	    ("[ibs,%d] insane cpu number (%d)", __LINE__, cpu));

	PMCDBG1(MDP, INI, 1, "ibs-cleanup cpu=%d", cpu);

	/*
	 * Turn off IBS.
	 */
	wrmsr(IBS_FETCH_CTL, 0);
	wrmsr(IBS_OP_CTL, 0);

	/*
	 * Free up allocated space.
	 */
	if ((pac = ibs_pcpu[cpu]) == NULL)
		return (0);

	ibs_pcpu[cpu] = NULL;

	pc = pmc_pcpu[cpu];
	KASSERT(pc != NULL, ("[ibs,%d] NULL per-cpu state", __LINE__));

	first_ri = md->pmd_classdep[PMC_MDEP_CLASS_INDEX_IBS].pcd_ri;

	/*
	 * Reset pointers in the MI 'per-cpu' state.
	 */
	for (i = 0; i < IBS_NPMCS; i++)
		pc->pc_hwpmcs[i + first_ri] = NULL;

	free(pac, M_PMC);

	return (0);
}

/*
 * Sysctl handlers for IBS configuration.
 */

/* Handler for fetch_enable and op_enable sysctls */
static int
sysctl_ibs_enable(SYSCTL_HANDLER_ARGS)
{
	int error, val;

	val = *(int *)arg1;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val != 0 && val != 1)
		return (EINVAL);
	*(int *)arg1 = val;
	return (0);
}

/* Handler for fetch_period and op_period sysctls */
static int
sysctl_ibs_period(SYSCTL_HANDLER_ARGS)
{
	int error;
	uint64_t val;

	val = *(uint64_t *)arg1;
	error = sysctl_handle_64(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	/*
	 * IBS period must be non-zero. The hardware interprets the period
	 * as a maximum count; zero would disable sampling.
	 */
	if (val == 0)
		return (EINVAL);
	*(uint64_t *)arg1 = val;
	return (0);
}

/*
 * Initialize the IBS sysctl tree.
 * This is called after ibs_features has been populated.
 */
static void
ibs_sysctl_init(void)
{
	struct ibs_sysctl *isc;

	isc = &ibs_sysctl_ctx;

	/* Initialize capability flags from detected features */
	isc->isc_fetch_cap =
	    (ibs_features & CPUID_IBSID_FETCHSAM) != 0 ? 1 : 0;
	isc->isc_op_cap =
	    (ibs_features & CPUID_IBSID_OPSAM) != 0 ? 1 : 0;
	isc->isc_zen4_extensions =
	    (ibs_features & CPUID_IBSID_ZEN4IBSEXTENSIONS) != 0 ? 1 : 0;
	isc->isc_l3miss_filter =
	    (ibs_features & CPUID_IBSID_ZEN4IBSEXTENSIONS) != 0 ? 1 : 0;
	isc->isc_load_latency_filter =
	    (ibs_features & CPUID_IBSID_IBSLOADLATENCYFILT) != 0 ? 1 : 0;

	/* Default configuration values */
	isc->isc_fetch_enable = 0;
	isc->isc_op_enable = 0;
	isc->isc_fetch_period = 0;
	isc->isc_op_period = 0;

	/* Create sysctl context */
	sysctl_ctx_init(&isc->isc_ctx);

	/* Create the sysctl tree under hw.amd_ibs */
	isc->isc_tree = SYSCTL_ADD_NODE(&isc->isc_ctx,
	    SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO, "amd_ibs",
	    CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "AMD IBS configuration");

	if (isc->isc_tree == NULL) {
		printf("hwpmc: failed to create amd_ibs sysctl node\n");
		return;
	}

	/* Read-only capability nodes */
	SYSCTL_ADD_INT(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "fetch_cap",
	    CTLFLAG_RD, &isc->isc_fetch_cap, 0,
	    "IBS Fetch sampling capability");
	SYSCTL_ADD_INT(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "op_cap",
	    CTLFLAG_RD, &isc->isc_op_cap, 0,
	    "IBS Op sampling capability");
	SYSCTL_ADD_INT(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "zen4_extensions",
	    CTLFLAG_RD, &isc->isc_zen4_extensions, 0,
	    "IBS Zen 4+ extensions support");
	SYSCTL_ADD_INT(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "l3miss_filter",
	    CTLFLAG_RD, &isc->isc_l3miss_filter, 0,
	    "IBS L3MissOnly filter support");
	SYSCTL_ADD_INT(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "load_latency_filter",
	    CTLFLAG_RD, &isc->isc_load_latency_filter, 0,
	    "IBS Load Latency Filtering support");

	/* Read/write configuration nodes */
	SYSCTL_ADD_PROC(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "fetch_enable",
	    CTLTYPE_INT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
	    &isc->isc_fetch_enable, 0, sysctl_ibs_enable, "I",
	    "Enable/disable IBS Fetch sampling");
	SYSCTL_ADD_PROC(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "op_enable",
	    CTLTYPE_INT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
	    &isc->isc_op_enable, 0, sysctl_ibs_enable, "I",
	    "Enable/disable IBS Op sampling");
	SYSCTL_ADD_PROC(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "fetch_period",
	    CTLTYPE_U64 | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
	    &isc->isc_fetch_period, 0, sysctl_ibs_period, "QU",
	    "IBS Fetch sampling period");
	SYSCTL_ADD_PROC(&isc->isc_ctx,
	    SYSCTL_CHILDREN(isc->isc_tree), OID_AUTO, "op_period",
	    CTLTYPE_U64 | CTLFLAG_RWTUN | CTLFLAG_MPSAFE,
	    &isc->isc_op_period, 0, sysctl_ibs_period, "QU",
	    "IBS Op sampling period");
}

/*
 * Destroy the IBS sysctl tree.
 */
static void
ibs_sysctl_fini(void)
{
	struct ibs_sysctl *isc;

	isc = &ibs_sysctl_ctx;
	if (isc->isc_tree != NULL) {
		sysctl_ctx_free(&isc->isc_ctx);
		isc->isc_tree = NULL;
	}
}

/*
 * Initialize ourselves.
 */
int
pmc_ibs_initialize(struct pmc_mdep *pmc_mdep, int ncpus)
{
	u_int regs[4];
	struct pmc_classdep *pcd;

	/*
	 * Allocate space for pointers to PMC HW descriptors and for
	 * the MDEP structure used by MI code.
	 */
	ibs_pcpu = malloc(sizeof(struct ibs_cpu *) * pmc_cpu_max(), M_PMC,
	    M_WAITOK | M_ZERO);

	/* Initialize AMD IBS handling. */
	pcd = &pmc_mdep->pmd_classdep[PMC_MDEP_CLASS_INDEX_IBS];

	pcd->pcd_caps		= IBS_PMC_CAPS;
	pcd->pcd_class		= PMC_CLASS_IBS;
	pcd->pcd_num		= IBS_NPMCS;
	pcd->pcd_ri		= pmc_mdep->pmd_npmc;
	pcd->pcd_width		= 0;

	pcd->pcd_allocate_pmc	= ibs_allocate_pmc;
	pcd->pcd_config_pmc	= ibs_config_pmc;
	pcd->pcd_describe	= ibs_describe;
	pcd->pcd_get_config	= ibs_get_config;
	pcd->pcd_pcpu_fini	= ibs_pcpu_fini;
	pcd->pcd_pcpu_init	= ibs_pcpu_init;
	pcd->pcd_release_pmc	= ibs_release_pmc;
	pcd->pcd_start_pmc	= ibs_start_pmc;
	pcd->pcd_stop_pmc	= ibs_stop_pmc;
	pcd->pcd_read_pmc	= ibs_read_pmc;
	pcd->pcd_write_pmc	= ibs_write_pmc;

	pmc_mdep->pmd_npmc	+= IBS_NPMCS;

	if (cpu_exthigh >= CPUID_IBSID) {
		do_cpuid(CPUID_IBSID, regs);
		ibs_features = regs[0];
	} else {
		ibs_features = 0;
	}

	/* Log detected IBS features */
	if ((ibs_features & CPUID_IBSID_IBSFETCHCTLEXTD) != 0)
		printf("hwpmc: IBS Fetch Control Extended supported\n");
	if ((ibs_features & CPUID_IBSID_IBSOPDATA4) != 0)
		printf("hwpmc: IBS Op Data 4 supported\n");
	if ((ibs_features & CPUID_IBSID_ZEN4IBSEXTENSIONS) != 0)
		printf("hwpmc: IBS Zen 4 Extensions supported\n");
	if ((ibs_features & CPUID_IBSID_IBSLOADLATENCYFILT) != 0)
		printf("hwpmc: IBS Load Latency Filtering supported\n");

	/* Initialize sysctl MIB for IBS */
	ibs_sysctl_init();

	PMCDBG0(MDP, INI, 0, "ibs-initialize");

	return (0);
}

/*
 * Finalization code for AMD CPUs.
 */
void
pmc_ibs_finalize(struct pmc_mdep *md)
{
	PMCDBG0(MDP, INI, 1, "ibs-finalize");

	/* Destroy sysctl MIB for IBS */
	ibs_sysctl_fini();

	for (int i = 0; i < pmc_cpu_max(); i++)
		KASSERT(ibs_pcpu[i] == NULL,
		    ("[ibs,%d] non-null pcpu cpu %d", __LINE__, i));

	free(ibs_pcpu, M_PMC);
	ibs_pcpu = NULL;
}
