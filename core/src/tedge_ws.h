/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief A WebSocket client (RFC 6455), over a connection from the port.
 *
 * What remote access needs: open a session with extra headers, then move
 * binary data both ways as a byte stream. Message boundaries are not kept
 * (the tunnel has none); pings are answered and a close is returned.
 */

#ifndef TEDGE_WS_H_
#define TEDGE_WS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEDGE_WS_OP_CONT   0x0
#define TEDGE_WS_OP_TEXT   0x1
#define TEDGE_WS_OP_BINARY 0x2
#define TEDGE_WS_OP_CLOSE  0x8
#define TEDGE_WS_OP_PING   0x9
#define TEDGE_WS_OP_PONG   0xA

struct tedge_ws {
	int conn;
	/* Received bytes not yet parsed: the caller's buffer. */
	uint8_t *rx;
	size_t rx_cap;
	size_t rx_len;
	size_t rx_off;

	/* The frame being read. */
	uint8_t state;
	uint8_t hdr[14];
	uint8_t hdr_len;
	uint8_t hdr_need;
	uint8_t opcode;
	bool masked;
	uint8_t mask[4];
	uint8_t mask_pos;
	uint64_t remaining;
	uint8_t ctrl[125];
	uint8_t ctrl_len;
	/* The server's close arrived behind data that was handed over. */
	bool closed;
};

/**
 * Open a WebSocket on @p conn (already connected, TLS as needed): send the
 * upgrade request for @p path with the extra @p headers ("Name: value\r\n"
 * each, NULL-terminated), and check the server's answer.
 *
 * @param rx Receive buffer, kept for the session; 1 KB is enough.
 * @return 0; -EACCES when the server refused the credentials (401, 403);
 *         -ECONNREFUSED for any other answer than 101; -EPROTO when its
 *         Sec-WebSocket-Accept is wrong; or another negative errno.
 */
int tedge_ws_connect(struct tedge_ws *ws, int conn, const char *host,
		     const char *path, const char *const *headers, uint8_t *rx,
		     size_t rx_cap, uint32_t timeout_ms);

/** Send @p len bytes as one binary message. @return 0, or a negative
 *  errno. */
int tedge_ws_send(struct tedge_ws *ws, const void *data, size_t len);

/**
 * Receive message data into @p buf, waiting up to @p timeout_ms for the
 * connection when nothing is buffered. Returns what is at hand, without
 * waiting for more once some data has arrived.
 *
 * @return The bytes received; -EAGAIN when none arrived in time;
 *         -ENOTCONN when the server closed the session; or another
 *         negative errno when the connection failed.
 */
int tedge_ws_recv(struct tedge_ws *ws, uint8_t *buf, size_t len,
		  uint32_t timeout_ms);

/** Send a normal close (1000). Best effort; the caller closes @p conn. */
void tedge_ws_close(struct tedge_ws *ws);

#endif /* TEDGE_WS_H_ */
