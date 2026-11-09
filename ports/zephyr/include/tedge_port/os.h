/* SPDX-License-Identifier: Apache-2.0 */

/* Zephyr implementation of the types in <tedge/port/os.h>: the kernel
 * objects themselves, defined with Zephyr's own macros. */

#ifndef TEDGE_PORT_ZEPHYR_OS_H_
#define TEDGE_PORT_ZEPHYR_OS_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

typedef struct k_mutex tedge_port_mutex_t;
#define TEDGE_PORT_MUTEX_DEFINE(name) static K_MUTEX_DEFINE(name)

typedef struct k_spinlock tedge_port_spinlock_t;
typedef k_spinlock_key_t tedge_port_spinlock_key_t;

/* Zephyr passes a thread three arguments; the port keeps the client's
 * entry and argument beside the thread and calls them from its own. */
typedef struct {
	struct k_thread thread;
	void (*entry)(void *arg);
	void *arg;
} tedge_port_thread_t;

typedef k_thread_stack_t tedge_port_stack_t;
#define TEDGE_PORT_STACK_DEFINE(name, size) static K_THREAD_STACK_DEFINE(name, size)
#define TEDGE_PORT_STACK_ARRAY_DEFINE(name, n, size)                           \
	static K_THREAD_STACK_ARRAY_DEFINE(name, n, size)
#define TEDGE_PORT_STACK_SIZEOF(stack) K_THREAD_STACK_SIZEOF(stack)

/* K_MSGQ_DEFINE also defines the buffer, so it cannot take "static". */
typedef struct k_msgq tedge_port_msgq_t;
#define TEDGE_PORT_MSGQ_DEFINE(name, msg_size, n) K_MSGQ_DEFINE(name, msg_size, n, 4)

typedef struct k_event tedge_port_event_t;
#define TEDGE_PORT_EVENT_DEFINE(name) static K_EVENT_DEFINE(name)

typedef struct k_sem tedge_port_sem_t;

typedef struct k_work_delayable tedge_port_delayed_work_t;
#define TEDGE_PORT_DELAYED_WORK_DEFINE(name, fn)                               \
	static void name##_run(struct k_work *work)                            \
	{                                                                      \
		ARG_UNUSED(work);                                              \
		fn();                                                          \
	}                                                                      \
	static K_WORK_DELAYABLE_DEFINE(name, name##_run)

typedef atomic_t tedge_port_atomic_t;
#define TEDGE_PORT_ATOMIC_INIT(value) ATOMIC_INIT(value)

#endif /* TEDGE_PORT_ZEPHYR_OS_H_ */
