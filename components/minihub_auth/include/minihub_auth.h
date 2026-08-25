/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t minihub_auth_init(void);
bool minihub_auth_enabled(void);
esp_err_t minihub_auth_check(httpd_req_t *req);
esp_err_t minihub_auth_check_token(const char *token);
