/* SPDX-License-Identifier: Apache-2.0
 *
 * Lifecycle and state machine of the client: one thread that waits for the
 * application's network, sets the clock, lets the transport connect, and
 * services the session until it ends, then reconnects with back-off.
 *
 *   STOPPED -> WAITING_NETWORK -> WAITING_TIME -> [AWAITING_REGISTRATION]
 *           -> CONNECTING -> CONNECTED -> (loss) -> back-off -> CONNECTING
 *
 * The module never brings up or configures the network: it only listens for
 * connectivity events (tedge-client-module: "The application owns
 * connectivity").
 */

#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/device.h>
#include <tedge/port/kv.h>
#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>


#include <errno.h>
#include <stdio.h>
#include <string.h>

TEDGE_LOG_REGISTER();

#define BACKOFF_MIN_S 3
#define STABLE_S      60
/* Twin fragments held for republishing after a reconnect: every parameter
 * set (each is a fragment), the client's own tedge_RemoteAccess and
 * tedge_Certificate, and two for the application's tedge_publish_twin()
 * calls. At a flat 4 the parameter sets filled the table on images with
 * three or four sets and tedge_Certificate was dropped ("no free twin
 * slot", 2026-09-23). 36 B of RAM each. */
#define TWIN_SLOTS    (TEDGE_PARAM_MAX_SETS + 2 + 2)

static struct tedge_id identity;
static struct tedge_port_platform platform;
static const struct tedge_hooks *hooks;
static tedge_port_atomic_t state = TEDGE_PORT_ATOMIC_INIT(TEDGE_STATE_STOPPED);
static bool initialised;

TEDGE_PORT_EVENT_DEFINE(events);
#define EV_NET_UP    (1u << 0)
#define EV_NET_DOWN  (1u << 1)
#define EV_STOP      (1u << 2)
#define EV_WAKE      (1u << 3)
#define EV_RECONNECT (1u << 4)

static char c8y_host[80];

/* Twin fragments, republished after every reconnect. */
static struct {
	char fragment[32];
	char *json; /* module heap */
} twin[TWIN_SLOTS];
TEDGE_PORT_MUTEX_DEFINE(twin_lock);
static bool twin_dirty;

/* ------------------------------------------------------------------------ */
/* Heap                                                                      */
/* ------------------------------------------------------------------------ */

void *tedge_alloc(size_t size)
{
	void *p = tedge_port_heap_alloc(size);

	if (p == NULL) {
		TEDGE_LOG_ERR("out of client heap (%zu B wanted of %d B; raise "
			TEDGE_CONFIG_NAME(TEDGE_HEAP_SIZE) ")", size,
			TEDGE_HEAP_SIZE);
	}
	return p;
}

size_t tedge_heap_free(void)
{
	return tedge_port_heap_free_bytes();
}

/* JSON has few escapes; anything else that would break a payload becomes a
 * space, because telemetry text comes from applications and must never be
 * able to produce invalid JSON. */
void tedge_json_escape(const char *in, char *out, size_t len)
{
	size_t n = 0;

	for (const char *p = in; *p != '\0' && n + 2 < len; p++) {
		if (*p == '"' || *p == '\\') {
			out[n++] = '\\';
			out[n++] = *p;
		} else if ((unsigned char)*p < 0x20) {
			out[n++] = ' ';
		} else {
			out[n++] = *p;
		}
	}
	out[n] = '\0';
}

void tedge_free(void *p)
{
	tedge_port_heap_free(p);
}

/* ------------------------------------------------------------------------ */
/* State, identity, hooks                                                    */
/* ------------------------------------------------------------------------ */

static const char *state_name(enum tedge_state s)
{
	static const char *const names[] = {
		"stopped",   "waiting-network", "waiting-time",
		"awaiting-registration", "connecting", "connected", "updating",
	};

	return ((size_t)s < TEDGE_ARRAY_SIZE(names)) ? names[s] : "?";
}

void tedge_set_state(enum tedge_state s)
{
	if ((enum tedge_state)tedge_port_atomic_set(&state, s) == s) {
		return;
	}
	TEDGE_LOG_INF("state: %s", state_name(s));
	if (hooks != NULL && hooks->on_state != NULL) {
		hooks->on_state(s, hooks->user_data);
	}
}

enum tedge_state tedge_get_state(void)
{
	return (enum tedge_state)tedge_port_atomic_get(&state);
}

const struct tedge_id *tedge_identity(void)
{
	return &identity;
}

const struct tedge_port_platform *tedge_platform(void)
{
	return &platform;
}

const struct tedge_hooks *tedge_hook_table(void)
{
	return hooks;
}

static void progress(void)
{
	if (hooks != NULL && hooks->progress != NULL) {
		hooks->progress(hooks->user_data);
	}
}

/* ------------------------------------------------------------------------ */
/* Tenant host                                                               */
/* ------------------------------------------------------------------------ */

const char *tedge_c8y_host(void)
{
	return c8y_host[0] ? c8y_host : TEDGE_C8Y_URL;
}

int tedge_c8y_host_set(const char *host)
{
	if (host == NULL || host[0] == '\0' || strlen(host) >= sizeof(c8y_host)) {
		return -EINVAL;
	}
	strcpy(c8y_host, host);
	return tedge_port_kv_set(TEDGE_KEY_C8Y_URL, c8y_host, strlen(c8y_host));
}

int tedge_set_c8y_url(const char *host)
{
	if (!TEDGE_IS_ENABLED(TEDGE_TRANSPORT_C8Y)) {
		return -ENOTSUP;
	}
	return tedge_c8y_host_set(host);
}

int tedge_get_c8y_url(char *buf, size_t len)
{
	const char *host = tedge_c8y_host();
	size_t n = strlen(host);

	if (n >= len) {
		return -ENOSPC;
	}
	memcpy(buf, host, n + 1);
	return (int)n;
}

int tedge_get_device_id(char *buf, size_t len)
{
	size_t n;

	if (!initialised) {
		return -EPERM;
	}
	n = strlen(identity.external_id);
	if (n >= len) {
		return -ENOSPC;
	}
	memcpy(buf, identity.external_id, n + 1);
	return (int)n;
}

static void load_c8y_host(void)
{
	int n = tedge_port_kv_get(TEDGE_KEY_C8Y_URL, c8y_host,
				  sizeof(c8y_host) - 1);

	c8y_host[(n > 0) ? n : 0] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Twin fragments                                                            */
/* ------------------------------------------------------------------------ */

int tedge_publish_twin(const char *fragment, const char *json)
{
	int rc = -ENOMEM;

	if (fragment == NULL || fragment[0] == '\0' ||
	    strlen(fragment) >= sizeof(twin[0].fragment)) {
		return -EINVAL;
	}
	tedge_port_mutex_lock(&twin_lock);
	for (int i = 0; i < TWIN_SLOTS; i++) {
		bool match = strcmp(twin[i].fragment, fragment) == 0;

		if (!match && twin[i].fragment[0] != '\0') {
			continue;
		}
		if (json == NULL) { /* forget it */
			if (match) {
				tedge_free(twin[i].json);
				twin[i].json = NULL;
				twin[i].fragment[0] = '\0';
			}
			rc = 0;
			break;
		}
		char *copy = tedge_alloc(strlen(json) + 1);

		if (copy == NULL) {
			break;
		}
		strcpy(copy, json);
		tedge_free(twin[i].json);
		twin[i].json = copy;
		strcpy(twin[i].fragment, fragment);
		twin_dirty = true;
		rc = 0;
		break;
	}
	tedge_port_mutex_unlock(&twin_lock);
	if (rc == 0) {
		tedge_port_event_post(&events, EV_WAKE);
	} else if (rc == -ENOMEM) {
		TEDGE_LOG_ERR("no free twin slot for \"%s\" (%d)", fragment, TWIN_SLOTS);
	}
	return rc;
}

void tedge_telemetry_wake(void)
{
	tedge_port_event_post(&events, EV_WAKE);
}

void tedge_request_reconnect(void)
{
	tedge_port_event_post(&events, EV_RECONNECT | EV_WAKE);
}

void tedge_twin_republish(void)
{
	const struct tedge_transport *t = tedge_transport_get();

	tedge_port_mutex_lock(&twin_lock);
	for (int i = 0; i < TWIN_SLOTS; i++) {
		if (twin[i].fragment[0] != '\0' && twin[i].json != NULL) {
			(void)t->publish_twin(twin[i].fragment, twin[i].json);
		}
	}
	twin_dirty = false;
	tedge_port_mutex_unlock(&twin_lock);
}

/* ------------------------------------------------------------------------ */
/* Network events                                                            */
/* ------------------------------------------------------------------------ */

/* From the port, in whatever context it learns of the change. */
static void on_network(bool up)
{
	if (up) {
		tedge_port_event_clear(&events, EV_NET_DOWN);
		tedge_port_event_post(&events, EV_NET_UP | EV_WAKE);
	} else {
		tedge_port_event_clear(&events, EV_NET_UP);
		tedge_port_event_post(&events, EV_NET_DOWN | EV_WAKE);
	}
}

static bool network_up(void)
{
	return tedge_port_net_is_up() ||
	       tedge_port_event_test(&events, EV_NET_UP) != 0;
}

/* Wait for connectivity. Returns false when the client is stopping. */
static bool wait_for_network(void)
{
	if (network_up()) {
		return true;
	}
	tedge_set_state(TEDGE_STATE_WAITING_NETWORK);
	while (!network_up()) {
		uint32_t got = tedge_port_event_wait(&events,
						     EV_NET_UP | EV_STOP, 5000);

		if (got & EV_STOP) {
			return false;
		}
		progress();
	}
	return true;
}

/* ------------------------------------------------------------------------ */
/* Client thread                                                             */
/* ------------------------------------------------------------------------ */

static void client_thread(void *arg)
{
	const struct tedge_transport *transport = tedge_transport_get();
	uint32_t backoff = 0;

	TEDGE_UNUSED(arg);

	while (!(tedge_port_event_test(&events, EV_STOP))) {
		int64_t connected_at;
		int rc;

		if (!wait_for_network()) {
			break;
		}
		if (!tedge_port_time_is_valid()) {
			tedge_set_state(TEDGE_STATE_WAITING_TIME);
			if (tedge_port_time_sync() != 0) {
				tedge_port_sleep_ms(5000);
				continue;
			}
		}

		tedge_set_state(TEDGE_STATE_CONNECTING);
		rc = transport->connect();
		if (rc == -ECANCELED) {
			break; /* tedge_stop() during enrollment */
		}
		if (rc != 0) {
			backoff = tedge_backoff_next(backoff,
						     TEDGE_RECONNECT_BACKOFF_MAX_S);
			TEDGE_LOG_WRN("connect failed (%d); retrying in %u s", rc,
				backoff);
			uint32_t got = tedge_port_event_wait(
				&events, EV_STOP | EV_NET_DOWN, backoff * 1000);

			if (got & EV_STOP) {
				break;
			}
			continue;
		}

		connected_at = tedge_port_uptime_ms();
		tedge_set_state(TEDGE_STATE_CONNECTED);
		tedge_twin_republish();

		/* Service the session until it ends. */
		tedge_port_event_clear(&events, EV_RECONNECT);
		while (!(tedge_port_event_test(&events, EV_STOP))) {
			if (tedge_port_event_test(&events, EV_NET_DOWN)) {
				rc = -ENETDOWN;
				break;
			}
			if (tedge_port_event_test(&events, EV_RECONNECT)) {
				/* Something replaced a credential; the new one
				 * only takes effect on a new TLS session. */
				tedge_port_event_clear(&events, EV_RECONNECT);
				TEDGE_LOG_INF("reconnecting to pick up a new "
					"credential");
				rc = 0;
				break;
			}
			rc = transport->poll(1000);
			if (rc != 0) {
				break;
			}
			progress();
			if (twin_dirty) {
				tedge_twin_republish();
			}
#if defined(TEDGE_TELEMETRY)
			tedge_telemetry_flush();
#endif
#if defined(TEDGE_HEALTH)
			tedge_health_tick();
#endif
		}
		transport->disconnect();
		if (tedge_port_uptime_ms() - connected_at > STABLE_S * 1000) {
			backoff = 0; /* the session was healthy */
		}
		if (tedge_port_event_test(&events, EV_STOP)) {
			break;
		}
		TEDGE_LOG_WRN("session ended (%d)", rc);
		backoff = tedge_backoff_next(backoff,
						     TEDGE_RECONNECT_BACKOFF_MAX_S);
		TEDGE_LOG_INF("reconnecting in %u s", backoff);
		(void)tedge_port_event_wait(&events, EV_STOP, backoff * 1000);
	}
	tedge_set_state(TEDGE_STATE_STOPPED);
}

TEDGE_PORT_STACK_DEFINE(client_stack, TEDGE_THREAD_STACK_SIZE);
static tedge_port_thread_t client_tid;
static bool running;
static bool started_once;

/* ------------------------------------------------------------------------ */
/* Public lifecycle                                                          */
/* ------------------------------------------------------------------------ */

static void copy_or_default(char *dst, size_t len, const char *value,
			    const char *fallback)
{
	snprintf(dst, len, "%s",
		 (value != NULL && value[0]) ? value
		 : (fallback != NULL)        ? fallback
					     : "");
}

int tedge_init(const struct tedge_identity *id, const struct tedge_hooks *h)
{
	char mac_id[64] = "";
	char serial[16];

	if (initialised) {
		return -EALREADY;
	}
	hooks = h;

	(void)tedge_port_kv_init();
	load_c8y_host();

	if (tedge_default_device_id(mac_id, sizeof(mac_id)) != 0) {
		snprintf(mac_id, sizeof(mac_id), "%s-unknown",
			 TEDGE_DEVICE_ID_PREFIX);
	}
	copy_or_default(identity.external_id, sizeof(identity.external_id),
			id ? id->external_id : NULL, mac_id);
	copy_or_default(identity.name, sizeof(identity.name),
			id ? id->name : NULL, identity.external_id);
	copy_or_default(identity.type, sizeof(identity.type),
			id ? id->type : NULL, TEDGE_DEVICE_TYPE);
	copy_or_default(identity.firmware_name, sizeof(identity.firmware_name),
			id ? id->firmware_name : NULL,
			tedge_port_firmware_name());
	copy_or_default(identity.firmware_version,
			sizeof(identity.firmware_version),
			id ? id->firmware_version : NULL,
			tedge_port_firmware_version());
	tedge_port_platform(&platform);
	copy_or_default(identity.hardware_model, sizeof(identity.hardware_model),
			id ? id->hardware_model : NULL, platform.model);
	copy_or_default(identity.hardware_revision,
			sizeof(identity.hardware_revision),
			id ? id->hardware_revision : NULL, platform.revision);
	if (tedge_default_serial_number(serial, sizeof(serial)) != 0) {
		serial[0] = '\0';
	}
	copy_or_default(identity.serial_number, sizeof(identity.serial_number),
			id ? id->serial_number : NULL, serial);

	tedge_port_net_watch(on_network);

#if defined(TEDGE_FIRMWARE_UPDATE)
	/* Before anything else: a test-booted image must confirm itself or be
	 * rolled back, whether or not it ever reaches the cloud. */
	tedge_fw_arm_confirm_deadline();
#endif
	initialised = true;
	TEDGE_LOG_INF("tiny-edge %s: device \"%s\" (%s)", tedge_version(),
		identity.external_id, identity.type);
	return 0;
}

int tedge_start(void)
{
	if (!initialised) {
		return -EPERM;
	}
	if (running) {
		return -EALREADY;
	}
	tedge_port_event_clear(&events, EV_STOP);
	/* Once per boot: a client stopped and started again (to take new
	 * onboarding data, say) has declared all this already. */
	if (!started_once) {
		started_once = true;
#if defined(TEDGE_LOG_UPLOAD)
		tedge_log_upload_init();
#endif
#if defined(TEDGE_PARAMETERS_SELF)
		/* After the application's sets, so a declaration of its own
		 * wins a name clash, and before the sweep, which needs them
		 * all. */
		tedge_self_params_declare();
#endif
#if defined(TEDGE_PARAMETERS)
		/* Every set the application means to declare is declared by
		 * now. */
		tedge_params_on_start();
#endif
	}
	running = true;
	(void)tedge_port_thread_start(&client_tid, client_stack,
				      TEDGE_PORT_STACK_SIZEOF(client_stack),
				      client_thread, NULL, "tedge");
	return 0;
}

int tedge_stop(void)
{
	if (!running) {
		return -EALREADY;
	}
	tedge_port_event_post(&events, EV_STOP | EV_WAKE);
	/* A thread still running after the wait (stuck in a network call
	 * longer than any of its timeouts) keeps the client "running": a
	 * start now would put a second thread on the same stack. */
	if (tedge_port_thread_join(&client_tid, 10000) != 0) {
		return -EBUSY;
	}
	running = false;
	return 0;
}

bool tedge_wait_for_stop(uint32_t ms)
{
	return (tedge_port_event_wait(&events, EV_STOP, ms) & EV_STOP) != 0;
}
