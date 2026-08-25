/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_http_worker.h"
#include "minihub_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MINIHUB_HTTP_TASK_STACK 32768
#define MINIHUB_HTTP_QUEUE_LEN  4

typedef struct {
    minihub_http_worker_fn_t fn;
    void *ctx;
    esp_err_t *result_out;
    SemaphoreHandle_t done;
    minihub_http_worker_done_cb_t cb;
    void *user_ctx;
} minihub_http_worker_job_t;

static QueueHandle_t s_work_queue;
static SemaphoreHandle_t s_worker_mutex;
static TaskHandle_t s_worker_task;
static bool s_worker_ready;

static void minihub_http_worker_loop(void *arg)
{
    (void)arg;
    s_worker_task = xTaskGetCurrentTaskHandle();
    minihub_http_worker_job_t job;
    for (;;) {
        if (xQueueReceive(s_work_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t err = job.fn ? job.fn(job.ctx) : ESP_ERR_INVALID_ARG;
        if (job.result_out) {
            *job.result_out = err;
        }
        if (job.done) {
            xSemaphoreGive(job.done);
        }
        if (job.cb) {
            job.cb(err, job.user_ctx);
        }
    }
}

static esp_err_t minihub_http_worker_init(void)
{
    if (s_worker_ready) {
        return ESP_OK;
    }
    s_work_queue = xQueueCreate(MINIHUB_HTTP_QUEUE_LEN, sizeof(minihub_http_worker_job_t));
    s_worker_mutex = xSemaphoreCreateMutex();
    if (!s_work_queue || !s_worker_mutex) {
        if (s_work_queue) {
            vQueueDelete(s_work_queue);
            s_work_queue = NULL;
        }
        if (s_worker_mutex) {
            vSemaphoreDelete(s_worker_mutex);
            s_worker_mutex = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    if (minihub_task_create_psram(minihub_http_worker_loop, "http_worker", MINIHUB_HTTP_TASK_STACK, NULL, 5,
                                  NULL) != pdPASS) {
        vQueueDelete(s_work_queue);
        vSemaphoreDelete(s_worker_mutex);
        s_work_queue = NULL;
        s_worker_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_worker_ready = true;
    return ESP_OK;
}

esp_err_t minihub_http_run_worker(minihub_http_worker_fn_t fn, void *ctx)
{
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t init_err = minihub_http_worker_init();
    if (init_err != ESP_OK) {
        return init_err;
    }
    if (xSemaphoreTake(s_worker_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        xSemaphoreGive(s_worker_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;
    minihub_http_worker_job_t job = {
        .fn = fn,
        .ctx = ctx,
        .result_out = &result,
        .done = done,
        .cb = NULL,
        .user_ctx = NULL,
    };
    if (xQueueSend(s_work_queue, &job, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(done);
        xSemaphoreGive(s_worker_mutex);
        return ESP_FAIL;
    }
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    xSemaphoreGive(s_worker_mutex);
    return result;
}

esp_err_t minihub_http_run_worker_async(minihub_http_worker_fn_t fn, void *ctx,
                                        minihub_http_worker_done_cb_t cb, void *user_ctx)
{
    if (!fn || !cb) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t init_err = minihub_http_worker_init();
    if (init_err != ESP_OK) {
        return init_err;
    }
    minihub_http_worker_job_t job = {
        .fn = fn,
        .ctx = ctx,
        .result_out = NULL,
        .done = NULL,
        .cb = cb,
        .user_ctx = user_ctx,
    };
    if (xQueueSend(s_work_queue, &job, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool minihub_http_is_worker_task(void)
{
    return s_worker_task && xTaskGetCurrentTaskHandle() == s_worker_task;
}
