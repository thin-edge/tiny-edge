/* SPDX-License-Identifier: Apache-2.0
 *
 * The device as the client sees it: its default id, and how it is reset.
 */

#include "tedge_internal.h"

#include <tedge/port/device.h>
#include <tedge/port/log.h>

#include <errno.h>
#include <stdio.h>

TEDGE_LOG_DECLARE();

int tedge_default_device_id(char *buf, size_t len)
{
	uint8_t mac[6];
	int rc = tedge_port_mac_address(mac);

	if (rc != 0) {
		return rc;
	}
	return snprintf(buf, len, "%s-%02x%02x%02x%02x%02x%02x",
			TEDGE_DEVICE_ID_PREFIX, mac[0], mac[1], mac[2], mac[3],
			mac[4], mac[5]) < (int)len
		       ? 0
		       : -ENOSPC;
}

_Noreturn void tedge_device_reset(void)
{
	const struct tedge_hooks *hooks = tedge_hook_table();

	TEDGE_LOG_INF("resetting the device");
	tedge_port_log_flush(); /* get the line out first */
	if (hooks != NULL && hooks->reset != NULL) {
		hooks->reset(hooks->user_data);
		/* A hook that returns is a bug; fall through to the default. */
		TEDGE_LOG_ERR("the application's reset hook returned");
	}
	tedge_port_reset();
}
