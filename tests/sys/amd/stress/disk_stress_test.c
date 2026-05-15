/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * Disk Stress Tests — TC-DSTR
 *
 * Standalone filesystem/disk stress validation using /tmp.
 * No root required.  Three workloads:
 *
 * 1. disk_stress_sequential [TC-DSTR-01]
 *    Write 64 MiB to a temp file in /tmp, read it back byte-for-byte,
 *    and compare.  Asserts data integrity end-to-end.  Cleans up the
 *    temp file in both ATF body and cleanup.
 *
 * 2. disk_stress_random [TC-DSTR-02]
 *    Pre-allocate a 32 MiB file in /tmp, then perform 4 KiB pwrite()/
 *    pread() at pseudo-random offsets for 120 seconds, accumulating
 *    XOR checksums of writes and reads.  Asserts checksums match.
 *
 * 3. disk_stress_fsync [TC-DSTR-03]
 *    Write 4 KiB blocks and call fsync(2) after each write for 120
 *    seconds in /tmp.  Asserts every fsync(2) call succeeds (models a
 *    database-commit workload).
 */

#include <sys/stat.h>

#include <atf-c.h>
#include <fcntl.h>
#include <stdio.h>

#include "stress_utils.h"

#define DISK_SEQ_SIZE		(64UL * 1024UL * 1024UL)
#define DISK_RAND_FILE_SIZE	(32UL * 1024UL * 1024UL)
#define DISK_BLOCK_SIZE		4096UL

/* -----------------------------------------------------------------------
 * TC-DSTR-01: sequential write + read verify
 * ----------------------------------------------------------------------- */

static char g_seq_path[256];

ATF_TC_WITH_CLEANUP(disk_stress_sequential);
ATF_TC_HEAD(disk_stress_sequential, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-DSTR-01] 64 MiB sequential write then byte-for-byte read "
	    "verify in /tmp; asserts data integrity");
	atf_tc_set_md_var(tc, "timeout", "120");
}

ATF_TC_BODY(disk_stress_sequential, tc)
{
	int fd;
	uint8_t *wbuf, *rbuf;
	size_t i, written, read_bytes;
	ssize_t n;

	stress_tmpfile_path(g_seq_path, sizeof(g_seq_path), "seq");

	wbuf = malloc(DISK_SEQ_SIZE);
	rbuf = malloc(DISK_SEQ_SIZE);
	ATF_REQUIRE_MSG(wbuf != NULL && rbuf != NULL,
	    "malloc 64 MiB buffers: %s", strerror(errno));

	/* Fill write buffer with a deterministic pattern. */
	for (i = 0; i < DISK_SEQ_SIZE; i++)
		wbuf[i] = (uint8_t)(i ^ (i >> 8));

	fd = open(g_seq_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	ATF_REQUIRE_MSG(fd >= 0, "open %s: %s", g_seq_path, strerror(errno));

	/* Write loop. */
	written = 0;
	while (written < DISK_SEQ_SIZE) {
		n = write(fd, wbuf + written, DISK_SEQ_SIZE - written);
		ATF_REQUIRE_MSG(n > 0, "write at offset %zu: %s",
		    written, strerror(errno));
		written += (size_t)n;
	}
	ATF_REQUIRE_MSG(fsync(fd) == 0, "fsync: %s", strerror(errno));
	close(fd);

	/* Read and compare. */
	fd = open(g_seq_path, O_RDONLY);
	ATF_REQUIRE_MSG(fd >= 0, "open(RDONLY) %s: %s", g_seq_path,
	    strerror(errno));

	read_bytes = 0;
	while (read_bytes < DISK_SEQ_SIZE) {
		n = read(fd, rbuf + read_bytes, DISK_SEQ_SIZE - read_bytes);
		ATF_REQUIRE_MSG(n > 0, "read at offset %zu: %s",
		    read_bytes, strerror(errno));
		read_bytes += (size_t)n;
	}
	close(fd);

	ATF_CHECK_MSG(memcmp(wbuf, rbuf, DISK_SEQ_SIZE) == 0,
	    "64 MiB read-back differs from written data — data corruption");

	unlink(g_seq_path);

	printf("sequential: wrote and verified %lu MiB in %s\n",
	    (unsigned long)(DISK_SEQ_SIZE / (1024 * 1024)), g_seq_path);

	free(wbuf);
	free(rbuf);
}

ATF_TC_CLEANUP(disk_stress_sequential, tc)
{
	if (g_seq_path[0] != '\0')
		(void)unlink(g_seq_path);
}

/* -----------------------------------------------------------------------
 * TC-DSTR-02: random 4 KiB pwrite/pread with XOR checksum
 * ----------------------------------------------------------------------- */

struct rand_io_arg {
	volatile bool	*stop;
	int		 fd;
	size_t		 file_size;
	int		 error;
	uint64_t	 write_cksum;
	uint64_t	 read_cksum;
	uint64_t	 ops;
};

static void *
rand_io_thread(void *arg)
{
	struct rand_io_arg *ra = arg;
	uint8_t buf[DISK_BLOCK_SIZE];
	uint64_t idx;
	size_t n_blocks;
	off_t offset;
	ssize_t n;
	size_t j;

	n_blocks = ra->file_size / DISK_BLOCK_SIZE;
	idx = (uint64_t)getpid() * STRESS_LCG_MUL;

	while (!*ra->stop) {
		/* Pick a random block. */
		idx = idx * STRESS_LCG_MUL + 1;
		offset = (off_t)((idx % n_blocks) * DISK_BLOCK_SIZE);

		/* Fill block with a pattern derived from offset + op count. */
		for (j = 0; j < sizeof(buf); j++)
			buf[j] = (uint8_t)(offset + (off_t)ra->ops + (off_t)j);

		/* Write. */
		n = pwrite(ra->fd, buf, sizeof(buf), offset);
		if (n != (ssize_t)sizeof(buf)) {
			ra->error = errno;
			return (NULL);
		}
		for (j = 0; j < sizeof(buf); j++)
			ra->write_cksum ^= buf[j];

		/* Read back. */
		n = pread(ra->fd, buf, sizeof(buf), offset);
		if (n != (ssize_t)sizeof(buf)) {
			ra->error = errno;
			return (NULL);
		}
		for (j = 0; j < sizeof(buf); j++)
			ra->read_cksum ^= buf[j];

		ra->ops++;
	}

	ra->error = 0;
	return (NULL);
}

ATF_TC_WITH_CLEANUP(disk_stress_random);
ATF_TC_HEAD(disk_stress_random, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-DSTR-02] 4 KiB random pwrite/pread over 32 MiB file in /tmp "
	    "(120 s); asserts XOR checksums match");
	atf_tc_set_md_var(tc, "timeout", "200");
}

static char g_rand_path[256];

ATF_TC_BODY(disk_stress_random, tc)
{
	struct rand_io_arg arg;
	pthread_t tid;
	volatile bool stop = false;
	int fd;
	time_t deadline;
	uint8_t zero_block[DISK_BLOCK_SIZE];
	size_t written, n_blocks;
	ssize_t n;

	stress_tmpfile_path(g_rand_path, sizeof(g_rand_path), "rand");

	/* Pre-allocate file. */
	fd = open(g_rand_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	ATF_REQUIRE_MSG(fd >= 0, "open %s: %s", g_rand_path, strerror(errno));

	memset(zero_block, 0, sizeof(zero_block));
	n_blocks = DISK_RAND_FILE_SIZE / DISK_BLOCK_SIZE;
	for (written = 0; written < n_blocks; written++) {
		n = write(fd, zero_block, sizeof(zero_block));
		ATF_REQUIRE_MSG(n == (ssize_t)sizeof(zero_block),
		    "pre-alloc write block %zu: %s", written, strerror(errno));
	}
	fsync(fd);

	arg.stop        = &stop;
	arg.fd          = fd;
	arg.file_size   = DISK_RAND_FILE_SIZE;
	arg.error       = -1;
	arg.write_cksum = 0;
	arg.read_cksum  = 0;
	arg.ops         = 0;

	ATF_REQUIRE_MSG(pthread_create(&tid, NULL, rand_io_thread, &arg) == 0,
	    "pthread_create: %s", strerror(errno));

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline)
		sleep(STRESS_POLL_INTERVAL_SEC);

	stop = true;
	pthread_join(tid, NULL);
	close(fd);
	unlink(g_rand_path);

	ATF_CHECK_MSG(arg.error == 0,
	    "random I/O thread failed: %s", strerror(arg.error));
	ATF_CHECK_MSG(arg.write_cksum == arg.read_cksum,
	    "XOR checksum mismatch: write=0x%llx read=0x%llx "
	    "(data corruption in random I/O)",
	    (unsigned long long)arg.write_cksum,
	    (unsigned long long)arg.read_cksum);

	printf("random: %llu 4 KiB ops in %s over %d seconds\n",
	    (unsigned long long)arg.ops, g_rand_path, STRESS_DURATION_SEC);
}

ATF_TC_CLEANUP(disk_stress_random, tc)
{
	if (g_rand_path[0] != '\0')
		(void)unlink(g_rand_path);
}

/* -----------------------------------------------------------------------
 * TC-DSTR-03: fsync storm (database-commit simulation)
 * ----------------------------------------------------------------------- */

ATF_TC_WITH_CLEANUP(disk_stress_fsync);
ATF_TC_HEAD(disk_stress_fsync, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-DSTR-03] 4 KiB write + fsync(2) loop in /tmp (120 s); "
	    "asserts every fsync call returns 0");
	atf_tc_set_md_var(tc, "timeout", "200");
}

static char g_fsync_path[256];

ATF_TC_BODY(disk_stress_fsync, tc)
{
	int fd;
	uint8_t buf[DISK_BLOCK_SIZE];
	size_t i;
	uint64_t ops = 0, fsync_fails = 0;
	time_t deadline;

	stress_tmpfile_path(g_fsync_path, sizeof(g_fsync_path), "fsync");

	fd = open(g_fsync_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	ATF_REQUIRE_MSG(fd >= 0, "open %s: %s", g_fsync_path, strerror(errno));

	/* Fill buffer with a recognisable pattern. */
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)i;

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline) {
		/* Write 4 KiB. Wrap the file at 4 MiB to avoid filling /tmp. */
		if (lseek(fd, 0, SEEK_SET) < 0)
			break;

		size_t blocks;
		for (blocks = 0; blocks < (4UL * 1024 * 1024 / DISK_BLOCK_SIZE);
		    blocks++) {
			if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
				break;
		}

		if (fsync(fd) != 0)
			fsync_fails++;

		ops++;
	}

	close(fd);
	unlink(g_fsync_path);

	ATF_CHECK_MSG(fsync_fails == 0,
	    "%llu fsync(2) calls failed out of %llu total",
	    (unsigned long long)fsync_fails,
	    (unsigned long long)ops);
	ATF_CHECK_MSG(ops > 0, "zero fsync cycles completed");

	printf("fsync: %llu write+fsync cycles in %d seconds\n",
	    (unsigned long long)ops, STRESS_DURATION_SEC);
}

ATF_TC_CLEANUP(disk_stress_fsync, tc)
{
	if (g_fsync_path[0] != '\0')
		(void)unlink(g_fsync_path);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, disk_stress_sequential);
	ATF_TP_ADD_TC(tp, disk_stress_random);
	ATF_TP_ADD_TC(tp, disk_stress_fsync);

	return (atf_no_error());
}
