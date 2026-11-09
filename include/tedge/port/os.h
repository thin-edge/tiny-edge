/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file
 * @brief Kernel services: the port interface.
 *
 * The kernel services the core needs, named for what the core does with
 * them. Every object is allocated statically by the core, as each RTOS the
 * client targets allows (Zephyr's K_*_DEFINE, FreeRTOS's *Static() calls,
 * ThreadX's caller-supplied control blocks), so the client never takes
 * kernel memory at run time.
 *
 * The port implements the functions below and, in <tedge_port/os.h>, the
 * types and the macros that define objects:
 *
 * | Type                      | Defined with                                 |
 * |---------------------------|----------------------------------------------|
 * | tedge_port_mutex_t        | TEDGE_PORT_MUTEX_DEFINE(name)                |
 * | tedge_port_spinlock_t     | a plain variable, zeroed                     |
 * | tedge_port_thread_t       | a plain variable                             |
 * | tedge_port_stack_t        | TEDGE_PORT_STACK_DEFINE(name, size)          |
 * |                           | TEDGE_PORT_STACK_ARRAY_DEFINE(name, n, size) |
 * | tedge_port_msgq_t         | TEDGE_PORT_MSGQ_DEFINE(name, msg_size, n)    |
 * | tedge_port_event_t        | TEDGE_PORT_EVENT_DEFINE(name)                |
 * | tedge_port_sem_t          | a plain variable, then tedge_port_sem_init() |
 * | tedge_port_delayed_work_t | TEDGE_PORT_DELAYED_WORK_DEFINE(name, fn)     |
 * | tedge_port_atomic_t       | a plain variable = TEDGE_PORT_ATOMIC_INIT(v) |
 *
 * The DEFINE macros define objects at file scope that are ready before
 * their first use. The linker may see their names, so each must be unique
 * in the image. TEDGE_PORT_STACK_SIZEOF(stack) is the size to pass for a
 * stack defined with them.
 *
 * Unless a function says otherwise, it may be called from any thread the
 * client or the application runs, but not from an interrupt.
 */

#ifndef TEDGE_PORT_OS_H_
#define TEDGE_PORT_OS_H_

#include <tedge_port/os.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Mutexes ------------------------------------------------------------- */

/** Take @p mutex, waiting as long as it takes. Recursive locking is not
 *  required of the port, and the core never does it. */
void tedge_port_mutex_lock(tedge_port_mutex_t *mutex);

/** Release @p mutex, which the calling thread holds. */
void tedge_port_mutex_unlock(tedge_port_mutex_t *mutex);

/* --- Spinlocks ---------------------------------------------------------- */

/* For the few places that may be entered from any context, the RTOS's log
 * output among them: they never sleep, and are held for a few copies only.
 * tedge_port_spinlock_key_t is what the lock returns for its unlock. */

/** Take @p lock, from any context, with interrupts held off. */
tedge_port_spinlock_key_t tedge_port_spin_lock(tedge_port_spinlock_t *lock);

/** Release @p lock with the key its lock returned. */
void tedge_port_spin_unlock(tedge_port_spinlock_t *lock,
			    tedge_port_spinlock_key_t key);

/* --- Threads ------------------------------------------------------------- */

/**
 * Start a thread running @p entry(@p arg) on @p stack. Every client thread
 * runs at the client's priority (TEDGE_THREAD_PRIORITY, in the RTOS's own
 * scale), preemptible.
 *
 * @p thread and @p stack belong to the thread until it returns, and may be
 * used for a new thread after that.
 *
 * @param name A name for debugging, where the RTOS keeps one.
 * @return 0, or a negative errno.
 */
int tedge_port_thread_start(tedge_port_thread_t *thread,
			    tedge_port_stack_t *stack, size_t stack_size,
			    void (*entry)(void *arg), void *arg,
			    const char *name);

/**
 * Wait up to @p timeout_ms for @p thread to return.
 *
 * @return 0 once it has, or -EAGAIN when it is still running.
 */
int tedge_port_thread_join(tedge_port_thread_t *thread, uint32_t timeout_ms);

/** Let the calling thread sleep for @p ms. */
void tedge_port_sleep_ms(uint32_t ms);

/* --- Message queues ------------------------------------------------------ */

/** Copy one message of the queue's size into @p queue, without waiting.
 *  @return 0, or a negative errno when the queue is full. */
int tedge_port_msgq_put(tedge_port_msgq_t *queue, const void *msg);

/** Take the oldest message out of @p queue, without waiting.
 *  @return 0, or a negative errno when the queue is empty. */
int tedge_port_msgq_get(tedge_port_msgq_t *queue, void *msg);

/* --- Event flags --------------------------------------------------------- */

/* 32 flags that threads set, clear and wait for. The core uses one set for
 * the client thread's wake-ups. */

/** Set the flags in @p bits. May also be called from an RTOS callback,
 *  such as a network event handler. */
void tedge_port_event_post(tedge_port_event_t *event, uint32_t bits);

/** Clear the flags in @p bits. */
void tedge_port_event_clear(tedge_port_event_t *event, uint32_t bits);

/** @return The flags of @p bits that are set now. */
uint32_t tedge_port_event_test(tedge_port_event_t *event, uint32_t bits);

/**
 * Wait up to @p timeout_ms for any flag of @p bits to be set. The flags
 * stay set.
 *
 * @return The flags of @p bits that are set, or 0 on a timeout.
 */
uint32_t tedge_port_event_wait(tedge_port_event_t *event, uint32_t bits,
			       uint32_t timeout_ms);

/* --- Semaphores ---------------------------------------------------------- */

/** Initialise @p sem to @p count, which can rise to @p limit. */
void tedge_port_sem_init(tedge_port_sem_t *sem, unsigned int count,
			 unsigned int limit);

/** Add one to @p sem, up to its limit. May also be called from an RTOS
 *  callback, such as a DNS resolver's. */
void tedge_port_sem_give(tedge_port_sem_t *sem);

/** Take one from @p sem, waiting up to @p timeout_ms.
 *  @return 0, or -EAGAIN on a timeout. */
int tedge_port_sem_take(tedge_port_sem_t *sem, uint32_t timeout_ms);

/** Set @p sem's count back to 0. */
void tedge_port_sem_reset(tedge_port_sem_t *sem);

/* --- Delayed work -------------------------------------------------------- */

/* A function, void fn(void), that runs once, later, on a thread the port
 * owns (Zephyr's system work queue, a FreeRTOS timer task). It is named in
 * TEDGE_PORT_DELAYED_WORK_DEFINE(name, fn). */

/** Run @p work's function in @p delay_ms, or reschedule it if pending. */
void tedge_port_delayed_work_schedule(tedge_port_delayed_work_t *work,
				      uint32_t delay_ms);

/** Cancel @p work if it has not started. */
void tedge_port_delayed_work_cancel(tedge_port_delayed_work_t *work);

/* --- Atomics ------------------------------------------------------------- */

/* A word that threads read and change without a lock. In the port rather
 * than C11 <stdatomic.h>, which not every toolchain and CPU the client
 * targets provides lock-free. */

/** @return The value of @p atomic. */
intptr_t tedge_port_atomic_get(const tedge_port_atomic_t *atomic);

/** Set @p atomic to @p value. @return The value it had. */
intptr_t tedge_port_atomic_set(tedge_port_atomic_t *atomic, intptr_t value);

/** Set @p atomic to @p desired if it holds @p expected.
 *  @return true when it did, and was set. */
bool tedge_port_atomic_cas(tedge_port_atomic_t *atomic, intptr_t expected,
			   intptr_t desired);

/* --- Time and randomness ------------------------------------------------- */

/** Milliseconds since boot. Monotonic: it does not jump when the wall
 *  clock is set. */
int64_t tedge_port_uptime_ms(void);

/** 32 random bits, for jitter and similar uses. Not for keys: those come
 *  from the port's crypto. */
uint32_t tedge_port_random32(void);

/* --- The client's heap --------------------------------------------------- */

/* A heap of the client's own, TEDGE_HEAP_SIZE bytes, so the client never
 * takes memory the application counted on, and its use can be measured. */

/** @return @p size bytes from the client's heap, or NULL. Never waits. */
void *tedge_port_heap_alloc(size_t size);

/** Return @p p, which may be NULL, to the client's heap. */
void tedge_port_heap_free(void *p);

/** @return The bytes free in the client's heap now. */
size_t tedge_port_heap_free_bytes(void);

#endif /* TEDGE_PORT_OS_H_ */
