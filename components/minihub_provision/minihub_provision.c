/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_provision.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "minihub_core.h"

#if CONFIG_MINIHUB_WIFI_PROVISIONING
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"
#endif /* CONFIG_MINIHUB_WIFI_PROVISIONING */

static const char *TAG = "minihub_provision";
static const char *PROV_NAMESPACE = "minihub_wifi";
static const char *SOFTAP_SSID = "PROV_MiniHub";
static const char *PROV_POP = "minihub1234";

static bool s_prov_active;
static bool s_prov_mgr_ready;

static bool kconfig_wifi_configured(void)
{
    return strcmp(CONFIG_MINIHUB_WIFI_SSID, "YOUR_SSID") != 0 &&
           CONFIG_MINIHUB_WIFI_SSID[0] != '\0';
}

static esp_err_t load_wifi_from_nvs(wifi_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PROV_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = sizeof(cfg->sta.ssid);
    size_t pass_len = sizeof(cfg->sta.password);
    err = nvs_get_str(nvs, "ssid", (char *)cfg->sta.ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, "pass", (char *)cfg->sta.password, &pass_len);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t save_wifi_to_nvs(const wifi_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PROV_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, "ssid", (const char *)cfg->sta.ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", (const char *)cfg->sta.password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t minihub_provision_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {0};
    size_t ssid_len = strlen(ssid);
    size_t pass_len = password ? strlen(password) : 0;
    if (ssid_len >= sizeof(cfg.sta.ssid) || pass_len >= sizeof(cfg.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    if (pass_len > 0) {
        memcpy(cfg.sta.password, password, pass_len);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    return save_wifi_to_nvs(&cfg);
}

#if CONFIG_MINIHUB_WIFI_PROVISIONING
static void publish_provision_event(const char *type)
{
    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddStringToObject(data, "transport", "softap");
        cJSON_AddStringToObject(data, "ssid", SOFTAP_SSID);
        cJSON_AddStringToObject(data, "pop", PROV_POP);
        minihub_event_bus_publish(type, data);
    } else {
        minihub_event_bus_publish(type, NULL);
    }
}

static void prov_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != NETWORK_PROV_EVENT) {
        return;
    }

    switch (event_id) {
    case NETWORK_PROV_START:
        ESP_LOGI(TAG, "SoftAP provisioning active");
        publish_provision_event("wifi.provisioning");
        break;
    case NETWORK_PROV_WIFI_CRED_RECV: {
        wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
        ESP_LOGI(TAG, "received Wi-Fi credentials for SSID: %s", (const char *)wifi_sta_cfg->ssid);
        break;
    }
    case NETWORK_PROV_WIFI_CRED_FAIL: {
        network_prov_wifi_sta_fail_reason_t *reason = (network_prov_wifi_sta_fail_reason_t *)event_data;
        ESP_LOGE(TAG, "provisioning failed: %s",
                 (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ? "auth failed" : "AP not found");
        publish_provision_event("wifi.provision_failed");
        break;
    }
    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "provisioning successful");
        break;
    case NETWORK_PROV_END: {
        wifi_config_t cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0') {
            save_wifi_to_nvs(&cfg);
        }
        s_prov_active = false;
        if (s_prov_mgr_ready) {
            network_prov_mgr_deinit();
            s_prov_mgr_ready = false;
        }
        publish_provision_event("wifi.provisioned");
        break;
    }
    default:
        break;
    }
}
#endif /* CONFIG_MINIHUB_WIFI_PROVISIONING */

esp_err_t minihub_provision_load_credentials(wifi_config_t *out_sta_cfg)
{
    if (!out_sta_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_sta_cfg, 0, sizeof(*out_sta_cfg));

    /* Web (and provisioning) write NVS. That copy wins over the compile-time
     * default so a console change is what the STA actually joins. */
    if (load_wifi_from_nvs(out_sta_cfg) == ESP_OK && out_sta_cfg->sta.ssid[0] != '\0') {
        if (out_sta_cfg->sta.password[0] == '\0') {
            out_sta_cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
        } else {
            out_sta_cfg->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        }
        ESP_LOGD(TAG, "using NVS Wi-Fi SSID: %s", out_sta_cfg->sta.ssid);
        return ESP_OK;
    }

    memset(out_sta_cfg, 0, sizeof(*out_sta_cfg));
    if (kconfig_wifi_configured()) {
        strncpy((char *)out_sta_cfg->sta.ssid, CONFIG_MINIHUB_WIFI_SSID, sizeof(out_sta_cfg->sta.ssid) - 1);
        strncpy((char *)out_sta_cfg->sta.password, CONFIG_MINIHUB_WIFI_PASS, sizeof(out_sta_cfg->sta.password) - 1);
        if (strcmp(CONFIG_MINIHUB_WIFI_PASS, "") == 0) {
            out_sta_cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
        } else {
            out_sta_cfg->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        }
        ESP_LOGD(TAG, "using Kconfig Wi-Fi SSID");
        return ESP_OK;
    }

#if CONFIG_MINIHUB_WIFI_PROVISIONING
    return ESP_ERR_NOT_FOUND;
#else
    ESP_LOGW(TAG, "no Wi-Fi credentials; set MINIHUB_WIFI_SSID or enable provisioning");
    return ESP_ERR_NOT_FOUND;
#endif
}

#if CONFIG_MINIHUB_WIFI_PROVISIONING
esp_err_t minihub_provision_start_softap(void)
{
    if (s_prov_active || s_prov_mgr_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "starting SoftAP provisioning (SSID: %s, POP: %s)", SOFTAP_SSID, PROV_POP);

    esp_err_t err = esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    err = network_prov_mgr_init(config);
    if (err != ESP_OK) {
        esp_event_handler_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_handler);
        return err;
    }
    s_prov_mgr_ready = true;
    s_prov_active = true;

    err = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, PROV_POP, SOFTAP_SSID, NULL);
    if (err != ESP_OK) {
        s_prov_active = false;
        network_prov_mgr_deinit();
        s_prov_mgr_ready = false;
        esp_event_handler_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_handler);
        return err;
    }

    return ESP_OK;
}

esp_err_t minihub_provision_start(void)
{
    return minihub_provision_start_softap();
}
#else
esp_err_t minihub_provision_start_softap(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_provision_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

bool minihub_provision_active(void)
{
    return s_prov_active;
}

bool minihub_provision_busy(void)
{
    return s_prov_active || s_prov_mgr_ready;
}

const char *minihub_provision_softap_ssid(void)
{
    return SOFTAP_SSID;
}

const char *minihub_provision_pop(void)
{
    return PROV_POP;
}
