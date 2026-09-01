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

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include "sdkconfig.h"
#if CONFIG_MINIHUB_THREAD_ADAPTER
#include "minihub_thread.h"
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
#include "minihub_matter.h"
#endif

extern const minihub_adapter_t zigbee_adapter;
#if CONFIG_MINIHUB_BLE_MESH_SDK
extern const minihub_adapter_t ble_mesh_adapter;
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
extern const minihub_adapter_t matter_adapter;
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
extern const minihub_adapter_t thread_adapter;
#endif

static const char *TAG = "minihub_bringup";

static esp_err_t register_mvp_adapters(void)
{
    esp_err_t err = ESP_OK;
    /* Vendor cloud adapters load via Store DYNAMIC_MODULES — not static register.
     * hand_maintained claw has no adapters_setup.generated.c; register host
     * radio adapters here so Store 「已安装」shows them as 内置 (via register_rpc). */
#if CONFIG_MINIHUB_ZIGBEE_SDK
    err = minihub_adapter_register(&zigbee_adapter);
    if (err != ESP_OK) {
        return err;
    }
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    err = minihub_adapter_register(&ble_mesh_adapter);
    if (err != ESP_OK) {
        return err;
    }
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
    err = minihub_adapter_register(&matter_adapter);
    if (err != ESP_OK) {
        return err;
    }
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
    err = minihub_adapter_register(&thread_adapter);
    if (err != ESP_OK) {
        return err;
    }
#endif
    return err;
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

/**
 * Claw FAT config can lag the firmware image (app-flash without storage flash),
 * and legacy migrate once forced thread.enabled=false. Ensure compiled-in Matter
 * / Thread slots exist and stay enabled so Store 「内置」and BR init both work.
 */
static void ensure_host_radio_adapter_slots(cJSON *adapters_cfg)
{
    if (!adapters_cfg) {
        return;
    }
    cJSON *adapters = cJSON_GetObjectItem(adapters_cfg, "adapters");
    if (!cJSON_IsObject(adapters)) {
        adapters = cJSON_AddObjectToObject(adapters_cfg, "adapters");
        if (!adapters) {
            return;
        }
    }

    bool changed = false;

#if CONFIG_MINIHUB_MATTER_ADAPTER
    {
        cJSON *matter = cJSON_GetObjectItem(adapters, "matter");
        if (!cJSON_IsObject(matter)) {
            matter = cJSON_CreateObject();
            if (matter) {
                cJSON_AddBoolToObject(matter, "enabled", true);
                cJSON_AddItemToObject(matter, "devices", cJSON_CreateArray());
                cJSON_AddItemToObject(adapters, "matter", matter);
                changed = true;
            }
        } else {
            cJSON *en = cJSON_GetObjectItem(matter, "enabled");
            if (cJSON_IsBool(en) && !cJSON_IsTrue(en)) {
                cJSON_SetBoolValue(en, true);
                changed = true;
            } else if (!cJSON_IsBool(en)) {
                cJSON_AddBoolToObject(matter, "enabled", true);
                changed = true;
            }
        }
    }
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
    {
        cJSON *thread = cJSON_GetObjectItem(adapters, "thread");
        if (!cJSON_IsObject(thread)) {
            thread = cJSON_CreateObject();
            if (thread) {
                cJSON_AddBoolToObject(thread, "enabled", true);
                cJSON_AddNullToObject(thread, "dataset");
                cJSON_AddNullToObject(thread, "channel");
                cJSON_AddItemToObject(adapters, "thread", thread);
                changed = true;
            }
        } else {
            cJSON *en = cJSON_GetObjectItem(thread, "enabled");
            if (cJSON_IsBool(en) && !cJSON_IsTrue(en)) {
                cJSON_SetBoolValue(en, true);
                changed = true;
                ESP_LOGI(TAG, "re-enabled adapters.thread (compiled-in Thread BR)");
            } else if (!cJSON_IsBool(en)) {
                cJSON_AddBoolToObject(thread, "enabled", true);
                changed = true;
            }
            /* Drop leftover empty devices[] so migrate does not fire again. */
            cJSON *devs = cJSON_GetObjectItem(thread, "devices");
            if (cJSON_IsArray(devs) && cJSON_GetArraySize(devs) == 0) {
                cJSON_DeleteItemFromObject(thread, "devices");
                changed = true;
            }
        }
    }
#endif

    if (changed) {
        esp_err_t err = minihub_config_save_adapters(adapters_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "save adapters after host-radio ensure: %s",
                     esp_err_to_name(err));
        }
    }
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
    ensure_host_radio_adapter_slots(adapters_cfg);

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

#if CONFIG_MINIHUB_MATTER_ADAPTER
    /* Matter before Thread: CHIP EspDnssdInit needs first mdns_init() == ESP_OK. */
    {
        bool matter_enabled = true;
        const cJSON *section = adapters_cfg ? cJSON_GetObjectItem(adapters_cfg, "adapters") : NULL;
        const cJSON *matter = cJSON_GetObjectItem(section ? section : adapters_cfg, "matter");
        const cJSON *enabled = cJSON_IsObject(matter) ? cJSON_GetObjectItem(matter, "enabled") : NULL;
        if (cJSON_IsBool(enabled)) {
            matter_enabled = cJSON_IsTrue(enabled);
        }
        if (matter_enabled) {
            err = minihub_matter_init();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Matter init: %s (continuing)", esp_err_to_name(err));
            }
        }
    }
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
    err = minihub_thread_init();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Thread BR init: %s (continuing)", esp_err_to_name(err));
    }
#if CONFIG_MINIHUB_MATTER_ADAPTER
    else if (minihub_matter_ready()) {
        err = minihub_matter_attach_thread();
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Matter Thread attach: %s (continuing)", esp_err_to_name(err));
        }
    }
#endif
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

#if CONFIG_MINIHUB_MATTER_ADAPTER
esp_err_t minihub_bringup_matter_start(void)
{
    cJSON *adapters_cfg = minihub_config_get_adapters();
    bool matter_enabled = true;
    if (adapters_cfg) {
        const cJSON *section = cJSON_GetObjectItem(adapters_cfg, "adapters");
        const cJSON *matter = cJSON_GetObjectItem(section ? section : adapters_cfg, "matter");
        const cJSON *enabled = cJSON_IsObject(matter) ? cJSON_GetObjectItem(matter, "enabled") : NULL;
        if (cJSON_IsBool(enabled)) {
            matter_enabled = cJSON_IsTrue(enabled);
        }
    }
    if (!matter_enabled) {
        ESP_LOGI(TAG, "Matter skipped (adapters.json matter.enabled=false)");
        return ESP_OK;
    }

    EventGroupHandle_t net_eg = minihub_net_event_group();
    if (net_eg != NULL) {
        (void)xEventGroupWaitBits(net_eg, MINIHUB_NET_GOT_IP_BIT, pdFALSE, pdFALSE,
                                  pdMS_TO_TICKS(15000));
    }

    esp_err_t err = minihub_matter_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Matter bootstrap failed: %s", esp_err_to_name(err));
    }
    return err;
}
#endif
