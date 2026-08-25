/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_auth.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_auth";
static char s_token[65];

esp_err_t minihub_auth_init(void)
{
    s_token[0] = '\0';

    if (strlen(CONFIG_MINIHUB_API_TOKEN) > 0) {
        strncpy(s_token, CONFIG_MINIHUB_API_TOKEN, sizeof(s_token) - 1);
        ESP_LOGI(TAG, "API token loaded from Kconfig");
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("minihub", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return ESP_OK;
    }

    size_t len = sizeof(s_token);
    err = nvs_get_str(nvs, "api_token", s_token, &len);
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "API token loaded from NVS");
    }
    return ESP_OK;
}

bool minihub_auth_enabled(void)
{
    return s_token[0] != '\0';
}

esp_err_t minihub_auth_check(httpd_req_t *req)
{
    if (!minihub_auth_enabled()) {
        return ESP_OK;
    }

    char auth_hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    const char prefix[] = "Bearer ";
    if (strncmp(auth_hdr, prefix, sizeof(prefix) - 1) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *token = auth_hdr + sizeof(prefix) - 1;
    if (strcmp(token, s_token) != 0) {
        ESP_LOGW(TAG, "invalid API token");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t minihub_auth_check_token(const char *token)
{
    if (!minihub_auth_enabled()) {
        return ESP_OK;
    }
    if (!token || strcmp(token, s_token) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
