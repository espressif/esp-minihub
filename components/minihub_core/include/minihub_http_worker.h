/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

typedef esp_err_t (*minihub_http_worker_fn_t)(void *ctx);
typedef void (*minihub_http_worker_done_cb_t)(esp_err_t result, void *user_ctx);

/** Run HTTPS work on a dedicated task (avoids httpd stack overflow). Blocks until complete. */
esp_err_t minihub_http_run_worker(minihub_http_worker_fn_t fn, void *ctx);

/** Queue HTTPS work; callback runs on the worker task when finished (does not block caller). */
esp_err_t minihub_http_run_worker_async(minihub_http_worker_fn_t fn, void *ctx,
                                        minihub_http_worker_done_cb_t cb, void *user_ctx);

/** True when called from the dedicated http_worker task (safe to run HTTPS inline). */
bool minihub_http_is_worker_task(void);
