/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allowlisted Hub REST call (in-process; same semantics as Host Web /api subset).
 *
 * @param method  GET / POST / PUT
 * @param path    Absolute path without query (e.g. /api/points/history)
 * @param query   Optional query string without leading '?' (may be NULL)
 * @param body    Optional JSON body for POST/PUT (may be NULL)
 * @param out     Owned JSON response (caller cJSON_Delete). Never NULL on ESP_OK.
 */
esp_err_t hub_api_call(const char *method, const char *path, const char *query, const char *body,
                       cJSON **out);

#ifdef __cplusplus
}
#endif
