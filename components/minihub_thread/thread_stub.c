/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_thread.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "minihub_thread";
static char s_last_error[128];

static void set_error(const char *msg)
{
    if (!msg) {
        s_last_error[0] = '\0';
        return;
    }
    strncpy(s_last_error, msg, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
}

esp_err_t minihub_thread_init(void)
{
    set_error("Thread BR disabled (CONFIG_MINIHUB_THREAD_ADAPTER=n)");
    ESP_LOGW(TAG, "%s", s_last_error);
    return ESP_ERR_NOT_SUPPORTED;
}

bool minihub_thread_ready(void)
{
    return false;
}

esp_err_t minihub_thread_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_thread_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

cJSON *minihub_thread_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ready", false);
    cJSON_AddBoolToObject(root, "enabled_in_firmware", false);
    cJSON_AddStringToObject(root, "role", "disabled");
    cJSON_AddStringToObject(root, "last_error", s_last_error);
    return root;
}

const char *minihub_thread_last_error(void)
{
    return s_last_error;
}

esp_err_t minihub_thread_dataset_set(const uint8_t *tlvs, size_t len)
{
    (void)tlvs;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_thread_dataset_get(uint8_t **out_tlvs, size_t *out_len)
{
    (void)out_tlvs;
    (void)out_len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_thread_dataset_copy(uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return ESP_ERR_NOT_SUPPORTED;
}
