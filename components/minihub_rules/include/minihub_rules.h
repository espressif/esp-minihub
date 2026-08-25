/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

esp_err_t minihub_rules_init(const cJSON *rules_cfg);
esp_err_t minihub_rules_reload(const cJSON *rules_cfg);
esp_err_t minihub_rules_stop(void);
