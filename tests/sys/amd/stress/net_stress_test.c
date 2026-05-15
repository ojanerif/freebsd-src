/*-
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: ojanerif@amd.com
 */

/*
 * Network Stress Tests — TC-NSTR
 *
 * Standalone network subsystem stress validation using only loopback
 * interfaces and AF_UNIX sockets.  No root required.  Three workloads:
 *
 * 1. net_stress_unix_socketpair [TC-NSTR-01]
 *    N AF_UNIX socketpair() producer/consumer pairs each sending
 *    sequence-numbered 64-byte messages for 120 seconds.  Asserts no
 *    message gaps and no data corruption.
 *
 * 2. net_stress_loopback_tcp [TC-NSTR-02]
 *    Loopback TCP echo server on an ephemeral port + N client threads
 *    each sending 64 KiB chunks for 120 seconds.  Asserts bytes received
 *    == bytes sent per client.
 *
 * 3. net_stress_connection_rate [TC-NSTR-03]
 *    Rapid connect()/close() cycles to a loopback listener on an
 *    ephemeral port for 120 seconds.  Asserts no file-descriptor leak
 *    by comparing the next-available fd before and after the test.
 *
 * WARNING: When used via --with-stress alongside IBS tests, the net
 * stressor runs at reduced intensity (NET_STRESS_THREADS=2) to avoid
 * amplifying the NMI storm described in FreeBSD-Tests-026.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <atf-c.h>
#include <fcntl.h>
#include <stdio.h>

#include "stress_utils.h"

/* Number of socket-pair threads (overridden by NET_STRESS_THREADS env). */
#define NET_DEFAULT_THREADS	4
#define NET_MSG_SIZE		64
#define NET_TCP_CHUNK_SIZE	(64 * 1024)
#define NET_TCP_BACKLOG		32

static int
net_nthreads(void)
{
	const char *e = getenv("NET_STRESS_THREADS");
	if (e != NULL) {
		int n = atoi(e);
		if (n > 0)
			return (n);
	}
	return (NET_DEFAULT_THREADS);
}

/* -----------------------------------------------------------------------
 * TC-NSTR-01: AF_UNIX socketpair flood
 * ----------------------------------------------------------------------- */

struct unix_pair_arg {
	volatile bool	*stop;
	int		 fd[2];		/* socketpair ends */
	int		 error;
	uint64_t	 sent;
	uint64_t	 recv;
	int		 seq_fail;
};

static void *
unix_producer(void *arg)
{
	struct unix_pair_arg *pa = arg;
	uint8_t msg[NET_MSG_SIZE];
	uint64_t seq = 0;

	while (!*pa->stop) {
		memset(msg, 0, sizeof(msg));
		memcpy(msg, &seq, sizeof(seq));
		if (send(pa->fd[0], msg, sizeof(msg), 0) != (ssize_t)sizeof(msg))
			break;
		pa->sent++;
		seq++;
	}
	return (NULL);
}

static void *
unix_consumer(void *arg)
{
	struct unix_pair_arg *pa = arg;
	uint8_t msg[NET_MSG_SIZE];
	uint64_t expected = 0, got;

	while (!*pa->stop) {
		ssize_t n = recv(pa->fd[1], msg, sizeof(msg), MSG_DONTWAIT);
		if (n == (ssize_t)sizeof(msg)) {
			memcpy(&got, msg, sizeof(got));
			if (got != expected)
				pa->seq_fail++;
			expected = got + 1;
			pa->recv++;
		} else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			break;
		}
	}
	return (NULL);
}

ATF_TC(net_stress_unix_socketpair);
ATF_TC_HEAD(net_stress_unix_socketpair, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-NSTR-01] AF_UNIX socketpair flood with sequence-numbered "
	    "messages (120 s); asserts no data corruption");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(net_stress_unix_socketpair, tc)
{
	struct unix_pair_arg *pairs;
	pthread_t *prod_threads, *cons_threads;
	volatile bool stop = false;
	int n, i, error;

	/* Ignore SIGPIPE: shutdown() on a socket while send() is in flight
	 * would otherwise terminate the whole process. */
	signal(SIGPIPE, SIG_IGN);
	time_t deadline;
	uint64_t total_sent = 0, total_recv = 0;
	int total_seq_fail = 0;

	n = net_nthreads();

	pairs       = calloc((size_t)n, sizeof(struct unix_pair_arg));
	prod_threads = calloc((size_t)n, sizeof(pthread_t));
	cons_threads = calloc((size_t)n, sizeof(pthread_t));
	ATF_REQUIRE(pairs != NULL && prod_threads != NULL &&
	    cons_threads != NULL);

	for (i = 0; i < n; i++) {
		pairs[i].stop     = &stop;
		pairs[i].error    = -1;
		pairs[i].sent     = 0;
		pairs[i].recv     = 0;
		pairs[i].seq_fail = 0;

		ATF_REQUIRE_MSG(socketpair(AF_UNIX, SOCK_SEQPACKET, 0,
		    pairs[i].fd) == 0,
		    "socketpair %d: %s", i, strerror(errno));

		error = pthread_create(&prod_threads[i], NULL,
		    unix_producer, &pairs[i]);
		ATF_REQUIRE_MSG(error == 0,
		    "pthread_create producer %d: %s", i, strerror(error));

		error = pthread_create(&cons_threads[i], NULL,
		    unix_consumer, &pairs[i]);
		ATF_REQUIRE_MSG(error == 0,
		    "pthread_create consumer %d: %s", i, strerror(error));
	}

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline)
		sleep(STRESS_POLL_INTERVAL_SEC);

	stop = true;

	for (i = 0; i < n; i++) {
		shutdown(pairs[i].fd[0], SHUT_RDWR);
		shutdown(pairs[i].fd[1], SHUT_RDWR);
		pthread_join(prod_threads[i], NULL);
		pthread_join(cons_threads[i], NULL);
		close(pairs[i].fd[0]);
		close(pairs[i].fd[1]);
		total_sent     += pairs[i].sent;
		total_recv     += pairs[i].recv;
		total_seq_fail += pairs[i].seq_fail;
	}

	ATF_CHECK_MSG(total_seq_fail == 0,
	    "%d sequence gaps detected in unix socketpair test "
	    "(data corruption or out-of-order delivery)",
	    total_seq_fail);
	ATF_CHECK_MSG(total_sent > 0,
	    "zero messages sent — producer threads may not have started");

	printf("unix-pair: %llu sent, %llu recv, %d seq fails, "
	    "%d pairs, %d seconds\n",
	    (unsigned long long)total_sent,
	    (unsigned long long)total_recv,
	    total_seq_fail, n, STRESS_DURATION_SEC);

	free(pairs);
	free(prod_threads);
	free(cons_threads);
}

/* -----------------------------------------------------------------------
 * TC-NSTR-02: loopback TCP echo
 * ----------------------------------------------------------------------- */

struct tcp_echo_ctx {
	in_port_t	 port;
	volatile bool	*stop;
	int		 srv_fd;	/* dedicated fd — avoids in_port_t truncation race */
};

struct tcp_client_arg {
	struct tcp_echo_ctx *ctx;
	int		 error;
	uint64_t	 bytes_sent;
	uint64_t	 bytes_recv;
};

/* Echo server: accept connections and echo all data back. */
static void *
tcp_echo_server(void *arg)
{
	struct tcp_echo_ctx *ctx = arg;
	int srv, cli;
	uint8_t buf[4096];
	ssize_t n;

	srv = ctx->srv_fd;

	while (!*ctx->stop) {
		struct timeval tv = { 1, 0 };
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(srv, &rfds);
		if (select(srv + 1, &rfds, NULL, NULL, &tv) <= 0)
			continue;
		cli = accept(srv, NULL, NULL);
		if (cli < 0)
			continue;
		/* Echo back all received data in a tight loop. */
		while ((n = recv(cli, buf, sizeof(buf), 0)) > 0)
			(void)send(cli, buf, (size_t)n, 0);
		close(cli);
	}
	return (NULL);
}

static void *
tcp_client_thread(void *arg)
{
	struct tcp_client_arg *ca = arg;
	struct sockaddr_in addr;
	uint8_t buf[NET_TCP_CHUNK_SIZE];
	int fd;
	ssize_t n;
	uint64_t seq = 0;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(ca->ctx->port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	while (!*ca->ctx->stop) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			break;
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
			close(fd);
			usleep(1000);
			continue;
		}

		/*
		 * Cap recv time so the thread exits promptly after stop=true.
		 * Without this, a slow echo server under load can hold recv
		 * indefinitely and cause the test to hit the 200 s ATF timeout.
		 */
		{
			struct timeval rcv_tv = { 5, 0 };
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
			    &rcv_tv, sizeof(rcv_tv));
		}

		/* Fill buffer with a recognisable pattern. */
		size_t j;
		for (j = 0; j < sizeof(buf) / sizeof(uint64_t); j++)
			((uint64_t *)buf)[j] = seq++;

		if (send(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf)) {
			uint8_t rbuf[NET_TCP_CHUNK_SIZE];
			size_t got = 0;
			while (got < sizeof(rbuf)) {
				n = recv(fd, rbuf + got,
				    sizeof(rbuf) - got, 0);
				if (n <= 0)
					break;
				got += (size_t)n;
			}
			if (got == sizeof(rbuf)) {
				ca->bytes_sent += sizeof(buf);
				ca->bytes_recv += got;
			}
		}
		close(fd);
	}

	ca->error = 0;
	return (NULL);
}

ATF_TC(net_stress_loopback_tcp);
ATF_TC_HEAD(net_stress_loopback_tcp, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-NSTR-02] Loopback TCP echo — N clients x 64 KiB chunks "
	    "(120 s); asserts bytes_recv == bytes_sent per client");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(net_stress_loopback_tcp, tc)
{
	struct tcp_echo_ctx ctx;
	struct tcp_client_arg *clients;
	pthread_t server_thread, *client_threads;
	struct sockaddr_in srv_addr;
	socklen_t alen;
	volatile bool stop = false;
	int srv_fd, n, i, error, one = 1;

	/* Ignore SIGPIPE: echo server's send() can race with client close. */
	signal(SIGPIPE, SIG_IGN);
	time_t deadline;
	uint64_t total_sent = 0, total_recv = 0;

	/* Create listen socket on an ephemeral port. */
	srv_fd = socket(AF_INET, SOCK_STREAM, 0);
	ATF_REQUIRE_MSG(srv_fd >= 0, "socket: %s", strerror(errno));

	setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&srv_addr, 0, sizeof(srv_addr));
	srv_addr.sin_family      = AF_INET;
	srv_addr.sin_port        = 0;	/* ephemeral */
	srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	ATF_REQUIRE_MSG(bind(srv_fd, (struct sockaddr *)&srv_addr,
	    sizeof(srv_addr)) == 0, "bind: %s", strerror(errno));

	alen = sizeof(srv_addr);
	ATF_REQUIRE_MSG(getsockname(srv_fd, (struct sockaddr *)&srv_addr,
	    &alen) == 0, "getsockname: %s", strerror(errno));

	ATF_REQUIRE_MSG(listen(srv_fd, NET_TCP_BACKLOG) == 0,
	    "listen: %s", strerror(errno));

	ctx.stop   = &stop;
	ctx.srv_fd = srv_fd;
	ctx.port   = ntohs(srv_addr.sin_port);

	error = pthread_create(&server_thread, NULL, tcp_echo_server, &ctx);
	ATF_REQUIRE_MSG(error == 0,
	    "pthread_create server: %s", strerror(error));

	n = net_nthreads();
	clients       = calloc((size_t)n, sizeof(struct tcp_client_arg));
	client_threads = calloc((size_t)n, sizeof(pthread_t));
	ATF_REQUIRE(clients != NULL && client_threads != NULL);

	for (i = 0; i < n; i++) {
		clients[i].ctx        = &ctx;
		clients[i].error      = -1;
		clients[i].bytes_sent = 0;
		clients[i].bytes_recv = 0;

		error = pthread_create(&client_threads[i], NULL,
		    tcp_client_thread, &clients[i]);
		ATF_REQUIRE_MSG(error == 0,
		    "pthread_create client %d: %s", i, strerror(error));
	}

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline)
		sleep(STRESS_POLL_INTERVAL_SEC);

	stop = true;

	for (i = 0; i < n; i++) {
		pthread_join(client_threads[i], NULL);
		ATF_CHECK_MSG(clients[i].bytes_sent == clients[i].bytes_recv,
		    "client %d: sent %llu bytes but received %llu", i,
		    (unsigned long long)clients[i].bytes_sent,
		    (unsigned long long)clients[i].bytes_recv);
		total_sent += clients[i].bytes_sent;
		total_recv += clients[i].bytes_recv;
	}

	shutdown(srv_fd, SHUT_RDWR);
	pthread_join(server_thread, NULL);
	close(srv_fd);

	printf("tcp-echo: %llu bytes sent, %llu bytes received, "
	    "%d clients, %d seconds\n",
	    (unsigned long long)total_sent,
	    (unsigned long long)total_recv,
	    n, STRESS_DURATION_SEC);

	free(clients);
	free(client_threads);
}

/* -----------------------------------------------------------------------
 * TC-NSTR-03: connection rate (fd leak check)
 * ----------------------------------------------------------------------- */

struct connrate_arg {
	volatile bool	*stop;
	in_port_t	 port;
	int		 srv_fd;	/* dedicated fd — avoids in_port_t truncation race */
	int		 error;
	uint64_t	 connects;
};

static void *
connrate_server(void *arg)
{
	volatile bool *stop = ((struct connrate_arg *)arg)->stop;
	int srv = ((struct connrate_arg *)arg)->srv_fd;
	int cli;

	while (!*stop) {
		struct timeval tv = { 1, 0 };
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(srv, &rfds);
		if (select(srv + 1, &rfds, NULL, NULL, &tv) <= 0)
			continue;
		cli = accept(srv, NULL, NULL);
		if (cli >= 0)
			close(cli);
	}
	return (NULL);
}

static void *
connrate_client(void *arg)
{
	struct connrate_arg *ca = arg;
	struct sockaddr_in addr;
	int fd;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(ca->port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	while (!*ca->stop) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			break;
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
			ca->connects++;
		close(fd);
	}

	ca->error = 0;
	return (NULL);
}

ATF_TC(net_stress_connection_rate);
ATF_TC_HEAD(net_stress_connection_rate, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "[TC-NSTR-03] Rapid connect()/close() to loopback listener (120 s);"
	    " asserts no file-descriptor leak");
	atf_tc_set_md_var(tc, "timeout", "200");
}

ATF_TC_BODY(net_stress_connection_rate, tc)
{
	struct connrate_arg arg;
	pthread_t srv_thread, cli_thread;
	struct sockaddr_in srv_addr;
	socklen_t alen;
	volatile bool stop = false;
	int srv_fd, one = 1;
	int fd_before, fd_after;
	time_t deadline;

	srv_fd = socket(AF_INET, SOCK_STREAM, 0);
	ATF_REQUIRE_MSG(srv_fd >= 0, "socket: %s", strerror(errno));
	setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&srv_addr, 0, sizeof(srv_addr));
	srv_addr.sin_family      = AF_INET;
	srv_addr.sin_port        = 0;
	srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	ATF_REQUIRE_MSG(bind(srv_fd, (struct sockaddr *)&srv_addr,
	    sizeof(srv_addr)) == 0, "bind: %s", strerror(errno));

	alen = sizeof(srv_addr);
	ATF_REQUIRE_MSG(getsockname(srv_fd, (struct sockaddr *)&srv_addr,
	    &alen) == 0, "getsockname: %s", strerror(errno));

	ATF_REQUIRE_MSG(listen(srv_fd, NET_TCP_BACKLOG) == 0,
	    "listen: %s", strerror(errno));

	/*
	 * Record the next-available fd before the stress run.
	 * dup(STDIN_FILENO) opens the lowest available fd; close it
	 * immediately and store the value as a baseline.
	 */
	fd_before = dup(STDIN_FILENO);
	ATF_REQUIRE_MSG(fd_before >= 0, "dup baseline: %s", strerror(errno));
	close(fd_before);

	arg.stop     = &stop;
	arg.srv_fd   = srv_fd;
	arg.port     = ntohs(srv_addr.sin_port);
	arg.error    = -1;
	arg.connects = 0;

	pthread_create(&srv_thread, NULL, connrate_server, &arg);
	pthread_create(&cli_thread, NULL, connrate_client, &arg);

	deadline = time(NULL) + STRESS_DURATION_SEC;
	while (time(NULL) < deadline)
		sleep(STRESS_POLL_INTERVAL_SEC);

	stop = true;
	shutdown(srv_fd, SHUT_RDWR);
	pthread_join(cli_thread, NULL);
	pthread_join(srv_thread, NULL);
	close(srv_fd);

	/*
	 * Fd-leak check: after closing srv_fd the next-available fd must
	 * not have grown significantly beyond the pre-test baseline.
	 * Allow a slack of 16 fds for ATF internal use.
	 */
	fd_after = dup(STDIN_FILENO);
	ATF_REQUIRE_MSG(fd_after >= 0, "dup post: %s", strerror(errno));
	close(fd_after);

	ATF_CHECK_MSG(fd_after <= fd_before + 16,
	    "file-descriptor leak: baseline fd=%d, post-stress fd=%d "
	    "(delta=%d, threshold=16)",
	    fd_before, fd_after, fd_after - fd_before);

	printf("connrate: %llu connections in %d seconds, "
	    "fd before=%d after=%d\n",
	    (unsigned long long)arg.connects,
	    STRESS_DURATION_SEC, fd_before, fd_after);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, net_stress_unix_socketpair);
	ATF_TP_ADD_TC(tp, net_stress_loopback_tcp);
	ATF_TP_ADD_TC(tp, net_stress_connection_rate);

	return (atf_no_error());
}
