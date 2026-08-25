/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "adapter_mqtt";

#define MAX_MQTT_DEVICES 16

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    char state_topic[96];
    char command_topic[96];
} mqtt_device_t;

static mqtt_device_t s_devices[MAX_MQTT_DEVICES];
static size_t s_device_count;
static esp_mqtt_client_handle_t s_client;
static char s_broker[64];
static int s_port = 1883;

static mqtt_device_t *find_device_by_point(const char *point_id)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].point_id, point_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static mqtt_device_t *find_device_by_topic(const char *topic, int topic_len)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (strncmp(s_devices[i].state_topic, topic, topic_len) == 0 &&
            s_devices[i].state_topic[topic_len] == '\0') {
            return &s_devices[i];
        }
    }
    return NULL;
}

static const char *normalize_switch_value(const char *payload, int len)
{
    if (len >= 2 && (payload[0] == 'O' || payload[0] == 'o') && (payload[1] == 'N' || payload[1] == 'n')) {
        return "on";
    }
    if (len >= 3 && (payload[0] == 'O' || payload[0] == 'o') && (payload[1] == 'F' || payload[1] == 'f')) {
        return "off";
    }
    if (len >= 7 && strncmp(payload, "pressed", 7) == 0) {
        return "pressed";
    }
    if (len >= 8 && strncmp(payload, "released", 8) == 0) {
        return "released";
    }
    return NULL;
}

static void set_mqtt_points_online(bool online)
{
    for (size_t i = 0; i < s_device_count; i++) {
        minihub_point_set_online(s_devices[i].point_id, online);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        set_mqtt_points_online(true);
        for (size_t i = 0; i < s_device_count; i++) {
            if (s_devices[i].state_topic[0]) {
                esp_mqtt_client_subscribe(s_client, s_devices[i].state_topic, 0);
            }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        set_mqtt_points_online(false);
        break;
    case MQTT_EVENT_DATA: {
        mqtt_device_t *dev = find_device_by_topic(event->topic, event->topic_len);
        if (!dev) {
            break;
        }
        char payload[32] = {0};
        int copy_len = event->data_len < (int)sizeof(payload) - 1 ? event->data_len : (int)sizeof(payload) - 1;
        memcpy(payload, event->data, copy_len);
        const char *value = normalize_switch_value(payload, copy_len);
        if (!value) {
            value = payload;
        }
        minihub_point_set(dev->point_id, value, NULL);
        break;
    }
    default:
        break;
    }
}

static esp_err_t mqtt_parse_devices(const cJSON *cfg)
{
    const cJSON *devices = cJSON_GetObjectItem(cfg, "devices");
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        if (s_device_count >= MAX_MQTT_DEVICES) {
            break;
        }
        mqtt_device_t *dev = &s_devices[s_device_count];
        memset(dev, 0, sizeof(*dev));

        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *state_topic = cJSON_GetObjectItem(item, "state_topic");
        const cJSON *command_topic = cJSON_GetObjectItem(item, "command_topic");

        if (!cJSON_IsString(point_id) || !cJSON_IsString(type)) {
            continue;
        }

        strncpy(dev->point_id, point_id->valuestring, sizeof(dev->point_id) - 1);
        strncpy(dev->type, type->valuestring, sizeof(dev->type) - 1);
        if (cJSON_IsString(state_topic)) {
            strncpy(dev->state_topic, state_topic->valuestring, sizeof(dev->state_topic) - 1);
        }
        if (cJSON_IsString(command_topic)) {
            strncpy(dev->command_topic, command_topic->valuestring, sizeof(dev->command_topic) - 1);
        }
        s_device_count++;
    }
    return ESP_OK;
}

static esp_err_t mqtt_adapter_init(const cJSON *config)
{
    s_device_count = 0;
    if (!config) {
        return ESP_OK;
    }

    const cJSON *broker = cJSON_GetObjectItem(config, "broker");
    const cJSON *port = cJSON_GetObjectItem(config, "port");
    if (cJSON_IsString(broker)) {
        strncpy(s_broker, broker->valuestring, sizeof(s_broker) - 1);
    }
    if (cJSON_IsNumber(port)) {
        s_port = port->valueint;
    }
    return mqtt_parse_devices(config);
}

static esp_err_t mqtt_adapter_start(void)
{
    for (size_t i = 0; i < s_device_count; i++) {
        minihub_point_t point = {0};
        strncpy(point.point_id, s_devices[i].point_id, sizeof(point.point_id) - 1);
        strncpy(point.type, s_devices[i].type, sizeof(point.type) - 1);
        strncpy(point.adapter, "mqtt", sizeof(point.adapter) - 1);
        strncpy(point.value, "off", sizeof(point.value) - 1);
        point.online = false;
        minihub_point_register(&point);
    }

    if (s_broker[0] == '\0') {
        ESP_LOGW(TAG, "no broker configured, points registered in offline mode");
        return ESP_OK;
    }

    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", s_broker, s_port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
    };
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

static esp_err_t mqtt_adapter_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    return ESP_OK;
}

static esp_err_t mqtt_adapter_handle_command(const minihub_command_t *cmd)
{
    mqtt_device_t *dev = find_device_by_point(cmd->point_id);
    if (!dev || dev->command_topic[0] == '\0') {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *payload = NULL;
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        payload = "ON";
    } else if (strcmp(cmd->cmd, "turn_off") == 0) {
        payload = "OFF";
    } else if (strcmp(cmd->cmd, "toggle") == 0) {
        minihub_point_t snap = {0};
        minihub_point_get_snapshot(cmd->point_id, &snap);
        payload = (strcmp(snap.value, "on") == 0) ? "OFF" : "ON";
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_client) {
        esp_mqtt_client_publish(s_client, dev->command_topic, payload, 0, 1, 0);
    }
    minihub_point_set(cmd->point_id, (payload[0] == 'O' && payload[1] == 'N') ? "on" : "off", NULL);
    return ESP_OK;
}

const minihub_adapter_t mqtt_adapter = {
    .name = "mqtt",
    .version = "0.1.0",
    .init = mqtt_adapter_init,
    .start = mqtt_adapter_start,
    .stop = mqtt_adapter_stop,
    .deinit = mqtt_adapter_stop,
    .handle_command = mqtt_adapter_handle_command,
};

REGISTER_ADAPTER(mqtt_adapter);
