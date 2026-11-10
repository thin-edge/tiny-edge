/* SPDX-License-Identifier: Apache-2.0
 *
 * Improv Serial framing: each message on the console is
 *
 *   "IMPROV" | version (1) | type | length | data | checksum
 *
 * the checksum being the low byte of the sum of every byte before it. The
 * device ends its frames with a newline, so they stand on lines of their
 * own between log lines. Received bytes outside a valid frame are skipped,
 * so a console can carry Improv, typed text and logs at once.
 *
 * Portable: the port feeds bytes and writes frames (tedge/provisioning.h).
 */

#include "tedge_provisioning_internal.h"

#include <tedge/port/os.h>

#include <string.h>

#define VERSION 1

#define TYPE_STATE  0x01
#define TYPE_ERROR  0x02
#define TYPE_RPC    0x03
#define TYPE_RESULT 0x04

static const uint8_t header[6] = { 'I', 'M', 'P', 'R', 'O', 'V' };

/* --- Writing ------------------------------------------------------------- */

static tedge_provisioning_write_t writer;
static void *writer_ctx;

void tedge_provisioning_serial_attach(tedge_provisioning_write_t write,
				      void *ctx)
{
	writer = write;
	writer_ctx = ctx;
}

/* Only the worker thread writes, so one buffer serves every frame. */
static void send_frame(uint8_t type, const uint8_t *data, size_t len)
{
	static uint8_t frame[sizeof(header) + 3 + 255 + 2];
	uint8_t sum = 0;
	size_t n = 0;

	if (writer == NULL || len > 255) {
		return;
	}
	memcpy(frame, header, sizeof(header));
	n = sizeof(header);
	frame[n++] = VERSION;
	frame[n++] = type;
	frame[n++] = (uint8_t)len;
	memcpy(&frame[n], data, len);
	n += len;
	for (size_t i = 0; i < n; i++) {
		sum += frame[i];
	}
	frame[n++] = sum;
	frame[n++] = '\n';
	writer(frame, n, writer_ctx);
}

static void out_state(uint8_t state)
{
	send_frame(TYPE_STATE, &state, 1);
}

static void out_error(uint8_t error)
{
	send_frame(TYPE_ERROR, &error, 1);
}

static void out_result(const uint8_t *data, size_t len)
{
	send_frame(TYPE_RESULT, data, len);
}

const struct rpc_out tedge_prov_serial_out = {
	.state = out_state,
	.error = out_error,
	.result = out_result,
};

/* --- Reading ------------------------------------------------------------- */

static struct {
	size_t pos;  /* bytes of the current frame so far */
	uint8_t type;
	uint8_t len;
	uint8_t sum;
	uint8_t data[255];
} rx;

TEDGE_PORT_MUTEX_DEFINE(rx_lock);

/* One byte. @return true when it completed a valid frame. */
static bool rx_byte(uint8_t b)
{
	size_t at = rx.pos;

	if (at < sizeof(header)) {
		if (b == header[at]) {
			rx.pos++;
			rx.sum = (uint8_t)(at == 0 ? b : rx.sum + b);
		} else {
			/* A new frame may start right here. */
			rx.pos = (b == header[0]) ? 1 : 0;
			rx.sum = b;
		}
		return false;
	}
	if (at == sizeof(header)) { /* version */
		if (b != VERSION) {
			rx.pos = (b == header[0]) ? 1 : 0;
			rx.sum = b;
			return false;
		}
	} else if (at == sizeof(header) + 1) {
		rx.type = b;
	} else if (at == sizeof(header) + 2) {
		rx.len = b;
	} else if (at < sizeof(header) + 3 + rx.len) {
		rx.data[at - sizeof(header) - 3] = b;
	} else { /* the checksum */
		bool valid = (b == rx.sum);

		rx.pos = 0;
		if (!valid) {
			/* A whole frame arrived damaged: say so (an empty packet
			 * is answered "invalid RPC"), from the worker, which
			 * writes every answer. */
			(void)tedge_prov_rpc_submit(NULL, 0, &tedge_prov_serial_out);
			return false;
		}
		if (rx.type == TYPE_RPC) {
			(void)tedge_prov_rpc_submit(rx.data, rx.len,
						    &tedge_prov_serial_out);
		}
		return true;
	}
	rx.sum += b;
	rx.pos++;
	return false;
}

size_t tedge_provisioning_serial_feed(const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t frames = 0;

	tedge_port_mutex_lock(&rx_lock);
	for (size_t i = 0; i < len; i++) {
		if (rx_byte(p[i])) {
			frames++;
		}
	}
	tedge_port_mutex_unlock(&rx_lock);
	return frames;
}
