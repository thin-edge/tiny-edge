/* SPDX-License-Identifier: Apache-2.0
 *
 * A Zephyr log backend that feeds the client's log ring
 * (core/src/tedge_log_ring.c) with every line the image logs, formatted as
 * the console shows it.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>

#include <errno.h>

static int out_fn(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	tedge_log_ring_write(data, length);
	return (int)length;
}

/* One line at a time; the formatter needs no more than this. */
static uint8_t out_buf[64];
LOG_OUTPUT_DEFINE(log_output_tedge, out_fn, out_buf, sizeof(out_buf));

static void backend_process(const struct log_backend *const backend,
			    union log_msg_generic *msg)
{
	uint32_t flags = LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_TIMESTAMP |
			 LOG_OUTPUT_FLAG_FORMAT_TIMESTAMP;

	ARG_UNUSED(backend);
	log_output_msg_process(&log_output_tedge, &msg->log, flags);
}

static void backend_init(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
}

static int backend_format_set(const struct log_backend *const backend,
			      uint32_t log_type)
{
	ARG_UNUSED(backend);
	ARG_UNUSED(log_type);
	return -ENOTSUP; /* the text format is the only one worth keeping */
}

static void backend_panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	/* Nothing to flush: the ring is RAM, and whatever caused the panic
	 * is the crash dump's job, not this one's. */
}

static const struct log_backend_api backend_api = {
	.process = backend_process,
	.init = backend_init,
	.format_set = backend_format_set,
	.panic = backend_panic,
};

LOG_BACKEND_DEFINE(tedge_log_ring, backend_api, true);
