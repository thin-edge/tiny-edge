/* SPDX-License-Identifier: Apache-2.0
 *
 * The ESP-IDF side of <tedge/port/device.h>, and of the random numbers in
 * <tedge/port/os.h> and <tedge/port/crypto.h>.
 */

#include <tedge/port/crypto.h>
#include <tedge/port/device.h>
#include <tedge/port/os.h>

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_idf_version.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_system.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <errno.h>
#include <stdio.h>

/* --- The device ---------------------------------------------------------- */

/* esp_reset_reason()'s value (ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC,
 * ...), as a number. It is the reason for this boot; ESP-IDF works it out
 * afresh on every boot, so there is nothing to clear. */
int tedge_port_reset_cause(uint32_t *cause)
{
	*cause = (uint32_t)esp_reset_reason();
	return 0;
}

/* The MAC of the default interface (the Wi-Fi station's, as a rule), or
 * the station MAC the chip was given in the factory when no interface
 * exists yet: the same on every boot either way. */
int tedge_port_mac_address(uint8_t mac[6])
{
	esp_netif_t *netif = esp_netif_get_default_netif();

	if (netif != NULL && esp_netif_get_mac(netif, mac) == ESP_OK) {
		return 0;
	}
	return (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) ? 0 : -ENODEV;
}

/* The project's name and version, as the build put them in the image's
 * description (PROJECT_VER, or `git describe`). */
const char *tedge_port_firmware_name(void)
{
	return esp_app_get_description()->project_name;
}

const char *tedge_port_firmware_version(void)
{
	return esp_app_get_description()->version;
}

/* FreeRTOS under ESP-IDF, and the chip: the target's name, and the revision
 * the chip reports (esp_chip_info()'s major * 100 + minor). */
void tedge_port_platform(struct tedge_port_platform *out)
{
	static char sdk[32];
	static char revision[16];
	static const char kernel[] = tskKERNEL_VERSION_NUMBER;
	esp_chip_info_t chip;

	esp_chip_info(&chip);
	snprintf(revision, sizeof(revision), "v%u.%u",
		 (unsigned)(chip.revision / 100), (unsigned)(chip.revision % 100));
	snprintf(sdk, sizeof(sdk), "esp-idf %s", esp_get_idf_version());
	out->os = "freertos";
	/* "V11.1.0" -> "11.1.0", as the other ports state versions. */
	out->os_version = (kernel[0] == 'V') ? kernel + 1 : kernel;
	out->sdk = sdk;
	out->model = CONFIG_IDF_TARGET;
	out->revision = revision;
}

/* esp_restart() stops Wi-Fi and the other radios before a full-system
 * reset, which is what the interface asks for. */
_Noreturn void tedge_port_reset(void)
{
	esp_restart();
}

/* Crash dumps (TEDGE_COREDUMP) are not in this port yet: ESP-IDF's core
 * dump partition would be offered as a log type. */
void tedge_port_coredump_init(void)
{
}

void tedge_port_coredump_taken(void)
{
}

/* --- Random numbers ------------------------------------------------------ */

/* The hardware generator: cryptographically strong while a radio runs,
 * and also before, since the bootloader seeds it from its entropy source. */
uint32_t tedge_port_random32(void)
{
	return esp_random();
}

int tedge_port_random(void *buf, size_t len)
{
	esp_fill_random(buf, len);
	return 0;
}
