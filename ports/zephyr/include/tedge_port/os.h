/* SPDX-License-Identifier: Apache-2.0 */

/* Zephyr implementation of the types in <tedge/port/os.h>. */

#ifndef TEDGE_PORT_ZEPHYR_OS_H_
#define TEDGE_PORT_ZEPHYR_OS_H_

#include <zephyr/kernel.h>

typedef struct k_mutex tedge_port_mutex_t;

#define TEDGE_PORT_MUTEX_DEFINE(name) static K_MUTEX_DEFINE(name)

#endif /* TEDGE_PORT_ZEPHYR_OS_H_ */
