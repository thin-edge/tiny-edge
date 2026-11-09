/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr side of <tedge/port/os.h>: each service is a thin call into
 * the kernel, and the client's heap is a k_heap.
 */

#include <tedge/port/os.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/sys_heap.h>

#include <errno.h>

/* --- Mutexes ------------------------------------------------------------- */

void tedge_port_mutex_lock(tedge_port_mutex_t *mutex)
{
	(void)k_mutex_lock(mutex, K_FOREVER);
}

void tedge_port_mutex_unlock(tedge_port_mutex_t *mutex)
{
	(void)k_mutex_unlock(mutex);
}

/* --- Spinlocks ---------------------------------------------------------- */

tedge_port_spinlock_key_t tedge_port_spin_lock(tedge_port_spinlock_t *lock)
{
	return k_spin_lock(lock);
}

void tedge_port_spin_unlock(tedge_port_spinlock_t *lock,
			    tedge_port_spinlock_key_t key)
{
	k_spin_unlock(lock, key);
}

/* --- Threads ------------------------------------------------------------- */

static void thread_entry(void *p1, void *p2, void *p3)
{
	tedge_port_thread_t *thread = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	thread->entry(thread->arg);
}

int tedge_port_thread_start(tedge_port_thread_t *thread,
			    tedge_port_stack_t *stack, size_t stack_size,
			    void (*entry)(void *arg), void *arg,
			    const char *name)
{
	thread->entry = entry;
	thread->arg = arg;
	k_thread_create(&thread->thread, stack, stack_size, thread_entry,
			thread, NULL, NULL,
			K_PRIO_PREEMPT(CONFIG_TEDGE_THREAD_PRIORITY), 0,
			K_NO_WAIT);
	(void)k_thread_name_set(&thread->thread, name);
	return 0;
}

int tedge_port_thread_join(tedge_port_thread_t *thread, uint32_t timeout_ms)
{
	return (k_thread_join(&thread->thread, K_MSEC(timeout_ms)) == 0)
		       ? 0 : -EAGAIN;
}

void tedge_port_sleep_ms(uint32_t ms)
{
	(void)k_msleep((int32_t)ms);
}

/* --- Message queues ------------------------------------------------------ */

int tedge_port_msgq_put(tedge_port_msgq_t *queue, const void *msg)
{
	return k_msgq_put(queue, msg, K_NO_WAIT);
}

int tedge_port_msgq_get(tedge_port_msgq_t *queue, void *msg)
{
	return k_msgq_get(queue, msg, K_NO_WAIT);
}

/* --- Event flags --------------------------------------------------------- */

void tedge_port_event_post(tedge_port_event_t *event, uint32_t bits)
{
	(void)k_event_post(event, bits);
}

void tedge_port_event_clear(tedge_port_event_t *event, uint32_t bits)
{
	(void)k_event_clear(event, bits);
}

uint32_t tedge_port_event_test(tedge_port_event_t *event, uint32_t bits)
{
	return k_event_test(event, bits);
}

uint32_t tedge_port_event_wait(tedge_port_event_t *event, uint32_t bits,
			       uint32_t timeout_ms)
{
	return k_event_wait(event, bits, false, K_MSEC(timeout_ms));
}

/* --- Semaphores ---------------------------------------------------------- */

void tedge_port_sem_init(tedge_port_sem_t *sem, unsigned int count,
			 unsigned int limit)
{
	(void)k_sem_init(sem, count, limit);
}

void tedge_port_sem_give(tedge_port_sem_t *sem)
{
	k_sem_give(sem);
}

int tedge_port_sem_take(tedge_port_sem_t *sem, uint32_t timeout_ms)
{
	return (k_sem_take(sem, K_MSEC(timeout_ms)) == 0) ? 0 : -EAGAIN;
}

void tedge_port_sem_reset(tedge_port_sem_t *sem)
{
	k_sem_reset(sem);
}

/* --- Delayed work -------------------------------------------------------- */

void tedge_port_delayed_work_schedule(tedge_port_delayed_work_t *work,
				      uint32_t delay_ms)
{
	(void)k_work_schedule(work, K_MSEC(delay_ms));
}

void tedge_port_delayed_work_cancel(tedge_port_delayed_work_t *work)
{
	(void)k_work_cancel_delayable(work);
}

/* --- Atomics ------------------------------------------------------------- */

BUILD_ASSERT(sizeof(atomic_val_t) == sizeof(intptr_t));

intptr_t tedge_port_atomic_get(const tedge_port_atomic_t *atomic)
{
	return (intptr_t)atomic_get(atomic);
}

intptr_t tedge_port_atomic_set(tedge_port_atomic_t *atomic, intptr_t value)
{
	return (intptr_t)atomic_set(atomic, (atomic_val_t)value);
}

bool tedge_port_atomic_cas(tedge_port_atomic_t *atomic, intptr_t expected,
			   intptr_t desired)
{
	return atomic_cas(atomic, (atomic_val_t)expected,
			  (atomic_val_t)desired);
}

/* --- Time and randomness ------------------------------------------------- */

int64_t tedge_port_uptime_ms(void)
{
	return k_uptime_get();
}

uint32_t tedge_port_random32(void)
{
	return sys_rand32_get();
}

/* --- The client's heap --------------------------------------------------- */

/* By default the heap's memory is ordinary .bss rather than what
 * K_HEAP_DEFINE() uses (.noinit): on the classic ESP32 .noinit lands in
 * dram1, the 96 KB region that also holds every thread stack and the
 * network buffers, while .bss goes to dram0, where the libc malloc arena
 * gets whatever is left. Moving the 10-16 KB heap across is what lets the
 * ESP32-CAM carry log upload (reduce-memory-footprint, 2026-09-23). A board
 * whose dram0 is the full one (the WROOM-32, mbedTLS heap in dram0) keeps
 * it in .noinit with CONFIG_TEDGE_HEAP_NOINIT. On SoCs with one RAM region
 * nothing moves either way. */
#if defined(CONFIG_TEDGE_HEAP_NOINIT)
K_HEAP_DEFINE(tedge_heap, CONFIG_TEDGE_HEAP_SIZE);
#else
static uint8_t __aligned(8) tedge_heap_mem[CONFIG_TEDGE_HEAP_SIZE];
static struct k_heap tedge_heap;

/* Before the application runs, so the heap is ready before tedge_init(). */
static int tedge_heap_init(void)
{
	k_heap_init(&tedge_heap, tedge_heap_mem, sizeof(tedge_heap_mem));
	return 0;
}
SYS_INIT(tedge_heap_init, POST_KERNEL, 0);
#endif

void *tedge_port_heap_alloc(size_t size)
{
	return k_heap_alloc(&tedge_heap, size, K_NO_WAIT);
}

void tedge_port_heap_free(void *p)
{
	if (p != NULL) {
		k_heap_free(&tedge_heap, p);
	}
}

/* The statistics are only built when something reads them (the health
 * measurements select them). */
size_t tedge_port_heap_free_bytes(void)
{
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats stats;

	if (sys_heap_runtime_stats_get(&tedge_heap.heap, &stats) != 0) {
		return 0;
	}
	return stats.free_bytes;
#else
	return 0;
#endif
}
