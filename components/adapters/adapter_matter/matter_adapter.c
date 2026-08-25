/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_MATTER_ADAPTER

#include "minihub_adapter.h"
#include "minihub_matter.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "adapter_matter";

#define MAX_MATTER_DEVICES 16

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    uint64_t node_id;
    uint16_t endpoint_id;
} matter_device_t;

static matter_device_t s_devices[MAX_MATTER_DEVICES];
static size_t s_device_count;
static bool s_started;

static uint64_t parse_node_id(const char *s)
{
    if (!s || !s[0]) {
        return 1;
    }
    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0) {
        return strtoull(s, NULL, 16);
    }
    return strtoull(s, NULL, 10);
}

static matter_device_t *find_device(const char *point_id)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].point_id, point_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static esp_err_t parse_devices(const cJSON *cfg)
{
    const cJSON *devices = cJSON_GetObjectItem(cfg, "devices");
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        if (s_device_count >= MAX_MATTER_DEVICES) {
            break;
        }
        matter_device_t *dev = &s_devices[s_device_count];
        memset(dev, 0, sizeof(*dev));

        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *node_id = cJSON_GetObjectItem(item, "node_id");
        const cJSON *endpoint_id = cJSON_GetObjectItem(item, "endpoint_id");

        if (!cJSON_IsString(point_id)) {
            continue;
        }
        strncpy(dev->point_id, point_id->valuestring, sizeof(dev->point_id) - 1);
        if (cJSON_IsString(type)) {
            strncpy(dev->type, type->valuestring, sizeof(dev->type) - 1);
        } else {
            strncpy(dev->type, "light", sizeof(dev->type) - 1);
        }
        dev->node_id = cJSON_IsString(node_id) ? parse_node_id(node_id->valuestring) : 1;
        dev->endpoint_id = cJSON_IsNumber(endpoint_id) ? (uint16_t)endpoint_id->valueint : 1;
        s_device_count++;
    }
    return ESP_OK;
}

static esp_err_t matter_adapter_init(const cJSON *config)
{
    s_device_count = 0;
    s_started = false;
    if (config) {
        parse_devices(config);
    }
    return ESP_OK;
}

static esp_err_t matter_adapter_start(void)
{
    esp_err_t err = minihub_matter_init();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        if (minihub_matter_ready()) {
            err = ESP_OK;
        } else {
            ESP_LOGE(TAG, "Matter init failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    for (size_t i = 0; i < s_device_count; i++) {
        minihub_point_t point = {0};
        strncpy(point.point_id, s_devices[i].point_id, sizeof(point.point_id) - 1);
        strncpy(point.type, s_devices[i].type, sizeof(point.type) - 1);
        strncpy(point.adapter, "matter", sizeof(point.adapter) - 1);
        strncpy(point.value, "off", sizeof(point.value) - 1);
        point.online = minihub_matter_ready();
        minihub_point_register(&point);
    }

    s_started = true;
    ESP_LOGI(TAG, "Matter adapter started (%u devices, commissioner=%d)",
             (unsigned)s_device_count, minihub_matter_ready() ? 1 : 0);
    return ESP_OK;
}

static esp_err_t matter_adapter_stop(void)
{
    s_started = false;
    minihub_point_unregister_by_adapter("matter");
    return ESP_OK;
}

static esp_err_t matter_adapter_handle_command(const minihub_command_t *cmd)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    matter_device_t *dev = find_device(cmd->point_id);
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!minihub_matter_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    bool target_on = false;
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        target_on = true;
    } else if (strcmp(cmd->cmd, "turn_off") == 0) {
        target_on = false;
    } else if (strcmp(cmd->cmd, "toggle") == 0) {
        minihub_point_t snap = {0};
        minihub_point_get_snapshot(cmd->point_id, &snap);
        bool on = (strcmp(snap.value, "on") == 0) || (atoi(snap.value) > 0);
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        target_on = !on;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = minihub_matter_onoff(dev->node_id, dev->endpoint_id, target_on);
    if (err == ESP_OK) {
        (void)minihub_point_set(dev->point_id, target_on ? "on" : "off", NULL);
    }
    return err;
}

static esp_err_t matter_adapter_poll(void)
{
    if (!s_started || !minihub_matter_ready()) {
        return ESP_OK;
    }
    /* Matter read/invoke needs deep CHIP stack; adapter_poll runs on a small task.
     * Async attribute poll is not wired up yet — state updates come from commands. */
    return ESP_OK;
}

const minihub_adapter_t matter_adapter = {
    .name = "matter",
    .version = "0.2.0-matter",
    .init = matter_adapter_init,
    .start = matter_adapter_start,
    .stop = matter_adapter_stop,
    .handle_command = matter_adapter_handle_command,
    .poll = matter_adapter_poll,
};

REGISTER_ADAPTER(matter_adapter);

#else /* !CONFIG_MINIHUB_MATTER_ADAPTER */

/* Translation unit kept empty when Matter is disabled. */

#endif /* CONFIG_MINIHUB_MATTER_ADAPTER */
