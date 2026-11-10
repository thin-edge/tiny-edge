/* SPDX-License-Identifier: Apache-2.0
 *
 * Cloud remote access: the device bridges a Cumulocity session to a TCP
 * target, which may be the device itself or another host on its network.
 *
 *   530,<serial>,<host>,<port>,<key>   (on s/ds)
 *     -> resolve (".local" names over mDNS), retrying a lookup that times
 *        out, then check the target policy and the application's hook
 *     -> take a seat (TEDGE_REMOTE_ACCESS_MAX_SESSIONS)
 *     -> TCP to <host>:<port>
 *     -> wss://<tenant>/service/remoteaccess/device/<key>
 *        (Authorization: Bearer <token>, Sec-WebSocket-Protocol: binary)
 *     -> copy bytes both ways until either side closes or it goes idle
 *
 * Each session runs on its own thread: the bridge blocks in poll(), and the
 * client thread owns the MQTT session. Results travel back through a message
 * queue, because the MQTT session belongs to the client thread alone.
 *
 * The connection key is a bearer credential for the session: it is never
 * logged, at any level.
 */

#include "tedge_http.h"
#include "tedge_internal.h"
#include "tedge_util.h"
#include "tedge_ws.h"

#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>
#include <tedge/port/time.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

TEDGE_LOG_DECLARE();

#define MAX_SESSIONS TEDGE_REMOTE_ACCESS_MAX_SESSIONS
#define BUF_SIZE     TEDGE_REMOTE_ACCESS_BUF_SIZE
#define TELNET_PORT  23
/* How long the bridge waits when nothing is moving. */
#define IDLE_POLL_MS 1000
/* And while bytes are flowing: straight back for more. Pacing this was
 * tried against the allocation failures a saturating session provokes and
 * made them worse, not better. What a tunnel actually runs out of is
 * receive buffers on the board — see
 * docs/debugging-against-real-devices.md — so the answer is the port's
 * network buffers (on Zephyr NET_BUF_DATA_SIZE and NET_BUF_RX_COUNT), not
 * a throttle here. */
#define ACTIVE_POLL_MS 0
/* The cloud side: TLS and the WebSocket upgrade together. */
#define CLOUD_TIMEOUT_MS 15000

#define RESOLVE_TIMEOUT_MS  TEDGE_REMOTE_ACCESS_RESOLVE_TIMEOUT_MS
#define RESOLVE_ATTEMPTS    TEDGE_REMOTE_ACCESS_RESOLVE_ATTEMPTS
#define RESOLVE_RETRY_MS    TEDGE_REMOTE_ACCESS_RESOLVE_RETRY_DELAY_MS
/* The seat is held while a name resolves; keep the worst case bounded. */
_Static_assert(RESOLVE_ATTEMPTS * RESOLVE_TIMEOUT_MS +
		       (RESOLVE_ATTEMPTS - 1) * RESOLVE_RETRY_MS <= 30000,
	       "remote-access name resolution could hold a seat for over 30 s");

/* "255.255.255.255" and its NUL. */
#define ADDR_STR_LEN 16

struct session {
	char host[64];
	char addr_s[ADDR_STR_LEN]; /* what host resolved to */
	uint16_t port;
	char key[48]; /* the connection key; never logged */
	tedge_port_atomic_t busy;
	time_t since; /* wall clock while the tunnel is up, else 0 */
	tedge_port_thread_t thread;
	struct tedge_ws ws;
	uint8_t to_ws[BUF_SIZE];
	uint8_t to_target[BUF_SIZE];
	uint8_t ws_rx[1024];
};

static struct session sessions[MAX_SESSIONS];
TEDGE_PORT_STACK_ARRAY_DEFINE(session_stacks, MAX_SESSIONS,
			      TEDGE_REMOTE_ACCESS_STACK_SIZE);

TEDGE_PORT_MSGQ_DEFINE(ra_events, sizeof(struct tedge_ra_event), MAX_SESSIONS * 2);

static void post(enum tedge_ra_event_type type, const char *fmt, ...)
{
	struct tedge_ra_event ev = { .type = type };
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
	va_end(ap);
	(void)tedge_port_msgq_put(&ra_events, &ev);
}

int tedge_ra_poll_event(struct tedge_ra_event *ev)
{
	return tedge_port_msgq_get(&ra_events, ev);
}

static void addr_str(const uint8_t addr[4], char *out, size_t len)
{
	snprintf(out, len, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
}

/* ------------------------------------------------------------------------ */
/* Target policy                                                             */
/* ------------------------------------------------------------------------ */

/* Returns 0 when the target may be contacted, or -EACCES with a reason.
 * @p target is the target as events name it. */
static int policy_check(const uint8_t addr[4], const char *host,
			const char *target, uint16_t port, char *reason,
			size_t rlen)
{
	const struct tedge_hooks *hooks = tedge_hook_table();
	enum tedge_ra_policy policy =
		TEDGE_IS_ENABLED(TEDGE_REMOTE_ACCESS_TARGETS_LOCAL)
			? TEDGE_RA_POLICY_LOCAL
		: TEDGE_IS_ENABLED(TEDGE_REMOTE_ACCESS_TARGETS_LIST)
			? TEDGE_RA_POLICY_LIST
			: TEDGE_RA_POLICY_LAN;
	struct tedge_ra_facts facts = {
		.loopback = addr[0] == 127,
		.own = tedge_port_net_is_own(addr),
		.on_subnet = tedge_port_net_on_subnet(addr),
		.mdns = tedge_ra_is_mdns_name(host),
	};

#if defined(TEDGE_REMOTE_ACCESS_TARGETS_LIST)
	facts.in_list = tedge_ra_in_allow_list(
		TEDGE_REMOTE_ACCESS_ALLOW_LIST, host, port);
#endif

	switch (tedge_ra_policy_decide(policy, &facts)) {
	case TEDGE_RA_ALLOWED:
		break;
	case TEDGE_RA_REFUSED_OFF_LINK:
		snprintf(reason, rlen,
			 "target %s refused: an mDNS answer must be on this "
			 "device's network", target);
		return -EACCES;
	default:
		snprintf(reason, rlen, "target %s refused: %s", target,
			 policy == TEDGE_RA_POLICY_LOCAL ? "this device only"
			 : policy == TEDGE_RA_POLICY_LIST
				 ? "not in the allow-list"
				 : "not on this device's network");
		return -EACCES;
	}

	/* The application may narrow the policy, never widen it. */
	if (hooks != NULL && hooks->remote_access_allow != NULL) {
		struct tedge_remote_target t = { .host = host, .port = port };

		memcpy(t.ipv4, addr, sizeof(t.ipv4));
		if (!hooks->remote_access_allow(&t, hooks->user_data)) {
			snprintf(reason, rlen,
				 "target %s refused by the application",
				 target);
			return -EACCES;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Resolving the target                                                      */
/* ------------------------------------------------------------------------ */

#if defined(TEDGE_REMOTE_ACCESS_MDNS)
/* One mDNS lookup of s->host: 0 with @p addr, or a negative errno
 * (-ETIMEDOUT when nobody answered).
 *
 * Not the platform's resolver: Zephyr's (4.4) has an mDNS server only when
 * the application sets static DNS servers, and when the application also
 * runs the mDNS responder it sends from a socket it never reads, while
 * responders answer such a query to the port it came from (RFC 6762 6.7).
 * So the query goes out from a socket of our own, which reads the answer
 * itself: the same "legacy unicast" exchange, whatever else is running. */
static int mdns_once(struct session *s, uint8_t addr[4])
{
	static const uint8_t group[4] = { 224, 0, 0, 251 };
	uint16_t id = (uint16_t)tedge_port_random32();
	int64_t deadline = tedge_port_uptime_ms() + RESOLVE_TIMEOUT_MS;
	uint8_t buf[512];
	int conn, n, status = -ETIMEDOUT;

	n = tedge_mdns_build_query(buf, sizeof(buf), id, s->host);
	if (n < 0) {
		return -ENOENT;
	}
	conn = tedge_port_udp_open();
	if (conn < 0) {
		return -EAGAIN;
	}
	if (tedge_port_udp_sendto(conn, buf, n, group, 5353) < 0) {
		tedge_port_close(conn);
		return -EAGAIN;
	}

	/* Every responder that knows the name answers; take the first one
	 * that answers this query, and ignore anything else. */
	for (int64_t left; (left = deadline - tedge_port_uptime_ms()) > 0;) {
		n = tedge_port_recv(conn, buf, sizeof(buf), (uint32_t)left);
		if (n == -EAGAIN) {
			break;
		}
		if (n <= 0) {
			continue;
		}
		if (tedge_mdns_parse_a(buf, n, id, s->host, addr) == 0) {
			status = 0;
			break;
		}
	}
	tedge_port_close(conn);
	return status;
}
#endif

/* One lookup: 0 with @p addr, or a negative errno. */
static int resolve_once(struct session *s, uint8_t addr[4])
{
#if defined(TEDGE_REMOTE_ACCESS_MDNS)
	if (tedge_ra_is_mdns_name(s->host)) {
		return mdns_once(s, addr);
	}
#endif
	return tedge_port_resolve(s->host, addr, RESOLVE_TIMEOUT_MS);
}

/* Resolve s->host into @p addr, retrying what is worth retrying. Returns 0,
 * or -EHOSTUNREACH with the reason in @p reason. */
static int resolve_target(struct session *s, uint8_t addr[4], char *reason,
			  size_t rlen)
{
	bool mdns = tedge_ra_is_mdns_name(s->host);
	int status = -ENOENT;
	int attempt;

	if (tedge_port_ipv4_parse(s->host, addr)) {
		s->addr_s[0] = '\0';
		return 0;
	}
	if (mdns && !TEDGE_IS_ENABLED(TEDGE_REMOTE_ACCESS_MDNS)) {
		snprintf(reason, rlen, "cannot resolve %s: mDNS is not built in",
			 s->host);
		return -EHOSTUNREACH;
	}

	for (attempt = 1; attempt <= RESOLVE_ATTEMPTS; attempt++) {
		if (attempt > 1) {
			tedge_port_sleep_ms(RESOLVE_RETRY_MS);
		}
		status = resolve_once(s, addr);
		if (status == 0) {
			addr_str(addr, s->addr_s, sizeof(s->addr_s));
			return 0;
		}
		TEDGE_LOG_WRN("remote access: lookup %d/%d of %s failed (%d)",
			      attempt, RESOLVE_ATTEMPTS, s->host, status);
		if (!tedge_ra_resolve_retryable(status)) {
			break;
		}
	}
	attempt = TEDGE_MIN(attempt, RESOLVE_ATTEMPTS);

	if (tedge_ra_resolve_retryable(status)) {
		snprintf(reason, rlen, "%s did not answer%s after %d attempt%s",
			 s->host, mdns ? " over mDNS" : "", attempt,
			 attempt == 1 ? "" : "s");
	} else {
		snprintf(reason, rlen, "cannot resolve %s after %d attempt%s (%d)",
			 s->host, attempt, attempt == 1 ? "" : "s", status);
	}
	return -EHOSTUNREACH;
}

/* ------------------------------------------------------------------------ */
/* Connections                                                               */
/* ------------------------------------------------------------------------ */

static int tcp_connect(const uint8_t addr[4], uint16_t port)
{
	int conn = tedge_port_connect(addr, port, TEDGE_PORT_TCP, NULL,
				      CLOUD_TIMEOUT_MS);

	if (conn < 0) {
		return conn;
	}
	/* An interactive session is a stream of small writes — keystrokes one
	 * way, a shell's or a TUI's output the other. Nagle holds each of
	 * them until the previous segment is acknowledged, and against a peer
	 * doing delayed ACK that is tens to hundreds of milliseconds per
	 * exchange. Line-at-a-time output survives it; anything that repaints
	 * a screen does not. Best effort: a transport that does not support
	 * it is no worse off than before. */
	(void)tedge_port_set_nodelay(conn);
	return conn;
}

/* The device-side WebSocket. Cumulocity refuses the client certificate alone
 * (401), so this needs the token the client keeps for the tenant.
 * Returns the connection, or a negative errno.
 */
static int wss_connect(struct session *s)
{
	const char *host = tedge_c8y_host();
	const char *token = tedge_c8y_jwt();
	const char *headers[3] = { 0 };
	uint8_t addr[4];
	char auth[1100];
	char path[128];
	int conn, ret, h = 0;

	if (token == NULL || token[0] == '\0') {
		return -EACCES;
	}
	ret = tedge_port_resolve(host, addr, RESOLVE_TIMEOUT_MS);
	if (ret != 0) {
		return -EHOSTUNREACH;
	}
	conn = tedge_port_connect(addr, 443, TEDGE_PORT_TLS_DEVICE, host,
				  CLOUD_TIMEOUT_MS);
	if (conn < 0) {
		TEDGE_LOG_ERR("remote access: TLS to %s failed (%d); on Zephyr, "
			      "NET_SOCKETS_TLS_MAX_CONTEXTS may be too low",
			      host, conn);
		return conn;
	}
	(void)tedge_port_set_nodelay(conn);

	snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", token);
	headers[h++] = auth;
	headers[h++] = "Sec-WebSocket-Protocol: binary\r\n";
	snprintf(path, sizeof(path), "/service/remoteaccess/device/%s", s->key);

	ret = tedge_ws_connect(&s->ws, conn, host, path, headers, s->ws_rx,
			       sizeof(s->ws_rx), CLOUD_TIMEOUT_MS);
	memset(auth, 0, sizeof(auth));
	memset(path, 0, sizeof(path));
	if (ret < 0) {
		TEDGE_LOG_ERR("remote access: the cloud refused the WebSocket (%d)",
			      ret);
		tedge_port_close(conn);
		return ret;
	}
	return conn;
}

/* ------------------------------------------------------------------------ */
/* The bridge                                                                */
/* ------------------------------------------------------------------------ */

static void bridge(void *arg)
{
	struct session *s = arg;
	uint8_t addr[4];
	char reason[160], target[96];
	uint64_t up = 0, down = 0;
	int64_t t_up = 0, last_activity;
	int tcp = -1, ws = -1, ret;
	const char *why = "unknown";

	if (resolve_target(s, addr, reason, sizeof(reason)) != 0) {
		TEDGE_LOG_WRN("remote access: %s", reason);
		post(TEDGE_RA_FAILED, "%s", reason);
		goto out;
	}
	(void)tedge_ra_target_str(target, sizeof(target), s->host, s->addr_s,
				  s->port);

	/* The policy decides before any socket is opened. */
	if (policy_check(addr, s->host, target, s->port, reason,
			 sizeof(reason)) != 0) {
		TEDGE_LOG_WRN("%s", reason);
		post(TEDGE_RA_FAILED, "%s", reason);
		goto out;
	}

	tcp = tcp_connect(addr, s->port);
	if (tcp < 0) {
		post(TEDGE_RA_FAILED, "cannot reach %s (%d)", target, tcp);
		goto out;
	}
	ws = wss_connect(s);
	if (ws < 0) {
		post(TEDGE_RA_FAILED, "the cloud connection failed (%d)", ws);
		goto out;
	}

	{
		time_t now = 0;

		(void)tedge_port_time_now(&now);
		s->since = now;
	}
	t_up = tedge_port_uptime_ms();
	TEDGE_LOG_INF("remote access: tunnel to %s is up", target);
	post(TEDGE_RA_UP, TEDGE_RA_OPENED_FMT, target);

	/* Zephyr's telnet backend turns echo off and never offers it, and the
	 * cloud's terminal waits for the server: without this nobody echoes. */
	if (s->port == TELNET_PORT) {
		static const uint8_t offer[] = { 0xFF, 0xFB, 0x01,
						 0xFF, 0xFB, 0x03 };

		(void)tedge_ws_send(&s->ws, offer, sizeof(offer));
	}

	last_activity = tedge_port_uptime_ms();
	/* While bytes are flowing, come straight back for more instead of
	 * waiting on the next poll: a second between buffers caps the tunnel
	 * at one BUF_SIZE per poll (~2 KB/s at the default 2048), which is
	 * enough for a command and its output but not for anything that
	 * repaints a screen. The long wait is only for an idle session; a
	 * busy one paces at ACTIVE_POLL_MS instead. */
	for (int poll_ms = IDLE_POLL_MS;;) {
		struct tedge_port_pollfd fds[2] = {
			{ .conn = tcp, .events = TEDGE_PORT_POLLIN },
			{ .conn = ws, .events = TEDGE_PORT_POLLIN },
		};
		/* Frames already read off the connection do not make it
		 * readable again. */
		bool buffered = s->ws.rx_off < s->ws.rx_len;
		bool moved = false;

		ret = tedge_port_poll(fds, 2, buffered ? 0 : (uint32_t)poll_ms);
		if (ret < 0) {
			why = "poll error";
			break;
		}
		if (fds[0].revents & TEDGE_PORT_POLLIN) {
			int n = tedge_port_recv(tcp, s->to_ws, BUF_SIZE, 0);

			if (n == 0 || (n < 0 && n != -EAGAIN)) {
				why = (n == 0) ? "the target closed"
					       : "the target failed";
				break;
			}
			if (n > 0) {
				if (tedge_ws_send(&s->ws, s->to_ws, n) < 0) {
					why = "sending to the cloud failed";
					break;
				}
				up += n;
				moved = true;
				last_activity = tedge_port_uptime_ms();
			}
		}
		if (buffered || (fds[1].revents & TEDGE_PORT_POLLIN)) {
			bool closed = false;

			for (;;) {
				int n = tedge_ws_recv(&s->ws, s->to_target,
						      BUF_SIZE, 0);

				if (n == -EAGAIN) {
					break;
				}
				if (n < 0) {
					closed = true;
					why = (n == -ENOTCONN)
						      ? "the client closed the session"
						      : "the cloud connection dropped";
					break;
				}
				if (tedge_http_send_all(tcp, s->to_target, n) < 0) {
					closed = true;
					why = "sending to the target failed";
					break;
				}
				down += n;
				moved = true;
			}
			if (closed) {
				break;
			}
			if (moved) {
				last_activity = tedge_port_uptime_ms();
			}
		}
		poll_ms = moved ? ACTIVE_POLL_MS : IDLE_POLL_MS;
		/* A side that hung up with data still to read is not done:
		 * the data is relayed first, and the read that finds nothing
		 * left ends the session ("the target closed"). macOS reports
		 * the hang-up while the data is still queued, so a target
		 * that writes a large reply and closes at once lost all but
		 * the first buffer of it. */
		if (fds[0].revents & TEDGE_PORT_POLLIN) {
			fds[0].revents &= ~(TEDGE_PORT_POLLHUP | TEDGE_PORT_POLLERR);
		}
		if (fds[1].revents & TEDGE_PORT_POLLIN) {
			fds[1].revents &= ~(TEDGE_PORT_POLLHUP | TEDGE_PORT_POLLERR);
		}
		if ((fds[0].revents | fds[1].revents) &
		    (TEDGE_PORT_POLLHUP | TEDGE_PORT_POLLERR)) {
			why = (fds[0].revents &
			       (TEDGE_PORT_POLLHUP | TEDGE_PORT_POLLERR))
				      ? "the target hung up"
				      : "the cloud connection hung up";
			break;
		}
		if (TEDGE_REMOTE_ACCESS_IDLE_TIMEOUT_S > 0 &&
		    tedge_port_uptime_ms() - last_activity >
			    TEDGE_REMOTE_ACCESS_IDLE_TIMEOUT_S * 1000LL) {
			why = "idle timeout";
			break;
		}
	}

	{
		int64_t secs = TEDGE_MAX((tedge_port_uptime_ms() - t_up) / 1000, 1);

		TEDGE_LOG_INF("remote access: tunnel to %s ended (%s) after %lld s: "
			      "%llu B up, %llu B down", target, why,
			      (long long)secs, (unsigned long long)up,
			      (unsigned long long)down);
		/* uint64_t is unsigned long on a 64-bit host: cast for %llu. */
		post(TEDGE_RA_CLOSED, TEDGE_RA_CLOSED_FMT, target, why,
		     (unsigned long long)up, (unsigned long long)down);
	}
out:
	if (ws >= 0) {
		tedge_ws_close(&s->ws);
		tedge_port_close(ws);
	}
	tedge_port_close(tcp);
	s->since = 0;
	s->addr_s[0] = '\0';
	memset(s->key, 0, sizeof(s->key));
	(void)tedge_port_atomic_set(&s->busy, 0);
}

/* ------------------------------------------------------------------------ */
/* Requests and capacity                                                     */
/* ------------------------------------------------------------------------ */

int tedge_ra_request(const char *line, char *reason, size_t rlen)
{
	char host[64], port_s[8], key[48];
	struct session *s = NULL;
	int idx = 0;

	/* An operator can close the door on a device already in the field,
	 * through the "tedge" parameter set, without reflashing it. Checked
	 * before anything is parsed: a refusal should cost nothing. */
#if defined(TEDGE_PARAMETERS_SELF)
	if (!tedge_self_remote_access_allowed()) {
		snprintf(reason, rlen,
			 "remote access is turned off on this device");
		TEDGE_LOG_WRN("remote access: refused, turned off by parameter");
		return -EACCES;
	}
#endif

	/* 530,<serial>,<host>,<port>,<connectionKey> */
	if (tedge_sr_field(line, 2, host, sizeof(host)) <= 0 ||
	    tedge_sr_field(line, 3, port_s, sizeof(port_s)) <= 0 ||
	    tedge_sr_field(line, 4, key, sizeof(key)) <= 0) {
		snprintf(reason, rlen, "malformed remote-access request");
		return -EINVAL;
	}
	TEDGE_LOG_INF("remote access: request for %s:%s", host, port_s);

	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (tedge_port_atomic_cas(&sessions[i].busy, 0, 1)) {
			s = &sessions[i];
			idx = i;
			break;
		}
	}
	if (s == NULL) {
		snprintf(reason, rlen, "no free session (the limit is %d)",
			 MAX_SESSIONS);
		return -EBUSY;
	}

	snprintf(s->host, sizeof(s->host), "%s", host);
	s->addr_s[0] = '\0';
	s->port = (uint16_t)strtoul(port_s, NULL, 10);
	snprintf(s->key, sizeof(s->key), "%s", key);
	memset(key, 0, sizeof(key));

	(void)tedge_port_thread_start(&s->thread, session_stacks[idx],
				      TEDGE_PORT_STACK_SIZEOF(session_stacks[idx]),
				      bridge, s, "tedge_ra");
	return 0;
}

int tedge_ra_twin(char *buf, size_t len)
{
	int taken = 0, active = 0;
	const char *policy =
		TEDGE_IS_ENABLED(TEDGE_REMOTE_ACCESS_TARGETS_LOCAL)  ? "local"
		: TEDGE_IS_ENABLED(TEDGE_REMOTE_ACCESS_TARGETS_LIST) ? "list"
								      : "lan";
	int n;

	for (int i = 0; i < MAX_SESSIONS; i++) {
		taken += tedge_port_atomic_get(&sessions[i].busy) ? 1 : 0;
		active += sessions[i].since ? 1 : 0;
	}
	n = snprintf(buf, len,
		     "{\"maxSessions\":%d,\"activeSessions\":%d,"
		     "\"freeSessions\":%d,\"policy\":\"%s\"",
		     MAX_SESSIONS, active, MAX_SESSIONS - taken, policy);

#if defined(TEDGE_REMOTE_ACCESS_TWIN_SESSIONS)
	n += snprintf(buf + n, (n < (int)len) ? len - n : 0, ",\"sessions\":[");
	for (int i = 0, printed = 0; i < MAX_SESSIONS && n < (int)len; i++) {
		struct tm tm;
		char iso[24];
		time_t since = sessions[i].since;

		if (since == 0) {
			continue;
		}
		gmtime_r(&since, &tm);
		strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
		n += snprintf(buf + n, len - n,
			      "%s{\"target\":\"%s:%u\",\"since\":\"%s\"}",
			      printed++ ? "," : "", sessions[i].host,
			      sessions[i].port, iso);
	}
	if (n < (int)len) {
		n += snprintf(buf + n, len - n, "]");
	}
#endif
	if (n < (int)len) {
		n += snprintf(buf + n, len - n, "}");
	}
	return (n < (int)len) ? 0 : -ENOSPC;
}
