/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Ready-made provisioning hooks for Zephyr applications.
 *
 * Most of what the provisioning library asks of an application is the same
 * in every Zephyr application, so the port provides it:
 *
 * - Wi-Fi: join the network, keep it in Zephyr's Wi-Fi credentials store
 *   (CONFIG_WIFI_CREDENTIALS) only once the device has an address, and go
 *   back to the previous network otherwise;
 * - the scan, on the first Wi-Fi interface;
 * - identify: blink the devicetree's led0, or the first pixel of its
 *   led-strip;
 * - the chip: CONFIG_SOC.
 *
 * One call starts the library with them:
 *
 * @code
 * tedge_init(NULL, &hooks);
 * tedge_provisioning_zephyr_init(NULL);
 * tedge_start();
 * @endcode
 *
 * An application that manages the network itself, or has a better light,
 * passes its own hooks; each one it sets replaces the port's:
 *
 * @code
 * static const struct tedge_provisioning_hooks mine = {
 *	.wifi_connect = my_network_switch,
 *	.identify = my_status_led_blink,
 * };
 * tedge_provisioning_zephyr_init(&mine);
 * @endcode
 */

#ifndef TEDGE_PROVISIONING_ZEPHYR_H_
#define TEDGE_PROVISIONING_ZEPHYR_H_

#include <tedge/provisioning.h>

/**
 * Start the provisioning library with the port's hooks, replaced by any
 * member of @p overrides that is set (NULL keeps them all). Call after
 * tedge_init().
 *
 * @return 0, or a negative errno.
 */
int tedge_provisioning_zephyr_init(const struct tedge_provisioning_hooks *overrides);

/** The port's scan hook, on the first Wi-Fi interface (needs CONFIG_WIFI). */
int tedge_provisioning_wifi_scan(struct tedge_provisioning_network *out,
				 size_t max, void *ctx);

#endif /* TEDGE_PROVISIONING_ZEPHYR_H_ */
