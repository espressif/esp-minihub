/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE controller + host init for ESP BLE Mesh Provisioner.
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_BLE_MESH_SDK

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"

#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#endif

#include "esp_ble_mesh_defs.h"

static const char *TAG = "bm_bt";

#ifdef CONFIG_BT_BLUEDROID_ENABLED
static bool s_bt_ready;

void ble_mesh_host_get_dev_uuid(uint8_t *dev_uuid)
{
    if (!dev_uuid) {
        return;
    }
    memcpy(dev_uuid + 2, esp_bt_dev_get_address(), BD_ADDR_LEN);
}

esp_err_t ble_mesh_host_bluetooth_init(void)
{
    if (s_bt_ready) {
        return ESP_OK;
    }

    esp_err_t ret;

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mem_release classic: %s", esp_err_to_name(ret));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "controller already initialized");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "controller already enabled");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "bluedroid already initialized");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bluedroid_enable();
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "bluedroid already enabled");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_bt_ready = true;
    return ESP_OK;
}
#endif /* CONFIG_BT_BLUEDROID_ENABLED */

#ifdef CONFIG_BT_NIMBLE_ENABLED
static bool s_bt_ready;
static bool s_hs_synced; /* ble_hs_id_ctx may be used only after sync */
static bool s_addr_ready;
static uint8_t s_addr_val[6];
static SemaphoreHandle_t s_sync_sem;

#define BM_ADDR_RETRY_COUNT  50
#define BM_ADDR_RETRY_MS     20
#define BM_SYNC_WAIT_MS      5000

static void ble_mesh_nimble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int copy_id_addr_once(uint8_t *out)
{
    uint8_t own_type = BLE_ADDR_PUBLIC;
    int rc = ble_hs_id_infer_auto(0, &own_type);
    if (rc != 0) {
        own_type = BLE_ADDR_PUBLIC;
    }
    rc = ble_hs_id_copy_addr(own_type, out, NULL);
    if (rc != 0) {
        rc = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, out, NULL);
    }
    if (rc != 0) {
        rc = ble_hs_id_copy_addr(BLE_ADDR_RANDOM, out, NULL);
    }
    return rc;
}

static void mesh_on_sync(void)
{
    s_hs_synced = true;
    (void)ble_hs_util_ensure_addr(0);

    /* Addr may lag briefly after sync; retry instead of crashing on ENOADDR. */
    int rc = -1;
    for (int i = 0; i < 25; i++) {
        rc = copy_id_addr_once(s_addr_val);
        if (rc == 0) {
            s_addr_ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BM_ADDR_RETRY_MS));
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "sync: ble_hs_id_copy_addr failed rc=%d (will retry later)", rc);
    }

    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
}

void ble_mesh_host_get_dev_uuid(uint8_t *dev_uuid)
{
    if (!dev_uuid) {
        return;
    }

    for (int i = 0; i < BM_ADDR_RETRY_COUNT; i++) {
        if (s_addr_ready) {
            memcpy(dev_uuid + 2, s_addr_val, BD_ADDR_LEN);
            return;
        }
        /* Do not call ble_hs_id_* before sync: ble_hs_id_ctx can be NULL → Load fault. */
        if (s_hs_synced) {
            uint8_t tmp[6] = {0};
            if (copy_id_addr_once(tmp) == 0) {
                memcpy(s_addr_val, tmp, sizeof(s_addr_val));
                s_addr_ready = true;
                memcpy(dev_uuid + 2, s_addr_val, BD_ADDR_LEN);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BM_ADDR_RETRY_MS));
    }
    ESP_LOGW(TAG, "dev uuid: BLE address not ready after %d ms",
             BM_ADDR_RETRY_COUNT * BM_ADDR_RETRY_MS);
}

esp_err_t ble_mesh_host_bluetooth_init(void)
{
    if (s_bt_ready) {
        return ESP_OK;
    }

    if (!s_sync_sem) {
        s_sync_sem = xSemaphoreCreateBinary();
        if (!s_sync_sem) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = nimble_port_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "nimble already initialized");
        s_bt_ready = true;
        s_hs_synced = true; /* assume prior init already synced */
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sync_cb = mesh_on_sync;
    ble_hs_cfg.reset_cb = NULL;
    nimble_port_freertos_init(ble_mesh_nimble_host_task);

    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(BM_SYNC_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "NimBLE sync timeout (%d ms) — get_dev_uuid will keep retrying",
                 BM_SYNC_WAIT_MS);
    }

    s_bt_ready = true;
    return ESP_OK;
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */

#endif /* CONFIG_MINIHUB_BLE_MESH_SDK */
