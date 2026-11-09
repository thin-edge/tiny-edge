/* SPDX-License-Identifier: Apache-2.0
 *
 * A small HTTP/1.1 client: one request per connection, the response parsed
 * as it arrives.
 *
 * The parser takes the response a byte range at a time and never needs a
 * whole header or body in memory. That matters for two replies the client
 * gets: a redirect whose Location is nearly a kilobyte (a release asset),
 * and a firmware image sent chunked (Cumulocity serves binaries that way),
 * whose chunks are decoded here and handed on as plain bytes.
 */

#include "tedge_http.h"
#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/net.h>
#include <tedge/port/os.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a name lookup may take, within the caller's budget. */
#define RESOLVE_TIMEOUT_MS 10000

enum {
	S_STATUS,
	S_LINE_START,
	S_NAME,
	S_VALUE_SPACE,
	S_VALUE,
	S_BODY_LENGTH,
	S_BODY_TO_CLOSE,
	S_CHUNK_SIZE,
	S_CHUNK_EXT,
	S_CHUNK_DATA,
	S_CHUNK_END,
	S_TRAILER,
	S_DONE,
};

void tedge_http_parser_init(struct tedge_http_parser *p,
			    tedge_http_header_fn on_header,
			    tedge_http_body_fn on_body, void *ctx)
{
	memset(p, 0, sizeof(*p));
	p->on_header = on_header;
	p->on_body = on_body;
	p->ctx = ctx;
	p->content_length = -1;
	p->state = S_STATUS;
}

/* "HTTP/1.1 200 OK": the three digits after the first space. */
static int parse_status_line(struct tedge_http_parser *p)
{
	const char *sp;

	p->line[TEDGE_MIN(p->line_len, sizeof(p->line) - 1)] = '\0';
	if (strncmp(p->line, "HTTP/1.", 7) != 0) {
		return -EBADMSG;
	}
	sp = strchr(p->line, ' ');
	if (sp == NULL || !isdigit((unsigned char)sp[1]) ||
	    !isdigit((unsigned char)sp[2]) || !isdigit((unsigned char)sp[3])) {
		return -EBADMSG;
	}
	p->status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
	return 0;
}

/* The two headers the parser itself needs, read from the value it kept. */
static void header_done(struct tedge_http_parser *p)
{
	size_t n = TEDGE_MIN(p->line_len, sizeof(p->line) - 1);

	while (n > 0 && (p->line[n - 1] == ' ' || p->line[n - 1] == '\t')) {
		n--;
	}
	p->line[n] = '\0';
	if (strcmp(p->name, "content-length") == 0) {
		char *end;
		long long v = strtoll(p->line, &end, 10);

		if (end != p->line && v >= 0) {
			p->content_length = v;
		}
	} else if (strcmp(p->name, "transfer-encoding") == 0) {
		for (char *c = p->line; *c != '\0'; c++) {
			*c = (char)tolower((unsigned char)*c);
		}
		if (strstr(p->line, "chunked") != NULL) {
			p->chunked = true;
		}
	}
	if (p->on_header != NULL) {
		p->on_header(p->name, "", 0, p->value_len, true, p->ctx);
	}
}

/* The blank line after the headers: decide how the body is delimited. */
static int headers_done(struct tedge_http_parser *p)
{
	if (p->status == 101) {
		/* Switching protocols: what follows is not HTTP. */
		p->state = S_DONE;
		p->done = true;
		return 0;
	}
	if (p->status >= 100 && p->status < 200) {
		/* An interim response: the real one follows. */
		p->state = S_STATUS;
		p->line_len = 0;
		p->content_length = -1;
		p->chunked = false;
		return 0;
	}
	if (p->status == 204 || p->status == 304) {
		p->state = S_DONE;
	} else if (p->chunked) {
		p->state = S_CHUNK_SIZE;
		p->remaining = 0;
		p->line_len = 0;
	} else if (p->content_length == 0) {
		p->state = S_DONE;
	} else if (p->content_length > 0) {
		p->state = S_BODY_LENGTH;
		p->remaining = (uint64_t)p->content_length;
	} else {
		p->state = S_BODY_TO_CLOSE;
	}
	p->done = (p->state == S_DONE);
	return 0;
}

static int body(struct tedge_http_parser *p, const uint8_t *data, size_t len)
{
	if (len == 0 || p->on_body == NULL) {
		return 0;
	}
	p->rc = p->on_body(p->status, data, len, p->ctx);
	return p->rc;
}

static int hex_digit(uint8_t c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	c = (uint8_t)tolower(c);
	return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

int tedge_http_parse(struct tedge_http_parser *p, const uint8_t *data,
		     size_t len)
{
	size_t i = 0;

	p->used = 0;
	if (p->rc != 0) {
		return p->rc;
	}
	while (i < len && p->state != S_DONE) {
		uint8_t c = data[i];

		switch (p->state) {
		case S_STATUS:
			i++;
			if (c == '\n') {
				int rc = parse_status_line(p);

				if (rc != 0) {
					return rc;
				}
				p->state = S_LINE_START;
			} else if (c != '\r' && p->line_len < sizeof(p->line)) {
				p->line[p->line_len++] = (char)c;
			}
			break;

		case S_LINE_START:
			i++;
			if (c == '\n') {
				(void)headers_done(p);
			} else if (c != '\r') {
				p->name[0] = (char)tolower(c);
				p->name_len = 1;
				p->state = S_NAME;
			}
			break;

		case S_NAME:
			i++;
			if (c == ':') {
				p->name[TEDGE_MIN(p->name_len,
						  sizeof(p->name) - 1)] = '\0';
				p->value_len = 0;
				p->line_len = 0;
				p->state = S_VALUE_SPACE;
			} else if (c == '\n') {
				return -EBADMSG; /* a header line with no ':' */
			} else if (p->name_len < sizeof(p->name) - 1) {
				p->name[p->name_len++] = (char)tolower(c);
			}
			break;

		case S_VALUE_SPACE:
			if (c == ' ' || c == '\t') {
				i++;
				break;
			}
			p->state = S_VALUE;
			break;

		case S_VALUE: {
			size_t start = i;

			while (i < len && data[i] != '\r' && data[i] != '\n') {
				i++;
			}
			if (i > start) {
				size_t run = i - start;
				size_t keep = TEDGE_MIN(run, sizeof(p->line) -
								     p->line_len);

				memcpy(p->line + p->line_len, data + start, keep);
				p->line_len += keep;
				if (p->on_header != NULL) {
					p->on_header(p->name,
						     (const char *)data + start,
						     run, p->value_len, false,
						     p->ctx);
				}
				p->value_len += run;
			}
			if (i < len) {
				if (data[i] == '\n') {
					header_done(p);
					p->state = S_LINE_START;
				}
				i++; /* the '\r' or the '\n' */
			}
			break;
		}

		case S_BODY_LENGTH: {
			size_t run = (size_t)TEDGE_MIN((uint64_t)(len - i),
						       p->remaining);
			int rc = body(p, data + i, run);

			if (rc != 0) {
				return rc;
			}
			i += run;
			p->remaining -= run;
			if (p->remaining == 0) {
				p->state = S_DONE;
			}
			break;
		}

		case S_BODY_TO_CLOSE: {
			int rc = body(p, data + i, len - i);

			if (rc != 0) {
				return rc;
			}
			i = len;
			break;
		}

		case S_CHUNK_SIZE: {
			int d = hex_digit(c);

			i++;
			if (d >= 0) {
				if (p->remaining > (UINT64_MAX >> 4)) {
					return -EBADMSG;
				}
				p->remaining = (p->remaining << 4) | (uint64_t)d;
				p->line_len++;
			} else if (c == ';' || c == ' ' || c == '\t') {
				p->state = S_CHUNK_EXT;
			} else if (c == '\n') {
				if (p->line_len == 0) {
					return -EBADMSG; /* no size at all */
				}
				p->state = (p->remaining == 0) ? S_TRAILER
							       : S_CHUNK_DATA;
				p->line_len = 0;
			} else if (c != '\r') {
				return -EBADMSG;
			}
			break;
		}

		case S_CHUNK_EXT:
			i++;
			if (c == '\n') {
				if (p->line_len == 0) {
					return -EBADMSG;
				}
				p->state = (p->remaining == 0) ? S_TRAILER
							       : S_CHUNK_DATA;
				p->line_len = 0;
			}
			break;

		case S_CHUNK_DATA: {
			size_t run = (size_t)TEDGE_MIN((uint64_t)(len - i),
						       p->remaining);
			int rc = body(p, data + i, run);

			if (rc != 0) {
				return rc;
			}
			i += run;
			p->remaining -= run;
			if (p->remaining == 0) {
				p->state = S_CHUNK_END;
			}
			break;
		}

		case S_CHUNK_END:
			i++;
			if (c == '\n') {
				p->state = S_CHUNK_SIZE;
				p->remaining = 0;
				p->line_len = 0;
			} else if (c != '\r') {
				return -EBADMSG;
			}
			break;

		case S_TRAILER:
			/* Trailer fields, ignored, then a blank line. */
			i++;
			if (c == '\n') {
				if (p->line_len == 0) {
					p->state = S_DONE;
				}
				p->line_len = 0;
			} else if (c != '\r') {
				p->line_len++;
			}
			break;
		}
	}
	p->done = (p->state == S_DONE);
	p->used = i;
	return 0;
}

int tedge_http_parse_eof(struct tedge_http_parser *p)
{
	if (p->state == S_BODY_TO_CLOSE) {
		p->state = S_DONE;
		p->done = true;
	}
	return p->done ? 0 : -ECONNRESET;
}

/* ------------------------------------------------------------------------ */
/* The exchange                                                              */
/* ------------------------------------------------------------------------ */

int tedge_http_send_all(int conn, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len > 0) {
		int n = tedge_port_send(conn, p, len);

		if (n < 0) {
			return n;
		}
		if (n == 0) {
			return -ECONNRESET;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* The request head goes out in a few writes of this size, not one per
 * piece: on TLS each write is a record of its own. */
struct out {
	int conn;
	int rc;
	size_t n;
	char buf[256];
};

static void flush(struct out *o)
{
	if (o->rc == 0 && o->n > 0) {
		o->rc = tedge_http_send_all(o->conn, o->buf, o->n);
	}
	o->n = 0;
}

static void put(struct out *o, const char *s)
{
	size_t len = strlen(s);

	while (len > 0 && o->rc == 0) {
		size_t n = TEDGE_MIN(len, sizeof(o->buf) - o->n);

		memcpy(o->buf + o->n, s, n);
		o->n += n;
		s += n;
		len -= n;
		if (o->n == sizeof(o->buf)) {
			flush(o);
		}
	}
}

static int send_request(int conn, const struct tedge_http_request *req)
{
	struct out o = { .conn = conn };
	char length[24];

	put(&o, req->method);
	put(&o, " ");
	put(&o, req->path);
	put(&o, " HTTP/1.1\r\nHost: ");
	put(&o, req->host);
	put(&o, "\r\n");
	for (const char *const *h = req->headers; h != NULL && *h != NULL; h++) {
		put(&o, *h);
	}
	if (req->payload != NULL || req->send != NULL) {
		if (req->content_type != NULL) {
			put(&o, "Content-Type: ");
			put(&o, req->content_type);
			put(&o, "\r\n");
		}
		snprintf(length, sizeof(length), "Content-Length: %zu\r\n",
			 req->body_len);
		put(&o, length);
	}
	put(&o, "Connection: close\r\n\r\n");
	flush(&o);
	if (o.rc != 0) {
		return o.rc;
	}
	if (req->payload != NULL) {
		return tedge_http_send_all(conn, req->payload, req->body_len);
	}
	if (req->send != NULL) {
		return req->send(conn, req->ctx);
	}
	return 0;
}

int tedge_http_exchange(int conn, const struct tedge_http_request *req,
			int *status)
{
	struct tedge_http_parser p;
	int64_t deadline = tedge_port_uptime_ms() + req->timeout_ms;
	int rc;

	rc = send_request(conn, req);
	if (rc != 0) {
		return rc;
	}
	tedge_http_parser_init(&p, req->on_header, req->on_body, req->ctx);
	while (!p.done) {
		int64_t left = deadline - tedge_port_uptime_ms();
		int n;

		if (left <= 0) {
			return -ETIMEDOUT;
		}
		n = tedge_port_recv(conn, req->buf, req->buf_len, (uint32_t)left);
		if (n == -EAGAIN) {
			return -ETIMEDOUT;
		}
		if (n < 0) {
			return n;
		}
		if (n == 0) {
			rc = tedge_http_parse_eof(&p);
			if (rc != 0) {
				return rc;
			}
			break;
		}
		rc = tedge_http_parse(&p, req->buf, (size_t)n);
		if (rc != 0) {
			return rc;
		}
	}
	*status = p.status;
	return 0;
}

int tedge_http_connect(const char *host, uint16_t port, bool tls,
		       uint32_t timeout_ms)
{
	uint8_t addr[4];
	int rc = tedge_port_resolve(host, addr,
				    TEDGE_MIN(timeout_ms, RESOLVE_TIMEOUT_MS));

	if (rc != 0) {
		return (rc == -ENOENT) ? -EHOSTUNREACH : rc;
	}
	return tedge_port_connect(addr, port,
				  tls ? TEDGE_PORT_TLS : TEDGE_PORT_TCP, host,
				  timeout_ms);
}
