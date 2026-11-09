/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Wall-clock time: the port interface.
 *
 * TLS needs the date to judge a certificate, and telemetry carries it. The
 * application may already keep the clock (GNSS, a gateway, its own SNTP),
 * so the port only sets it when nobody has.
 */

#ifndef TEDGE_PORT_TIME_H_
#define TEDGE_PORT_TIME_H_

#include <stdbool.h>
#include <time.h>

/** True once the wall clock is plausible: set, by anyone, to a date after
 *  the client was written. */
bool tedge_port_time_is_valid(void);

/**
 * Set the wall clock from the network (SNTP, as a rule) unless it is
 * already valid. Called from the client thread, with the network up.
 *
 * @return 0 when the clock is valid afterwards, else a negative errno.
 */
int tedge_port_time_sync(void);

/**
 * The wall-clock time now, in seconds since the Unix epoch, UTC.
 *
 * @return 0, or -EAGAIN while the clock is not valid yet.
 */
int tedge_port_time_now(time_t *secs);

#endif /* TEDGE_PORT_TIME_H_ */
