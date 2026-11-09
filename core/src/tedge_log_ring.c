/* SPDX-License-Identifier: Apache-2.0
 *
 * The client's own log, kept in a ring of RAM so that the last minutes
 * before a problem can be fetched from the cloud afterwards.
 *
 * The port feeds it from the RTOS's logging (on Zephyr, a log backend), so
 * it sees every message the image logs, not only the client's, which is
 * what makes it useful on a device whose application never built a log of
 * its own. It is deliberately small and
 * deliberately volatile — a reboot loses it, which is what the crash dump
 * exists for.
 *
 * When the ring is full the oldest whole line is dropped, so what remains is
 * always readable from its first character: half a line at the top of a log
 * file wastes the reader's time.
 */

#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/os.h>

#include <string.h>

#define RING_BYTES TEDGE_LOG_RING_BYTES

static uint8_t ring[RING_BYTES];
static size_t used;
static uint32_t dropped_lines;
static tedge_port_spinlock_t lock;

/* ------------------------------------------------------------------------ */
/* The ring                                                                  */
/* ------------------------------------------------------------------------ */

/* Drop whole lines until @p need bytes are free. */
static void make_room(size_t need)
{
	while (used + need > RING_BYTES && used > 0) {
		uint8_t *nl = memchr(ring, '\n', used);
		size_t line = (nl != NULL) ? (size_t)(nl - ring) + 1 : used;

		memmove(ring, ring + line, used - line);
		used -= line;
		dropped_lines++;
	}
}

/* Called by the port with each piece of formatted log output, and by the
 * unit test directly. */
void tedge_log_ring_write(const uint8_t *data, size_t len)
{
	tedge_port_spinlock_key_t key = tedge_port_spin_lock(&lock);

	if (len > RING_BYTES) {
		/* One enormous line: keep its tail, which is where the
		 * interesting part of a message usually is. */
		data += len - RING_BYTES;
		len = RING_BYTES;
	}
	make_room(len);
	memcpy(ring + used, data, len);
	used += len;
	tedge_port_spin_unlock(&lock, key);
}

size_t tedge_log_ring_read(size_t offset, uint8_t *out, size_t len)
{
	tedge_port_spinlock_key_t key = tedge_port_spin_lock(&lock);
	size_t n = 0;

	if (offset < used) {
		n = TEDGE_MIN(len, used - offset);
		memcpy(out, ring + offset, n);
	}
	tedge_port_spin_unlock(&lock, key);
	return n;
}

size_t tedge_log_ring_size(void)
{
	tedge_port_spinlock_key_t key = tedge_port_spin_lock(&lock);
	size_t n = used;

	tedge_port_spin_unlock(&lock, key);
	return n;
}

uint32_t tedge_log_ring_dropped(void)
{
	return dropped_lines;
}
