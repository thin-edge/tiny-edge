/* SPDX-License-Identifier: Apache-2.0
 *
 * Telemetry: the measurements, events and alarms an application asks the
 * client to send.
 *
 * A call builds the message there and then — that is when the values and the
 * time are true — and puts it in a bounded buffer. The client thread drains
 * the buffer while it is connected, so an application never waits for the
 * network and the MQTT client stays owned by one thread.
 *
 * The buffer is small and in RAM. When it fills, the oldest measurement is
 * dropped and counted, so a device that cannot keep up says so instead of
 * growing its memory or losing data quietly. Events and alarms are never
 * dropped for a measurement: "the pump stopped" matters more than the
 * pressure reading beside it.
 */

#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/config.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>
#include <tedge/port/time.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

TEDGE_LOG_DECLARE();

/* One message in the buffer: a header, then the topic suffix and payload. */
struct queued {
	uint16_t len;   /* of this record, including the header */
	uint8_t kind;   /* enum tedge_msg_kind */
	char text[];    /* "<type>\0<payload>\0" */
};

#define RING_BYTES TEDGE_TELEMETRY_BUFFER_BYTES

static uint8_t ring[RING_BYTES];
static size_t ring_used;
static uint32_t dropped;
TEDGE_PORT_MUTEX_DEFINE(ring_lock);

/* ------------------------------------------------------------------------ */
/* The buffer                                                                */
/* ------------------------------------------------------------------------ */

static struct queued *first(void)
{
	return (ring_used > 0) ? (struct queued *)ring : NULL;
}

static void pop_first(void)
{
	struct queued *q = first();

	if (q == NULL) {
		return;
	}
	ring_used -= q->len;
	memmove(ring, ring + q->len, ring_used);
}

/* Drop the oldest measurement to make room; returns false when there is
 * nothing left to drop (the buffer holds only events and alarms). */
static bool drop_oldest_measurement(void)
{
	size_t off = 0;

	while (off < ring_used) {
		struct queued *q = (struct queued *)(ring + off);

		if (q->kind == TEDGE_MSG_MEASUREMENT) {
			size_t len = q->len;

			memmove(ring + off, ring + off + len,
				ring_used - off - len);
			ring_used -= len;
			dropped++;
			return true;
		}
		off += q->len;
	}
	return false;
}

/* Longest type and payload a queued message may carry, with the NUL. The
 * payload is always built in a buffer this size; the type comes from the
 * application, so it is checked here rather than cut short on the way out,
 * where it would name a different type. */
#define TYPE_MAX    40
#define PAYLOAD_MAX 224

static int enqueue(enum tedge_msg_kind kind, const char *type,
		   const char *payload)
{
	size_t tlen = strlen(type) + 1;
	size_t plen = strlen(payload) + 1;
	size_t need = sizeof(struct queued) + tlen + plen;
	int rc = 0;

	if (tlen > TYPE_MAX) {
		return -EINVAL;
	}
	if (plen > PAYLOAD_MAX || need > RING_BYTES) {
		return -EMSGSIZE;
	}
	tedge_port_mutex_lock(&ring_lock);
	while (ring_used + need > RING_BYTES) {
		if (!drop_oldest_measurement()) {
			/* Only events and alarms left, and still no room: the
			 * new message is the one that has to go. */
			rc = -ENOMEM;
			goto out;
		}
	}
	{
		struct queued *q = (struct queued *)(ring + ring_used);

		q->len = (uint16_t)need;
		q->kind = (uint8_t)kind;
		memcpy(q->text, type, tlen);
		memcpy(q->text + tlen, payload, plen);
		ring_used += need;
	}
out:
	tedge_port_mutex_unlock(&ring_lock);
	if (rc == 0) {
		tedge_telemetry_wake();
	} else {
		TEDGE_LOG_WRN("telemetry: no room for a %s message",
			(kind == TEDGE_MSG_MEASUREMENT) ? "measurement"
							: "event or alarm");
	}
	return rc;
}

/* Called from the client thread while connected. */
void tedge_telemetry_flush(void)
{
	for (;;) {
		char type[TYPE_MAX];
		char payload[PAYLOAD_MAX];
		size_t tlen;
		enum tedge_msg_kind kind;
		struct queued *q;

		tedge_port_mutex_lock(&ring_lock);
		q = first();
		if (q == NULL) {
			tedge_port_mutex_unlock(&ring_lock);
			return;
		}
		kind = (enum tedge_msg_kind)q->kind;
		/* enqueue() checked both lengths, so neither copy cuts. */
		tlen = strlen(q->text) + 1;
		memcpy(type, q->text, tlen);
		memcpy(payload, q->text + tlen, strlen(q->text + tlen) + 1);
		tedge_port_mutex_unlock(&ring_lock);

		if (tedge_c8y_publish_telemetry(kind, type, payload) != 0) {
			return; /* still offline: keep it for the next round */
		}
		tedge_port_mutex_lock(&ring_lock);
		pop_first();
		tedge_port_mutex_unlock(&ring_lock);
	}
}

uint32_t tedge_telemetry_dropped(void)
{
	return dropped;
}

/* ------------------------------------------------------------------------ */
/* Building messages                                                         */
/* ------------------------------------------------------------------------ */

/* "time":"2026-09-20T10:15:00Z", or nothing at all when the clock has not
 * been set yet: a message without a time is better than no message, and the
 * cloud stamps it on arrival. */
/* Appends "time":"<ISO 8601>", at @p n of @p buf (tedge_appendf()). */
static size_t put_time(char *buf, size_t len, size_t n, int64_t timestamp_ms)
{
	struct tm tm;
	char iso[24];
	time_t secs;

	if (timestamp_ms > 0) {
		secs = (time_t)(timestamp_ms / 1000);
	} else {
		if (tedge_port_time_now(&secs) != 0) {
			return n;
		}
	}
	gmtime_r(&secs, &tm);
	strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return tedge_appendf(buf, len, n, "\"time\":\"%s\",", iso);
}

/* Formats a double with two decimals; Zephyr's printf has no floats by
 * default and telemetry should not force them on an application. */
static size_t put_number(char *buf, size_t len, size_t n, double v)
{
	long scaled = (long)(v * 100.0 + (v >= 0 ? 0.5 : -0.5));
	long whole = scaled / 100;
	long frac = scaled % 100;

	return tedge_appendf(buf, len, n, "%s%ld.%02ld",
			     (scaled < 0 && whole == 0) ? "-" : "", whole,
			     frac < 0 ? -frac : frac);
}

int tedge_publish_measurement(const char *type,
			      const struct tedge_measurement_value *values,
			      size_t count, int64_t timestamp_ms)
{
	char json[224];
	size_t n = 0;

	if (!TEDGE_IS_ENABLED(TEDGE_TELEMETRY)) {
		return -ENOTSUP;
	}
	if (type == NULL || values == NULL || count == 0) {
		return -EINVAL;
	}
	n = tedge_appendf(json, sizeof(json), n, "{");
	n = put_time(json, sizeof(json), n, timestamp_ms);
	for (size_t i = 0; i < count && n < sizeof(json); i++) {
		n = tedge_appendf(json, sizeof(json), n, "%s\"%s\":",
				  i ? "," : "", values[i].series);
		n = put_number(json, sizeof(json), n, values[i].value);
	}
	n = tedge_appendf(json, sizeof(json), n, "}");
	if (n >= sizeof(json)) {
		return -EMSGSIZE;
	}
	return enqueue(TEDGE_MSG_MEASUREMENT, type, json);
}

int tedge_publish_event(const char *type, const char *text,
			int64_t timestamp_ms)
{
	char json[224];
	char escaped[160];
	size_t n = 0;

	if (!TEDGE_IS_ENABLED(TEDGE_TELEMETRY)) {
		return -ENOTSUP;
	}
	if (type == NULL || text == NULL) {
		return -EINVAL;
	}
	tedge_json_escape(text, escaped, sizeof(escaped));
	n = tedge_appendf(json, sizeof(json), n, "{");
	n = put_time(json, sizeof(json), n, timestamp_ms);
	n = tedge_appendf(json, sizeof(json), n, "\"text\":\"%s\"}", escaped);
	if (n >= sizeof(json)) {
		return -EMSGSIZE;
	}
	return enqueue(TEDGE_MSG_EVENT, type, json);
}

static const char *severity_name(enum tedge_alarm_severity s)
{
	switch (s) {
	case TEDGE_ALARM_CRITICAL:
		return "critical";
	case TEDGE_ALARM_MAJOR:
		return "major";
	case TEDGE_ALARM_MINOR:
		return "minor";
	default:
		return "warning";
	}
}

int tedge_raise_alarm(const char *type, enum tedge_alarm_severity severity,
		      const char *text)
{
	char json[224];
	char escaped[140];
	size_t n = 0;

	if (!TEDGE_IS_ENABLED(TEDGE_TELEMETRY)) {
		return -ENOTSUP;
	}
	if (type == NULL || text == NULL) {
		return -EINVAL;
	}
	tedge_json_escape(text, escaped, sizeof(escaped));
	n = tedge_appendf(json, sizeof(json), n, "{");
	n = put_time(json, sizeof(json), n, 0);
	n = tedge_appendf(json, sizeof(json), n,
			  "\"severity\":\"%s\",\"text\":\"%s\"}",
			  severity_name(severity), escaped);
	if (n >= sizeof(json)) {
		return -EMSGSIZE;
	}
	return enqueue(TEDGE_MSG_ALARM, type, json);
}

int tedge_clear_alarm(const char *type)
{
	if (!TEDGE_IS_ENABLED(TEDGE_TELEMETRY)) {
		return -ENOTSUP;
	}
	if (type == NULL) {
		return -EINVAL;
	}
	/* thin-edge.io clears an alarm with an empty payload. */
	return enqueue(TEDGE_MSG_ALARM_CLEAR, type, "");
}
