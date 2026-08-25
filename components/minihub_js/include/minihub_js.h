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

#ifdef __cplusplus
}
#endif
