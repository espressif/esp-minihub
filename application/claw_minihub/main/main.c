/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_capabilities.h"
#include "minihub_im_notify.h"
#include "minihub_local_mcp.h"
#include "app_fs.h"
#include "claw_version.h"
#include "claw_paths.h"
#include "edge_agent_version.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wifi_manager.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "cmd_module.h"
#include "cmd_wechat_qr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "app_config.h"
#include "cap_mcp_server.h"
#include "minihub_core.h"
#include "minihub_mcp_server.h"
#include "minihub_bringup.h"
#include "minihub_net.h"
#include "minihub_web.h"
#include "cap_account_bind.h"
#include "cap_llm_config.h"
#include "sdkconfig.h"
#include "esp_flash_dispatcher.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "minihub_host";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;
#if CONFIG_MINIHUB_NET_ETH
static char s_eth_ip_str[16];
#endif

static void *app_psram_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(n, sz);
}

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = app_psram_calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = app_psram_calloc(1, sizeof(*s_claw_config));
    }

    ESP_RETURN_ON_FALSE(s_config && s_claw_config, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate runtime state");

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void log_wifi_startup_config(const app_config_t *config)
{
    ESP_LOGI(TAG,
             "Wi-Fi startup STA: ssid=%s pwd_len=%u",
             config->wifi_ssid[0] ? config->wifi_ssid : "(empty)",
             (unsigned)strlen(config->wifi_password));

    ESP_LOGI(TAG,
             "Wi-Fi startup AP: ssid=%s pwd_len=%u behavior=%s",
             config->ap_ssid[0] ? config->ap_ssid : "(auto:mac-suffix)",
             (unsigned)strlen(config->ap_password),
             config->ap_behavior[0] ? config->ap_behavior : "keep");
}

#if CONFIG_MINIHUB_NET_ETH && CONFIG_MINIHUB_NET_PREFER_ETH
static void prefer_eth_default_route(void)
{
    esp_netif_t *eth = minihub_net_eth_netif();
    esp_netif_ip_info_t info = {0};

    if (!eth || esp_netif_get_ip_info(eth, &info) != ESP_OK || info.ip.addr == 0) {
        return;
    }
    esp_netif_set_default_netif(eth);
}
#endif

static bool host_has_uplink(void)
{
#if CONFIG_MINIHUB_NET_ETH
    if (minihub_net_has_ip()) {
        return true;
    }
#endif
    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    return status.sta_connected;
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

#if CONFIG_MINIHUB_NET_ETH && CONFIG_MINIHUB_NET_PREFER_ETH
    if (connected) {
        prefer_eth_default_route();
    }
#endif

    esp_err_t err = app_claw_set_network_status(host_has_uplink(), ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network UI: %s", esp_err_to_name(err));
    }
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    esp_err_t err;
    app_claw_config_t *claw_config = NULL;

    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_config_validate_wifi(config, NULL), TAG, "Invalid Wi-Fi config");

    err = app_config_save(config);
    if (err != ESP_OK) {
        return err;
    }

    claw_config = app_psram_calloc(1, sizeof(*claw_config));
    if (!claw_config) {
        ESP_LOGW(TAG, "Failed to allocate Claw config for runtime update");
        return ESP_OK;
    }
    app_config_to_claw(config, claw_config);
    err = app_claw_update_config(claw_config);
    free(claw_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to update running Claw config: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void main_copy_claw_to_app_config(const app_claw_config_t *src, app_config_t *dst)
{
    strlcpy(dst->llm_api_key, src->llm_api_key, sizeof(dst->llm_api_key));
    strlcpy(dst->llm_backend_type, src->llm_backend_type, sizeof(dst->llm_backend_type));
    strlcpy(dst->llm_model, src->llm_model, sizeof(dst->llm_model));
    strlcpy(dst->llm_base_url, src->llm_base_url, sizeof(dst->llm_base_url));
    strlcpy(dst->llm_auth_type, src->llm_auth_type, sizeof(dst->llm_auth_type));
    strlcpy(dst->llm_timeout_ms, src->llm_timeout_ms, sizeof(dst->llm_timeout_ms));
    strlcpy(dst->llm_max_tokens, src->llm_max_tokens, sizeof(dst->llm_max_tokens));
    strlcpy(dst->llm_default_image_max_bytes,
            src->llm_default_image_max_bytes,
            sizeof(dst->llm_default_image_max_bytes));
    strlcpy(dst->llm_max_tokens_field, src->llm_max_tokens_field, sizeof(dst->llm_max_tokens_field));
    strlcpy(dst->llm_supports_tools, src->llm_supports_tools, sizeof(dst->llm_supports_tools));
    strlcpy(dst->llm_supports_vision, src->llm_supports_vision, sizeof(dst->llm_supports_vision));
    strlcpy(dst->llm_image_remote_url_only,
            src->llm_image_remote_url_only,
            sizeof(dst->llm_image_remote_url_only));
    strlcpy(dst->wechat_token, src->wechat_token, sizeof(dst->wechat_token));
    strlcpy(dst->wechat_base_url, src->wechat_base_url, sizeof(dst->wechat_base_url));
    strlcpy(dst->wechat_cdn_base_url, src->wechat_cdn_base_url, sizeof(dst->wechat_cdn_base_url));
    strlcpy(dst->wechat_account_id, src->wechat_account_id, sizeof(dst->wechat_account_id));
}

static esp_err_t main_save_claw_config(const app_claw_config_t *config, void *user_ctx)
{
    esp_err_t err;
    app_config_t *app_config = NULL;

    (void)user_ctx;
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    app_config = app_psram_calloc(1, sizeof(*app_config));
    ESP_RETURN_ON_FALSE(app_config, ESP_ERR_NO_MEM, TAG, "Failed to allocate app config for Claw save");

    err = app_config_load(app_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config for Claw save: %s", esp_err_to_name(err));
        free(app_config);
        return err;
    }
    main_copy_claw_to_app_config(config, app_config);
    err = app_config_save(app_config);
    free(app_config);
    return err;
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;

#if CONFIG_MINIHUB_NET_ETH
    /* Prefer Ethernet for displayed IP and default route when both uplinks are up. */
    if (minihub_net_has_ip() && minihub_net_get_ip(s_eth_ip_str, sizeof(s_eth_ip_str))) {
        status->wifi_connected = true;
        status->ip = s_eth_ip_str;
        if (wifi_status.sta_connected) {
            status->wifi_mode = "eth";
        } else if (!status->wifi_mode || strcmp(status->wifi_mode, "off") == 0) {
            status->wifi_mode = "eth";
        }
    }
#endif
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create restart task");
    return ESP_OK;
}

const char *cap_llm_config_next_step_hint(void)
{
    return "下一步: 从 MiniHub Store 安装需要的插件，再用 Web 登录或串口 module --rpc";
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    esp_err_t ret = ESP_OK;
    cap_im_wechat_qr_login_status_t *raw = NULL;

    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    raw = app_psram_calloc(1, sizeof(*raw));
    ESP_RETURN_ON_FALSE(raw, ESP_ERR_NO_MEM, TAG, "Failed to allocate login status");

    ESP_GOTO_ON_ERROR(cap_im_wechat_qr_login_get_status(raw), cleanup, TAG,
                      "Failed to query WeChat login status");

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));

cleanup:
    free(raw);
    return ret;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}

/**
 * Strong definition for claw's weak wechat_qr_login_persist_hook:
 * save WeChat QR credentials into NVS and refresh running Claw config.
 */
esp_err_t wechat_qr_login_persist_hook(const cap_im_wechat_qr_login_status_t *status)
{
    app_config_t *app_config = NULL;
    app_claw_config_t *claw_config = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(status && status->token[0], ESP_ERR_INVALID_ARG, TAG, "missing token");

    app_config = app_psram_calloc(1, sizeof(*app_config));
    claw_config = app_psram_calloc(1, sizeof(*claw_config));
    if (!app_config || !claw_config) {
        free(app_config);
        free(claw_config);
        return ESP_ERR_NO_MEM;
    }

    err = app_config_load(app_config);
    if (err != ESP_OK) {
        goto cleanup;
    }

    strlcpy(app_config->wechat_token, status->token, sizeof(app_config->wechat_token));
    strlcpy(app_config->wechat_base_url,
            status->base_url[0] ? status->base_url : "https://ilinkai.weixin.qq.com",
            sizeof(app_config->wechat_base_url));
    if (!app_config->wechat_cdn_base_url[0]) {
        strlcpy(app_config->wechat_cdn_base_url,
                "https://novac2c.cdn.weixin.qq.com/c2c",
                sizeof(app_config->wechat_cdn_base_url));
    }
    strlcpy(app_config->wechat_account_id,
            status->account_id[0] ? status->account_id : "default",
            sizeof(app_config->wechat_account_id));

    err = app_config_save(app_config);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = app_claw_get_config(claw_config);
    if (err != ESP_OK) {
        goto cleanup;
    }
    strlcpy(claw_config->wechat_token, app_config->wechat_token, sizeof(claw_config->wechat_token));
    strlcpy(claw_config->wechat_base_url, app_config->wechat_base_url,
            sizeof(claw_config->wechat_base_url));
    strlcpy(claw_config->wechat_cdn_base_url, app_config->wechat_cdn_base_url,
            sizeof(claw_config->wechat_cdn_base_url));
    strlcpy(claw_config->wechat_account_id, app_config->wechat_account_id,
            sizeof(claw_config->wechat_account_id));
    err = app_claw_update_config(claw_config);
    if (err == ESP_OK) {
        printf("下一步微信私聊: /llm setup deepseek <token>\n");
        ESP_LOGI(TAG, "下一步微信私聊: /llm setup deepseek <token>");
    }

cleanup:
    free(app_config);
    free(claw_config);
    return err;
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(timezone && timezone[0] != '\0', ESP_ERR_INVALID_ARG, tz_default, TAG,
                      "Timezone is empty.");
    ESP_GOTO_ON_FALSE(setenv("TZ", timezone, 1) == 0, ESP_FAIL, tz_default, TAG,
                      "Failed to set TZ env");
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return ret;
}

static void *app_cjson_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}

static void app_cjson_use_psram(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = app_cjson_malloc,
        .free_fn = free,
    };
    cJSON_InitHooks(&hooks);
}

#if CONFIG_MINIHUB_NET_ETH
static bool wait_for_eth_ip(uint32_t timeout_ms)
{
    if (minihub_net_has_ip()) {
        return true;
    }
    EventGroupHandle_t eg = minihub_net_event_group();
    if (!eg) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(eg, MINIHUB_NET_GOT_IP_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & MINIHUB_NET_GOT_IP_BIT) && minihub_net_has_ip();
}
#endif

static void start_portal(void)
{
    esp_err_t err = http_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP portal start failed: %s", esp_err_to_name(err));
    }
}

static void start_wifi_and_portal(void)
{
#if CONFIG_MINIHUB_NET_ETH
    if (wait_for_eth_ip(5000)) {
        char ip[16] = {0};
        minihub_net_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "Ethernet uplink %s (Wi-Fi still starts per saved config)", ip);
    } else {
        ESP_LOGW(TAG, "No Ethernet IP after wait; continuing with Wi-Fi");
    }
#endif

    esp_err_t init_err = wifi_manager_init();
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(init_err));
        (void)app_claw_set_network_status(host_has_uplink(), NULL);
        start_portal();
        return;
    }

    esp_err_t cb_err = wifi_manager_register_state_callback(on_wifi_state_changed, NULL);
    if (cb_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi state callback register failed: %s", esp_err_to_name(cb_err));
    }
    log_wifi_startup_config(s_config);

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
        .ap_max_conn = 2,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
        (void)app_claw_set_network_status(host_has_uplink(), NULL);
        start_portal();
        return;
    }

    start_portal();

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    if (status.ap_active &&
            captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
        ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
    }

    if (s_config->wifi_ssid[0] != '\0') {
        esp_err_t wait_err = wifi_manager_wait_connected(30000);
        if (wait_err == ESP_OK) {
            wifi_manager_get_status(&status);
            ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
#if CONFIG_MINIHUB_NET_ETH && CONFIG_MINIHUB_NET_PREFER_ETH
            prefer_eth_default_route();
#endif
        } else if (wait_err == ESP_ERR_TIMEOUT) {
            wifi_manager_get_status(&status);
            ESP_LOGW(TAG,
                     "Wi-Fi STA not connected within wait window; retrying in background: mode=%s ap_active=%d ap_ip=%s",
                     status.mode ? status.mode : "off",
                     status.ap_active,
                     status.ap_ip ? status.ap_ip : "0.0.0.0");
        } else {
            ESP_LOGW(TAG, "Wi-Fi STA wait returned error: %s", esp_err_to_name(wait_err));
        }
    }

    wifi_manager_get_status(&status);
    (void)app_claw_set_network_status(host_has_uplink(),
                                      status.ap_active ? status.ap_ssid : NULL);
    if (status.ap_active) {
        const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
        ESP_LOGW(TAG,
                 "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                 status.ap_ssid,
                 portal_auth,
                 status.ap_ip,
                 status.ap_ip);
    }
}

static esp_err_t minihub_local_mcp_register_cap(const app_claw_config_t *config,
                                                const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return minihub_local_mcp_register();
}

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_LOGI(TAG, "ESP-Claw version: %s", claw_get_version());
    ESP_LOGI(TAG, "ESP-Claw git version: %s", claw_get_git_version());
    ESP_LOGI(TAG, "Edge Agent version: %s", edge_agent_get_version());

    ESP_ERROR_CHECK(app_allocate_runtime_state());

    /* main_task stack may live in PSRAM (SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY on
     * claw board). Wrapped esp_flash_* must go through the dispatcher task
     * (internal stack) before any NVS/partition access. */
    {
        const esp_flash_dispatcher_config_t flash_disp_cfg = {
            .task_stack_size = 4096,
            .task_priority = 10,
            .task_core_id = tskNO_AFFINITY,
            .queue_size = 1,
        };
        ESP_ERROR_CHECK(esp_flash_dispatcher_init(&flash_disp_cfg));
        ESP_LOGI(TAG, "esp_flash_dispatcher ready");
    }

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    app_cjson_use_psram();
    {
        esp_err_t mem_err = minihub_heap_monitor_start();
        if (mem_err != ESP_OK) {
            ESP_LOGW(TAG, "heap monitor start failed: %s", esp_err_to_name(mem_err));
        }
    }
    ESP_ERROR_CHECK(app_config_load(s_config));
    /* MiniHub dual-stack: keep stock Claw defaults, expose local MiniHub MCP
     * wrappers (and keep stock mcp_discover via cap_mcp_client). */
    {
        const app_capability_group_info_t *groups = NULL;
        size_t group_count = 0;
        char built[sizeof(s_config->llm_visible_cap_groups)];
        size_t used = 0;
        bool have_mcp_client = false;
        bool have_local_mcp = false;

        built[0] = '\0';
        if (s_config->llm_visible_cap_groups[0] == '\0' &&
            app_capabilities_get_compiled_groups(&groups, &group_count) == ESP_OK &&
            groups && group_count > 0) {
            for (size_t i = 0; i < group_count; i++) {
                if (!groups[i].group_id || !groups[i].llm_visible_by_default) {
                    continue;
                }
                size_t id_len = strlen(groups[i].group_id);
                if (used + id_len + 2 >= sizeof(built)) {
                    break;
                }
                if (used > 0) {
                    built[used++] = ',';
                }
                memcpy(built + used, groups[i].group_id, id_len);
                used += id_len;
                built[used] = '\0';
                if (strcmp(groups[i].group_id, "cap_mcp_client") == 0) {
                    have_mcp_client = true;
                }
                if (strcmp(groups[i].group_id, "cap_minihub_local_mcp") == 0) {
                    have_local_mcp = true;
                }
            }
            if (!have_mcp_client && used + sizeof("cap_mcp_client") < sizeof(built)) {
                if (used > 0) {
                    built[used++] = ',';
                    built[used] = '\0';
                }
                strlcat(built, "cap_mcp_client", sizeof(built));
                used = strlen(built);
            }
            if (!have_local_mcp && used + sizeof("cap_minihub_local_mcp") < sizeof(built)) {
                if (used > 0) {
                    built[used++] = ',';
                    built[used] = '\0';
                }
                strlcat(built, "cap_minihub_local_mcp", sizeof(built));
            }
            if (built[0]) {
                strlcpy(s_config->llm_visible_cap_groups, built,
                        sizeof(s_config->llm_visible_cap_groups));
                ESP_LOGI(TAG, "LLM-visible groups from defaults + local MCP");
            }
        } else if (s_config->llm_visible_cap_groups[0] != '\0') {
            if (!strstr(s_config->llm_visible_cap_groups, "cap_mcp_client")) {
                size_t len = strlen(s_config->llm_visible_cap_groups);
                if (len + sizeof(",cap_mcp_client") < sizeof(s_config->llm_visible_cap_groups)) {
                    strlcat(s_config->llm_visible_cap_groups, ",cap_mcp_client",
                            sizeof(s_config->llm_visible_cap_groups));
                }
            }
            if (!strstr(s_config->llm_visible_cap_groups, "cap_minihub_local_mcp")) {
                size_t len = strlen(s_config->llm_visible_cap_groups);
                if (len + sizeof(",cap_minihub_local_mcp") <
                    sizeof(s_config->llm_visible_cap_groups)) {
                    strlcat(s_config->llm_visible_cap_groups, ",cap_minihub_local_mcp",
                            sizeof(s_config->llm_visible_cap_groups));
                    ESP_LOGI(TAG, "Added cap_minihub_local_mcp to LLM-visible groups");
                } else {
                    ESP_LOGW(TAG, "llm_visible_cap_groups full; cannot append local MCP");
                }
            }
        }
    }
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(app_fs_init());

    /* Publish the resolved storage roots so any component can compose paths
     * without knowing whether data lives on flash or an SD card. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, app_fs_storage_base_path()));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, app_fs_system_base_path()));

    /* wifi_manager_init must run before minihub_net_init: stock esp-claw
     * ESP_ERROR_CHECKs esp_event_loop_create_default(), so ETH must not
     * own the default loop first. Start/AP still happens later after Claw. */
    {
        esp_err_t wifi_init_err = wifi_manager_init();
        if (wifi_init_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi manager init failed: %s", esp_err_to_name(wifi_init_err));
        }
    }

#if CONFIG_MINIHUB_NET_ETH
    /* Claw owns Wi-Fi; MiniHub starts ETH only (prefer ETH when link has IP). */
    {
        esp_err_t eth_err = minihub_net_init();
        if (eth_err != ESP_OK) {
            ESP_LOGW(TAG, "Ethernet init failed: %s (continuing with Wi-Fi)",
                     esp_err_to_name(eth_err));
        }
    }
#endif

    ESP_ERROR_CHECK(app_claw_ui_start());

    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fs_storage_base_path(),
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));

    /* Claw first: skill/agent/FILE locks need internal RAM before Wi-Fi/AP. */
    ESP_ERROR_CHECK(minihub_core_init());
    ESP_ERROR_CHECK(cap_mcp_server_init());
    ESP_ERROR_CHECK(minihub_mcp_server_init());
    ESP_ERROR_CHECK(minihub_mcp_server_register_tools());
    ESP_ERROR_CHECK(minihub_bringup_start());
    ESP_ERROR_CHECK(app_claw_set_save_config_callback(main_save_claw_config, NULL));
    /* Register before app_claw_start so cap_minihub_local_mcp is in the
     * enabled/LLM-visible maps (post-start register left the tools denied). */
    {
        static const app_capability_external_group_t s_local_mcp_ext = {
            .group_id = "cap_minihub_local_mcp",
            .display_name = "MiniHub local MCP",
            .llm_visible_by_default = true,
            .prepare = NULL,
            .reg = minihub_local_mcp_register_cap,
        };
        esp_err_t ext_err = app_capabilities_register_external_group(&s_local_mcp_ext);
        if (ext_err != ESP_OK) {
            ESP_LOGW(TAG, "register local MCP external group: %s", esp_err_to_name(ext_err));
        }
    }
    {
        esp_err_t claw_err = app_claw_start(s_claw_config);
        if (claw_err != ESP_OK) {
            ESP_LOGW(TAG, "Claw start failed: %s", esp_err_to_name(claw_err));
            minihub_heap_log();
        } else {
            esp_err_t hook_err = minihub_im_notify_register_claw_hooks();
            if (hook_err != ESP_OK) {
                ESP_LOGW(TAG, "IM notify claw hooks: %s", esp_err_to_name(hook_err));
            }
        }
    }

    start_wifi_and_portal();
    {
        void *httpd = http_server_httpd();
        if (httpd) {
            minihub_web_attach_config_t web_cfg = {
                .index_uri = "/minihub",
                .index_path = "/fatfs/web/index.html",
                .register_favicon = false,
                .start_websocket = false,
            };
            esp_err_t web_err = minihub_web_attach(httpd, &web_cfg);
            if (web_err != ESP_OK) {
                ESP_LOGW(TAG, "MiniHub web attach failed: %s", esp_err_to_name(web_err));
            }
        } else {
            ESP_LOGW(TAG, "MiniHub web skip: portal httpd not running");
        }
    }
    {
        esp_err_t mcp_err = cap_mcp_server_start();
        if (mcp_err != ESP_OK) {
            ESP_LOGW(TAG, "MCP HTTP server not started: %s",
                     esp_err_to_name(mcp_err));
        }
    }
    {
        esp_err_t bind_err = cap_account_bind_register_group();
        if (bind_err != ESP_OK) {
            /* Claw start may have failed (e.g. corrupt DATA skill); keep hub up. */
            ESP_LOGW(TAG, "account_bind register: %s", esp_err_to_name(bind_err));
        }
    }
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    {
        esp_err_t im_err = http_server_webim_bind_im();
        if (im_err != ESP_OK) {
            ESP_LOGW(TAG, "webim bind: %s", esp_err_to_name(im_err));
        }
    }
#endif

    register_wifi_command();
    register_module_command();
    register_wechat_qr_command();

    minihub_heap_log();

    app_free_runtime_state();
}
