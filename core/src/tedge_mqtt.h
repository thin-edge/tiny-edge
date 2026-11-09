/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief A small MQTT 3.1.1 client, over a connection from the port.
 *
 * What a Cumulocity session needs: connect with a client id and, for
 * basic authentication, a user name and password; subscribe to one filter
 * at a time; publish at QoS 0 or 1; receive messages at either; and keep
 * the connection alive. One thread owns a session.
 *
 * Every incoming packet is read whole into one buffer, the caller's: a
 * message larger than it is dropped (and reported as such) rather than
 * cut.
 */

#ifndef TEDGE_MQTT_H_
#define TEDGE_MQTT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * A message from the server. @p payload is NUL-terminated (the buffer
 * keeps a byte for it). @p dropped is true when the message was larger than
 * the buffer: then only @p topic is valid, and @p payload is NULL.
 */
typedef void (*tedge_mqtt_message_fn)(const char *topic, const char *payload,
				      size_t len, bool dropped, void *ctx);

struct tedge_mqtt {
	int conn;
	tedge_mqtt_message_fn on_message;
	void *ctx;
	/* Incoming packets; holds the largest message the session takes. */
	uint8_t *rx;
	size_t rx_cap;

	uint16_t keepalive_s;
	uint16_t next_id;
	int64_t last_tx;
	int64_t ping_sent_at; /* 0 when no ping is outstanding */

	/* The packet being read. */
	uint8_t state;
	uint8_t type;
	uint32_t length;
	uint8_t length_shift;
	uint32_t got;

	/* Acknowledgements the caller may be waiting for. */
	bool connack;
	uint8_t connack_code;
	uint16_t suback_id;
	uint8_t suback_code;
};

struct tedge_mqtt_connect {
	const char *client_id;
	const char *user;     /**< NULL without basic authentication */
	const char *password;
	uint16_t keepalive_s;
};

/** Ready @p m for a session on @p conn, with @p rx as its packet buffer. */
void tedge_mqtt_init(struct tedge_mqtt *m, int conn, uint8_t *rx,
		     size_t rx_cap, tedge_mqtt_message_fn on_message,
		     void *ctx);

/**
 * Send CONNECT (clean session) and wait up to @p timeout_ms for CONNACK.
 *
 * @return 0; -ECONNREFUSED when the server refused (the code is in
 *         m->connack_code); -ETIMEDOUT; or another negative errno.
 */
int tedge_mqtt_connect(struct tedge_mqtt *m,
		       const struct tedge_mqtt_connect *opts,
		       uint32_t timeout_ms);

/**
 * Subscribe to one @p filter at QoS 1 and wait up to @p timeout_ms for the
 * SUBACK, handling messages that arrive meanwhile.
 *
 * @return 0; -EACCES when the server refused it (0x80); -ETIMEDOUT; or
 *         another negative errno.
 */
int tedge_mqtt_subscribe(struct tedge_mqtt *m, const char *filter,
			 uint32_t timeout_ms);

/** Publish @p len bytes to @p topic at @p qos (0 or 1). A QoS 1 message
 *  is not retried: the session is clean, and a lost one is lost with it.
 *  @return 0, or a negative errno. */
int tedge_mqtt_publish(struct tedge_mqtt *m, const char *topic,
		       const void *payload, size_t len, int qos);

/**
 * Read and handle what arrives for up to @p timeout_ms, and keep the
 * session alive: a ping when the connection has been quiet for the
 * keepalive, and an error when the server does not answer it in time.
 *
 * @return 0, or a negative errno when the session is gone.
 */
int tedge_mqtt_poll(struct tedge_mqtt *m, uint32_t timeout_ms);

/** Send DISCONNECT. Best effort; the caller closes the connection. */
void tedge_mqtt_disconnect(struct tedge_mqtt *m);

/**
 * Feed received bytes to the packet reader. For the unit tests;
 * tedge_mqtt_poll() does this with what the connection delivers.
 *
 * @return 0, or -EBADMSG for a malformed packet.
 */
int tedge_mqtt_input(struct tedge_mqtt *m, const uint8_t *data, size_t len);

#endif /* TEDGE_MQTT_H_ */
