/* SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX side of <tedge/port/os.h>: pthreads, one thread of the port's
 * own for delayed work, and the client's heap as an allowance on malloc().
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* pthread_setname_np() */
#endif

#include <tedge/port/os.h>

#include <tedge/port/config.h>

#include "tedge_posix_internal.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* --- Waiting ------------------------------------------------------------- */

/* Every timed wait here is measured on the monotonic clock, so setting the
 * wall clock does not stretch or cut it. pthread_cond_timedwait() takes a
 * wall-clock deadline (macOS cannot change a condition's clock, and the
 * statically initialised ones cannot anywhere), so a wait is cut into
 * slices of at most a second, each re-measured: a clock change can then
 * shift a wait by a second at most. */
#define WAIT_SLICE_MS 1000

static int64_t monotonic_ms(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wait on @p cond until @p deadline_ms (monotonic). Returns false once the
 * deadline has passed; the caller re-checks its condition either way. */
static bool cond_wait_until(pthread_cond_t *cond, pthread_mutex_t *lock,
			    int64_t deadline_ms)
{
	int64_t left = deadline_ms - monotonic_ms();
	struct timespec ts;

	if (left <= 0) {
		return false;
	}
	if (left > WAIT_SLICE_MS) {
		left = WAIT_SLICE_MS;
	}
	(void)clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += left / 1000;
	ts.tv_nsec += (left % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	(void)pthread_cond_timedwait(cond, lock, &ts);
	return true;
}

/* --- Mutexes ------------------------------------------------------------- */

void tedge_port_mutex_lock(tedge_port_mutex_t *mutex)
{
	(void)pthread_mutex_lock(mutex);
}

void tedge_port_mutex_unlock(tedge_port_mutex_t *mutex)
{
	(void)pthread_mutex_unlock(mutex);
}

/* --- Spinlocks ---------------------------------------------------------- */

tedge_port_spinlock_key_t tedge_port_spin_lock(tedge_port_spinlock_t *lock)
{
	while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
		sched_yield();
	}
	return 0;
}

void tedge_port_spin_unlock(tedge_port_spinlock_t *lock,
			    tedge_port_spinlock_key_t key)
{
	(void)key;
	__atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

/* --- Threads ------------------------------------------------------------- */

static void *thread_entry(void *p)
{
	tedge_port_thread_t *thread = p;

	thread->entry(thread->arg);
	(void)pthread_mutex_lock(&thread->lock);
	thread->done = true;
	(void)pthread_cond_broadcast(&thread->done_cond);
	(void)pthread_mutex_unlock(&thread->lock);
	return NULL;
}

int tedge_port_thread_start(tedge_port_thread_t *thread,
			    tedge_port_stack_t *stack, size_t stack_size,
			    void (*entry)(void *arg), void *arg,
			    const char *name)
{
	int ret;

	(void)stack;
	(void)stack_size;
	/* Once only: a thread that has just signalled its end may still be
	 * unlocking when its successor starts. */
	if (!thread->ready) {
		(void)pthread_mutex_init(&thread->lock, NULL);
		(void)pthread_cond_init(&thread->done_cond, NULL);
		thread->ready = true;
	}
	(void)pthread_mutex_lock(&thread->lock);
	thread->entry = entry;
	thread->arg = arg;
	thread->done = false;
	(void)pthread_mutex_unlock(&thread->lock);
	/* Detached: tedge_port_thread_join() waits on the flag instead, which
	 * has a timeout everywhere (pthread_timedjoin_np() is glibc's). */
	ret = pthread_create(&thread->id, NULL, thread_entry, thread);
	if (ret != 0) {
		return -ret;
	}
	(void)pthread_detach(thread->id);
#if defined(__APPLE__)
	(void)name; /* macOS names only the calling thread */
#elif defined(__linux__)
	{
		char short_name[16]; /* the kernel keeps 15 characters */

		strncpy(short_name, name, sizeof(short_name) - 1);
		short_name[sizeof(short_name) - 1] = '\0';
		(void)pthread_setname_np(thread->id, short_name);
	}
#else
	(void)name;
#endif
	return 0;
}

int tedge_port_thread_join(tedge_port_thread_t *thread, uint32_t timeout_ms)
{
	int64_t deadline = monotonic_ms() + timeout_ms;
	bool done;

	(void)pthread_mutex_lock(&thread->lock);
	while (!thread->done && cond_wait_until(&thread->done_cond,
						&thread->lock, deadline)) {
	}
	done = thread->done;
	(void)pthread_mutex_unlock(&thread->lock);
	return done ? 0 : -EAGAIN;
}

void tedge_port_sleep_ms(uint32_t ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (long)(ms % 1000) * 1000000 };

	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

/* --- Message queues ------------------------------------------------------ */

int tedge_port_msgq_put(tedge_port_msgq_t *queue, const void *msg)
{
	int ret = -ENOMSG;

	(void)pthread_mutex_lock(&queue->lock);
	if (queue->count < queue->max) {
		size_t slot = (queue->head + queue->count) % queue->max;

		memcpy(queue->buf + slot * queue->msg_size, msg, queue->msg_size);
		queue->count++;
		ret = 0;
	}
	(void)pthread_mutex_unlock(&queue->lock);
	return ret;
}

int tedge_port_msgq_get(tedge_port_msgq_t *queue, void *msg)
{
	int ret = -ENOMSG;

	(void)pthread_mutex_lock(&queue->lock);
	if (queue->count > 0) {
		memcpy(msg, queue->buf + queue->head * queue->msg_size,
		       queue->msg_size);
		queue->head = (queue->head + 1) % queue->max;
		queue->count--;
		ret = 0;
	}
	(void)pthread_mutex_unlock(&queue->lock);
	return ret;
}

/* --- Event flags --------------------------------------------------------- */

void tedge_port_event_post(tedge_port_event_t *event, uint32_t bits)
{
	(void)pthread_mutex_lock(&event->lock);
	event->bits |= bits;
	(void)pthread_cond_broadcast(&event->cond);
	(void)pthread_mutex_unlock(&event->lock);
}

void tedge_port_event_clear(tedge_port_event_t *event, uint32_t bits)
{
	(void)pthread_mutex_lock(&event->lock);
	event->bits &= ~bits;
	(void)pthread_mutex_unlock(&event->lock);
}

uint32_t tedge_port_event_test(tedge_port_event_t *event, uint32_t bits)
{
	uint32_t set;

	(void)pthread_mutex_lock(&event->lock);
	set = event->bits & bits;
	(void)pthread_mutex_unlock(&event->lock);
	return set;
}

uint32_t tedge_port_event_wait(tedge_port_event_t *event, uint32_t bits,
			       uint32_t timeout_ms)
{
	int64_t deadline = monotonic_ms() + timeout_ms;
	uint32_t set;

	(void)pthread_mutex_lock(&event->lock);
	while ((event->bits & bits) == 0 &&
	       cond_wait_until(&event->cond, &event->lock, deadline)) {
	}
	set = event->bits & bits;
	(void)pthread_mutex_unlock(&event->lock);
	return set;
}

/* --- Semaphores ---------------------------------------------------------- */

void tedge_port_sem_init(tedge_port_sem_t *sem, unsigned int count,
			 unsigned int limit)
{
	(void)pthread_mutex_init(&sem->lock, NULL);
	(void)pthread_cond_init(&sem->cond, NULL);
	sem->count = count;
	sem->limit = limit;
}

void tedge_port_sem_give(tedge_port_sem_t *sem)
{
	(void)pthread_mutex_lock(&sem->lock);
	if (sem->count < sem->limit) {
		sem->count++;
	}
	(void)pthread_cond_signal(&sem->cond);
	(void)pthread_mutex_unlock(&sem->lock);
}

int tedge_port_sem_take(tedge_port_sem_t *sem, uint32_t timeout_ms)
{
	int64_t deadline = monotonic_ms() + timeout_ms;
	int ret = -EAGAIN;

	(void)pthread_mutex_lock(&sem->lock);
	while (sem->count == 0 &&
	       cond_wait_until(&sem->cond, &sem->lock, deadline)) {
	}
	if (sem->count > 0) {
		sem->count--;
		ret = 0;
	}
	(void)pthread_mutex_unlock(&sem->lock);
	return ret;
}

void tedge_port_sem_reset(tedge_port_sem_t *sem)
{
	(void)pthread_mutex_lock(&sem->lock);
	sem->count = 0;
	(void)pthread_mutex_unlock(&sem->lock);
}

/* --- Delayed work -------------------------------------------------------- */

/* One thread runs every item when it falls due, as Zephyr's system work
 * queue does: pending items form a list in due order, under one lock. The
 * thread starts with the first item ever scheduled. */
static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER;
static tedge_port_delayed_work_t *work_list;
static pthread_once_t work_once = PTHREAD_ONCE_INIT;

static void work_unlink(tedge_port_delayed_work_t *work)
{
	for (tedge_port_delayed_work_t **p = &work_list; *p != NULL;
	     p = &(*p)->next) {
		if (*p == work) {
			*p = work->next;
			break;
		}
	}
	work->next = NULL;
	work->pending = false;
}

static void *work_thread(void *arg)
{
	(void)arg;
	(void)pthread_mutex_lock(&work_lock);
	for (;;) {
		tedge_port_delayed_work_t *first = work_list;

		if (first == NULL) {
			(void)pthread_cond_wait(&work_cond, &work_lock);
			continue;
		}
		if (first->due_ms > monotonic_ms()) {
			(void)cond_wait_until(&work_cond, &work_lock,
					      first->due_ms);
			continue;
		}
		work_unlink(first);
		/* Unlocked while it runs, so it may schedule work itself. */
		(void)pthread_mutex_unlock(&work_lock);
		first->fn();
		(void)pthread_mutex_lock(&work_lock);
	}
	return NULL;
}

static void work_start(void)
{
	pthread_t id;

	if (pthread_create(&id, NULL, work_thread, NULL) == 0) {
		(void)pthread_detach(id);
	}
}

void tedge_port_delayed_work_schedule(tedge_port_delayed_work_t *work,
				      uint32_t delay_ms)
{
	tedge_port_delayed_work_t **p;

	(void)pthread_once(&work_once, work_start);
	(void)pthread_mutex_lock(&work_lock);
	if (work->pending) {
		work_unlink(work);
	}
	work->due_ms = monotonic_ms() + delay_ms;
	for (p = &work_list; *p != NULL && (*p)->due_ms <= work->due_ms;
	     p = &(*p)->next) {
	}
	work->next = *p;
	*p = work;
	work->pending = true;
	(void)pthread_cond_signal(&work_cond);
	(void)pthread_mutex_unlock(&work_lock);
}

void tedge_port_delayed_work_cancel(tedge_port_delayed_work_t *work)
{
	(void)pthread_mutex_lock(&work_lock);
	if (work->pending) {
		work_unlink(work);
	}
	(void)pthread_mutex_unlock(&work_lock);
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

/* --- Time and randomness ------------------------------------------------- */

/* Uptime counts from the first time anything asks, which is tedge_init()
 * or earlier: close enough to the process's start for health reports. */
static int64_t start_ms;
static pthread_once_t start_once = PTHREAD_ONCE_INIT;

static void start_mark(void)
{
	start_ms = monotonic_ms();
}

int64_t tedge_port_uptime_ms(void)
{
	(void)pthread_once(&start_once, start_mark);
	return monotonic_ms() - start_ms;
}

uint32_t tedge_port_random32(void)
{
	uint32_t value = 0;

	(void)getentropy(&value, sizeof(value));
	return value;
}

/* --- The client's heap --------------------------------------------------- */

/* malloc() with an allowance: the client may hold TEDGE_HEAP_SIZE bytes at
 * most, as on an MCU, so a build that runs here also fits its heap there
 * and the health measurements mean the same. Each block carries its size
 * in front, aligned for any type. */
typedef union {
	size_t size;
	max_align_t align;
} heap_header;

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t heap_used;

void *tedge_port_heap_alloc(size_t size)
{
	heap_header *h;

	(void)pthread_mutex_lock(&heap_lock);
	if (size > TEDGE_HEAP_SIZE - heap_used) {
		(void)pthread_mutex_unlock(&heap_lock);
		return NULL;
	}
	heap_used += size;
	(void)pthread_mutex_unlock(&heap_lock);

	h = malloc(sizeof(*h) + size);
	if (h == NULL) {
		(void)pthread_mutex_lock(&heap_lock);
		heap_used -= size;
		(void)pthread_mutex_unlock(&heap_lock);
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
	(void)pthread_mutex_lock(&heap_lock);
	heap_used -= h->size;
	(void)pthread_mutex_unlock(&heap_lock);
	free(h);
}

size_t tedge_port_heap_free_bytes(void)
{
	size_t free_bytes;

	(void)pthread_mutex_lock(&heap_lock);
	free_bytes = TEDGE_HEAP_SIZE - heap_used;
	(void)pthread_mutex_unlock(&heap_lock);
	return free_bytes;
}
