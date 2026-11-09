/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief The device itself: the port interface.
 */

#ifndef TEDGE_PORT_DEVICE_H_
#define TEDGE_PORT_DEVICE_H_

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

#endif /* TEDGE_PORT_DEVICE_H_ */
