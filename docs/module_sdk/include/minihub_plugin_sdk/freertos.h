/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file freertos.h
 * @brief Plugin SDK — FreeRTOS helpers and libgcc soft-float exports.
 *
 * Host tick rate is 100 Hz (``configTICK_RATE_HZ``). Prefer ``xSemaphore*``
 * macros over calling queue primitives directly.
 * Each API carries an ``@note`` with its IDF header origin.
 */

/** @brief Opaque task handle.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
typedef void *TaskHandle_t;
/** @brief Opaque queue handle.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
typedef void *QueueHandle_t;
/** @brief Semaphore handle (queue-based).
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
typedef QueueHandle_t SemaphoreHandle_t;
/** @brief FreeRTOS tick count type.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
typedef uint32_t TickType_t;
/** @brief Signed base type for API return values.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
typedef int32_t BaseType_t;
/** @brief Unsigned base type for priorities and counts.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
typedef uint32_t UBaseType_t;

#ifdef DOXYGEN
/** @brief Task entry function type.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
typedef void TaskFunction_t(void *pvParameters);
#else
typedef void (*TaskFunction_t)(void *);
#endif

/** @brief Host tick rate (Hz).
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
#define configTICK_RATE_HZ     100
/** @brief Milliseconds per tick.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
#define portTICK_PERIOD_MS     (1000 / configTICK_RATE_HZ)
/** @brief Convert milliseconds to ticks.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
#define pdMS_TO_TICKS(x)       ((TickType_t)(((uint64_t)(x) * configTICK_RATE_HZ) / 1000u))
/** @brief Maximum block time (infinite wait).
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
#define portMAX_DELAY          ((TickType_t)0xffffffffu)
/** @brief Boolean false for FreeRTOS APIs.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
#define pdFALSE                ((BaseType_t)0)
/** @brief Boolean true for FreeRTOS APIs.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
#define pdTRUE                 ((BaseType_t)1)
/** @brief Successful queue/semaphore operation.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
#define pdPASS                 pdTRUE
/** @brief No CPU affinity (any core).
 *  @note IDF: ``components/freertos/esp_additions/include/freertos/idf_additions.h`` */
#define tskNO_AFFINITY         ((BaseType_t)0x7fffffff)
/** @brief Send item to back of queue.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
#define queueSEND_TO_BACK      ((BaseType_t)0)
/** @brief Queue type for mutex creation.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
#define queueQUEUE_TYPE_MUTEX  ((uint8_t)1)
/** @brief Non-blocking semaphore give.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
#define semGIVE_BLOCK_TIME     ((TickType_t)0)

/** @brief Block the calling task for a number of ticks.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
void vTaskDelay(const TickType_t xTicksToWait);
/** @brief Delete a task.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/task.h`` */
void vTaskDelete(TaskHandle_t xTaskToDelete);
/** @brief Create a task pinned to a specific core.
 *  @note IDF: ``components/freertos/esp_additions/include/freertos/idf_additions.h`` */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char *const pcName,
                                   const uint32_t ulStackDepth, void *const pvParameters,
                                   UBaseType_t uxPriority, TaskHandle_t *const pxCreatedTask,
                                   const BaseType_t xCoreID);
/** @brief Create a mutex queue.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
QueueHandle_t xQueueCreateMutex(const uint8_t ucQueueType);
/** @brief Send to queue (generic).
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *const pvItemToQueue,
                             TickType_t xTicksToWait, const BaseType_t xCopyPosition);
/** @brief Take a semaphore from a queue.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
BaseType_t xQueueSemaphoreTake(QueueHandle_t xQueue, TickType_t xTicksToWait);

/** @brief Create a mutex semaphore.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
#define xSemaphoreCreateMutex()  xQueueCreateMutex(queueQUEUE_TYPE_MUTEX)
/** @brief Take a semaphore.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
#define xSemaphoreTake(s, t)     xQueueSemaphoreTake((s), (t))
/** @brief Give a semaphore.
 *  @note IDF: ``components/freertos/FreeRTOS-Kernel/include/freertos/queue.h`` */
#define xSemaphoreGive(s)        xQueueGenericSend((QueueHandle_t)(s), NULL, semGIVE_BLOCK_TIME, \
                                                   queueSEND_TO_BACK)

/** @name libgcc helpers
 * Soft-float / integer helpers pulled by some plugins (no public IDF header).
 * Each API carries an ``@note`` with its origin.
 * @{
 */
/** @brief Signed 64-bit division.
 *  @note Origin: libgcc (no public IDF header) */
long long __divdi3(long long, long long);
/** @brief Compare double equality.
 *  @note Origin: libgcc (no public IDF header) */
int __eqdf2(double, double);
/** @brief Promote float to double.
 *  @note Origin: libgcc (no public IDF header) */
double __extendsfdf2(float);
/** @brief Convert double to signed 64-bit integer.
 *  @note Origin: libgcc (no public IDF header) */
long long __fixdfdi(double);
/** @brief Convert double to signed 32-bit integer.
 *  @note Origin: libgcc (no public IDF header) */
int __fixdfsi(double);
/** @brief Convert signed 64-bit integer to double.
 *  @note Origin: libgcc (no public IDF header) */
double __floatdidf(long long);
/** @brief Convert signed 32-bit integer to double.
 *  @note Origin: libgcc (no public IDF header) */
double __floatsidf(int);
/** @brief Convert unsigned 32-bit integer to double.
 *  @note Origin: libgcc (no public IDF header) */
double __floatunsidf(unsigned int);
/** @brief Compare double greater-than.
 *  @note Origin: libgcc (no public IDF header) */
int __gtdf2(double, double);
/** @brief Compare double less-than.
 *  @note Origin: libgcc (no public IDF header) */
int __ltdf2(double, double);
/** @brief Compare double not-equal.
 *  @note Origin: libgcc (no public IDF header) */
int __nedf2(double, double);
/** @} */

#ifdef __cplusplus
}
#endif
