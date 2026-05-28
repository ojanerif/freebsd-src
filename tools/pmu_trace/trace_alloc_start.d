/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: Davi Chaves Azevedo
 *
 * Purpose:
 *   DTrace FBT observability for FreeBSD hwpmc(4) allocation and AMD start.
 *
 * Usage (requires dtrace -c or -p so $target is defined):
 *   dtrace -Z -q -s tools/pmu_trace/trace_alloc_start.d -o trace.csv \
 *       -c '/usr/sbin/pmcstat -C -q -p ls_not_halted_cyc -p ls_not_halted_cyc -- sleep 1'
 */

#pragma D option quiet

dtrace:::BEGIN
{
	printf("kind,cpu,ri,pid,ns\n");
}

fbt::pmc_do_op_pmcallocate:entry
/pid == $target || progenyof($target)/
{
	self->alloc_ts = timestamp;
}

fbt::pmc_do_op_pmcallocate:return
/self->alloc_ts && (pid == $target || progenyof($target))/
{
	printf("alloc,%d,-,%d,%lld\n", cpu, pid, timestamp - self->alloc_ts);
	self->alloc_ts = 0;
}

fbt::amd_allocate_pmc:entry
/pid == $target || progenyof($target)/
{
	printf("md_alloc,%d,%d,%d,%lld\n", cpu, arg1, pid, timestamp);
}

fbt::amd_start_pmc:entry
/pid == $target || progenyof($target)/
{
	printf("start,%d,%d,%d,%lld\n", cpu, arg1, pid, timestamp);
}

fbt::pmc_release_pmc_descriptor:entry
/pid == $target || progenyof($target)/
{
	printf("release,%d,-,%d,%lld\n", cpu, pid, timestamp);
}
