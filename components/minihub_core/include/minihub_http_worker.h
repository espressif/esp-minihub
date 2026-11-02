/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP / TLS worker callback.
 *
 * @param ctx User context from `minihub_http_run_worker` / `_async`.
 *
 * @return Result propagated to the caller or done callback.
 */
#ifdef DOXYGEN
typedef esp_err_t minihub_http_worker_fn_t(void *ctx);
#else
typedef esp_err_t (*minihub_http_worker_fn_t)(void *ctx);
#endif

/**
 * @brief Completion callback for `minihub_http_run_worker_async`.
 *
 * @param result Worker function return value.
 * @param user_ctx User context passed to `minihub_http_run_worker_async`.
 */
#ifdef DOXYGEN
typedef void minihub_http_worker_done_cb_t(esp_err_t result, void *user_ctx);
#else
typedef void (*minihub_http_worker_done_cb_t)(esp_err_t result, void *user_ctx);
#endif

/**
 * @brief Run HTTPS / heavy TLS work on a dedicated task.
 *
 * Avoids httpd task stack overflow. Blocks until `fn` returns.
 *
 * @param fn  Worker function.
 * @param ctx Passed to `fn`.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - other: from `fn` or scheduling failure
 */
esp_err_t minihub_http_run_worker(minihub_http_worker_fn_t fn, void *ctx);

/**
 * @brief Queue HTTPS work without blocking the caller.
 *
 * `cb` runs on the worker task when finished.
 *
 * @param fn       Worker function.
 * @param ctx      Passed to `fn`.
 * @param cb       Optional completion callback (may be NULL).
 * @param user_ctx Passed to `cb`.
 *
 * @return
 *     - ESP_OK: Queued.
 *     - ESP_ERR_NO_MEM: Queue full / OOM.
 */
esp_err_t minihub_http_run_worker_async(minihub_http_worker_fn_t fn, void *ctx,
                                        minihub_http_worker_done_cb_t cb, void *user_ctx);

/**
 * @brief True if called from the dedicated http_worker task; false otherwise.
 *
 * Safe to run HTTPS inline when this returns true.
 */
bool minihub_http_is_worker_task(void);

#ifdef __cplusplus
}
#endif
