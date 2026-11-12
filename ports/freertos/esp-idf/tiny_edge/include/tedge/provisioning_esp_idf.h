/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Ready-made provisioning hooks for ESP-IDF applications.
 *
 * What the provisioning library (<tedge/provisioning.h>) asks of an
 * application is mostly the same in every ESP-IDF application, so the port
 * provides it:
 *
 * - Improv Serial on the console: the UART console and the USB Serial/JTAG
 *   console, whichever the image uses, so tedge-onboard finds the board on
 *   either USB port;
 * - Wi-Fi: join the network with esp_wifi and keep it in NVS; when the
 *   device gets no address, go back to the previous network, or clear the
 *   stored one;
 * - the scan, with esp_wifi;
 * - the chip: CONFIG_IDF_TARGET.
 *
 * The application owns Wi-Fi as before (esp_wifi_init(), station mode,
 * esp_wifi_start()). One call starts the library with these hooks:
 *
 * @code
 * tedge_init(NULL, &hooks);
 * tedge_provisioning_esp_idf_init(NULL);
 * tedge_start();
 * @endcode
 *
 * Each hook an application passes replaces the port's, for example its own
 * LED for identify:
 *
 * @code
 * static const struct tedge_provisioning_hooks mine = {
 *	.identify = blink_status_led,
 * };
 * tedge_provisioning_esp_idf_init(&mine);
 * @endcode
 */

#ifndef TEDGE_PROVISIONING_ESP_IDF_H_
#define TEDGE_PROVISIONING_ESP_IDF_H_

#include <tedge/provisioning.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the provisioning library with the port's hooks, and Improv Serial on
 * the console. Call once, after tedge_init() and esp_wifi_start().
 *
 * @param overrides Hooks that replace the port's (each member that is not
 *        NULL), or NULL. Copied; only its ctx must stay valid.
 * @return 0, or a negative errno.
 */
int tedge_provisioning_esp_idf_init(const struct tedge_provisioning_hooks *overrides);

#ifdef __cplusplus
}
#endif

#endif /* TEDGE_PROVISIONING_ESP_IDF_H_ */
