/* SPDX-License-Identifier: Apache-2.0
 *
 * The Cumulocity transport: one MQTTS session (the MQTT Service on 9883 with
 * a certificate, or Core MQTT on 8883), the SmartREST dispatcher, the JWT,
 * the twin/health publishing and the restart operation.
 *
 * Only the client thread (tedge_core.c) touches this file's state: Zephyr's
 * MQTT client is not thread-safe.
 */

#include "tedge_internal.h"

#include "tedge_mqtt.h"
#include "tedge_util.h"

#include <tedge/port/kv.h>
#include <tedge/port/log.h>
#include <tedge/port/net.h>
#include <tedge/port/os.h>
#include <tedge/port/time.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

TEDGE_LOG_DECLARE();

#if defined(TEDGE_C8Y_MQTT_SERVICE)
#define C8Y_PORT 9883
#define FREE_FORM_TOPICS 1
#else
#define C8Y_PORT 8883
#define FREE_FORM_TOPICS 0
#endif

#define QOS_0 0
#define QOS_1 1
#define CONNACK_TIMEOUT_MS 15000
#define SUBACK_TIMEOUT_MS  3000
#define SUBACK_RETRY_MAX   5
#define JWT_REFRESH_MS     (50 * 60 * 1000)
/* Three broker closes this soon after CONNACK look like another client using
 * the same ID (c8y-direct-spikes, Spike B finding 6). */
#define TAKEOVER_WINDOW_MS 10000
#define TAKEOVER_STRIKES   3

/* The trust anchors listed in TEDGE_C8Y_CA_FILES. */
static const unsigned char server_ca[] = {
#include "tedge_ca_bundle.inc"
	0x00
};


/* Downstream topics: thin-edge.io's Cumulocity bridge set, minus the
 * bootstrap topics (tedge_bootstrap.c owns those). */
#if defined(TEDGE_C8Y_OPERATION_JSON)
/* Operations come as JSON, which is the only delivery that carries their
 * id; s/ds would deliver the same operations a second time. */
#define OP_TOPIC "devicecontrol/notifications"
static const char *const sub_topics[] = { OP_TOPIC, "s/e", "s/dat" };
#else
static const char *const sub_topics[] = { "s/ds", "s/e", "s/dat" };
#endif

static struct tedge_mqtt client;
static int conn = -1;
/* Every incoming packet is read whole into this: the payload, and its
 * topic and header in front of it. */
#define PACKET_OVERHEAD 64
static uint8_t packet_buf[TEDGE_C8Y_PAYLOAD_BYTES + PACKET_OVERHEAD];
static bool session_open;
static bool ca_registered;
static char jwt[1024];
static int64_t jwt_at;
static char device_id[64];

static int64_t connected_at;
static int takeover_strikes;

/* Operations the application registered, plus the built-in ones. */
struct op_entry {
	const char *name;
	tedge_operation_handler_t handler;
	void *user_data;
};
#define OP_SLOTS 4
static struct op_entry ops[OP_SLOTS];

struct tedge_operation {
	char name[40];
	char payload[192];
	bool done;
};
static struct tedge_operation current_op;

#if defined(TEDGE_RESTART)
static bool restart_requested;
static bool restart_pending_after_boot;
#endif

/* ------------------------------------------------------------------------ */
/* Publishing                                                                */
/* ------------------------------------------------------------------------ */

static int publish(const char *topic, const char *payload, int qos)
{
	int ret;

	if (!session_open) {
		return -ENOTCONN;
	}
	ret = tedge_mqtt_publish(&client, topic, payload, strlen(payload), qos);
	if (ret) {
		TEDGE_LOG_ERR("publish to %s failed (%d)", topic, ret);
	}
	return ret;
}

/* One operation at a time.
 *
 * Cumulocity's 501, 502 and 503 act on the *oldest* operation in the
 * matching state, not on an operation named in the message, so a device with
 * two operations executing at once gets their results crossed: the log
 * upload's URL lands on the command, and an operation is left EXECUTING for
 * ever. The client therefore runs one operation at a time and keeps the rest
 * in a small queue.
 *
 * The gate is maintained here, where every status message passes, rather
 * than at each of the places that send one: a feature added later cannot
 * forget it.
 */
#define OP_QUEUE_DEPTH 2
/* A firmware request carries a URL, which is the longest line that arrives. */
#define OP_LINE_MAX    768

static bool op_executing;
static char op_queue[OP_QUEUE_DEPTH][OP_LINE_MAX];
static uint8_t op_queued;

/* The operation in flight is also kept in settings, as the status that
 * started it ("504,<id>" or "501,<fragment>"), and removed when it ends. A
 * reset in between (a crash, a watchdog, the power) leaves it there, and
 * the first session after the reboot reports it as failed. Without that,
 * the operation stays EXECUTING in Cumulocity for ever.
 *
 * The restart and the firmware update end across a reboot on purpose: they
 * remove this key and keep their own. */
#define OP_MARK_MAX 48

/* What the boot before left running; reported once, by session_start(). */
static char op_interrupted[OP_MARK_MAX];

/* "504,<id>" or "501,<fragment>": the key of the operation @p line starts
 * or ends. @return its length, or 0 when @p line is not such a status. */
static int op_mark_of(const char *line, int tmpl, char *out, size_t len)
{
	char field[OP_MARK_MAX - 4];
	int start;

	switch (tmpl) {
	case 501: case 502: case 503:
		start = 501;
		break;
	case 504: case 505: case 506:
		start = 504;
		break;
	default:
		return 0;
	}
	if (tedge_sr_field(line, 1, field, sizeof(field)) <= 0) {
		return 0;
	}
	int n = snprintf(out, len, "%d,%s", start, field);

	return (n > 0 && (size_t)n < len) ? n : 0;
}

static void op_mark_update(const char *line, int tmpl)
{
	char mark[OP_MARK_MAX];
	char stored[OP_MARK_MAX];
	int n = op_mark_of(line, tmpl, mark, sizeof(mark));
	int m;

	if (n == 0) {
		return;
	}
	if (tmpl == 501 || tmpl == 504) {
		(void)tedge_port_kv_set(TEDGE_KEY_OP_RUNNING, mark, (size_t)n);
		return;
	}
	/* Only the operation it names: a restart or firmware update reported
	 * after its reboot ends while another operation may be running. */
	m = tedge_port_kv_get(TEDGE_KEY_OP_RUNNING, stored, sizeof(stored) - 1);
	if (m == n && memcmp(stored, mark, (size_t)n) == 0) {
		(void)tedge_port_kv_delete(TEDGE_KEY_OP_RUNNING);
	}
}

int tedge_c8y_publish_sr(const char *line)
{
	int tmpl = tedge_sr_template(line);

	if (tmpl == 501 || tmpl == 504) {
		op_executing = true;
	} else if (tmpl == 502 || tmpl == 503 || tmpl == 505 || tmpl == 506) {
		op_executing = false;
	}
	op_mark_update(line, tmpl);
	return publish("s/us", line, QOS_1);
}

/* Read what the boot before left running, once, before anything of this
 * boot can overwrite it. */
static void op_interrupted_load(void)
{
	int n = tedge_port_kv_get(TEDGE_KEY_OP_RUNNING, op_interrupted,
				  sizeof(op_interrupted) - 1);

	op_interrupted[(n > 0) ? n : 0] = '\0';
	if (op_interrupted[0] != '\0') {
		TEDGE_LOG_WRN("the device restarted while an operation was running "
			      "(%s); it is reported as failed once connected",
			      op_interrupted);
	}
}

/* Fail the operation the boot before left running. Called before the
 * pending operations are asked for, so no operation of this boot is
 * running yet. */
static void op_interrupted_report(void)
{
	char line[OP_MARK_MAX + 80];
	char quoted[72];

	if (op_interrupted[0] == '\0') {
		return;
	}
	(void)tedge_sr_quote("the device restarted while the operation was running",
			     quoted, sizeof(quoted));
	/* "504,<id>" becomes "505,<id>,...", "501,<name>" "502,<name>,...". */
	snprintf(line, sizeof(line), "%d%s,%s",
		 (op_interrupted[2] == '4') ? 505 : 502, op_interrupted + 3,
		 quoted);
	if (tedge_c8y_publish_sr(line) == 0) {
		TEDGE_LOG_INF("operation %s reported as failed: the device "
			      "restarted while it was running", op_interrupted + 4);
		op_interrupted[0] = '\0';
	}
}

const char *tedge_c8y_jwt(void)
{
	return jwt;
}

/* ------------------------------------------------------------------------ */
/* Operations                                                                */
/* ------------------------------------------------------------------------ */

/* The id of the operation in flight, when the client knows it. Exactly one
 * operation runs at a time (see the queue above), so one id is enough for
 * everything that finishes inside the session; the restart and the firmware
 * update, which finish after a reboot, keep theirs in settings.
 *
 * With an id, a status names the operation it belongs to (504, 505, 506).
 * Without one, it names the fragment and Cumulocity applies it to the
 * oldest operation in that state (501, 502, 503) — which is why knowing the
 * id matters: an operation left executing by a reset otherwise swallows
 * every later result.
 */
static char current_op_id[24];

static void op_status(int by_id, int by_name, const char *name,
		      const char *quoted)
{
	char line[288];

	if (current_op_id[0] != '\0') {
		if (quoted != NULL) {
			snprintf(line, sizeof(line), "%d,%s,%s", by_id,
				 current_op_id, quoted);
		} else {
			snprintf(line, sizeof(line), "%d,%s", by_id,
				 current_op_id);
		}
	} else if (quoted != NULL) {
		snprintf(line, sizeof(line), "%d,%s,%s", by_name, name, quoted);
	} else {
		snprintf(line, sizeof(line), "%d,%s", by_name, name);
	}
	(void)tedge_c8y_publish_sr(line);
}

/* @p reason and @p result are raw text; they are quoted here. */
static void op_executing_now(const char *name)
{
	op_status(504, 501, name, NULL);
}

static void op_failed(const char *name, const char *reason)
{
	char quoted[224];

	(void)tedge_sr_quote((reason != NULL) ? reason : "failed", quoted,
			     sizeof(quoted));
	op_status(505, 502, name, quoted);
}

static void op_succeeded(const char *name, const char *result)
{
	char quoted[224];

	if (result == NULL || result[0] == '\0') {
		op_status(506, 503, name, NULL);
		return;
	}
	(void)tedge_sr_quote(result, quoted, sizeof(quoted));
	op_status(506, 503, name, quoted);
}

#if defined(TEDGE_SHELL_COMMAND)
/* Completes an operation with text whose line breaks matter, such as what
 * a shell command printed: they travel inside the quoted field. */
static void op_finished_lines(const char *name, bool ok, const char *text)
{
	char quoted[224];

	if (ok && (text == NULL || text[0] == '\0')) {
		op_status(506, 503, name, NULL);
		return;
	}
	(void)tedge_sr_quote_lines((text != NULL) ? text : "failed", quoted,
				   sizeof(quoted));
	if (ok) {
		op_status(506, 503, name, quoted);
	} else {
		op_status(505, 502, name, quoted);
	}
}
#endif

#if defined(TEDGE_RESTART) || defined(TEDGE_FIRMWARE_UPDATE)
/* Completes an operation whose id was kept across a reboot. */
static void op_finish_by_id(const char *id, const char *name, bool ok,
			    const char *text)
{
	char saved[sizeof(current_op_id)];

	snprintf(saved, sizeof(saved), "%s", current_op_id);
	snprintf(current_op_id, sizeof(current_op_id), "%s",
		 (id != NULL) ? id : "");
	if (ok) {
		op_succeeded(name, text);
	} else {
		op_failed(name, text);
	}
	snprintf(current_op_id, sizeof(current_op_id), "%s", saved);
}
#endif

int tedge_register_operation(const char *name, tedge_operation_handler_t handler,
			     void *user_data)
{
	if (name == NULL || handler == NULL) {
		return -EINVAL;
	}
	for (int i = 0; i < OP_SLOTS; i++) {
		if (ops[i].name == NULL) {
			ops[i].name = name;
			ops[i].handler = handler;
			ops[i].user_data = user_data;
			return 0;
		}
	}
	TEDGE_LOG_ERR("no free operation slot for \"%s\" (%d)", name, OP_SLOTS);
	return -ENOMEM;
}

const char *tedge_operation_payload(const struct tedge_operation *op)
{
	return (op != NULL) ? op->payload : NULL;
}

int tedge_operation_succeed(struct tedge_operation *op, const char *result)
{
	if (op == NULL || op->done) {
		return -EINVAL;
	}
	op->done = true;
	op_succeeded(op->name, result);
	return 0;
}

int tedge_operation_fail(struct tedge_operation *op, const char *reason)
{
	if (op == NULL || op->done) {
		return -EINVAL;
	}
	op->done = true;
	op_failed(op->name, reason);
	return 0;
}

/* Report an operation the image cannot run (baseline requirement: never leave
 * it pending).
 *
 * 501 first, always: Cumulocity's 502 fails the oldest *executing* operation,
 * so refusing one straight from PENDING would leave it pending for ever —
 * exactly the state this function exists to avoid. */
static void op_unsupported(const char *name, const char *why)
{
	/* Executing first, always: without an id, Cumulocity's failure acts
	 * on the oldest *executing* operation, so an operation refused
	 * straight from PENDING would stay pending for ever — exactly the
	 * state this function exists to avoid. With an id it is harmless. */
	op_executing_now(name);
	op_failed(name, why);
}

/* ------------------------------------------------------------------------ */
/* Restart (TEDGE_RESTART)                                            */
/* ------------------------------------------------------------------------ */

#if defined(TEDGE_RESTART)
static void handle_restart(void)
{
	const struct tedge_hooks *hooks = tedge_hook_table();
	char reason[96] = "";
	uint8_t one = 1;

	restart_requested = false;
	if (hooks != NULL && hooks->restart_request != NULL) {
		int rc = hooks->restart_request(reason, sizeof(reason),
						hooks->user_data);

		if (rc != 0) {
			TEDGE_LOG_WRN("restart vetoed by the application (%d): %s", rc,
				reason[0] ? reason : "no reason given");
			op_unsupported("c8y_Restart",
				       reason[0] ? reason
						 : "the application refused the restart");
			return;
		}
	}
	TEDGE_LOG_INF("restart requested by the cloud");
	op_executing_now("c8y_Restart");
	/* The operation is completed after the reboot, so what identifies it
	 * has to survive the reboot too. An empty id means "the oldest
	 * restart operation", which is all the static path can say. */
	if (current_op_id[0] != '\0') {
		(void)tedge_port_kv_set(TEDGE_KEY_RESTART, current_op_id,
					strlen(current_op_id) + 1);
	} else {
		(void)tedge_port_kv_set(TEDGE_KEY_RESTART, &one, sizeof(one));
	}
	/* Its own marker reports it after the reboot: not as interrupted. */
	(void)tedge_port_kv_delete(TEDGE_KEY_OP_RUNNING);
	tedge_port_sleep_ms(500);
	tedge_mqtt_disconnect(&client);
	tedge_port_close(conn);
	conn = -1;
	tedge_port_sleep_ms(300);
	tedge_device_reset();
}

/* The marker is either a single byte (the older format: "a restart was
 * asked for") or the id of the operation that asked. */
static char restart_op_id[24];

static bool restart_marker_load(void)
{
	int n = tedge_port_kv_get(TEDGE_KEY_RESTART, restart_op_id,
				  sizeof(restart_op_id));

	if (n == 1) {
		bool pending = ((uint8_t)restart_op_id[0] == 1);

		restart_op_id[0] = '\0';
		return pending;
	}
	if (n > 1) {
		restart_op_id[TEDGE_MIN((size_t)n, sizeof(restart_op_id) - 1)] = '\0';
		return true;
	}
	restart_op_id[0] = '\0';
	return false;
}
#endif /* TEDGE_RESTART */

/* ------------------------------------------------------------------------ */
/* Downstream messages                                                       */
/* ------------------------------------------------------------------------ */

static void dispatch_operation(const char *line);

/* Runs @p line now, or holds it until the operation in flight finishes.
 * Called on the client thread only. */
static void handle_operation(const char *line)
{
	int tmpl = tedge_sr_template(line);

	if (op_executing && tmpl >= 510 && tmpl <= 599) {
		if (op_queued >= OP_QUEUE_DEPTH) {
			/* Left PENDING deliberately: failing it would fail the
			 * operation that is actually running, because that is
			 * the one Cumulocity's 502 acts on. Cumulocity sends
			 * pending operations again when the device
			 * reconnects. */
			TEDGE_LOG_WRN("operation %d arrived while %d were already "
				"waiting; it stays pending until the next "
				"connect", tmpl, op_queued);
			return;
		}
		snprintf(op_queue[op_queued], OP_LINE_MAX, "%s", line);
		op_queued++;
		TEDGE_LOG_INF("operation %d waits: another one is running", tmpl);
		return;
	}
	dispatch_operation(line);
}

/* Starts the next waiting operation, if the last one has finished. */
static void run_queued_operation(void)
{
	char line[OP_LINE_MAX];

	if (op_executing || op_queued == 0) {
		return;
	}
	memcpy(line, op_queue[0], sizeof(line));
	op_queued--;
	if (op_queued > 0) {
		memmove(op_queue[0], op_queue[1], OP_LINE_MAX);
	}
	dispatch_operation(line);
}

static void dispatch_operation(const char *line)
{
#if defined(TEDGE_C8Y_OPERATION_JSON)
	/* The first field is the operation's id, where the static templates
	 * put the device serial nobody read. */
	if (tedge_sr_field(line, 1, current_op_id, sizeof(current_op_id)) <= 0) {
		current_op_id[0] = '\0';
	}
#endif
	char op_name[40];
	int tmpl = tedge_sr_template(line);

	switch (tmpl) {
	case 510: /* c8y_Restart */
#if defined(TEDGE_RESTART)
		restart_requested = true;
#else
		op_unsupported("c8y_Restart",
			       "restart is not built into this image "
			       "(" TEDGE_CONFIG_NAME(TEDGE_RESTART) ")");
#endif
		return;
	case 532: /* c8y_ParameterUpdate_<set> */
#if defined(TEDGE_PARAMETERS)
	{
		char set[40];
		char object[OP_LINE_MAX];
		char reason[160];
		char frag[64];

		(void)tedge_sr_field(line, 2, set, sizeof(set));
		(void)tedge_sr_field(line, 3, object, sizeof(object));
		/* The status has to name the fragment the change arrived as,
		 * not the set on its own. */
		snprintf(frag, sizeof(frag), "c8y_ParameterUpdate_%s", set);

		op_executing_now(frag);
		if (tedge_params_apply(set, object, reason, sizeof(reason)) !=
		    0) {
			op_failed(frag, reason);
		} else {
			op_succeeded(frag, NULL);
		}
	}
#else
		op_unsupported("c8y_ParameterUpdate",
			       "parameters are not built into this image "
			       "(" TEDGE_CONFIG_NAME(TEDGE_PARAMETERS) ")");
#endif
		return;
	case 515:
#if defined(TEDGE_FIRMWARE_UPDATE)
	{
		char reason[128];

		/* Executing first, always: see op_unsupported(). */
		op_executing_now("c8y_Firmware");
		if (tedge_fw_request(line, reason, sizeof(reason)) != 0) {
			op_failed("c8y_Firmware", reason);
		} else {
			char sr[224], quoted[160];

			tedge_fw_remember_operation(current_op_id);
			(void)tedge_sr_quote(tedge_fw_downtime_hint(), quoted,
					     sizeof(quoted));
			snprintf(sr, sizeof(sr), "400,c8y_FirmwareUpdateStarted,%s",
				 quoted);
			(void)tedge_c8y_publish_sr(sr);
		}
	}
#else
		op_unsupported("c8y_Firmware",
			       "firmware update is not built into this image "
			       "(" TEDGE_CONFIG_NAME(TEDGE_FIRMWARE_UPDATE) ")");
#endif
		return;
	case 530:
#if defined(TEDGE_REMOTE_ACCESS)
	{
		char reason[112];

		op_executing_now("c8y_RemoteAccessConnect");
		if (tedge_ra_request(line, reason, sizeof(reason)) != 0) {
			op_failed("c8y_RemoteAccessConnect", reason);
		}
	}
#else
		op_unsupported("c8y_RemoteAccessConnect",
			       "remote access is not built into this image "
			       "(" TEDGE_CONFIG_NAME(TEDGE_REMOTE_ACCESS) ")");
#endif
		return;
	case 511:
#if defined(TEDGE_SHELL_COMMAND)
	{
		char reason[160];

		op_executing_now("c8y_Command");
		if (tedge_shell_request(line, reason, sizeof(reason)) != 0) {
			op_failed("c8y_Command", reason);
		}
		return;
	}
#else
		/* Without the feature the application may still register its
		 * own c8y_Command handler, so fall through to the registered
		 * operations rather than refusing here. */
		snprintf(op_name, sizeof(op_name), "c8y_Command");
		break;
#endif
	case 522:
#if defined(TEDGE_LOG_UPLOAD)
	{
		char reason[128];

		op_executing_now("c8y_LogfileRequest");
		if (tedge_log_request(line, reason, sizeof(reason)) != 0) {
			op_failed("c8y_LogfileRequest", reason);
		}
	}
#else
		op_unsupported("c8y_LogfileRequest",
			       "log upload is not built into this image "
			       "(" TEDGE_CONFIG_NAME(TEDGE_LOG_UPLOAD) ")");
#endif
		return;
	default:
		if (tmpl < 0) {
			return; /* not a SmartREST line */
		}
		snprintf(op_name, sizeof(op_name), "%d", tmpl);
		break;
	}

	for (int i = 0; i < OP_SLOTS; i++) {
		if (ops[i].name != NULL && strcmp(ops[i].name, op_name) == 0) {
			char line_sr[64];

			memset(&current_op, 0, sizeof(current_op));
			snprintf(current_op.name, sizeof(current_op.name), "%s",
				 op_name);
			snprintf(current_op.payload, sizeof(current_op.payload),
				 "%.*s", (int)sizeof(current_op.payload) - 1,
				 line);
			TEDGE_UNUSED(line_sr);
			op_executing_now(op_name);
			ops[i].handler(&current_op, ops[i].user_data);
			return;
		}
	}
	/* Cumulocity's operations live in the 5xx templates. One this image
	 * has no handler for is failed with a reason, never dropped: an
	 * operation nobody answers stays pending in the cloud for ever, and
	 * the device looks broken rather than merely incomplete. Anything
	 * else on this topic is a response, not an operation. */
	if (tmpl >= 510 && tmpl <= 599) {
		char why[96];

		snprintf(why, sizeof(why),
			 "this image has no handler for this operation (%d)",
			 tmpl);
		op_unsupported(op_name, why);
		return;
	}
	TEDGE_LOG_DBG("unhandled downstream message: %.40s", line);
}

static void handle_message(const char *topic, const char *payload, size_t len)
{
	if (strcmp(topic, "s/dat") == 0) {
		/* "71,<jwt>"; the token itself is never logged. */
		if (len > 3 && len - 3 < sizeof(jwt)) {
			memcpy(jwt, payload + 3, len - 3);
			jwt[len - 3] = '\0';
			jwt_at = tedge_port_uptime_ms();
			TEDGE_LOG_DBG("token received (%zu B)", len - 3);
		}
		return;
	}
	if (strcmp(topic, "s/e") == 0) {
		TEDGE_LOG_WRN("cloud error: %.*s", (int)TEDGE_MIN(len, 160), payload);
		return;
	}
#if defined(TEDGE_C8Y_OPERATION_JSON)
	if (strcmp(topic, OP_TOPIC) == 0) {
		char line[OP_LINE_MAX];

		if (tedge_operation_from_json(payload, line, sizeof(line))) {
			handle_operation(line);
		}
		return;
	}
#endif
	if (strcmp(topic, "s/ds") == 0) {
		TEDGE_LOG_DBG("s/ds: %.60s", payload);
		handle_operation(payload);
	}
}

static void on_message(const char *topic, const char *payload, size_t len,
		       bool dropped, void *ctx)
{
	TEDGE_UNUSED(ctx);
	if (dropped) {
		TEDGE_LOG_WRN("%s: %zu-byte message dropped (buffer is %zu)",
			      topic, len, sizeof(packet_buf) - PACKET_OVERHEAD);
		return;
	}
	handle_message(topic, payload, len);
}

/* ------------------------------------------------------------------------ */
/* Session                                                                   */
/* ------------------------------------------------------------------------ */

/* Read input (and keep the connection alive) for up to @p ms. */
static int pump(int ms)
{
	return tedge_mqtt_poll(&client, (uint32_t)ms);
}

/* TLS to the tenant: with the device certificate, or only verifying the
 * server when the device logs in with a user name and password. */
static int open_connection(void)
{
	uint8_t addr[4];
	int ret;

	ret = tedge_port_resolve(tedge_c8y_host(), addr, 10000);
	if (ret != 0) {
		TEDGE_LOG_ERR("DNS lookup of %s failed (%d)", tedge_c8y_host(),
			      ret);
		return -EHOSTUNREACH;
	}
	return tedge_port_connect(addr, C8Y_PORT,
				  (tedge_auth_username() != NULL)
					  ? TEDGE_PORT_TLS
					  : TEDGE_PORT_TLS_DEVICE,
				  tedge_c8y_host(), CONNACK_TIMEOUT_MS);
}

/* Subscribe to one filter (the MQTT Service refuses several per packet).
 * Cumulocity refuses a subscription with 0x80 while the device does not exist
 * yet, so retry rather than give up (spikes P8).
 */
static int subscribe_one(const char *topic)
{
	for (int attempt = 1; attempt <= SUBACK_RETRY_MAX; attempt++) {
		int ret = tedge_mqtt_subscribe(&client, topic, SUBACK_TIMEOUT_MS);

		if (ret == 0) {
			return 0;
		}
		if (ret != -EACCES && ret != -ETIMEDOUT) {
			TEDGE_LOG_ERR("subscribing to %s failed (%d)", topic, ret);
			return ret;
		}
		TEDGE_LOG_WRN("subscription to %s refused (attempt %d of %d); the "
			      "device may not exist yet", topic, attempt,
			      SUBACK_RETRY_MAX);
		ret = pump(5000); /* let 100 be processed, then retry */
		if (ret != 0) {
			return ret;
		}
	}
	TEDGE_LOG_ERR("could not subscribe to %s", topic);
	return -EACCES;
}

/* The operations this image can actually run. */
static void publish_supported_ops(void)
{
	char line[160];
	size_t n = tedge_appendf(line, sizeof(line), 0, "114");

	if (TEDGE_IS_ENABLED(TEDGE_RESTART)) {
		n = tedge_appendf(line, sizeof(line), n, ",c8y_Restart");
	}
	if (TEDGE_IS_ENABLED(TEDGE_REMOTE_ACCESS)) {
		n = tedge_appendf(line, sizeof(line), n, ",c8y_RemoteAccessConnect");
	}
	if (TEDGE_IS_ENABLED(TEDGE_FIRMWARE_UPDATE)) {
		n = tedge_appendf(line, sizeof(line), n, ",c8y_Firmware");
	}
	if (TEDGE_IS_ENABLED(TEDGE_SHELL_COMMAND)) {
		n = tedge_appendf(line, sizeof(line), n, ",c8y_Command");
	}
	if (TEDGE_IS_ENABLED(TEDGE_LOG_UPLOAD)) {
		n = tedge_appendf(line, sizeof(line), n, ",c8y_LogfileRequest");
	}
	for (int i = 0; i < OP_SLOTS; i++) {
		if (ops[i].name != NULL) {
			n = tedge_appendf(line, sizeof(line), n, ",%s", ops[i].name);
		}
	}
	/* A line cut short would offer half an operation name. */
	if (n >= sizeof(line)) {
		TEDGE_LOG_ERR("the supported operations do not fit in one message "
			      "(%zu bytes)", n);
	} else if (n > 3) {
		(void)tedge_c8y_publish_sr(line);
	}

#if defined(TEDGE_LOG_UPLOAD)
	/* The log types this image can actually produce, so an operator is
	 * never offered one the device does not have. */
	{
		char logs[160];

		if (tedge_log_types_line(logs, sizeof(logs)) > 0) {
			(void)tedge_c8y_publish_sr(logs);
		}
	}
#endif
}

static int publish_twin_impl(const char *fragment, const char *json)
{
	char topic[128];
	int rc;

	if (FREE_FORM_TOPICS) {
		snprintf(topic, sizeof(topic), "te/device/%s///twin/%s", device_id,
			 fragment);
		rc = publish(topic, json, QOS_1);
	} else {
		/* Core MQTT has no free-form topics: a direct inventory update,
		 * as thin-edge.io's mapper does. */
		char body[320];

		snprintf(topic, sizeof(topic),
			 "inventory/managedObjects/update/%s", device_id);
		snprintf(body, sizeof(body), "{\"%s\":%s}", fragment, json);
		rc = publish(topic, body, QOS_1);
	}
	if (rc == 0) {
		TEDGE_LOG_INF("twin %s: %s", fragment, json);
	}
	return rc;
}

/* Progress: free-form topic, at most once. Nothing on Core MQTT, which has
 * no free-form topics; the operation status is the report there. */
int tedge_c8y_publish_progress(const char *kind, const char *json)
{
	char topic[128];

	if (!FREE_FORM_TOPICS) {
		return -ENOTSUP;
	}
	snprintf(topic, sizeof(topic), "te/device/%s///progress/%s", device_id,
		 kind);
	TEDGE_LOG_DBG("progress/%s: %s", kind, json);
	return publish(topic, json, QOS_0);
}

#if defined(TEDGE_TELEMETRY)
/* Core MQTT has no free-form topics: one SmartREST 200 per series, taken
 * back out of the JSON the API built. */
static int smartrest_measurement(const char *type, const char *payload)
{
	size_t pos = 0;
	char series[40], value[24], line[128], when[32];
	int sent = 0;

	/* Static template 200 takes ",<unit>,<time>" after the value. The
	 * unit stays empty (the API's unit is advisory and the JSON does not
	 * carry it); the time is what makes a buffered reading truthful. */
	tedge_json_field(payload, "time", when, sizeof(when));
	while (tedge_json_next_number(payload, &pos, series, sizeof(series),
				      value, sizeof(value)) == 1) {
		snprintf(line, sizeof(line), "200,%s,%s,%s,,%s", type, series,
			 value, when);
		if (tedge_c8y_publish_sr(line) != 0) {
			return -ENOTCONN;
		}
		sent++;
	}
	return (sent > 0) ? 0 : -EINVAL;
}

/* Free-form te/ topics where the transport has them, SmartREST where it
 * does not; the application sees no difference. */
int tedge_c8y_publish_telemetry(enum tedge_msg_kind kind, const char *type,
				const char *payload)
{
	char topic[128];
	char line[256];

	if (!session_open) {
		return -ENOTCONN;
	}
	if (FREE_FORM_TOPICS) {
		static const char *const kinds[] = { "m", "e", "a", "a" };

		snprintf(topic, sizeof(topic), "te/device/%s///%s/%s", device_id,
			 kinds[kind], type);
		TEDGE_LOG_DBG("%s: %s", topic + strlen(topic) - strlen(type) - 3,
			payload);
		/* A measurement lost in a reconnect is one reading; an event or
		 * an alarm carries meaning, so those go at least once. */
		return publish(topic, payload,
			       (kind == TEDGE_MSG_MEASUREMENT)
				       ? QOS_0
				       : QOS_1);
	}

	/* Core MQTT: SmartREST. The payload is JSON either way, so the
	 * fields are taken back out of it here. */
	switch (kind) {
	case TEDGE_MSG_MEASUREMENT:
		return smartrest_measurement(type, payload);
	case TEDGE_MSG_EVENT: {
		char text[160], quoted[180], when[32];

		tedge_json_field(payload, "text", text, sizeof(text));
		tedge_json_field(payload, "time", when, sizeof(when));
		(void)tedge_sr_quote(text, quoted, sizeof(quoted));
		snprintf(line, sizeof(line), "400,%s,%s,%s", type, quoted,
			 when);
		return tedge_c8y_publish_sr(line);
	}
	case TEDGE_MSG_ALARM: {
		char text[160], quoted[180], severity[16], when[32];
		static const struct {
			const char *name;
			const char *template;
		} map[] = {
			{ "critical", "301" }, { "major", "302" },
			{ "minor", "303" },    { "warning", "304" },
		};
		const char *tmpl = "304";

		tedge_json_field(payload, "severity", severity, sizeof(severity));
		tedge_json_field(payload, "text", text, sizeof(text));
		tedge_json_field(payload, "time", when, sizeof(when));
		for (size_t i = 0; i < TEDGE_ARRAY_SIZE(map); i++) {
			if (strcmp(severity, map[i].name) == 0) {
				tmpl = map[i].template;
				break;
			}
		}
		(void)tedge_sr_quote(text, quoted, sizeof(quoted));
		snprintf(line, sizeof(line), "%s,%s,%s,%s", tmpl, type, quoted,
			 when);
		return tedge_c8y_publish_sr(line);
	}
	case TEDGE_MSG_ALARM_CLEAR:
	default:
		snprintf(line, sizeof(line), "306,%s", type);
		return tedge_c8y_publish_sr(line);
	}
}
#endif /* TEDGE_TELEMETRY */

static void publish_health(void)
{
	char topic[128];
	char body[96];
	time_t now = 0;

	if (!FREE_FORM_TOPICS) {
		return;
	}
	(void)tedge_port_time_now(&now);
	snprintf(topic, sizeof(topic), "te/device/%s/service/tedge-zephyr/status/health",
		 device_id);
	snprintf(body, sizeof(body), "{\"status\":\"up\",\"time\":%lld}",
		 (long long)now);
	(void)publish(topic, body, QOS_1);
}

/* 100, subscriptions, 114, 117, s/uat, then state (design D6). */
static int session_start(void)
{
	/* A new session: Cumulocity sends its pending operations again, so
	 * whatever this client thought it was running is no longer true. */
	op_executing = false;
	op_queued = 0;

	const struct tedge_id *id = tedge_identity();
	char line[160];
	char agent[160];

	snprintf(line, sizeof(line), "100,%s,%s", id->name, id->type);
	(void)tedge_c8y_publish_sr(line);
	(void)pump(500);

	for (size_t i = 0; i < TEDGE_ARRAY_SIZE(sub_topics); i++) {
		int rc = subscribe_one(sub_topics[i]);

		if (rc != 0) {
			return rc;
		}
	}

	publish_supported_ops();

	/* Ask Cumulocity for anything still PENDING for this device.
	 *
	 * This is what makes operations raised while the device was away
	 * arrive at all, and it governs delivery on both channels — the
	 * SmartREST s/ds topic and devicecontrol/notifications alike. Without
	 * it an operation can sit PENDING until something else prompts a
	 * redelivery, which looks from the cloud like a device that has
	 * stopped listening. */
	op_interrupted_report();
	(void)tedge_c8y_publish_sr("500");
	{
#if defined(TEDGE_PARAMETERS_SELF)
		int required = tedge_self_required_interval_min();
#else
		int required = TEDGE_REQUIRED_INTERVAL_MIN;
#endif

		if (required > 0) {
			snprintf(line, sizeof(line), "117,%d", required);
			(void)tedge_c8y_publish_sr(line);
		}
	}
	if (tedge_auth_username() == NULL) {
		/* Only certificate devices get a token. */
		(void)publish("s/uat", "", QOS_0);
	}

#if defined(TEDGE_FIRMWARE_UPDATE)
	{
		char ver[24] = "";
		char fw[96];

		if (tedge_fw_running_version(ver, sizeof(ver)) == 0) {
			TEDGE_LOG_INF("firmware: running %s %s", id->firmware_name, ver);
			snprintf(fw, sizeof(fw), "115,%s,%s", id->firmware_name,
				 ver);
			(void)tedge_c8y_publish_sr(fw);
		} else {
			TEDGE_LOG_WRN("firmware: cannot read the running version");
		}
	}
#endif
	snprintf(agent, sizeof(agent),
		 "{\"name\":\"tedge-zephyr\",\"version\":\"%s\","
		 "\"transport\":\"%s\",\"firmware\":\"%s %s\"}",
		 tedge_version(), FREE_FORM_TOPICS ? "c8y-mqtt-service" : "c8y-core-mqtt",
		 id->firmware_name, id->firmware_version);
	(void)publish_twin_impl("tedge_Agent", agent);
#if defined(TEDGE_REMOTE_ACCESS)
	{
		char ra[224];

		/* State, not an event: a reboot must not leave a stale count. */
		if (tedge_ra_twin(ra, sizeof(ra)) == 0) {
			(void)tedge_publish_twin("tedge_RemoteAccess", ra);
		}
	}
#endif
	publish_health();

#if defined(TEDGE_FIRMWARE_UPDATE)
	/* Confirm a test boot, or report an image MCUboot rolled back. */
	tedge_fw_on_connected();
#endif
#if defined(TEDGE_CERT_RENEWAL)
	{
		char cert[160];

		if (tedge_cert_twin(cert, sizeof(cert)) == 0) {
			(void)tedge_publish_twin("tedge_Certificate", cert);
		}
	}
#endif
#if defined(TEDGE_RESTART)
	if (restart_pending_after_boot) {
		op_finish_by_id(restart_op_id, "c8y_Restart", true, NULL);
		(void)tedge_port_kv_delete(TEDGE_KEY_RESTART);
		restart_pending_after_boot = false;
		TEDGE_LOG_INF("restart operation reported as successful");
	}
#endif
	return pump(500);
}

/* ------------------------------------------------------------------------ */
/* Transport interface                                                       */
/* ------------------------------------------------------------------------ */

static int c8y_connect(void)
{
	int64_t t0 = tedge_port_uptime_ms();
	int ret;

	if (tedge_c8y_host()[0] == '\0') {
		TEDGE_LOG_ERR("no Cumulocity host: set " TEDGE_CONFIG_NAME(TEDGE_C8Y_URL)
			      " or call "
			"tedge_set_c8y_url()");
		return -EDESTADDRREQ;
	}

	if (!ca_registered) {
		ret = tedge_port_tls_set_ca(server_ca, sizeof(server_ca));
		if (ret != 0) {
			TEDGE_LOG_ERR("could not register the trust anchor (%d)",
				      ret);
			return ret;
		}
		ca_registered = true;
	}

	ret = tedge_auth_prepare(device_id, sizeof(device_id));
	if (ret != 0) {
		return ret;
	}

	conn = open_connection();
	if (conn < 0) {
		TEDGE_LOG_ERR("TLS connect to %s:%d failed (%d)",
			      tedge_c8y_host(), C8Y_PORT, conn);
		return conn;
	}
	tedge_mqtt_init(&client, conn, packet_buf, sizeof(packet_buf),
			on_message, NULL);
	{
		const struct tedge_mqtt_connect opts = {
			.client_id = device_id,
			.user = tedge_auth_username(),
			.password = tedge_auth_password(),
			.keepalive_s = 60,
		};

		ret = tedge_mqtt_connect(&client, &opts, CONNACK_TIMEOUT_MS);
	}
	if (ret != 0) {
		TEDGE_LOG_ERR("no CONNACK from %s:%d (ret=%d result=%d)",
			      tedge_c8y_host(), C8Y_PORT, ret,
			      client.connack_code);
		tedge_port_close(conn);
		conn = -1;
		return ret;
	}
	session_open = true;
	connected_at = tedge_port_uptime_ms();
	TEDGE_LOG_INF("connected to %s:%d as \"%s\" in %lld ms", tedge_c8y_host(),
		C8Y_PORT, device_id, (long long)(connected_at - t0));

	ret = session_start();
	if (ret != 0) {
		tedge_port_close(conn);
		conn = -1;
		session_open = false;
	}
	return ret;
}

static int c8y_poll(int timeout_ms)
{
	int ret = pump(timeout_ms);

	if (ret != 0) {
		return ret;
	}
	if (!session_open) {
		return -ENOTCONN;
	}
#if defined(TEDGE_RESTART)
	if (restart_requested) {
		handle_restart();
	}
#endif
#if defined(TEDGE_FIRMWARE_UPDATE)
	{
		struct tedge_fw_event ev;
		char sr[224], quoted[160];

		while (tedge_fw_poll_event(&ev) == 0) {
			(void)tedge_sr_quote(ev.text, quoted, sizeof(quoted));
			switch (ev.type) {
			case TEDGE_FW_INSTALLED: {
				char id[24];

				/* sr holds "115," with the longest name (47)
				 * and event text (143); a smaller buffer could
				 * cut the version. */
				_Static_assert(sizeof(sr) >= sizeof("115,,") +
					       sizeof(tedge_identity()->firmware_name) +
					       sizeof(ev.text),
					       "the installed-version message could be cut");
				snprintf(sr, sizeof(sr), "115,%s,%s",
					 tedge_identity()->firmware_name, ev.text);
				(void)tedge_c8y_publish_sr(sr);
				/* The update finished after a reboot, so its
				 * id comes back from settings rather than
				 * from the operation in flight. */
				(void)tedge_fw_operation_id(id, sizeof(id));
				op_finish_by_id(id, "c8y_Firmware", true, NULL);
				continue;
			}
			case TEDGE_FW_REBOOTING:
				snprintf(sr, sizeof(sr),
					 "400,c8y_FirmwareInstalling,%s", quoted);
				break;
			case TEDGE_FW_REVERTED:
			case TEDGE_FW_FAILED:
			default: {
				char id[24];

				(void)tedge_fw_operation_id(id, sizeof(id));
				op_finish_by_id(id, "c8y_Firmware", false,
						ev.text);
				continue;
			}
			}
			(void)tedge_c8y_publish_sr(sr);
		}
	}
#endif
#if defined(TEDGE_SHELL_COMMAND)
	{
		struct tedge_shell_event ev;

		while (tedge_shell_poll_event(&ev) == 0) {
			/* The output keeps its lines, but a long answer is
			 * cut. A command with a lot to say belongs behind a
			 * log type. */
			op_finished_lines("c8y_Command", ev.rc == 0, ev.output);
		}
	}
#endif
#if defined(TEDGE_LOG_UPLOAD)
	{
		struct tedge_log_event ev;

		while (tedge_log_poll_event(&ev) == 0) {
			if (ev.rc == 0) {
				op_succeeded("c8y_LogfileRequest", ev.url);
			} else {
				op_failed("c8y_LogfileRequest", ev.reason);
			}
		}
	}
#endif
#if defined(TEDGE_REMOTE_ACCESS)
	{
		struct tedge_ra_event ev;
		static int64_t twin_due;
		char sr[272], quoted[208];

		while (tedge_ra_poll_event(&ev) == 0) {
			(void)tedge_sr_quote(ev.text, quoted, sizeof(quoted));
			switch (ev.type) {
			case TEDGE_RA_UP:
				op_succeeded("c8y_RemoteAccessConnect", NULL);
				snprintf(sr, sizeof(sr),
					 "400,c8y_RemoteAccessOpened,%s", quoted);
				break;
			case TEDGE_RA_FAILED:
				op_failed("c8y_RemoteAccessConnect", ev.text);
				continue;
			default:
				snprintf(sr, sizeof(sr),
					 "400,c8y_RemoteAccessClosed,%s", quoted);
				break;
			}
			(void)tedge_c8y_publish_sr(sr);
			/* The seat is freed just after the event is posted. */
			twin_due = tedge_port_uptime_ms() + 1000;
		}
		if (twin_due != 0 && tedge_port_uptime_ms() >= twin_due) {
			char ra[224];

			twin_due = 0;
			if (tedge_ra_twin(ra, sizeof(ra)) == 0) {
				(void)tedge_publish_twin("tedge_RemoteAccess", ra);
			}
		}
	}
#endif
	/* Anything that finished above freed the gate, so the next operation
	 * can start — on this thread, like every other dispatch. */
	run_queued_operation();
#if defined(TEDGE_CERT_RENEWAL)
	/* Renew the certificate while there is still plenty of time. */
	tedge_cert_renew_tick();
#endif
	if (tedge_auth_username() == NULL && jwt_at != 0 &&
	    tedge_port_uptime_ms() - jwt_at > JWT_REFRESH_MS) {
		jwt_at = tedge_port_uptime_ms(); /* one request per interval */
		(void)publish("s/uat", "", QOS_0);
	}
	return 0;
}

static void c8y_disconnect(void)
{
	if (session_open) {
		tedge_mqtt_disconnect(&client);
		session_open = false;
	}
	tedge_port_close(conn);
	conn = -1;
	/* A session the broker closed just after CONNACK, three times in a
	 * row, usually means another client uses this ID. */
	if (connected_at != 0 &&
	    tedge_port_uptime_ms() - connected_at < TAKEOVER_WINDOW_MS) {
		if (++takeover_strikes >= TAKEOVER_STRIKES) {
			TEDGE_LOG_WRN("the broker keeps closing this session moments "
				"after it connects: another client may be using "
				"the ID \"%s\"", device_id);
			takeover_strikes = 0;
		}
	} else {
		takeover_strikes = 0;
	}
	connected_at = 0;
}

static int c8y_publish_twin(const char *fragment, const char *json)
{
	return publish_twin_impl(fragment, json);
}

static const struct tedge_transport c8y_transport = {
	.connect = c8y_connect,
	.poll = c8y_poll,
	.disconnect = c8y_disconnect,
	.publish_twin = c8y_publish_twin,
};

const struct tedge_transport *tedge_transport_get(void)
{
	static bool op_loaded;

	if (!op_loaded) {
		op_loaded = true;
		op_interrupted_load();
	}
#if defined(TEDGE_RESTART)
	static bool marker_loaded;

	if (!marker_loaded) {
		marker_loaded = true;
		restart_pending_after_boot = restart_marker_load();
		if (restart_pending_after_boot) {
			TEDGE_LOG_INF("a restart operation is waiting to be reported");
		}
	}
#endif
	return &c8y_transport;
}
