/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief The device itself: the port interface.
 */

#ifndef TEDGE_PORT_DEVICE_H_
#define TEDGE_PORT_DEVICE_H_

#include <stddef.h>
#include <stdint.h>

/**
 * Why the device last reset, then clear it so the next boot reports its
 * own cause. The core calls it once per boot and keeps the answer.
 *
 * @param cause The platform's reset-cause flags. Their meaning is the
 *        port's: Zephyr reports its RESET_* hwinfo flags.
 * @return 0, or a negative errno when the platform cannot tell.
 */
int tedge_port_reset_cause(uint32_t *cause);

/**
 * The MAC address of the network interface the client uses. The default
 * device id is made from it, so it must be the same on every boot.
 *
 * @return 0, or a negative errno when there is no such interface yet.
 */
int tedge_port_mac_address(uint8_t mac[6]);

/**
 * Reset the whole device, now. The core has already given the
 * application's reset hook its chance, and flushed the log.
 *
 * A full-system reset, not a CPU-only one, where the platform makes the
 * difference: a radio left running across a CPU reset can hang the boot.
 */
_Noreturn void tedge_port_reset(void);

/* --- Crash dumps (TEDGE_COREDUMP) ---------------------------------------- */

/** Offer a stored crash dump, if there is one, as a log type the cloud
 *  can request. Called once at start-up. */
void tedge_port_coredump_init(void);

/** The dump reached the cloud and may be erased. */
void tedge_port_coredump_taken(void);

#endif /* TEDGE_PORT_DEVICE_H_ */
