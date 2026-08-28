/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_bringup.h"
#include "minihub_im_notify.h"

#include <string.h>

#include "esp_log.h"

#include "minihub_adapter.h"
#include "minihub_config.h"
#include "minihub_core.h"
#include "minihub_mcp_server.h"
#include "minihub_module.h"
#include "minihub_net.h"
#include "minihub_pack.h"
#include "minihub_rules.h"

#include "sdkconfig.h"

extern const minihub_adapter_t zigbee_adapter;

static const char *TAG = "minihub_bringup";

static esp_err_t register_mvp_adapters(void)
{
    /* Vendor cloud adapters load via Store DYNAMIC_MODULES — not static register. */
#if CONFIG_MINIHUB_ZIGBEE_SDK
    return minihub_adapter_register(&zigbee_adapter);
#else
    return ESP_OK;
#endif
}

static esp_err_t register_demo_point(void)
{
    minihub_point_t demo = {0};
    strncpy(demo.point_id, "switch/demo", sizeof(demo.point_id) - 1);
    strncpy(demo.type, "switch", sizeof(demo.type) - 1);
    strncpy(demo.value, "off", sizeof(demo.value) - 1);
    strncpy(demo.adapter, "mock", sizeof(demo.adapter) - 1);
    demo.online = true;
    return minihub_point_register(&demo);
}

esp_err_t minihub_bringup_start(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "MiniHub Core bring-up starting");

    err = minihub_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = minihub_adapter_registry_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adapter registry init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = register_mvp_adapters();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adapter register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = minihub_module_loader_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "module loader init: %s (continuing)", esp_err_to_name(err));
    }

    cJSON *adapters_cfg = minihub_config_get_adapters();

    err = minihub_module_load_all(adapters_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "module load: %s (continuing)", esp_err_to_name(err));
    }

    err = minihub_pack_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pack init: %s (continuing)", esp_err_to_name(err));
    } else {
        err = minihub_pack_merge_into_adapters(adapters_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "pack merge: %s (continuing)", esp_err_to_name(err));
        }
    }

    err = minihub_adapter_load_all(adapters_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adapter load failed: %s", esp_err_to_name(err));
        return err;
    }

    err = minihub_rules_init(minihub_config_get_rules());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rules init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = minihub_im_notify_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IM notify init: %s (continuing)", esp_err_to_name(err));
    }

#if CONFIG_MINIHUB_NET_WIFI_STA && !CONFIG_MINIHUB_NET_WIFI_SOFTAP
    err = minihub_net_start_wifi();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "minihub_net Wi-Fi start: %s (continuing)", esp_err_to_name(err));
    }
#else
    /* claw wifi_manager (MINIHUB_NET_WIFI_SOFTAP) or ETH-only: no minihub_net Wi-Fi here. */
#endif

    err = minihub_adapter_start_all();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adapter start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = register_demo_point();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "demo point register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = minihub_adapter_start_poll_task();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adapter poll task: %s", esp_err_to_name(err));
    }

    (void)minihub_event_bus_publish("hub.ready", NULL);
    minihub_mcp_set_hub_ready(true);

    ESP_LOGI(TAG, "MiniHub Core bring-up complete (adapters=%u)",
             (unsigned)minihub_adapter_count());
    return ESP_OK;
}
