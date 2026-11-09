/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief A small HTTP/1.1 client, over a connection from the port.
 *
 * One request per connection ("Connection: close"), which is all the
 * client needs: a download, an upload, an enrollment. The response is
 * parsed as it arrives, so neither its headers nor its body have to fit in
 * memory: a header's value and the body are handed over in pieces.
 */

#ifndef TEDGE_HTTP_H_
#define TEDGE_HTTP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * A piece of a response header's value. @p name is the header's name in
 * lower case (cut at 31 characters). A long value arrives in several
 * pieces, at @p offset into the value; @p last marks its end (and may come
 * with no bytes).
 */
typedef void (*tedge_http_header_fn)(const char *name, const char *piece,
				     size_t len, size_t offset, bool last,
				     void *ctx);

/** A piece of the body of a response with @p status, decoded (chunked
 *  transfer is undone). @return 0 to go on, or a negative errno to stop
 *  the transfer. */
typedef int (*tedge_http_body_fn)(int status, const void *data, size_t len,
				  void *ctx);

/* --- The response parser ------------------------------------------------- */

struct tedge_http_parser {
	tedge_http_header_fn on_header;
	tedge_http_body_fn on_body;
	void *ctx;

	/* out */
	int status;
	/** Content-Length, or -1 when the response has none. */
	int64_t content_length;
	bool done;

	/* internal */
	uint8_t state;
	bool chunked;
	char line[64];
	size_t line_len;
	char name[32];
	size_t name_len;
	size_t value_len;
	uint64_t remaining;
	int rc;
};

/** Start parsing a response. Either callback may be NULL. */
void tedge_http_parser_init(struct tedge_http_parser *p,
			    tedge_http_header_fn on_header,
			    tedge_http_body_fn on_body, void *ctx);

/**
 * Feed @p len bytes of the response.
 *
 * @return 0 while more is expected or once the response is complete
 *         (p->done); -EBADMSG for a malformed response; or the body
 *         callback's error.
 */
int tedge_http_parse(struct tedge_http_parser *p, const uint8_t *data,
		     size_t len);

/** The connection closed: complete a response whose body runs to the
 *  close. @return 0, or -ECONNRESET when the response was cut short. */
int tedge_http_parse_eof(struct tedge_http_parser *p);

/* --- One exchange -------------------------------------------------------- */

/** Writes the request body on @p conn: exactly body_len bytes. */
typedef int (*tedge_http_send_fn)(int conn, void *ctx);

struct tedge_http_request {
	const char *method;          /**< "GET", "POST" */
	const char *host;            /**< for the Host header */
	const char *path;
	/** Extra header lines, each "Name: value\r\n", NULL-terminated. */
	const char *const *headers;
	const char *content_type;    /**< with a body */
	/** The body: @p body_len bytes of @p payload, or written by @p send. */
	const void *payload;
	tedge_http_send_fn send;
	size_t body_len;

	tedge_http_header_fn on_header;
	tedge_http_body_fn on_body;
	void *ctx;

	/** Receive buffer; 512 bytes is plenty. */
	uint8_t *buf;
	size_t buf_len;
	/** For the whole exchange, from the request's first byte. */
	uint32_t timeout_ms;
};

/**
 * Send @p req on @p conn and read the response to its end.
 *
 * @return 0 with @p status set (any status: the caller judges it), or a
 *         negative errno: the connection failed, the response was
 *         malformed, the time ran out, or the body callback refused.
 */
int tedge_http_exchange(int conn, const struct tedge_http_request *req,
			int *status);

/**
 * Resolve @p host and open a connection to it, TLS when @p tls (verifying
 * the server, no client certificate).
 *
 * @return A connection, or a negative errno.
 */
int tedge_http_connect(const char *host, uint16_t port, bool tls,
		       uint32_t timeout_ms);

/** Send all of @p len bytes. @return 0, or a negative errno. */
int tedge_http_send_all(int conn, const void *data, size_t len);

#endif /* TEDGE_HTTP_H_ */
