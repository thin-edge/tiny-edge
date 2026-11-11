/* SPDX-License-Identifier: Apache-2.0
 *
 * The ESP-IDF side of <tedge/port/firmware.h>: OTA app partitions, with the
 * bootloader's rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE), which is
 * the MCUboot model the interface describes. A new image is written to the
 * next OTA partition and made the boot partition; it then boots in the
 * "pending verify" state, and the bootloader returns to the old image if
 * it resets before confirming itself.
 */

#include <tedge/port/firmware.h>
#include <tedge/port/log.h>

#include <esp_app_desc.h>
#include <esp_ota_ops.h>

#include <errno.h>
#include <stdio.h>

#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
#error "TEDGE_FIRMWARE_UPDATE needs CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE"
#endif

TEDGE_LOG_DECLARE();

static esp_ota_handle_t ota;
static const esp_partition_t *target;

static int errno_of(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return 0;
	case ESP_ERR_NO_MEM:
		return -ENOMEM;
	case ESP_ERR_NOT_FOUND:
		return -ENOENT;
	case ESP_ERR_INVALID_SIZE:
	case ESP_ERR_OTA_PARTITION_CONFLICT:
		return -ENOSPC;
	case ESP_ERR_OTA_VALIDATE_FAILED:
	case ESP_ERR_INVALID_ARG:
		return -EINVAL;
	default:
		return -EIO;
	}
}

int tedge_port_fw_begin(void)
{
	esp_err_t e;

	if (ota != 0) {
		(void)esp_ota_abort(ota); /* a download that never finished */
		ota = 0;
	}
	target = esp_ota_get_next_update_partition(NULL);
	if (target == NULL) {
		TEDGE_LOG_ERR("firmware: no OTA partition to write to (the "
			      "partition table needs two OTA app partitions)");
		return -ENOENT;
	}
	/* Erased as it is written, not all at once: a whole partition takes
	 * seconds to erase, and the download would stall meanwhile. */
	e = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
	if (e != ESP_OK) {
		TEDGE_LOG_ERR("firmware: esp_ota_begin(%s): %s", target->label,
			      esp_err_to_name(e));
		ota = 0;
	}
	return errno_of(e);
}

int tedge_port_fw_write(const void *data, size_t len)
{
	if (ota == 0) {
		return -EINVAL;
	}
	return errno_of(esp_ota_write(ota, data, len));
}

int tedge_port_fw_finish(void)
{
	esp_err_t e;

	if (ota == 0) {
		return -EINVAL;
	}
	/* Checks the image (its header, its hash, and its signature when
	 * secure boot is on). */
	e = esp_ota_end(ota);
	ota = 0;
	if (e != ESP_OK) {
		TEDGE_LOG_ERR("firmware: the image is not valid (%s)",
			      esp_err_to_name(e));
	}
	return errno_of(e);
}

int tedge_port_fw_request_trial(void)
{
	esp_err_t e;

	if (target == NULL) {
		return -EINVAL;
	}
	e = esp_ota_set_boot_partition(target);
	if (e != ESP_OK) {
		TEDGE_LOG_ERR("firmware: could not boot %s next (%s)",
			      target->label, esp_err_to_name(e));
	}
	return errno_of(e);
}

bool tedge_port_fw_is_confirmed(void)
{
	esp_ota_img_states_t state;

	if (esp_ota_get_state_partition(esp_ota_get_running_partition(),
					&state) != ESP_OK) {
		/* A factory partition has no state: nothing to confirm. */
		return true;
	}
	return state != ESP_OTA_IMG_PENDING_VERIFY;
}

int tedge_port_fw_confirm(void)
{
	return errno_of(esp_ota_mark_app_valid_cancel_rollback());
}

/* The image's version as the build stated it (PROJECT_VER). */
int tedge_port_fw_running_version(char *buf, size_t len)
{
	const esp_app_desc_t *desc = esp_app_get_description();

	if (desc->version[0] == '\0') {
		return -ENOENT;
	}
	snprintf(buf, len, "%s", desc->version);
	return 0;
}
