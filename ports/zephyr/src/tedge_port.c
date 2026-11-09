/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr side of the port interface's logging (tedge/port/log.h) and
 * device (tedge/port/device.h) services. The kernel's are in
 * tedge_port_os.c.
 */

#include <tedge/port/device.h>
#include <tedge/port/log.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include <errno.h>

/* The tedge_log_level values are Zephyr's LOG_LEVEL_* values, so a level
 * passes through as it is. */
BUILD_ASSERT(TEDGE_LOG_LEVEL_ERR == LOG_LEVEL_ERR &&
	     TEDGE_LOG_LEVEL_DBG == LOG_LEVEL_DBG);

int tedge_port_log_set_level(enum tedge_log_level level)
{
#if defined(CONFIG_LOG_RUNTIME_FILTERING)
	int source = log_source_id_get("tedge");

	if (source < 0) {
		return -ENOENT;
	}
	(void)log_filter_set(NULL, 0, (uint32_t)source, (uint32_t)level);
	return 0;
#else
	ARG_UNUSED(level);
	return -ENOTSUP;
#endif
}

void tedge_port_log_flush(void)
{
	log_flush();
}

int tedge_port_reset_cause(uint32_t *cause)
{
#if defined(CONFIG_HWINFO)
	int rc = hwinfo_get_reset_cause(cause);

	(void)hwinfo_clear_reset_cause();
	return rc;
#else
	*cause = 0;
	return -ENOTSUP;
#endif
}
