/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX port against its interface (include/tedge/port/): what the
 * core relies on from each service, run on the host. TLS is exercised by
 * the sample against a real tenant; here the plain transport is.
 */

#include <tedge/port/kv.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>
#include <tedge/posix.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
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

/* --- Kernel services ----------------------------------------------------- */

TEDGE_PORT_MSGQ_DEFINE(queue, sizeof(int), 2);
TEDGE_PORT_EVENT_DEFINE(events);
TEDGE_PORT_MUTEX_DEFINE(lock);
TEDGE_PORT_STACK_DEFINE(stack, 4096);

static void test_msgq(void)
{
	int a = 1, b = 2, c = 3, out = 0;

	CHECK(tedge_port_msgq_get(&queue, &out) != 0);
	CHECK(tedge_port_msgq_put(&queue, &a) == 0);
	CHECK(tedge_port_msgq_put(&queue, &b) == 0);
	CHECK(tedge_port_msgq_put(&queue, &c) != 0); /* full */
	CHECK(tedge_port_msgq_get(&queue, &out) == 0 && out == 1);
	CHECK(tedge_port_msgq_put(&queue, &c) == 0); /* wraps */
	CHECK(tedge_port_msgq_get(&queue, &out) == 0 && out == 2);
	CHECK(tedge_port_msgq_get(&queue, &out) == 0 && out == 3);
	CHECK(tedge_port_msgq_get(&queue, &out) != 0);
}

static void poster(void *arg)
{
	tedge_port_sleep_ms(50);
	tedge_port_event_post(&events, (uint32_t)(uintptr_t)arg);
}

static void test_events_and_threads(void)
{
	tedge_port_thread_t thread = { 0 };
	int64_t t0;

	CHECK(tedge_port_event_wait(&events, 0x1, 30) == 0); /* times out */
	t0 = tedge_port_uptime_ms();
	CHECK(tedge_port_thread_start(&thread, stack, TEDGE_PORT_STACK_SIZEOF(stack),
				      poster, (void *)(uintptr_t)0x6, "poster") == 0);
	CHECK(tedge_port_thread_join(&thread, 0) == -EAGAIN); /* still running */
	CHECK(tedge_port_event_wait(&events, 0x2, 2000) == 0x2);
	CHECK(tedge_port_uptime_ms() - t0 >= 40);
	CHECK(tedge_port_event_test(&events, 0xff) == 0x6); /* flags stay set */
	tedge_port_event_clear(&events, 0x4);
	CHECK(tedge_port_event_test(&events, 0xff) == 0x2);
	CHECK(tedge_port_thread_join(&thread, 2000) == 0);

	/* The same thread object, again. */
	CHECK(tedge_port_thread_start(&thread, stack, TEDGE_PORT_STACK_SIZEOF(stack),
				      poster, (void *)(uintptr_t)0x8, "poster") == 0);
	CHECK(tedge_port_event_wait(&events, 0x8, 2000) == 0x8);
	CHECK(tedge_port_thread_join(&thread, 2000) == 0);
}

static void test_sem(void)
{
	tedge_port_sem_t sem;

	tedge_port_sem_init(&sem, 0, 1);
	CHECK(tedge_port_sem_take(&sem, 20) == -EAGAIN);
	tedge_port_sem_give(&sem);
	tedge_port_sem_give(&sem); /* at its limit already */
	CHECK(tedge_port_sem_take(&sem, 0) == 0);
	CHECK(tedge_port_sem_take(&sem, 0) == -EAGAIN);
	tedge_port_sem_give(&sem);
	tedge_port_sem_reset(&sem);
	CHECK(tedge_port_sem_take(&sem, 0) == -EAGAIN);
}

static int work_runs;
static int64_t work_ran_at;

static void work_fn(void)
{
	tedge_port_mutex_lock(&lock);
	work_runs++;
	work_ran_at = tedge_port_uptime_ms();
	tedge_port_mutex_unlock(&lock);
}

TEDGE_PORT_DELAYED_WORK_DEFINE(work, work_fn);
TEDGE_PORT_DELAYED_WORK_DEFINE(cancelled, work_fn);

static void test_delayed_work(void)
{
	int64_t t0 = tedge_port_uptime_ms();

	tedge_port_delayed_work_schedule(&cancelled, 50);
	tedge_port_delayed_work_schedule(&work, 500);
	tedge_port_delayed_work_schedule(&work, 100); /* rescheduled earlier */
	tedge_port_delayed_work_cancel(&cancelled);
	tedge_port_sleep_ms(400);
	tedge_port_mutex_lock(&lock);
	CHECK(work_runs == 1);
	CHECK(work_ran_at - t0 >= 90 && work_ran_at - t0 < 400);
	tedge_port_mutex_unlock(&lock);
}

static void test_atomics_and_heap(void)
{
	tedge_port_atomic_t a = TEDGE_PORT_ATOMIC_INIT(5);
	size_t before = tedge_port_heap_free_bytes();
	void *p;

	CHECK(tedge_port_atomic_get(&a) == 5);
	CHECK(tedge_port_atomic_set(&a, 7) == 5);
	CHECK(!tedge_port_atomic_cas(&a, 5, 9));
	CHECK(tedge_port_atomic_cas(&a, 7, 9) && tedge_port_atomic_get(&a) == 9);

	p = tedge_port_heap_alloc(100);
	CHECK(p != NULL);
	CHECK(tedge_port_heap_free_bytes() == before - 100);
	CHECK(tedge_port_heap_alloc(before) == NULL); /* over the allowance */
	tedge_port_heap_free(p);
	tedge_port_heap_free(NULL);
	CHECK(tedge_port_heap_free_bytes() == before);
}

/* --- Storage ------------------------------------------------------------- */

static char seen[8][64];
static int nseen;

static void collect(const char *subkey, void *ctx)
{
	(void)ctx;
	if (nseen < 8) {
		snprintf(seen[nseen++], sizeof(seen[0]), "%s", subkey);
	}
}

static bool was_seen(const char *subkey)
{
	for (int i = 0; i < nseen; i++) {
		if (strcmp(seen[i], subkey) == 0) {
			return true;
		}
	}
	return false;
}

static void test_kv(void)
{
	char buf[16];

	CHECK(tedge_port_kv_init() == 0);
	CHECK(tedge_port_kv_get("tedge/c8y/url", buf, sizeof(buf)) == -ENOENT);
	CHECK(tedge_port_kv_set("tedge/c8y/url", "example.com", 11) == 0);
	CHECK(tedge_port_kv_get("tedge/c8y/url", buf, sizeof(buf)) == 11);
	CHECK(memcmp(buf, "example.com", 11) == 0);
	CHECK(tedge_port_kv_get("tedge/c8y/url", buf, 4) == -ENOSPC);
	CHECK(tedge_port_kv_set("tedge/c8y/url", "\x00\x01", 2) == 0); /* binary */
	CHECK(tedge_port_kv_get("tedge/c8y/url", buf, sizeof(buf)) == 2);

	/* A key that is a value and a level above others. */
	CHECK(tedge_port_kv_set("tedge/param", "x", 1) == 0);
	CHECK(tedge_port_kv_set("tedge/param/modbus/unit_id", "1", 1) == 0);
	CHECK(tedge_port_kv_set("tedge/param/modbus/baud", "2", 1) == 0);
	CHECK(tedge_port_kv_set("tedge/param/self/health", "3", 1) == 0);
	CHECK(tedge_port_kv_foreach("tedge/param", collect, NULL) == 0);
	CHECK(nseen == 3);
	CHECK(was_seen("modbus/unit_id") && was_seen("modbus/baud") &&
	      was_seen("self/health"));

	CHECK(tedge_port_kv_delete("tedge/param/modbus/baud") == 0);
	CHECK(tedge_port_kv_delete("tedge/param/modbus/baud") == 0); /* gone */
	nseen = 0;
	CHECK(tedge_port_kv_foreach("tedge/param", collect, NULL) == 0);
	CHECK(nseen == 2);
	nseen = 0;
	CHECK(tedge_port_kv_foreach("tedge/none", collect, NULL) == 0);
	CHECK(nseen == 0);
	CHECK(tedge_port_kv_get("tedge/param", buf, sizeof(buf)) == 1);

	/* Nothing outside the store. */
	CHECK(tedge_port_kv_set("tedge/../escape", "x", 1) == -EINVAL);
	CHECK(tedge_port_kv_set("/abs", "x", 1) == -EINVAL);
	CHECK(tedge_port_kv_set("tedge//double", "x", 1) == -EINVAL);
}

/* --- Network ------------------------------------------------------------- */

static void test_net(void)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t len = sizeof(sa);
	uint8_t lo[4] = { 127, 0, 0, 1 };
	uint8_t addr[4];
	struct tedge_port_pollfd pfd;
	char buf[16];
	int srv, peer, conn;

	CHECK(tedge_port_ipv4_parse("192.168.1.20", addr) && addr[0] == 192 &&
	      addr[3] == 20);
	CHECK(!tedge_port_ipv4_parse("example.com", addr));
	CHECK(tedge_port_resolve("10.0.0.1", addr, 1000) == 0 && addr[0] == 10);
	CHECK(tedge_port_resolve("localhost", addr, 1000) == 0 && addr[0] == 127);
	CHECK(tedge_port_resolve("no-such-host.invalid", addr, 1000) < 0);
	CHECK(tedge_port_net_is_own(lo));

	srv = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(listen(srv, 1) == 0);
	CHECK(getsockname(srv, (struct sockaddr *)&sa, &len) == 0);

	conn = tedge_port_connect(lo, ntohs(sa.sin_port), TEDGE_PORT_TCP, NULL,
				  2000);
	CHECK(conn >= 0);
	peer = accept(srv, NULL, NULL);
	CHECK(peer >= 0);
	CHECK(tedge_port_set_nodelay(conn) == 0);

	CHECK(tedge_port_recv(conn, buf, sizeof(buf), 0) == -EAGAIN);
	CHECK(tedge_port_recv(conn, buf, sizeof(buf), 50) == -EAGAIN);
	pfd = (struct tedge_port_pollfd){ .conn = conn, .events = TEDGE_PORT_POLLIN };
	CHECK(tedge_port_poll(&pfd, 1, 20) == 0);

	CHECK(write(peer, "hello", 5) == 5);
	CHECK(tedge_port_poll(&pfd, 1, 1000) == 1 &&
	      (pfd.revents & TEDGE_PORT_POLLIN));
	CHECK(tedge_port_recv(conn, buf, sizeof(buf), 1000) == 5);
	CHECK(memcmp(buf, "hello", 5) == 0);

	CHECK(tedge_port_send(conn, "ping", 4) == 4);
	CHECK(read(peer, buf, sizeof(buf)) == 4);

	close(peer);
	CHECK(tedge_port_recv(conn, buf, sizeof(buf), 1000) == 0); /* closed */
	tedge_port_close(conn);
	tedge_port_close(-1);
	CHECK(tedge_port_send(conn, "x", 1) == -EBADF);

	/* Nobody listening: refused, not a hang. */
	close(srv);
	CHECK(tedge_port_connect(lo, ntohs(sa.sin_port), TEDGE_PORT_TCP, NULL,
				 2000) == -ECONNREFUSED);
}

int main(int argc, char **argv)
{
	char dir[] = "/tmp/tedge-port-test-XXXXXX";
	struct tedge_posix_config cfg = { 0 };

	(void)argc;
	(void)argv;
	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	cfg.state_dir = dir;
	CHECK(tedge_posix_configure(&cfg) == 0);
	CHECK(tedge_posix_configure(NULL) == -EINVAL);

	test_msgq();
	test_events_and_threads();
	test_sem();
	test_delayed_work();
	test_atomics_and_heap();
	test_kv();
	test_net();

	{
		char rm[64];

		snprintf(rm, sizeof(rm), "rm -rf %s", dir);
		(void)!system(rm);
	}
	if (failures == 0) {
		printf("all port checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
