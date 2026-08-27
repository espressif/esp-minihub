/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "minihub_module.h"
#include "minihub_zigbee.h"
#include "minihub_product_params.h"
#include "zigbee_map.h"
#include "zigbee_rpc.h"
#include "zigbee_stack.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "adapter_zigbee";

static const cJSON *s_config;
static float s_last_humidity = -1.0f;

static const char *default_value_for_dev(const minihub_zigbee_device_t *dev)
{
    if (!dev) {
        return "off";
    }
    if (dev->caps & MINIHUB_ZB_CAP_IAS_ZONE) {
        return "closed";
    }
    if (dev->type[0] && strcmp(dev->type, "sensor") == 0) {
        return "22.0";
    }
    return "off";
}

static bool type_supports_onoff(const char *type)
{
    return type && (strcmp(type, "light") == 0 || strcmp(type, "switch") == 0);
}

static void register_point_from_dev(const minihub_zigbee_device_t *dev)
{
    if (!dev || !dev->point_id[0]) {
        return;
    }
    minihub_point_t point = {0};
    strncpy(point.point_id, dev->point_id, sizeof(point.point_id) - 1);
    strncpy(point.type, dev->type, sizeof(point.type) - 1);
    strncpy(point.adapter, "zigbee", sizeof(point.adapter) - 1);
    strncpy(point.value, default_value_for_dev(dev), sizeof(point.value) - 1);
    point.online = true;
    minihub_point_register(&point);
}

static void on_zb_device(const minihub_zigbee_device_t *dev, void *ctx)
{
    (void)ctx;
    if (!dev) {
        return;
    }
    ESP_LOGI(TAG, "map device %s type=%s short=0x%04x ep=%u caps=0x%02x", dev->point_id, dev->type,
             dev->short_addr, dev->endpoint, dev->caps);
    register_point_from_dev(dev);
}

static void on_zb_attr(const minihub_zigbee_attr_evt_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) {
        return;
    }
    const minihub_zigbee_device_t *dev =
        minihub_zigbee_find_by_addr(evt->short_addr, evt->endpoint);
    if (!dev) {
        ESP_LOGW(TAG, "attr evt short=0x%04x ep=%u kind=%d — no mapped device", evt->short_addr,
                 evt->endpoint, (int)evt->kind);
        return;
    }

    if (evt->kind == MINIHUB_ZB_ATTR_ONOFF) {
        ESP_LOGI(TAG, "point %s ← onoff %s", dev->point_id, evt->u.onoff ? "on" : "off");
        minihub_point_set(dev->point_id, evt->u.onoff ? "on" : "off", NULL);
        return;
    }

    if (evt->kind == MINIHUB_ZB_ATTR_CONTACT) {
        ESP_LOGI(TAG, "point %s ← contact %s status=0x%04x", dev->point_id,
                 evt->u.contact.open ? "open" : "closed", evt->u.contact.zone_status);
        cJSON *props = cJSON_CreateObject();
        if (props) {
            cJSON_AddBoolToObject(props, "open", evt->u.contact.open);
            cJSON_AddNumberToObject(props, "zone_status", evt->u.contact.zone_status);
            if (evt->u.contact.tamper) {
                cJSON_AddTrueToObject(props, "tamper");
            }
            if (evt->u.contact.battery_low) {
                cJSON_AddTrueToObject(props, "battery_low");
            }
        }
        minihub_point_set(dev->point_id, evt->u.contact.open ? "open" : "closed", props);
        return;
    }

    if (evt->kind == MINIHUB_ZB_ATTR_HUMIDITY_PCT) {
        s_last_humidity = evt->u.humidity_pct;
        ESP_LOGI(TAG, "point %s ← humidity %.1f", dev->point_id, (double)evt->u.humidity_pct);
        cJSON *props = cJSON_CreateObject();
        if (props) {
            cJSON_AddNumberToObject(props, "humidity", evt->u.humidity_pct);
        }
        minihub_point_t snap = {0};
        if (minihub_point_get_snapshot(dev->point_id, &snap) == ESP_OK) {
            minihub_point_set(dev->point_id, snap.value[0] ? snap.value : "0", props);
            if (snap.props) {
                cJSON_Delete(snap.props);
            }
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0f", (double)evt->u.humidity_pct);
            minihub_point_set(dev->point_id, buf, props);
        }
        return;
    }

    if (evt->kind == MINIHUB_ZB_ATTR_TEMP_C) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", (double)evt->u.temp_c);
        ESP_LOGI(TAG, "point %s ← temp %s", dev->point_id, buf);
        cJSON *props = NULL;
        if (s_last_humidity >= 0.0f) {
            props = cJSON_CreateObject();
            if (props) {
                cJSON_AddNumberToObject(props, "humidity", s_last_humidity);
            }
        }
        minihub_point_set(dev->point_id, buf, props);
        return;
    }

    const char *prop_key = NULL;
    double prop_val = 0.0;
    if (evt->kind == MINIHUB_ZB_ATTR_POWER_W || evt->kind == MINIHUB_ZB_ATTR_VOLTAGE_V ||
        evt->kind == MINIHUB_ZB_ATTR_CURRENT_A) {
        if (!(dev->caps & MINIHUB_ZB_CAP_ELECTRICAL)) {
            return;
        }
    }
    if (evt->kind == MINIHUB_ZB_ATTR_ENERGY_WH) {
        if (!(dev->caps & MINIHUB_ZB_CAP_ENERGY)) {
            return;
        }
    }
    if (evt->kind == MINIHUB_ZB_ATTR_POWER_W) {
        prop_key = "power_w";
        prop_val = evt->u.power_w;
    } else if (evt->kind == MINIHUB_ZB_ATTR_VOLTAGE_V) {
        prop_key = "voltage_v";
        prop_val = evt->u.voltage_v;
    } else if (evt->kind == MINIHUB_ZB_ATTR_CURRENT_A) {
        prop_key = "current_a";
        prop_val = evt->u.current_a;
    } else if (evt->kind == MINIHUB_ZB_ATTR_ENERGY_WH) {
        prop_key = "energy_wh";
        prop_val = evt->u.energy_wh;
    }
    if (!prop_key) {
        return;
    }

    ESP_LOGD(TAG, "point %s ← %s %.3f", dev->point_id, prop_key, prop_val);
    minihub_point_t snap = {0};
    cJSON *props = NULL;
    const char *value = "off";
    if (minihub_point_get_snapshot(dev->point_id, &snap) == ESP_OK) {
        value = snap.value[0] ? snap.value : "off";
        props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
    } else {
        props = cJSON_CreateObject();
    }
    if (props) {
        cJSON_DeleteItemFromObject(props, prop_key);
        cJSON_AddNumberToObject(props, prop_key, prop_val);
    }
    minihub_point_set(dev->point_id, value, props);
    if (props) {
        cJSON_Delete(props);
    }
}

static esp_err_t apply_config_binds(const cJSON *config)
{
    if (!config) {
        return ESP_OK;
    }
    const cJSON *devices = cJSON_GetObjectItem(config, "devices");
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *ieee = cJSON_GetObjectItem(item, "ieee");
        const cJSON *short_addr = cJSON_GetObjectItem(item, "short_addr");
        const cJSON *endpoint = cJSON_GetObjectItem(item, "endpoint");
        if (!cJSON_IsString(point_id) || !cJSON_IsString(type)) {
            continue;
        }
        minihub_zigbee_device_t dev = {0};
        strncpy(dev.point_id, point_id->valuestring, sizeof(dev.point_id) - 1);
        strncpy(dev.type, type->valuestring, sizeof(dev.type) - 1);
        if (cJSON_IsString(ieee)) {
            (void)minihub_zb_parse_ieee(ieee->valuestring, &dev.ieee);
        }
        if (cJSON_IsNumber(short_addr)) {
            dev.short_addr = (uint16_t)short_addr->valuedouble;
        }
        if (cJSON_IsNumber(endpoint)) {
            dev.endpoint = (uint8_t)endpoint->valuedouble;
        } else {
            dev.endpoint = 1;
        }
        if (strcmp(dev.type, "sensor") == 0) {
            const cJSON *kind = cJSON_GetObjectItem(item, "kind");
            if (cJSON_IsString(kind) && strcmp(kind->valuestring, "contact") == 0) {
                dev.caps = MINIHUB_ZB_CAP_IAS_ZONE;
            } else if (cJSON_IsNumber(cJSON_GetObjectItem(item, "caps"))) {
                dev.caps = (uint8_t)cJSON_GetObjectItem(item, "caps")->valuedouble;
            } else {
                dev.caps = MINIHUB_ZB_CAP_TEMP | MINIHUB_ZB_CAP_HUMIDITY;
            }
        } else {
            dev.caps = MINIHUB_ZB_CAP_ONOFF;
        }
        (void)minihub_zigbee_upsert_device(&dev);
    }
    return ESP_OK;
}

static esp_err_t zigbee_adapter_init(const cJSON *config)
{
    esp_err_t err = minihub_adapter_check_core_api(1, 1);
    if (err != ESP_OK) {
        return err;
    }
    s_config = config;
    (void)minihub_product_params_init();
    (void)minihub_zigbee_init();
    apply_config_binds(config);
    (void)minihub_module_register_rpc("zigbee", zigbee_module_rpc);
    return ESP_OK;
}

static esp_err_t zigbee_adapter_start(void)
{
    minihub_zigbee_set_callbacks(on_zb_device, on_zb_attr, NULL);

#if CONFIG_MINIHUB_ZIGBEE_SDK
    esp_err_t err = minihub_zigbee_start();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Host stack start: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGI(TAG, "simulation mode (CONFIG_MINIHUB_ZIGBEE_SDK=n)");
#endif

    minihub_zigbee_device_t list[MINIHUB_ZB_MAX_DEVICES];
    size_t n = 0;
    if (minihub_zigbee_list_devices(list, MINIHUB_ZB_MAX_DEVICES, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            register_point_from_dev(&list[i]);
        }
    }

    ESP_LOGI(TAG, "adapter started (%u mapped devices)", (unsigned)n);
    (void)s_config;
    return ESP_OK;
}

static esp_err_t zigbee_adapter_stop(void)
{
    return minihub_zigbee_stop();
}

static esp_err_t zigbee_adapter_poll(void)
{
    return ESP_OK;
}

static esp_err_t zigbee_adapter_handle_command(const minihub_command_t *cmd)
{
    if (cmd && strcmp(cmd->cmd, "set_prop") == 0) {
        const cJSON *prop = cmd->params ? cJSON_GetObjectItem(cmd->params, "prop") : NULL;
        const cJSON *value = cmd->params ? cJSON_GetObjectItem(cmd->params, "value") : NULL;
        if (!cJSON_IsString(prop) || !value) {
            return ESP_ERR_INVALID_ARG;
        }
        minihub_pp_zb_param_t zb = {0};
        bool is_zb = false;
        esp_err_t err = minihub_pp_set_prop(cmd->point_id, prop->valuestring, value, &zb, &is_zb);
        if (err != ESP_OK) {
            return err;
        }
        if (is_zb) {
            const minihub_zigbee_device_t *dev = minihub_zigbee_find_by_point(cmd->point_id);
            if (!dev) {
                return ESP_ERR_NOT_FOUND;
            }
            uint8_t u8 = 0;
            uint16_t u16 = 0;
            int16_t s16 = 0;
            void *pval = &u8;
            uint16_t psz = 1;
            if (zb.zcl_type == 0x10 || zb.zcl_type == 0x20) {
                u8 = cJSON_IsBool(value) ? (uint8_t)cJSON_IsTrue(value) : (uint8_t)value->valuedouble;
                pval = &u8;
                psz = 1;
            } else if (zb.zcl_type == 0x21) {
                u16 = (uint16_t)value->valuedouble;
                pval = &u16;
                psz = 2;
            } else if (zb.zcl_type == 0x29) {
                s16 = (int16_t)value->valuedouble;
                pval = &s16;
                psz = 2;
            }
            return zigbee_stack_write_attr(dev->short_addr, dev->endpoint, zb.cluster, zb.attr_id, zb.zcl_type,
                                           pval, psz);
        }
        return ESP_OK;
    }

    const minihub_zigbee_device_t *dev = minihub_zigbee_find_by_point(cmd->point_id);
#if !CONFIG_MINIHUB_ZIGBEE_SDK
    /* Simulation: allow command if point exists even without Host table */
    if (!dev) {
        minihub_point_t snap = {0};
        if (minihub_point_get_snapshot(cmd->point_id, &snap) != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
        if (!type_supports_onoff(snap.type)) {
            if (snap.props) {
                cJSON_Delete(snap.props);
            }
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        if (strcmp(cmd->cmd, "turn_on") == 0) {
            return minihub_point_set(cmd->point_id, "on", NULL);
        }
        if (strcmp(cmd->cmd, "turn_off") == 0) {
            return minihub_point_set(cmd->point_id, "off", NULL);
        }
        if (strcmp(cmd->cmd, "toggle") == 0) {
            minihub_point_t cur = {0};
            minihub_point_get_snapshot(cmd->point_id, &cur);
            const char *next = (strcmp(cur.value, "on") == 0) ? "off" : "on";
            if (cur.props) {
                cJSON_Delete(cur.props);
            }
            return minihub_point_set(cmd->point_id, next, NULL);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!type_supports_onoff(dev->type)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    minihub_zigbee_onoff_cmd_t zb_cmd;
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        zb_cmd = MINIHUB_ZB_ONOFF_ON;
    } else if (strcmp(cmd->cmd, "turn_off") == 0) {
        zb_cmd = MINIHUB_ZB_ONOFF_OFF;
    } else if (strcmp(cmd->cmd, "toggle") == 0) {
        zb_cmd = MINIHUB_ZB_ONOFF_TOGGLE;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

#if CONFIG_MINIHUB_ZIGBEE_SDK
    esp_err_t err = minihub_zigbee_onoff(dev->short_addr, dev->endpoint, zb_cmd);
    if (err != ESP_OK) {
        return err;
    }
#endif
    if (zb_cmd == MINIHUB_ZB_ONOFF_ON) {
        return minihub_point_set(cmd->point_id, "on", NULL);
    }
    if (zb_cmd == MINIHUB_ZB_ONOFF_OFF) {
        return minihub_point_set(cmd->point_id, "off", NULL);
    }
    minihub_point_t snap = {0};
    minihub_point_get_snapshot(cmd->point_id, &snap);
    const char *next = (strcmp(snap.value, "on") == 0) ? "off" : "on";
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
    return minihub_point_set(cmd->point_id, next, NULL);
}

const minihub_adapter_t zigbee_adapter = {
    .name = "zigbee",
    .version = "0.3.0",
    .init = zigbee_adapter_init,
    .start = zigbee_adapter_start,
    .stop = zigbee_adapter_stop,
    .poll = zigbee_adapter_poll,
    .handle_command = zigbee_adapter_handle_command,
};

REGISTER_ADAPTER(zigbee_adapter);
