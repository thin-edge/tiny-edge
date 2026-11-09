/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Kernel services: the port interface.
 *
 * The few kernel services the core needs, named for what the core does
 * with them. The port implements the functions and, in <tedge_port/os.h>,
 * the types the core allocates statically:
 *
 * - tedge_port_mutex_t, and TEDGE_PORT_MUTEX_DEFINE(name), which defines
 *   a file-scope (static) mutex that is ready before the first lock.
 *   Zephyr: struct k_mutex and K_MUTEX_DEFINE.
 *
 * Every function here may be called from any thread the client or the
 * application runs, but not from an interrupt.
 */

#ifndef TEDGE_PORT_OS_H_
#define TEDGE_PORT_OS_H_

#include <tedge_port/os.h>

#include <stdint.h>

/** Take @p mutex, waiting as long as it takes. Recursive locking is not
 *  required of the port, and the core never does it. */
void tedge_port_mutex_lock(tedge_port_mutex_t *mutex);

/** Release @p mutex, which the calling thread holds. */
void tedge_port_mutex_unlock(tedge_port_mutex_t *mutex);

/** Milliseconds since boot. Monotonic: it does not jump when the wall
 *  clock is set. */
int64_t tedge_port_uptime_ms(void);

/** 32 random bits, for jitter and similar uses. Not for keys: those come
 *  from the port's crypto. */
uint32_t tedge_port_random32(void);

#endif /* TEDGE_PORT_OS_H_ */
