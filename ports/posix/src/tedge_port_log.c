/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX side of <tedge/port/log.h>: each line on standard error,
 *
 *   2026-11-10T09:15:02.481Z <inf> tedge: connected to example.c8y.io
 *
 * and, with log upload, the same line in the client's log ring, so the
 * cloud gets what the console showed.
 */

#include <tedge/port/log.h>

#include "tedge_internal.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int level = TEDGE_LOG_LEVEL;
/* One line at a time, so lines from different threads do not interleave. */
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;

int tedge_port_log_set_level(enum tedge_log_level new_level)
{
	__atomic_store_n(&level, (int)new_level, __ATOMIC_RELAXED);
	return 0;
}

void tedge_port_log_flush(void)
{
	(void)fflush(stderr);
}

void tedge_posix_log(int line_level, const char *fmt, ...)
{
	static const char *const names[] = { "", "err", "wrn", "inf", "dbg" };
	char line[512];
	struct timespec ts;
	struct tm tm;
	va_list ap;
	int n, m;

	if (line_level > __atomic_load_n(&level, __ATOMIC_RELAXED) ||
	    line_level < 1 || line_level > 4) {
		return;
	}
	(void)clock_gettime(CLOCK_REALTIME, &ts);
	(void)gmtime_r(&ts.tv_sec, &tm);
	n = snprintf(line, sizeof(line),
		     "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ <%s> tedge: ",
		     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		     tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
		     names[line_level]);
	va_start(ap, fmt);
	m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (m < 0) {
		m = 0;
	}
	n += m;
	if ((size_t)n > sizeof(line) - 2) {
		n = (int)sizeof(line) - 2; /* cut, keeping room for the newline */
	}
	line[n++] = '\n';
	line[n] = '\0';

	(void)pthread_mutex_lock(&out_lock);
	(void)fputs(line, stderr);
	(void)pthread_mutex_unlock(&out_lock);
#if defined(TEDGE_LOG_UPLOAD)
	tedge_log_ring_write((const uint8_t *)line, (size_t)n);
#endif
}
