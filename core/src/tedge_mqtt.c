/* SPDX-License-Identifier: Apache-2.0
 *
 * A small MQTT 3.1.1 client for the Cumulocity session and bootstrap.
 *
 * Only what those need: CONNECT, SUBSCRIBE one filter at a time (the MQTT
 * Service refuses several per packet), PUBLISH at QoS 0 and 1 both ways,
 * PUBACK for what the server sends at QoS 1, and PINGREQ. An incoming
 * packet is read whole into the caller's buffer, so a message is never
 * handed over in pieces, and a message too big for it is skipped, not cut.
 */

#include "tedge_mqtt.h"
#include "tedge_util.h"

#include <tedge/port/net.h>
#include <tedge/port/os.h>

#include <errno.h>
#include <string.h>

#define PKT_CONNECT     0x10
#define PKT_CONNACK     0x20
#define PKT_PUBLISH     0x30
#define PKT_PUBACK      0x40
#define PKT_SUBSCRIBE   0x82 /* with the reserved flags 0010 */
#define PKT_SUBACK      0x90
#define PKT_PINGREQ     0xC0
#define PKT_PINGRESP    0xD0
#define PKT_DISCONNECT  0xE0

/* How much of the polling interval one wait for input may take, so the
 * keepalive is looked at often enough. */
#define WAIT_SLICE_MS 1000

enum { R_TYPE, R_LENGTH, R_BODY };

/* ------------------------------------------------------------------------ */
/* Writing                                                                   */
/* ------------------------------------------------------------------------ */

static size_t put_length(uint8_t *out, uint32_t len)
{
	size_t n = 0;

	do {
		uint8_t b = len & 0x7F;

		len >>= 7;
		out[n++] = b | (len ? 0x80 : 0);
	} while (len > 0);
	return n;
}

static size_t put_u16(uint8_t *out, uint16_t v)
{
	out[0] = (uint8_t)(v >> 8);
	out[1] = (uint8_t)v;
	return 2;
}

/* A UTF-8 string field: its length, then its bytes. */
static size_t put_str(uint8_t *out, const char *s, size_t len)
{
	put_u16(out, (uint16_t)len);
	memcpy(out + 2, s, len);
	return 2 + len;
}

static int send_bytes(struct tedge_mqtt *m, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len > 0) {
		int n = tedge_port_send(m->conn, p, len);

		if (n <= 0) {
			return (n == 0) ? -ECONNRESET : n;
		}
		p += n;
		len -= (size_t)n;
	}
	m->last_tx = tedge_port_uptime_ms();
	return 0;
}

static uint16_t next_id(struct tedge_mqtt *m)
{
	if (++m->next_id == 0) {
		m->next_id = 1; /* 0 is not a packet id */
	}
	return m->next_id;
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                   */
/* ------------------------------------------------------------------------ */

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void handle_publish(struct tedge_mqtt *m)
{
	uint8_t qos = (m->type >> 1) & 3;
	bool dropped = m->got > m->length; /* see R_BODY: body was skipped */
	size_t kept = TEDGE_MIN((size_t)m->length, m->rx_cap - 1);
	uint16_t tlen, id = 0;
	size_t at;

	if (kept < 2) {
		return;
	}
	tlen = get_u16(m->rx);
	at = 2 + tlen;
	if (at > kept) {
		return; /* not even the topic fits: nothing to report */
	}
	if (qos > 0) {
		if (at + 2 > kept) {
			return;
		}
		id = get_u16(m->rx + at);
		at += 2;
	}
	/* Acknowledged before it is handled, as the server would otherwise
	 * send it again on a session that may never come back. */
	if (qos == 1) {
		uint8_t ack[4] = { PKT_PUBACK, 2 };

		put_u16(ack + 2, id);
		(void)send_bytes(m, ack, sizeof(ack));
	}
	if (m->on_message == NULL) {
		return;
	}
	if (dropped) {
		/* The topic is at the start of the buffer, and was kept. */
		char topic[64];
		size_t n = TEDGE_MIN((size_t)tlen, sizeof(topic) - 1);

		memcpy(topic, m->rx + 2, n);
		topic[n] = '\0';
		m->on_message(topic, NULL, m->length - at, true, m->ctx);
		return;
	}
	/* Topic and payload in place: the topic is NUL-terminated over the
	 * packet id or the payload's first byte, so it moves down by two. */
	memmove(m->rx, m->rx + 2, tlen);
	m->rx[tlen] = '\0';
	m->rx[m->length] = '\0'; /* room was kept for it */
	m->on_message((const char *)m->rx, (const char *)m->rx + at,
		      m->length - at, false, m->ctx);
}

static void handle_packet(struct tedge_mqtt *m)
{
	switch (m->type & 0xF0) {
	case PKT_CONNACK:
		if (m->length >= 2) {
			m->connack = true;
			m->connack_code = m->rx[1];
		}
		break;
	case PKT_SUBACK:
		if (m->length >= 3) {
			m->suback_id = get_u16(m->rx);
			m->suback_code = m->rx[2];
		}
		break;
	case PKT_PUBLISH:
		handle_publish(m);
		break;
	case PKT_PINGRESP:
		m->ping_sent_at = 0;
		break;
	default:
		break; /* PUBACK for what we sent, and anything else: ignored */
	}
}

int tedge_mqtt_input(struct tedge_mqtt *m, const uint8_t *data, size_t len)
{
	size_t i = 0;

	while (i < len) {
		switch (m->state) {
		case R_TYPE:
			m->type = data[i++];
			m->length = 0;
			m->length_shift = 0;
			m->got = 0;
			m->state = R_LENGTH;
			break;

		case R_LENGTH: {
			uint8_t b = data[i++];

			if (m->length_shift > 21) {
				return -EBADMSG; /* more than four bytes */
			}
			m->length |= (uint32_t)(b & 0x7F) << m->length_shift;
			m->length_shift += 7;
			if (b & 0x80) {
				break;
			}
			if (m->length == 0) {
				handle_packet(m);
				m->state = R_TYPE;
			} else {
				m->state = R_BODY;
			}
			break;
		}

		case R_BODY: {
			/* Keep what fits, with a byte spare for a NUL; skip the
			 * rest. A skipped body is marked by counting it past
			 * the length (handle_publish() reads that). */
			size_t n = TEDGE_MIN(len - i, (size_t)(m->length - m->got));
			size_t room = (m->got < m->rx_cap - 1)
					      ? m->rx_cap - 1 - m->got : 0;

			memcpy(m->rx + m->got, data + i, TEDGE_MIN(n, room));
			m->got += n;
			i += n;
			if (m->got == m->length) {
				if (m->length > m->rx_cap - 1) {
					m->got = m->length + 1;
				}
				handle_packet(m);
				m->state = R_TYPE;
			}
			break;
		}
		}
	}
	return 0;
}

/* Read what the connection has for up to @p wait_ms. */
static int read_some(struct tedge_mqtt *m, uint32_t wait_ms)
{
	uint8_t buf[128];
	int n = tedge_port_recv(m->conn, buf, sizeof(buf), wait_ms);

	if (n == -EAGAIN) {
		return 0;
	}
	if (n <= 0) {
		return (n == 0) ? -ECONNRESET : n;
	}
	return tedge_mqtt_input(m, buf, (size_t)n);
}

/* ------------------------------------------------------------------------ */
/* The session                                                               */
/* ------------------------------------------------------------------------ */

void tedge_mqtt_init(struct tedge_mqtt *m, int conn, uint8_t *rx,
		     size_t rx_cap, tedge_mqtt_message_fn on_message,
		     void *ctx)
{
	memset(m, 0, sizeof(*m));
	m->conn = conn;
	m->rx = rx;
	m->rx_cap = rx_cap;
	m->on_message = on_message;
	m->ctx = ctx;
}

int tedge_mqtt_connect(struct tedge_mqtt *m,
		       const struct tedge_mqtt_connect *opts,
		       uint32_t timeout_ms)
{
	size_t id_len = strlen(opts->client_id);
	size_t user_len = opts->user ? strlen(opts->user) : 0;
	size_t pass_len = opts->password ? strlen(opts->password) : 0;
	uint32_t body = 10 + 2 + id_len;
	uint8_t head[16];
	size_t h = 0;
	int64_t deadline;
	int rc;

	if (opts->user != NULL) {
		body += 2 + user_len + 2 + pass_len;
	}
	m->keepalive_s = opts->keepalive_s;

	/* Fixed header and variable header: "MQTT", level 4, the flags and
	 * the keepalive. */
	head[h++] = PKT_CONNECT;
	h += put_length(head + h, body);
	h += put_str(head + h, "MQTT", 4);
	head[h++] = 4;
	head[h++] = 0x02 | (opts->user ? 0xC0 : 0); /* clean session */
	h += put_u16(head + h, opts->keepalive_s);
	rc = send_bytes(m, head, h);

	/* The payload: client id, then user name and password. Each is
	 * sent as it is, without being copied into one buffer. */
	{
		const char *fields[3] = { opts->client_id, opts->user,
					  opts->password };
		size_t lens[3] = { id_len, user_len, pass_len };

		for (int f = 0; rc == 0 && f < 3; f++) {
			uint8_t len[2];

			if (fields[f] == NULL) {
				continue;
			}
			put_u16(len, (uint16_t)lens[f]);
			rc = send_bytes(m, len, 2);
			if (rc == 0) {
				rc = send_bytes(m, fields[f], lens[f]);
			}
		}
	}
	if (rc != 0) {
		return rc;
	}

	deadline = tedge_port_uptime_ms() + timeout_ms;
	while (!m->connack) {
		int64_t left = deadline - tedge_port_uptime_ms();

		if (left <= 0) {
			return -ETIMEDOUT;
		}
		rc = read_some(m, (uint32_t)TEDGE_MIN(left, WAIT_SLICE_MS));
		if (rc != 0) {
			return rc;
		}
	}
	return (m->connack_code == 0) ? 0 : -ECONNREFUSED;
}

int tedge_mqtt_subscribe(struct tedge_mqtt *m, const char *filter,
			 uint32_t timeout_ms)
{
	size_t flen = strlen(filter);
	uint16_t id = next_id(m);
	uint8_t head[8];
	size_t h = 0;
	int64_t deadline;
	int rc;

	head[h++] = PKT_SUBSCRIBE;
	h += put_length(head + h, (uint32_t)(2 + 2 + flen + 1));
	h += put_u16(head + h, id);
	h += put_u16(head + h, (uint16_t)flen);
	rc = send_bytes(m, head, h);
	if (rc == 0) {
		rc = send_bytes(m, filter, flen);
	}
	if (rc == 0) {
		static const uint8_t qos1 = 1;

		rc = send_bytes(m, &qos1, 1);
	}
	if (rc != 0) {
		return rc;
	}

	deadline = tedge_port_uptime_ms() + timeout_ms;
	m->suback_id = 0;
	while (m->suback_id != id) {
		int64_t left = deadline - tedge_port_uptime_ms();

		if (left <= 0) {
			return -ETIMEDOUT;
		}
		rc = read_some(m, (uint32_t)TEDGE_MIN(left, WAIT_SLICE_MS));
		if (rc != 0) {
			return rc;
		}
	}
	return (m->suback_code == 0x80) ? -EACCES : 0;
}

int tedge_mqtt_publish(struct tedge_mqtt *m, const char *topic,
		       const void *payload, size_t len, int qos)
{
	size_t tlen = strlen(topic);
	uint32_t body = (uint32_t)(2 + tlen + (qos > 0 ? 2 : 0) + len);
	uint8_t head[8];
	size_t h = 0;
	int rc;

	head[h++] = PKT_PUBLISH | (qos > 0 ? 0x02 : 0);
	h += put_length(head + h, body);
	h += put_u16(head + h, (uint16_t)tlen);
	rc = send_bytes(m, head, h);
	if (rc == 0) {
		rc = send_bytes(m, topic, tlen);
	}
	if (rc == 0 && qos > 0) {
		uint8_t id[2];

		put_u16(id, next_id(m));
		rc = send_bytes(m, id, 2);
	}
	if (rc == 0 && len > 0) {
		rc = send_bytes(m, payload, len);
	}
	return rc;
}

/* A ping when the connection has been quiet for the keepalive; an error
 * when the last one went unanswered for as long. */
static int keep_alive(struct tedge_mqtt *m)
{
	int64_t now = tedge_port_uptime_ms();
	int64_t period = (int64_t)m->keepalive_s * 1000;

	if (m->keepalive_s == 0) {
		return 0;
	}
	if (m->ping_sent_at != 0) {
		return (now - m->ping_sent_at > period) ? -ETIMEDOUT : 0;
	}
	if (now - m->last_tx >= period) {
		static const uint8_t ping[2] = { PKT_PINGREQ, 0 };
		int rc = send_bytes(m, ping, sizeof(ping));

		if (rc != 0) {
			return rc;
		}
		m->ping_sent_at = now;
	}
	return 0;
}

int tedge_mqtt_poll(struct tedge_mqtt *m, uint32_t timeout_ms)
{
	int64_t end = tedge_port_uptime_ms() + timeout_ms;

	do {
		int64_t left = end - tedge_port_uptime_ms();
		int rc = read_some(m, (uint32_t)TEDGE_MAX(
					      TEDGE_MIN(left, WAIT_SLICE_MS), 0));

		if (rc == 0) {
			rc = keep_alive(m);
		}
		if (rc != 0) {
			return rc;
		}
	} while (tedge_port_uptime_ms() < end);
	return 0;
}

void tedge_mqtt_disconnect(struct tedge_mqtt *m)
{
	static const uint8_t disconnect[2] = { PKT_DISCONNECT, 0 };

	(void)send_bytes(m, disconnect, sizeof(disconnect));
}
