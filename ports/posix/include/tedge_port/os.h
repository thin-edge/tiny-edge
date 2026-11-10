/* SPDX-License-Identifier: Apache-2.0 */

/* POSIX implementation of the types in <tedge/port/os.h>: pthreads.
 *
 * Objects defined with a DEFINE macro are ready at once (pthreads' static
 * initialisers). The "plain variable" ones (threads, semaphores, spinlocks)
 * are zeroed memory, which is not a valid pthread object everywhere (it is
 * on Linux, not on macOS), so their pthread parts are set up by
 * tedge_port_thread_start() and tedge_port_sem_init(), or not used at all.
 *
 * Stacks are the one thing the port does not take from the core: the sizes
 * the core asks for are an MCU's, below what a POSIX C library needs, so
 * each thread gets the system's default stack. A stack "defined" here is a
 * placeholder of one byte, and the size passed with it is not used. */

#ifndef TEDGE_PORT_POSIX_OS_H_
#define TEDGE_PORT_POSIX_OS_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef pthread_mutex_t tedge_port_mutex_t;
#define TEDGE_PORT_MUTEX_DEFINE(name)                                          \
	static tedge_port_mutex_t name = PTHREAD_MUTEX_INITIALIZER

/* Nothing on POSIX runs in an interrupt, but the lock may be taken while a
 * mutex is held elsewhere (the log), so it is a lock of its own: a flag
 * spun on with the compiler's atomics, which zeroed memory initialises. */
typedef struct {
	volatile char locked;
} tedge_port_spinlock_t;
typedef int tedge_port_spinlock_key_t;

typedef struct {
	pthread_t id;
	pthread_mutex_t lock;
	pthread_cond_t done_cond;
	bool ready; /* lock and done_cond are initialised */
	bool done;
	void (*entry)(void *arg);
	void *arg;
} tedge_port_thread_t;

typedef uint8_t tedge_port_stack_t;
#define TEDGE_PORT_STACK_DEFINE(name, size) static tedge_port_stack_t name[1]
#define TEDGE_PORT_STACK_ARRAY_DEFINE(name, n, size)                           \
	static tedge_port_stack_t name[n][1]
#define TEDGE_PORT_STACK_SIZEOF(stack) sizeof(stack)

typedef struct {
	pthread_mutex_t lock;
	uint8_t *buf;
	size_t msg_size;
	size_t max;
	size_t head;
	size_t count;
} tedge_port_msgq_t;
#define TEDGE_PORT_MSGQ_DEFINE(name, msg_size, n)                              \
	static uint8_t name##_buf[(msg_size) * (n)];                           \
	tedge_port_msgq_t name = { PTHREAD_MUTEX_INITIALIZER, name##_buf,      \
				   (msg_size), (n), 0, 0 }

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t bits;
} tedge_port_event_t;
#define TEDGE_PORT_EVENT_DEFINE(name)                                          \
	static tedge_port_event_t name = { PTHREAD_MUTEX_INITIALIZER,          \
					   PTHREAD_COND_INITIALIZER, 0 }

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	unsigned int count;
	unsigned int limit;
} tedge_port_sem_t;

/* Run by the port's one work thread (tedge_port_os.c), in due order. */
typedef struct tedge_port_delayed_work {
	void (*fn)(void);
	int64_t due_ms;
	bool pending;
	struct tedge_port_delayed_work *next;
} tedge_port_delayed_work_t;
#define TEDGE_PORT_DELAYED_WORK_DEFINE(name, fn)                               \
	static tedge_port_delayed_work_t name = { (fn), 0, false, NULL }

/* Read and written with the compiler's __atomic built-ins (GCC, Clang). */
typedef intptr_t tedge_port_atomic_t;
#define TEDGE_PORT_ATOMIC_INIT(value) (value)

#endif /* TEDGE_PORT_POSIX_OS_H_ */
