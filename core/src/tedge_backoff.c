/* SPDX-License-Identifier: Apache-2.0
 *
 * How long to wait before the next connect attempt: start at a few seconds,
 * double up to the configured maximum, and add jitter so a fleet coming back
 * after an outage does not reconnect in lockstep. The floor exists because a
 * closed TLS connection holds its TCP context for a moment (spikes, 3.5).
 */

#include "tedge_internal.h"
#include "tedge_util.h"

#include <tedge/port/os.h>

#define BACKOFF_MIN_S 3

uint32_t tedge_backoff_next(uint32_t current, uint32_t max)
{
	uint32_t next = (current == 0) ? BACKOFF_MIN_S : current * 2;

	if (next > max) {
		next = max;
	}
	/* ±20% jitter, so a fleet doesn't reconnect in lockstep. */
	int32_t spread = (int32_t)(next / 5);

	if (spread > 0) {
		next = (uint32_t)((int32_t)next - spread +
				  (int32_t)(tedge_port_random32() % (2 * spread + 1)));
	}
	return TEDGE_MAX(next, (uint32_t)BACKOFF_MIN_S);
}
