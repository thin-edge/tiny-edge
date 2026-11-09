/* SPDX-License-Identifier: Apache-2.0 */

/* The types of <tedge/port/os.h>, with nothing behind them: enough to
 * compile the core, not to run it. */

#ifndef TEDGE_HOST_COMPILE_OS_H_
#define TEDGE_HOST_COMPILE_OS_H_

#include <stddef.h>
#include <stdint.h>

typedef struct { int unused; } tedge_port_mutex_t;
#define TEDGE_PORT_MUTEX_DEFINE(name) static tedge_port_mutex_t name

typedef struct { int unused; } tedge_port_spinlock_t;
typedef unsigned int tedge_port_spinlock_key_t;

typedef struct { int unused; } tedge_port_thread_t;
typedef uint8_t tedge_port_stack_t;
#define TEDGE_PORT_STACK_DEFINE(name, size) static tedge_port_stack_t name[size]
#define TEDGE_PORT_STACK_ARRAY_DEFINE(name, n, size)                           \
	static tedge_port_stack_t name[n][size]
#define TEDGE_PORT_STACK_SIZEOF(stack) sizeof(stack)

typedef struct { int unused; } tedge_port_msgq_t;
#define TEDGE_PORT_MSGQ_DEFINE(name, msg_size, n) static tedge_port_msgq_t name

typedef struct { int unused; } tedge_port_event_t;
#define TEDGE_PORT_EVENT_DEFINE(name) static tedge_port_event_t name

typedef struct { int unused; } tedge_port_sem_t;

typedef struct { void (*fn)(void); } tedge_port_delayed_work_t;
#define TEDGE_PORT_DELAYED_WORK_DEFINE(name, fn)                               \
	static tedge_port_delayed_work_t name = { fn }

typedef intptr_t tedge_port_atomic_t;
#define TEDGE_PORT_ATOMIC_INIT(value) (value)

#endif /* TEDGE_HOST_COMPILE_OS_H_ */
