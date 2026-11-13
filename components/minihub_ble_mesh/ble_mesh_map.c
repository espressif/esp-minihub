/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_mesh_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "bm_map";

/* The host tests redirect the overlay into their own build tree, so they never
 * read or seed anything under /data on the machine running them. */
#ifndef MINIHUB_BM_MAP_DATA_PATH
#define MINIHUB_BM_MAP_DATA_PATH "/data/config/ble_mesh_map.json"
#endif

/* Embedded via CMake EMBED_TXTFILES "config/ble_mesh_map.json"
 * (symbol uses the file basename, not the config/ prefix). */
extern const uint8_t ble_mesh_map_json_start[] asm("_binary_ble_mesh_map_json_start");
extern const uint8_t ble_mesh_map_json_end[] asm("_binary_ble_mesh_map_json_end");

static minihub_bm_map_model_t s_models[MINIHUB_BM_MAP_MAX_MODELS];
static size_t s_model_count;
static minihub_bm_map_classify_t s_classify[MINIHUB_BM_MAP_MAX_CLASSIFY];
static size_t s_classify_count;
static minihub_bm_map_param_t s_params[MINIHUB_BM_MAP_MAX_PARAMS];
static size_t s_param_count;
static minihub_bm_map_control_t s_controls[MINIHUB_BM_MAP_MAX_CTRLS];
static size_t s_control_count;
static minihub_bm_sensor_t s_sensors[MINIHUB_BM_MAP_MAX_SENSORS];
static size_t s_sensor_count;
static minihub_bm_report_t s_report;
static bool s_inited;

static uint16_t json_u16(const cJSON *v)
{
    if (cJSON_IsString(v) && v->valuestring) {
        return (uint16_t)strtoul(v->valuestring, NULL, 0);
    }
    if (cJSON_IsNumber(v)) {
        return (uint16_t)v->valuedouble;
    }
    return 0;
}

static bool json_i64(const cJSON *v, int64_t *out)
{
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        *out = (int64_t)strtoll(v->valuestring, NULL, 0);
        return true;
    }
    if (cJSON_IsNumber(v)) {
        *out = (int64_t)v->valuedouble;
        return true;
    }
    return false;
}

static uint16_t cap_bit_from_name(const char *name)
{
    if (!name || !name[0]) {
        return 0;
    }
    if (strcmp(name, "onoff") == 0) {
        return MINIHUB_BM_CAP_ONOFF;
    }
    if (strcmp(name, "level") == 0) {
        return MINIHUB_BM_CAP_LEVEL;
    }
    if (strcmp(name, "lightness") == 0) {
        return MINIHUB_BM_CAP_LIGHTNESS;
    }
    if (strcmp(name, "ctl") == 0) {
        return MINIHUB_BM_CAP_CTL;
    }
    if (strcmp(name, "hsl") == 0) {
        return MINIHUB_BM_CAP_HSL;
    }
    if (strcmp(name, "sensor") == 0) {
        return MINIHUB_BM_CAP_SENSOR;
    }
    /* Unknown names (e.g. vendor models) → no CAP bit; the row stays inert. */
    return 0;
}

static minihub_bm_ctrl_kind_t ctrl_kind_from_name(const char *name)
{
    if (!name || !name[0]) {
        return MINIHUB_BM_CTRL_NONE;
    }
    if (strcmp(name, "onoff") == 0) {
        return MINIHUB_BM_CTRL_ONOFF;
    }
    if (strcmp(name, "level") == 0) {
        return MINIHUB_BM_CTRL_LEVEL;
    }
    if (strcmp(name, "lightness") == 0) {
        return MINIHUB_BM_CTRL_LIGHTNESS;
    }
    if (strcmp(name, "ctl") == 0) {
        return MINIHUB_BM_CTRL_CTL;
    }
    if (strcmp(name, "hsl") == 0) {
        return MINIHUB_BM_CTRL_HSL;
    }
    return MINIHUB_BM_CTRL_NONE;
}

static void clear_tables(void)
{
    memset(s_models, 0, sizeof(s_models));
    s_model_count = 0;
    memset(s_classify, 0, sizeof(s_classify));
    s_classify_count = 0;
    memset(s_params, 0, sizeof(s_params));
    s_param_count = 0;
    memset(s_controls, 0, sizeof(s_controls));
    s_control_count = 0;
    memset(s_sensors, 0, sizeof(s_sensors));
    s_sensor_count = 0;
    memset(&s_report, 0, sizeof(s_report));
}

static void upsert_model(const minihub_bm_map_model_t *in)
{
    for (size_t i = 0; i < s_model_count; i++) {
        if (s_models[i].model_id == in->model_id) {
            s_models[i] = *in;
            return;
        }
    }
    if (s_model_count >= MINIHUB_BM_MAP_MAX_MODELS) {
        ESP_LOGW(TAG, "model table full");
        return;
    }
    s_models[s_model_count++] = *in;
}

static void upsert_classify(const minihub_bm_map_classify_t *in)
{
    for (size_t i = 0; i < s_classify_count; i++) {
        if (strcmp(s_classify[i].type, in->type) == 0) {
            s_classify[i] = *in;
            return;
        }
    }
    if (s_classify_count >= MINIHUB_BM_MAP_MAX_CLASSIFY) {
        ESP_LOGW(TAG, "classify table full");
        return;
    }
    s_classify[s_classify_count++] = *in;
}

static void upsert_param(const minihub_bm_map_param_t *in)
{
    for (size_t i = 0; i < s_param_count; i++) {
        if (s_params[i].cap_bit == in->cap_bit && s_params[i].property_id == in->property_id &&
            strcmp(s_params[i].param, in->param) == 0) {
            s_params[i] = *in;
            return;
        }
    }
    if (s_param_count >= MINIHUB_BM_MAP_MAX_PARAMS) {
        ESP_LOGW(TAG, "param table full");
        return;
    }
    s_params[s_param_count++] = *in;
}

static void upsert_control(const minihub_bm_map_control_t *in)
{
    for (size_t i = 0; i < s_control_count; i++) {
        if (strcmp(s_controls[i].param, in->param) == 0) {
            s_controls[i] = *in;
            return;
        }
    }
    if (s_control_count >= MINIHUB_BM_MAP_MAX_CTRLS) {
        ESP_LOGW(TAG, "control table full");
        return;
    }
    s_controls[s_control_count++] = *in;
}

static void upsert_sensor(const minihub_bm_sensor_t *in)
{
    for (size_t i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].property_id == in->property_id) {
            s_sensors[i] = *in;
            return;
        }
    }
    if (s_sensor_count >= MINIHUB_BM_MAP_MAX_SENSORS) {
        ESP_LOGW(TAG, "sensor table full");
        return;
    }
    s_sensors[s_sensor_count++] = *in;
}

static void merge_models_array(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        minihub_bm_map_model_t m = {0};
        m.model_id = json_u16(cJSON_GetObjectItem(item, "id"));
        if (!m.model_id) {
            continue;
        }
        const cJSON *cap = cJSON_GetObjectItem(item, "cap");
        if (cJSON_IsString(cap) && cap->valuestring) {
            m.cap_bits |= cap_bit_from_name(cap->valuestring);
            /* The cap name doubles as the model's config name (onoff, ctl, …). */
            strncpy(m.name, cap->valuestring, sizeof(m.name) - 1);
        }
        const cJSON *also = cJSON_GetObjectItem(item, "also");
        if (cJSON_IsString(also)) {
            m.cap_bits |= cap_bit_from_name(also->valuestring);
        }
        m.client_id = json_u16(cJSON_GetObjectItem(item, "client"));
        m.get_opcode = json_u16(cJSON_GetObjectItem(item, "get"));
        const cJSON *alias = cJSON_GetObjectItem(item, "alias");
        if (cJSON_IsString(alias) && alias->valuestring) {
            strncpy(m.alias, alias->valuestring, sizeof(m.alias) - 1);
        }
        upsert_model(&m);
    }
}

static size_t merge_prop_list(const cJSON *arr, uint16_t *out, size_t max)
{
    size_t n = 0;
    if (!cJSON_IsArray(arr)) {
        return 0;
    }
    const cJSON *p = NULL;
    cJSON_ArrayForEach(p, arr)
    {
        if (n >= max) {
            ESP_LOGW(TAG, "classify prop list truncated at %u", (unsigned)max);
            break;
        }
        uint16_t id = json_u16(p);
        if (id) {
            out[n++] = id;
        }
    }
    return n;
}

static void merge_classify_array(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        if (!cJSON_IsString(type) || !type->valuestring || !type->valuestring[0]) {
            continue;
        }
        minihub_bm_map_classify_t c = {0};
        strncpy(c.type, type->valuestring, sizeof(c.type) - 1);
        const cJSON *prio = cJSON_GetObjectItem(item, "priority");
        c.priority = cJSON_IsNumber(prio) ? (int)prio->valuedouble : 0;

        const cJSON *any_caps = cJSON_GetObjectItem(item, "any_caps");
        if (cJSON_IsArray(any_caps)) {
            const cJSON *cn = NULL;
            cJSON_ArrayForEach(cn, any_caps)
            {
                if (cJSON_IsString(cn) && cn->valuestring) {
                    c.any_caps_mask |= cap_bit_from_name(cn->valuestring);
                }
            }
        }
        c.all_prop_count = merge_prop_list(cJSON_GetObjectItem(item, "all_props"), c.all_props,
                                           MINIHUB_BM_MAP_MAX_PROPS);
        c.any_prop_count = merge_prop_list(cJSON_GetObjectItem(item, "any_props"), c.any_props,
                                           MINIHUB_BM_MAP_MAX_PROPS);
        upsert_classify(&c);
    }
}

static void merge_params_array(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        const cJSON *param = cJSON_GetObjectItem(item, "param");
        if (!cJSON_IsString(param) || !param->valuestring || !param->valuestring[0]) {
            continue;
        }
        minihub_bm_map_param_t p = {0};
        strncpy(p.param, param->valuestring, sizeof(p.param) - 1);
        const cJSON *cap = cJSON_GetObjectItem(item, "cap");
        if (cJSON_IsString(cap)) {
            p.cap_bit = cap_bit_from_name(cap->valuestring);
        }
        p.property_id = json_u16(cJSON_GetObjectItem(item, "prop"));
        const cJSON *access = cJSON_GetObjectItem(item, "access");
        if (cJSON_IsString(access) && access->valuestring) {
            strncpy(p.access, access->valuestring, sizeof(p.access) - 1);
        }
        const cJSON *init = cJSON_GetObjectItem(item, "init");
        if (cJSON_IsNumber(init)) {
            snprintf(p.init, sizeof(p.init), "%g", init->valuedouble);
            p.init_is_number = true;
        } else if (cJSON_IsString(init) && init->valuestring) {
            strncpy(p.init, init->valuestring, sizeof(p.init) - 1);
        }
        upsert_param(&p);
    }
}

static void merge_controls_array(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        const cJSON *param = cJSON_GetObjectItem(item, "param");
        if (!cJSON_IsString(param) || !param->valuestring || !param->valuestring[0]) {
            continue;
        }
        minihub_bm_map_control_t c = {0};
        strncpy(c.param, param->valuestring, sizeof(c.param) - 1);
        const cJSON *kind = cJSON_GetObjectItem(item, "kind");
        if (cJSON_IsString(kind) && kind->valuestring) {
            c.kind = ctrl_kind_from_name(kind->valuestring);
        }
        c.model_id = json_u16(cJSON_GetObjectItem(item, "model"));
        const cJSON *fallback = cJSON_GetObjectItem(item, "fallback");
        if (cJSON_IsString(fallback) && fallback->valuestring) {
            c.fallback = ctrl_kind_from_name(fallback->valuestring);
        }
        const cJSON *field = cJSON_GetObjectItem(item, "field");
        if (cJSON_IsString(field) && field->valuestring) {
            strncpy(c.field, field->valuestring, sizeof(c.field) - 1);
        }
        upsert_control(&c);
    }
}

static void merge_sensors_array(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        const cJSON *param = cJSON_GetObjectItem(item, "param");
        if (!cJSON_IsString(param) || !param->valuestring || !param->valuestring[0]) {
            continue;
        }
        minihub_bm_sensor_t s = {0};
        s.property_id = json_u16(cJSON_GetObjectItem(item, "property"));
        if (!s.property_id) {
            continue;
        }
        strncpy(s.param, param->valuestring, sizeof(s.param) - 1);
        const cJSON *size = cJSON_GetObjectItem(item, "size");
        s.size = cJSON_IsNumber(size) ? (uint8_t)size->valuedouble : 0;
        s.is_signed = cJSON_IsTrue(cJSON_GetObjectItem(item, "signed"));
        const cJSON *scale = cJSON_GetObjectItem(item, "scale");
        s.scale = cJSON_IsNumber(scale) ? scale->valuedouble : 1.0;
        /* Absent sentinels stay -1 so the decoder can tell "no sentinel" from a real 0. */
        s.unknown = -1;
        s.has_unknown = json_i64(cJSON_GetObjectItem(item, "unknown"), &s.unknown);
        s.bool_threshold = -1;
        const cJSON *thr = cJSON_GetObjectItem(item, "bool_threshold");
        if (cJSON_IsNumber(thr)) {
            s.bool_threshold = (int)thr->valuedouble;
            s.has_bool_threshold = true;
        }
        const cJSON *mn = cJSON_GetObjectItem(item, "min");
        if (cJSON_IsNumber(mn)) {
            s.min = mn->valuedouble;
            s.has_min = true;
        }
        const cJSON *mx = cJSON_GetObjectItem(item, "max");
        if (cJSON_IsNumber(mx)) {
            s.max = mx->valuedouble;
            s.has_max = true;
        }
        upsert_sensor(&s);
    }
}

bool minihub_bm_encode_pub_period(uint16_t seconds, uint8_t *octet_out)
{
    if (!octet_out) {
        return false;
    }
    if (seconds == 0) {
        *octet_out = 0x00;
        return true;
    }
    /* Coarsest exact step size first: 10 minutes, then 10 seconds, then 1 second.
     * Bits 0-5 hold at most 63 steps, so a period that is not a multiple of the
     * step size has no exact encoding and is rejected rather than rounded. */
    if (seconds % 600 == 0 && seconds / 600 <= 63) {
        *octet_out = (uint8_t)(0xC0 | (seconds / 600));
        return true;
    }
    if (seconds % 10 == 0 && seconds / 10 <= 63) {
        *octet_out = (uint8_t)(0x80 | (seconds / 10));
        return true;
    }
    if (seconds <= 63) {
        *octet_out = (uint8_t)(0x40 | seconds);
        return true;
    }
    return false;
}

const minihub_bm_report_t *minihub_bm_report(void)
{
    return &s_report;
}

static void merge_report(const cJSON *obj)
{
    if (!cJSON_IsObject(obj)) {
        return;
    }
    minihub_bm_report_t r = {0};
    r.group = json_u16(cJSON_GetObjectItem(obj, "group"));
    if (!r.group) {
        ESP_LOGW(TAG, "report block names no group address — reporting stays off");
        return;
    }
    const cJSON *period = cJSON_GetObjectItem(obj, "period_s");
    r.period_s = cJSON_IsNumber(period) ? (uint16_t)period->valuedouble : 0;
    if (!minihub_bm_encode_pub_period(r.period_s, &r.period_octet)) {
        ESP_LOGW(TAG, "report period %u s has no exact publication encoding — reporting stays off",
                 (unsigned)r.period_s);
        return;
    }
    const cJSON *ttl = cJSON_GetObjectItem(obj, "ttl");
    r.ttl = cJSON_IsNumber(ttl) ? (uint8_t)ttl->valuedouble : 0;
    const cJSON *cnt = cJSON_GetObjectItem(obj, "retransmit_count");
    r.retransmit_count = cJSON_IsNumber(cnt) ? (uint8_t)cnt->valuedouble : 0;
    const cJSON *itvl = cJSON_GetObjectItem(obj, "retransmit_ms");
    r.retransmit_ms = cJSON_IsNumber(itvl) ? (uint16_t)itvl->valuedouble : 0;
    /* Outside 0-127 and not 255 (default), a node drops the Publication Set
     * without sending a Status, so the failure would look like a timeout. */
    if (r.ttl > 0x7F && r.ttl != 0xFF) {
        ESP_LOGW(TAG, "report ttl %u is neither 0-127 nor 255 — reporting stays off",
                 (unsigned)r.ttl);
        return;
    }
    if (r.retransmit_count > 7) {
        ESP_LOGW(TAG, "retransmit_count %u does not fit the 3-bit field — reporting stays off",
                 (unsigned)r.retransmit_count);
        return;
    }
    if (r.retransmit_count && (r.retransmit_ms < 50 || r.retransmit_ms % 50)) {
        ESP_LOGW(TAG, "retransmit_ms %u must be a multiple of 50 — reporting stays off",
                 (unsigned)r.retransmit_ms);
        return;
    }
    r.enabled = true;
    s_report = r;
}

static void merge_root(const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return;
    }
    merge_models_array(cJSON_GetObjectItem(root, "models"));
    merge_classify_array(cJSON_GetObjectItem(root, "classify"));
    merge_params_array(cJSON_GetObjectItem(root, "params"));
    merge_controls_array(cJSON_GetObjectItem(root, "controls"));
    merge_sensors_array(cJSON_GetObjectItem(root, "sensors"));
    merge_report(cJSON_GetObjectItem(root, "report"));
}

static esp_err_t merge_json_text(const char *text, size_t len, const char *src_tag)
{
    if (!text || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "parse fail (%s)", src_tag ? src_tag : "?");
        return ESP_FAIL;
    }
    merge_root(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "merged %s → models=%u classify=%u params=%u controls=%u sensors=%u",
             src_tag ? src_tag : "?", (unsigned)s_model_count, (unsigned)s_classify_count,
             (unsigned)s_param_count, (unsigned)s_control_count, (unsigned)s_sensor_count);
    return ESP_OK;
}

static esp_err_t merge_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 128 * 1024) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    esp_err_t err = merge_json_text(buf, n, path);
    free(buf);
    return err;
}

esp_err_t minihub_bm_map_reload(void)
{
    clear_tables();

    size_t embed_len = (size_t)(ble_mesh_map_json_end - ble_mesh_map_json_start);
    esp_err_t err = merge_json_text((const char *)ble_mesh_map_json_start, embed_len, "embedded");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "embedded default map failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Optional overlays — same order as product_params (preinstall then /data). */
    (void)merge_file("/store/preinstall/ble_mesh_map.json");
    (void)merge_file(MINIHUB_BM_MAP_DATA_PATH);
    return ESP_OK;
}

esp_err_t minihub_bm_map_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = minihub_bm_map_reload();
    if (err == ESP_OK) {
        s_inited = true;
    }
    return err;
}

static bool prop_present(const uint16_t *props, size_t n_props, uint16_t id)
{
    if (!props) {
        return false;
    }
    for (size_t i = 0; i < n_props; i++) {
        if (props[i] == id) {
            return true;
        }
    }
    return false;
}

static bool classify_matches(const minihub_bm_map_classify_t *c, uint16_t caps,
                             const uint16_t *props, size_t n_props)
{
    if (c->any_caps_mask && (caps & c->any_caps_mask) == 0) {
        return false;
    }
    for (size_t i = 0; i < c->all_prop_count; i++) {
        if (!prop_present(props, n_props, c->all_props[i])) {
            return false;
        }
    }
    if (c->any_prop_count) {
        bool any = false;
        for (size_t i = 0; i < c->any_prop_count; i++) {
            if (prop_present(props, n_props, c->any_props[i])) {
                any = true;
                break;
            }
        }
        if (!any) {
            return false;
        }
    }
    return true;
}

const char *minihub_bm_classify(uint16_t caps, const uint16_t *props, size_t n_props)
{
    (void)minihub_bm_map_init();
    const minihub_bm_map_classify_t *best = NULL;
    for (size_t i = 0; i < s_classify_count; i++) {
        const minihub_bm_map_classify_t *c = &s_classify[i];
        if (!classify_matches(c, caps, props, n_props)) {
            continue;
        }
        /* Highest priority first; table order breaks a tie so an overlay stays predictable. */
        if (!best || c->priority > best->priority) {
            best = c;
        }
    }
    return best ? best->type : NULL;
}

bool minihub_bm_model_cap(uint16_t model_id, uint16_t *caps_out, uint16_t *client_out)
{
    (void)minihub_bm_map_init();
    for (size_t i = 0; i < s_model_count; i++) {
        if (s_models[i].model_id == model_id) {
            if (caps_out) {
                *caps_out = s_models[i].cap_bits;
            }
            if (client_out) {
                *client_out = s_models[i].client_id;
            }
            return true;
        }
    }
    return false;
}

size_t minihub_bm_classify_count(void)
{
    (void)minihub_bm_map_init();
    return s_classify_count;
}

const minihub_bm_map_classify_t *minihub_bm_classify_at(size_t index)
{
    (void)minihub_bm_map_init();
    if (index >= s_classify_count) {
        return NULL;
    }
    return &s_classify[index];
}

size_t minihub_bm_model_count(void)
{
    (void)minihub_bm_map_init();
    return s_model_count;
}

const minihub_bm_map_model_t *minihub_bm_model_at(size_t index)
{
    (void)minihub_bm_map_init();
    if (index >= s_model_count) {
        return NULL;
    }
    return &s_models[index];
}

const minihub_bm_map_model_t *minihub_bm_model_by_id(uint16_t model_id)
{
    (void)minihub_bm_map_init();
    for (size_t i = 0; i < s_model_count; i++) {
        if (s_models[i].model_id == model_id) {
            return &s_models[i];
        }
    }
    return NULL;
}

bool minihub_bm_model_id_by_name(const char *name, uint16_t *model_id)
{
    (void)minihub_bm_map_init();
    if (!name || !name[0] || !model_id) {
        return false;
    }
    for (size_t i = 0; i < s_model_count; i++) {
        const minihub_bm_map_model_t *m = &s_models[i];
        if (strcmp(m->name, name) == 0 || (m->alias[0] && strcmp(m->alias, name) == 0)) {
            *model_id = m->model_id;
            return true;
        }
    }
    return false;
}

size_t minihub_bm_params_for(uint16_t caps, const uint16_t *props, size_t n_props,
                             const char **names, size_t max)
{
    (void)minihub_bm_map_init();
    size_t n = 0;
    if (!names || max == 0) {
        return 0;
    }
    for (size_t i = 0; i < s_param_count && n < max; i++) {
        const minihub_bm_map_param_t *p = &s_params[i];
        bool hit = (p->cap_bit && (caps & p->cap_bit) != 0) ||
                   (p->property_id && prop_present(props, n_props, p->property_id));
        if (!hit) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < n; j++) {
            if (strcmp(names[j], p->param) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        names[n++] = p->param;
    }
    return n;
}

const minihub_bm_map_param_t *minihub_bm_param_by_name(const char *name)
{
    (void)minihub_bm_map_init();
    if (!name || !name[0]) {
        return NULL;
    }
    for (size_t i = 0; i < s_param_count; i++) {
        if (strcmp(s_params[i].param, name) == 0) {
            return &s_params[i];
        }
    }
    return NULL;
}

const minihub_bm_map_control_t *minihub_bm_control_for_param(const char *param)
{
    (void)minihub_bm_map_init();
    if (!param || !param[0]) {
        return NULL;
    }
    for (size_t i = 0; i < s_control_count; i++) {
        if (strcmp(s_controls[i].param, param) == 0) {
            return &s_controls[i];
        }
    }
    return NULL;
}

size_t minihub_bm_control_count(void)
{
    (void)minihub_bm_map_init();
    return s_control_count;
}

const minihub_bm_map_control_t *minihub_bm_control_at(size_t index)
{
    (void)minihub_bm_map_init();
    if (index >= s_control_count) {
        return NULL;
    }
    return &s_controls[index];
}

const minihub_bm_sensor_t *minihub_bm_sensor_by_property(uint16_t property_id)
{
    (void)minihub_bm_map_init();
    for (size_t i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].property_id == property_id) {
            return &s_sensors[i];
        }
    }
    return NULL;
}

int minihub_bm_format_point_id(char *out, size_t out_len, uint16_t unicast)
{
    if (!out || out_len == 0) {
        return -1;
    }
    int n = snprintf(out, out_len, "ble_mesh.bm_%04x", (unsigned)unicast);
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    return 0;
}

const char *minihub_bm_stable_suffix(const char *point_id)
{
    if (!point_id || point_id[0] == '\0') {
        return NULL;
    }
    if (strncmp(point_id, "ble_mesh.", 9) == 0) {
        const char *s = point_id + 9;
        return (s[0] == 'b' && s[1] == 'm' && s[2] == '_') ? s : NULL;
    }
    const char *slash = strrchr(point_id, '/');
    if (slash && slash[1] == 'b' && slash[2] == 'm' && slash[3] == '_') {
        return slash + 1;
    }
    if (point_id[0] == 'b' && point_id[1] == 'm' && point_id[2] == '_') {
        return point_id;
    }
    return NULL;
}

bool minihub_bm_normalize_point_id(char *point_id, size_t len)
{
    if (!point_id || len == 0 || point_id[0] == '\0') {
        return false;
    }
    if (strncmp(point_id, "ble_mesh.", 9) == 0) {
        return false;
    }
    /* Legacy discovery form: "{type}/bm_XXXX" */
    const char *slash = strchr(point_id, '/');
    if (!slash || slash[1] != 'b' || slash[2] != 'm' || slash[3] != '_') {
        return false;
    }
    const char *stable = slash + 1;
    char buf[MINIHUB_POINT_ID_MAX];
    int n = snprintf(buf, sizeof(buf), "ble_mesh.%s", stable);
    if (n < 0 || (size_t)n >= len || (size_t)n >= sizeof(buf)) {
        return false;
    }
    if (strcmp(point_id, buf) == 0) {
        return false;
    }
    strncpy(point_id, buf, len - 1);
    point_id[len - 1] = '\0';
    return true;
}

int minihub_bm_uuid_to_hex(const uint8_t uuid[16], char *out, size_t out_len)
{
    if (!uuid || !out || out_len < MINIHUB_BM_UUID_HEX_LEN) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", uuid[i]);
    }
    out[32] = '\0';
    return 0;
}

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

int minihub_bm_hex_to_uuid(const char *hex, uint8_t uuid[16])
{
    if (!hex || !uuid) {
        return -1;
    }
    const char *p = hex;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (strlen(p) != 32) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble(p[i * 2]);
        int lo = hex_nibble(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        uuid[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
