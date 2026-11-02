/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start OpenThread + Border Router when adapters.thread.enabled (or force).
 */
esp_err_t minihub_thread_init(void);

bool minihub_thread_ready(void);

esp_err_t minihub_thread_start(void);
esp_err_t minihub_thread_stop(void);

cJSON *minihub_thread_status_json(void);
const char *minihub_thread_last_error(void);

/**
 * @brief Import operational dataset TLVs (binary). len==0 → form a new network.
 * Persists hex dataset into adapters.thread when successful.
 */
esp_err_t minihub_thread_dataset_set(const uint8_t *tlvs, size_t len);

/**
 * @brief Export active dataset TLVs; *out_len set on success. Caller frees *out_tlvs.
 */
esp_err_t minihub_thread_dataset_get(uint8_t **out_tlvs, size_t *out_len);

/**
 * @brief Copy active dataset into caller buffer for Matter Thread commission.
 * @return ESP_OK and *out_len bytes written, or ESP_ERR_INVALID_SIZE if too small.
 */
esp_err_t minihub_thread_dataset_copy(uint8_t *buf, size_t buf_len, size_t *out_len);

#ifdef __cplusplus
}
#endif
