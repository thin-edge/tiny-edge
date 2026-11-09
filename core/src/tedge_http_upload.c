/* SPDX-License-Identifier: Apache-2.0
 *
 * Sending a body to the cloud over HTTP(S): the other half of
 * tedge_http_download.c, and it keeps that file's two rules — the token
 * reaches the tenant and nowhere else, and a redirect is followed rather
 * than failed.
 *
 * The body is either a string the caller already has, or written by a
 * producer callback. Either way its length is known before the request
 * starts, because Cumulocity wants a Content-Length and this device has no
 * room to hold a log twice. A source that runs out early is padded, and one
 * that has more to say is cut: the length promised in the header is the
 * length that goes on the wire, whatever the source does underneath.
 */

#include "tedge_http.h"
#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/log.h>
#include <tedge/port/net.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

TEDGE_LOG_DECLARE();

#define RX_SIZE   512
#define SEND_SIZE 256

/* ------------------------------------------------------------------------ */
/* The body                                                                  */
/* ------------------------------------------------------------------------ */

/* The sink the producer writes through while the request body is on the
 * wire: it sends, counts, and refuses more than the length promised in the
 * header. */
struct wire {
	int conn;
	size_t left;
	int rc;
};

static int wire_sink(const void *data, size_t len, void *ctx)
{
	struct wire *w = ctx;
	int ret;

	if (w->left == 0) {
		return -ENOSPC; /* the producer has more to say than it said */
	}
	len = TEDGE_MIN(len, w->left);
	ret = tedge_http_send_all(w->conn, data, len);
	if (ret < 0) {
		w->rc = ret;
		return ret;
	}
	w->left -= len;
	return 0;
}

static int send_body(int conn, void *ctx)
{
	struct tedge_upload *up = ctx;
	struct wire w = { .conn = conn, .left = up->length };
	uint8_t pad[SEND_SIZE];

	(void)up->producer(wire_sink, &w, up->user_data);
	if (w.rc != 0) {
		return w.rc;
	}
	/* A producer that ran short still owes the server the bytes the
	 * Content-Length promised; spaces are harmless in a log. */
	memset(pad, ' ', sizeof(pad));
	while (w.left > 0) {
		size_t n = TEDGE_MIN(w.left, sizeof(pad));
		int ret = tedge_http_send_all(conn, pad, n);

		if (ret < 0) {
			return ret;
		}
		w.left -= n;
	}
	return 0;
}

/* The Location header, when the caller wants it. */
static void on_header(const char *name, const char *piece, size_t len,
		      size_t offset, bool last, void *ctx)
{
	struct tedge_upload *up = ctx;

	TEDGE_UNUSED(last);
	if (strcmp(name, "location") == 0 && up->location != NULL &&
	    offset + len < up->location_len) {
		memcpy(up->location + offset, piece, len);
		up->location[offset + len] = '\0';
	}
}

/* ------------------------------------------------------------------------ */
/* One request                                                               */
/* ------------------------------------------------------------------------ */

int tedge_upload(struct tedge_upload *req)
{
	static uint8_t rx[RX_SIZE];
	const char *headers[4] = { 0 };
	char auth[1100];
	char disposition[96];
	char host[TEDGE_HOST_MAX];
	const char *path;
	uint16_t port;
	bool tls;
	int conn, ret, h = 0;

	if (req == NULL || req->url == NULL ||
	    (req->payload == NULL && req->producer == NULL)) {
		return -EINVAL;
	}
	if (req->timeout_ms == 0) {
		req->timeout_ms = 30000;
	}
	if (req->location != NULL && req->location_len > 0) {
		req->location[0] = '\0';
	}
	ret = tedge_url_split(req->url, &tls, host, sizeof(host), &port, &path);
	if (ret != 0) {
		TEDGE_LOG_ERR("upload: cannot parse the URL");
		return ret;
	}
	conn = tedge_http_connect(host, port, tls, req->timeout_ms);
	if (conn < 0) {
		TEDGE_LOG_ERR("upload: cannot connect to %s (%d)", host, conn);
		return conn;
	}

	/* Cumulocity answers 201 with an empty body and no Location unless it
	 * is asked for JSON, and the created object's id is the whole point of
	 * the request. */
	headers[h++] = "Accept: application/json\r\n";
	if (req->filename != NULL) {
		snprintf(disposition, sizeof(disposition),
			 "Content-Disposition: attachment; filename=\"%s\"\r\n",
			 req->filename);
		headers[h++] = disposition;
	}
	/* The token is for the tenant only. */
	if (req->token != NULL && req->token[0] != '\0' &&
	    tedge_url_is_tenant(host)) {
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n",
			 req->token);
		headers[h++] = auth;
	}

	if (req->payload != NULL && req->length == 0) {
		req->length = strlen(req->payload);
	}
	req->status = 0;
	{
		struct tedge_http_request hr = {
			.method = "POST",
			.host = host,
			.path = path,
			.headers = headers,
			.content_type = (req->content_type != NULL)
						? req->content_type
						: "text/plain",
			.payload = req->payload,
			.send = (req->payload == NULL) ? send_body : NULL,
			.body_len = req->length,
			.on_header = on_header,
			.ctx = req,
			.buf = rx,
			.buf_len = sizeof(rx),
			.timeout_ms = (uint32_t)req->timeout_ms,
		};

		ret = tedge_http_exchange(conn, &hr, &req->status);
	}
	memset(auth, 0, sizeof(auth));
	tedge_port_close(conn);

	if (ret < 0) {
		TEDGE_LOG_ERR("upload: the request failed (%d)", ret);
		return ret;
	}
	if (req->status < 200 || req->status >= 300) {
		TEDGE_LOG_ERR("upload: the server answered %d", req->status);
		return -EIO;
	}
	return 0;
}
