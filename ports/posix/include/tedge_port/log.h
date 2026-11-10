/* SPDX-License-Identifier: Apache-2.0 */

/* POSIX implementation of the logging macros in <tedge/port/log.h>: one
 * line per call on standard error, as a daemon under systemd or launchd
 * logs, and (with log upload) into the client's log ring. A line below
 * TEDGE_LOG_LEVEL is compiled out; the level can then be lowered further,
 * or raised back up to it, while the client runs. */

#ifndef TEDGE_PORT_POSIX_LOG_H_
#define TEDGE_PORT_POSIX_LOG_H_

#include <tedge_config.h>

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void tedge_posix_log(int level, const char *fmt, ...);

#define TEDGE_LOG_REGISTER()
#define TEDGE_LOG_DECLARE()

/* The level test is a constant, so a line above TEDGE_LOG_LEVEL is
 * compiled out while its arguments are still type-checked. */
#define TEDGE_POSIX_LOG_(level, ...)                                           \
	do {                                                                   \
		if ((level) <= TEDGE_LOG_LEVEL) {                              \
			tedge_posix_log((level), __VA_ARGS__);                 \
		}                                                              \
	} while (0)

#define TEDGE_LOG_ERR(...) TEDGE_POSIX_LOG_(1, __VA_ARGS__)
#define TEDGE_LOG_WRN(...) TEDGE_POSIX_LOG_(2, __VA_ARGS__)
#define TEDGE_LOG_INF(...) TEDGE_POSIX_LOG_(3, __VA_ARGS__)
#define TEDGE_LOG_DBG(...) TEDGE_POSIX_LOG_(4, __VA_ARGS__)

#define TEDGE_PORT_LOG_RUNTIME_LEVEL 1

#endif /* TEDGE_PORT_POSIX_LOG_H_ */
