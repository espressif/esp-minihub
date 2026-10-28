/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_matter.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "matter_stub";
static char s_last_error[96];

static void set_error(const char *msg)
{
    if (!msg) {
        s_last_error[0] = '\0';
        return;
    }
    strncpy(s_last_error, msg, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
}

esp_err_t minihub_matter_init(void)
{
    set_error("Matter adapter disabled (CONFIG_MINIHUB_MATTER_ADAPTER=n)");
    ESP_LOGW(TAG, "%s", s_last_error);
    return ESP_ERR_NOT_SUPPORTED;
}

bool minihub_matter_ready(void)
{
    return false;
}

esp_err_t minihub_matter_attach_thread(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_set_identity_cb(minihub_matter_identity_cb_t cb, void *ctx)
{
    (void)cb;
    (void)ctx;
    return ESP_OK;
}

esp_err_t minihub_matter_set_attr_cb(minihub_matter_attr_bool_cb_t cb, void *ctx)
{
    (void)cb;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_interview_identity(uint64_t node_id)
{
    (void)node_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_commission_code(uint64_t node_id, const char *pairing_code)
{
    (void)node_id;
    (void)pairing_code;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_commission_preflight(minihub_matter_discovery_t discovery,
                                              const char *wifi_ssid)
{
    (void)discovery;
    (void)wifi_ssid;
    snprintf(s_last_error, sizeof(s_last_error), "%s", "matter commissioner not built");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_commission_code_ex(uint64_t node_id, const char *pairing_code,
                                            minihub_matter_discovery_t discovery,
                                            const char *wifi_ssid, const char *wifi_password)
{
    (void)node_id;
    (void)pairing_code;
    (void)discovery;
    (void)wifi_ssid;
    (void)wifi_password;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_commission_wait(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_commission_cancel(uint64_t node_id)
{
    (void)node_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_commission_cancel_all(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_set_pending_device(uint64_t node_id, const uint16_t *endpoints,
                                            size_t endpoint_count, bool merge_existing,
                                            bool do_reload, const char *transport)
{
    (void)node_id;
    (void)endpoints;
    (void)endpoint_count;
    (void)merge_existing;
    (void)do_reload;
    (void)transport;
    return ESP_ERR_NOT_SUPPORTED;
}

/* No minihub_matter_devices_set_commission_phase() here: matter_sync.c is compiled in
 * both configurations (CMakeLists SRCS), so a stub copy is a multiple definition. */

esp_err_t minihub_matter_commission_pin(uint64_t node_id, uint32_t setup_pin)
{
    (void)node_id;
    (void)setup_pin;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_onoff(uint64_t node_id, uint16_t endpoint_id, bool on)
{
    (void)node_id;
    (void)endpoint_id;
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_read_onoff(uint64_t node_id, uint16_t endpoint_id, bool *out_on)
{
    (void)node_id;
    (void)endpoint_id;
    if (out_on) {
        *out_on = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_subscribe_node(uint64_t node_id, const uint16_t *endpoint_ids, size_t count)
{
    (void)node_id;
    (void)endpoint_ids;
    (void)count;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_matter_drop_node(uint64_t node_id)
{
    (void)node_id;
    return ESP_ERR_NOT_SUPPORTED;
}

/* Rendered by the stub status JSON too, so /api/matter/status always carries the
 * same "subscriptions" field regardless of build configuration. */
cJSON *minihub_matter_subscriptions_json(void)
{
    return cJSON_CreateArray();
}

cJSON *minihub_matter_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ready", false);
    cJSON_AddBoolToObject(root, "commissioned", false);
    cJSON_AddBoolToObject(root, "thread_br_ready", false);
    cJSON_AddStringToObject(root, "mode", "stub");
    cJSON_AddStringToObject(root, "commission", "idle");
    cJSON *prefill = cJSON_CreateObject();
    if (prefill) {
        cJSON_AddStringToObject(prefill, "ssid", "");
        cJSON_AddBoolToObject(prefill, "has_password", false);
        cJSON_AddItemToObject(root, "wifi_prefill", prefill);
    }
    cJSON *subs = minihub_matter_subscriptions_json();
    if (subs) {
        cJSON_AddItemToObject(root, "subscriptions", subs);
    }
    if (s_last_error[0]) {
        cJSON_AddStringToObject(root, "last_error", s_last_error);
    }
    return root;
}

const char *minihub_matter_last_error(void)
{
    return s_last_error[0] ? s_last_error : NULL;
}

esp_err_t minihub_matter_ota_offer(const char *point_id, const char *file_path)
{
    (void)point_id;
    (void)file_path;
    return ESP_ERR_NOT_SUPPORTED;
}
