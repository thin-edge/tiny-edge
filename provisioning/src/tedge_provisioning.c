/* SPDX-License-Identifier: Apache-2.0
 *
 * The RPC layer: Improv's commands and tiny-edge's own, the same on every
 * transport. Packets are queued by the transport and handled on one
 * worker thread, since joining a network or restarting the client takes
 * seconds and must not hold up the console.
 *
 * Portable: the public tedge_* API and the port interface only.
 */

#include "tedge_provisioning_internal.h"

#include <tedge/port/config.h>
#include <tedge/port/device.h>
#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>
#include <tedge/tedge.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

TEDGE_LOG_DECLARE();

/* After the application's hook has joined the network, how long to wait
 * for an address. */
#define ADDRESS_WAIT_MS 15000

#define EV_RPC (1u << 0)

#define STR_(x) #x
#define STR(x) STR_(x)

struct rpc_msg {
	const struct rpc_out *out;
	uint8_t len;
	uint8_t data[RPC_MAX];
};

TEDGE_PORT_MSGQ_DEFINE(tedge_prov_queue, sizeof(struct rpc_msg), 2);
TEDGE_PORT_EVENT_DEFINE(prov_events);
TEDGE_PORT_STACK_DEFINE(prov_stack, TEDGE_PROVISIONING_STACK_SIZE);
static tedge_port_thread_t prov_thread;

static const struct tedge_provisioning_hooks *hooks;
static uint8_t improv_state = IMPROV_STATE_READY;

/* --- Results --------------------------------------------------------------- */

/* A result being built: [command][length][strings...]. */
struct result {
	uint8_t buf[2 + 255];
	size_t len;
	bool overflow;
};

static void result_start(struct result *r, uint8_t cmd)
{
	r->buf[0] = cmd;
	r->len = 2;
	r->overflow = false;
}

static void result_add(struct result *r, const char *s)
{
	size_t n = strlen(s);

	if (n > 255 || r->len + 1 + n > sizeof(r->buf)) {
		r->overflow = true;
		return;
	}
	r->buf[r->len++] = (uint8_t)n;
	memcpy(&r->buf[r->len], s, n);
	r->len += n;
}

static void result_send(struct result *r, const struct rpc_out *out)
{
	if (r->overflow) {
		out->error(IMPROV_ERR_UNKNOWN);
		return;
	}
	r->buf[1] = (uint8_t)(r->len - 2);
	out->result(r->buf, r->len);
}

static void send_empty_result(uint8_t cmd, const struct rpc_out *out)
{
	struct result r;

	result_start(&r, cmd);
	result_send(&r, out);
}

static void set_state(uint8_t state, const struct rpc_out *out)
{
	improv_state = state;
	out->state(state);
}

/* --- Reading a packet ---------------------------------------------------- */

/* A length-prefixed string at @p *pos of @p data (length @p len), copied
 * NUL-terminated into @p out (room for @p cap bytes with the NUL).
 * @return false when it does not fit or runs past the packet. */
static bool take_string(const uint8_t *data, size_t len, size_t *pos,
			char *out, size_t cap)
{
	size_t n;

	if (*pos >= len) {
		return false;
	}
	n = data[(*pos)++];
	if (n >= cap || *pos + n > len) {
		return false;
	}
	memcpy(out, &data[*pos], n);
	out[n] = '\0';
	*pos += n;
	/* A NUL inside would cut the string short where it is used. */
	return strlen(out) == n;
}

/* --- Improv's commands --------------------------------------------------- */

static void wifi_settings(const uint8_t *data, size_t len,
			  const struct rpc_out *out)
{
	char ssid[33], password[65];
	size_t pos = 0;
	int rc;

	if (!take_string(data, len, &pos, ssid, sizeof(ssid)) || ssid[0] == '\0' ||
	    !take_string(data, len, &pos, password, sizeof(password)) ||
	    pos != len) {
		out->error(IMPROV_ERR_INVALID_RPC);
		return;
	}
	if (hooks == NULL || hooks->wifi_connect == NULL) {
		TEDGE_LOG_WRN("provisioning: Wi-Fi settings received, but this "
			      "application cannot join a network");
		out->error(IMPROV_ERR_UNABLE_CONNECT);
		return;
	}

	TEDGE_LOG_INF("provisioning: joining Wi-Fi \"%s\"", ssid);
	set_state(IMPROV_STATE_PROVISIONING, out);
	rc = hooks->wifi_connect(ssid, password, hooks->ctx);
	memset(password, 0, sizeof(password));
	if (rc == 0) {
		int64_t until = tedge_port_uptime_ms() + ADDRESS_WAIT_MS;

		while (!tedge_port_net_is_up() && tedge_port_uptime_ms() < until) {
			tedge_port_sleep_ms(250);
		}
		if (!tedge_port_net_is_up()) {
			rc = -ETIMEDOUT;
		}
	}
	if (rc != 0) {
		TEDGE_LOG_WRN("provisioning: could not join \"%s\" (%d)", ssid, rc);
		out->error(IMPROV_ERR_UNABLE_CONNECT);
		set_state(IMPROV_STATE_READY, out);
		return;
	}
	TEDGE_LOG_INF("provisioning: joined \"%s\"", ssid);
	set_state(IMPROV_STATE_PROVISIONED, out);
	/* Improv's result after provisioning: URLs to open next. The device
	 * does not know its page in Cumulocity, so there are none. */
	send_empty_result(IMPROV_CMD_WIFI_SETTINGS, out);
}

static void current_state(const struct rpc_out *out)
{
	if (improv_state != IMPROV_STATE_PROVISIONING) {
		improv_state = tedge_port_net_is_up() ? IMPROV_STATE_PROVISIONED
						      : IMPROV_STATE_READY;
	}
	out->state(improv_state);
	if (improv_state == IMPROV_STATE_PROVISIONED) {
		send_empty_result(IMPROV_CMD_CURRENT_STATE, out);
	}
}

static void device_info(const struct rpc_out *out)
{
	char id[65];
	struct result r;

	if (tedge_get_device_id(id, sizeof(id)) < 0) {
		id[0] = '\0';
	}
	result_start(&r, IMPROV_CMD_DEVICE_INFO);
	result_add(&r, tedge_port_firmware_name());
	result_add(&r, tedge_port_firmware_version());
	result_add(&r, (hooks != NULL && hooks->chip != NULL) ? hooks->chip
							      : "unknown");
	result_add(&r, id);
	result_send(&r, out);
}

/* The most networks one scan reports. */
#define SCAN_MAX 16

/* Improv's scan answer: one result per network, [SSID, RSSI, "YES" or "NO"
 * for a password], strongest first and each SSID once, then a result with
 * no strings to end the list. */
static void wifi_networks(const struct rpc_out *out)
{
	static struct tedge_provisioning_network found[SCAN_MAX];
	struct result r;
	int n;

	if (hooks == NULL || hooks->wifi_scan == NULL) {
		out->error(IMPROV_ERR_UNKNOWN_RPC);
		return;
	}
	n = hooks->wifi_scan(found, SCAN_MAX, hooks->ctx);
	if (n < 0) {
		TEDGE_LOG_WRN("provisioning: Wi-Fi scan failed (%d)", n);
		out->error(IMPROV_ERR_UNKNOWN);
		return;
	}
	if (n > SCAN_MAX) {
		n = SCAN_MAX;
	}
	/* Strongest first (a few entries: insertion sort). */
	for (int i = 1; i < n; i++) {
		struct tedge_provisioning_network t = found[i];
		int j = i - 1;

		while (j >= 0 && found[j].rssi < t.rssi) {
			found[j + 1] = found[j];
			j--;
		}
		found[j + 1] = t;
	}
	for (int i = 0; i < n; i++) {
		char rssi[8];
		bool seen = false;

		found[i].ssid[sizeof(found[i].ssid) - 1] = '\0';
		if (found[i].ssid[0] == '\0') {
			continue; /* hidden network */
		}
		for (int j = 0; j < i && !seen; j++) {
			seen = strcmp(found[j].ssid, found[i].ssid) == 0;
		}
		if (seen) {
			continue; /* another access point of a network already sent */
		}
		snprintf(rssi, sizeof(rssi), "%d", found[i].rssi);
		result_start(&r, IMPROV_CMD_WIFI_NETWORKS);
		result_add(&r, found[i].ssid);
		result_add(&r, rssi);
		result_add(&r, found[i].secured ? "YES" : "NO");
		result_send(&r, out);
	}
	send_empty_result(IMPROV_CMD_WIFI_NETWORKS, out);
}

/* --- tiny-edge's commands ------------------------------------------------ */

static const char *state_name(enum tedge_state state)
{
	switch (state) {
	case TEDGE_STATE_STOPPED:
		return "stopped";
	case TEDGE_STATE_WAITING_NETWORK:
		return "waiting-network";
	case TEDGE_STATE_WAITING_TIME:
		return "waiting-time";
	case TEDGE_STATE_AWAITING_REGISTRATION:
		return "awaiting-registration";
	case TEDGE_STATE_CONNECTING:
		return "connecting";
	case TEDGE_STATE_CONNECTED:
		return "connected";
	case TEDGE_STATE_UPDATING:
		return "updating";
	default:
		return "unknown";
	}
}

static void info(const struct rpc_out *out)
{
	char id[65], host[65];
	struct result r;

	if (tedge_get_device_id(id, sizeof(id)) < 0) {
		id[0] = '\0';
	}
	if (tedge_get_c8y_url(host, sizeof(host)) < 0) {
		host[0] = '\0';
	}
	result_start(&r, TEDGE_PROVISIONING_CMD_INFO);
	result_add(&r, STR(TEDGE_PROVISIONING_PROTOCOL_VERSION));
	result_add(&r, id);
	result_add(&r, TEDGE_IS_ENABLED(TEDGE_AUTH_C8Y_CA) ? "c8y-ca" : "bootstrap");
	result_add(&r, host);
	result_add(&r, state_name(tedge_get_state()));
	result_send(&r, out);
}

static void onboard(const uint8_t *data, size_t len, const struct rpc_out *out)
{
	char host[65], otp[65];
	size_t pos = 1;
	uint8_t flags;
	int rc;

	if (!TEDGE_IS_ENABLED(TEDGE_AUTH_C8Y_CA)) {
		/* Bootstrap devices have no one-time password to set. */
		out->error(IMPROV_ERR_UNKNOWN_RPC);
		return;
	}
	if (len < 1 || !take_string(data, len, &pos, host, sizeof(host)) ||
	    host[0] == '\0' ||
	    !take_string(data, len, &pos, otp, sizeof(otp)) || otp[0] == '\0' ||
	    pos != len) {
		out->error(IMPROV_ERR_INVALID_RPC);
		return;
	}
	flags = data[0];
	if (tedge_get_state() == TEDGE_STATE_CONNECTED &&
	    !(flags & TEDGE_PROVISIONING_FLAG_REONBOARD)) {
		TEDGE_LOG_WRN("provisioning: refused to set up a connected device "
			      "again without the re-onboard flag");
		memset(otp, 0, sizeof(otp));
		out->error(IMPROV_ERR_NOT_AUTHORIZED);
		return;
	}

	/* The client takes its tenant and password before it starts. */
	rc = tedge_stop();
	if (rc != 0 && rc != -EALREADY) {
		TEDGE_LOG_ERR("provisioning: the client did not stop (%d)", rc);
		memset(otp, 0, sizeof(otp));
		out->error(IMPROV_ERR_UNKNOWN);
		return;
	}
	rc = tedge_set_c8y_url(host);
	if (rc == 0) {
		rc = tedge_set_enroll_otp(otp);
	}
	memset(otp, 0, sizeof(otp));
	if (rc != 0) {
		TEDGE_LOG_WRN("provisioning: onboarding data refused (%d)", rc);
		(void)tedge_start();
		out->error(IMPROV_ERR_INVALID_RPC);
		return;
	}
	TEDGE_LOG_INF("provisioning: set up for %s", host);
	rc = tedge_start();
	if (rc != 0 && rc != -EALREADY) {
		TEDGE_LOG_ERR("provisioning: the client did not start (%d)", rc);
		out->error(IMPROV_ERR_UNKNOWN);
		return;
	}
	send_empty_result(TEDGE_PROVISIONING_CMD_ONBOARD, out);
}

static void identify(const struct rpc_out *out)
{
	char id[65];

	if (tedge_get_device_id(id, sizeof(id)) < 0) {
		id[0] = '\0';
	}
	TEDGE_LOG_INF("provisioning: this is \"%s\"", id);
	if (hooks != NULL && hooks->identify != NULL) {
		hooks->identify(hooks->ctx);
	}
	send_empty_result(TEDGE_PROVISIONING_CMD_IDENTIFY, out);
}

/* --- Dispatch ------------------------------------------------------------ */

static void handle(const uint8_t *pkt, size_t n, const struct rpc_out *out)
{
	const uint8_t *data = pkt + 2;
	size_t len;

	if (n < 2 || (size_t)pkt[1] + 2 != n) {
		out->error(IMPROV_ERR_INVALID_RPC);
		return;
	}
	len = pkt[1];
	switch (pkt[0]) {
	case IMPROV_CMD_WIFI_SETTINGS:
		wifi_settings(data, len, out);
		break;
	case IMPROV_CMD_CURRENT_STATE:
		current_state(out);
		break;
	case IMPROV_CMD_DEVICE_INFO:
		device_info(out);
		break;
	case IMPROV_CMD_WIFI_NETWORKS:
		wifi_networks(out);
		break;
	case TEDGE_PROVISIONING_CMD_INFO:
		info(out);
		break;
	case TEDGE_PROVISIONING_CMD_ONBOARD:
		onboard(data, len, out);
		break;
	case TEDGE_PROVISIONING_CMD_IDENTIFY:
		identify(out);
		break;
	default:
		out->error(IMPROV_ERR_UNKNOWN_RPC);
		break;
	}
}

int tedge_prov_rpc_submit(const uint8_t *pkt, size_t len,
			  const struct rpc_out *out)
{
	struct rpc_msg msg;

	if (len > sizeof(msg.data)) {
		len = 0; /* too long for any command: answered "invalid RPC" */
	}
	msg.out = out;
	msg.len = (uint8_t)len;
	if (len > 0) {
		memcpy(msg.data, pkt, len);
	}
	if (tedge_port_msgq_put(&tedge_prov_queue, &msg) != 0) {
		return -ENOMEM; /* a tool sends one request at a time */
	}
	tedge_port_event_post(&prov_events, EV_RPC);
	return 0;
}

static void worker(void *arg)
{
	struct rpc_msg msg;

	(void)arg;
	for (;;) {
		(void)tedge_port_event_wait(&prov_events, EV_RPC, 1000);
		tedge_port_event_clear(&prov_events, EV_RPC);
		while (tedge_port_msgq_get(&tedge_prov_queue, &msg) == 0) {
			handle(msg.data, msg.len, msg.out);
		}
	}
}

int tedge_provisioning_init(const struct tedge_provisioning_hooks *h)
{
	static bool started;

	if (started) {
		return -EALREADY;
	}
	hooks = h;
	started = true;
	return tedge_port_thread_start(&prov_thread, prov_stack,
				       TEDGE_PORT_STACK_SIZEOF(prov_stack), worker,
				       NULL, "tedge_prov");
}
