/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rainmaker_bridge.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "cJSON.h"

#include "esp_rmaker_core.h"
#include "esp_rmaker_standard_types.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_devices.h"

static const char *TAG = "rmaker_bridge";

#define BRIDGE_MAX_DEVICES MINIHUB_MAX_POINTS

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char device_name[MINIHUB_POINT_ID_MAX];
    char *priv_point_id; /* owned; passed to RainMaker device priv_data */
    esp_rmaker_device_t *device;
    bool has_brightness;
    bool has_cct;
    bool has_humidity;
    bool has_recording;
} bridge_entry_t;

static esp_rmaker_node_t *s_node;
static bridge_entry_t s_entries[BRIDGE_MAX_DEVICES];
static size_t s_entry_count;
static bool s_updating_from_cloud;
static bool s_suppress_config_report;

static void maybe_report_node_config(void)
{
    if (!s_suppress_config_report) {
        (void)esp_rmaker_report_node_details();
    }
}

static bool value_is_on(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }
    if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        return true;
    }
    return false;
}

static void point_id_to_device_name(const char *point_id, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; point_id[i] != '\0' && j + 1 < out_len; i++) {
        char c = point_id[i];
        out[j++] = (c == '/') ? '_' : c;
    }
    out[j] = '\0';
}

static bridge_entry_t *find_entry(const char *point_id)
{
    for (size_t i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].point_id, point_id) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static int props_int(const cJSON *props, const char *key, int def)
{
    if (!props) {
        return def;
    }
    const cJSON *v = cJSON_GetObjectItem(props, key);
    if (cJSON_IsNumber(v)) {
        return v->valueint;
    }
    return def;
}

static float props_float(const cJSON *props, const char *key, float def)
{
    if (!props) {
        return def;
    }
    const cJSON *v = cJSON_GetObjectItem(props, key);
    if (cJSON_IsNumber(v)) {
        return (float)v->valuedouble;
    }
    return def;
}

static esp_err_t invoke_cmd(const char *point_id, const char *cmd, cJSON *params)
{
    minihub_command_t c = {0};
    strncpy(c.point_id, point_id, sizeof(c.point_id) - 1);
    strncpy(c.cmd, cmd, sizeof(c.cmd) - 1);
    c.params = params;
    return minihub_command_invoke(&c);
}

static esp_err_t write_cb(const esp_rmaker_device_t *device, const esp_rmaker_param_t *param,
                          const esp_rmaker_param_val_t val, void *priv_data,
                          esp_rmaker_write_ctx_t *ctx)
{
    (void)ctx;
    const char *point_id = (const char *)priv_data;
    if (!point_id || point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *param_name = esp_rmaker_param_get_name(param);
    if (!param_name) {
        return ESP_ERR_INVALID_ARG;
    }

    s_updating_from_cloud = true;
    esp_err_t err = ESP_OK;

    if (strcmp(param_name, ESP_RMAKER_DEF_POWER_NAME) == 0) {
        err = invoke_cmd(point_id, val.val.b ? "turn_on" : "turn_off", NULL);
        if (err == ESP_OK) {
            esp_rmaker_param_update_and_report(param, val);
        }
    } else if (strcmp(param_name, ESP_RMAKER_DEF_BRIGHTNESS_NAME) == 0) {
        int bright = val.val.i;
        if (bright < 0) {
            bright = 0;
        }
        if (bright > 100) {
            bright = 100;
        }
        cJSON *params = cJSON_CreateObject();
        if (params) {
            cJSON_AddNumberToObject(params, "brightness", bright);
        }
        err = invoke_cmd(point_id, "set_brightness", params);
        if (params) {
            cJSON_Delete(params);
        }
        if (err == ESP_OK) {
            esp_rmaker_param_update_and_report(param, esp_rmaker_int(bright));
        }
    } else if (strcmp(param_name, ESP_RMAKER_DEF_CCT_NAME) == 0) {
        cJSON *params = cJSON_CreateObject();
        if (params) {
            cJSON_AddNumberToObject(params, "temperature", val.val.i);
        }
        err = invoke_cmd(point_id, "set_color_temp", params);
        if (params) {
            cJSON_Delete(params);
        }
        if (err == ESP_OK) {
            esp_rmaker_param_update_and_report(param, val);
        }
    } else if (strcmp(param_name, ESP_RMAKER_DEF_NAME_PARAM) == 0) {
        esp_rmaker_param_update_and_report(param, val);
    } else {
        ESP_LOGD(TAG, "ignore write %s on %s", param_name, point_id);
    }

    s_updating_from_cloud = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cloud write %s/%s failed: %s", point_id, param_name, esp_err_to_name(err));
    }
    return err;
}

static void report_switch_or_light(bridge_entry_t *e, const minihub_point_t *point)
{
    esp_rmaker_param_t *power = esp_rmaker_device_get_param_by_name(e->device, ESP_RMAKER_DEF_POWER_NAME);
    if (power) {
        esp_rmaker_param_update_and_report(power, esp_rmaker_bool(value_is_on(point->value)));
    }
    if (e->has_brightness) {
        esp_rmaker_param_t *br = esp_rmaker_device_get_param_by_name(e->device, ESP_RMAKER_DEF_BRIGHTNESS_NAME);
        if (br) {
            int bright = props_int(point->props, "brightness", 0);
            if (bright <= 0 && point->value[0] && !value_is_on(point->value) &&
                strcmp(point->value, "off") != 0) {
                bright = atoi(point->value);
            }
            if (bright < 0) {
                bright = 0;
            }
            if (bright > 100) {
                bright = 100;
            }
            esp_rmaker_param_update_and_report(br, esp_rmaker_int(bright));
        }
    }
    if (e->has_cct) {
        esp_rmaker_param_t *cct = esp_rmaker_device_get_param_by_name(e->device, ESP_RMAKER_DEF_CCT_NAME);
        if (cct) {
            int temp = props_int(point->props, "temperature", 4000);
            esp_rmaker_param_update_and_report(cct, esp_rmaker_int(temp));
        }
    }
}

static void report_sensor(bridge_entry_t *e, const minihub_point_t *point)
{
    esp_rmaker_param_t *temp = esp_rmaker_device_get_param_by_name(e->device, ESP_RMAKER_DEF_TEMPERATURE_NAME);
    if (temp) {
        float t = (float)atof(point->value);
        esp_rmaker_param_update_and_report(temp, esp_rmaker_float(t));
    }
    if (e->has_humidity) {
        esp_rmaker_param_t *hum = esp_rmaker_device_get_param_by_name(e->device, "Ambient Humidity");
        if (hum) {
            float h = props_float(point->props, "humidity", 0.0f);
            esp_rmaker_param_update_and_report(hum, esp_rmaker_float(h));
        }
    }
}

static void report_camera(bridge_entry_t *e, const minihub_point_t *point)
{
    esp_rmaker_param_t *power = esp_rmaker_device_get_param_by_name(e->device, ESP_RMAKER_DEF_POWER_NAME);
    if (power) {
        esp_rmaker_param_update_and_report(power, esp_rmaker_bool(value_is_on(point->value)));
    }
    if (e->has_recording) {
        esp_rmaker_param_t *rec = esp_rmaker_device_get_param_by_name(e->device, "Recording");
        if (rec) {
            bool recording = false;
            if (point->props) {
                const cJSON *v = cJSON_GetObjectItem(point->props, "recording");
                recording = cJSON_IsTrue(v);
            }
            esp_rmaker_param_update_and_report(rec, esp_rmaker_bool(recording));
        }
    }
}

static esp_err_t alloc_priv(bridge_entry_t *e)
{
    free(e->priv_point_id);
    e->priv_point_id = strdup(e->point_id);
    return e->priv_point_id ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t create_switch(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    e->device = esp_rmaker_switch_device_create(e->device_name, e->priv_point_id, value_is_on(point->value));
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);
    return esp_rmaker_node_add_device(s_node, e->device);
}

static esp_err_t create_light(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    e->device = esp_rmaker_lightbulb_device_create(e->device_name, e->priv_point_id, value_is_on(point->value));
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);

    int bright = props_int(point->props, "brightness", 50);
    if (bright < 0) {
        bright = 0;
    }
    if (bright > 100) {
        bright = 100;
    }
    esp_rmaker_param_t *br = esp_rmaker_brightness_param_create(ESP_RMAKER_DEF_BRIGHTNESS_NAME, bright);
    if (br) {
        esp_rmaker_device_add_param(e->device, br);
        e->has_brightness = true;
    }

    if (point->props && cJSON_GetObjectItem(point->props, "temperature")) {
        int cct = props_int(point->props, "temperature", 4000);
        esp_rmaker_param_t *cct_p = esp_rmaker_cct_param_create(ESP_RMAKER_DEF_CCT_NAME, cct);
        if (cct_p) {
            esp_rmaker_device_add_param(e->device, cct_p);
            e->has_cct = true;
        }
    }

    return esp_rmaker_node_add_device(s_node, e->device);
}

static esp_err_t create_sensor(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    float t = (float)atof(point->value);
    e->device = esp_rmaker_temp_sensor_device_create(e->device_name, e->priv_point_id, t);
    if (!e->device) {
        return ESP_FAIL;
    }

    if (point->props && cJSON_GetObjectItem(point->props, "humidity")) {
        float h = props_float(point->props, "humidity", 0.0f);
        esp_rmaker_param_t *hum = esp_rmaker_param_create("Ambient Humidity", "esp.param.humidity",
                                                          esp_rmaker_float(h), PROP_FLAG_READ);
        if (hum) {
            esp_rmaker_param_add_ui_type(hum, ESP_RMAKER_UI_TEXT);
            esp_rmaker_device_add_param(e->device, hum);
            e->has_humidity = true;
        }
    }

    return esp_rmaker_node_add_device(s_node, e->device);
}

static esp_err_t create_camera(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    e->device = esp_rmaker_device_create(e->device_name, "esp.device.camera", e->priv_point_id);
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);

    esp_rmaker_param_t *name = esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM, e->device_name);
    if (name) {
        esp_rmaker_device_add_param(e->device, name);
    }

    esp_rmaker_param_t *power = esp_rmaker_param_create(ESP_RMAKER_DEF_POWER_NAME, ESP_RMAKER_PARAM_POWER,
                                                        esp_rmaker_bool(value_is_on(point->value)),
                                                        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (power) {
        esp_rmaker_param_add_ui_type(power, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(e->device, power);
    }

    bool recording = false;
    if (point->props) {
        const cJSON *v = cJSON_GetObjectItem(point->props, "recording");
        recording = cJSON_IsTrue(v);
    }
    esp_rmaker_param_t *rec = esp_rmaker_param_create("Recording", "esp.param.recording",
                                                      esp_rmaker_bool(recording), PROP_FLAG_READ);
    if (rec) {
        esp_rmaker_param_add_ui_type(rec, ESP_RMAKER_UI_TEXT);
        esp_rmaker_device_add_param(e->device, rec);
        e->has_recording = true;
    }

    return esp_rmaker_node_add_device(s_node, e->device);
}

esp_err_t rainmaker_bridge_add_point(const minihub_point_t *point)
{
    if (!s_node || !point || point->point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(point->type, "switch") != 0 && strcmp(point->type, "light") != 0 &&
        strcmp(point->type, "sensor") != 0 && strcmp(point->type, "camera") != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (find_entry(point->point_id)) {
        return ESP_OK;
    }
    if (s_entry_count >= BRIDGE_MAX_DEVICES) {
        return ESP_ERR_NO_MEM;
    }

    bridge_entry_t *e = &s_entries[s_entry_count];
    memset(e, 0, sizeof(*e));
    strncpy(e->point_id, point->point_id, sizeof(e->point_id) - 1);
    point_id_to_device_name(point->point_id, e->device_name, sizeof(e->device_name));

    esp_err_t err;
    if (strcmp(point->type, "switch") == 0) {
        err = create_switch(e, point);
    } else if (strcmp(point->type, "light") == 0) {
        err = create_light(e, point);
    } else if (strcmp(point->type, "camera") == 0) {
        err = create_camera(e, point);
    } else {
        err = create_sensor(e, point);
    }

    if (err != ESP_OK || !e->device) {
        ESP_LOGW(TAG, "add device for %s failed: %s", point->point_id, esp_err_to_name(err));
        free(e->priv_point_id);
        memset(e, 0, sizeof(*e));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    esp_rmaker_device_add_attribute(e->device, "point_id", point->point_id);
    if (point->adapter[0]) {
        esp_rmaker_device_add_attribute(e->device, "adapter", point->adapter);
    }

    s_entry_count++;
    ESP_LOGI(TAG, "mapped %s (%s) -> %s", point->point_id, point->type, e->device_name);
    maybe_report_node_config();
    return ESP_OK;
}

esp_err_t rainmaker_bridge_remove_point(const char *point_id)
{
    if (!s_node || !point_id) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].point_id, point_id) != 0) {
            continue;
        }
        if (s_entries[i].device) {
            esp_rmaker_node_remove_device(s_node, s_entries[i].device);
            esp_rmaker_device_delete(s_entries[i].device);
        }
        free(s_entries[i].priv_point_id);
        for (size_t j = i; j + 1 < s_entry_count; j++) {
            s_entries[j] = s_entries[j + 1];
        }
        memset(&s_entries[s_entry_count - 1], 0, sizeof(bridge_entry_t));
        s_entry_count--;
        ESP_LOGI(TAG, "unmapped %s", point_id);
        maybe_report_node_config();
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void rainmaker_bridge_on_point_updated(uint16_t point_index)
{
    if (s_updating_from_cloud || !s_node) {
        return;
    }
    minihub_point_t *point = minihub_point_get_by_index(point_index);
    if (!point) {
        return;
    }
    bridge_entry_t *e = find_entry(point->point_id);
    if (!e || !e->device) {
        return;
    }
    if (strcmp(point->type, "sensor") == 0) {
        report_sensor(e, point);
    } else if (strcmp(point->type, "camera") == 0) {
        report_camera(e, point);
    } else {
        report_switch_or_light(e, point);
    }
}

esp_err_t rainmaker_bridge_sync_all(void)
{
    if (!s_node) {
        return ESP_ERR_INVALID_STATE;
    }
    minihub_point_t *points = NULL;
    size_t count = 0;
    esp_err_t err = minihub_point_list(&points, &count);
    if (err != ESP_OK) {
        return err;
    }

    s_suppress_config_report = true;

    /* Drop RainMaker devices whose points no longer exist (soft reload / sync). */
    for (size_t i = 0; i < s_entry_count; ) {
        bool still_present = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(s_entries[i].point_id, points[j].point_id) == 0) {
                still_present = true;
                break;
            }
        }
        if (!still_present) {
            (void)rainmaker_bridge_remove_point(s_entries[i].point_id);
            continue;
        }
        i++;
    }

    for (size_t i = 0; i < count; i++) {
        rainmaker_bridge_add_point(&points[i]);
        if (points[i].props) {
            cJSON_Delete(points[i].props);
            points[i].props = NULL;
        }
    }
    free(points);

    s_suppress_config_report = false;
    (void)esp_rmaker_report_node_details();
    return ESP_OK;
}

static void on_point_updated(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event) {
        return;
    }
    rainmaker_bridge_on_point_updated(event->point_index);
}

static void on_point_registered(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event || !event->data) {
        return;
    }
    const cJSON *id = cJSON_GetObjectItem(event->data, "point_id");
    if (!cJSON_IsString(id) || !id->valuestring) {
        return;
    }
    minihub_point_t *p = minihub_point_get(id->valuestring);
    if (p) {
        rainmaker_bridge_add_point(p);
    }
}

static void on_point_unregistered(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event || !event->data) {
        return;
    }
    const cJSON *id = cJSON_GetObjectItem(event->data, "point_id");
    if (cJSON_IsString(id) && id->valuestring) {
        rainmaker_bridge_remove_point(id->valuestring);
    }
}

esp_err_t rainmaker_bridge_init(esp_rmaker_node_t *node)
{
    if (!node) {
        return ESP_ERR_INVALID_ARG;
    }
    s_node = node;
    s_entry_count = 0;
    memset(s_entries, 0, sizeof(s_entries));

    esp_err_t err = minihub_event_bus_subscribe("point.updated", on_point_updated, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_event_bus_subscribe("point.registered", on_point_registered, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_event_bus_subscribe("point.unregistered", on_point_unregistered, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return rainmaker_bridge_sync_all();
}
