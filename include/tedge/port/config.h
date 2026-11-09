/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Build configuration, as the core reads it.
 *
 * The core never reads an RTOS configuration system. It reads TEDGE_*
 * macros from <tedge_config.h>, which each port provides in its own way:
 *
 * - Zephyr generates it from Kconfig, without the CONFIG_ prefix: the
 *   Kconfig option TEDGE_HEALTH is TEDGE_HEALTH to the core
 *   (ports/zephyr/cmake/tedge_config.cmake).
 * - Other ports have the application write a tedge_user_config.h, in the
 *   style of FreeRTOSConfig.h and tx_user.h.
 *
 * A feature that is off is not defined at all; one that is on is defined
 * to 1. A value (a size, an interval, a string) is defined to that value.
 *
 * <tedge_config.h> also defines TEDGE_CONFIG_NAME(name): how a user of
 * the port spells the option @p name, as a string, for messages that tell
 * an operator which option to change.
 */

#ifndef TEDGE_PORT_CONFIG_H_
#define TEDGE_PORT_CONFIG_H_

#include <tedge_config.h>

/**
 * 1 when the feature @p option is on, 0 when it is off, in an ordinary
 * expression: `if (!TEDGE_IS_ENABLED(TEDGE_TELEMETRY))`. The compiler
 * still checks the code that is switched off, which an #if would hide.
 *
 * It works because an option that is on is defined to 1: the helpers turn
 * "1" into a second argument that the selection picks up. Zephyr and the
 * Linux kernel use the same trick for their IS_ENABLED.
 */
#define TEDGE_IS_ENABLED(option) TEDGE_IS_ENABLED_1_(option)
#define TEDGE_IS_ENABLED_1_(value) TEDGE_IS_ENABLED_2_(TEDGE_ON_##value)
#define TEDGE_ON_1 0,
#define TEDGE_IS_ENABLED_2_(arg) TEDGE_IS_ENABLED_3_(arg 1, 0, 0)
#define TEDGE_IS_ENABLED_3_(ignored, value, ...) value

#endif /* TEDGE_PORT_CONFIG_H_ */
