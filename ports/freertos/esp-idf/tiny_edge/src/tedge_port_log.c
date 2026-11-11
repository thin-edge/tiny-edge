/* SPDX-License-Identifier: Apache-2.0
 *
 * The ESP-IDF side of <tedge/port/log.h>: the client's level is its tag's
 * level in ESP_LOG. With log upload, every line the image logs is also
 * copied into the client's log ring (core/src/tedge_log_ring.c), as the
 * console shows it, through ESP_LOG's output function.
 */

#include <tedge/port/config.h>
#include <tedge/port/log.h>

#include "tedge_internal.h"

#include <esp_log.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

TEDGE_LOG_DECLARE();

int tedge_port_log_set_level(enum tedge_log_level level)
{
	static const esp_log_level_t map[] = {
		[TEDGE_LOG_LEVEL_NONE] = ESP_LOG_NONE,
		[TEDGE_LOG_LEVEL_ERR] = ESP_LOG_ERROR,
		[TEDGE_LOG_LEVEL_WRN] = ESP_LOG_WARN,
		[TEDGE_LOG_LEVEL_INF] = ESP_LOG_INFO,
		[TEDGE_LOG_LEVEL_DBG] = ESP_LOG_DEBUG,
	};

	if ((unsigned int)level >= sizeof(map) / sizeof(map[0])) {
		return -EINVAL;
	}
	/* Lines below TEDGE_LOG_LEVEL are not in the image to be shown. */
	esp_log_level_set(tedge_log_tag, map[level]);
	return 0;
}

void tedge_port_log_flush(void)
{
	/* ESP_LOG writes each line as it is logged. */
}

#if defined(TEDGE_LOG_UPLOAD)

/* The output function ESP_LOG had before: the console, as a rule. */
static vprintf_like_t console;

/* Called for each line, from whichever task logs (ESP_LOG hands over a
 * whole line, its newline included). The buffer is on that task's stack,
 * so it is kept small: a longer line is cut in the ring, never on the
 * console, and the cut is marked and keeps the line's end, so the next
 * line still starts on its own. */
static int capture(const char *fmt, va_list args)
{
	static const char cut[] = "...\n";
	char buf[128];
	va_list copy;
	size_t len;
	int n;

	va_copy(copy, args);
	n = vsnprintf(buf, sizeof(buf), fmt, copy);
	va_end(copy);
	if (n > 0) {
		len = (size_t)n;
		if (len >= sizeof(buf)) {
			size_t f = strlen(fmt);

			len = sizeof(buf) - 1;
			if (f > 0 && fmt[f - 1] == '\n') {
				memcpy(buf + len - (sizeof(cut) - 1), cut,
				       sizeof(cut) - 1);
			}
		}
		tedge_log_ring_write((const uint8_t *)buf, len);
	}
	return (console != NULL) ? console(fmt, args) : n;
}

/* Before app_main(), so the ring holds the boot's lines too. */
__attribute__((constructor)) static void capture_start(void)
{
	console = esp_log_set_vprintf(capture);
}

#endif /* TEDGE_LOG_UPLOAD */
