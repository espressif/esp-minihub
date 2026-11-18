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
    /* External-media staging could not begin because the selected medium is
     * present but not LittleFS-formatted. The web console / LCD System screen
     * must ask the operator to confirm a format before the upgrade can proceed. */
    MINIHUB_OTA_NEEDS_FORMAT = 4,
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
/**
 * @brief Progress 0–100, or -1 if unknown.
 */
int minihub_ota_get_progress(void);
size_t minihub_ota_get_bytes_written(void);
bool minihub_ota_is_busy(void);
const char *minihub_ota_source_str(minihub_ota_source_t src);

/** "sdmmc" | "usb" | "none": the external media the next (or last) firmware
 *  stage writes to. Derived from minihub_vibe_get_device(). */
const char *minihub_ota_target_str(void);

/** Bitmask (MINIHUB_VIBE_DEV_*_BIT) of the operator-selected staging media.
 *  When MINIHUB_OTA_EXTERNAL_STORAGE is off this is unused by the writer (the
 *  upgrade goes to the internal A/B slot instead). */
uint8_t minihub_ota_target_mask(void);

/** Persist the user's chosen staging media ("sdmmc" | "usb") into NVS so the
 *  next external upgrade writes to that device only. Returns ESP_ERR_INVALID_ARG
 *  for an unknown value. */
esp_err_t minihub_ota_set_target(const char *target);

/** Persist a set of staging media ("sdmmc" and/or "usb") into NVS. At least one
 *  valid entry is required; an empty or unknown value yields ESP_ERR_INVALID_ARG. */
esp_err_t minihub_ota_set_targets(const char *const *targets, size_t n);

/** When minihub_ota_get_state() is MINIHUB_OTA_NEEDS_FORMAT, the device string
 *  ("sdmmc" | "usb") of the medium that must be formatted, else "". */
const char *minihub_ota_get_format_target(void);

/** Format the named medium ("sdmmc" | "usb") to LittleFS via Vibe's media layer.
 *  Returns ESP_ERR_INVALID_ARG for an unknown name. */
esp_err_t minihub_ota_format_target(const char *target);

/** Reset a stalled/pending upgrade (e.g. after the operator cancels a format
 *  prompt) back to IDLE, aborting any in-flight staging. */
void minihub_ota_abort(void);

/**
 * @brief Async URL download. Stages the image on external LittleFS media and
 * hands off to Vibe Mode, which copies it into ota_0.
 * Returns ESP_ERR_INVALID_STATE if busy.
 */
esp_err_t minihub_ota_start_url(const char *url);

/**
 * @brief Streaming upload: begin → write* → finish/abort. Bytes are staged on
 * external media, not written to an app slot.
 * begin returns ESP_ERR_INVALID_STATE if busy.
 */
esp_err_t minihub_ota_upload_begin(size_t expected_size);
esp_err_t minihub_ota_upload_write(const void *data, size_t len);
esp_err_t minihub_ota_upload_finish(void);
void minihub_ota_upload_abort(const char *err);

/**
 * @brief RainMaker bridge: claim / release unified status without writing flash here.
 * try_begin returns false if local OTA holds the lock.
 */
bool minihub_ota_rainmaker_try_begin(size_t expected_size);
void minihub_ota_rainmaker_set_progress(int progress_0_100, size_t bytes_written);
void minihub_ota_rainmaker_finish(bool success, const char *err);
