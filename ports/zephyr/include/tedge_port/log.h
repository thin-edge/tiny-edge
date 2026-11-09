/* SPDX-License-Identifier: Apache-2.0 */

/* Zephyr implementation of the logging macros in <tedge/port/log.h>: the
 * client logs as the "tedge" module of Zephyr's logging subsystem, at the
 * level CONFIG_TEDGE_LOG_LEVEL sets. */

#ifndef TEDGE_PORT_ZEPHYR_LOG_H_
#define TEDGE_PORT_ZEPHYR_LOG_H_

#include <zephyr/logging/log.h>

#define TEDGE_LOG_DECLARE() LOG_MODULE_DECLARE(tedge, CONFIG_TEDGE_LOG_LEVEL)

#define TEDGE_LOG_ERR(...) LOG_ERR(__VA_ARGS__)
#define TEDGE_LOG_WRN(...) LOG_WRN(__VA_ARGS__)
#define TEDGE_LOG_INF(...) LOG_INF(__VA_ARGS__)
#define TEDGE_LOG_DBG(...) LOG_DBG(__VA_ARGS__)

#if defined(CONFIG_LOG_RUNTIME_FILTERING)
#define TEDGE_PORT_LOG_RUNTIME_LEVEL 1
#endif

#endif /* TEDGE_PORT_ZEPHYR_LOG_H_ */
