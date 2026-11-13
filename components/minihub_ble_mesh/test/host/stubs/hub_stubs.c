/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hub_stubs.h"
#include "minihub_module.h"
#include "ble_mesh_rpc.h"

#include <string.h>

hub_mesh_cmd_t hub_mesh_cmds[HUB_MESH_CMD_MAX];
size_t hub_mesh_cmd_count;
esp_err_t hub_mesh_result = ESP_OK;

minihub_ble_mesh_device_t hub_devs[HUB_DEV_MAX];
size_t hub_dev_count;

hub_point_t hub_points[HUB_POINT_MAX];
size_t hub_point_register_count;
size_t hub_point_set_count;

minihub_bm_provision_policy_t hub_policy;
size_t hub_policy_count;

minihub_ble_mesh_device_cb_t hub_on_device;
minihub_ble_mesh_attr_cb_t hub_on_attr;
void *hub_cb_ctx;

hub_pp_apply_t hub_pp_applies[HUB_PP_MAX];
size_t hub_pp_apply_count;
hub_pp_set_t hub_pp_sets[HUB_PP_MAX];
size_t hub_pp_set_count;

bool hub_running = true;
char hub_ota_point[MINIHUB_POINT_ID_MAX];
size_t hub_ota_count;

void hub_stub_reset(void)
{
    for (size_t i = 0; i < HUB_POINT_MAX; i++) {
        if (hub_points[i].used && hub_points[i].point.props) {
            cJSON_Delete(hub_points[i].point.props);
        }
    }
    memset(hub_points, 0, sizeof(hub_points));
    memset(hub_mesh_cmds, 0, sizeof(hub_mesh_cmds));
    memset(hub_devs, 0, sizeof(hub_devs));
    memset(hub_pp_applies, 0, sizeof(hub_pp_applies));
    memset(hub_pp_sets, 0, sizeof(hub_pp_sets));
    memset(&hub_policy, 0, sizeof(hub_policy));
    memset(hub_ota_point, 0, sizeof(hub_ota_point));
    hub_mesh_cmd_count = 0;
    hub_mesh_result = ESP_OK;
    hub_dev_count = 0;
    hub_point_register_count = 0;
    hub_point_set_count = 0;
    hub_policy_count = 0;
    hub_pp_apply_count = 0;
    hub_pp_set_count = 0;
    hub_ota_count = 0;
    hub_on_device = NULL;
    hub_on_attr = NULL;
    hub_cb_ctx = NULL;
    hub_running = true;
}

hub_point_t *hub_point_find(const char *point_id)
{
    if (!point_id) {
        return NULL;
    }
    for (size_t i = 0; i < HUB_POINT_MAX; i++) {
        if (hub_points[i].used && strcmp(hub_points[i].point.point_id, point_id) == 0) {
            return &hub_points[i];
        }
    }
    return NULL;
}

const cJSON *hub_point_param(const char *point_id, const char *param)
{
    const hub_point_t *p = hub_point_find(point_id);
    if (!p || !p->point.props) {
        return NULL;
    }
    const cJSON *params = cJSON_GetObjectItem(p->point.props, "params");
    const cJSON *ent = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, param) : NULL;
    return cJSON_IsObject(ent) ? cJSON_GetObjectItem(ent, "value") : NULL;
}

const hub_mesh_cmd_t *hub_last_mesh_cmd(void)
{
    return hub_mesh_cmd_count ? &hub_mesh_cmds[hub_mesh_cmd_count - 1] : NULL;
}

static hub_mesh_cmd_t *mesh_cmd_slot(hub_mesh_kind_t kind, uint16_t unicast)
{
    if (hub_mesh_cmd_count >= HUB_MESH_CMD_MAX) {
        return NULL;
    }
    hub_mesh_cmd_t *c = &hub_mesh_cmds[hub_mesh_cmd_count++];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->unicast = unicast;
    return c;
}

/* ------------------------------------------------------------- mesh stack */

esp_err_t minihub_ble_mesh_init(void)
{
    return ESP_OK;
}

esp_err_t minihub_ble_mesh_start(void)
{
    return ESP_OK;
}

esp_err_t minihub_ble_mesh_stop(void)
{
    return ESP_OK;
}

bool minihub_ble_mesh_running(void)
{
    return hub_running;
}

esp_err_t minihub_ble_mesh_set_provision_policy(const minihub_bm_provision_policy_t *policy)
{
    if (!policy) {
        return ESP_ERR_INVALID_ARG;
    }
    hub_policy = *policy;
    hub_policy_count++;
    return ESP_OK;
}

esp_err_t minihub_ble_mesh_set_callbacks(minihub_ble_mesh_device_cb_t on_device,
                                         minihub_ble_mesh_attr_cb_t on_attr, void *ctx)
{
    hub_on_device = on_device;
    hub_on_attr = on_attr;
    hub_cb_ctx = ctx;
    return ESP_OK;
}

esp_err_t minihub_ble_mesh_upsert_device(const minihub_ble_mesh_device_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < hub_dev_count; i++) {
        if (strcmp(hub_devs[i].point_id, dev->point_id) == 0) {
            hub_devs[i] = *dev;
            return ESP_OK;
        }
    }
    if (hub_dev_count >= HUB_DEV_MAX) {
        return ESP_ERR_NO_MEM;
    }
    hub_devs[hub_dev_count++] = *dev;
    return ESP_OK;
}

esp_err_t minihub_ble_mesh_list_devices(minihub_ble_mesh_device_t *out, size_t cap, size_t *count)
{
    if (!out || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = hub_dev_count < cap ? hub_dev_count : cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = hub_devs[i];
    }
    *count = n;
    return ESP_OK;
}

const minihub_ble_mesh_device_t *minihub_ble_mesh_find_by_point(const char *point_id)
{
    if (!point_id) {
        return NULL;
    }
    for (size_t i = 0; i < hub_dev_count; i++) {
        if (strcmp(hub_devs[i].point_id, point_id) == 0) {
            return &hub_devs[i];
        }
    }
    return NULL;
}

const minihub_ble_mesh_device_t *minihub_ble_mesh_find_by_unicast(uint16_t unicast)
{
    for (size_t i = 0; i < hub_dev_count; i++) {
        if (hub_devs[i].unicast == unicast) {
            return &hub_devs[i];
        }
    }
    return NULL;
}

esp_err_t minihub_ble_mesh_onoff(uint16_t unicast, minihub_ble_mesh_onoff_cmd_t cmd)
{
    hub_mesh_cmd_t *c = mesh_cmd_slot(HUB_MESH_ONOFF, unicast);
    if (c) {
        c->onoff = (int)cmd;
    }
    return hub_mesh_result;
}

esp_err_t minihub_ble_mesh_level(uint16_t unicast, int16_t level)
{
    hub_mesh_cmd_t *c = mesh_cmd_slot(HUB_MESH_LEVEL, unicast);
    if (c) {
        c->level = level;
    }
    return hub_mesh_result;
}

esp_err_t minihub_ble_mesh_lightness(uint16_t unicast, uint16_t lightness)
{
    hub_mesh_cmd_t *c = mesh_cmd_slot(HUB_MESH_LIGHTNESS, unicast);
    if (c) {
        c->lightness = lightness;
    }
    return hub_mesh_result;
}

esp_err_t minihub_ble_mesh_ctl(uint16_t unicast, uint16_t lightness, uint16_t temperature,
                               int16_t delta_uv)
{
    hub_mesh_cmd_t *c = mesh_cmd_slot(HUB_MESH_CTL, unicast);
    if (c) {
        c->lightness = lightness;
        c->temperature = temperature;
        c->delta_uv = delta_uv;
    }
    return hub_mesh_result;
}

esp_err_t minihub_ble_mesh_hsl(uint16_t unicast, uint16_t hue, uint16_t saturation,
                               uint16_t lightness)
{
    hub_mesh_cmd_t *c = mesh_cmd_slot(HUB_MESH_HSL, unicast);
    if (c) {
        c->hue = hue;
        c->saturation = saturation;
        c->lightness = lightness;
    }
    return hub_mesh_result;
}

esp_err_t minihub_ble_mesh_ota_offer(const char *point_id, const char *file_path)
{
    (void)file_path;
    hub_ota_count++;
    if (point_id) {
        strncpy(hub_ota_point, point_id, sizeof(hub_ota_point) - 1);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ Core */

esp_err_t minihub_point_register(const minihub_point_t *point)
{
    if (!point || !point->point_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    hub_point_t *slot = hub_point_find(point->point_id);
    if (!slot) {
        for (size_t i = 0; i < HUB_POINT_MAX; i++) {
            if (!hub_points[i].used) {
                slot = &hub_points[i];
                break;
            }
        }
    }
    if (!slot) {
        return ESP_ERR_NO_MEM;
    }
    if (slot->used && slot->point.props) {
        cJSON_Delete(slot->point.props);
    }
    slot->point = *point;
    slot->point.props = point->props ? cJSON_Duplicate(point->props, true) : NULL;
    slot->used = true;
    hub_point_register_count++;
    return ESP_OK;
}

esp_err_t minihub_point_get_snapshot(const char *point_id, minihub_point_t *out)
{
    const hub_point_t *p = hub_point_find(point_id);
    if (!p || !out) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = p->point;
    out->props = p->point.props ? cJSON_Duplicate(p->point.props, true) : NULL;
    return ESP_OK;
}

esp_err_t minihub_point_set(const char *point_id, const char *value, cJSON *props)
{
    hub_point_t *p = hub_point_find(point_id);
    if (!p) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Core turns a NULL value into "" and keeps props when props is NULL. */
    strncpy(p->point.value, value ? value : "", sizeof(p->point.value) - 1);
    p->point.value[sizeof(p->point.value) - 1] = '\0';
    if (props) {
        cJSON *dup = cJSON_Duplicate(props, true);
        if (p->point.props) {
            cJSON_Delete(p->point.props);
        }
        p->point.props = dup;
    }
    hub_point_set_count++;
    return ESP_OK;
}

esp_err_t minihub_point_merge_props(const char *point_id, const char *value, cJSON *props_patch)
{
    hub_point_t *p = hub_point_find(point_id);
    if (!p) {
        return ESP_ERR_NOT_FOUND;
    }
    if (value) {
        strncpy(p->point.value, value, sizeof(p->point.value) - 1);
        p->point.value[sizeof(p->point.value) - 1] = '\0';
    }
    if (!props_patch) {
        return ESP_OK;
    }
    if (!p->point.props) {
        p->point.props = cJSON_CreateObject();
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, props_patch)
    {
        cJSON_DeleteItemFromObject(p->point.props, item->string);
        cJSON_AddItemToObject(p->point.props, item->string, cJSON_Duplicate(item, true));
    }
    return ESP_OK;
}

void minihub_heap_log(void)
{
}

/* -------------------------------------------------------- product_params */

esp_err_t minihub_product_params_init(void)
{
    return ESP_OK;
}

size_t minihub_pp_ble_mesh_list_params(const char *uuid_hex, minihub_pp_bm_param_t *out, size_t cap)
{
    (void)uuid_hex;
    (void)out;
    (void)cap;
    return 0;
}

bool minihub_pp_ble_mesh_apply(const char *point_id, const char *uuid_hex, uint16_t model_id,
                               uint16_t company_id, uint32_t opcode, const char *field, double raw)
{
    (void)uuid_hex;
    (void)company_id;
    (void)opcode;
    if (hub_pp_apply_count < HUB_PP_MAX) {
        hub_pp_apply_t *a = &hub_pp_applies[hub_pp_apply_count++];
        memset(a, 0, sizeof(*a));
        if (point_id) {
            strncpy(a->point_id, point_id, sizeof(a->point_id) - 1);
        }
        a->model_id = model_id;
        if (field) {
            strncpy(a->field, field, sizeof(a->field) - 1);
        }
        a->raw = raw;
    }
    return true;
}

esp_err_t minihub_pp_set_prop(const char *point_id, const char *prop, const cJSON *value,
                              minihub_pp_zb_param_t *out_zb, bool *is_zigbee)
{
    (void)value;
    (void)out_zb;
    if (is_zigbee) {
        *is_zigbee = false;
    }
    if (hub_pp_set_count < HUB_PP_MAX) {
        hub_pp_set_t *s = &hub_pp_sets[hub_pp_set_count++];
        memset(s, 0, sizeof(*s));
        if (point_id) {
            strncpy(s->point_id, point_id, sizeof(s->point_id) - 1);
        }
        if (prop) {
            strncpy(s->prop, prop, sizeof(s->prop) - 1);
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------- module / rpc glue */

esp_err_t minihub_module_register_rpc(const char *module_id, minihub_module_rpc_fn rpc)
{
    (void)module_id;
    (void)rpc;
    return ESP_OK;
}

esp_err_t ble_mesh_module_rpc(const char *method, const cJSON *params, cJSON **out)
{
    (void)method;
    (void)params;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
