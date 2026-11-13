/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zigbee_cluster_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "zb_cluster_map";

/* Embedded via CMake EMBED_TXTFILES "config/zigbee_cluster_map.json"
 * (symbol uses the file basename, not the config/ prefix). */
extern const uint8_t zigbee_cluster_map_json_start[] asm("_binary_zigbee_cluster_map_json_start");
extern const uint8_t zigbee_cluster_map_json_end[] asm("_binary_zigbee_cluster_map_json_end");

static minihub_zb_map_cluster_t s_clusters[MINIHUB_ZB_MAP_MAX_CLUSTERS];
static size_t s_cluster_count;
static minihub_zb_map_classify_t s_classify[MINIHUB_ZB_MAP_MAX_CLASSIFY];
static size_t s_classify_count;
static minihub_zb_map_attr_t s_attrs[MINIHUB_ZB_MAP_MAX_ATTRS];
static size_t s_attr_count;
static minihub_zb_map_control_t s_controls[MINIHUB_ZB_MAP_MAX_CTRLS];
static size_t s_control_count;
static bool s_inited;

static uint32_t parse_u32_flex(const char *s)
{
    if (!s || !s[0]) {
        return 0;
    }
    return (uint32_t)strtoul(s, NULL, 0);
}

static uint16_t json_u16(const cJSON *v)
{
    if (cJSON_IsString(v) && v->valuestring) {
        return (uint16_t)parse_u32_flex(v->valuestring);
    }
    if (cJSON_IsNumber(v)) {
        return (uint16_t)v->valuedouble;
    }
    return 0;
}

static uint8_t cap_bit_from_name(const char *name)
{
    if (!name || !name[0]) {
        return 0;
    }
    if (strcmp(name, "onoff") == 0) {
        return MINIHUB_ZB_CAP_ONOFF;
    }
    if (strcmp(name, "temp") == 0 || strcmp(name, "temperature") == 0) {
        return MINIHUB_ZB_CAP_TEMP;
    }
    if (strcmp(name, "humidity") == 0) {
        return MINIHUB_ZB_CAP_HUMIDITY;
    }
    if (strcmp(name, "ias_zone") == 0 || strcmp(name, "contact") == 0) {
        return MINIHUB_ZB_CAP_IAS_ZONE;
    }
    if (strcmp(name, "electrical") == 0) {
        return MINIHUB_ZB_CAP_ELECTRICAL;
    }
    if (strcmp(name, "energy") == 0) {
        return MINIHUB_ZB_CAP_ENERGY;
    }
    /* Unknown names (e.g. future clusters) → no CAP bit; seed still uses attrs. */
    return 0;
}

static minihub_zb_ctrl_kind_t ctrl_kind_from_str(const char *s)
{
    if (!s) {
        return MINIHUB_ZB_CTRL_NONE;
    }
    if (strcmp(s, "on_off") == 0 || strcmp(s, "onoff") == 0) {
        return MINIHUB_ZB_CTRL_ON_OFF;
    }
    if (strcmp(s, "move_to_level") == 0 || strcmp(s, "level") == 0) {
        return MINIHUB_ZB_CTRL_MOVE_TO_LEVEL;
    }
    if (strcmp(s, "write_attr") == 0 || strcmp(s, "attr_write") == 0) {
        return MINIHUB_ZB_CTRL_WRITE_ATTR;
    }
    return MINIHUB_ZB_CTRL_NONE;
}

static uint8_t zcl_type_from_str(const char *s)
{
    if (!s) {
        return 0x20;
    }
    if (strcmp(s, "bool") == 0 || strcmp(s, "boolean") == 0) {
        return 0x10;
    }
    if (strcmp(s, "uint8") == 0) {
        return 0x20;
    }
    if (strcmp(s, "uint16") == 0) {
        return 0x21;
    }
    if (strcmp(s, "int16") == 0) {
        return 0x29;
    }
    if (strcmp(s, "int8") == 0) {
        return 0x28;
    }
    if (strcmp(s, "uint32") == 0) {
        return 0x23;
    }
    return (uint8_t)parse_u32_flex(s);
}

static uint8_t cap_for_cluster(uint16_t cluster_id)
{
    for (size_t i = 0; i < s_cluster_count; i++) {
        if (s_clusters[i].cluster_id == cluster_id) {
            return s_clusters[i].cap_bit;
        }
    }
    return 0;
}

static void clear_tables(void)
{
    memset(s_clusters, 0, sizeof(s_clusters));
    s_cluster_count = 0;
    memset(s_classify, 0, sizeof(s_classify));
    s_classify_count = 0;
    memset(s_attrs, 0, sizeof(s_attrs));
    s_attr_count = 0;
    memset(s_controls, 0, sizeof(s_controls));
    s_control_count = 0;
}

static void upsert_cluster(uint16_t id, uint8_t cap_bit)
{
    for (size_t i = 0; i < s_cluster_count; i++) {
        if (s_clusters[i].cluster_id == id) {
            s_clusters[i].cap_bit = cap_bit;
            return;
        }
    }
    if (s_cluster_count >= MINIHUB_ZB_MAP_MAX_CLUSTERS) {
        ESP_LOGW(TAG, "cluster table full");
        return;
    }
    s_clusters[s_cluster_count].cluster_id = id;
    s_clusters[s_cluster_count].cap_bit = cap_bit;
    s_cluster_count++;
}

static void upsert_classify(const minihub_zb_map_classify_t *in)
{
    for (size_t i = 0; i < s_classify_count; i++) {
        if (strcmp(s_classify[i].type, in->type) == 0) {
            s_classify[i] = *in;
            return;
        }
    }
    if (s_classify_count >= MINIHUB_ZB_MAP_MAX_CLASSIFY) {
        ESP_LOGW(TAG, "classify table full");
        return;
    }
    s_classify[s_classify_count++] = *in;
}

static void upsert_attr(const minihub_zb_map_attr_t *in)
{
    for (size_t i = 0; i < s_attr_count; i++) {
        if (s_attrs[i].cluster == in->cluster && s_attrs[i].attr == in->attr &&
            strcmp(s_attrs[i].param, in->param) == 0) {
            s_attrs[i] = *in;
            return;
        }
    }
    if (s_attr_count >= MINIHUB_ZB_MAP_MAX_ATTRS) {
        ESP_LOGW(TAG, "attr table full");
        return;
    }
    s_attrs[s_attr_count++] = *in;
}

static void upsert_control(const minihub_zb_map_control_t *in)
{
    for (size_t i = 0; i < s_control_count; i++) {
        if (strcmp(s_controls[i].param, in->param) == 0) {
            s_controls[i] = *in;
            return;
        }
    }
    if (s_control_count >= MINIHUB_ZB_MAP_MAX_CTRLS) {
        ESP_LOGW(TAG, "control table full");
        return;
    }
    s_controls[s_control_count++] = *in;
}

static void merge_models_array(const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        uint16_t id = json_u16(cJSON_GetObjectItem(item, "id"));
        if (!id && !cJSON_GetObjectItem(item, "id")) {
            continue;
        }
        const cJSON *cap = cJSON_GetObjectItem(item, "cap");
        uint8_t bit = cJSON_IsString(cap) ? cap_bit_from_name(cap->valuestring) : 0;
        upsert_cluster(id, bit);
    }
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
        minihub_zb_map_classify_t c = {0};
        strncpy(c.type, type->valuestring, sizeof(c.type) - 1);
        const cJSON *prio = cJSON_GetObjectItem(item, "priority");
        c.priority = cJSON_IsNumber(prio) ? (int)prio->valuedouble : 0;

        const cJSON *ha = cJSON_GetObjectItem(item, "ha_device_ids");
        if (cJSON_IsArray(ha)) {
            const cJSON *hid = NULL;
            cJSON_ArrayForEach(hid, ha)
            {
                if (c.ha_device_id_count >= MINIHUB_ZB_MAP_MAX_HA_IDS) {
                    break;
                }
                c.ha_device_ids[c.ha_device_id_count++] = json_u16(hid);
            }
        }

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
        upsert_classify(&c);
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
        minihub_zb_map_attr_t a = {0};
        a.cluster = json_u16(cJSON_GetObjectItem(item, "cluster"));
        a.attr = json_u16(cJSON_GetObjectItem(item, "attr"));
        strncpy(a.param, param->valuestring, sizeof(a.param) - 1);
        const cJSON *access = cJSON_GetObjectItem(item, "access");
        if (cJSON_IsString(access) && access->valuestring) {
            strncpy(a.access, access->valuestring, sizeof(a.access) - 1);
        }
        const cJSON *report = cJSON_GetObjectItem(item, "report");
        if (cJSON_IsString(report) && report->valuestring) {
            strncpy(a.report, report->valuestring, sizeof(a.report) - 1);
        }
        const cJSON *field = cJSON_GetObjectItem(item, "field");
        if (cJSON_IsString(field) && field->valuestring) {
            strncpy(a.field, field->valuestring, sizeof(a.field) - 1);
        }
        const cJSON *summary = cJSON_GetObjectItem(item, "summary");
        a.summary = cJSON_IsTrue(summary);
        upsert_attr(&a);
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
        minihub_zb_map_control_t c = {0};
        strncpy(c.param, param->valuestring, sizeof(c.param) - 1);
        const cJSON *kind = cJSON_GetObjectItem(item, "kind");
        if (cJSON_IsString(kind)) {
            c.kind = ctrl_kind_from_str(kind->valuestring);
        }
        c.cluster = json_u16(cJSON_GetObjectItem(item, "cluster"));
        c.attr = json_u16(cJSON_GetObjectItem(item, "attr"));
        const cJSON *zcl = cJSON_GetObjectItem(item, "zcl_type");
        if (cJSON_IsString(zcl)) {
            c.zcl_type = zcl_type_from_str(zcl->valuestring);
        } else if (cJSON_IsNumber(zcl)) {
            c.zcl_type = (uint8_t)zcl->valuedouble;
        } else {
            c.zcl_type = 0x20;
        }
        const cJSON *aliases = cJSON_GetObjectItem(item, "aliases");
        if (cJSON_IsArray(aliases)) {
            const cJSON *al = NULL;
            cJSON_ArrayForEach(al, aliases)
            {
                if (!cJSON_IsString(al) || !al->valuestring) {
                    continue;
                }
                if (c.alias_count >= MINIHUB_ZB_MAP_MAX_ALIASES) {
                    break;
                }
                strncpy(c.aliases[c.alias_count], al->valuestring, MINIHUB_ZB_MAP_PARAM_MAX - 1);
                c.alias_count++;
            }
        }
        upsert_control(&c);
    }
}

static void merge_root(const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return;
    }
    merge_models_array(cJSON_GetObjectItem(root, "models"));
    merge_classify_array(cJSON_GetObjectItem(root, "classify"));
    merge_sensors_array(cJSON_GetObjectItem(root, "sensors"));
    merge_controls_array(cJSON_GetObjectItem(root, "controls"));
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
    ESP_LOGI(TAG, "merged %s → clusters=%u classify=%u attrs=%u controls=%u",
             src_tag ? src_tag : "?", (unsigned)s_cluster_count, (unsigned)s_classify_count,
             (unsigned)s_attr_count, (unsigned)s_control_count);
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

esp_err_t minihub_zb_cluster_map_reload(void)
{
    clear_tables();

    size_t embed_len = (size_t)(zigbee_cluster_map_json_end - zigbee_cluster_map_json_start);
    esp_err_t err = merge_json_text((const char *)zigbee_cluster_map_json_start, embed_len, "embedded");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "embedded default map failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Optional overlays — same order as product_params (preinstall then /data). */
    (void)merge_file("/store/preinstall/zigbee_cluster_map.json");
    (void)merge_file("/data/config/zigbee_cluster_map.json");
    return ESP_OK;
}

esp_err_t minihub_zb_cluster_map_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = minihub_zb_cluster_map_reload();
    if (err == ESP_OK) {
        s_inited = true;
    }
    return err;
}

uint8_t minihub_zb_map_caps_from_clusters(const uint16_t *clusters, size_t count)
{
    (void)minihub_zb_cluster_map_init();
    uint8_t caps = 0;
    if (!clusters) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        caps |= cap_for_cluster(clusters[i]);
    }
    return caps;
}

const char *minihub_zb_map_classify_type(uint8_t caps, uint16_t app_device_id)
{
    (void)minihub_zb_cluster_map_init();
    int best_prio = -1;
    const char *best = NULL;

    for (size_t i = 0; i < s_classify_count; i++) {
        const minihub_zb_map_classify_t *c = &s_classify[i];
        bool match = false;
        if (c->any_caps_mask && (caps & c->any_caps_mask) != 0) {
            match = true;
        }
        for (size_t h = 0; h < c->ha_device_id_count; h++) {
            if (c->ha_device_ids[h] == app_device_id) {
                match = true;
                break;
            }
        }
        if (!match) {
            continue;
        }
        if (c->priority > best_prio) {
            best_prio = c->priority;
            best = c->type;
        }
    }
    return best ? best : "unknown";
}

bool minihub_zb_map_access_is_readonly(const char *access)
{
    if (!access || !access[0]) {
        return false;
    }
    if (strcmp(access, "R") == 0 || strcmp(access, "r") == 0 || strcmp(access, "read") == 0) {
        return true;
    }
    return false;
}

size_t minihub_zb_map_params_for_caps(uint8_t caps, char out_params[][MINIHUB_ZB_MAP_PARAM_MAX],
                                      size_t cap)
{
    (void)minihub_zb_cluster_map_init();
    size_t n = 0;
    if (!out_params || cap == 0) {
        return 0;
    }
    for (size_t i = 0; i < s_attr_count && n < cap; i++) {
        uint8_t bit = cap_for_cluster(s_attrs[i].cluster);
        if (bit == 0 || (caps & bit) == 0) {
            continue;
        }
        /* de-dupe param names */
        bool seen = false;
        for (size_t j = 0; j < n; j++) {
            if (strcmp(out_params[j], s_attrs[i].param) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        strncpy(out_params[n], s_attrs[i].param, MINIHUB_ZB_MAP_PARAM_MAX - 1);
        out_params[n][MINIHUB_ZB_MAP_PARAM_MAX - 1] = '\0';
        n++;
    }
    return n;
}

size_t minihub_zb_map_read_paths_for_caps(uint8_t caps, minihub_zb_map_path_t *out, size_t out_cap)
{
    (void)minihub_zb_cluster_map_init();
    size_t n = 0;
    if (!out || out_cap == 0 || caps == 0) {
        return 0;
    }
    for (size_t i = 0; i < s_attr_count && n < out_cap; i++) {
        uint8_t bit = cap_for_cluster(s_attrs[i].cluster);
        if (bit == 0 || (caps & bit) == 0) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < n; j++) {
            if (out[j].cluster == s_attrs[i].cluster && out[j].attr == s_attrs[i].attr) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        out[n].cluster = s_attrs[i].cluster;
        out[n].attr = s_attrs[i].attr;
        n++;
    }
    return n;
}

const minihub_zb_map_attr_t *minihub_zb_map_attr_by_param(const char *param)
{
    (void)minihub_zb_cluster_map_init();
    if (!param || !param[0]) {
        return NULL;
    }
    for (size_t i = 0; i < s_attr_count; i++) {
        if (strcmp(s_attrs[i].param, param) == 0) {
            return &s_attrs[i];
        }
    }
    return NULL;
}

const minihub_zb_map_attr_t *minihub_zb_map_attr_by_report(const char *report)
{
    (void)minihub_zb_cluster_map_init();
    if (!report || !report[0]) {
        return NULL;
    }
    for (size_t i = 0; i < s_attr_count; i++) {
        if (strcmp(s_attrs[i].report, report) == 0) {
            return &s_attrs[i];
        }
    }
    return NULL;
}

size_t minihub_zb_map_attrs_by_report(const char *report, const minihub_zb_map_attr_t **out,
                                      size_t out_cap)
{
    (void)minihub_zb_cluster_map_init();
    size_t n = 0;
    if (!report || !out || out_cap == 0) {
        return 0;
    }
    for (size_t i = 0; i < s_attr_count && n < out_cap; i++) {
        if (strcmp(s_attrs[i].report, report) == 0) {
            out[n++] = &s_attrs[i];
        }
    }
    return n;
}

const minihub_zb_map_control_t *minihub_zb_map_control_for_param(const char *param)
{
    (void)minihub_zb_cluster_map_init();
    if (!param || !param[0]) {
        return NULL;
    }
    for (size_t i = 0; i < s_control_count; i++) {
        if (strcmp(s_controls[i].param, param) == 0) {
            return &s_controls[i];
        }
        for (size_t a = 0; a < s_controls[i].alias_count; a++) {
            if (strcmp(s_controls[i].aliases[a], param) == 0) {
                return &s_controls[i];
            }
        }
    }
    return NULL;
}

bool minihub_zb_map_type_has_onoff(const char *type)
{
    (void)minihub_zb_cluster_map_init();
    /* Prefer control table — any on_off control means the param is wired. */
    for (size_t i = 0; i < s_control_count; i++) {
        if (s_controls[i].kind == MINIHUB_ZB_CTRL_ON_OFF) {
            if (!type || !type[0]) {
                return true;
            }
            /* light / switch historically support power; sensors do not. */
            if (strcmp(type, "light") == 0 || strcmp(type, "switch") == 0) {
                return true;
            }
        }
    }
    return type && (strcmp(type, "light") == 0 || strcmp(type, "switch") == 0);
}

const char *minihub_zb_map_default_summary(const minihub_zigbee_device_t *dev)
{
    if (!dev) {
        return "off";
    }
    if (dev->caps & MINIHUB_ZB_CAP_IAS_ZONE) {
        const minihub_zb_map_attr_t *a = minihub_zb_map_attr_by_report("contact");
        (void)a;
        return "closed";
    }
    if (dev->type[0] && strcmp(dev->type, "sensor") == 0) {
        return "22.0";
    }
    return "off";
}

const char *minihub_zb_map_report_name_for_kind(minihub_zigbee_attr_kind_t kind)
{
    switch (kind) {
    case MINIHUB_ZB_ATTR_ONOFF:
        return "onoff";
    case MINIHUB_ZB_ATTR_TEMP_C:
        return "temp_c";
    case MINIHUB_ZB_ATTR_HUMIDITY_PCT:
        return "humidity_pct";
    case MINIHUB_ZB_ATTR_CONTACT:
        return "contact";
    case MINIHUB_ZB_ATTR_POWER_W:
        return "power_w";
    case MINIHUB_ZB_ATTR_VOLTAGE_V:
        return "voltage_v";
    case MINIHUB_ZB_ATTR_CURRENT_A:
        return "current_a";
    case MINIHUB_ZB_ATTR_ENERGY_WH:
        return "energy_wh";
    default:
        return NULL;
    }
}
