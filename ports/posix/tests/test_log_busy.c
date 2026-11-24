/* SPDX-License-Identifier: Apache-2.0
 *
 * The HTTP client keeps its buffers in static storage, so only one transfer
 * may run at a time. Firmware update and log upload each refuse a request
 * while the other's job runs, and both ask the same question:
 * tedge_log_busy() and tedge_fw_busy(). The POSIX port has no firmware
 * update, so this test holds a log job open and checks the log side of that
 * question: busy from the request until the job ends, and free afterwards.
 *
 * The tenant is a socket on 127.0.0.1 that accepts the connection and never
 * answers, so the log job sits in its first request until the test closes
 * the connection.
 */

#include <tedge/port/os.h>

#include "tedge_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,     \
				#cond);                                        \
			failures++;                                            \
		}                                                              \
	} while (0)

/* A listening socket on 127.0.0.1; its port goes to @p port. */
static int listen_local(uint16_t *port)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	socklen_t len = sizeof(addr);
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		return -1;
	}
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(fd, 1) != 0 ||
	    getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
		close(fd);
		return -1;
	}
	*port = ntohs(addr.sin_port);
	return fd;
}

/* The log job's result, waiting up to 20 s. */
static int wait_event(struct tedge_log_event *ev)
{
	for (int i = 0; i < 2000; i++) {
		if (tedge_log_poll_event(ev) == 0) {
			return 0;
		}
		tedge_port_sleep_ms(10);
	}
	return -ETIMEDOUT;
}

int main(void)
{
	char reason[128] = "";
	char host[32];
	uint16_t port = 0;
	struct pollfd pfd;
	struct tedge_log_event ev;
	int lfd, conn = -1;

	lfd = listen_local(&port);
	if (lfd < 0) {
		perror("listen");
		return 1;
	}
	snprintf(host, sizeof(host), "127.0.0.1:%u", (unsigned int)port);
	/* Without a storage directory the host is not persisted; it is set. */
	(void)tedge_c8y_host_set(host);
	tedge_log_upload_init();

	CHECK(!tedge_log_busy());
	CHECK(tedge_log_request("522,dev,tedge-log", reason, sizeof(reason)) ==
	      0);
	CHECK(tedge_log_busy());

	/* The job is in its first request: the HTTP client is in use. */
	pfd = (struct pollfd){ .fd = lfd, .events = POLLIN };
	CHECK(poll(&pfd, 1, 5000) == 1);
	conn = accept(lfd, NULL, NULL);
	CHECK(conn >= 0);
	CHECK(tedge_log_busy());

	/* A second log while the first runs: refused, the first goes on. */
	CHECK(tedge_log_request("522,dev,tedge-log", reason, sizeof(reason)) ==
	      -EBUSY);
	CHECK(tedge_log_busy());

	/* The tenant hangs up: the job fails and gives the client back. */
	if (conn >= 0) {
		close(conn);
	}
	CHECK(wait_event(&ev) == 0);
	CHECK(ev.rc != 0);
	for (int i = 0; i < 100 && tedge_log_busy(); i++) {
		tedge_port_sleep_ms(10);
	}
	CHECK(!tedge_log_busy());
	close(lfd);

	if (failures == 0) {
		printf("log busy: ok\n");
	}
	return failures == 0 ? 0 : 1;
}
