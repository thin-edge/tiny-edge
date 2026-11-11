/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX side of <tedge/port/net.h>: the interface queries, from the
 * interface list (getifaddrs()). The connections and TLS are shared with
 * other ports (ports/common/src/tedge_sockets.c).
 */

#include <tedge/port/net.h>
#include <tedge/port/os.h>

#include "tedge_common.h"
#include "tedge_posix_internal.h"

#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>

/* Every socket the client opens is closed in the child of an exec (a
 * restart), and never raises SIGPIPE: MSG_NOSIGNAL where send() has it,
 * SO_NOSIGPIPE on the socket where it does not (macOS). */
void tedge_common_socket_opened(int fd)
{
	tedge_posix_cloexec(fd);
#if defined(SO_NOSIGPIPE)
	{
		int on = 1;

		(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
	}
#endif
}

/* --- Connectivity -------------------------------------------------------- */

/* Calls @p fn for each IPv4 address of the system's interfaces; with
 * @p only_ours, only those of the interface the client uses. Stops when
 * @p fn returns true. @return what the last call returned. */
static bool each_ipv4(bool only_ours,
		      bool (*fn)(const struct sockaddr_in *addr,
				 const struct sockaddr_in *mask,
				 unsigned int flags, const void *ctx),
		      const void *ctx)
{
	char name[IF_NAMESIZE];
	struct ifaddrs *list, *ifa;
	bool found = false;

	if (only_ours && tedge_posix_interface(name) != 0) {
		return false;
	}
	if (getifaddrs(&list) != 0) {
		return false;
	}
	for (ifa = list; ifa != NULL && !found; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET ||
		    (only_ours && strcmp(ifa->ifa_name, name) != 0)) {
			continue;
		}
		found = fn((const struct sockaddr_in *)(const void *)ifa->ifa_addr,
			   (const struct sockaddr_in *)(const void *)ifa->ifa_netmask,
			   ifa->ifa_flags, ctx);
	}
	freeifaddrs(list);
	return found;
}

static bool is_up_fn(const struct sockaddr_in *addr,
		     const struct sockaddr_in *mask, unsigned int flags,
		     const void *ctx)
{
	(void)addr;
	(void)mask;
	(void)ctx;
	return (flags & IFF_UP) && (flags & IFF_RUNNING);
}

bool tedge_port_net_is_up(void)
{
	return each_ipv4(true, is_up_fn, NULL);
}

/* The system announces no change in a way every POSIX system shares
 * (netlink, route sockets, SystemConfiguration), so a thread looks every
 * few seconds and reports a change. */
#define WATCH_INTERVAL_MS 2000

static void (*net_cb)(bool up);

static void *watch_thread(void *arg)
{
	bool up = tedge_port_net_is_up();

	(void)arg;
	for (;;) {
		bool now;

		tedge_port_sleep_ms(WATCH_INTERVAL_MS);
		now = tedge_port_net_is_up();
		if (now != up) {
			up = now;
			net_cb(up);
		}
	}
	return NULL;
}

void tedge_port_net_watch(void (*cb)(bool up))
{
	pthread_t id;

	net_cb = cb;
	if (pthread_create(&id, NULL, watch_thread, NULL) == 0) {
		(void)pthread_detach(id);
	}
}

static bool same_addr_fn(const struct sockaddr_in *addr,
			 const struct sockaddr_in *mask, unsigned int flags,
			 const void *ctx)
{
	(void)mask;
	(void)flags;
	return memcmp(&addr->sin_addr, ctx, 4) == 0;
}

bool tedge_port_net_is_own(const uint8_t addr[4])
{
	return each_ipv4(false, same_addr_fn, addr);
}

static bool subnet_fn(const struct sockaddr_in *addr,
		      const struct sockaddr_in *mask, unsigned int flags,
		      const void *ctx)
{
	uint32_t a, m, t;

	(void)flags;
	if (mask == NULL) {
		return false;
	}
	memcpy(&a, &addr->sin_addr, 4);
	memcpy(&m, &mask->sin_addr, 4);
	memcpy(&t, ctx, 4);
	return (a & m) == (t & m);
}

bool tedge_port_net_on_subnet(const uint8_t addr[4])
{
	return each_ipv4(true, subnet_fn, addr);
}
