/* SPDX-License-Identifier: Apache-2.0
 *
 * The FreeRTOS side of <tedge/port/os.h>: kernel objects created with the
 * *Static() calls on first use, a 64-bit uptime from the tick count, and
 * the client's heap as an allowance on pvPortMalloc().
 *
 * Only the FreeRTOS kernel API: what the platform adds (random numbers)
 * is in the platform's own sources.
 */

#include <tedge/port/config.h>
#include <tedge/port/log.h>
#include <tedge/port/os.h>

#include <errno.h>
#include <string.h>

#if !INCLUDE_vTaskSuspend || !INCLUDE_eTaskGetState || !INCLUDE_vTaskDelete
#error "tiny-edge needs INCLUDE_vTaskSuspend, INCLUDE_eTaskGetState and INCLUDE_vTaskDelete"
#endif

TEDGE_LOG_DECLARE();

/* --- Critical sections --------------------------------------------------- */

/* ESP-IDF runs FreeRTOS on two cores, and its critical sections take a
 * spinlock; mainline FreeRTOS's take nothing. */
#if defined(ESP_PLATFORM)
static portMUX_TYPE tedge_mux = portMUX_INITIALIZER_UNLOCKED;
#define ENTER() taskENTER_CRITICAL(&tedge_mux)
#define EXIT()  taskEXIT_CRITICAL(&tedge_mux)
#else
#define ENTER() taskENTER_CRITICAL()
#define EXIT()  taskEXIT_CRITICAL()
#endif

/* Create @p obj's kernel object on its first use. Two threads may race to
 * be first; the critical section lets one create it. The *Static() calls
 * only fill memory, which is allowed there. */
#define ONCE(obj, create)                                                      \
	do {                                                                   \
		if ((obj)->handle == NULL) {                                   \
			ENTER();                                               \
			if ((obj)->handle == NULL) {                           \
				(obj)->handle = (create);                      \
			}                                                      \
			EXIT();                                                \
		}                                                              \
	} while (0)

/* Milliseconds as ticks, rounded up so a short wait is never no wait,
 * and kept below portMAX_DELAY, which means "for ever". */
static TickType_t ticks(uint32_t ms)
{
	uint64_t t = ((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000;

	return (t >= (uint64_t)portMAX_DELAY) ? portMAX_DELAY - 1 : (TickType_t)t;
}

/* --- Mutexes ------------------------------------------------------------- */

void tedge_port_mutex_lock(tedge_port_mutex_t *mutex)
{
	ONCE(mutex, xSemaphoreCreateMutexStatic(&mutex->buf));
	(void)xSemaphoreTake(mutex->handle, portMAX_DELAY);
}

void tedge_port_mutex_unlock(tedge_port_mutex_t *mutex)
{
	(void)xSemaphoreGive(mutex->handle);
}

/* --- Spinlocks ---------------------------------------------------------- */

tedge_port_spinlock_key_t tedge_port_spin_lock(tedge_port_spinlock_t *lock)
{
	(void)lock;
#if defined(ESP_PLATFORM)
	/* The _SAFE form works in a task and in an interrupt. */
	portENTER_CRITICAL_SAFE(&tedge_mux);
	return 0;
#else
	/* Raises the interrupt mask, in a task as in an interrupt. */
	return taskENTER_CRITICAL_FROM_ISR();
#endif
}

void tedge_port_spin_unlock(tedge_port_spinlock_t *lock,
			    tedge_port_spinlock_key_t key)
{
	(void)lock;
#if defined(ESP_PLATFORM)
	(void)key;
	portEXIT_CRITICAL_SAFE(&tedge_mux);
#else
	taskEXIT_CRITICAL_FROM_ISR(key);
#endif
}

/* --- Threads ------------------------------------------------------------- */

/* A FreeRTOS task cannot simply return, and one that deletes itself is
 * only cleaned up later, by the idle task: its TCB and stack are still in
 * the kernel's lists until then, and a new task on them would corrupt
 * those lists. So a finished task suspends itself instead, and whoever
 * joins it or reuses its memory deletes it: a task that is not running is
 * deleted at once, also on ESP-IDF's two cores. */
static void thread_trampoline(void *p)
{
	tedge_port_thread_t *thread = p;

	thread->entry(thread->arg);
#if INCLUDE_uxTaskGetStackHighWaterMark
	/* What the thread left untouched of its stack, to size it. */
	TEDGE_LOG_DBG("thread %s ended: %u bytes of its stack never used",
		      pcTaskGetName(NULL),
		      (unsigned int)(uxTaskGetStackHighWaterMark(NULL) *
				     sizeof(StackType_t)));
#endif
	__atomic_store_n(&thread->finished, true, __ATOMIC_RELEASE);
	(void)xSemaphoreGive(thread->done);
	vTaskSuspend(NULL);
}

/* Delete @p thread's finished task, once it has suspended itself.
 * @return 0, or -EBUSY when it is still running. */
static int thread_reap(tedge_port_thread_t *thread)
{
	if (thread->handle == NULL) {
		return 0;
	}
	if (!__atomic_load_n(&thread->finished, __ATOMIC_ACQUIRE)) {
		return -EBUSY;
	}
	/* It is between giving "done" and suspending: a tick at most. */
	while (eTaskGetState(thread->handle) != eSuspended) {
		vTaskDelay(1);
	}
	vTaskDelete(thread->handle);
	thread->handle = NULL;
	return 0;
}

int tedge_port_thread_start(tedge_port_thread_t *thread,
			    tedge_port_stack_t *stack, size_t stack_size,
			    void (*entry)(void *arg), void *arg,
			    const char *name)
{
	int ret = thread_reap(thread);

	if (ret != 0) {
		return ret;
	}
	if (thread->done == NULL) {
		thread->done = xSemaphoreCreateBinaryStatic(&thread->done_buf);
	}
	(void)xSemaphoreTake(thread->done, 0);
	thread->finished = false;
	thread->entry = entry;
	thread->arg = arg;
	thread->handle = xTaskCreateStatic(thread_trampoline, name,
					   stack_size / sizeof(StackType_t),
					   thread, TEDGE_THREAD_PRIORITY, stack,
					   &thread->tcb);
	return (thread->handle != NULL) ? 0 : -EINVAL;
}

int tedge_port_thread_join(tedge_port_thread_t *thread, uint32_t timeout_ms)
{
	if (thread->handle == NULL) {
		return 0;
	}
	if (!__atomic_load_n(&thread->finished, __ATOMIC_ACQUIRE) &&
	    xSemaphoreTake(thread->done, ticks(timeout_ms)) != pdTRUE) {
		return -EAGAIN;
	}
	return thread_reap(thread);
}

void tedge_port_sleep_ms(uint32_t ms)
{
	vTaskDelay(ticks(ms));
}

/* --- Message queues ------------------------------------------------------ */

int tedge_port_msgq_put(tedge_port_msgq_t *queue, const void *msg)
{
	ONCE(queue, xQueueCreateStatic(queue->len, queue->msg_size,
				       queue->storage, &queue->buf));
	return (xQueueSend(queue->handle, msg, 0) == pdTRUE) ? 0 : -ENOMSG;
}

int tedge_port_msgq_get(tedge_port_msgq_t *queue, void *msg)
{
	ONCE(queue, xQueueCreateStatic(queue->len, queue->msg_size,
				       queue->storage, &queue->buf));
	return (xQueueReceive(queue->handle, msg, 0) == pdTRUE) ? 0 : -ENOMSG;
}

/* --- Event flags --------------------------------------------------------- */

/* An event group holds 24 flags with 32-bit ticks (the top byte is the
 * kernel's own). */
#define EVENT_BITS 0x00ffffffu

static EventGroupHandle_t event_group(tedge_port_event_t *event)
{
	ONCE(event, xEventGroupCreateStatic(&event->buf));
	return event->handle;
}

void tedge_port_event_post(tedge_port_event_t *event, uint32_t bits)
{
	configASSERT((bits & ~EVENT_BITS) == 0);
	(void)xEventGroupSetBits(event_group(event), bits & EVENT_BITS);
}

void tedge_port_event_clear(tedge_port_event_t *event, uint32_t bits)
{
	(void)xEventGroupClearBits(event_group(event), bits & EVENT_BITS);
}

uint32_t tedge_port_event_test(tedge_port_event_t *event, uint32_t bits)
{
	return (uint32_t)xEventGroupGetBits(event_group(event)) & bits;
}

uint32_t tedge_port_event_wait(tedge_port_event_t *event, uint32_t bits,
			       uint32_t timeout_ms)
{
	/* Any of the bits, left set: the return value holds the bits at the
	 * end of the wait, so a timeout leaves those asked for clear. */
	return (uint32_t)xEventGroupWaitBits(event_group(event),
					     bits & EVENT_BITS, pdFALSE,
					     pdFALSE, ticks(timeout_ms)) &
	       bits;
}

/* --- Semaphores ---------------------------------------------------------- */

void tedge_port_sem_init(tedge_port_sem_t *sem, unsigned int count,
			 unsigned int limit)
{
	if (sem->handle != NULL) {
		vSemaphoreDelete(sem->handle); /* static: only unregisters it */
	}
	sem->handle = xSemaphoreCreateCountingStatic(limit, count, &sem->buf);
}

void tedge_port_sem_give(tedge_port_sem_t *sem)
{
	/* At the limit the give fails, which is the interface's "up to". */
	(void)xSemaphoreGive(sem->handle);
}

int tedge_port_sem_take(tedge_port_sem_t *sem, uint32_t timeout_ms)
{
	return (xSemaphoreTake(sem->handle, ticks(timeout_ms)) == pdTRUE)
		       ? 0 : -EAGAIN;
}

void tedge_port_sem_reset(tedge_port_sem_t *sem)
{
	while (xSemaphoreTake(sem->handle, 0) == pdTRUE) {
	}
}

/* --- Delayed work -------------------------------------------------------- */

static void work_timer_fn(TimerHandle_t timer)
{
	tedge_port_delayed_work_t *work = pvTimerGetTimerID(timer);

	work->fn();
}

void tedge_port_delayed_work_schedule(tedge_port_delayed_work_t *work,
				      uint32_t delay_ms)
{
	TickType_t t = ticks(delay_ms);

	ONCE(work, xTimerCreateStatic(work->name, 1, pdFALSE, work,
				      work_timer_fn, &work->buf));
	/* Changing the period starts a stopped timer and restarts a running
	 * one: "reschedule it if pending". */
	(void)xTimerChangePeriod(work->handle, (t > 0) ? t : 1, portMAX_DELAY);
}

void tedge_port_delayed_work_cancel(tedge_port_delayed_work_t *work)
{
	if (work->handle != NULL) {
		(void)xTimerStop(work->handle, portMAX_DELAY);
	}
}

/* --- Atomics ------------------------------------------------------------- */

intptr_t tedge_port_atomic_get(const tedge_port_atomic_t *atomic)
{
	return __atomic_load_n(atomic, __ATOMIC_SEQ_CST);
}

intptr_t tedge_port_atomic_set(tedge_port_atomic_t *atomic, intptr_t value)
{
	return __atomic_exchange_n(atomic, value, __ATOMIC_SEQ_CST);
}

bool tedge_port_atomic_cas(tedge_port_atomic_t *atomic, intptr_t expected,
			   intptr_t desired)
{
	return __atomic_compare_exchange_n(atomic, &expected, desired, false,
					   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* --- Time ---------------------------------------------------------------- */

/* The tick count wraps (after 49 days at 1 kHz with 32-bit ticks); the
 * wraps are counted here. The client asks far more often than that. */
int64_t tedge_port_uptime_ms(void)
{
	static TickType_t last;
	static uint64_t wraps;
	uint64_t t;

	ENTER();
	TickType_t now = xTaskGetTickCount();

	if (sizeof(TickType_t) < sizeof(uint64_t)) {
		if (now < last) {
			wraps++;
		}
		last = now;
		t = (wraps << (8 * sizeof(TickType_t))) | now;
	} else {
		t = now;
	}
	EXIT();
	return (int64_t)(t * 1000 / configTICK_RATE_HZ);
}

/* --- The client's heap --------------------------------------------------- */

/* An allowance of TEDGE_HEAP_SIZE bytes on the FreeRTOS heap: the client
 * never takes more, and what it uses is counted. A header in front of each
 * block records its size, aligned for any type. */
typedef union {
	size_t size;
	long double align_ld;
	void *align_p;
	long long align_ll;
} heap_header;

static size_t heap_used;

void *tedge_port_heap_alloc(size_t size)
{
	heap_header *h;

	ENTER();
	if (size > TEDGE_HEAP_SIZE - heap_used) {
		EXIT();
		return NULL;
	}
	heap_used += size;
	EXIT();

	h = pvPortMalloc(sizeof(*h) + size);
	if (h == NULL) {
		ENTER();
		heap_used -= size;
		EXIT();
		return NULL;
	}
	h->size = size;
	return h + 1;
}

void tedge_port_heap_free(void *p)
{
	heap_header *h;

	if (p == NULL) {
		return;
	}
	h = (heap_header *)p - 1;
	ENTER();
	heap_used -= h->size;
	EXIT();
	vPortFree(h);
}

size_t tedge_port_heap_free_bytes(void)
{
	size_t free_bytes;

	ENTER();
	free_bytes = TEDGE_HEAP_SIZE - heap_used;
	EXIT();
	return free_bytes;
}
