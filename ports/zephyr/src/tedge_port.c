/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr side of the port interface's small services: kernel
 * (tedge/port/os.h), logging (tedge/port/log.h) and the device
 * (tedge/port/device.h). Each is a thin call into the Zephyr API.
 */

#include <tedge/port/device.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include <errno.h>

void tedge_port_mutex_lock(tedge_port_mutex_t *mutex)
{
	(void)k_mutex_lock(mutex, K_FOREVER);
}

void tedge_port_mutex_unlock(tedge_port_mutex_t *mutex)
{
	(void)k_mutex_unlock(mutex);
}

int64_t tedge_port_uptime_ms(void)
{
	return k_uptime_get();
}

uint32_t tedge_port_random32(void)
{
	return sys_rand32_get();
}

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
