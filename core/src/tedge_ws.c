/* SPDX-License-Identifier: Apache-2.0
 *
 * A WebSocket client (RFC 6455) for the remote-access tunnel.
 *
 * The handshake is an HTTP request read with the client's HTTP parser,
 * which stops at the end of the 101 response and leaves whatever followed
 * it in the same read to the frame reader. Frames from the client are
 * masked, as the RFC requires; frames from the server are not, but a
 * masked one is unmasked rather than refused.
 */

#include "tedge_ws.h"
#include "tedge_http.h"
#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/net.h>
#include <tedge/port/os.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ------------------------------------------------------------------------ */
/* The handshake                                                             */
/* ------------------------------------------------------------------------ */

struct handshake {
	char accept[32];
};

static void on_header(const char *name, const char *piece, size_t len,
		      size_t offset, bool last, void *ctx)
{
	struct handshake *hs = ctx;

	TEDGE_UNUSED(last);
	if (strcmp(name, "sec-websocket-accept") == 0 &&
	    offset + len < sizeof(hs->accept)) {
		memcpy(hs->accept + offset, piece, len);
		hs->accept[offset + len] = '\0';
	}
}

int tedge_ws_connect(struct tedge_ws *ws, int conn, const char *host,
		     const char *path, const char *const *headers, uint8_t *rx,
		     size_t rx_cap, uint32_t timeout_ms)
{
	struct handshake hs = { 0 };
	struct tedge_http_parser p;
	uint8_t nonce[16];
	char key[32], expect[32], keyed[sizeof(key) + sizeof(WS_GUID)];
	uint8_t digest[20];
	int64_t deadline = tedge_port_uptime_ms() + timeout_ms;
	int n, rc;

	memset(ws, 0, sizeof(*ws));
	ws->conn = conn;
	ws->rx = rx;
	ws->rx_cap = rx_cap;

	for (size_t i = 0; i < sizeof(nonce); i += 4) {
		uint32_t r = tedge_port_random32();

		memcpy(nonce + i, &r, 4);
	}
	(void)tedge_base64_encode(nonce, sizeof(nonce), key, sizeof(key));
	snprintf(keyed, sizeof(keyed), "%s" WS_GUID, key);
	tedge_sha1(keyed, strlen(keyed), digest);
	(void)tedge_base64_encode(digest, sizeof(digest), expect,
				  sizeof(expect));

	/* The head, in the receive buffer, then the caller's headers as they
	 * are: one of them is a token of a kilobyte. */
	n = snprintf((char *)rx, rx_cap,
		     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
		     "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
		     "Sec-WebSocket-Version: 13\r\n",
		     path, host, key);
	if (n < 0 || (size_t)n >= rx_cap) {
		return -ENOSPC;
	}
	rc = tedge_http_send_all(conn, rx, (size_t)n);
	for (const char *const *h = headers; rc == 0 && h != NULL && *h != NULL;
	     h++) {
		rc = tedge_http_send_all(conn, *h, strlen(*h));
	}
	if (rc == 0) {
		rc = tedge_http_send_all(conn, "\r\n", 2);
	}
	if (rc != 0) {
		return rc;
	}

	tedge_http_parser_init(&p, on_header, NULL, &hs);
	while (!p.done) {
		int64_t left = deadline - tedge_port_uptime_ms();

		if (left <= 0) {
			return -ETIMEDOUT;
		}
		n = tedge_port_recv(conn, rx, rx_cap, (uint32_t)left);
		if (n == -EAGAIN) {
			return -ETIMEDOUT;
		}
		if (n <= 0) {
			return (n == 0) ? -ECONNRESET : n;
		}
		rc = tedge_http_parse(&p, rx, (size_t)n);
		if (rc != 0) {
			return rc;
		}
	}
	if (p.status != 101) {
		return (p.status == 401 || p.status == 403) ? -EACCES
							     : -ECONNREFUSED;
	}
	if (strcmp(hs.accept, expect) != 0) {
		return -EPROTO;
	}
	/* Frames the server sent straight after its answer. */
	ws->rx_len = (size_t)n;
	ws->rx_off = p.used;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Sending                                                                   */
/* ------------------------------------------------------------------------ */

static int send_frame(struct tedge_ws *ws, uint8_t opcode, const void *data,
		      size_t len)
{
	uint8_t out[14 + 256];
	const uint8_t *in = data;
	uint32_t r = tedge_port_random32();
	uint8_t mask[4];
	size_t h = 0, done = 0;

	memcpy(mask, &r, sizeof(mask));
	out[h++] = 0x80 | opcode; /* FIN: every message is one frame */
	if (len < 126) {
		out[h++] = 0x80 | (uint8_t)len;
	} else if (len <= 0xFFFF) {
		out[h++] = 0x80 | 126;
		out[h++] = (uint8_t)(len >> 8);
		out[h++] = (uint8_t)len;
	} else {
		out[h++] = 0x80 | 127;
		for (int i = 7; i >= 0; i--) {
			out[h++] = (uint8_t)((uint64_t)len >> (8 * i));
		}
	}
	memcpy(out + h, mask, sizeof(mask));
	h += sizeof(mask);

	/* The header goes out with the first piece of the payload. */
	do {
		size_t n = TEDGE_MIN(len - done, sizeof(out) - h);
		int rc;

		for (size_t i = 0; i < n; i++) {
			out[h + i] = in[done + i] ^ mask[(done + i) & 3];
		}
		rc = tedge_http_send_all(ws->conn, out, h + n);
		if (rc != 0) {
			return rc;
		}
		done += n;
		h = 0;
	} while (done < len);
	return 0;
}

int tedge_ws_send(struct tedge_ws *ws, const void *data, size_t len)
{
	return send_frame(ws, TEDGE_WS_OP_BINARY, data, len);
}

void tedge_ws_close(struct tedge_ws *ws)
{
	static const uint8_t normal[] = { 0x03, 0xE8 }; /* 1000 */

	(void)send_frame(ws, TEDGE_WS_OP_CLOSE, normal, sizeof(normal));
}

/* ------------------------------------------------------------------------ */
/* Receiving                                                                 */
/* ------------------------------------------------------------------------ */

enum { R_HEADER, R_PAYLOAD };

/* Bytes of header still to come, once the first two are in. */
static uint8_t header_need(const uint8_t *hdr)
{
	uint8_t len7 = hdr[1] & 0x7F;
	uint8_t need = 2;

	need += (len7 == 126) ? 2 : (len7 == 127) ? 8 : 0;
	need += (hdr[1] & 0x80) ? 4 : 0;
	return need;
}

static void header_done(struct tedge_ws *ws)
{
	uint8_t len7 = ws->hdr[1] & 0x7F;
	size_t at = 2;

	ws->opcode = ws->hdr[0] & 0x0F;
	ws->masked = (ws->hdr[1] & 0x80) != 0;
	if (len7 == 126) {
		ws->remaining = ((uint64_t)ws->hdr[2] << 8) | ws->hdr[3];
		at = 4;
	} else if (len7 == 127) {
		ws->remaining = 0;
		for (int i = 0; i < 8; i++) {
			ws->remaining = (ws->remaining << 8) | ws->hdr[2 + i];
		}
		at = 10;
	} else {
		ws->remaining = len7;
	}
	if (ws->masked) {
		memcpy(ws->mask, ws->hdr + at, 4);
	}
	ws->mask_pos = 0;
	ws->ctrl_len = 0;
	ws->state = R_PAYLOAD;
}

/* A control frame is complete: answer a ping, and report a close. */
static int control_done(struct tedge_ws *ws)
{
	switch (ws->opcode) {
	case TEDGE_WS_OP_PING:
		return send_frame(ws, TEDGE_WS_OP_PONG, ws->ctrl, ws->ctrl_len);
	case TEDGE_WS_OP_CLOSE:
		/* Echo the status back, as the closing handshake asks. */
		(void)send_frame(ws, TEDGE_WS_OP_CLOSE, ws->ctrl,
				 TEDGE_MIN(ws->ctrl_len, 2));
		return -ENOTCONN;
	default:
		return 0; /* a pong, or something unknown: ignored */
	}
}

int tedge_ws_recv(struct tedge_ws *ws, uint8_t *buf, size_t len,
		  uint32_t timeout_ms)
{
	size_t got = 0;
	bool waited = false;

	if (ws->closed) {
		return -ENOTCONN;
	}
	while (got < len) {
		uint8_t c;

		if (ws->rx_off == ws->rx_len) {
			int n;

			if (got > 0) {
				break; /* hand over what has arrived */
			}
			n = tedge_port_recv(ws->conn, ws->rx, ws->rx_cap,
					    waited ? 0 : timeout_ms);
			waited = true;
			if (n == -EAGAIN) {
				return -EAGAIN;
			}
			if (n <= 0) {
				return (n == 0) ? -ECONNRESET : n;
			}
			ws->rx_len = (size_t)n;
			ws->rx_off = 0;
		}

		if (ws->state == R_HEADER) {
			c = ws->rx[ws->rx_off++];
			ws->hdr[ws->hdr_len++] = c;
			if (ws->hdr_len == 2) {
				ws->hdr_need = header_need(ws->hdr);
			}
			if (ws->hdr_len >= 2 && ws->hdr_len == ws->hdr_need) {
				header_done(ws);
				ws->hdr_len = 0;
			} else {
				continue;
			}
			if (ws->opcode >= TEDGE_WS_OP_CLOSE &&
			    ws->remaining > sizeof(ws->ctrl)) {
				return -EPROTO; /* control frames are short */
			}
		} else {
			size_t avail = ws->rx_len - ws->rx_off;
			size_t n = (size_t)TEDGE_MIN((uint64_t)avail, ws->remaining);
			uint8_t *src = ws->rx + ws->rx_off;

			if (ws->opcode >= TEDGE_WS_OP_CLOSE) {
				memcpy(ws->ctrl + ws->ctrl_len, src, n);
				for (size_t i = 0; ws->masked && i < n; i++) {
					ws->ctrl[ws->ctrl_len + i] ^=
						ws->mask[ws->mask_pos++ & 3];
				}
				ws->ctrl_len += (uint8_t)n;
			} else {
				n = TEDGE_MIN(n, len - got);
				memcpy(buf + got, src, n);
				for (size_t i = 0; ws->masked && i < n; i++) {
					buf[got + i] ^=
						ws->mask[ws->mask_pos++ & 3];
				}
				got += n;
			}
			ws->rx_off += n;
			ws->remaining -= n;
		}

		if (ws->state == R_PAYLOAD && ws->remaining == 0) {
			ws->state = R_HEADER;
			if (ws->opcode >= TEDGE_WS_OP_CLOSE) {
				int rc = control_done(ws);

				if (rc == -ENOTCONN && got > 0) {
					ws->closed = true;
					return (int)got;
				}
				if (rc != 0) {
					return rc;
				}
			}
		}
	}
	return (got > 0) ? (int)got : -EAGAIN;
}
