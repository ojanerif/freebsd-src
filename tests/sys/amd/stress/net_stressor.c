/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * net_stressor — background network load generator
 *
 * Creates NET_STRESS_THREADS (default 2 when set by run.sh --with-stress,
 * 4 otherwise) AF_UNIX socketpair producer/consumer loops until SIGTERM
 * or SIGINT.  Uses only loopback/AF_UNIX — no external network traffic.
 *
 * WARNING: Do not raise NET_STRESS_THREADS beyond 4 when running alongside
 * IBS tests.  High network load amplifies NMI storms (FreeBSD-Tests-026).
 *
 * Exit code: 0 on clean stop, 1 on internal error.
 */

#include <sys/socket.h>

#include <signal.h>
#include <stdio.h>

#include "stress_utils.h"

static volatile sig_atomic_t g_stop = 0;

static void
handle_stop(int sig __unused)
{
	g_stop = 1;
}

#define NET_STR_MSG_SIZE	64

struct pair_arg {
	int	fd[2];
};

static void *
producer(void *arg)
{
	struct pair_arg *pa = arg;
	uint8_t msg[NET_STR_MSG_SIZE];
	uint64_t seq = 0;

	memset(msg, 0xAB, sizeof(msg));
	while (!g_stop) {
		memcpy(msg, &seq, sizeof(seq));
		if (send(pa->fd[0], msg, sizeof(msg), 0) !=
		    (ssize_t)sizeof(msg))
			break;
		seq++;
	}
	return (NULL);
}

static void *
consumer(void *arg)
{
	struct pair_arg *pa = arg;
	uint8_t msg[NET_STR_MSG_SIZE];

	while (!g_stop) {
		ssize_t n = recv(pa->fd[1], msg, sizeof(msg), MSG_DONTWAIT);
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			break;
	}
	return (NULL);
}

int
main(void)
{
	const char *env;
	int nthreads, i;
	struct pair_arg *pairs;
	pthread_t *prod_threads, *cons_threads;

	signal(SIGTERM, handle_stop);
	signal(SIGINT,  handle_stop);

	nthreads = 2;
	env = getenv("NET_STRESS_THREADS");
	if (env != NULL && atoi(env) > 0)
		nthreads = atoi(env);

	pairs        = calloc((size_t)nthreads, sizeof(struct pair_arg));
	prod_threads = calloc((size_t)nthreads, sizeof(pthread_t));
	cons_threads = calloc((size_t)nthreads, sizeof(pthread_t));
	if (pairs == NULL || prod_threads == NULL || cons_threads == NULL) {
		fprintf(stderr, "net_stressor: calloc: %s\n", strerror(errno));
		return (1);
	}

	for (i = 0; i < nthreads; i++) {
		if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0,
		    pairs[i].fd) != 0) {
			fprintf(stderr, "net_stressor: socketpair %d: %s\n",
			    i, strerror(errno));
			g_stop = 1;
			break;
		}
		pthread_create(&prod_threads[i], NULL, producer, &pairs[i]);
		pthread_create(&cons_threads[i], NULL, consumer, &pairs[i]);
	}

	while (!g_stop)
		sleep(1);

	for (i = 0; i < nthreads; i++) {
		shutdown(pairs[i].fd[0], SHUT_RDWR);
		shutdown(pairs[i].fd[1], SHUT_RDWR);
		pthread_join(prod_threads[i], NULL);
		pthread_join(cons_threads[i], NULL);
		close(pairs[i].fd[0]);
		close(pairs[i].fd[1]);
	}

	fprintf(stderr, "net_stressor[%d]: stopped cleanly (threads=%d)\n",
	    (int)getpid(), nthreads);

	free(pairs);
	free(prod_threads);
	free(cons_threads);
	return (0);
}
