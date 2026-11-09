/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr side of <tedge/port/net.h>: BSD sockets (zsock_*), TLS
 * sockets with credentials under the client's security tags, the DNS
 * resolver, and network management events.
 */

#include <tedge/port/net.h>

#include "tedge_port_zephyr.h"

#include <zephyr/kernel.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>

#include <errno.h>
#include <string.h>

/* --- Connectivity -------------------------------------------------------- */

static void (*net_cb)(bool up);
static struct net_mgmt_event_callback l4_cb;

static void l4_handler(struct net_mgmt_event_callback *cb, uint64_t event,
		       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (net_cb == NULL) {
		return;
	}
	if (event == NET_EVENT_L4_CONNECTED ||
	    event == NET_EVENT_IPV4_ADDR_ADD) { /* whichever the app provides */
		net_cb(true);
	} else if (event == NET_EVENT_L4_DISCONNECTED ||
		   event == NET_EVENT_IPV4_ADDR_DEL) {
		net_cb(false);
	}
}

bool tedge_port_net_is_up(void)
{
	struct net_if *iface = net_if_get_default();

	return iface != NULL &&
	       net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

void tedge_port_net_watch(void (*cb)(bool up))
{
	net_cb = cb;
	/* L4 events come from the connection manager when the application
	 * enables it; the IPv4 address events are always there. */
	net_mgmt_init_event_callback(&l4_cb, l4_handler,
				     NET_EVENT_IPV4_ADDR_ADD |
					     NET_EVENT_IPV4_ADDR_DEL |
					     (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)
						      ? NET_EVENT_L4_CONNECTED |
								NET_EVENT_L4_DISCONNECTED
						      : 0));
	net_mgmt_add_event_callback(&l4_cb);
}

/* --- Names --------------------------------------------------------------- */

bool tedge_port_ipv4_parse(const char *text, uint8_t addr[4])
{
	struct in_addr in;

	if (zsock_inet_pton(AF_INET, text, &in) != 1) {
		return false;
	}
	memcpy(addr, &in, 4);
	return true;
}

struct resolve_ctx {
	struct k_sem done;
	uint8_t addr[4];
	bool have_answer;
	int status;
};

static void resolve_cb(enum dns_resolve_status status,
		       struct dns_addrinfo *info, void *user_data)
{
	struct resolve_ctx *ctx = user_data;

	if (status == DNS_EAI_INPROGRESS && info != NULL) {
		if (!ctx->have_answer && info->ai_family == AF_INET) {
			memcpy(ctx->addr,
			       &((struct sockaddr_in *)&info->ai_addr)->sin_addr,
			       4);
			ctx->have_answer = true;
		}
		return;
	}
	ctx->status = (status == DNS_EAI_ALLDONE && ctx->have_answer)
			      ? 0 : (int)status;
	k_sem_give(&ctx->done);
}

static int resolve_errno(int status)
{
	switch (status) {
	case 0:
		return 0;
	case DNS_EAI_CANCELED: /* nobody answered in time */
		return -ETIMEDOUT;
	case DNS_EAI_AGAIN:
	case DNS_EAI_SYSTEM:   /* no socket or server to send on, yet */
	case DNS_EAI_MEMORY:
		return -EAGAIN;
	default:
		/* An authoritative "no such name" (DNS_EAI_NONAME, _NODATA,
		 * _FAIL) gives the same answer the next time. */
		return -ENOENT;
	}
}

/* Zephyr's asynchronous resolver rather than getaddrinfo(), so that a query
 * that never completes can be cancelled and cannot call back later. */
int tedge_port_resolve(const char *host, uint8_t addr[4], uint32_t timeout_ms)
{
	struct resolve_ctx ctx = { .status = DNS_EAI_CANCELED };
	uint16_t id = 0;
	int ret;

	if (tedge_port_ipv4_parse(host, addr)) {
		return 0;
	}
	k_sem_init(&ctx.done, 0, 1);
	ret = dns_get_addr_info(host, DNS_QUERY_TYPE_A, &id, resolve_cb, &ctx,
				timeout_ms);
	if (ret < 0) {
		/* An errno: no server to send to, or a query slot in use (one
		 * at a time by default). getaddrinfo() calls this a system
		 * error too. */
		return -EAGAIN;
	}
	/* The resolver times the query out itself and calls back with
	 * DNS_EAI_CANCELED; the margin covers a callback that never comes,
	 * and cancelling then makes sure none arrives later, into a context
	 * that is gone. */
	if (k_sem_take(&ctx.done, K_MSEC(timeout_ms + 500)) != 0) {
		(void)dns_cancel_addr_info_with_name(host, DNS_QUERY_TYPE_A, id);
		return -ETIMEDOUT;
	}
	if (ctx.status == 0) {
		memcpy(addr, ctx.addr, 4);
	}
	return resolve_errno(ctx.status);
}

bool tedge_port_net_is_own(const uint8_t addr[4])
{
	struct in_addr in;

	memcpy(&in, addr, 4);
	return net_if_ipv4_addr_lookup(&in, NULL) != NULL;
}

bool tedge_port_net_on_subnet(const uint8_t addr[4])
{
	struct net_if *iface = net_if_get_default();
	struct in_addr in;

	memcpy(&in, addr, 4);
	return iface != NULL && net_if_ipv4_addr_mask_cmp(iface, &in);
}

/* --- TLS credentials ----------------------------------------------------- */


int tedge_port_tls_set_ca(const void *pem, size_t len)
{
	(void)tls_credential_delete(TAG_SERVER_CA,
				    TLS_CREDENTIAL_CA_CERTIFICATE);
	return tls_credential_add(TAG_SERVER_CA, TLS_CREDENTIAL_CA_CERTIFICATE,
				  pem, len);
}

/* --- Connections --------------------------------------------------------- */

int tedge_port_connect(const uint8_t addr[4], uint16_t port,
		       enum tedge_port_security security, const char *hostname,
		       uint32_t timeout_ms)
{
	static const sec_tag_t tags_server[] = { TAG_SERVER_CA };
	static const sec_tag_t tags_device[] = { TAG_SERVER_CA, TAG_DEVICE };
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_port = htons(port) };
	bool tls = security != TEDGE_PORT_TCP;
	int fd, ret;

	/* Zephyr has no per-call connect timeout: TCP's own applies, and for
	 * TLS the handshake's budget is CONFIG_NET_SOCKETS_TLS_CONNECT_TIMEOUT.
	 * A host outside the tenant may present a chain that takes ~13 s to
	 * verify in software, which is why the README asks applications to
	 * raise it. */
	ARG_UNUSED(timeout_ms);
	memcpy(&sa.sin_addr, addr, 4);
	fd = zsock_socket(AF_INET, SOCK_STREAM,
			  tls ? IPPROTO_TLS_1_2 : IPPROTO_TCP);
	if (fd < 0) {
		return -errno;
	}
	if (tls) {
		const sec_tag_t *tags = (security == TEDGE_PORT_TLS_DEVICE)
						? tags_device : tags_server;
		size_t size = (security == TEDGE_PORT_TLS_DEVICE)
				      ? sizeof(tags_device) : sizeof(tags_server);

		(void)zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST,
				       tags, size);
		(void)zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME,
				       hostname, strlen(hostname) + 1);
	}
	ret = zsock_connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (ret < 0) {
		ret = -errno;
		zsock_close(fd);
		return ret;
	}
	return fd;
}

int tedge_port_udp_open(void)
{
	int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	return (fd < 0) ? -errno : fd;
}

int tedge_port_udp_sendto(int conn, const void *buf, size_t len,
			  const uint8_t addr[4], uint16_t port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_port = htons(port) };

	memcpy(&sa.sin_addr, addr, 4);
	return (zsock_sendto(conn, buf, len, 0, (struct sockaddr *)&sa,
			     sizeof(sa)) < 0) ? -errno : 0;
}

int tedge_port_set_nodelay(int conn)
{
	int on = 1;

	return (zsock_setsockopt(conn, IPPROTO_TCP, ZSOCK_TCP_NODELAY, &on,
				 sizeof(on)) < 0) ? -errno : 0;
}

int tedge_port_send(int conn, const void *buf, size_t len)
{
	ssize_t n = zsock_send(conn, buf, len, 0);

	return (n < 0) ? -errno : (int)n;
}

int tedge_port_recv(int conn, void *buf, size_t len, uint32_t timeout_ms)
{
	ssize_t n;

	if (timeout_ms > 0) {
		struct zsock_pollfd pfd = { .fd = conn, .events = ZSOCK_POLLIN };
		int ret = zsock_poll(&pfd, 1, (int)timeout_ms);

		if (ret < 0) {
			return -errno;
		}
		if (ret == 0) {
			return -EAGAIN;
		}
	}
	n = zsock_recv(conn, buf, len, ZSOCK_MSG_DONTWAIT);
	if (n < 0) {
		return (errno == EWOULDBLOCK) ? -EAGAIN : -errno;
	}
	return (int)n;
}

int tedge_port_poll(struct tedge_port_pollfd *fds, size_t n,
		    uint32_t timeout_ms)
{
	struct zsock_pollfd zfds[4];
	int ret;

	if (n > ARRAY_SIZE(zfds)) {
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i++) {
		zfds[i].fd = fds[i].conn;
		zfds[i].events = (fds[i].events & TEDGE_PORT_POLLIN)
					 ? ZSOCK_POLLIN : 0;
		zfds[i].revents = 0;
	}
	ret = zsock_poll(zfds, n, (int)timeout_ms);
	if (ret < 0) {
		return -errno;
	}
	for (size_t i = 0; i < n; i++) {
		fds[i].revents =
			((zfds[i].revents & ZSOCK_POLLIN) ? TEDGE_PORT_POLLIN : 0) |
			((zfds[i].revents & ZSOCK_POLLERR) ? TEDGE_PORT_POLLERR : 0) |
			((zfds[i].revents & ZSOCK_POLLHUP) ? TEDGE_PORT_POLLHUP : 0);
	}
	return ret;
}

void tedge_port_close(int conn)
{
	if (conn >= 0) {
		(void)zsock_close(conn);
	}
}
