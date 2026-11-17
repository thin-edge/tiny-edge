/* SPDX-License-Identifier: Apache-2.0
 *
 * The NuttX side of <tedge/port/firmware.h>: MCUboot, through nuttx-apps'
 * MCUboot library (boot/mcuboot). The new image is written to the
 * secondary slot (CONFIG_MCUBOOT_SECONDARY_SLOT_PATH), marked for one
 * trial boot, and confirmed once the core says it has proven itself.
 * MCUboot swaps the slots at the next reset, and swaps them back if the new
 * image resets before it is confirmed.
 *
 * The image is collected into whole 4 KB sectors, and each sector is
 * erased and written in one go as it fills: the download starts at once
 * rather than after the whole slot is erased, and each flash operation,
 * which stalls the CPU while the flash cache is off, covers a sector rather
 * than each piece of the download. What follows the image is erased at the
 * end, so no trailer from an earlier image is left for MCUboot to read.
 */

#include <nuttx/config.h>

#include <tedge/port/config.h>
#include <tedge/port/firmware.h>
#include <tedge/port/log.h>

#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <bootutil/bootutil_public.h>
#include <bootutil/image.h>

#include "flash_map_backend/flash_map_backend.h"
#include "sysflash/sysflash.h"

#if defined(TEDGE_FIRMWARE_UPDATE)

TEDGE_LOG_DECLARE();

/* The ESP chips' flash erases 4 KB sectors and writes 4-byte words. */
#define SECTOR 4096u
#define ALIGN 4u

static const struct flash_area *slot;
static uint32_t written;          /* bytes written to the slot */
static uint32_t erased;           /* the slot is erased up to here */
static uint8_t sector[SECTOR];    /* the sector being collected */
static size_t nsector;

static int erase_to(uint32_t end)
{
	end = (end + SECTOR - 1) / SECTOR * SECTOR;
	if (end > slot->fa_size) {
		end = slot->fa_size;
	}
	if (end <= erased) {
		return 0;
	}
	if (flash_area_erase(slot, erased, end - erased) != 0) {
		TEDGE_LOG_ERR("firmware: erasing the slot at %lu failed",
			      (unsigned long)erased);
		return -EIO;
	}
	erased = end;
	return 0;
}

static int put(const uint8_t *data, size_t len)
{
	int ret;

	if (written + len > slot->fa_size) {
		TEDGE_LOG_ERR("firmware: the image is larger than the slot "
			      "(%lu bytes)", (unsigned long)slot->fa_size);
		return -EFBIG;
	}
	ret = erase_to(written + len);
	if (ret != 0) {
		return ret;
	}
	if (flash_area_write(slot, written, data, len) != 0) {
		TEDGE_LOG_ERR("firmware: writing the slot at %lu failed",
			      (unsigned long)written);
		return -EIO;
	}
	written += len;
	return 0;
}

int tedge_port_fw_begin(void)
{
	if (slot != NULL) {
		flash_area_close(slot);
		slot = NULL;
	}
	if (flash_area_open(FLASH_AREA_IMAGE_SECONDARY(0), &slot) != 0) {
		TEDGE_LOG_ERR("firmware: cannot open the secondary slot (%s)",
			      CONFIG_MCUBOOT_SECONDARY_SLOT_PATH);
		slot = NULL;
		return -ENODEV;
	}
	written = 0;
	erased = 0;
	nsector = 0;
	return 0;
}

int tedge_port_fw_write(const void *data, size_t len)
{
	const uint8_t *p = data;
	int ret;

	if (slot == NULL) {
		return -EINVAL;
	}
	while (len > 0) {
		size_t n = SECTOR - nsector;

		if (n > len) {
			n = len;
		}
		memcpy(sector + nsector, p, n);
		nsector += n;
		p += n;
		len -= n;
		if (nsector == SECTOR) {
			ret = put(sector, SECTOR);
			nsector = 0;
			if (ret != 0) {
				return ret;
			}
		}
	}
	return 0;
}

int tedge_port_fw_finish(void)
{
	int ret = 0;

	if (slot == NULL) {
		return -EINVAL;
	}
	if (nsector > 0) {
		/* The last sector, to a whole word, padded with the erased
		 * value, as the slot is. */
		size_t n = (nsector + ALIGN - 1) / ALIGN * ALIGN;

		memset(sector + nsector, 0xff, n - nsector);
		ret = put(sector, n);
		nsector = 0;
	}
	if (ret == 0) {
		ret = erase_to(slot->fa_size);
	}
	if (ret == 0) {
		TEDGE_LOG_INF("firmware: %lu bytes written to the secondary slot",
			      (unsigned long)written);
	}
	return ret;
}

int tedge_port_fw_request_trial(void)
{
	int ret = boot_set_pending_multi(0, 0);

	if (ret != 0) {
		TEDGE_LOG_ERR("firmware: MCUboot did not accept the image (%d)",
			      ret);
		return -EINVAL;
	}
	return 0;
}

bool tedge_port_fw_is_confirmed(void)
{
	struct boot_swap_state state;

	if (boot_read_swap_state_by_id(FLASH_AREA_IMAGE_PRIMARY(0), &state) !=
	    0) {
		return false;
	}
	return state.image_ok == BOOT_FLAG_SET;
}

int tedge_port_fw_confirm(void)
{
	return boot_set_confirmed_multi(0) == 0 ? 0 : -EIO;
}

int tedge_port_fw_running_version(char *buf, size_t len)
{
	const struct flash_area *primary;
	struct image_header hdr;
	int rc;

	if (flash_area_open(FLASH_AREA_IMAGE_PRIMARY(0), &primary) != 0) {
		return -ENOENT;
	}
	rc = flash_area_read(primary, 0, &hdr, sizeof(hdr));
	flash_area_close(primary);
	if (rc != 0 || le32toh(hdr.ih_magic) != IMAGE_MAGIC) {
		return -ENOENT;
	}
	snprintf(buf, len, "%u.%u.%u", hdr.ih_ver.iv_major, hdr.ih_ver.iv_minor,
		 le16toh(hdr.ih_ver.iv_revision));
	return 0;
}

#endif /* TEDGE_FIRMWARE_UPDATE */
