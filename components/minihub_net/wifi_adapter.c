/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_NET_WIFI_STA

#include "minihub_adapter.h"
#include "minihub_net.h"

#include <stdbool.h>
#include "esp_log.h"

static const char *TAG = "wifi_adapter";
static bool s_started;

static esp_err_t wifi_adapter_init(const cJSON *config)
{
    (void)config;
    s_started = false;
    return ESP_OK;
}

static esp_err_t wifi_adapter_start(void)
{
    esp_err_t err = minihub_net_start_wifi();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi STA start: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "Wi-Fi adapter started");
    return ESP_OK;
}

static esp_err_t wifi_adapter_stop(void)
{
    /* Do not tear down the Wi-Fi controller on adapter stop/reload. */
    s_started = false;
    ESP_LOGI(TAG, "Wi-Fi adapter stop (controller left running)");
    return ESP_OK;
}

static esp_err_t wifi_adapter_poll(void)
{
    (void)s_started;
    return ESP_OK;
}

const minihub_adapter_t wifi_adapter = {
    .name = "wifi",
    .version = "0.1.0",
    .init = wifi_adapter_init,
    .start = wifi_adapter_start,
    .stop = wifi_adapter_stop,
    .poll = wifi_adapter_poll,
};

REGISTER_ADAPTER(wifi_adapter);

#else /* !CONFIG_MINIHUB_NET_WIFI_STA */

#endif /* CONFIG_MINIHUB_NET_WIFI_STA */
