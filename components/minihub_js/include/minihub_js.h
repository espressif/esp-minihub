/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load/reload enabled L2 JS modules under /store/modules (Host API v0). */
esp_err_t minihub_js_reload(void);

/** Tear down JS realms (before module unload / shutdown). */
void minihub_js_unload_all(void);

#if CONFIG_MINIHUB_JS_PLUGINS
/**
 * Submit a job to run on the JS worker WITHOUT blocking the caller. The worker
 * drains the queue each loop iteration. Used by the web server so a long module
 * RPC (e.g. miot_js sync) never stalls the single httpd task — the response is
 * delivered by the job itself once the worker finishes.
 */
esp_err_t minihub_js_run_on_worker_async(esp_err_t (*fn)(void *arg), void *arg);
#endif

#ifdef __cplusplus
}
#endif
