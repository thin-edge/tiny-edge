/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Logging: the port interface.
 *
 * The core logs through the RTOS's own logging, so its lines sit beside the
 * application's, filtered and routed the same way. The port maps these
 * macros onto it in <tedge_port/log.h>:
 *
 * - TEDGE_LOG_DECLARE(): once per source file, at file scope, before the
 *   first log line. Zephyr declares the "tedge" log module.
 * - TEDGE_LOG_ERR(fmt, ...), TEDGE_LOG_WRN, TEDGE_LOG_INF, TEDGE_LOG_DBG:
 *   one line each, printf-style, no trailing newline.
 * - TEDGE_PORT_LOG_RUNTIME_LEVEL: defined to 1 when this build can change
 *   the level while it runs (tedge_port_log_set_level()), so the core
 *   leaves the code out of builds that cannot.
 *
 * A line can be compiled out below the configured level (TEDGE_LOG_LEVEL),
 * so its arguments must have no side effects.
 */

#ifndef TEDGE_PORT_LOG_H_
#define TEDGE_PORT_LOG_H_

#include <tedge/port/config.h>
#include <tedge_port/log.h>

/** Log levels, as TEDGE_LOG_LEVEL and tedge_port_log_set_level() use them. */
enum tedge_log_level {
	TEDGE_LOG_LEVEL_NONE = 0,
	TEDGE_LOG_LEVEL_ERR = 1,
	TEDGE_LOG_LEVEL_WRN = 2,
	TEDGE_LOG_LEVEL_INF = 3,
	TEDGE_LOG_LEVEL_DBG = 4,
};

/**
 * Change the client's log level while it runs, for an operator who needs
 * more detail from one device.
 *
 * @return 0, or -ENOTSUP when this build cannot change it at run time. The
 *         core then keeps the value and it takes effect at the next boot.
 */
int tedge_port_log_set_level(enum tedge_log_level level);

#endif /* TEDGE_PORT_LOG_H_ */
