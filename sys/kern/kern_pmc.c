/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2003-2008 Joseph Koshy
 * Copyright (c) 2007 The FreeBSD Foundation
 * All rights reserved.
 *
 * Portions of this software were developed by A. Joseph Koshy under
 * sponsorship from the FreeBSD Foundation and Google, Inc.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include "opt_hwpmc_hooks.h"

#include <sys/param.h>
#include <sys/ctype.h>
#include <sys/domainset.h>
#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/pmc.h>
#include <sys/pmckern.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>

#ifdef	HWPMC_HOOKS
FEATURE(hwpmc_hooks, "Kernel support for HW PMC");
#define	PMC_KERNEL_VERSION	PMC_VERSION
#else
#define	PMC_KERNEL_VERSION	0
#endif

MALLOC_DECLARE(M_PMCHOOKS);
MALLOC_DEFINE(M_PMCHOOKS, "pmchooks", "Memory space for PMC hooks");

/* memory pool */
MALLOC_DEFINE(M_PMC, "pmc", "Memory space for the PMC module");

const int pmc_kernel_version = PMC_KERNEL_VERSION;

/* Hook variable. */
int __read_mostly (*pmc_hook)(struct thread *td, int function, void *arg) = NULL;

/* Interrupt handler */
int __read_mostly (*pmc_intr)(struct trapframe *tf) = NULL;

/* HWT hooks */
void __read_mostly (*hwt_hook)(struct thread *td, int func, void *arg) = NULL;
int __read_mostly (*hwt_intr)(struct trapframe *tf) = NULL;

DPCPU_DEFINE(uint8_t, pmc_sampled);

/*
 * A global count of SS mode PMCs.  When non-zero, this means that
 * we have processes that are sampling the system as a whole.
 */
volatile int pmc_ss_count;

/*
 * Since PMC(4) may not be loaded in the current kernel, the
 * convention followed is that a non-NULL value of 'pmc_hook' implies
 * the presence of this kernel module.
 *
 * This requires us to protect 'pmc_hook' with a
 * shared (sx) lock -- thus making the process of calling into PMC(4)
 * somewhat more expensive than a simple 'if' check and indirect call.
 */
struct sx pmc_sx;
SX_SYSINIT(pmcsx, &pmc_sx, "pmc-sx");

/*
 * PMC Soft per cpu trapframe.
 */
struct trapframe pmc_tf[MAXCPU];

/*
 * Per domain list of buffer headers
 */
__read_mostly struct pmc_domain_buffer_header *pmc_dom_hdrs[MAXMEMDOM];

/*
 * PMC Soft use a global table to store registered events.
 */

SYSCTL_NODE(_kern, OID_AUTO, hwpmc, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "HWPMC parameters");

static int pmc_softevents = 16;
SYSCTL_INT(_kern_hwpmc, OID_AUTO, softevents, CTLFLAG_RDTUN,
    &pmc_softevents, 0, "maximum number of soft events");

int pmc_softs_count;
struct pmc_soft **pmc_softs;

struct mtx pmc_softs_mtx;
MTX_SYSINIT(pmc_soft_mtx, &pmc_softs_mtx, "pmc-softs", MTX_SPIN);

/*
 * Helper functions.
 */

/*
 * A note on the CPU numbering scheme used by the hwpmc(4) driver.
 *
 * CPUs are denoted using numbers in the range 0..[pmc_cpu_max()-1].
 * CPUs could be numbered "sparsely" in this range; the predicate
 * `pmc_cpu_is_present()' is used to test whether a given CPU is
 * physically present.
 *
 * Further, a CPU that is physically present may be administratively
 * disabled or otherwise unavailable for use by hwpmc(4).  The
 * `pmc_cpu_is_active()' predicate tests for CPU usability.  An
 * "active" CPU participates in thread scheduling and can field
 * interrupts raised by PMC hardware.
 *
 * On systems with hyperthreaded CPUs, multiple logical CPUs may share
 * PMC hardware resources.  For such processors one logical CPU is
 * denoted as the primary owner of the in-CPU PMC resources. The
 * pmc_cpu_is_primary() predicate is used to distinguish this primary
 * CPU from the others.
 */

int
pmc_cpu_is_active(int cpu)
{
#ifdef	SMP
	return (pmc_cpu_is_present(cpu));
#else
	return (1);
#endif
}

/* Deprecated. */
int
pmc_cpu_is_disabled(int cpu)
{
	return (!pmc_cpu_is_active(cpu));
}

int
pmc_cpu_is_present(int cpu)
{
#ifdef	SMP
	return (!CPU_ABSENT(cpu));
#else
	return (1);
#endif
}

int
pmc_cpu_is_primary(int cpu)
{
#ifdef	SMP
	return (!CPU_ISSET(cpu, &logical_cpus_mask));
#else
	return (1);
#endif
}

/*
 * Return the maximum CPU number supported by the system.  The return
 * value is used for scaling internal data structures and for runtime
 * checks.
 */
unsigned int
pmc_cpu_max(void)
{
#ifdef	SMP
	return (mp_maxid+1);
#else
	return (1);
#endif
}

#ifdef	INVARIANTS

/*
 * Return the count of CPUs in the `active' state in the system.
 */
int
pmc_cpu_max_active(void)
{
#ifdef	SMP
	/*
	 * When support for CPU hot-plugging is added to the kernel,
	 * this function would change to return the current number
	 * of "active" CPUs.
	 */
	return (mp_ncpus);
#else
	return (1);
#endif
}

#endif

/*
 * Cleanup event name:
 * - remove duplicate '_'
 * - all uppercase
 */
static void
pmc_soft_namecleanup(char *name)
{
	char *p, *q;

	p = q = name;

	for ( ; *p == '_' ; p++)
		;
	for ( ; *p ; p++) {
		if (*p == '_' && (*(p + 1) == '_' || *(p + 1) == '\0'))
			continue;
		else
			*q++ = toupper(*p);
	}
	*q = '\0';
}

void
pmc_soft_ev_register(struct pmc_soft *ps)
{
	static int warned = 0;
	int n;

	ps->ps_running  = 0;
	ps->ps_ev.pm_ev_code = 0; /* invalid */
	pmc_soft_namecleanup(ps->ps_ev.pm_ev_name);

	mtx_lock_spin(&pmc_softs_mtx);

	if (pmc_softs_count >= pmc_softevents) {
		/*
		 * XXX Reusing events can enter a race condition where
		 * new allocated event will be used as an old one.
		 */
		for (n = 0; n < pmc_softevents; n++)
			if (pmc_softs[n] == NULL)
				break;
		if (n == pmc_softevents) {
			mtx_unlock_spin(&pmc_softs_mtx);
			if (!warned) {
				printf("hwpmc: too many soft events, "
				    "increase kern.hwpmc.softevents tunable\n");
				warned = 1;
			}
			return;
		}

		ps->ps_ev.pm_ev_code = PMC_EV_SOFT_FIRST + n;
		pmc_softs[n] = ps;
	} else {
		ps->ps_ev.pm_ev_code = PMC_EV_SOFT_FIRST + pmc_softs_count;
		pmc_softs[pmc_softs_count++] = ps;
	}

	mtx_unlock_spin(&pmc_softs_mtx);
}

void
pmc_soft_ev_deregister(struct pmc_soft *ps)
{

	KASSERT(ps != NULL, ("pmc_soft_deregister: called with NULL"));

	mtx_lock_spin(&pmc_softs_mtx);

	if (ps->ps_ev.pm_ev_code != 0 &&
	    (ps->ps_ev.pm_ev_code - PMC_EV_SOFT_FIRST) < pmc_softevents) {
		KASSERT((int)ps->ps_ev.pm_ev_code >= PMC_EV_SOFT_FIRST &&
		    (int)ps->ps_ev.pm_ev_code <= PMC_EV_SOFT_LAST,
		    ("pmc_soft_deregister: invalid event value"));
		pmc_softs[ps->ps_ev.pm_ev_code - PMC_EV_SOFT_FIRST] = NULL;
	}

	mtx_unlock_spin(&pmc_softs_mtx);
}

struct pmc_soft *
pmc_soft_ev_acquire(enum pmc_event ev)
{
	struct pmc_soft *ps;

	if (ev == 0 || (ev - PMC_EV_SOFT_FIRST) >= pmc_softevents)
		return NULL;

	KASSERT((int)ev >= PMC_EV_SOFT_FIRST &&
	    (int)ev <= PMC_EV_SOFT_LAST,
	    ("event out of range"));

	mtx_lock_spin(&pmc_softs_mtx);

	ps = pmc_softs[ev - PMC_EV_SOFT_FIRST];
	if (ps == NULL)
		mtx_unlock_spin(&pmc_softs_mtx);

	return ps;
}

void
pmc_soft_ev_release(struct pmc_soft *ps)
{

	mtx_unlock_spin(&pmc_softs_mtx);
}

/*
 * AMD IOMMU performance counter registry.
 *
 * This lives here, rather than in the hwpmc(4) module, because the AMD IOMMU
 * driver is compiled into the kernel while hwpmc(4) is loadable: the kernel
 * must not depend on a symbol provided by a module.  Both the IOMMU driver
 * and the hwpmc class driver reference only this file, which is always
 * present.
 *
 * Units are registered from amdiommu_attach() and are never removed while in
 * use, so the array is effectively write-once at boot.  Access is unlocked
 * for the same reason the IOMMU unit list is.
 */
static struct pmc_amdiommu_unit amdiommu_pmcs[PMC_AMDIOMMU_MAX_UNITS];
static int amdiommu_npmcs;

int
amdiommu_pmc_register(int unit, void *arg, uint8_t nbanks, uint8_t ncounters)
{

	if (unit < 0 || unit >= PMC_AMDIOMMU_MAX_UNITS)
		return (EINVAL);
	if (arg == NULL || nbanks == 0 || ncounters == 0)
		return (EINVAL);
	if (amdiommu_pmcs[unit].pau_arg != NULL)
		return (EEXIST);

	amdiommu_pmcs[unit].pau_arg = arg;
	amdiommu_pmcs[unit].pau_nbanks = nbanks;
	amdiommu_pmcs[unit].pau_ncounters = ncounters;
	amdiommu_npmcs++;
	return (0);
}

void
amdiommu_pmc_unregister(int unit)
{

	if (unit < 0 || unit >= PMC_AMDIOMMU_MAX_UNITS)
		return;
	if (amdiommu_pmcs[unit].pau_arg == NULL)
		return;

	amdiommu_pmcs[unit].pau_arg = NULL;
	amdiommu_pmcs[unit].pau_nbanks = 0;
	amdiommu_pmcs[unit].pau_ncounters = 0;
	amdiommu_npmcs--;
}

int
pmc_amdiommu_nunits(void)
{

	return (amdiommu_npmcs);
}

/*
 * Number of PMC classes the AMD IOMMU counters require: one if any
 * counter-capable unit registered, zero otherwise.  hwpmc uses this to size
 * pmd_classdep[] before pmc_mdep_alloc().
 */
int
pmc_amdiommu_nclasses(void)
{

	return (amdiommu_npmcs > 0 ? 1 : 0);
}

const struct pmc_amdiommu_unit *
pmc_amdiommu_unit(int idx)
{
	int i, n;

	if (idx < 0 || idx >= amdiommu_npmcs)
		return (NULL);
	for (i = 0, n = 0; i < PMC_AMDIOMMU_MAX_UNITS; i++) {
		if (amdiommu_pmcs[i].pau_arg == NULL)
			continue;
		if (n++ == idx)
			return (&amdiommu_pmcs[i]);
	}
	return (NULL);
}

/*
 *  Initialise hwpmc.
 */
static void
init_hwpmc(void *dummy __unused)
{
	int domain, cpu;

	if (pmc_softevents <= 0 ||
	    pmc_softevents > PMC_EV_DYN_COUNT) {
		(void) printf("hwpmc: tunable \"softevents\"=%d out of "
		    "range.\n", pmc_softevents);
		pmc_softevents = PMC_EV_DYN_COUNT;
	}
	pmc_softs = malloc(pmc_softevents * sizeof(*pmc_softs), M_PMCHOOKS,
	    M_WAITOK | M_ZERO);

	for (domain = 0; domain < vm_ndomains; domain++) {
		pmc_dom_hdrs[domain] = malloc_domainset(
		    sizeof(struct pmc_domain_buffer_header), M_PMC,
		    DOMAINSET_PREF(domain), M_WAITOK | M_ZERO);
		mtx_init(&pmc_dom_hdrs[domain]->pdbh_mtx, "pmc_bufferlist_mtx", "pmc-leaf", MTX_SPIN);
		TAILQ_INIT(&pmc_dom_hdrs[domain]->pdbh_head);
	}
	CPU_FOREACH(cpu) {
		domain = pcpu_find(cpu)->pc_domain;
		KASSERT(pmc_dom_hdrs[domain] != NULL, ("no mem allocated for domain: %d", domain));
		pmc_dom_hdrs[domain]->pdbh_ncpus++;
	}

}

SYSINIT(hwpmc, SI_SUB_KDTRACE, SI_ORDER_FIRST, init_hwpmc, NULL);

/*
 * PMC wrappers around amdiommu_pc_* — these live here so that
 * hwpmc(4), which is a loadable module, does not need to include
 * <x86/iommu/amd_iommu.h>.  kern_pmc.c is compiled into the kernel
 * and may include device-driver headers freely.
 */
#if defined(__amd64__) || defined(__i386__)
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/vmem.h>
#include <sys/memdesc.h>

#include <machine/bus.h>

#include <dev/iommu/iommu.h>
#include <x86/iommu/amd_reg.h>
#include <x86/iommu/x86_iommu.h>
#include <x86/iommu/amd_iommu.h>

void
pmc_amdiommu_get_topology(const struct amdiommu_unit *u,
    uint8_t *nbanks, uint8_t *ncounters)
{
	amdiommu_pc_get_topology(u, nbanks, ncounters);
}

int
pmc_amdiommu_read(const struct amdiommu_unit *u, uint8_t bank,
    uint8_t cntr, uint8_t fxn, uint64_t *val)
{
	return (amdiommu_pc_read(u, bank, cntr, fxn, val));
}

int
pmc_amdiommu_write(const struct amdiommu_unit *u, uint8_t bank,
    uint8_t cntr, uint8_t fxn, uint64_t val)
{
	return (amdiommu_pc_write(u, bank, cntr, fxn, val));
}

/*
 * Return true if bank N is locked in ANY of the three filter lock registers
 * (PASID, Domain or DeviceID).  A locked bank silently ignores filter writes
 * (spec §3.4.22, MMIO 0x4008/0x4010/0x4018).  hwpmc must refuse start_pmc
 * rather than count the wrong events with no diagnostic.
 *
 * Locking: the three BANKLOCK registers are set only by firmware during ACPI
 * S-state transitions and are immutable post-boot.  Reads without
 * AMDIOMMU_LOCK are therefore safe.  The check and the subsequent filter
 * writes in amdiommu_start_pmc are NOT atomic vs. firmware, but that is
 * acceptable because firmware lock changes only happen on S3/S4 resume,
 * long before hwpmc would run.
 *
 * The lock registers are global MMIO, not per-counter, so we read them
 * directly via amdiommu_read8 without going through the per-counter offset
 * decoder used by amdiommu_pc_{read,write}.
 */
bool
pmc_amdiommu_bank_is_locked(const struct amdiommu_unit *u, uint8_t bank)
{
	uint64_t pasid_lock, domain_lock, devid_lock;
	uint64_t bit;

	/* NCounterBanks is 6-bit (0..63); bit 64+ would shift into UB. */
	if (bank >= 64)
		return (false);

	bit = (uint64_t)1 << bank;

	pasid_lock  = amdiommu_read8(u, AMDIOMMU_PC_PASID_BANKLOCK);
	domain_lock = amdiommu_read8(u, AMDIOMMU_PC_DOMAIN_BANKLOCK);
	devid_lock  = amdiommu_read8(u, AMDIOMMU_PC_DEVID_BANKLOCK);

	return ((pasid_lock | domain_lock | devid_lock) & bit) != 0;
}
#endif
