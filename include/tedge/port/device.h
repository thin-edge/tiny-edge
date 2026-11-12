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

/** The firmware's name and version, as the build knows them, for an
 *  application that does not name its firmware itself. Never NULL. */
const char *tedge_port_firmware_name(void);
const char *tedge_port_firmware_version(void);

/** What the device runs on, as the cloud shows it. A member the port cannot
 *  tell is NULL or "". The strings stay valid for the program's life. */
struct tedge_port_platform {
	/** The operating system, lower case: "zephyr", "freertos", "linux". */
	const char *os;
	/** Its version, as the operating system states it. */
	const char *os_version;
	/** The SDK or framework above it, if any, with its version:
	 *  "esp-idf v6.1". */
	const char *sdk;
	/** The board, or the chip when the platform has no boards: c8y_Hardware's
	 *  model unless the application names it. */
	const char *model;
	/** The board's or the chip's revision: c8y_Hardware's revision. */
	const char *revision;
};

/** Fill @p out. The core calls it once, from tedge_init(). */
void tedge_port_platform(struct tedge_port_platform *out);

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
