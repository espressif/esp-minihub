/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "matter_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "matter_map";

/* Embedded via CMake EMBED_TXTFILES "matter_map.json"
 * (symbol uses the file basename, not any directory prefix). */
extern const uint8_t matter_map_json_start[] asm("_binary_matter_map_json_start");
extern const uint8_t matter_map_json_end[] asm("_binary_matter_map_json_end");

typedef struct {
    uint16_t cluster;
    char cap[16]; /* capability name from models[] (onoff/level/ctl/hsl/sensor) */
} matter_model_t;

typedef struct {
    char param[MINIHUB_MATTER_MAP_PARAM_MAX];
    uint16_t cluster; /* target cluster for the control */
    uint16_t attr;    /* attribute id for subscribe/read; 0 = OnOff / CurrentLevel / MeasuredValue */
    char field[MINIHUB_MATTER_MAP_FIELD_MAX]; /* field name within the cluster */
} matter_ctrl_t;

typedef struct {
    uint16_t cluster;
    char param[MINIHUB_MATTER_MAP_PARAM_MAX];
} matter_caparam_t;

static matter_model_t s_models[MINIHUB_MATTER_MAP_MAX_MODELS];
static size_t s_model_count;
static matter_ctrl_t s_ctrls[MINIHUB_MATTER_MAP_MAX_CTRLS];
static size_t s_ctrl_count;
static matter_caparam_t s_caparam[MINIHUB_MATTER_MAP_MAX_CAPARAMS];
static size_t s_caparam_count;

typedef struct {
    uint16_t cluster;
    uint16_t attr; /* MeasuredValue / Occupancy / StateValue; default 0 */
    char param[MINIHUB_MATTER_MAP_PARAM_MAX];
    bool is_signed;
    double scale;
    double min;
    double max;
    int bool_threshold; /* <0 numeric; >=0 boolean (raw > thr => on) */
} matter_sensor_t;

static matter_sensor_t s_sensors[MINIHUB_MATTER_MAP_MAX_CAPARAMS];
static size_t s_sensor_count;
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

static void clear_tables(void)
{
    memset(s_models, 0, sizeof(s_models));
    s_model_count = 0;
    memset(s_ctrls, 0, sizeof(s_ctrls));
    s_ctrl_count = 0;
    memset(s_caparam, 0, sizeof(s_caparam));
    s_caparam_count = 0;
    memset(s_sensors, 0, sizeof(s_sensors));
    s_sensor_count = 0;
}

static void upsert_model(const matter_model_t *in)
{
    for (size_t i = 0; i < s_model_count; i++) {
        if (s_models[i].cluster == in->cluster) {
            s_models[i] = *in;
            return;
        }
    }
    if (s_model_count >= MINIHUB_MATTER_MAP_MAX_MODELS) {
        ESP_LOGW(TAG, "model table full");
        return;
    }
    s_models[s_model_count++] = *in;
}

static void upsert_ctrl(const matter_ctrl_t *in)
{
    for (size_t i = 0; i < s_ctrl_count; i++) {
        if (strcmp(s_ctrls[i].param, in->param) == 0) {
            s_ctrls[i] = *in;
            return;
        }
    }
    if (s_ctrl_count >= MINIHUB_MATTER_MAP_MAX_CTRLS) {
        ESP_LOGW(TAG, "control table full");
        return;
    }
    s_ctrls[s_ctrl_count++] = *in;
}

static void upsert_caparam(uint16_t cluster, const char *param)
{
    if (!cluster || !param || !param[0]) {
        return;
    }
    for (size_t i = 0; i < s_caparam_count; i++) {
        if (s_caparam[i].cluster == cluster) {
            strncpy(s_caparam[i].param, param, sizeof(s_caparam[i].param) - 1);
            s_caparam[i].param[sizeof(s_caparam[i].param) - 1] = '\0';
            return;
        }
    }
    if (s_caparam_count >= MINIHUB_MATTER_MAP_MAX_CAPARAMS) {
        ESP_LOGW(TAG, "caparam table full");
        return;
    }
    s_caparam[s_caparam_count].cluster = cluster;
    strncpy(s_caparam[s_caparam_count].param, param,
            sizeof(s_caparam[s_caparam_count].param) - 1);
    s_caparam[s_caparam_count].param[sizeof(s_caparam[s_caparam_count].param) - 1] = '\0';
    s_caparam_count++;
}

static void merge_models(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        matter_model_t m = {0};
        m.cluster = json_u16(cJSON_GetObjectItem(item, "id"));
        if (!m.cluster) {
            continue;
        }
        const cJSON *cap = cJSON_GetObjectItem(item, "cap");
        if (cJSON_IsString(cap) && cap->valuestring) {
            strncpy(m.cap, cap->valuestring, sizeof(m.cap) - 1);
        }
        upsert_model(&m);
    }
}

static void merge_params(const cJSON *arr)
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
        /* prop → directly maps a cluster id to a param. */
        uint16_t prop = json_u16(cJSON_GetObjectItem(item, "prop"));
        if (prop) {
            upsert_caparam(prop, param->valuestring);
            continue;
        }
        /* cap → resolve through models[] to the cluster id. */
        const cJSON *cap = cJSON_GetObjectItem(item, "cap");
        if (cJSON_IsString(cap) && cap->valuestring) {
            for (size_t i = 0; i < s_model_count; i++) {
                if (strcmp(s_models[i].cap, cap->valuestring) == 0) {
                    upsert_caparam(s_models[i].cluster, param->valuestring);
                    break;
                }
            }
        }
    }
}

static void merge_controls(const cJSON *arr)
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
        matter_ctrl_t c = {0};
        strncpy(c.param, param->valuestring, sizeof(c.param) - 1);
        c.cluster = json_u16(cJSON_GetObjectItem(item, "model"));
        c.attr = json_u16(cJSON_GetObjectItem(item, "attr"));
        const cJSON *field = cJSON_GetObjectItem(item, "field");
        if (cJSON_IsString(field) && field->valuestring) {
            strncpy(c.field, field->valuestring, sizeof(c.field) - 1);
            if (!c.attr) {
                if (strcmp(c.field, "color_temperature_mireds") == 0) {
                    c.attr = 0x0007;
                } else if (strcmp(c.field, "current_saturation") == 0) {
                    c.attr = 0x0001;
                }
            }
        }
        upsert_ctrl(&c);
    }
}

static void merge_sensors(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        matter_sensor_t s = {0};
        s.cluster = json_u16(cJSON_GetObjectItem(item, "property"));
        if (!s.cluster) {
            continue;
        }
        s.attr = json_u16(cJSON_GetObjectItem(item, "attr"));
        const cJSON *param = cJSON_GetObjectItem(item, "param");
        if (!cJSON_IsString(param) || !param->valuestring || !param->valuestring[0]) {
            continue;
        }
        strncpy(s.param, param->valuestring, sizeof(s.param) - 1);
        const cJSON *sc = cJSON_GetObjectItem(item, "scale");
        if (cJSON_IsNumber(sc)) {
            s.scale = sc->valuedouble;
        } else {
            s.scale = 1.0;
        }
        const cJSON *sg = cJSON_GetObjectItem(item, "signed");
        s.is_signed = cJSON_IsBool(sg) ? sg->valueint : false;
        const cJSON *mn = cJSON_GetObjectItem(item, "min");
        if (cJSON_IsNumber(mn)) {
            s.min = mn->valuedouble;
        }
        const cJSON *mx = cJSON_GetObjectItem(item, "max");
        if (cJSON_IsNumber(mx)) {
            s.max = mx->valuedouble;
        }
        const cJSON *bt = cJSON_GetObjectItem(item, "bool_threshold");
        s.bool_threshold = cJSON_IsNumber(bt) ? bt->valueint : -1;

        if (s_sensor_count >= MINIHUB_MATTER_MAP_MAX_CAPARAMS) {
            ESP_LOGW(TAG, "sensor table full");
            return;
        }
        s_sensors[s_sensor_count++] = s;
    }
}

static void merge_root(const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return;
    }
    merge_models(cJSON_GetObjectItem(root, "models"));
    /* classify is informational for Matter (device-type driven elsewhere); skip. */
    merge_params(cJSON_GetObjectItem(root, "params"));
    merge_controls(cJSON_GetObjectItem(root, "controls"));
    /* sensors[] decode descriptors drive the typed attr callback: raw reported
     * integers are scaled/clamped (or thresholded for boolean sensors) into the
     * normalized display value. */
    merge_sensors(cJSON_GetObjectItem(root, "sensors"));
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
    ESP_LOGI(TAG, "merged %s → models=%u ctrls=%u caparam=%u",
             src_tag ? src_tag : "?", (unsigned)s_model_count,
             (unsigned)s_ctrl_count, (unsigned)s_caparam_count);
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

esp_err_t minihub_matter_map_reload(void)
{
    clear_tables();

    size_t embed_len = (size_t)(matter_map_json_end - matter_map_json_start);
    esp_err_t err = merge_json_text((const char *)matter_map_json_start, embed_len, "embedded");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "embedded default map failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Optional overlays — same order as product_params (preinstall then /data). */
    (void)merge_file("/store/preinstall/matter_map.json");
    (void)merge_file("/data/config/matter_map.json");
    return ESP_OK;
}

esp_err_t minihub_matter_map_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = minihub_matter_map_reload();
    if (err == ESP_OK) {
        s_inited = true;
    }
    return err;
}

bool minihub_matter_map_control_for_param(const char *param,
                                          uint16_t *cluster_out,
                                          const char **field_out)
{
    (void)minihub_matter_map_init();
    if (!param || !param[0]) {
        return false;
    }
    for (size_t i = 0; i < s_ctrl_count; i++) {
        if (strcmp(s_ctrls[i].param, param) == 0) {
            if (cluster_out) {
                *cluster_out = s_ctrls[i].cluster;
            }
            if (field_out) {
                *field_out = s_ctrls[i].field;
            }
            return true;
        }
    }
    return false;
}

static bool path_seen(const minihub_matter_map_path_t *paths, size_t n, uint16_t cluster,
                      uint16_t attr)
{
    for (size_t i = 0; i < n; i++) {
        if (paths[i].cluster_id == cluster && paths[i].attribute_id == attr) {
            return true;
        }
    }
    return false;
}

size_t minihub_matter_map_collect_paths(minihub_matter_map_path_t *out, size_t out_cap)
{
    (void)minihub_matter_map_init();
    size_t n = 0;
    if (!out || out_cap == 0) {
        return 0;
    }
    for (size_t i = 0; i < s_ctrl_count && n < out_cap; i++) {
        if (!s_ctrls[i].cluster) {
            continue;
        }
        uint16_t attr = s_ctrls[i].attr;
        if (path_seen(out, n, s_ctrls[i].cluster, attr)) {
            continue;
        }
        out[n].cluster_id = s_ctrls[i].cluster;
        out[n].attribute_id = attr;
        n++;
    }
    for (size_t i = 0; i < s_sensor_count && n < out_cap; i++) {
        if (!s_sensors[i].cluster) {
            continue;
        }
        uint16_t attr = s_sensors[i].attr;
        if (path_seen(out, n, s_sensors[i].cluster, attr)) {
            continue;
        }
        out[n].cluster_id = s_sensors[i].cluster;
        out[n].attribute_id = attr;
        n++;
    }
    if (n == 0 && out_cap > 0) {
        out[0].cluster_id = 0x0006;
        out[0].attribute_id = 0x0000;
        n = 1;
    }
    return n;
}

const char *minihub_matter_map_param_for_cluster_attr(uint32_t cluster, uint32_t attr)
{
    (void)minihub_matter_map_init();
    for (size_t i = 0; i < s_ctrl_count; i++) {
        if (s_ctrls[i].cluster == (uint16_t)cluster && s_ctrls[i].attr == (uint16_t)attr) {
            return s_ctrls[i].param;
        }
    }
    for (size_t i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].cluster == (uint16_t)cluster &&
            s_sensors[i].attr == (uint16_t)attr) {
            return s_sensors[i].param;
        }
    }
    for (size_t i = 0; i < s_caparam_count; i++) {
        if (s_caparam[i].cluster == (uint16_t)cluster) {
            return s_caparam[i].param;
        }
    }
    return NULL;
}

bool minihub_matter_map_sensor_for_cluster(uint16_t cluster, minihub_matter_map_sensor_t *out)
{
    (void)minihub_matter_map_init();
    if (!out) {
        return false;
    }
    for (size_t i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].cluster == cluster) {
            out->cluster = s_sensors[i].cluster;
            strncpy(out->param, s_sensors[i].param, sizeof(out->param) - 1);
            out->param[sizeof(out->param) - 1] = '\0';
            out->is_signed = s_sensors[i].is_signed;
            out->scale = s_sensors[i].scale;
            out->min = s_sensors[i].min;
            out->max = s_sensors[i].max;
            out->bool_threshold = s_sensors[i].bool_threshold;
            return true;
        }
    }
    return false;
}
