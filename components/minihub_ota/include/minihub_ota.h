/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    MINIHUB_OTA_IDLE = 0,
    MINIHUB_OTA_IN_PROGRESS = 1,
    MINIHUB_OTA_SUCCESS = 2,
    MINIHUB_OTA_FAILED = 3,
} minihub_ota_state_t;

typedef enum {
    MINIHUB_OTA_SRC_NONE = 0,
    MINIHUB_OTA_SRC_URL,
    MINIHUB_OTA_SRC_UPLOAD,
    MINIHUB_OTA_SRC_RAINMAKER,
} minihub_ota_source_t;

esp_err_t minihub_ota_init(void);

minihub_ota_state_t minihub_ota_get_state(void);
const char *minihub_ota_get_error(void);
minihub_ota_source_t minihub_ota_get_source(void);
/** Progress 0–100, or -1 if unknown. */
int minihub_ota_get_progress(void);
size_t minihub_ota_get_bytes_written(void);
bool minihub_ota_is_busy(void);
const char *minihub_ota_source_str(minihub_ota_source_t src);

/** Async URL download OTA. Returns ESP_ERR_INVALID_STATE if busy. */
esp_err_t minihub_ota_start_url(const char *url);

/**
 * Streaming upload: begin → write* → finish/abort.
 * begin returns ESP_ERR_INVALID_STATE if busy.
 */
esp_err_t minihub_ota_upload_begin(size_t expected_size);
esp_err_t minihub_ota_upload_write(const void *data, size_t len);
esp_err_t minihub_ota_upload_finish(void);
void minihub_ota_upload_abort(const char *err);

/**
 * RainMaker bridge: claim / release unified status without writing flash here.
 * try_begin returns false if local OTA holds the lock.
 */
bool minihub_ota_rainmaker_try_begin(size_t expected_size);
void minihub_ota_rainmaker_set_progress(int progress_0_100, size_t bytes_written);
void minihub_ota_rainmaker_finish(bool success, const char *err);
