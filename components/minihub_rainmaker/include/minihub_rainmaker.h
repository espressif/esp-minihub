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
 * @brief Initialize RainMaker Node + Bridge (non-blocking).
 * Call after Wi-Fi stack init and adapter_start so initial Points exist.
 * Safe when CONFIG_MINIHUB_RAINMAKER is disabled (stub returns ESP_OK).
 */
esp_err_t minihub_rainmaker_start(void);

/**
 * @brief Optional hint after STA GOT_IP; SDK also waits for IP internally.
 */
void minihub_rainmaker_notify_got_ip(void);

bool minihub_rainmaker_ready(void);

/**
 * @brief Reconcile RainMaker devices with current Points and report node config.
 */
esp_err_t minihub_rainmaker_sync_points(void);

#ifdef __cplusplus
}
#endif
