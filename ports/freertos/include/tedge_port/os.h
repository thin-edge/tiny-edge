/* SPDX-License-Identifier: Apache-2.0 */

/* FreeRTOS implementation of the types in <tedge/port/os.h>.
 *
 * Every kernel object is created with FreeRTOS's *Static() calls into
 * memory the object itself holds, so the client takes nothing from the
 * FreeRTOS heap. The interface wants objects that are ready at file scope,
 * which FreeRTOS cannot initialise at compile time; each object is
 * therefore created on its first use (tedge_port_os.c), under the kernel's
 * critical section.
 *
 * Only the FreeRTOS kernel API is used here and in tedge_port_os.c, so any
 * FreeRTOS platform shares them; ESP-IDF's SMP FreeRTOS included, whose
 * critical sections take a spinlock argument (TEDGE_FREERTOS_ENTER below).
 *
 * Needs configSUPPORT_STATIC_ALLOCATION and configUSE_TIMERS (both on in
 * ESP-IDF). Event groups give the client 24 flags, of the 32 the interface
 * allows; the core uses five. */

#ifndef TEDGE_PORT_FREERTOS_OS_H_
#define TEDGE_PORT_FREERTOS_OS_H_

/* ESP-IDF keeps the kernel's headers under freertos/; mainline FreeRTOS
 * puts them on the include path directly. */
#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#else
#include <FreeRTOS.h>
#include <event_groups.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>
#include <timers.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !configSUPPORT_STATIC_ALLOCATION
#error "tiny-edge needs configSUPPORT_STATIC_ALLOCATION"
#endif
#if !configUSE_TIMERS
#error "tiny-edge needs configUSE_TIMERS (delayed work)"
#endif

typedef struct {
	StaticSemaphore_t buf;
	SemaphoreHandle_t handle;
} tedge_port_mutex_t;
#define TEDGE_PORT_MUTEX_DEFINE(name) static tedge_port_mutex_t name

/* One lock for every client spinlock: ESP-IDF's portMUX_TYPE is not valid
 * as zeroed memory, which the interface allows. It is only held for a few
 * copies, so sharing it costs nothing measurable. */
typedef struct {
	char unused;
} tedge_port_spinlock_t;
typedef UBaseType_t tedge_port_spinlock_key_t;

typedef struct {
	StaticTask_t tcb;
	TaskHandle_t handle;
	/* Given when entry returns; the task then suspends itself and the
	 * port deletes it (tedge_port_os.c explains why). */
	StaticSemaphore_t done_buf;
	SemaphoreHandle_t done;
	bool finished;
	void (*entry)(void *arg);
	void *arg;
} tedge_port_thread_t;

/* ESP-IDF counts stack depth in bytes (StackType_t is uint8_t there);
 * mainline FreeRTOS in words. Sizing the array in StackType_t serves both. */
typedef StackType_t tedge_port_stack_t;
#define TEDGE_PORT_STACK_WORDS_(size)                                          \
	(((size) + sizeof(StackType_t) - 1) / sizeof(StackType_t))
#define TEDGE_PORT_STACK_DEFINE(name, size)                                    \
	static tedge_port_stack_t name[TEDGE_PORT_STACK_WORDS_(size)]
#define TEDGE_PORT_STACK_ARRAY_DEFINE(name, n, size)                           \
	static tedge_port_stack_t name[n][TEDGE_PORT_STACK_WORDS_(size)]
#define TEDGE_PORT_STACK_SIZEOF(stack) sizeof(stack)

typedef struct {
	StaticQueue_t buf;
	QueueHandle_t handle;
	uint8_t *storage;
	UBaseType_t msg_size;
	UBaseType_t len;
} tedge_port_msgq_t;
/* Not static, as on Zephyr, whose K_MSGQ_DEFINE cannot be. */
#define TEDGE_PORT_MSGQ_DEFINE(name, size_, n_)                                \
	static uint8_t name##_storage[(size_) * (n_)];                         \
	tedge_port_msgq_t name = { .storage = name##_storage,                  \
				   .msg_size = (size_),                        \
				   .len = (n_) }

typedef struct {
	StaticEventGroup_t buf;
	EventGroupHandle_t handle;
} tedge_port_event_t;
#define TEDGE_PORT_EVENT_DEFINE(name) static tedge_port_event_t name

typedef struct {
	StaticSemaphore_t buf;
	SemaphoreHandle_t handle;
} tedge_port_sem_t;

/* A one-shot software timer: the function runs on FreeRTOS's timer task,
 * whose stack (configTIMER_TASK_STACK_DEPTH) must have room for it. */
typedef struct {
	StaticTimer_t buf;
	TimerHandle_t handle;
	void (*fn)(void);
	const char *name;
} tedge_port_delayed_work_t;
#define TEDGE_PORT_DELAYED_WORK_DEFINE(id_, fn_)                               \
	static tedge_port_delayed_work_t id_ = { .fn = (fn_), .name = #id_ }

/* Read and written with the compiler's __atomic built-ins (GCC, Clang),
 * which every FreeRTOS toolchain the client targets has; on a CPU without
 * atomic instructions (ESP32-C3) the toolchain's library provides them. */
typedef intptr_t tedge_port_atomic_t;
#define TEDGE_PORT_ATOMIC_INIT(value) (value)

#endif /* TEDGE_PORT_FREERTOS_OS_H_ */
