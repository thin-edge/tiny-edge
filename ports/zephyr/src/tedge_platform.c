/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr side of the device in <tedge/port/device.h>: the MAC address
 * the default device id is made from, the firmware's name and version, and
 * the reset.
 */

#include <tedge/port/device.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/reboot.h>

#include <errno.h>
#include <string.h>

#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <esp_rom_sys.h>
#endif

int tedge_port_mac_address(uint8_t mac[6])
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *link;

	if (iface == NULL) {
		return -ENODEV;
	}
	link = net_if_get_link_addr(iface);
	if (link == NULL || link->len < 6) {
		return -ENODEV;
	}
	memcpy(mac, link->addr, 6);
	return 0;
}

/* The image's name and version from the application's VERSION file, when
 * the build has one (APP_VERSION_STRING), with the same headers in scope as
 * the core had when it decided this itself. */
const char *tedge_port_firmware_name(void)
{
#if defined(CONFIG_APP_VERSION_EXTENDED_STRING) || defined(APP_VERSION_STRING)
	return CONFIG_KERNEL_BIN_NAME;
#else
	return "zephyr";
#endif
}

const char *tedge_port_firmware_version(void)
{
#if defined(APP_VERSION_STRING)
	return APP_VERSION_STRING;
#else
	return "0.0.0";
#endif
}

FUNC_NORETURN void tedge_port_reset(void)
{
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
	/* A full-system reset. After sys_reboot()'s CPU-only reset with Wi-Fi
	 * running, an ESP32-C6 hangs in MCUboot until it is power-cycled
	 * (c8y-direct-spikes, P2).
	 */
	esp_rom_software_reset_system();
#else
	sys_reboot(SYS_REBOOT_COLD);
#endif
	CODE_UNREACHABLE;
}
