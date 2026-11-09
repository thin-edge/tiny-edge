/* SPDX-License-Identifier: Apache-2.0
 *
 * Downloading a file over HTTP(S) into a sink the caller provides.
 *
 * Two rules make this worth keeping in one place, away from the features
 * that use it (firmware update now; configuration later):
 *
 *   - A redirect is followed, and its Location can be nearly a kilobyte (a
 *     release asset).
 *   - The cloud token must reach the tenant and nowhere else, including
 *     after a redirect.
 *
 * The HTTP client (tedge_http.c) decodes a chunked body, which is how
 * Cumulocity serves binaries.
 */

#include "tedge_http.h"
#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/log.h>
#include <tedge/port/net.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEDGE_LOG_DECLARE();

#define RX_SIZE 1024

struct download_state {
	struct tedge_download *req;
	int64_t written;
	/* Headers worth keeping: a redirect's target, and the length a
	 * "Content-Range: bytes 0-887801/887802" reply states. */
	char redirect_to[TEDGE_URL_MAX];
	bool redirect_too_long;
	char range[64];
	char length[24];
};

/* ------------------------------------------------------------------------ */
/* The response                                                              */
/* ------------------------------------------------------------------------ */

static void copy_piece(char *dst, size_t cap, const char *piece, size_t len,
		       size_t offset, bool *too_long)
{
	if (offset + len >= cap) {
		if (too_long != NULL) {
			*too_long = true;
		}
		return;
	}
	memcpy(dst + offset, piece, len);
	dst[offset + len] = '\0';
}

static void on_header(const char *name, const char *piece, size_t len,
		      size_t offset, bool last, void *ctx)
{
	struct download_state *st = ctx;

	if (strcmp(name, "location") == 0) {
		copy_piece(st->redirect_to, sizeof(st->redirect_to), piece, len,
			   offset, &st->redirect_too_long);
	} else if (strcmp(name, "content-length") == 0) {
		/* Chunked transfers have none; progress then has no
		 * percentage. */
		copy_piece(st->length, sizeof(st->length), piece, len, offset,
			   NULL);
		if (last && strtoll(st->length, NULL, 10) > 0) {
			st->req->total = strtoll(st->length, NULL, 10);
		}
	} else if (strcmp(name, "content-range") == 0) {
		copy_piece(st->range, sizeof(st->range), piece, len, offset,
			   NULL);
		if (last) {
			/* "bytes <first>-<last>/<total>"; anything else is
			 * ignored. */
			const char *slash = strrchr(st->range, '/');

			if (slash != NULL && slash[1] != '*') {
				int64_t total = strtoll(slash + 1, NULL, 10);

				if (total > 0) {
					st->req->total = total;
				}
			}
		}
	}
}

static int on_body(int status, const void *data, size_t len, void *ctx)
{
	struct download_state *st = ctx;
	int rc;

	/* Only the file goes to the sink: not a redirect's or an error's
	 * page. */
	if (status != 200 && status != 206) {
		return 0;
	}
	rc = st->req->sink(data, len, st->req->user_data);
	if (rc != 0) {
		return rc; /* stops the transfer */
	}
	st->written += len;
	if (st->req->progress != NULL) {
		st->req->progress(st->written, st->req->user_data);
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* One request                                                               */
/* ------------------------------------------------------------------------ */

static int fetch_once(const char *url, struct download_state *st,
		      bool *redirected)
{
	static uint8_t rx[RX_SIZE];
	const char *headers[3] = { 0 };
	char auth[1100];
	char host[TEDGE_HOST_MAX];
	const char *path;
	uint16_t port;
	bool tls;
	int conn, ret, status = 0, h = 0;

	*redirected = false;
	ret = tedge_url_split(url, &tls, host, sizeof(host), &port, &path);
	if (ret != 0) {
		TEDGE_LOG_ERR("download: cannot parse the URL");
		return ret;
	}
	/* A host outside the tenant may present a chain that takes ~13 s to
	 * verify in software. The port's handshake budget has to allow for
	 * it: on Zephyr that is the NET_SOCKETS_TLS_CONNECT_TIMEOUT option,
	 * which the README asks applications to raise. */
	conn = tedge_http_connect(host, port, tls, st->req->timeout_ms);
	if (conn < 0) {
		TEDGE_LOG_ERR("download: cannot connect to %s (%d)", host, conn);
		return conn;
	}

	/* Ask for the whole file as a range: a server that supports ranges
	 * answers 206 with "Content-Range: bytes 0-<last>/<total>", which is
	 * the only way to learn the size of a chunked transfer (Cumulocity
	 * serves binaries chunked). A server that ignores it answers 200 as
	 * usual. */
	headers[h++] = "Range: bytes=0-\r\n";

	/* The token is for the tenant only, never a redirect target. */
	if (st->req->token != NULL && st->req->token[0] != '\0' &&
	    tedge_url_is_tenant(host)) {
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n",
			 st->req->token);
		headers[h++] = auth;
	}

	st->redirect_to[0] = '\0';
	st->redirect_too_long = false;
	st->range[0] = '\0';
	st->length[0] = '\0';
	{
		struct tedge_http_request req = {
			.method = "GET",
			.host = host,
			.path = path,
			.headers = headers,
			.on_header = on_header,
			.on_body = on_body,
			.ctx = st,
			.buf = rx,
			.buf_len = sizeof(rx),
			.timeout_ms = (uint32_t)st->req->timeout_ms,
		};

		ret = tedge_http_exchange(conn, &req, &status);
	}
	memset(auth, 0, sizeof(auth));
	tedge_port_close(conn);

	if (ret < 0) {
		TEDGE_LOG_ERR("download: the transfer failed (%d)", ret);
		return ret;
	}
	if (status >= 300 && status < 400) {
		if (st->redirect_too_long) {
			TEDGE_LOG_ERR("download: the redirect target is too long");
			return -ENOSPC;
		}
		if (st->redirect_to[0] != '\0') {
			*redirected = true;
			return 0;
		}
	}
	if (status != 200 && status != 206) {
		TEDGE_LOG_ERR("download: the server answered %d", status);
		return -EIO;
	}
	return 0;
}

int tedge_download(struct tedge_download *req)
{
	static struct download_state st; /* the redirect target is 1 KB */
	char url[TEDGE_URL_MAX];
	char next[TEDGE_URL_MAX];
	int hops = 0;

	if (req == NULL || req->url == NULL || req->sink == NULL) {
		return -EINVAL;
	}
	if (snprintf(url, sizeof(url), "%s", req->url) >= (int)sizeof(url)) {
		return -ENOSPC;
	}
	if (req->timeout_ms == 0) {
		req->timeout_ms = 60000;
	}
	memset(&st, 0, sizeof(st));
	st.req = req;

	for (;;) {
		bool redirected;
		int ret = fetch_once(url, &st, &redirected);

		if (ret != 0) {
			return ret;
		}
		if (!redirected) {
			break;
		}
		if (++hops > TEDGE_HTTP_MAX_REDIRECTS) {
			TEDGE_LOG_ERR("download: too many redirects");
			return -ELOOP;
		}
		ret = tedge_url_resolve(url, st.redirect_to, next, sizeof(next));
		if (ret != 0) {
			return ret;
		}
		memcpy(url, next, sizeof(url));
		TEDGE_LOG_INF("download: following a redirect (hop %d)", hops);
	}
	req->written = st.written;
	return 0;
}
