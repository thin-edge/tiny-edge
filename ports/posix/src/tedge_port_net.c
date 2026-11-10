/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX side of <tedge/port/net.h>: BSD sockets, TLS with mbedTLS,
 * the system's resolver, and the interface list (getifaddrs()).
 *
 * A connection handle is a slot in a table of the port's own, not the
 * socket itself: a TLS connection carries its mbedTLS session beside the
 * socket. Each slot is used by one thread at a time (the interface's
 * rule), so only taking and freeing slots needs the table's lock.
 *
 * Every socket is non-blocking, and every wait is a poll() with the time
 * left, so a timeout holds whether TLS or the kernel is waiting.
 */

#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>

#include "tedge_posix_internal.h"
#include "tedge_posix_tls.h"

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

TEDGE_LOG_DECLARE();

/* More than the client holds at once: MQTT, a download or an upload,
 * enrollment, and the remote-access sessions with their targets. */
#define MAX_CONNS 16
/* How long a send may wait for room in the socket before the connection is
 * given up as dead. */
#define SEND_TIMEOUT_MS 30000

#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0 /* SO_NOSIGPIPE on the socket instead (macOS) */
#endif

struct conn {
	bool used;
	bool tls;
	int fd;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	mbedtls_x509_crt own;
	mbedtls_pk_context key;
};

static struct conn conns[MAX_CONNS];
static pthread_mutex_t conns_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct conn *conn_get(int handle)
{
	if (handle < 0 || handle >= MAX_CONNS || !conns[handle].used) {
		return NULL;
	}
	return &conns[handle];
}

static int conn_alloc(int fd, bool tls)
{
	int handle = -ENFILE;

	(void)pthread_mutex_lock(&conns_lock);
	for (int i = 0; i < MAX_CONNS; i++) {
		if (!conns[i].used) {
			conns[i].used = true;
			conns[i].tls = tls;
			conns[i].fd = fd;
			if (tls) {
				/* Before anything can fail, so conn_free()
				 * always frees initialised state. */
				mbedtls_ssl_init(&conns[i].ssl);
				mbedtls_ssl_config_init(&conns[i].conf);
				mbedtls_x509_crt_init(&conns[i].ca);
				mbedtls_x509_crt_init(&conns[i].own);
				mbedtls_pk_init(&conns[i].key);
			}
			handle = i;
			break;
		}
	}
	(void)pthread_mutex_unlock(&conns_lock);
	return handle;
}

static void conn_free(struct conn *c)
{
	if (c->tls) {
		mbedtls_ssl_free(&c->ssl);
		mbedtls_ssl_config_free(&c->conf);
		mbedtls_x509_crt_free(&c->ca);
		mbedtls_x509_crt_free(&c->own);
		mbedtls_pk_free(&c->key);
	}
	if (c->fd >= 0) {
		(void)close(c->fd);
	}
	(void)pthread_mutex_lock(&conns_lock);
	c->used = false;
	c->tls = false;
	c->fd = -1;
	(void)pthread_mutex_unlock(&conns_lock);
}

/* A socket as every connection here has it: non-blocking, closed on exec,
 * and never raising SIGPIPE. */
static int socket_open(int type)
{
	int fd = socket(AF_INET, type, 0);

	if (fd < 0) {
		return -errno;
	}
	tedge_posix_cloexec(fd);
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#if defined(SO_NOSIGPIPE)
	{
		int on = 1;

		(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
	}
#endif
	return fd;
}

/* Wait until @p fd is ready for @p events or @p deadline passes.
 * @return 0 when ready, -EAGAIN at the deadline, or a negative errno. */
static int wait_fd(int fd, short events, int64_t deadline)
{
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = events };
		int64_t left = deadline - now_ms();
		int ret;

		if (left <= 0) {
			return -EAGAIN;
		}
		ret = poll(&pfd, 1, (int)left);
		if (ret > 0) {
			return 0;
		}
		if (ret == 0) {
			return -EAGAIN;
		}
		if (errno != EINTR) {
			return -errno;
		}
	}
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

/* --- Names --------------------------------------------------------------- */

bool tedge_port_ipv4_parse(const char *text, uint8_t addr[4])
{
	struct in_addr in;

	if (inet_pton(AF_INET, text, &in) != 1) {
		return false;
	}
	memcpy(addr, &in, 4);
	return true;
}

/* getaddrinfo(), which follows the system's configuration (resolv.conf,
 * nsswitch.conf, mDNS on macOS). It cannot be cancelled, so the timeout is
 * the resolver's own (resolv.conf's "timeout" and "attempts"), not
 * @p timeout_ms. */
int tedge_port_resolve(const char *host, uint8_t addr[4], uint32_t timeout_ms)
{
	struct addrinfo hints = { .ai_family = AF_INET,
				  .ai_socktype = SOCK_STREAM };
	struct addrinfo *res;
	int ret;

	(void)timeout_ms;
	if (tedge_port_ipv4_parse(host, addr)) {
		return 0;
	}
	ret = getaddrinfo(host, NULL, &hints, &res);
	switch (ret) {
	case 0:
		memcpy(addr, &((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr,
		       4);
		freeaddrinfo(res);
		return 0;
	case EAI_AGAIN: /* the servers did not answer */
		return -ETIMEDOUT;
	case EAI_MEMORY:
	case EAI_SYSTEM:
		return -EAGAIN;
	default: /* EAI_NONAME, EAI_FAIL, ...: the next try says the same */
		return -ENOENT;
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

/* --- TLS credentials ----------------------------------------------------- */

/* The buffers stay the caller's (the interface's rule). Each TLS
 * connection parses what is set when it opens, so a change of credentials
 * never pulls them from under a session. */
static pthread_mutex_t cred_lock = PTHREAD_MUTEX_INITIALIZER;
static const void *ca_pem;
static size_t ca_len;
static const void *device_cert;
static size_t device_cert_len;

int tedge_port_tls_set_ca(const void *pem, size_t len)
{
	(void)pthread_mutex_lock(&cred_lock);
	ca_pem = pem;
	ca_len = len;
	(void)pthread_mutex_unlock(&cred_lock);
	return 0;
}

int tedge_port_tls_set_device(const void *cert, size_t cert_len)
{
	(void)pthread_mutex_lock(&cred_lock);
	device_cert = cert;
	device_cert_len = cert_len;
	(void)pthread_mutex_unlock(&cred_lock);
	return 0;
}

void tedge_port_tls_clear_device(void)
{
	(void)pthread_mutex_lock(&cred_lock);
	device_cert = NULL;
	device_cert_len = 0;
	(void)pthread_mutex_unlock(&cred_lock);
}

/* --- TLS over a socket --------------------------------------------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	struct conn *c = ctx;
	ssize_t n = send(c->fd, buf, len, SEND_FLAGS);

	if (n >= 0) {
		return (int)n;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	}
	return (errno == EPIPE || errno == ECONNRESET) ? MBEDTLS_ERR_NET_CONN_RESET
						       : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	struct conn *c = ctx;
	ssize_t n = recv(c->fd, buf, len, 0);

	if (n >= 0) {
		return (int)n;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return MBEDTLS_ERR_SSL_WANT_READ;
	}
	return (errno == ECONNRESET) ? MBEDTLS_ERR_NET_CONN_RESET
				     : MBEDTLS_ERR_NET_RECV_FAILED;
}

static int tls_setup(struct conn *c, enum tedge_port_security security,
		     const char *hostname)
{
	const char *what = "CA bundle";
	int ret;

	ret = tedge_posix_psa_init();
	if (ret != 0) {
		return ret;
	}
	(void)pthread_mutex_lock(&cred_lock);
	ret = (ca_pem != NULL)
		      ? mbedtls_x509_crt_parse(&c->ca, ca_pem, ca_len)
		      : MBEDTLS_ERR_X509_BAD_INPUT_DATA;
	if (ret == 0 && security == TEDGE_PORT_TLS_DEVICE) {
		what = "device certificate";
		ret = (device_cert != NULL)
			      ? mbedtls_x509_crt_parse_der(&c->own, device_cert,
							   device_cert_len)
			      : MBEDTLS_ERR_X509_BAD_INPUT_DATA;
	}
	(void)pthread_mutex_unlock(&cred_lock);
	if (ret != 0) {
		TEDGE_LOG_ERR("TLS: the %s is missing or not valid (-0x%04x)",
			      what, (unsigned int)-ret);
		return -EINVAL;
	}
	if (security == TEDGE_PORT_TLS_DEVICE) {
		ret = tedge_posix_device_pk(&c->key);
		if (ret != 0) {
			return ret;
		}
	}

	ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
					  MBEDTLS_SSL_TRANSPORT_STREAM,
					  MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) {
		return -ENOMEM;
	}
	/* TLS 1.2, as the Zephyr port connects. */
	mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
	if (security == TEDGE_PORT_TLS_DEVICE &&
	    mbedtls_ssl_conf_own_cert(&c->conf, &c->own, &c->key) != 0) {
		return -ENOMEM;
	}
	if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0 ||
	    mbedtls_ssl_set_hostname(&c->ssl, hostname) != 0) {
		return -ENOMEM;
	}
	mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
	return 0;
}

static int tls_handshake(struct conn *c, const char *hostname, int64_t deadline)
{
	for (;;) {
		int ret = mbedtls_ssl_handshake(&c->ssl);

		if (ret == 0) {
			return 0;
		}
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			ret = wait_fd(c->fd,
				      (ret == MBEDTLS_ERR_SSL_WANT_READ) ? POLLIN
									 : POLLOUT,
				      deadline);
			if (ret == -EAGAIN) {
				TEDGE_LOG_WRN("TLS: %s: the handshake timed out",
					      hostname);
				return -ETIMEDOUT;
			}
			if (ret != 0) {
				return ret;
			}
			continue;
		}
		if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			char info[160];

			(void)mbedtls_x509_crt_verify_info(
				info, sizeof(info), "",
				mbedtls_ssl_get_verify_result(&c->ssl));
			info[strcspn(info, "\n")] = '\0';
			TEDGE_LOG_ERR("TLS: %s: the server's certificate is not "
				      "trusted: %s", hostname, info);
			return -EACCES;
		}
		TEDGE_LOG_WRN("TLS: %s: the handshake failed (-0x%04x)", hostname,
			      (unsigned int)-ret);
		return -ECONNABORTED;
	}
}

/* --- Connections --------------------------------------------------------- */

int tedge_port_connect(const uint8_t addr[4], uint16_t port,
		       enum tedge_port_security security, const char *hostname,
		       uint32_t timeout_ms)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_port = htons(port) };
	int64_t deadline = now_ms() + timeout_ms;
	bool tls = security != TEDGE_PORT_TCP;
	struct conn *c;
	int fd, handle, ret;

	memcpy(&sa.sin_addr, addr, 4);
	fd = socket_open(SOCK_STREAM);
	if (fd < 0) {
		return fd;
	}
	handle = conn_alloc(fd, tls);
	if (handle < 0) {
		(void)close(fd);
		return handle;
	}
	c = &conns[handle];

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		if (errno != EINPROGRESS) {
			ret = -errno;
			goto fail;
		}
		ret = wait_fd(fd, POLLOUT, deadline);
		if (ret != 0) {
			ret = (ret == -EAGAIN) ? -ETIMEDOUT : ret;
			goto fail;
		}
		{
			int err = 0;
			socklen_t len = sizeof(err);

			(void)getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
			if (err != 0) {
				ret = -err;
				goto fail;
			}
		}
	}
	if (tls) {
		ret = tls_setup(c, security, hostname);
		if (ret == 0) {
			ret = tls_handshake(c, hostname, deadline);
		}
		if (ret != 0) {
			goto fail;
		}
	}
	return handle;

fail:
	conn_free(c);
	return ret;
}

int tedge_port_udp_open(void)
{
	int fd = socket_open(SOCK_DGRAM);
	int handle;

	if (fd < 0) {
		return fd;
	}
	handle = conn_alloc(fd, false);
	if (handle < 0) {
		(void)close(fd);
	}
	return handle;
}

int tedge_port_udp_sendto(int conn, const void *buf, size_t len,
			  const uint8_t addr[4], uint16_t port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_port = htons(port) };
	struct conn *c = conn_get(conn);

	if (c == NULL) {
		return -EBADF;
	}
	memcpy(&sa.sin_addr, addr, 4);
	return (sendto(c->fd, buf, len, SEND_FLAGS, (struct sockaddr *)&sa,
		       sizeof(sa)) < 0) ? -errno : 0;
}

int tedge_port_set_nodelay(int conn)
{
	struct conn *c = conn_get(conn);
	int on = 1;

	if (c == NULL) {
		return -EBADF;
	}
	return (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0)
		       ? -errno : 0;
}

int tedge_port_send(int conn, const void *buf, size_t len)
{
	int64_t deadline = now_ms() + SEND_TIMEOUT_MS;
	struct conn *c = conn_get(conn);

	if (c == NULL) {
		return -EBADF;
	}
	for (;;) {
		short want = POLLOUT;
		int ret;

		if (c->tls) {
			ret = mbedtls_ssl_write(&c->ssl, buf, len);
			if (ret >= 0) {
				return ret;
			}
			if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
				want = POLLIN;
			} else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
				return -ECONNRESET;
			}
		} else {
			ssize_t n = send(c->fd, buf, len, SEND_FLAGS);

			if (n >= 0) {
				return (int)n;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR) {
				return -errno;
			}
		}
		ret = wait_fd(c->fd, want, deadline);
		if (ret != 0) {
			return (ret == -EAGAIN) ? -ETIMEDOUT : ret;
		}
	}
}

int tedge_port_recv(int conn, void *buf, size_t len, uint32_t timeout_ms)
{
	int64_t deadline = now_ms() + timeout_ms;
	struct conn *c = conn_get(conn);

	if (c == NULL) {
		return -EBADF;
	}
	for (;;) {
		short want = POLLIN;
		int ret;

		if (c->tls) {
			ret = mbedtls_ssl_read(&c->ssl, buf, len);
			if (ret >= 0) {
				return ret; /* 0: the peer closed */
			}
			if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
				return 0;
			}
			if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
				want = POLLOUT;
			} else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
				return -ECONNRESET;
			}
		} else {
			ssize_t n = recv(c->fd, buf, len, 0);

			if (n >= 0) {
				return (int)n;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR) {
				return -errno;
			}
		}
		if (timeout_ms == 0) {
			return -EAGAIN;
		}
		ret = wait_fd(c->fd, want, deadline);
		if (ret != 0) {
			return ret;
		}
	}
}

int tedge_port_poll(struct tedge_port_pollfd *fds, size_t n,
		    uint32_t timeout_ms)
{
	struct pollfd pfds[8];
	int buffered = 0;
	int ret;

	if (n > sizeof(pfds) / sizeof(pfds[0])) {
		return -EINVAL;
	}
	for (size_t i = 0; i < n; i++) {
		struct conn *c = conn_get(fds[i].conn);

		fds[i].revents = 0;
		pfds[i].fd = (c != NULL) ? c->fd : -1;
		pfds[i].events = (fds[i].events & TEDGE_PORT_POLLIN) ? POLLIN : 0;
		pfds[i].revents = 0;
		/* TLS may hold a decrypted record the socket no longer shows. */
		if (c != NULL && c->tls && (fds[i].events & TEDGE_PORT_POLLIN) &&
		    mbedtls_ssl_get_bytes_avail(&c->ssl) > 0) {
			fds[i].revents = TEDGE_PORT_POLLIN;
			buffered++;
		}
	}
	do {
		ret = poll(pfds, n, buffered > 0 ? 0 : (int)timeout_ms);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		return -errno;
	}
	ret = 0;
	for (size_t i = 0; i < n; i++) {
		fds[i].revents |=
			((pfds[i].revents & POLLIN) ? TEDGE_PORT_POLLIN : 0) |
			((pfds[i].revents & (POLLERR | POLLNVAL)) ? TEDGE_PORT_POLLERR
								  : 0) |
			((pfds[i].revents & POLLHUP) ? TEDGE_PORT_POLLHUP : 0);
		if (fds[i].revents != 0) {
			ret++;
		}
	}
	return ret;
}

void tedge_port_close(int conn)
{
	struct conn *c = conn_get(conn);

	if (c == NULL) {
		return;
	}
	if (c->tls) {
		/* Best effort: the socket is non-blocking, and a peer that is
		 * gone does not hold the close up. */
		(void)mbedtls_ssl_close_notify(&c->ssl);
	}
	conn_free(c);
}
