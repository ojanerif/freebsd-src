#! /usr/libexec/atf-sh
#-
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Author: Davi Chaves Azevedo
#
# Purpose:
#   Validate the real pmcstat(8) offline IBS decode path for AMD Zen 3 B0
#   erratum #1238.  The test writes a minimal pmclog-compatible stream with
#   a producer CPUID from PMCLOG_TYPE_INITIALIZE plus multipart IBS Fetch and
#   Op records, then checks pmcstat -R output.  The OP record keeps erratum
#   #1293 trigger bits in the stream so multipart Op decode is covered too,
#   but pmcstat does not currently print the affected fields in a directly
#   assertable form.
#
#   Affected synthetic CPUIDs:
#     AuthenticAMD-25-00-1  Family 19h model 00h, Zen 3 B0 range
#     AuthenticAMD-25-0F-1  Family 19h model 0Fh, Zen 3 B0 range boundary
#
#   Unaffected synthetic CPUIDs:
#     AuthenticAMD-25-20-1  Family 19h model 20h, Zen 3 non-B0 range
#     AuthenticAMD-25-10-1  Family 19h model 10h, first Zen 4 server range
#     AuthenticAMD-25-11-1  Family 19h model 11h, Zen 4 server range
#     AuthenticAMD-26-00-0  Family 1Ah model 00h, Zen 5 server range

pmcstat_errata_pmcstat()
{
	local p resolved

	p=${PMCSTAT:-$(atf_config_get amd.pmcstat.path pmcstat)}
	case "$p" in
	*/*)
		if [ ! -x "$p" ]; then
			atf_skip "pmcstat override is not executable: $p"
		fi
		PMCSTAT_BIN="$p"
		;;
	*)
		resolved=$(command -v "$p") || \
		    atf_skip "pmcstat not found in PATH"
		PMCSTAT_BIN="$resolved"
		;;
	esac
}

pmcstat_errata_check_support()
{
	if [ "$(uname -m)" != "amd64" ]; then
		atf_skip "synthetic IBS pmclog layout is amd64-specific"
	fi
	command -v cc > /dev/null 2>&1 || atf_skip "cc not found in PATH"
}

pmcstat_errata_build_writer()
{
	cat > mk_ibs_pmclog.c <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))

#define	PMCLOG_HEADER_MAGIC		0xeeU
#define	PMCLOG_TYPE_CLOSELOG		1U
#define	PMCLOG_TYPE_INITIALIZE		3U
#define	PMCLOG_TYPE_CALLCHAIN		16U

#define	PMC_VERSION			0x0a010000U
#define	PMC_CPU_AMD_K8			0x01U
#define	PMC_CPUID_LEN			64U

#define	PMC_CC_F_MULTIPART		0x02U
#define	PMC_CC_MULTIPART_IBS_FETCH	2U
#define	PMC_CC_MULTIPART_IBS_OP		3U

#define	IBS_FETCH_CTL_ICMISS		(1ULL << 51)
#define	IBS_FETCH_CTL_L1TLBMISS		(1ULL << 55)
#define	IBS_FETCH_CTL_OPCACHEMISS	(1ULL << 60)
#define	IBS_FETCH_CTL_L3MISS		(1ULL << 61)

#define	IBS_OP_DATA3_LOAD		(1ULL << 0)
#define	IBS_OP_DATA3_DCL1TLBMISS		(1ULL << 2)
#define	IBS_OP_DATA3_DCMISS		(1ULL << 7)
#define	IBS_OP_DATA3_DCMISSNOMABALLOC	(1ULL << 16)
#define	IBS_OP_DATA3_DCLINADDRVALID	(1ULL << 17)
#define	IBS_OP_DATA3_DCPHYADDRVALID	(1ULL << 18)
#define	IBS_OP_DATA3_L2MISS		(1ULL << 20)
#define	IBS_OP_DATA3_SWPF		(1ULL << 21)

static void
write_bytes(FILE *f, const void *p, size_t n)
{
	if (fwrite(p, n, 1, f) != 1) {
		perror("fwrite");
		exit(1);
	}
}

static void
w32(FILE *f, uint32_t v)
{
	write_bytes(f, &v, sizeof(v));
}

static void
w64(FILE *f, uint64_t v)
{
	write_bytes(f, &v, sizeof(v));
}

static void
write_header(FILE *f, uint8_t type, uint16_t len, uint64_t tsc)
{
	uint32_t h;

	h = (PMCLOG_HEADER_MAGIC << 24) | ((uint32_t)type << 16) | len;
	w32(f, h);
	w32(f, 0);
	w64(f, tsc);
}

static void
write_init(FILE *f, const char *cpuid)
{
	struct timespec ts;
	char id[PMC_CPUID_LEN];

	memset(id, 0, sizeof(id));
	(void)snprintf(id, sizeof(id), "%s", cpuid);
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		perror("clock_gettime");
		exit(1);
	}

	write_header(f, PMCLOG_TYPE_INITIALIZE,
	    16 + 4 + 4 + 8 + sizeof(ts) + PMC_CPUID_LEN, 1);
	w32(f, PMC_VERSION);
	w32(f, PMC_CPU_AMD_K8);
	w64(f, 3500000000ULL);
	write_bytes(f, &ts, sizeof(ts));
	write_bytes(f, id, sizeof(id));
}

static void
write_callchain(FILE *f, uint8_t part, const uint64_t *qwords, size_t nqwords,
    uint64_t tsc)
{
	uint8_t hdr[8];
	size_t len;

	if (nqwords > UINT8_MAX) {
		fprintf(stderr, "too many multipart qwords: %zu\n", nqwords);
		exit(1);
	}

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = part;
	hdr[1] = (uint8_t)nqwords;
	len = 16 + 16 + sizeof(hdr) + nqwords * sizeof(uint64_t);
	if (len > UINT16_MAX) {
		fprintf(stderr, "pmclog callchain record too large: %zu\n", len);
		exit(1);
	}

	write_header(f, PMCLOG_TYPE_CALLCHAIN, (uint16_t)len, tsc);
	w32(f, 1234);
	w32(f, 1234);
	w32(f, 0);
	w32(f, PMC_CC_F_MULTIPART);
	write_bytes(f, hdr, sizeof(hdr));
	write_bytes(f, qwords, nqwords * sizeof(uint64_t));
}

int
main(int argc, char **argv)
{
	FILE *f;
	uint64_t fetch[4];
	uint64_t op[9];
	uint64_t data3;

	if (argc != 3) {
		fprintf(stderr, "usage: %s output.pmc cpuid\n", argv[0]);
		return (2);
	}

	f = fopen(argv[1], "wb");
	if (f == NULL) {
		perror(argv[1]);
		return (1);
	}

	write_init(f, argv[2]);

	fetch[0] = IBS_FETCH_CTL_ICMISS | IBS_FETCH_CTL_L1TLBMISS |
	    IBS_FETCH_CTL_OPCACHEMISS | IBS_FETCH_CTL_L3MISS | (0x33ULL << 32);
	fetch[1] = 0;
	fetch[2] = 0x40001234ULL;
	fetch[3] = 0;
	write_callchain(f, PMC_CC_MULTIPART_IBS_FETCH, fetch, nitems(fetch), 2);

	/*
	 * Include OP DATA3 bits from erratum #1293 so the synthetic log also
	 * carries an Op multipart record.  pmcstat does not print the affected
	 * L2Miss/OpenMemReqs fields today, so the test only asserts that
	 * non-erratum Op fields survive decode.
	 */
	data3 = IBS_OP_DATA3_LOAD | IBS_OP_DATA3_DCL1TLBMISS |
	    IBS_OP_DATA3_DCMISS | IBS_OP_DATA3_DCMISSNOMABALLOC |
	    IBS_OP_DATA3_DCLINADDRVALID | IBS_OP_DATA3_DCPHYADDRVALID |
	    IBS_OP_DATA3_L2MISS | IBS_OP_DATA3_SWPF | (0x2aULL << 26) |
	    (0x44ULL << 32);
	op[0] = 0;
	op[1] = 0x401000ULL;
	op[2] = 0;
	op[3] = 0;
	op[4] = data3;
	op[5] = 0x50001234ULL;
	op[6] = 0x60001234ULL;
	op[7] = 0;
	op[8] = 0;
	write_callchain(f, PMC_CC_MULTIPART_IBS_OP, op, nitems(op), 3);

	write_header(f, PMCLOG_TYPE_CLOSELOG, 16, 4);
	if (fclose(f) != 0) {
		perror("fclose");
		return (1);
	}
	return (0);
}
EOF

	atf_check -s exit:0 -o empty -e save:mk_ibs_pmclog.cc.err \
	    cc -Wall -Wextra -O2 -o mk_ibs_pmclog mk_ibs_pmclog.c
}

pmcstat_errata_decode()
{
	local cpuid log out pmcstat_bin

	cpuid="$1"
	log="$2"
	out="$3"
	pmcstat_bin="$4"

	./mk_ibs_pmclog "$log" "$cpuid" || \
	    atf_fail "failed to create synthetic pmclog for $cpuid"
	atf_check -s exit:0 -o save:"$out" -e save:"$out.err" \
	    "$pmcstat_bin" -R "$log"
	if [ ! -s "$out" ]; then
		atf_fail "pmcstat -R produced empty output for $cpuid"
	fi
}

pmcstat_errata_fetch_line()
{
	local out line

	out="$1"
	line=$(grep '^ibs-fetch[[:space:]]' "$out" | grep -v 'Latency' | head -1)
	if [ -z "$line" ]; then
		atf_fail "pmcstat -R output has no IBS Fetch decode line: $(cat "$out")"
	fi
	printf '%s\n' "$line"
}

pmcstat_errata_op_line()
{
	local out line

	out="$1"
	line=$(grep '^ibs-op[[:space:]].*load ' "$out" | head -1)
	if [ -z "$line" ]; then
		atf_fail "pmcstat -R output has no IBS Op decode line: $(cat "$out")"
	fi
	printf '%s\n' "$line"
}

pmcstat_errata_line_has_token()
{

	case " $1 " in
	*" $2 "*)
		return 0
		;;
	*)
		return 1
		;;
	esac
}

pmcstat_errata_require_fetch_context()
{
	local line

	line="$1"
	for bit in l1tlbmiss opcachemiss l3miss; do
		if ! pmcstat_errata_line_has_token "$line" "$bit"; then
			atf_fail "IBS Fetch line lost $bit while checking errata: $line"
		fi
	done
}

pmcstat_errata_require_op_context()
{
	local line

	line="$1"
	for bit in load l1tlbmiss dcmiss; do
		if ! pmcstat_errata_line_has_token "$line" "$bit"; then
			atf_fail "IBS Op line lost $bit while checking errata: $line"
		fi
	done
}

atf_test_case zen3_b0_fetch_icmiss_suppressed cleanup
zen3_b0_fetch_icmiss_suppressed_head()
{
	atf_set "descr" \
	    "pmcstat -R suppresses invalid IbsIcMiss for AMD Zen 3 B0 producer CPUIDs"
	atf_set "require.progs" "cc"
}
zen3_b0_fetch_icmiss_suppressed_body()
{
	local cpuid line opline

	pmcstat_errata_check_support
	pmcstat_errata_pmcstat
	pmcstat_errata_build_writer

	for cpuid in AuthenticAMD-25-00-1 AuthenticAMD-25-0F-1; do
		pmcstat_errata_decode "$cpuid" "$cpuid.pmc" "$cpuid.out" \
		    "$PMCSTAT_BIN"
		line=$(pmcstat_errata_fetch_line "$cpuid.out")
		opline=$(pmcstat_errata_op_line "$cpuid.out")
		pmcstat_errata_require_fetch_context "$line"
		pmcstat_errata_require_op_context "$opline"
		if pmcstat_errata_line_has_token "$line" icmiss; then
			atf_fail "Zen 3 B0 CPUID $cpuid still prints invalid icmiss: $line"
		fi
	done
}
zen3_b0_fetch_icmiss_suppressed_cleanup()
{
	rm -f mk_ibs_pmclog mk_ibs_pmclog.c mk_ibs_pmclog.cc.err \
	    AuthenticAMD-25-*.pmc \
	    AuthenticAMD-25-*.out AuthenticAMD-25-*.out.err
}

atf_test_case unaffected_fetch_icmiss_preserved cleanup
unaffected_fetch_icmiss_preserved_head()
{
	atf_set "descr" \
	    "pmcstat -R preserves IbsIcMiss for unaffected AMD producer CPUIDs"
	atf_set "require.progs" "cc"
}
unaffected_fetch_icmiss_preserved_body()
{
	local cpuid line opline

	pmcstat_errata_check_support
	pmcstat_errata_pmcstat
	pmcstat_errata_build_writer

	for cpuid in AuthenticAMD-25-20-1 AuthenticAMD-25-10-1 \
	    AuthenticAMD-25-11-1 AuthenticAMD-26-00-0; do
		pmcstat_errata_decode "$cpuid" "$cpuid.pmc" "$cpuid.out" \
		    "$PMCSTAT_BIN"
		line=$(pmcstat_errata_fetch_line "$cpuid.out")
		opline=$(pmcstat_errata_op_line "$cpuid.out")
		pmcstat_errata_require_fetch_context "$line"
		pmcstat_errata_require_op_context "$opline"
		if ! pmcstat_errata_line_has_token "$line" icmiss; then
			atf_fail "unaffected CPUID $cpuid lost valid icmiss: $line"
		fi
	done
}
unaffected_fetch_icmiss_preserved_cleanup()
{
	rm -f mk_ibs_pmclog mk_ibs_pmclog.c mk_ibs_pmclog.cc.err \
	    AuthenticAMD-25-*.pmc \
	    AuthenticAMD-25-*.out AuthenticAMD-25-*.out.err \
	    AuthenticAMD-26-*.pmc AuthenticAMD-26-*.out AuthenticAMD-26-*.out.err
}

atf_init_test_cases()
{
	atf_add_test_case zen3_b0_fetch_icmiss_suppressed
	atf_add_test_case unaffected_fetch_icmiss_preserved
}
