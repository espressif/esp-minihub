/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_rainmaker.h"
#include "rainmaker_bridge.h"
#include "minihub_ota.h"

#include <stdio.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rmaker_core.h"
#include "esp_rmaker_common_events.h"
#include "esp_rmaker_ota.h"

#if CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE
#include "esp_rmaker_on_network_chal_resp.h"
#endif

static const char *TAG = "rmaker_node";

/* MiniHub HTTP UI owns port 80; RainMaker on-network uses a free port. */
#define MINIHUB_RMAKER_ON_NETWORK_PORT 8081

/*
 * Override rmaker_common weak stub: default uses esp_wifi_get_mac(STA), which
 * fails when MiniHub runs Ethernet-only (Wi-Fi never started).
 */
char *platform_get_product_UID(void)
{
    static char mac_str[13];
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "product UID: esp_read_mac failed: %s", esp_err_to_name(err));
        return NULL;
    }
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return mac_str;
}

static esp_rmaker_node_t *s_node;
static bool s_started;
static bool s_ready;
static bool s_on_network_started;
static bool s_got_ip;
static bool s_claimed_ready; /* claim done this boot, or already claimed */

static void log_node_id_banner(const char *why)
{
    const char *node_id = esp_rmaker_get_node_id();
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " RainMaker Node ID: %s", node_id ? node_id : "(null)");
    ESP_LOGI(TAG, " (%s)", why ? why : "");
    ESP_LOGI(TAG, "================================================");
}

#if CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE
static void try_start_on_network_mapping(const char *trigger)
{
    if (s_on_network_started) {
        ESP_LOGD(TAG, "on-network skip (%s): already started", trigger ? trigger : "");
        return;
    }
    if (!s_got_ip) {
        ESP_LOGI(TAG, "on-network wait (%s): no IP yet", trigger ? trigger : "");
        return;
    }
    if (!s_claimed_ready) {
        ESP_LOGI(TAG, "on-network wait (%s): wait claim/MQTT", trigger ? trigger : "");
        return;
    }
    if (esp_rmaker_on_network_chal_resp_is_running()) {
        s_on_network_started = true;
        ESP_LOGI(TAG, "on-network already running (%s)", trigger ? trigger : "");
        return;
    }

    ESP_LOGI(TAG, "starting on-network mapping (%s)...", trigger ? trigger : "");

    esp_rmaker_on_network_chal_resp_config_t cfg = ESP_RMAKER_ON_NETWORK_CHAL_RESP_DEFAULT_CONFIG();
    cfg.port = MINIHUB_RMAKER_ON_NETWORK_PORT;
    cfg.sec_ver = ESP_RMAKER_ON_NETWORK_SEC1;
    cfg.pop = NULL; /* no PoP — APP On-Network can proceed without PIN */
    cfg.enable_mdns = true;
    cfg.mdns_instance_name = NULL; /* defaults to node_id */

    esp_err_t err = esp_rmaker_on_network_chal_resp_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "on-network chal_resp start failed: %s", esp_err_to_name(err));
        return;
    }
    s_on_network_started = true;

    const char *node_id = esp_rmaker_get_node_id();
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " On-Network user mapping READY");
    ESP_LOGI(TAG, " Node ID : %s", node_id ? node_id : "(null)");
    ESP_LOGI(TAG, " mDNS    : _esp_rmaker_chal_resp._tcp");
    ESP_LOGI(TAG, " Port    : %d (not 80 — MiniHub Web uses 80)", MINIHUB_RMAKER_ON_NETWORK_PORT);
    ESP_LOGI(TAG, " PoP     : (none)");
    ESP_LOGI(TAG, " APP     : Add Device → On Network (same Wi-Fi)");
    ESP_LOGI(TAG, " trigger : %s", trigger ? trigger : "");
    ESP_LOGI(TAG, "================================================");
}

static void stop_on_network_mapping(void)
{
    if (!s_on_network_started && !esp_rmaker_on_network_chal_resp_is_running()) {
        return;
    }
    esp_err_t err = esp_rmaker_on_network_chal_resp_stop();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "on-network stop: %s", esp_err_to_name(err));
    }
    s_on_network_started = false;
    ESP_LOGI(TAG, "On-Network mapping stopped");
}
#endif /* CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE */

static void rmaker_ota_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    switch (event_id) {
    case RMAKER_OTA_EVENT_STARTING:
    case RMAKER_OTA_EVENT_IN_PROGRESS:
        if (!minihub_ota_is_busy() || minihub_ota_get_source() == MINIHUB_OTA_SRC_RAINMAKER) {
            if (minihub_ota_get_source() != MINIHUB_OTA_SRC_RAINMAKER) {
                (void)minihub_ota_rainmaker_try_begin(0);
            }
            minihub_ota_rainmaker_set_progress(-1, minihub_ota_get_bytes_written());
        }
        break;
    case RMAKER_OTA_EVENT_SUCCESSFUL:
    case RMAKER_OTA_EVENT_REQ_FOR_REBOOT:
        minihub_ota_rainmaker_finish(true, NULL);
        break;
    case RMAKER_OTA_EVENT_FAILED:
        minihub_ota_rainmaker_finish(false, "rainmaker OTA failed");
        break;
    case RMAKER_OTA_EVENT_REJECTED:
        minihub_ota_rainmaker_finish(false, "rainmaker OTA rejected");
        break;
    case RMAKER_OTA_EVENT_DELAYED:
        ESP_LOGI(TAG, "RainMaker OTA delayed");
        break;
    default:
        break;
    }
}

static esp_err_t minihub_rmaker_ota_cb(esp_rmaker_ota_handle_t handle, esp_rmaker_ota_data_t *ota_data)
{
    size_t expected = 0;
    if (ota_data && ota_data->filesize > 0) {
        expected = (size_t)ota_data->filesize;
    }
    if (minihub_ota_is_busy() && minihub_ota_get_source() != MINIHUB_OTA_SRC_RAINMAKER) {
        ESP_LOGW(TAG, "reject RainMaker OTA: local OTA busy");
        (void)esp_rmaker_ota_report_status(handle, OTA_STATUS_REJECTED, "local OTA in progress");
        return ESP_FAIL;
    }
    if (!minihub_ota_rainmaker_try_begin(expected)) {
        if (minihub_ota_get_source() != MINIHUB_OTA_SRC_RAINMAKER) {
            (void)esp_rmaker_ota_report_status(handle, OTA_STATUS_REJECTED, "OTA busy");
            return ESP_FAIL;
        }
    }
    return esp_rmaker_ota_default_cb(handle, ota_data);
}

static void rmaker_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == RMAKER_COMMON_EVENT) {
        if (event_id == RMAKER_MQTT_EVENT_CONNECTED) {
            ESP_LOGI(TAG, "MQTT connected");
            s_ready = true;
            /* Already claimed on a previous boot: no CLAIM_SUCCESSFUL this run. */
            s_claimed_ready = true;
            log_node_id_banner("MQTT connected");
#if CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE
            try_start_on_network_mapping("mqtt_connected");
#endif
        } else if (event_id == RMAKER_MQTT_EVENT_DISCONNECTED) {
            ESP_LOGW(TAG, "MQTT disconnected");
            s_ready = false;
        }
        return;
    }
    if (event_base != RMAKER_EVENT) {
        return;
    }
    if (event_id == RMAKER_EVENT_CLAIM_SUCCESSFUL) {
        ESP_LOGI(TAG, "claim successful");
        s_claimed_ready = true;
        log_node_id_banner("claim successful");
#if CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE
        try_start_on_network_mapping("claim_successful");
#endif
    } else if (event_id == RMAKER_EVENT_CLAIM_FAILED) {
        ESP_LOGW(TAG, "claim failed");
    } else if (event_id == RMAKER_EVENT_STARTED) {
        /* Fires after MQTT + (existing) user mapping; good second chance to start. */
        s_claimed_ready = true;
#if CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE
        try_start_on_network_mapping("rmaker_started");
#endif
    } else if (event_id == RMAKER_EVENT_USER_NODE_MAPPING_DONE) {
        /* Only stop if this was a fresh mapping done via on-network (service was up). */
        ESP_LOGI(TAG, "user-node mapping done");
#if CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE
        if (s_on_network_started) {
            stop_on_network_mapping();
        }
#endif
    }
}

esp_err_t minihub_rainmaker_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_event_handler_register(RMAKER_COMMON_EVENT, ESP_EVENT_ANY_ID, &rmaker_event_handler, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "common event register failed: %s", esp_err_to_name(err));
    }
    err = esp_event_handler_register(RMAKER_EVENT, ESP_EVENT_ANY_ID, &rmaker_event_handler, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "rmaker event register failed: %s", esp_err_to_name(err));
    }

    esp_rmaker_config_t cfg = {
        .enable_time_sync = false,
    };
    s_node = esp_rmaker_node_init(&cfg, "MiniHub", "Gateway");
    if (!s_node) {
        ESP_LOGE(TAG, "esp_rmaker_node_init failed");
        return ESP_FAIL;
    }

    log_node_id_banner("node_init");

    err = rainmaker_bridge_init(s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge init failed: %s", esp_err_to_name(err));
        return err;
    }

    {
        esp_err_t ota_err = esp_event_handler_register(RMAKER_OTA_EVENT, ESP_EVENT_ANY_ID,
                                                       &rmaker_ota_event_handler, NULL);
        if (ota_err != ESP_OK && ota_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "OTA event register: %s", esp_err_to_name(ota_err));
        }
        esp_rmaker_ota_config_t ota_config = {
            .ota_cb = minihub_rmaker_ota_cb,
            .ota_diag = NULL,
            .server_cert = ESP_RMAKER_OTA_DEFAULT_SERVER_CERT,
            .priv = NULL,
        };
        ota_err = esp_rmaker_ota_enable(&ota_config, OTA_USING_PARAMS);
        if (ota_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_rmaker_ota_enable: %s", esp_err_to_name(ota_err));
        } else {
            ESP_LOGI(TAG, "RainMaker OTA enabled (params)");
        }
    }

    err = esp_rmaker_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_rmaker_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "RainMaker started (self-claim when IP available)");
    return ESP_OK;
}

void minihub_rainmaker_notify_got_ip(void)
{
    if (!s_started) {
        return;
    }
    s_got_ip = true;
    ESP_LOGI(TAG, "GOT_IP — claim/MQTT proceeds in RainMaker agent");
#if CONFIG_ESP_RMAKER_ON_NETWORK_CHAL_RESP_ENABLE
    try_start_on_network_mapping("got_ip");
#endif
}

bool minihub_rainmaker_ready(void)
{
    return s_ready;
}

esp_err_t minihub_rainmaker_sync_points(void)
{
    if (!s_node) {
        return ESP_ERR_INVALID_STATE;
    }
    return rainmaker_bridge_sync_all();
}
