/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_im_notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_router.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "minihub_core.h"
#include "minihub_task.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_MINIHUB_ZIGBEE_SDK
#include "minihub_zigbee.h"
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
#include "minihub_ble_mesh.h"
#endif

static const char *TAG = "im_notify";

#define NVS_NS "mh_im"
#define MIN_INTERNAL_HEAP 12288
#define POINT_COOLDOWN_MS 8000
#define GLOBAL_COOLDOWN_MS 2500
#define QUEUE_LEN 6
#define MSG_MAX 160
/* wechat_send_message does HTTPS + cJSON on this task; 4K overflows (see boot panic). */
#define NOTIFY_TASK_STACK 12288

typedef struct {
    char text[MSG_MAX];
} notify_job_t;

static QueueHandle_t s_q;
static char s_channel[16];
static char s_chat_id[96];
static int64_t s_last_global_ms;
static int64_t s_last_point_ms[MINIHUB_MAX_POINTS];

static void persist_chat(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(h, "channel", s_channel);
    (void)nvs_set_str(h, "chat_id", s_chat_id);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void load_chat(void)
{
    nvs_handle_t h;
    size_t n;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    n = sizeof(s_channel);
    (void)nvs_get_str(h, "channel", s_channel, &n);
    n = sizeof(s_chat_id);
    (void)nvs_get_str(h, "chat_id", s_chat_id, &n);
    nvs_close(h);
}

void minihub_im_notify_remember(const char *channel, const char *chat_id)
{
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) {
        return;
    }
    if (strcmp(channel, "wechat") != 0) {
        return;
    }
    if (strcmp(s_chat_id, chat_id) == 0 && strcmp(s_channel, channel) == 0) {
        return;
    }
    strlcpy(s_channel, channel, sizeof(s_channel));
    strlcpy(s_chat_id, chat_id, sizeof(s_chat_id));
    persist_chat();
    ESP_LOGI(TAG, "remember chat channel=%s", channel);
}

static bool type_is_sensor(const char *type)
{
    return type && (strcmp(type, "binary_sensor") == 0 ||
                    strcmp(type, "button") == 0);
}

static bool value_is_discrete_event(const char *v)
{
    static const char *const vals[] = {
        "open", "closed", "pressed", "released", "detected", "clear",
        "occupied", "vacant", "on", "off",
    };
    if (!v || !v[0] || strcmp(v, "unknown") == 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        if (strcmp(v, vals[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool should_auto_notify(const minihub_point_t *point, const char *new_value)
{
    if (!point || !new_value) {
        return false;
    }
    if (strcmp(point->point_id, "switch/demo") == 0) {
        return false;
    }
    if (type_is_sensor(point->type)) {
        return value_is_discrete_event(new_value);
    }
    /* Motion 2S is currently typed switch; still notify occupancy-like values. */
    if (strcmp(new_value, "pressed") == 0 || strcmp(new_value, "released") == 0 ||
        strcmp(new_value, "open") == 0 || strcmp(new_value, "closed") == 0 ||
        strcmp(new_value, "detected") == 0 || strcmp(new_value, "occupied") == 0) {
        return true;
    }
    return false;
}

static bool cooldown_ok(uint16_t index)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (s_last_global_ms != 0 && (now - s_last_global_ms) < GLOBAL_COOLDOWN_MS) {
        return false;
    }
    if (index < MINIHUB_MAX_POINTS && s_last_point_ms[index] != 0 &&
        (now - s_last_point_ms[index]) < POINT_COOLDOWN_MS) {
        return false;
    }
    s_last_global_ms = now;
    if (index < MINIHUB_MAX_POINTS) {
        s_last_point_ms[index] = now;
    }
    return true;
}

esp_err_t minihub_im_notify_text(const char *text)
{
    notify_job_t job = {0};
    if (!text || !text[0] || !s_q) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(job.text, text, sizeof(job.text));
    if (xQueueSend(s_q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "notify queue full, drop");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t send_wechat(const char *text)
{
    char output[96];
    char *payload;
    cJSON *root;
    claw_cap_call_context_t ctx = {
        .channel = s_channel[0] ? s_channel : "wechat",
        .chat_id = s_chat_id,
        .target_channel = s_channel[0] ? s_channel : "wechat",
        .target_chat_id = s_chat_id,
        .source_cap = "minihub_im_notify",
        .caller = CLAW_CAP_CALLER_SYSTEM,
    };

    if (!s_chat_id[0]) {
        ESP_LOGW(TAG, "no WeChat chat yet; send any DM once to bind notify target");
        return ESP_ERR_INVALID_STATE;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < MIN_INTERNAL_HEAP) {
        ESP_LOGW(TAG, "skip notify (internal heap low)");
        return ESP_ERR_NO_MEM;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "chat_id", s_chat_id);
    cJSON_AddStringToObject(root, "message", text);
    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    output[0] = '\0';
    esp_err_t err = claw_cap_call("wechat_send_message", payload, &ctx, output, sizeof(output));
    free(payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wechat_send_message %s out=%s", esp_err_to_name(err), output);
    }
    return err;
}

static void notify_task(void *arg)
{
    notify_job_t job;
    (void)arg;
    for (;;) {
        if (xQueueReceive(s_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        (void)send_wechat(job.text);
    }
}

static void on_point_registered(const minihub_event_t *event, void *user_data)
{
    const cJSON *adapter_j;
    const cJSON *pid_j;
    const cJSON *type_j;
    char msg[MSG_MAX];
    uint16_t index = UINT16_MAX;
    (void)user_data;

    if (!event || !event->data) {
        return;
    }
    adapter_j = cJSON_GetObjectItem(event->data, "adapter");
    if (!cJSON_IsString(adapter_j) || !adapter_j->valuestring[0]) {
        return;
    }
    if (strcmp(adapter_j->valuestring, "zigbee") == 0) {
#if CONFIG_MINIHUB_ZIGBEE_SDK
        if (!minihub_zigbee_permit_join_active()) {
            return;
        }
#endif
    } else if (strcmp(adapter_j->valuestring, "ble_mesh") == 0) {
#if CONFIG_MINIHUB_BLE_MESH_SDK
        if (!minihub_ble_mesh_permit_provision_active()) {
            return;
        }
#else
        return;
#endif
    } else {
        return;
    }
    pid_j = cJSON_GetObjectItem(event->data, "point_id");
    type_j = cJSON_GetObjectItem(event->data, "type");
    if (!cJSON_IsString(pid_j) || !pid_j->valuestring[0]) {
        return;
    }
    index = minihub_point_index_of(pid_j->valuestring);
    if (!cooldown_ok(index)) {
        return;
    }
    if (strcmp(adapter_j->valuestring, "ble_mesh") == 0) {
        snprintf(msg, sizeof(msg), "BLE Mesh 新设备入网: %s (%s)",
                 pid_j->valuestring,
                 (cJSON_IsString(type_j) && type_j->valuestring[0]) ? type_j->valuestring : "device");
    } else {
        snprintf(msg, sizeof(msg), "Zigbee 新设备入网: %s (%s)",
                 pid_j->valuestring,
                 (cJSON_IsString(type_j) && type_j->valuestring[0]) ? type_j->valuestring : "device");
    }
    (void)minihub_im_notify_text(msg);
}

static void on_radio_permit_closed(const minihub_event_t *event, void *user_data)
{
    const cJSON *adapter_j;
    char msg[MSG_MAX];
    (void)user_data;
    if (!event || !event->data) {
        return;
    }
    adapter_j = cJSON_GetObjectItem(event->data, "adapter");
    if (!cJSON_IsString(adapter_j) || !adapter_j->valuestring[0]) {
        return;
    }
    if (strcmp(adapter_j->valuestring, "ble_mesh") == 0) {
        snprintf(msg, sizeof(msg), "BLE Mesh 配网窗口已结束");
    } else if (strcmp(adapter_j->valuestring, "zigbee") == 0) {
        snprintf(msg, sizeof(msg), "Zigbee 入网窗口已结束");
    } else {
        return;
    }
    (void)minihub_im_notify_text(msg);
}

static void on_point_updated(const minihub_event_t *event, void *user_data)
{
    minihub_point_t *point;
    char msg[MSG_MAX];
    (void)user_data;
    if (!event) {
        return;
    }
    point = minihub_point_get_by_index(event->point_index);
    if (!point) {
        return;
    }
    if (!should_auto_notify(point, event->new_value)) {
        return;
    }
    if (!cooldown_ok(event->point_index)) {
        return;
    }
    snprintf(msg, sizeof(msg), "Hub %s: %s → %s",
             point->type, point->point_id, event->new_value);
    (void)minihub_im_notify_text(msg);
}

esp_err_t minihub_im_notify_init(void)
{
    load_chat();
    s_q = xQueueCreate(QUEUE_LEN, sizeof(notify_job_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    if (minihub_task_create_psram(notify_task, "im_notify", NOTIFY_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = minihub_event_bus_subscribe("point.updated", on_point_updated, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_event_bus_subscribe("point.registered", on_point_registered, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_event_bus_subscribe("radio.permit_closed", on_radio_permit_closed, NULL);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "IM notify ready chat=%s", s_chat_id[0] ? "saved" : "(none)");
    return ESP_OK;
}

static esp_err_t remember_chat_execute(const char *input_json,
                                       const claw_cap_call_context_t *ctx,
                                       char *output,
                                       size_t output_size)
{
    const char *channel = NULL;
    const char *chat_id = NULL;
    cJSON *root = NULL;

    if (output && output_size) {
        output[0] = '\0';
    }
    if (ctx) {
        if (ctx->channel && ctx->channel[0]) {
            channel = ctx->channel;
        }
        if (ctx->chat_id && ctx->chat_id[0]) {
            chat_id = ctx->chat_id;
        }
    }
    if (input_json && input_json[0]) {
        root = cJSON_Parse(input_json);
        if (root) {
            const cJSON *ch = cJSON_GetObjectItem(root, "channel");
            const cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
            if (cJSON_IsString(ch) && ch->valuestring && ch->valuestring[0]) {
                channel = ch->valuestring;
            }
            if (cJSON_IsString(cid) && cid->valuestring && cid->valuestring[0]) {
                chat_id = cid->valuestring;
            }
        }
    }
    minihub_im_notify_remember(channel, chat_id);
    cJSON_Delete(root);
    if (output && output_size) {
        snprintf(output, output_size, "{\"ok\":true}");
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_remember_caps[] = {
    {
        .id = "minihub_remember_chat",
        .name = "minihub_remember_chat",
        .family = "app",
        .description = "Remember last WeChat chat_id for MiniHub sensor notify.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\"},"
            "\"chat_id\":{\"type\":\"string\"}}}",
        .execute = remember_chat_execute,
    },
};

static const claw_cap_group_t s_remember_group = {
    .group_id = "cap_minihub_im_notify",
    .plugin_name = "cap_minihub_im_notify",
    .version = "1.0.0",
    .descriptors = s_remember_caps,
    .descriptor_count = sizeof(s_remember_caps) / sizeof(s_remember_caps[0]),
};

esp_err_t minihub_im_notify_register_claw_hooks(void)
{
    esp_err_t err;

    if (!claw_cap_group_exists(s_remember_group.group_id)) {
        err = claw_cap_register_group(&s_remember_group);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "register remember cap: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* Non-consuming rule so chat_id is bound before agent consumes the message.
     * Does not require patching cap_im_wechat. */
    static const char *rule_json =
        "{"
        "\"id\":\"minihub_remember_im_chat\","
        "\"enabled\":true,"
        "\"consume_on_match\":false,"
        "\"match\":{"
        "\"event_type\":\"message\","
        "\"event_key\":\"text\","
        "\"content_type\":\"text\""
        "},"
        "\"actions\":[{"
        "\"type\":\"call_cap\","
        "\"cap\":\"minihub_remember_chat\","
        "\"input\":{"
        "\"channel\":\"{{event.source_channel}}\","
        "\"chat_id\":\"{{event.chat_id}}\""
        "}"
        "}]"
        "}";

    err = claw_event_router_add_rule_json(rule_json);
    if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "router not ready for remember rule: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        /* Duplicate id on reboot after rules already contain it is fine. */
        ESP_LOGW(TAG, "add remember rule: %s (ok if already present)", esp_err_to_name(err));
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "installed router rule minihub_remember_im_chat");
    }
    return ESP_OK;
}
