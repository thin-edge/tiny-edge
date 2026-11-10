/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Ready-made provisioning hooks for Zephyr applications.
 *
 * Scanning for Wi-Fi networks is the same in every Zephyr application, so
 * the port provides the hook (tedge_provisioning_hooks::wifi_scan).
 * Joining and storing a network stays the application's, since only it
 * knows how it keeps its credentials:
 *
 * @code
 * static const struct tedge_provisioning_hooks hooks = {
 *	.wifi_connect = my_join_and_store,
 *	.wifi_scan = tedge_provisioning_wifi_scan,
 *	.chip = CONFIG_SOC,
 * };
 * @endcode
 */

#ifndef TEDGE_PROVISIONING_ZEPHYR_H_
#define TEDGE_PROVISIONING_ZEPHYR_H_

#include <tedge/provisioning.h>

/** Scan on the first Wi-Fi interface (needs CONFIG_WIFI). */
int tedge_provisioning_wifi_scan(struct tedge_provisioning_network *out,
				 size_t max, void *ctx);

#endif /* TEDGE_PROVISIONING_ZEPHYR_H_ */
