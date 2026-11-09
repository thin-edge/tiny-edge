/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr side of <tedge/port/firmware.h>: MCUboot, with the new image
 * written to slot 1 through flash_img.
 */

#include <tedge/port/firmware.h>

#include <zephyr/kernel.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <stdio.h>

static struct flash_img_context flash_ctx;

int tedge_port_fw_begin(void)
{
	return flash_img_init(&flash_ctx);
}

int tedge_port_fw_write(const void *data, size_t len)
{
	return flash_img_buffered_write(&flash_ctx, data, len, false);
}

int tedge_port_fw_finish(void)
{
	return flash_img_buffered_write(&flash_ctx, NULL, 0, true);
}

int tedge_port_fw_request_trial(void)
{
	return boot_request_upgrade(BOOT_UPGRADE_TEST);
}

bool tedge_port_fw_is_confirmed(void)
{
	return boot_is_img_confirmed();
}

int tedge_port_fw_confirm(void)
{
	return boot_write_img_confirmed();
}

int tedge_port_fw_running_version(char *buf, size_t len)
{
	struct mcuboot_img_header hdr;

	if (boot_read_bank_header(PARTITION_ID(slot0_partition), &hdr,
				  sizeof(hdr)) != 0) {
		return -ENOENT;
	}
	snprintf(buf, len, "%u.%u.%u", hdr.h.v1.sem_ver.major,
		 hdr.h.v1.sem_ver.minor, hdr.h.v1.sem_ver.revision);
	return 0;
}
