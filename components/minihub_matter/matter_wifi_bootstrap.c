/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_matter.h"

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_MINIHUB_NET_WIFI_STA
#include "minihub_provision.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#endif

static const char *TAG = "matter_wifi";

#if CONFIG_MINIHUB_NET_WIFI_STA

esp_err_t __real_esp_wifi_init(const wifi_init_config_t *config);

esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        return __real_esp_wifi_init(config);
    }
    return ESP_OK;
}

esp_err_t minihub_matter_wifi_bootstrap(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_config_t wifi_config = {0};
    err = minihub_provision_load_credentials(&wifi_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no Wi-Fi credentials for Matter bootstrap");
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi credentials applied for Matter");
    return ESP_OK;
}

#else /* !CONFIG_MINIHUB_NET_WIFI_STA */

esp_err_t minihub_matter_wifi_bootstrap(void)
{
    /* Matter uses MiniHub Ethernet (or other non-Wi-Fi uplink). Do not create
     * WIFI_STA_DEF or call esp_wifi_* — CHIP ENABLE_WIFI_STATION must also be off. */
    ESP_LOGI(TAG, "skip Wi-Fi bootstrap (MINIHUB_NET_WIFI_STA=n; Matter on Ethernet)");
    return ESP_OK;
}

#endif /* CONFIG_MINIHUB_NET_WIFI_STA */
