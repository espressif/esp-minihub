/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "minihub_module.h"
#include "minihub_ble_mesh.h"
#include "ble_mesh_map.h"
#include "minihub_product_params.h"
#include "minihub_device_widget.h"
#include "ble_mesh_adapter.h"
#include "ble_mesh_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "minihub_core.h"
#include "sdkconfig.h"

static const char *TAG = "adapter_ble_mesh";

static const cJSON *s_config;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** Parse even-length hex string into bytes. Returns length or -1. */
static int parse_hex_bytes(const char *hex, uint8_t *out, size_t out_cap)
{
    if (!hex || !out) {
        return -1;
    }
    while (*hex == ' ' || *hex == '\t') {
        hex++;
    }
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }
    size_t n = strlen(hex);
    if (n == 0 || (n & 1) || (n / 2) > out_cap) {
        return -1;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

static bool parse_model_id(const cJSON *item, uint16_t *model_id)
{
    if (!model_id) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        *model_id = (uint16_t)item->valuedouble;
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    const char *s = item->valuestring;
    if (minihub_bm_model_id_by_name(s, model_id)) {
        return true;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || v > 0xFFFFUL) {
        return false;
    }
    *model_id = (uint16_t)v;
    return true;
}

static void apply_provision_policy(const cJSON *config)
{
    minihub_bm_provision_policy_t policy = {0};
    const cJSON *prov = cJSON_GetObjectItem(config, "provision");
    const cJSON *filters = NULL;
    const cJSON *bind_models = NULL;
    const cJSON *disabled = NULL;

    if (cJSON_IsObject(prov)) {
        filters = cJSON_GetObjectItem(prov, "uuid_filters");
        bind_models = cJSON_GetObjectItem(prov, "bind_models");
        disabled = cJSON_GetObjectItem(prov, "uuid_match_disabled");
    }
    /* Flat keys also accepted for pack inject convenience. */
    if (!filters) {
        filters = cJSON_GetObjectItem(config, "uuid_filters");
    }
    if (!bind_models) {
        bind_models = cJSON_GetObjectItem(config, "bind_models");
    }

    if (cJSON_IsTrue(disabled)) {
        policy.accept_all_uuid = true;
    } else if (cJSON_IsArray(filters)) {
        if (cJSON_GetArraySize(filters) == 0) {
            policy.accept_all_uuid = true;
        } else {
            cJSON *f = NULL;
            cJSON_ArrayForEach(f, filters) {
                if (policy.filter_count >= MINIHUB_BM_UUID_FILTER_MAX) {
                    break;
                }
                const cJSON *prefix = cJSON_GetObjectItem(f, "prefix_hex");
                if (!prefix) {
                    prefix = cJSON_GetObjectItem(f, "hex");
                }
                if (!cJSON_IsString(prefix)) {
                    continue;
                }
                minihub_bm_uuid_filter_t *slot = &policy.filters[policy.filter_count];
                int len = parse_hex_bytes(prefix->valuestring, slot->bytes,
                                          MINIHUB_BM_UUID_FILTER_BYTES_MAX);
                if (len <= 0) {
                    ESP_LOGW(TAG, "skip bad uuid filter: %s", prefix->valuestring);
                    continue;
                }
                slot->len = (uint8_t)len;
                const cJSON *offset = cJSON_GetObjectItem(f, "offset");
                slot->offset = cJSON_IsNumber(offset) ? (uint8_t)offset->valuedouble : 0;
                if ((size_t)slot->offset + slot->len > 16) {
                    ESP_LOGW(TAG, "uuid filter offset+len exceeds 16");
                    continue;
                }
                policy.filter_count++;
            }
            /* All filter entries invalid → fall open rather than block every UUID. */
            if (policy.filter_count == 0) {
                policy.accept_all_uuid = true;
            }
        }
    } else {
        /* No uuid_filters in adapters.json / packs → accept any unprov device.
         * Packs (e.g. esp_ble_mesh_onoff_server) may inject prefix filters later. */
        policy.accept_all_uuid = true;
    }

    if (cJSON_IsArray(bind_models)) {
        cJSON *m = NULL;
        cJSON_ArrayForEach(m, bind_models) {
            if (policy.bind_model_count >= MINIHUB_BM_BIND_MODEL_MAX) {
                break;
            }
            const cJSON *mid = cJSON_IsObject(m) ? cJSON_GetObjectItem(m, "model_id") : m;
            const cJSON *name = cJSON_IsObject(m) ? cJSON_GetObjectItem(m, "name") : NULL;
            uint16_t model_id = 0;
            bool ok = parse_model_id(mid, &model_id);
            if (!ok && cJSON_IsString(name)) {
                ok = parse_model_id(name, &model_id);
            }
            if (!ok) {
                ESP_LOGW(TAG, "skip unknown bind model");
                continue;
            }
            /* Record for AppKey bind; Composition ∩ policy chooses element at runtime. */
            uint16_t cap_bits = 0, client_id = 0;
            if (!minihub_bm_model_cap(model_id, &cap_bits, &client_id)) {
                ESP_LOGW(TAG, "bind model 0x%04x: AppKey bind only (no Hub client for it)",
                         model_id);
            }
            minihub_bm_bind_model_t *bm = &policy.bind_models[policy.bind_model_count];
            bm->model_id = model_id;
            bm->company_id = MINIHUB_BM_CID_SIG;
            const cJSON *cid = cJSON_IsObject(m) ? cJSON_GetObjectItem(m, "company_id") : NULL;
            if (cJSON_IsNumber(cid)) {
                bm->company_id = (uint16_t)cid->valuedouble;
            } else if (cJSON_IsString(cid) && cid->valuestring) {
                char *end = NULL;
                unsigned long v = strtoul(cid->valuestring, &end, 0);
                if (end != cid->valuestring && v <= 0xFFFFUL) {
                    bm->company_id = (uint16_t)v;
                }
            }
            policy.bind_model_count++;
        }
    }

    if (policy.filter_count > 0 || policy.accept_all_uuid || policy.bind_model_count > 0) {
        esp_err_t err = minihub_ble_mesh_set_provision_policy(&policy);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "set provision policy: %s", esp_err_to_name(err));
        }
    }
}

static const char *primary_param_for_type(const char *type)
{
    if (!type || !type[0]) {
        return NULL;
    }
    const minihub_device_widget_device_t *dw = minihub_device_widget_get_or_fetch(NULL, type);
    return (dw && dw->primary_param && dw->primary_param[0]) ? dw->primary_param : NULL;
}

static const char *default_value_for_type(const char *type)
{
    const char *primary = primary_param_for_type(type);
    const minihub_bm_map_param_t *row = primary ? minihub_bm_param_by_name(primary) : NULL;
    return (row && row->init[0]) ? row->init : "";
}

static const char *cid_manufacturer(uint16_t cid)
{
    if (cid == 0x02E5) {
        return "Espressif";
    }
    return NULL;
}

/* Read-modify-write one entry of props.params. Other params and plugin extras are
 * kept (minihub_point_merge_params would replace the whole params object). `val` is
 * owned by this call. `summary` NULL keeps the current point value. */
static void bm_set_param(const char *point_id, const char *summary, const char *name, cJSON *val)
{
    if (!val) {
        return;
    }
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(point_id, &snap) != ESP_OK) {
        cJSON_Delete(val);
        return;
    }
    cJSON *props = snap.props ? snap.props : cJSON_CreateObject();
    if (!props) {
        cJSON_Delete(val);
        return;
    }
    cJSON *params = cJSON_GetObjectItem(props, "params");
    if (!cJSON_IsObject(params)) {
        cJSON_DeleteItemFromObject(props, "params");
        params = cJSON_CreateObject();
        if (params) {
            cJSON_AddItemToObject(props, "params", params);
        }
    }
    cJSON *ent = params ? cJSON_CreateObject() : NULL;
    if (ent) {
        cJSON_AddItemToObject(ent, "value", val);
        val = NULL;
        cJSON_DeleteItemFromObject(params, name);
        cJSON_AddItemToObject(params, name, ent);
    }
    cJSON_Delete(val);
    (void)minihub_point_set(point_id, summary ? summary : snap.value, props);
    cJSON_Delete(props);
}

static bool name_in_list(const char **names, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (names[i] && strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* Create one param with the default the map table carries, and mark read-only rows. */
static void seed_mapped_param(cJSON *params, const char *name)
{
    if (!params || !name || !name[0]) {
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive(params, name)) {
        return;
    }
    const minihub_bm_map_param_t *row = minihub_bm_param_by_name(name);
    cJSON *ent = cJSON_CreateObject();
    if (!ent) {
        return;
    }
    if (row && row->init[0]) {
        if (row->init_is_number) {
            cJSON_AddNumberToObject(ent, "value", strtod(row->init, NULL));
        } else {
            cJSON_AddStringToObject(ent, "value", row->init);
        }
    } else {
        cJSON_AddStringToObject(ent, "value", "");
    }
    if (row && strcmp(row->access, "R") == 0) {
        cJSON_AddStringToObject(ent, "access", "R");
    }
    cJSON_AddItemToObject(params, name, ent);
}

/* Params of one device: the type catalog's params ∩ the map table's params for the
 * node's Composition caps and sensor properties, then every mapped param the
 * catalog does not list. The catalog supplies label / widget / bounds; the table
 * says which params the node exposes. */
static void seed_params_for_dev(cJSON *params, const minihub_ble_mesh_device_t *dev)
{
    if (!params || !dev) {
        return;
    }
    const char *mapped[MINIHUB_BM_MAP_MAX_PARAMS];
    size_t mapped_n = minihub_bm_params_for(dev->caps, dev->props, dev->prop_count, mapped,
                                            MINIHUB_BM_MAP_MAX_PARAMS);

    const minihub_device_widget_device_t *dw =
        dev->type[0] ? minihub_device_widget_get_or_fetch(NULL, dev->type) : NULL;
    if (dw && dw->entries) {
        for (size_t i = 0; i < dw->count; i++) {
            const char *pname = dw->entries[i].parameter;
            if (!pname || !pname[0] || !name_in_list(mapped, mapped_n, pname)) {
                continue;
            }
            seed_mapped_param(params, pname);
        }
    }

    for (size_t m = 0; m < mapped_n; m++) {
        seed_mapped_param(params, mapped[m]);
    }
}

/* Merge Composition identity without replacing owner / ota / params. */
static void apply_identity_props(const minihub_ble_mesh_device_t *dev)
{
    if (!dev || !dev->point_id[0]) {
        return;
    }
    cJSON *patch = cJSON_CreateObject();
    if (!patch) {
        return;
    }
    if (dev->uuid_hex[0]) {
        cJSON_AddStringToObject(patch, "uuid", dev->uuid_hex);
    }
    if (dev->cid) {
        const char *name = cid_manufacturer(dev->cid);
        if (name) {
            cJSON_AddStringToObject(patch, "manufacturer", name);
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%04X", (unsigned)dev->cid);
            cJSON_AddStringToObject(patch, "manufacturer", buf);
        }
        char pid[8];
        snprintf(pid, sizeof(pid), "0x%04X", (unsigned)dev->pid);
        cJSON_AddStringToObject(patch, "model", pid);
        char vid[8];
        snprintf(vid, sizeof(vid), "0x%04X", (unsigned)dev->vid);
        cJSON_AddStringToObject(patch, "sw_version", vid);
    }
    if (!patch->child) {
        cJSON_Delete(patch);
        return;
    }
    (void)minihub_point_merge_props(dev->point_id, NULL, patch);
    cJSON_Delete(patch);
}

static void register_point_from_dev(const minihub_ble_mesh_device_t *dev)
{
    if (!dev || !dev->point_id[0]) {
        return;
    }
    /* Keep live props.params across re-register; seed only the names caps expose. */
    minihub_point_t snap = {0};
    cJSON *seed = NULL;
    if (minihub_point_get_snapshot(dev->point_id, &snap) == ESP_OK && snap.props) {
        const cJSON *p = cJSON_GetObjectItem(snap.props, "params");
        if (cJSON_IsObject(p)) {
            seed = cJSON_Duplicate(p, true);
        }
    }
    if (!seed) {
        seed = cJSON_CreateObject();
    }
    if (seed) {
        seed_params_for_dev(seed, dev);
    }

    minihub_point_t point = {0};
    strncpy(point.point_id, dev->point_id, sizeof(point.point_id) - 1);
    strncpy(point.type, dev->type, sizeof(point.type) - 1);
    strncpy(point.adapter, "ble_mesh", sizeof(point.adapter) - 1);
    strncpy(point.value, default_value_for_type(dev->type), sizeof(point.value) - 1);
    point.online = true;
    point.props = cJSON_CreateObject();
    if (point.props && seed) {
        cJSON_AddItemToObject(point.props, "params", seed);
        seed = NULL;
    }
    minihub_point_register(&point);
    if (point.props) {
        cJSON_Delete(point.props);
    }
    cJSON_Delete(seed);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
    apply_identity_props(dev);

    minihub_pp_bm_param_t params[MINIHUB_PP_MAX_PARAMS];
    size_t n = minihub_pp_ble_mesh_list_params(dev->uuid_hex, params, MINIHUB_PP_MAX_PARAMS);
    if (n > 0) {
        ESP_LOGI(TAG, "product_params matched %s n=%u", dev->point_id, (unsigned)n);
    }
}

static void on_bm_device(const minihub_ble_mesh_device_t *dev, void *ctx)
{
    (void)ctx;
    register_point_from_dev(dev);
}

/* Forward the raw mesh state to the per-product quirk layer. The (model id,
 * field) pairs are the published quirk vocabulary, so they stay protocol
 * constants here: an OEM editing controls[] must not move a quirk's key. */
static void try_product_params_attr(const minihub_ble_mesh_device_t *dev,
                                    const minihub_ble_mesh_attr_evt_t *evt)
{
    if (!dev || !evt || !dev->uuid_hex[0]) {
        return;
    }
    uint16_t model_id = 0;
    const char *field = NULL;
    double raw = 0;
    switch (evt->kind) {
    case MINIHUB_BM_ATTR_ONOFF:
        model_id = 0x1000;
        field = "onoff";
        raw = evt->u.onoff ? 1.0 : 0.0;
        break;
    case MINIHUB_BM_ATTR_LEVEL:
        model_id = 0x1002;
        field = "level";
        raw = (double)evt->u.level;
        break;
    case MINIHUB_BM_ATTR_LIGHTNESS:
        model_id = 0x1300;
        field = "lightness";
        raw = (double)evt->u.lightness;
        break;
    case MINIHUB_BM_ATTR_CTL:
        model_id = 0x1303;
        field = "cct";
        raw = (double)evt->u.ctl.temperature;
        (void)minihub_pp_ble_mesh_apply(dev->point_id, dev->uuid_hex, model_id, MINIHUB_BM_CID_SIG, 0,
                                        "lightness", (double)evt->u.ctl.lightness);
        break;
    case MINIHUB_BM_ATTR_HSL:
        model_id = 0x1307;
        field = "hue";
        raw = (double)evt->u.hsl.hue;
        (void)minihub_pp_ble_mesh_apply(dev->point_id, dev->uuid_hex, model_id, MINIHUB_BM_CID_SIG, 0,
                                        "saturation", (double)evt->u.hsl.saturation);
        (void)minihub_pp_ble_mesh_apply(dev->point_id, dev->uuid_hex, model_id, MINIHUB_BM_CID_SIG, 0,
                                        "lightness", (double)evt->u.hsl.lightness);
        break;
    case MINIHUB_BM_ATTR_SENSOR: {
        const minihub_bm_sensor_t *row = minihub_bm_sensor_by_property(evt->u.sensor.property_id);
        if (!row || !row->param[0]) {
            return;
        }
        model_id = 0x1100;
        field = row->param;
        raw = evt->u.sensor.value;
        break;
    }
    default:
        return;
    }
    (void)minihub_pp_ble_mesh_apply(dev->point_id, dev->uuid_hex, model_id, MINIHUB_BM_CID_SIG, 0, field,
                                    raw);
}

/* The control row that drives one message field of a given kind. */
static const char *bm_control_param_for_field(minihub_bm_ctrl_kind_t kind, const char *field)
{
    size_t n = minihub_bm_control_count();
    for (size_t i = 0; i < n; i++) {
        const minihub_bm_map_control_t *c = minihub_bm_control_at(i);
        if (c && c->kind == kind && c->field[0] && strcmp(c->field, field) == 0) {
            return c->param;
        }
    }
    return NULL;
}

/* OnOff is one value in two places: the point summary and the parameter the
 * console's toggle reads. Every command and report path publishes both. */
static void bm_echo_onoff(const char *point_id, bool on, const char *pname)
{
    const char *state = on ? "on" : "off";
    if (pname) {
        bm_set_param(point_id, state, pname, cJSON_CreateString(state));
    } else {
        (void)minihub_point_set(point_id, state, NULL);
    }
}

/* Publish the HSL state the node reported: hue in degrees, saturation and
 * lightness in percent (Mesh Lighting Model §6.1.4). */
static void bm_report_hsl(const minihub_ble_mesh_device_t *dev, uint16_t raw_hue,
                          uint16_t raw_sat, uint16_t raw_lightness)
{
    double hue = (double)(((uint32_t)raw_hue * 360u + 32767u) / 65535u);
    double sat = (double)(((uint32_t)raw_sat * 100u + 32767u) / 65535u);
    double bri = (double)(((uint32_t)raw_lightness * 100u + 32767u) / 65535u);
    const char *hue_p = bm_control_param_for_field(MINIHUB_BM_CTRL_HSL, "hue");
    const char *sat_p = bm_control_param_for_field(MINIHUB_BM_CTRL_HSL, "saturation");
    const char *bri_p = bm_control_param_for_field(MINIHUB_BM_CTRL_LIGHTNESS, "lightness");
    if (hue_p) {
        bm_set_param(dev->point_id, NULL, hue_p, cJSON_CreateNumber(hue));
    }
    if (sat_p) {
        bm_set_param(dev->point_id, NULL, sat_p, cJSON_CreateNumber(sat));
    }
    if (bri_p) {
        bm_set_param(dev->point_id, NULL, bri_p, cJSON_CreateNumber(bri));
    }
}

static void on_bm_attr(const minihub_ble_mesh_attr_evt_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) {
        return;
    }
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_unicast(evt->unicast);
    if (!dev) {
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_ONOFF) {
        bm_echo_onoff(dev->point_id, evt->u.onoff,
                      bm_control_param_for_field(MINIHUB_BM_CTRL_ONOFF, "onoff"));
        try_product_params_attr(dev, evt);
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_LIGHTNESS) {
        int pct = (int)((evt->u.lightness * 100u) / 65535u);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pct);
        const char *pname = bm_control_param_for_field(MINIHUB_BM_CTRL_LIGHTNESS, "lightness");
        if (pname) {
            bm_set_param(dev->point_id, buf, pname, cJSON_CreateNumber(pct));
        }
        try_product_params_attr(dev, evt);
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_LEVEL) {
        /* Map signed level to 0..100 brightness-ish for Hub value. */
        int pct = (int)(((int)evt->u.level + 32768) * 100 / 65535);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pct);
        const char *pname = bm_control_param_for_field(MINIHUB_BM_CTRL_LIGHTNESS, "lightness");
        if (pname) {
            bm_set_param(dev->point_id, buf, pname, cJSON_CreateNumber(pct));
        }
        try_product_params_attr(dev, evt);
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_CTL) {
        int pct = (int)((evt->u.ctl.lightness * 100u) / 65535u);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pct);
        const char *bri_p = bm_control_param_for_field(MINIHUB_BM_CTRL_LIGHTNESS, "lightness");
        const char *cct_p = bm_control_param_for_field(MINIHUB_BM_CTRL_CTL, "temperature");
        if (bri_p) {
            bm_set_param(dev->point_id, buf, bri_p, cJSON_CreateNumber(pct));
        }
        if (cct_p) {
            bm_set_param(dev->point_id, NULL, cct_p, cJSON_CreateNumber(evt->u.ctl.temperature));
        }
        try_product_params_attr(dev, evt);
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_HSL) {
        /* Mesh HSL state is 0–65535 (Mesh Lighting Model §6.1.4); the catalog
         * works in degrees and percent. */
        bm_report_hsl(dev, evt->u.hsl.hue, evt->u.hsl.saturation, evt->u.hsl.lightness);
        try_product_params_attr(dev, evt);
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_SENSOR) {
        const minihub_bm_sensor_t *row = minihub_bm_sensor_by_property(evt->u.sensor.property_id);
        if (!row || !row->param[0]) {
            return;
        }
        char buf[24];
        snprintf(buf, sizeof(buf), "%g", evt->u.sensor.value);
        const char *primary = primary_param_for_type(dev->type);
        const char *summary = (primary && strcmp(primary, row->param) == 0) ? buf : NULL;
        bm_set_param(dev->point_id, summary, row->param, cJSON_CreateNumber(evt->u.sensor.value));
        try_product_params_attr(dev, evt);
        return;
    }
}

/* A config declaration carries no Composition data, so the type describes
 * itself: its catalog params give the mesh caps, its classify row gives the
 * sensor properties. A type the tables do not describe yields caps 0. */
static void caps_for_type(const char *type, uint16_t *caps, uint16_t *props, size_t *prop_count)
{
    const minihub_device_widget_device_t *dw = minihub_device_widget_get_or_fetch(NULL, type);
    if (dw && dw->entries) {
        for (size_t i = 0; i < dw->count; i++) {
            const minihub_bm_map_param_t *row = minihub_bm_param_by_name(dw->entries[i].parameter);
            if (row) {
                *caps |= row->cap_bit;
            }
        }
    }
    for (size_t i = 0; i < minihub_bm_classify_count(); i++) {
        const minihub_bm_map_classify_t *r = minihub_bm_classify_at(i);
        if (!r || strcmp(r->type, type) != 0) {
            continue;
        }
        for (size_t k = 0; k < r->all_prop_count && *prop_count < MINIHUB_BM_MAX_PROPS; k++) {
            props[(*prop_count)++] = r->all_props[k];
        }
        for (size_t k = 0; k < r->any_prop_count && *prop_count < MINIHUB_BM_MAX_PROPS; k++) {
            props[(*prop_count)++] = r->any_props[k];
        }
        if (*prop_count > 0) {
            *caps |= MINIHUB_BM_CAP_SENSOR;
        }
        break;
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
        const cJSON *uuid = cJSON_GetObjectItem(item, "uuid");
        const cJSON *unicast = cJSON_GetObjectItem(item, "unicast");
        const cJSON *elem_num = cJSON_GetObjectItem(item, "elem_num");
        const cJSON *models = cJSON_GetObjectItem(item, "models");
        if (!cJSON_IsString(point_id)) {
            continue;
        }
        bool models_given = cJSON_IsArray(models);
        uint16_t caps = 0;
        if (models_given) {
            const cJSON *m = NULL;
            cJSON_ArrayForEach(m, models) {
                uint16_t model_id = 0;
                if (!parse_model_id(m, &model_id)) {
                    ESP_LOGW(TAG, "%s: unreadable model id — ignored", point_id->valuestring);
                    continue;
                }
                uint16_t cap_bits = 0, client_id = 0;
                if (!minihub_bm_model_cap(model_id, &cap_bits, &client_id)) {
                    ESP_LOGW(TAG, "%s: model 0x%04x is not in the map table — ignored",
                             point_id->valuestring, (unsigned)model_id);
                    continue;
                }
                caps |= cap_bits;
            }
            if (caps == 0) {
                ESP_LOGW(TAG, "%s: no known model — not registered", point_id->valuestring);
                continue;
            }
        }
        if (!cJSON_IsString(type) && !models_given) {
            continue;
        }
        minihub_ble_mesh_device_t dev = {0};
        strncpy(dev.point_id, point_id->valuestring, sizeof(dev.point_id) - 1);
        (void)minihub_bm_normalize_point_id(dev.point_id, sizeof(dev.point_id));
        if (cJSON_IsString(type)) {
            strncpy(dev.type, type->valuestring, sizeof(dev.type) - 1);
        } else {
            const char *classified = minihub_bm_classify(caps, NULL, 0);
            if (!classified) {
                ESP_LOGW(TAG, "%s: no classify rule for caps 0x%04x — not registered",
                         point_id->valuestring, (unsigned)caps);
                continue;
            }
            strncpy(dev.type, classified, sizeof(dev.type) - 1);
        }
        if (cJSON_IsString(uuid)) {
            strncpy(dev.uuid_hex, uuid->valuestring, sizeof(dev.uuid_hex) - 1);
        }
        if (cJSON_IsNumber(unicast)) {
            dev.unicast = (uint16_t)unicast->valuedouble;
        }
        if (cJSON_IsNumber(elem_num)) {
            dev.elem_num = (uint8_t)elem_num->valuedouble;
        } else {
            dev.elem_num = 1;
        }
        uint16_t type_caps = 0;
        size_t prop_count = 0;
        caps_for_type(dev.type, &type_caps, dev.props, &prop_count);
        dev.prop_count = prop_count;
        dev.caps = models_given ? caps : type_caps;
        if (dev.caps == 0) {
            ESP_LOGW(TAG, "%s: type '%s' exposes no mapped parameter — not registered",
                     point_id->valuestring, dev.type);
            continue;
        }
        (void)minihub_ble_mesh_upsert_device(&dev);
    }
    return ESP_OK;
}

static esp_err_t ble_mesh_adapter_init(const cJSON *config)
{
    esp_err_t err = minihub_adapter_check_core_api(1, 1);
    if (err != ESP_OK) {
        return err;
    }
    s_config = config;
    (void)minihub_product_params_init();
    (void)minihub_ble_mesh_init();
    apply_provision_policy(config);
    apply_config_binds(config);
    (void)minihub_module_register_rpc("ble_mesh", ble_mesh_module_rpc);
    return ESP_OK;
}

static void ble_mesh_register_mapped_points(void)
{
    minihub_ble_mesh_device_t list[MINIHUB_BM_MAX_DEVICES];
    size_t n = 0;
    if (minihub_ble_mesh_list_devices(list, MINIHUB_BM_MAX_DEVICES, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            register_point_from_dev(&list[i]);
        }
    }

#if !CONFIG_MINIHUB_BLE_MESH_SDK
    if (n == 0) {
        minihub_ble_mesh_device_t light = {0};
        strncpy(light.point_id, "ble_mesh.ble_mesh_light", sizeof(light.point_id) - 1);
        strncpy(light.type, "light", sizeof(light.type) - 1);
        light.caps = MINIHUB_BM_CAP_ONOFF;
        light.elem_num = 1;
        register_point_from_dev(&light);

        minihub_ble_mesh_device_t sw = {0};
        strncpy(sw.point_id, "ble_mesh.ble_mesh_plug", sizeof(sw.point_id) - 1);
        strncpy(sw.type, "switch", sizeof(sw.type) - 1);
        sw.caps = MINIHUB_BM_CAP_ONOFF;
        sw.elem_num = 1;
        register_point_from_dev(&sw);
        n = 2;
    }
#endif
}

static esp_err_t ble_mesh_adapter_start(void)
{
    minihub_ble_mesh_set_callbacks(on_bm_device, on_bm_attr, NULL);

    /* Register before the stack's boot Get. Otherwise a fast Status is applied
     * to a point that does not exist yet, and the following register writes the
     * map default ("off" / 100) over the real state. */
    ble_mesh_register_mapped_points();
#if CONFIG_MINIHUB_BLE_MESH_SDK
    /* Host (main) sequences Zigbee-before-BLE when both are enabled; start here immediately. */
    minihub_heap_log();
    esp_err_t err = minihub_ble_mesh_start();
    minihub_heap_log();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Host stack start: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGI(TAG, "simulation mode (CONFIG_MINIHUB_BLE_MESH_SDK=n)");
#endif
    if (minihub_ble_mesh_running()) {
        ESP_LOGI(TAG, "ble_mesh adapter started");
    } else {
        ESP_LOGW(TAG, "ble_mesh adapter up but stack not running");
    }

    (void)s_config;
    return ESP_OK;
}

static esp_err_t ble_mesh_adapter_stop(void)
{
    return minihub_ble_mesh_stop();
}

static esp_err_t ble_mesh_adapter_poll(void)
{
    return ESP_OK;
}

#if CONFIG_MINIHUB_BLE_MESH_SDK
static int bm_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The map table owns every param default; a row without a numeric init has none. */
static double bm_param_init(const char *name)
{
    const minihub_bm_map_param_t *row = name ? minihub_bm_param_by_name(name) : NULL;
    return (row && row->init_is_number) ? strtod(row->init, NULL) : 0.0;
}

static double bm_param_number(const char *point_id, const char *name)
{
    minihub_point_t snap = {0};
    double out = bm_param_init(name);
    if (minihub_point_get_snapshot(point_id, &snap) == ESP_OK) {
        const cJSON *p = snap.props ? cJSON_GetObjectItem(snap.props, "params") : NULL;
        const cJSON *ent = cJSON_IsObject(p) ? cJSON_GetObjectItem(p, name) : NULL;
        const cJSON *v = cJSON_IsObject(ent) ? cJSON_GetObjectItem(ent, "value") : NULL;
        if (cJSON_IsNumber(v)) {
            out = v->valuedouble;
        }
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
    }
    return out;
}

/* Catalog bounds for one param (D2: clamp to the catalog, never widen it). */
typedef struct {
    double min;
    double max;
    bool has_min;
    bool has_max;
} bm_range_t;

static void bm_catalog_range(const char *type, const char *param, bm_range_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!type || !param || !param[0]) {
        return;
    }
    const minihub_device_widget_device_t *dw = minihub_device_widget_get_or_fetch(NULL, type);
    if (!dw) {
        return;
    }
    const minihub_device_widget_entry_t *e = minihub_device_widget_find(dw->device, param);
    if (!e || !e->style_json) {
        return;
    }
    cJSON *style = cJSON_Parse(e->style_json);
    if (!style) {
        return;
    }
    const cJSON *mn = cJSON_GetObjectItem(style, "min");
    const cJSON *mx = cJSON_GetObjectItem(style, "max");
    if (cJSON_IsNumber(mn)) {
        out->min = mn->valuedouble;
        out->has_min = true;
    }
    if (cJSON_IsNumber(mx)) {
        out->max = mx->valuedouble;
        out->has_max = true;
    }
    cJSON_Delete(style);
}

/* Clamp a written value to the catalog range. A written 0 stays 0: the catalog
 * lower bound is the smallest displayable value, and 0 means "off". */
static double bm_clamp_write(double v, const bm_range_t *r)
{
    if (v == 0.0) {
        return 0.0;
    }
    if (r->has_min && v < r->min) {
        v = r->min;
    }
    if (r->has_max && v > r->max) {
        v = r->max;
    }
    return v;
}

/* Web / rules write one param: {"parameter": name, "value": v} → one Mesh message.
 * The map table decides which writer a param goes to. */
static esp_err_t light_set_param(const minihub_ble_mesh_device_t *dev, const char *point_id,
                                 const char *param, const cJSON *value)
{
    const minihub_bm_map_control_t *ctrl = minihub_bm_control_for_param(param);
    if (!ctrl) {
        ESP_LOGW(TAG, "parameter '%s' not in control map on %s", param, point_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (ctrl->kind == MINIHUB_BM_CTRL_ONOFF) {
        bool on = false;
        if (cJSON_IsBool(value)) {
            on = cJSON_IsTrue(value);
        } else if (cJSON_IsNumber(value)) {
            on = value->valuedouble != 0.0;
        } else if (cJSON_IsString(value)) {
            on = strcmp(value->valuestring, "on") == 0 || strcmp(value->valuestring, "1") == 0 ||
                 strcmp(value->valuestring, "true") == 0;
        }
        esp_err_t err = minihub_ble_mesh_onoff(dev->unicast, on ? MINIHUB_BM_ONOFF_ON : MINIHUB_BM_ONOFF_OFF);
        if (err == ESP_OK) {
            bm_echo_onoff(point_id, on, ctrl->param);
        }
        return err;
    }

    if (!cJSON_IsNumber(value)) {
        ESP_LOGW(TAG, "parameter '%s' on %s needs a numeric value", param, point_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (ctrl->kind == MINIHUB_BM_CTRL_LIGHTNESS || ctrl->kind == MINIHUB_BM_CTRL_LEVEL) {
        minihub_bm_ctrl_kind_t use = MINIHUB_BM_CTRL_NONE;
        if (ctrl->kind == MINIHUB_BM_CTRL_LIGHTNESS && (dev->caps & MINIHUB_BM_CAP_LIGHTNESS)) {
            use = MINIHUB_BM_CTRL_LIGHTNESS;
        } else if (ctrl->kind == MINIHUB_BM_CTRL_LEVEL && (dev->caps & MINIHUB_BM_CAP_LEVEL)) {
            use = MINIHUB_BM_CTRL_LEVEL;
        } else if (ctrl->fallback == MINIHUB_BM_CTRL_LEVEL && (dev->caps & MINIHUB_BM_CAP_LEVEL)) {
            use = MINIHUB_BM_CTRL_LEVEL;
        } else if (ctrl->fallback == MINIHUB_BM_CTRL_LIGHTNESS && (dev->caps & MINIHUB_BM_CAP_LIGHTNESS)) {
            use = MINIHUB_BM_CTRL_LIGHTNESS;
        }
        if (use == MINIHUB_BM_CTRL_NONE) {
            ESP_LOGW(TAG, "parameter '%s' on %s has no lightness/level model", param, point_id);
            return ESP_ERR_NOT_SUPPORTED;
        }
        bm_range_t range;
        bm_catalog_range(dev->type, ctrl->param, &range);
        double pct = bm_clamp_write(value->valuedouble, &range);
        esp_err_t err;
        if (use == MINIHUB_BM_CTRL_LIGHTNESS) {
            err = minihub_ble_mesh_lightness(dev->unicast, (uint16_t)(((pct * 65535.0) / 100.0) + 0.5));
        } else {
            err = minihub_ble_mesh_level(dev->unicast, (int16_t)((pct * 65535.0 / 100.0) - 32768.0));
        }
        if (err == ESP_OK) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", (int)pct);
            bm_set_param(point_id, buf, ctrl->param, cJSON_CreateNumber(pct));
        }
        return err;
    }

    if (ctrl->kind == MINIHUB_BM_CTRL_CTL) {
        if (!(dev->caps & MINIHUB_BM_CAP_CTL)) {
            ESP_LOGW(TAG, "parameter '%s' on %s has no CTL model", param, point_id);
            return ESP_ERR_NOT_SUPPORTED;
        }
        bm_range_t range;
        bm_catalog_range(dev->type, ctrl->param, &range);
        double kelvin = bm_clamp_write(value->valuedouble, &range);
        const char *bri_param = bm_control_param_for_field(MINIHUB_BM_CTRL_LIGHTNESS, "lightness");
        double bri = bm_param_number(point_id, bri_param);
        /* Percentage → 65535 scale, not a catalog write: a dark light is 0 here
         * even though the catalog's display min is 1. */
        bri = bm_clamp((int)bri, 0, 100);
        esp_err_t err = minihub_ble_mesh_ctl(dev->unicast, (uint16_t)(((bri * 65535.0) / 100.0) + 0.5),
                                             (uint16_t)kelvin, 0);
        if (err == ESP_OK) {
            bm_set_param(point_id, NULL, ctrl->param, cJSON_CreateNumber(kelvin));
        }
        return err;
    }

    if (ctrl->kind == MINIHUB_BM_CTRL_HSL) {
        if (!(dev->caps & MINIHUB_BM_CAP_HSL)) {
            ESP_LOGW(TAG, "parameter '%s' on %s has no HSL model", param, point_id);
            return ESP_ERR_NOT_SUPPORTED;
        }
        const char *hue_param = bm_control_param_for_field(MINIHUB_BM_CTRL_HSL, "hue");
        const char *sat_param = bm_control_param_for_field(MINIHUB_BM_CTRL_HSL, "saturation");
        const char *bri_param = bm_control_param_for_field(MINIHUB_BM_CTRL_LIGHTNESS, "lightness");
        double hue = bm_param_number(point_id, hue_param);
        double sat = bm_param_number(point_id, sat_param);
        double bri = bm_param_number(point_id, bri_param);
        bm_range_t range;
        bm_catalog_range(dev->type, ctrl->param, &range);
        double v = bm_clamp_write(value->valuedouble, &range);
        if (strcmp(ctrl->field, "hue") == 0) {
            hue = v;
        } else if (strcmp(ctrl->field, "saturation") == 0) {
            sat = v;
        } else {
            ESP_LOGW(TAG, "parameter '%s' on %s has no HSL field mapping", param, point_id);
            return ESP_ERR_NOT_SUPPORTED;
        }
        bri = bm_clamp((int)bri, 0, 100);
        esp_err_t err = minihub_ble_mesh_hsl(dev->unicast,
                                              (uint16_t)(((hue * 65535.0) / 360.0) + 0.5),
                                              (uint16_t)(((sat * 65535.0) / 100.0) + 0.5),
                                              (uint16_t)(((bri * 65535.0) / 100.0) + 0.5));
        if (err == ESP_OK) {
            /* The node reports the HSL state back; the echo only covers nodes
             * that publish no status. */
            bm_set_param(point_id, NULL, ctrl->param, cJSON_CreateNumber(v));
        }
        return err;
    }

    ESP_LOGW(TAG, "parameter '%s' on %s has no mesh writer", param, point_id);
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

static esp_err_t ble_mesh_adapter_handle_command(const minihub_command_t *cmd)
{
    if (cmd && strcmp(cmd->cmd, "set_prop") == 0) {
        const cJSON *prop = cmd->params ? cJSON_GetObjectItem(cmd->params, "prop") : NULL;
        const cJSON *value = cmd->params ? cJSON_GetObjectItem(cmd->params, "value") : NULL;
        if (!cJSON_IsString(prop) || !value) {
            return ESP_ERR_INVALID_ARG;
        }
        /* v1: validate access + local echo; opcode write lands later. */
        return minihub_pp_set_prop(cmd->point_id, prop->valuestring, value, NULL, NULL);
    }

    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point(cmd->point_id);
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(cmd->cmd, "ota") == 0 || strcmp(cmd->cmd, "start_ota") == 0) {
        const char *path = NULL;
        const cJSON *pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "path") : NULL;
        if (!cJSON_IsString(pj) || !pj->valuestring) {
            pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "file_path") : NULL;
        }
        if (!cJSON_IsString(pj) || !pj->valuestring) {
            pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "url") : NULL;
        }
        if (cJSON_IsString(pj) && pj->valuestring) {
            path = pj->valuestring;
        }
        return minihub_ble_mesh_ota_offer(cmd->point_id, path);
    }

#if CONFIG_MINIHUB_BLE_MESH_SDK
    if (dev->unicast == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Web sliders / switches and rules: {"parameter": name, "value": v}. */
    const cJSON *pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "parameter") : NULL;
    if (cJSON_IsString(pj) && pj->valuestring && pj->valuestring[0]) {
        const cJSON *val = cJSON_GetObjectItem(cmd->params, "value");
        if (!val) {
            return ESP_ERR_INVALID_ARG;
        }
        return light_set_param(dev, cmd->point_id, pj->valuestring, val);
    }

    if (strcmp(cmd->cmd, "set_brightness") == 0) {
        const char *pname = bm_control_param_for_field(MINIHUB_BM_CTRL_LIGHTNESS, "lightness");
        if (!pname) {
            ESP_LOGW(TAG, "command 'set_brightness' has no brightness control on %s", cmd->point_id);
            return ESP_ERR_NOT_SUPPORTED;
        }
        double bright = bm_param_init(pname);
        if (cmd->params) {
            const cJSON *v = cJSON_GetObjectItem(cmd->params, "brightness");
            if (cJSON_IsNumber(v)) {
                bright = v->valuedouble;
            }
        }
        cJSON *val = cJSON_CreateNumber(bright);
        esp_err_t err = light_set_param(dev, cmd->point_id, pname, val);
        cJSON_Delete(val);
        return err;
    }

    if (strcmp(cmd->cmd, "set_color_temp") == 0 || strcmp(cmd->cmd, "set_ct") == 0) {
        const char *pname = bm_control_param_for_field(MINIHUB_BM_CTRL_CTL, "temperature");
        if (!pname) {
            ESP_LOGW(TAG, "command '%s' has no color-temperature control on %s", cmd->cmd, cmd->point_id);
            return ESP_ERR_NOT_SUPPORTED;
        }
        double temp = bm_param_init(pname);
        if (cmd->params) {
            const cJSON *t = cJSON_GetObjectItem(cmd->params, "cct");
            if (cJSON_IsNumber(t)) {
                temp = t->valuedouble;
            }
        }
        cJSON *val = cJSON_CreateNumber(temp);
        esp_err_t err = light_set_param(dev, cmd->point_id, pname, val);
        cJSON_Delete(val);
        return err;
    }
#endif

    if (!(dev->caps & MINIHUB_BM_CAP_ONOFF)) {
        ESP_LOGW(TAG, "command '%s' on %s: type '%s' has no on/off model", cmd->cmd, cmd->point_id,
                 dev->type);
        return ESP_ERR_NOT_SUPPORTED;
    }

#if CONFIG_MINIHUB_BLE_MESH_SDK
    minihub_ble_mesh_onoff_cmd_t bm_cmd;
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        bm_cmd = MINIHUB_BM_ONOFF_ON;
    } else if (strcmp(cmd->cmd, "turn_off") == 0) {
        bm_cmd = MINIHUB_BM_ONOFF_OFF;
    } else if (strcmp(cmd->cmd, "toggle") == 0) {
        bm_cmd = MINIHUB_BM_ONOFF_TOGGLE;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (dev->unicast == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = minihub_ble_mesh_onoff(dev->unicast, bm_cmd);
    if (err != ESP_OK) {
        return err;
    }
#endif

    const char *onoff_param = bm_control_param_for_field(MINIHUB_BM_CTRL_ONOFF, "onoff");
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        bm_echo_onoff(cmd->point_id, true, onoff_param);
        return ESP_OK;
    }
    if (strcmp(cmd->cmd, "turn_off") == 0) {
        bm_echo_onoff(cmd->point_id, false, onoff_param);
        return ESP_OK;
    }
    if (strcmp(cmd->cmd, "toggle") == 0) {
        minihub_point_t snap = {0};
        minihub_point_get_snapshot(cmd->point_id, &snap);
        bool next = strcmp(snap.value, "on") != 0;
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        bm_echo_onoff(cmd->point_id, next, onoff_param);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "command '%s' not supported on %s", cmd->cmd, cmd->point_id);
    return ESP_ERR_NOT_SUPPORTED;
}

const minihub_adapter_t ble_mesh_adapter = {
    .name = "ble_mesh",
    .version = "0.2.0",
    .init = ble_mesh_adapter_init,
    .start = ble_mesh_adapter_start,
    .stop = ble_mesh_adapter_stop,
    .poll = ble_mesh_adapter_poll,
    .handle_command = ble_mesh_adapter_handle_command,
};

REGISTER_ADAPTER(ble_mesh_adapter);
