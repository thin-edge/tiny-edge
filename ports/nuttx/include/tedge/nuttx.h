/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief What the NuttX port adds to the POSIX port: Wi-Fi from a stored
 * network, and set-up over USB (Improv Serial on the console).
 *
 * The application owns the network, as on every port. These are ready-made
 * pieces for it: the sample agent (samples/agent) uses all of them.
 */

#ifndef TEDGE_NUTTX_H_
#define TEDGE_NUTTX_H_

#include <tedge/provisioning.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Join the network stored in CONFIG_TINY_EDGE_WIFI_FILE, take an address
 * over DHCP, and start the NTP client.
 *
 * @return 0, -ENOENT when no network is stored, or another negative errno.
 */
int tedge_nuttx_wifi_join_stored(void);

/**
 * Join @p ssid with @p password (empty for an open network), take an
 * address over DHCP, and start the NTP client. Once joined, the network is
 * stored for the next boot; when it cannot be joined, the stored network
 * is joined again, so a mistyped password cannot strand the device.
 *
 * @return 0, or a negative errno.
 */
int tedge_nuttx_wifi_join(const char *ssid, const char *password);

/**
 * Scan for Wi-Fi networks: up to @p max entries of @p out, strongest first,
 * each network named once. Blocks for 15 s at most.
 *
 * @return The number of networks found, or a negative errno.
 */
int tedge_nuttx_wifi_scan(struct tedge_provisioning_network *out, size_t max);

/**
 * Start set-up over USB: Improv Serial on the console, with
 * tedge_nuttx_wifi_join() for the network, tedge_nuttx_wifi_scan() for the
 * networks the device sees, and the chip's name for Improv's device info. Members of @p overrides that are set replace the port's.
 * Call after tedge_init(). The console is switched to raw input, so it
 * cannot also run a shell.
 *
 * @return 0, or a negative errno.
 */
int tedge_provisioning_nuttx_init(const struct tedge_provisioning_hooks *overrides);

#ifdef __cplusplus
}
#endif

#endif /* TEDGE_NUTTX_H_ */
