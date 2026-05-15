/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * disk_stressor — background disk I/O load generator
 *
 * Runs sequential write/read and random pwrite/pread loops over a temp
 * file in /tmp until SIGTERM or SIGINT.  Used by run.sh --with-stress to
 * simulate I/O pressure while another test suite executes.
 *
 * The temp file is capped at 4 MiB to avoid filling /tmp on CI machines.
 *
 * Exit code: 0 on clean stop, 1 on internal error.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>

#include "stress_utils.h"

#define DISK_STR_FILE_SIZE	(4UL * 1024UL * 1024UL)
#define DISK_STR_BLOCK_SIZE	4096UL

static volatile sig_atomic_t g_stop = 0;
static char g_path[256];

static void
handle_stop(int sig __unused)
{
	g_stop = 1;
}

static void *
seq_worker(void *arg __unused)
{
	uint8_t buf[DISK_STR_BLOCK_SIZE];
	int fd;
	size_t i, n_blocks;

	memset(buf, 0xCD, sizeof(buf));
	n_blocks = DISK_STR_FILE_SIZE / sizeof(buf);

	fd = open(g_path, O_RDWR);
	if (fd < 0)
		return (NULL);

	while (!g_stop) {
		if (lseek(fd, 0, SEEK_SET) < 0)
			break;
		for (i = 0; i < n_blocks && !g_stop; i++)
			(void)write(fd, buf, sizeof(buf));
		if (lseek(fd, 0, SEEK_SET) < 0)
			break;
		for (i = 0; i < n_blocks && !g_stop; i++)
			(void)read(fd, buf, sizeof(buf));
	}
	close(fd);
	return (NULL);
}

static void *
rand_worker(void *arg __unused)
{
	uint8_t buf[DISK_STR_BLOCK_SIZE];
	uint64_t idx;
	size_t n_blocks;
	int fd;

	n_blocks = DISK_STR_FILE_SIZE / DISK_STR_BLOCK_SIZE;
	idx = (uint64_t)getpid() * STRESS_LCG_MUL;

	fd = open(g_path, O_RDWR);
	if (fd < 0)
		return (NULL);

	while (!g_stop) {
		off_t offset;
		idx = idx * STRESS_LCG_MUL + 1;
		offset = (off_t)((idx % n_blocks) * DISK_STR_BLOCK_SIZE);
		(void)pwrite(fd, buf, sizeof(buf), offset);
		(void)pread(fd, buf, sizeof(buf), offset);
	}
	close(fd);
	return (NULL);
}

static void *
fsync_worker(void *arg __unused)
{
	uint8_t buf[DISK_STR_BLOCK_SIZE];
	int fd;

	memset(buf, 0xEF, sizeof(buf));

	fd = open(g_path, O_WRONLY);
	if (fd < 0)
		return (NULL);

	while (!g_stop) {
		if (lseek(fd, 0, SEEK_SET) < 0)
			break;
		(void)write(fd, buf, sizeof(buf));
		(void)fsync(fd);
	}
	close(fd);
	return (NULL);
}

int
main(void)
{
	pthread_t seq_tid, rand_tid, fsync_tid;
	int fd;
	uint8_t zero[DISK_STR_BLOCK_SIZE];
	size_t i, n_blocks;

	signal(SIGTERM, handle_stop);
	signal(SIGINT,  handle_stop);

	/* Create the temp file. */
	snprintf(g_path, sizeof(g_path), "/tmp/disk_stressor_%d", (int)getpid());

	fd = open(g_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		fprintf(stderr, "disk_stressor: open %s: %s\n", g_path,
		    strerror(errno));
		return (1);
	}

	memset(zero, 0, sizeof(zero));
	n_blocks = DISK_STR_FILE_SIZE / sizeof(zero);
	for (i = 0; i < n_blocks; i++) {
		if (write(fd, zero, sizeof(zero)) != (ssize_t)sizeof(zero)) {
			fprintf(stderr, "disk_stressor: pre-alloc: %s\n",
			    strerror(errno));
			close(fd);
			unlink(g_path);
			return (1);
		}
	}
	fsync(fd);
	close(fd);

	pthread_create(&seq_tid,   NULL, seq_worker,   NULL);
	pthread_create(&rand_tid,  NULL, rand_worker,  NULL);
	pthread_create(&fsync_tid, NULL, fsync_worker, NULL);

	while (!g_stop)
		sleep(1);

	pthread_join(seq_tid,   NULL);
	pthread_join(rand_tid,  NULL);
	pthread_join(fsync_tid, NULL);

	unlink(g_path);

	fprintf(stderr, "disk_stressor[%d]: stopped cleanly\n",
	    (int)getpid());
	return (0);
}
