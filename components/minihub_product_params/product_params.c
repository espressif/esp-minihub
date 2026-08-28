/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_product_params.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "minihub_core.h"

static const char *TAG = "product_params";

typedef struct {
    char prop[MINIHUB_PP_PROP_MAX];
    char label[MINIHUB_PP_LABEL_MAX];
    char unit[16];
    bool writable;
    float multiply;
    float divide;
    float offset;
    /* zigbee */
    uint16_t cluster;
    uint16_t attr_id;
    uint8_t zcl_type;
    /* ble mesh */
    uint16_t model_id;
    uint16_t company_id;
    uint32_t opcode;
    char field[24];
} pp_param_t;

typedef struct {
    bool in_use;
    minihub_pp_transport_t transport;
    char id[40];
    char manufacturer[40];
    char model[40];
    char ieee_prefix[24];
    char uuid_prefix[40];
    pp_param_t params[MINIHUB_PP_MAX_PARAMS];
    size_t param_count;
} pp_quirk_t;

EXT_RAM_BSS_ATTR static pp_quirk_t s_quirks[MINIHUB_PP_MAX_QUIRKS];
static size_t s_quirk_count;

static const char *k_reserved[] = {
    "power_w", "voltage_v", "current_a", "energy_wh", "humidity", "open", "zone_status", "tamper",
    "battery_low", NULL,
};

static bool prop_reserved(const char *prop)
{
    for (size_t i = 0; k_reserved[i]; i++) {
        if (strcmp(prop, k_reserved[i]) == 0) {
            return true;
        }
    }
    return false;
}

static uint32_t parse_u32_flex(const char *s)
{
    if (!s || !s[0]) {
        return 0;
    }
    return (uint32_t)strtoul(s, NULL, 0);
}

static uint8_t zcl_type_from_str(const char *s)
{
    if (!s) {
        return 0x20; /* uint8 */
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

static void ieee_to_hex(uint64_t ieee, char *out, size_t out_len)
{
    snprintf(out, out_len, "%016llx", (unsigned long long)ieee);
}

static bool prefix_match_hex(const char *prefix, const char *full_hex)
{
    if (!prefix || !prefix[0]) {
        return true;
    }
    size_t n = strlen(prefix);
    if (n > strlen(full_hex)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)prefix[i]) != tolower((unsigned char)full_hex[i])) {
            return false;
        }
    }
    return true;
}

static bool quirk_match_zigbee(const pp_quirk_t *q, uint64_t ieee, const char *manufacturer,
                               const char *model)
{
    if (q->transport != MINIHUB_PP_TRANSPORT_ZIGBEE) {
        return false;
    }
    char hex[20];
    ieee_to_hex(ieee, hex, sizeof(hex));
    if (q->ieee_prefix[0] && !prefix_match_hex(q->ieee_prefix, hex)) {
        return false;
    }
    if (q->manufacturer[0]) {
        if (!manufacturer || strcasecmp(q->manufacturer, manufacturer) != 0) {
            return false;
        }
    }
    if (q->model[0]) {
        if (!model || strcasecmp(q->model, model) != 0) {
            return false;
        }
    }
    /* Need at least one concrete matcher to avoid matching all devices. */
    if (!q->ieee_prefix[0] && !q->manufacturer[0] && !q->model[0]) {
        return false;
    }
    return true;
}

static bool quirk_match_ble(const pp_quirk_t *q, const char *uuid_hex)
{
    if (q->transport != MINIHUB_PP_TRANSPORT_BLE_MESH) {
        return false;
    }
    if (!q->uuid_prefix[0]) {
        return false;
    }
    return uuid_hex && prefix_match_hex(q->uuid_prefix, uuid_hex);
}

static double raw_to_number(uint8_t zcl_type, const void *value, size_t value_len)
{
    if (!value || value_len == 0) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)value;
    switch (zcl_type) {
    case 0x10: /* bool */
        return p[0] ? 1.0 : 0.0;
    case 0x20: /* uint8 */
    case 0x28: /* int8 */
        return (zcl_type == 0x28) ? (double)(int8_t)p[0] : (double)p[0];
    case 0x21: /* uint16 */
        if (value_len >= 2) {
            return (double)(p[0] | (p[1] << 8));
        }
        break;
    case 0x29: /* int16 */
        if (value_len >= 2) {
            return (double)(int16_t)(p[0] | (p[1] << 8));
        }
        break;
    case 0x23: /* uint32 */
        if (value_len >= 4) {
            return (double)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        }
        break;
    default:
        if (value_len >= 2) {
            return (double)(p[0] | (p[1] << 8));
        }
        return (double)p[0];
    }
    return 0;
}

static double apply_scale(double raw, float mult, float div, float offset)
{
    if (div == 0.0f) {
        div = 1.0f;
    }
    if (mult == 0.0f) {
        mult = 1.0f;
    }
    return (raw * (double)mult) / (double)div + (double)offset;
}

static esp_err_t merge_prop_number(const char *point_id, const char *prop, double val)
{
    minihub_point_t snap = {0};
    cJSON *props = NULL;
    const char *value = "";
    if (minihub_point_get_snapshot(point_id, &snap) == ESP_OK) {
        value = snap.value;
        props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
    } else {
        props = cJSON_CreateObject();
    }
    if (!props) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(props, prop);
    cJSON_AddNumberToObject(props, prop, val);
    esp_err_t err = minihub_point_set(point_id, value[0] ? value : "off", props);
    cJSON_Delete(props);
    return err;
}

static esp_err_t merge_prop_bool(const char *point_id, const char *prop, bool val)
{
    minihub_point_t snap = {0};
    cJSON *props = NULL;
    const char *value = "";
    if (minihub_point_get_snapshot(point_id, &snap) == ESP_OK) {
        value = snap.value;
        props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
    } else {
        props = cJSON_CreateObject();
    }
    if (!props) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(props, prop);
    cJSON_AddBoolToObject(props, prop, val);
    esp_err_t err = minihub_point_set(point_id, value[0] ? value : "off", props);
    cJSON_Delete(props);
    return err;
}

static void load_param(pp_param_t *p, const cJSON *obj, minihub_pp_transport_t transport)
{
    memset(p, 0, sizeof(*p));
    p->multiply = 1.0f;
    p->divide = 1.0f;
    const cJSON *prop = cJSON_GetObjectItem(obj, "prop");
    const cJSON *label = cJSON_GetObjectItem(obj, "label");
    const cJSON *unit = cJSON_GetObjectItem(obj, "unit");
    const cJSON *access = cJSON_GetObjectItem(obj, "access");
    const cJSON *scale = cJSON_GetObjectItem(obj, "scale");
    const cJSON *source = cJSON_GetObjectItem(obj, "source");
    if (cJSON_IsString(prop)) {
        strncpy(p->prop, prop->valuestring, sizeof(p->prop) - 1);
    }
    if (cJSON_IsString(label)) {
        strncpy(p->label, label->valuestring, sizeof(p->label) - 1);
    }
    if (cJSON_IsString(unit)) {
        strncpy(p->unit, unit->valuestring, sizeof(p->unit) - 1);
    }
    p->writable = cJSON_IsString(access) && strcmp(access->valuestring, "rw") == 0;
    if (cJSON_IsObject(scale)) {
        const cJSON *m = cJSON_GetObjectItem(scale, "multiply");
        const cJSON *d = cJSON_GetObjectItem(scale, "divide");
        const cJSON *o = cJSON_GetObjectItem(scale, "offset");
        if (cJSON_IsNumber(m)) {
            p->multiply = (float)m->valuedouble;
        }
        if (cJSON_IsNumber(d)) {
            p->divide = (float)d->valuedouble;
        }
        if (cJSON_IsNumber(o)) {
            p->offset = (float)o->valuedouble;
        }
    }
    if (!cJSON_IsObject(source)) {
        return;
    }
    if (transport == MINIHUB_PP_TRANSPORT_ZIGBEE) {
        const cJSON *cluster = cJSON_GetObjectItem(source, "cluster");
        const cJSON *attr = cJSON_GetObjectItem(source, "attr");
        const cJSON *type = cJSON_GetObjectItem(source, "type");
        if (cJSON_IsString(cluster)) {
            p->cluster = (uint16_t)parse_u32_flex(cluster->valuestring);
        } else if (cJSON_IsNumber(cluster)) {
            p->cluster = (uint16_t)cluster->valuedouble;
        }
        if (cJSON_IsString(attr)) {
            p->attr_id = (uint16_t)parse_u32_flex(attr->valuestring);
        } else if (cJSON_IsNumber(attr)) {
            p->attr_id = (uint16_t)attr->valuedouble;
        }
        p->zcl_type = cJSON_IsString(type) ? zcl_type_from_str(type->valuestring) : 0x20;
    } else {
        const cJSON *model_id = cJSON_GetObjectItem(source, "model_id");
        const cJSON *company_id = cJSON_GetObjectItem(source, "company_id");
        const cJSON *opcode = cJSON_GetObjectItem(source, "opcode");
        const cJSON *field = cJSON_GetObjectItem(source, "field");
        if (cJSON_IsString(model_id)) {
            p->model_id = (uint16_t)parse_u32_flex(model_id->valuestring);
        } else if (cJSON_IsNumber(model_id)) {
            p->model_id = (uint16_t)model_id->valuedouble;
        }
        if (cJSON_IsNumber(company_id)) {
            p->company_id = (uint16_t)company_id->valuedouble;
        } else if (cJSON_IsString(company_id)) {
            p->company_id = (uint16_t)parse_u32_flex(company_id->valuestring);
        } else {
            p->company_id = 0xFFFF;
        }
        if (cJSON_IsString(opcode)) {
            p->opcode = parse_u32_flex(opcode->valuestring);
        } else if (cJSON_IsNumber(opcode)) {
            p->opcode = (uint32_t)opcode->valuedouble;
        }
        if (cJSON_IsString(field)) {
            strncpy(p->field, field->valuestring, sizeof(p->field) - 1);
        }
    }
}

static void load_quirk_obj(const cJSON *root)
{
    const cJSON *transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport)) {
        return;
    }
    minihub_pp_transport_t tr = 0;
    if (strcmp(transport->valuestring, "zigbee") == 0) {
        tr = MINIHUB_PP_TRANSPORT_ZIGBEE;
    } else if (strcmp(transport->valuestring, "ble_mesh") == 0) {
        tr = MINIHUB_PP_TRANSPORT_BLE_MESH;
    } else {
        return;
    }

    const cJSON *id = cJSON_GetObjectItem(root, "id");
    char id_buf[40] = {0};
    if (cJSON_IsString(id)) {
        strncpy(id_buf, id->valuestring, sizeof(id_buf) - 1);
    }

    pp_quirk_t *q = NULL;
    bool replacing = false;
    if (id_buf[0]) {
        for (size_t i = 0; i < s_quirk_count; i++) {
            if (strcmp(s_quirks[i].id, id_buf) == 0) {
                q = &s_quirks[i];
                replacing = true;
                break;
            }
        }
    }
    if (!q) {
        if (s_quirk_count >= MINIHUB_PP_MAX_QUIRKS) {
            ESP_LOGW(TAG, "quirk table full");
            return;
        }
        q = &s_quirks[s_quirk_count];
    }

    pp_quirk_t tmp = {0};
    tmp.transport = tr;
    if (id_buf[0]) {
        strncpy(tmp.id, id_buf, sizeof(tmp.id) - 1);
    }
    const cJSON *match = cJSON_GetObjectItem(root, "match");
    if (cJSON_IsObject(match)) {
        const cJSON *mfr = cJSON_GetObjectItem(match, "manufacturer");
        const cJSON *model = cJSON_GetObjectItem(match, "model");
        const cJSON *ieee_prefix = cJSON_GetObjectItem(match, "ieee_prefix");
        const cJSON *uuid_prefix = cJSON_GetObjectItem(match, "uuid_prefix");
        if (cJSON_IsString(mfr)) {
            strncpy(tmp.manufacturer, mfr->valuestring, sizeof(tmp.manufacturer) - 1);
        }
        if (cJSON_IsString(model)) {
            strncpy(tmp.model, model->valuestring, sizeof(tmp.model) - 1);
        }
        if (cJSON_IsString(ieee_prefix)) {
            strncpy(tmp.ieee_prefix, ieee_prefix->valuestring, sizeof(tmp.ieee_prefix) - 1);
        }
        if (cJSON_IsString(uuid_prefix)) {
            strncpy(tmp.uuid_prefix, uuid_prefix->valuestring, sizeof(tmp.uuid_prefix) - 1);
        }
    }
    const cJSON *params = cJSON_GetObjectItem(root, "params");
    if (cJSON_IsArray(params)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, params)
        {
            if (tmp.param_count >= MINIHUB_PP_MAX_PARAMS) {
                break;
            }
            load_param(&tmp.params[tmp.param_count], item, tmp.transport);
            if (tmp.params[tmp.param_count].prop[0] == '\0' ||
                prop_reserved(tmp.params[tmp.param_count].prop)) {
                if (prop_reserved(tmp.params[tmp.param_count].prop)) {
                    ESP_LOGW(TAG, "skip reserved prop %s in %s", tmp.params[tmp.param_count].prop,
                             tmp.id);
                }
                continue;
            }
            tmp.param_count++;
        }
    }
    if (tmp.param_count == 0) {
        return;
    }
    tmp.in_use = true;
    *q = tmp;
    if (!replacing) {
        s_quirk_count++;
    }
    ESP_LOGI(TAG, "loaded quirk %s transport=%s params=%u%s", q->id,
             q->transport == MINIHUB_PP_TRANSPORT_ZIGBEE ? "zigbee" : "ble_mesh",
             (unsigned)q->param_count, replacing ? " (override)" : "");
}

static void load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) {
        fclose(f);
        return;
    }
    char *buf = calloc(1, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "parse fail %s", path);
        return;
    }
    if (cJSON_IsArray(root)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, root)
        {
            load_quirk_obj(item);
        }
    } else {
        load_quirk_obj(root);
    }
    cJSON_Delete(root);
}

static void load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".json") != 0) {
            continue;
        }
        char path[320];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            load_file(path);
        }
    }
    closedir(d);
}

esp_err_t minihub_product_params_reload(void)
{
    memset(s_quirks, 0, sizeof(s_quirks));
    s_quirk_count = 0;
    load_dir("/store/preinstall/product_params");
    load_dir("/data/config/product_params");
    ESP_LOGI(TAG, "quirks loaded: %u", (unsigned)s_quirk_count);
    return ESP_OK;
}

esp_err_t minihub_product_params_init(void)
{
    static bool s_inited;
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = minihub_product_params_reload();
    if (err == ESP_OK) {
        s_inited = true;
    }
    return err;
}

size_t minihub_pp_zigbee_list_params(uint64_t ieee, const char *manufacturer, const char *model,
                                     minihub_pp_zb_param_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < s_quirk_count && n < cap; i++) {
        if (!quirk_match_zigbee(&s_quirks[i], ieee, manufacturer, model)) {
            continue;
        }
        for (size_t j = 0; j < s_quirks[i].param_count && n < cap; j++) {
            const pp_param_t *p = &s_quirks[i].params[j];
            minihub_pp_zb_param_t *d = &out[n++];
            memset(d, 0, sizeof(*d));
            strncpy(d->prop, p->prop, sizeof(d->prop) - 1);
            strncpy(d->label, p->label, sizeof(d->label) - 1);
            strncpy(d->unit, p->unit, sizeof(d->unit) - 1);
            d->writable = p->writable;
            d->cluster = p->cluster;
            d->attr_id = p->attr_id;
            d->zcl_type = p->zcl_type;
            d->multiply = p->multiply;
            d->divide = p->divide;
            d->offset = p->offset;
        }
        break; /* first quirk wins */
    }
    return n;
}

bool minihub_pp_zigbee_apply(const char *point_id, uint64_t ieee, const char *manufacturer,
                             const char *model, uint16_t cluster, uint16_t attr_id, uint8_t zcl_type,
                             const void *value, size_t value_len)
{
    if (!point_id || !value) {
        return false;
    }
    minihub_pp_zb_param_t list[MINIHUB_PP_MAX_PARAMS];
    size_t n = minihub_pp_zigbee_list_params(ieee, manufacturer, model, list, MINIHUB_PP_MAX_PARAMS);
    for (size_t i = 0; i < n; i++) {
        if (list[i].cluster != cluster || list[i].attr_id != attr_id) {
            continue;
        }
        uint8_t ty = zcl_type ? zcl_type : list[i].zcl_type;
        double raw = raw_to_number(ty, value, value_len);
        double scaled = apply_scale(raw, list[i].multiply, list[i].divide, list[i].offset);
        ESP_LOGI(TAG, "zb apply %s %s=%.4f (raw=%.4f cluster=0x%04x attr=0x%04x)", point_id,
                 list[i].prop, scaled, raw, cluster, attr_id);
        if (ty == 0x10) {
            (void)merge_prop_bool(point_id, list[i].prop, scaled != 0.0);
        } else {
            (void)merge_prop_number(point_id, list[i].prop, scaled);
        }
        return true;
    }
    return false;
}

size_t minihub_pp_ble_mesh_list_params(const char *uuid_hex, minihub_pp_bm_param_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < s_quirk_count && n < cap; i++) {
        if (!quirk_match_ble(&s_quirks[i], uuid_hex)) {
            continue;
        }
        for (size_t j = 0; j < s_quirks[i].param_count && n < cap; j++) {
            const pp_param_t *p = &s_quirks[i].params[j];
            minihub_pp_bm_param_t *d = &out[n++];
            memset(d, 0, sizeof(*d));
            strncpy(d->prop, p->prop, sizeof(d->prop) - 1);
            strncpy(d->label, p->label, sizeof(d->label) - 1);
            strncpy(d->unit, p->unit, sizeof(d->unit) - 1);
            d->writable = p->writable;
            d->model_id = p->model_id;
            d->company_id = p->company_id;
            d->opcode = p->opcode;
            strncpy(d->field, p->field, sizeof(d->field) - 1);
            d->multiply = p->multiply;
            d->divide = p->divide;
            d->offset = p->offset;
        }
        break;
    }
    return n;
}

bool minihub_pp_ble_mesh_apply(const char *point_id, const char *uuid_hex, uint16_t model_id,
                               uint16_t company_id, uint32_t opcode, const char *field, double raw)
{
    if (!point_id || !uuid_hex) {
        return false;
    }
    minihub_pp_bm_param_t list[MINIHUB_PP_MAX_PARAMS];
    size_t n = minihub_pp_ble_mesh_list_params(uuid_hex, list, MINIHUB_PP_MAX_PARAMS);
    for (size_t i = 0; i < n; i++) {
        if (list[i].model_id != model_id || list[i].company_id != company_id) {
            continue;
        }
        if (opcode && list[i].opcode && list[i].opcode != opcode) {
            continue;
        }
        if (field && list[i].field[0] && strcmp(list[i].field, field) != 0) {
            continue;
        }
        double scaled = apply_scale(raw, list[i].multiply, list[i].divide, list[i].offset);
        ESP_LOGI(TAG, "bm apply %s %s=%.4f", point_id, list[i].prop, scaled);
        (void)merge_prop_number(point_id, list[i].prop, scaled);
        return true;
    }
    return false;
}

esp_err_t minihub_pp_set_prop(const char *point_id, const char *prop, const cJSON *value,
                              minihub_pp_zb_param_t *out_zb, bool *is_zigbee)
{
    if (!point_id || !prop || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_zigbee) {
        *is_zigbee = false;
    }
    /* Resolve via zigbee ieee from point registry is caller's job; scan all quirks for prop. */
    for (size_t i = 0; i < s_quirk_count; i++) {
        for (size_t j = 0; j < s_quirks[i].param_count; j++) {
            pp_param_t *p = &s_quirks[i].params[j];
            if (strcmp(p->prop, prop) != 0) {
                continue;
            }
            if (!p->writable) {
                return ESP_ERR_NOT_ALLOWED;
            }
            if (s_quirks[i].transport == MINIHUB_PP_TRANSPORT_ZIGBEE) {
                if (out_zb) {
                    memset(out_zb, 0, sizeof(*out_zb));
                    strncpy(out_zb->prop, p->prop, sizeof(out_zb->prop) - 1);
                    out_zb->writable = true;
                    out_zb->cluster = p->cluster;
                    out_zb->attr_id = p->attr_id;
                    out_zb->zcl_type = p->zcl_type;
                    out_zb->multiply = p->multiply;
                    out_zb->divide = p->divide;
                    out_zb->offset = p->offset;
                }
                if (is_zigbee) {
                    *is_zigbee = true;
                }
                /* Optimistic local echo */
                if (cJSON_IsBool(value)) {
                    (void)merge_prop_bool(point_id, prop, cJSON_IsTrue(value));
                } else if (cJSON_IsNumber(value)) {
                    (void)merge_prop_number(point_id, prop, value->valuedouble);
                }
                return ESP_OK;
            }
            /* BLE Mesh write: echo only in v1 until opcode writer lands */
            if (cJSON_IsBool(value)) {
                (void)merge_prop_bool(point_id, prop, cJSON_IsTrue(value));
            } else if (cJSON_IsNumber(value)) {
                (void)merge_prop_number(point_id, prop, value->valuedouble);
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

cJSON *minihub_pp_meta_for_point(const char *point_id, uint64_t ieee, const char *uuid_hex)
{
    (void)point_id;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }
    if (ieee) {
        minihub_pp_zb_param_t list[MINIHUB_PP_MAX_PARAMS];
        size_t n = minihub_pp_zigbee_list_params(ieee, NULL, NULL, list, MINIHUB_PP_MAX_PARAMS);
        for (size_t i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "prop", list[i].prop);
            cJSON_AddStringToObject(o, "label", list[i].label[0] ? list[i].label : list[i].prop);
            cJSON_AddStringToObject(o, "unit", list[i].unit);
            cJSON_AddStringToObject(o, "access", list[i].writable ? "rw" : "r");
            cJSON_AddItemToArray(arr, o);
        }
    }
    if (uuid_hex && uuid_hex[0]) {
        minihub_pp_bm_param_t list[MINIHUB_PP_MAX_PARAMS];
        size_t n = minihub_pp_ble_mesh_list_params(uuid_hex, list, MINIHUB_PP_MAX_PARAMS);
        for (size_t i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "prop", list[i].prop);
            cJSON_AddStringToObject(o, "label", list[i].label[0] ? list[i].label : list[i].prop);
            cJSON_AddStringToObject(o, "unit", list[i].unit);
            cJSON_AddStringToObject(o, "access", list[i].writable ? "rw" : "r");
            cJSON_AddItemToArray(arr, o);
        }
    }
    return arr;
}
