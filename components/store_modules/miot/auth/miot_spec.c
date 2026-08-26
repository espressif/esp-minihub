/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fetch MIoT-Spec-V2 profiles during sync; cache slim JSON under /data/config/miot_spec/.
 */

#include "miot_spec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "miot_http.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "miot_spec";

#define SPEC_INDEX_PATH    "/data/config/miot_spec_index.json"
#define SPEC_MODEL_PREFIX  "/data/config/miot_spec_"
#define SPEC_INSTANCES_URL "https://miot-spec.org/miot-spec-v2/instances?status=released"
#define SPEC_INSTANCE_FMT  "https://miot-spec.org/miot-spec-v2/instance?type=%s"
#define SPEC_SCAN_WIN      24576
#define SPEC_URN_SCAN      256
#define SPEC_MAX_MODELS    64
/* Optional: MiniHub Store LAN helper — POST /api/miot-spec/resolve {"models":["…"]} */

static void model_path(const char *model, char *out, size_t out_len)
{
    /* Flat file under /data/config (no mkdir — .mhub cannot reliably export mkdir). */
    snprintf(out, out_len, "%s%s.json", SPEC_MODEL_PREFIX, model);
}

const char *miot_spec_hub_type(const char *spec_device)
{
    if (!spec_device || !spec_device[0]) {
        return "switch";
    }
    if (strcmp(spec_device, "intelligent-speaker") == 0 ||
        strcmp(spec_device, "speaker") == 0) {
        return "speaker";
    }
    if (strcmp(spec_device, "curtain") == 0 || strcmp(spec_device, "airer") == 0 ||
        strcmp(spec_device, "window-opener") == 0) {
        return "cover";
    }
    if (strcmp(spec_device, "magnet-sensor") == 0 ||
        strcmp(spec_device, "motion-sensor") == 0) {
        return "binary_sensor";
    }
    if (strcmp(spec_device, "smoke-sensor") == 0) {
        return "smoke";
    }
    if (strcmp(spec_device, "remote-control") == 0) {
        return "button";
    }
    if (strcmp(spec_device, "outlet") == 0) {
        return "switch";
    }
    /* camera, television, lock, light, switch, router, … fit MINIHUB_TYPE_MAX */
    if (strlen(spec_device) >= 16) {
        return "switch";
    }
    return spec_device;
}

bool miot_spec_device_cloud_only(const char *spec_device)
{
    if (!spec_device) {
        return false;
    }
    return strcmp(spec_device, "camera") == 0 ||
           strcmp(spec_device, "television") == 0 ||
           strcmp(spec_device, "projector") == 0 ||
           strcmp(spec_device, "tv-box") == 0 ||
           strcmp(spec_device, "lock") == 0 ||
           strcmp(spec_device, "speaker") == 0 ||
           strcmp(spec_device, "intelligent-speaker") == 0 ||
           strcmp(spec_device, "video-doorbell") == 0;
}

static bool parse_device_from_urn(const char *urn, char *out, size_t out_len)
{
    /* urn:miot-spec-v2:device:<type>:… */
    if (!urn || !out || out_len < 2) {
        return false;
    }
    const char *p = strstr(urn, ":device:");
    if (!p) {
        return false;
    }
    p += strlen(":device:");
    size_t n = 0;
    while (p[n] && p[n] != ':' && n + 1 < out_len) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static const char *prop_name_from_type(const char *type_urn)
{
    /* …:property:<name>:<hex>:… */
    if (!type_urn) {
        return NULL;
    }
    const char *p = strstr(type_urn, ":property:");
    if (!p) {
        return NULL;
    }
    p += strlen(":property:");
    return p;
}

static const char *action_name_from_type(const char *type_urn)
{
    /* …:action:<name>:<hex>:… */
    if (!type_urn) {
        return NULL;
    }
    const char *p = strstr(type_urn, ":action:");
    if (!p) {
        return NULL;
    }
    p += strlen(":action:");
    return p;
}

static const char *svc_name_from_type(const char *type_urn)
{
    /* …:service:<name>:<hex>:… */
    if (!type_urn) {
        return NULL;
    }
    const char *p = strstr(type_urn, ":service:");
    if (!p) {
        return NULL;
    }
    p += strlen(":service:");
    return p;
}

static void copy_urn_name(const char *raw, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw) {
        return;
    }
    size_t i = 0;
    while (raw[i] && raw[i] != ':' && i + 1 < out_len) {
        out[i] = raw[i];
        i++;
    }
    out[i] = '\0';
}

/** Prefer camera-control.on over indicator-light; skip doorbell/etc. */
static int on_service_rank(const char *device, const char *svc)
{
    if (!svc || !svc[0]) {
        return 0;
    }
    if (strcmp(svc, "indicator-light") == 0 || strcmp(svc, "doorbell") == 0 ||
        strcmp(svc, "alarm") == 0 || strcmp(svc, "air-conditioner-alarm") == 0) {
        return -1;
    }
    if (device && strcmp(device, "camera") == 0 &&
        (strcmp(svc, "camera-control") == 0 || strcmp(svc, "camera") == 0)) {
        return 100;
    }
    if (device && strcmp(device, "television") == 0 && strcmp(svc, "television") == 0) {
        return 100;
    }
    if (device && strcmp(device, "lock") == 0 &&
        (strcmp(svc, "lock") == 0 || strcmp(svc, "door-lock") == 0)) {
        return 100;
    }
    if (device && (strcmp(device, "light") == 0 || strcmp(device, "outlet") == 0 ||
                   strcmp(device, "switch") == 0) &&
        (strcmp(svc, "light") == 0 || strcmp(svc, "switch") == 0 ||
         strcmp(svc, "outlet") == 0)) {
        return 100;
    }
    if (strcmp(svc, "screen") == 0 || strcmp(svc, "indicator-light") == 0) {
        return 5; /* Prefer primary switch/camera-control over screen/LED */
    }
    if (strcmp(svc, "door-lock-camera") == 0) {
        return 5; /* lock cam switch — better than doorbell, worse than lock */
    }
    return 10;
}

static bool str_case_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) {
        return false;
    }
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               ((unsigned char)p[i] == (unsigned char)needle[i] ||
                (p[i] >= 'A' && p[i] <= 'Z' && p[i] + 32 == needle[i]) ||
                (p[i] >= 'a' && p[i] <= 'z' && p[i] - 32 == needle[i]))) {
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

static void parse_motor_value_list(const cJSON *prop, miot_spec_profile_t *out)
{
    const cJSON *vl = cJSON_GetObjectItem(prop, "value-list");
    if (!cJSON_IsArray(vl)) {
        return;
    }
    cJSON *ent = NULL;
    cJSON_ArrayForEach(ent, vl) {
        const cJSON *val_j = cJSON_GetObjectItem(ent, "value");
        const cJSON *desc_j = cJSON_GetObjectItem(ent, "description");
        if (!cJSON_IsNumber(val_j) || !cJSON_IsString(desc_j)) {
            continue;
        }
        int val = val_j->valueint;
        if (val < 0 || val > 255) {
            continue;
        }
        const char *desc = desc_j->valuestring;
        if (str_case_contains(desc, "Open")) {
            out->motor_open = (uint8_t)val;
        } else if (str_case_contains(desc, "Close")) {
            out->motor_close = (uint8_t)val;
        } else if (str_case_contains(desc, "Pause")) {
            out->motor_pause = (uint8_t)val;
        }
    }
}

static bool profile_is_cover_device(const char *device)
{
    if (!device || !device[0]) {
        return false;
    }
    return strcmp(device, "curtain") == 0 || strcmp(device, "airer") == 0 ||
           strcmp(device, "window-opener") == 0;
}

static void profile_add_num_if_nonzero(cJSON *o, const char *key, int val)
{
    if (val != 0) {
        cJSON_AddNumberToObject(o, key, val);
    }
}

static void profile_add_u8_if_nonzero(cJSON *o, const char *key, uint8_t val)
{
    if (val != 0) {
        cJSON_AddNumberToObject(o, key, val);
    }
}

static void profile_load_int_field(const cJSON *root, const char *key, int *out)
{
    const cJSON *j = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(j)) {
        *out = j->valueint;
    }
}

static void profile_load_u8_field(const cJSON *root, const char *key, uint8_t *out)
{
    const cJSON *j = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= 255) {
        *out = (uint8_t)j->valueint;
    }
}

typedef struct {
    const char *key;
    const char *kind;
    bool is_action;
    const char *h1;
    const char *h2;
} spec_extra_map_t;

/* Substring, case-insensitive; exact name scores higher than a partial hit. */
static int extra_hint_score(const char *name, const spec_extra_map_t *spec)
{
    if (!name || !name[0] || !spec) {
        return 0;
    }
    int best = 0;
    const char *hints[2] = { spec->h1, spec->h2 };
    for (int i = 0; i < 2; i++) {
        const char *hint = hints[i];
        if (!hint || !hint[0] || !str_case_contains(name, hint)) {
            continue;
        }
        size_t nlen = 0;
        while (name[nlen] && name[nlen] != ':') {
            nlen++;
        }
        int sc = (nlen == strlen(hint)) ? 2 : 1;
        if (sc > best) {
            best = sc;
        }
    }
    return best;
}

static bool extra_key_exists(const miot_spec_profile_t *p, const char *key)
{
    if (!p || !key) {
        return false;
    }
    for (size_t i = 0; i < p->extras_n; i++) {
        if (strcmp(p->extras[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

static bool profile_add_extra(miot_spec_profile_t *out, const char *key, const char *kind,
                              int siid, int piid, int aiid)
{
    if (!out || !key || !key[0] || !kind || !kind[0]) {
        return false;
    }
    if (out->extras_n >= MIOT_SPEC_EXTRAS_MAX || extra_key_exists(out, key)) {
        return false;
    }
    miot_spec_extra_t *e = &out->extras[out->extras_n];
    memset(e, 0, sizeof(*e));
    strncpy(e->key, key, sizeof(e->key) - 1);
    strncpy(e->kind, kind, sizeof(e->kind) - 1);
    e->siid = siid;
    e->piid = piid;
    e->aiid = aiid;
    out->extras_n++;
    return true;
}

static cJSON *extras_array_to_json(const miot_spec_extra_t *extras, size_t n)
{
    if (!extras || n == 0) {
        return NULL;
    }
    if (n > MIOT_SPEC_EXTRAS_MAX) {
        n = MIOT_SPEC_EXTRAS_MAX;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(o, "key", extras[i].key);
        cJSON_AddStringToObject(o, "kind", extras[i].kind);
        cJSON_AddNumberToObject(o, "siid", extras[i].siid);
        cJSON_AddNumberToObject(o, "piid", extras[i].piid);
        cJSON_AddNumberToObject(o, "aiid", extras[i].aiid);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static void extras_array_from_json(const cJSON *arr, miot_spec_profile_t *out)
{
    if (!cJSON_IsArray(arr) || !out) {
        return;
    }
    const cJSON *ent = NULL;
    cJSON_ArrayForEach(ent, arr) {
        if (out->extras_n >= MIOT_SPEC_EXTRAS_MAX) {
            break;
        }
        if (!cJSON_IsObject(ent)) {
            continue;
        }
        const cJSON *key = cJSON_GetObjectItem(ent, "key");
        const cJSON *kind = cJSON_GetObjectItem(ent, "kind");
        if (!cJSON_IsString(key) || !key->valuestring || !key->valuestring[0] ||
            !cJSON_IsString(kind) || !kind->valuestring || !kind->valuestring[0]) {
            continue;
        }
        if (strcmp(key->valuestring, "motion_tracking") == 0) {
            continue;
        }
        miot_spec_extra_t *e = &out->extras[out->extras_n];
        memset(e, 0, sizeof(*e));
        strncpy(e->key, key->valuestring, sizeof(e->key) - 1);
        strncpy(e->kind, kind->valuestring, sizeof(e->kind) - 1);
        const cJSON *siid = cJSON_GetObjectItem(ent, "siid");
        const cJSON *piid = cJSON_GetObjectItem(ent, "piid");
        const cJSON *aiid = cJSON_GetObjectItem(ent, "aiid");
        e->siid = cJSON_IsNumber(siid) ? siid->valueint : 0;
        e->piid = cJSON_IsNumber(piid) ? piid->valueint : 0;
        e->aiid = cJSON_IsNumber(aiid) ? aiid->valueint : 0;
        out->extras_n++;
    }
}

static void harvest_profile_extras(const cJSON *root, miot_spec_profile_t *out)
{
    static const spec_extra_map_t maps[] = {
        { "indicator", "switch", false, "indicator-light", "indicator" },
        { "motion_detection", "switch", false, "motion-detection", NULL },
        /* 米家「人形联动追踪」= Spec human-tracking（C500: siid 2 / piid 9）。
         * motion-tracking（2/8）在实机上无效果，不再暴露。 */
        { "human_tracking", "switch", false, "human-tracking", NULL },
        { "time_watermark", "switch", false, "time-watermark", NULL },
        { "wdr", "switch", false, "wide-dynamic-range", "wdr-mode" },
        { "volume", "number", false, "volume", NULL },
        { "dnd", "switch", false, "no-disturb", "do-not-disturb" },
        { "play_text", "action", true, "play-text", "play_text" },
        { "execute_text", "action", true, "execute-text-directive", "text-directive" },
    };

    if (!root || !out) {
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 8192) {
        ESP_LOGW(TAG, "skip extras harvest (internal heap < 8192)");
        return;
    }
    const cJSON *services = cJSON_GetObjectItem(root, "services");
    if (!cJSON_IsArray(services)) {
        return;
    }

    for (size_t mi = 0; mi < sizeof(maps) / sizeof(maps[0]); mi++) {
        if (out->extras_n >= MIOT_SPEC_EXTRAS_MAX) {
            break;
        }
        const spec_extra_map_t *spec = &maps[mi];
        if (strcmp(spec->key, "volume") == 0 && out->piid_volume > 0) {
            continue;
        }
        int best = 0;
        int best_siid = 0;
        int best_piid = 0;
        int best_aiid = 0;
        cJSON *svc = NULL;
        cJSON_ArrayForEach(svc, services) {
            const cJSON *siid_j = cJSON_GetObjectItem(svc, "iid");
            if (!cJSON_IsNumber(siid_j)) {
                continue;
            }
            int siid = siid_j->valueint;
            char svc_name[48];
            svc_name[0] = '\0';
            const cJSON *st = cJSON_GetObjectItem(svc, "type");
            if (cJSON_IsString(st)) {
                copy_urn_name(svc_name_from_type(st->valuestring), svc_name, sizeof(svc_name));
            }
            if (spec->is_action) {
                const cJSON *actions = cJSON_GetObjectItem(svc, "actions");
                if (!cJSON_IsArray(actions)) {
                    continue;
                }
                cJSON *act = NULL;
                cJSON_ArrayForEach(act, actions) {
                    const cJSON *aiid_j = cJSON_GetObjectItem(act, "iid");
                    const cJSON *at = cJSON_GetObjectItem(act, "type");
                    if (!cJSON_IsNumber(aiid_j) || !cJSON_IsString(at)) {
                        continue;
                    }
                    char name[48];
                    copy_urn_name(action_name_from_type(at->valuestring), name, sizeof(name));
                    int sc = extra_hint_score(name, spec);
                    if (sc > best) {
                        best = sc;
                        best_siid = siid;
                        best_piid = 0;
                        best_aiid = aiid_j->valueint;
                    }
                }
            } else {
                const cJSON *props = cJSON_GetObjectItem(svc, "properties");
                if (!cJSON_IsArray(props)) {
                    continue;
                }
                int svc_sc = extra_hint_score(svc_name, spec);
                cJSON *prop = NULL;
                cJSON_ArrayForEach(prop, props) {
                    const cJSON *piid_j = cJSON_GetObjectItem(prop, "iid");
                    const cJSON *pt = cJSON_GetObjectItem(prop, "type");
                    if (!cJSON_IsNumber(piid_j) || !cJSON_IsString(pt)) {
                        continue;
                    }
                    char name[48];
                    copy_urn_name(prop_name_from_type(pt->valuestring), name, sizeof(name));
                    int sc = extra_hint_score(name, spec);
                    if (strcmp(name, "on") == 0 && svc_sc > sc) {
                        sc = svc_sc;
                    }
                    if (sc > best) {
                        best = sc;
                        best_siid = siid;
                        best_piid = piid_j->valueint;
                        best_aiid = 0;
                    }
                }
            }
        }
        if (best > 0) {
            profile_add_extra(out, spec->key, spec->kind, best_siid, best_piid, best_aiid);
        }
    }
    if (out->extras_n > 0) {
        ESP_LOGI(TAG, "Spec extras %s n=%u", out->model, (unsigned)out->extras_n);
    }
}

static void fill_profile_from_instance(cJSON *root, const char *model, miot_spec_profile_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(root) || !model) {
        return;
    }
    strncpy(out->model, model, sizeof(out->model) - 1);
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        strncpy(out->urn, type->valuestring, sizeof(out->urn) - 1);
        parse_device_from_urn(type->valuestring, out->device, sizeof(out->device));
    }

    out->siid = 2;
    out->piid_on = 0;

    int best_on_rank = -1;
    int sleep_siid = 0;
    int sleep_piid = 0;
    int volume_siid = 0;
    int volume_piid = 0;
    int door_siid = 0;
    int door_piid = 0;

    const cJSON *services = cJSON_GetObjectItem(root, "services");
    if (!cJSON_IsArray(services)) {
        out->valid = out->device[0] != '\0';
        return;
    }

    cJSON *svc = NULL;
    cJSON_ArrayForEach(svc, services) {
        const cJSON *siid_j = cJSON_GetObjectItem(svc, "iid");
        const cJSON *st = cJSON_GetObjectItem(svc, "type");
        const cJSON *props = cJSON_GetObjectItem(svc, "properties");
        if (!cJSON_IsNumber(siid_j) || !cJSON_IsArray(props)) {
            continue;
        }
        int siid = siid_j->valueint;
        char svc_name[48];
        svc_name[0] = '\0';
        if (cJSON_IsString(st)) {
            copy_urn_name(svc_name_from_type(st->valuestring), svc_name, sizeof(svc_name));
        }
        cJSON *prop = NULL;
        cJSON_ArrayForEach(prop, props) {
            const cJSON *piid_j = cJSON_GetObjectItem(prop, "iid");
            const cJSON *pt = cJSON_GetObjectItem(prop, "type");
            if (!cJSON_IsNumber(piid_j) || !cJSON_IsString(pt)) {
                continue;
            }
            char name[48];
            copy_urn_name(prop_name_from_type(pt->valuestring), name, sizeof(name));
            if (!name[0]) {
                continue;
            }
            int piid = piid_j->valueint;
            bool is_cover = profile_is_cover_device(out->device);
            if (strcmp(name, "on") == 0 && !is_cover &&
                strcmp(out->device, "lock") != 0 &&
                strcmp(out->device, "magnet-sensor") != 0) {
                int rank = on_service_rank(out->device, svc_name);
                if (rank > best_on_rank) {
                    best_on_rank = rank;
                    out->siid = siid;
                    out->piid_on = piid;
                    out->invert_on = false;
                }
            } else if (strcmp(name, "contact-state") == 0) {
                /* Door/window magnets: primary Hub value (cloud prop/get). */
                out->siid = siid;
                out->piid_on = piid;
                out->invert_on = false;
                best_on_rank = 200;
            } else if (strcmp(name, "illumination") == 0) {
                out->piid_illum = piid;
            } else if (strcmp(name, "no-motion-duration") == 0) {
                out->piid_no_motion = piid;
            } else if (strcmp(name, "battery-level") == 0) {
                out->siid_battery = siid;
                out->piid_battery = piid;
            } else if (strcmp(name, "video-codec") == 0) {
                out->siid_stream = siid;
                out->piid_video_codec = piid;
            } else if (strcmp(name, "audio-codec") == 0) {
                if (out->siid_stream <= 0) {
                    out->siid_stream = siid;
                }
                out->piid_audio_codec = piid;
            } else if (strcmp(name, "sleep-mode") == 0) {
                if (sleep_piid <= 0 || strcmp(svc_name, "television") == 0) {
                    sleep_siid = siid;
                    sleep_piid = piid;
                }
            } else if (strcmp(name, "volume") == 0) {
                if (volume_piid <= 0 || strcmp(svc_name, "speaker") == 0) {
                    volume_siid = siid;
                    volume_piid = piid;
                }
            } else if (strcmp(name, "door-state") == 0) {
                if (door_piid <= 0) {
                    door_siid = siid;
                    door_piid = piid;
                }
            } else if (strcmp(name, "brightness") == 0) {
                if (out->piid_on <= 0) {
                    out->siid = siid;
                }
                out->piid_bright = piid;
                out->supports_brightness = true;
                {
                    const cJSON *vr = cJSON_GetObjectItem(prop, "value-range");
                    if (cJSON_IsArray(vr) && cJSON_GetArraySize(vr) >= 2) {
                        const cJSON *mx = cJSON_GetArrayItem(vr, 1);
                        if (cJSON_IsNumber(mx) && mx->valuedouble >= 1.0) {
                            out->bright_max = (int)mx->valuedouble;
                        }
                    }
                }
            } else if (strcmp(name, "color-temperature") == 0) {
                if (out->piid_on <= 0) {
                    out->siid = siid;
                }
                out->piid_ct = piid;
                out->supports_color_temp = true;
            } else if (strcmp(name, "color") == 0) {
                if (out->piid_on <= 0) {
                    out->siid = siid;
                }
                out->piid_color = piid;
                out->supports_color = true;
            } else if (strcmp(name, "mode") == 0) {
                if (out->piid_on <= 0 && out->piid_position <= 0) {
                    out->siid = siid;
                }
                out->piid_mode = piid;
            } else if (strcmp(name, "target-temperature") == 0) {
                if (out->piid_on <= 0) {
                    out->siid = siid;
                }
                out->piid_target_temp = piid;
            } else if (strcmp(name, "fan-level") == 0) {
                out->siid_fan = siid;
                out->piid_fan = piid;
            } else if (strcmp(name, "temperature") == 0) {
                out->siid_env = siid;
                out->piid_temp = piid;
            } else if (strcmp(name, "power-consumption") == 0) {
                out->siid_power = siid;
                out->piid_energy = piid;
            } else if (strcmp(name, "electric-current") == 0) {
                out->siid_power = siid;
                out->piid_current = piid;
            } else if (strcmp(name, "electric-power") == 0) {
                out->siid_power = siid;
                out->piid_power = piid;
            } else if (strcmp(name, "voltage") == 0) {
                out->siid_power = siid;
                out->piid_voltage = piid;
            } else if (strcmp(name, "fault") == 0) {
                if (out->piid_fault <= 0) {
                    out->piid_fault = piid;
                }
            } else if (strcmp(name, "current-position") == 0) {
                out->siid = siid;
                out->piid_position = piid;
            } else if (strcmp(name, "target-position") == 0) {
                out->siid = siid;
                out->piid_target_position = piid;
            } else if (strcmp(name, "motor-control") == 0) {
                out->siid_motor = siid;
                out->piid_motor = piid;
                parse_motor_value_list(prop, out);
            }
        }
    }
    if (profile_is_cover_device(out->device)) {
        out->piid_on = 0;
        out->invert_on = false;
    } else if (out->piid_on <= 0) {
        /* TV/projector: sleep-mode as power proxy (asleep ⇒ off). */
        if (sleep_piid > 0 &&
            (strcmp(out->device, "television") == 0 ||
             strcmp(out->device, "projector") == 0 ||
             strcmp(out->device, "tv-box") == 0)) {
            out->siid = sleep_siid;
            out->piid_on = sleep_piid;
            out->invert_on = true;
        } else if (strcmp(out->device, "speaker") == 0 ||
                   strcmp(out->device, "intelligent-speaker") == 0) {
            if (volume_piid > 0) {
                out->siid = volume_siid;
                out->piid_volume = volume_piid;
            }
            out->piid_on = 0;
        } else if (strcmp(out->device, "lock") == 0) {
            if (door_piid > 0) {
                out->siid = door_siid;
                out->piid_door = door_piid;
            }
            out->piid_on = 0;
        } else if (strcmp(out->device, "motion-sensor") == 0 ||
                   strcmp(out->device, "remote-control") == 0) {
            /* Event-only / occupancy-via-event — no writable primary on. */
            out->piid_on = 0;
        }
    }
    if (volume_piid > 0 && out->piid_volume <= 0 &&
        (strcmp(out->device, "speaker") == 0 ||
         strcmp(out->device, "intelligent-speaker") == 0)) {
        out->piid_volume = volume_piid;
    }
    if (door_piid > 0 && out->piid_door <= 0 && strcmp(out->device, "lock") == 0) {
        out->piid_door = door_piid;
    }
    harvest_profile_extras(root, out);
    out->profile_rev = 11; /* rev11: drop no-op motion-tracking extra */
    out->valid = out->device[0] != '\0';
}

static esp_err_t write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? ESP_OK : ESP_FAIL;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 512 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = calloc(1, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_len) {
        *out_len = (size_t)sz;
    }
    return buf;
}

static esp_err_t save_slim_profile(const miot_spec_profile_t *p)
{
    if (!p || !p->valid || !p->model[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(o, "model", p->model);
    cJSON_AddStringToObject(o, "urn", p->urn);
    cJSON_AddStringToObject(o, "device", p->device);
    cJSON_AddNumberToObject(o, "siid", p->siid);
    cJSON_AddNumberToObject(o, "piid_on", p->piid_on);
    cJSON_AddNumberToObject(o, "piid_bright", p->piid_bright);
    profile_add_num_if_nonzero(o, "bright_max", p->bright_max);
    cJSON_AddNumberToObject(o, "piid_ct", p->piid_ct);
    cJSON_AddNumberToObject(o, "piid_color", p->piid_color);
    cJSON_AddBoolToObject(o, "supports_brightness", p->supports_brightness);
    cJSON_AddBoolToObject(o, "supports_color_temp", p->supports_color_temp);
    cJSON_AddBoolToObject(o, "supports_color", p->supports_color);
    profile_add_num_if_nonzero(o, "siid_fan", p->siid_fan);
    profile_add_num_if_nonzero(o, "piid_fan", p->piid_fan);
    profile_add_num_if_nonzero(o, "siid_env", p->siid_env);
    profile_add_num_if_nonzero(o, "piid_temp", p->piid_temp);
    profile_add_num_if_nonzero(o, "piid_mode", p->piid_mode);
    profile_add_num_if_nonzero(o, "piid_target_temp", p->piid_target_temp);
    profile_add_num_if_nonzero(o, "piid_position", p->piid_position);
    profile_add_num_if_nonzero(o, "piid_target_position", p->piid_target_position);
    profile_add_num_if_nonzero(o, "siid_motor", p->siid_motor);
    profile_add_num_if_nonzero(o, "piid_motor", p->piid_motor);
    profile_add_u8_if_nonzero(o, "motor_open", p->motor_open);
    profile_add_u8_if_nonzero(o, "motor_close", p->motor_close);
    profile_add_u8_if_nonzero(o, "motor_pause", p->motor_pause);
    if (p->invert_on) {
        cJSON_AddBoolToObject(o, "invert_on", true);
    }
    profile_add_num_if_nonzero(o, "piid_volume", p->piid_volume);
    profile_add_num_if_nonzero(o, "piid_door", p->piid_door);
    profile_add_num_if_nonzero(o, "piid_illum", p->piid_illum);
    profile_add_num_if_nonzero(o, "siid_battery", p->siid_battery);
    profile_add_num_if_nonzero(o, "piid_battery", p->piid_battery);
    profile_add_num_if_nonzero(o, "siid_stream", p->siid_stream);
    profile_add_num_if_nonzero(o, "piid_video_codec", p->piid_video_codec);
    profile_add_num_if_nonzero(o, "piid_audio_codec", p->piid_audio_codec);
    profile_add_num_if_nonzero(o, "siid_power", p->siid_power);
    profile_add_num_if_nonzero(o, "piid_energy", p->piid_energy);
    profile_add_num_if_nonzero(o, "piid_current", p->piid_current);
    profile_add_num_if_nonzero(o, "piid_power", p->piid_power);
    profile_add_num_if_nonzero(o, "piid_voltage", p->piid_voltage);
    profile_add_num_if_nonzero(o, "piid_fault", p->piid_fault);
    if (p->extras_n > 0) {
        cJSON *arr = extras_array_to_json(p->extras, p->extras_n);
        if (arr) {
            cJSON_AddItemToObject(o, "extras", arr);
        }
    }
    cJSON_AddNumberToObject(o, "profile_rev", p->profile_rev > 0 ? p->profile_rev : 11);

    char *printed = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    char path[160];
    model_path(p->model, path, sizeof(path));
    esp_err_t err = write_file(path, printed, strlen(printed));
    free(printed);
    return err;
}

esp_err_t miot_spec_load_profile(const char *model, miot_spec_profile_t *out)
{
    if (!model || !model[0] || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[160];
    model_path(model, path, sizeof(path));
    char *body = read_file(path, NULL);
    if (!body) {
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return ESP_FAIL;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->model, model, sizeof(out->model) - 1);
    const cJSON *urn = cJSON_GetObjectItem(root, "urn");
    const cJSON *device = cJSON_GetObjectItem(root, "device");
    if (cJSON_IsString(urn)) {
        strncpy(out->urn, urn->valuestring, sizeof(out->urn) - 1);
    }
    if (cJSON_IsString(device)) {
        strncpy(out->device, device->valuestring, sizeof(out->device) - 1);
    }
    const cJSON *j;
    j = cJSON_GetObjectItem(root, "siid");
    out->siid = cJSON_IsNumber(j) ? j->valueint : 2;
    j = cJSON_GetObjectItem(root, "piid_on");
    if (cJSON_IsNumber(j)) {
        out->piid_on = j->valueint;
    } else {
        out->piid_on = 1;
    }
    j = cJSON_GetObjectItem(root, "piid_bright");
    out->piid_bright = cJSON_IsNumber(j) ? j->valueint : 0;
    profile_load_int_field(root, "bright_max", &out->bright_max);
    j = cJSON_GetObjectItem(root, "piid_ct");
    out->piid_ct = cJSON_IsNumber(j) ? j->valueint : 0;
    j = cJSON_GetObjectItem(root, "piid_color");
    out->piid_color = cJSON_IsNumber(j) ? j->valueint : 0;
    j = cJSON_GetObjectItem(root, "supports_brightness");
    out->supports_brightness = cJSON_IsTrue(j) || out->piid_bright > 0;
    j = cJSON_GetObjectItem(root, "supports_color_temp");
    out->supports_color_temp = cJSON_IsTrue(j) || out->piid_ct > 0;
    j = cJSON_GetObjectItem(root, "supports_color");
    out->supports_color = cJSON_IsTrue(j) || out->piid_color > 0;
    profile_load_int_field(root, "siid_fan", &out->siid_fan);
    profile_load_int_field(root, "piid_fan", &out->piid_fan);
    profile_load_int_field(root, "siid_env", &out->siid_env);
    profile_load_int_field(root, "piid_temp", &out->piid_temp);
    profile_load_int_field(root, "piid_mode", &out->piid_mode);
    profile_load_int_field(root, "piid_target_temp", &out->piid_target_temp);
    profile_load_int_field(root, "piid_position", &out->piid_position);
    profile_load_int_field(root, "piid_target_position", &out->piid_target_position);
    profile_load_int_field(root, "siid_motor", &out->siid_motor);
    profile_load_int_field(root, "piid_motor", &out->piid_motor);
    profile_load_u8_field(root, "motor_open", &out->motor_open);
    profile_load_u8_field(root, "motor_close", &out->motor_close);
    profile_load_u8_field(root, "motor_pause", &out->motor_pause);
    j = cJSON_GetObjectItem(root, "invert_on");
    out->invert_on = cJSON_IsTrue(j);
    profile_load_int_field(root, "piid_volume", &out->piid_volume);
    profile_load_int_field(root, "piid_door", &out->piid_door);
    profile_load_int_field(root, "piid_illum", &out->piid_illum);
    profile_load_int_field(root, "siid_battery", &out->siid_battery);
    profile_load_int_field(root, "piid_battery", &out->piid_battery);
    profile_load_int_field(root, "siid_stream", &out->siid_stream);
    profile_load_int_field(root, "piid_video_codec", &out->piid_video_codec);
    profile_load_int_field(root, "piid_audio_codec", &out->piid_audio_codec);
    profile_load_int_field(root, "siid_power", &out->siid_power);
    profile_load_int_field(root, "piid_energy", &out->piid_energy);
    profile_load_int_field(root, "piid_current", &out->piid_current);
    profile_load_int_field(root, "piid_power", &out->piid_power);
    profile_load_int_field(root, "piid_voltage", &out->piid_voltage);
    profile_load_int_field(root, "piid_fault", &out->piid_fault);
    extras_array_from_json(cJSON_GetObjectItem(root, "extras"), out);
    profile_load_int_field(root, "profile_rev", &out->profile_rev);
    out->valid = out->device[0] != '\0';
    cJSON_Delete(root);
    return out->valid ? ESP_OK : ESP_FAIL;
}

static cJSON *load_index(void)
{
    char *body = read_file(SPEC_INDEX_PATH, NULL);
    if (!body) {
        return cJSON_CreateObject();
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return cJSON_CreateObject();
    }
    return root;
}

static esp_err_t save_index(cJSON *index)
{
    if (!cJSON_IsObject(index)) {
        return ESP_ERR_INVALID_ARG;
    }
    char *printed = cJSON_PrintUnformatted(index);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = write_file(SPEC_INDEX_PATH, printed, strlen(printed));
    free(printed);
    return err;
}

static void index_put(cJSON *index, const miot_spec_profile_t *p, int version)
{
    if (!index || !p || !p->model[0]) {
        return;
    }
    cJSON *entry = cJSON_CreateObject();
    if (!entry) {
        return;
    }
    cJSON_AddStringToObject(entry, "urn", p->urn);
    cJSON_AddStringToObject(entry, "device", p->device);
    cJSON_AddNumberToObject(entry, "version", version);
    cJSON_AddNumberToObject(entry, "updated_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_DeleteItemFromObject(index, p->model);
    cJSON_AddItemToObject(index, p->model, entry);
}

typedef struct {
    char models[SPEC_MAX_MODELS][MIOT_SPEC_MODEL_MAX];
    char urns[SPEC_MAX_MODELS][MIOT_SPEC_URN_MAX];
    int versions[SPEC_MAX_MODELS];
    size_t n;
    char *win;
    size_t win_len;
    size_t win_cap;
} spec_scan_t;

static bool scan_try_extract(spec_scan_t *sc, size_t idx)
{
    if (sc->urns[idx][0]) {
        return true;
    }
    char needle[MIOT_SPEC_MODEL_MAX + 16];
    snprintf(needle, sizeof(needle), "\"model\":\"%s\"", sc->models[idx]);
    char *hit = strstr(sc->win, needle);
    if (!hit) {
        return false;
    }
    char *type_key = strstr(hit, "\"type\":\"");
    if (!type_key || type_key > hit + SPEC_URN_SCAN) {
        return false;
    }
    type_key += strlen("\"type\":\"");
    char *end = strchr(type_key, '"');
    if (!end || (size_t)(end - type_key) >= MIOT_SPEC_URN_MAX) {
        return false;
    }
    size_t n = (size_t)(end - type_key);
    memcpy(sc->urns[idx], type_key, n);
    sc->urns[idx][n] = '\0';

    char *ver_key = strstr(hit, "\"version\":");
    if (ver_key && ver_key < type_key) {
        sc->versions[idx] = atoi(ver_key + strlen("\"version\":"));
    } else {
        sc->versions[idx] = 1;
    }
    return true;
}

static void scan_process_window(spec_scan_t *sc)
{
    for (size_t i = 0; i < sc->n; i++) {
        scan_try_extract(sc, i);
    }
    /* Keep tail so URNs spanning chunks still match. */
    if (sc->win_len > 4096) {
        size_t keep = 4096;
        memmove(sc->win, sc->win + sc->win_len - keep, keep);
        sc->win_len = keep;
        sc->win[sc->win_len] = '\0';
    }
}

static esp_err_t http_event_scan(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data) {
        return ESP_OK;
    }
    spec_scan_t *sc = evt->user_data;
    if (!evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    size_t need = sc->win_len + (size_t)evt->data_len + 1;
    if (need > sc->win_cap) {
        size_t ncap = sc->win_cap ? sc->win_cap * 2 : SPEC_SCAN_WIN;
        while (ncap < need) {
            ncap *= 2;
        }
        if (ncap > 96 * 1024) {
            /* Force compact before growing further. */
            scan_process_window(sc);
            need = sc->win_len + (size_t)evt->data_len + 1;
            if (need > 96 * 1024) {
                return ESP_ERR_NO_MEM;
            }
            ncap = 96 * 1024;
        }
        char *nw = realloc(sc->win, ncap);
        if (!nw) {
            return ESP_ERR_NO_MEM;
        }
        sc->win = nw;
        sc->win_cap = ncap;
    }
    memcpy(sc->win + sc->win_len, evt->data, (size_t)evt->data_len);
    sc->win_len += (size_t)evt->data_len;
    sc->win[sc->win_len] = '\0';
    scan_process_window(sc);
    return ESP_OK;
}

static void load_store_bases(char bases[][96], size_t *nb, size_t max_nb);
static esp_err_t resolve_via_store_base(const char *base, spec_scan_t *sc);

typedef struct {
    char *body;
    size_t len;
    size_t cap;
} store_http_buf_t;

static esp_err_t store_http_event(esp_http_client_event_t *evt)
{
    store_http_buf_t *b = evt->user_data;
    if (!b || evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    size_t need = b->len + (size_t)evt->data_len + 1;
    if (need > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < need) {
            ncap *= 2;
        }
        if (ncap > 512 * 1024) {
            return ESP_ERR_NO_MEM;
        }
        char *nw = realloc(b->body, ncap);
        if (!nw) {
            return ESP_ERR_NO_MEM;
        }
        b->body = nw;
        b->cap = ncap;
    }
    memcpy(b->body + b->len, evt->data, (size_t)evt->data_len);
    b->len += (size_t)evt->data_len;
    b->body[b->len] = '\0';
    return ESP_OK;
}

/** POST JSON to MiniHub Store (LAN); short timeout so unreachable bases fail fast. */
static esp_err_t store_post_json(const char *url, const char *json_body, store_http_buf_t *out)
{
    if (!url || !json_body || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000; /* fail fast if Store unreachable; try next base */
    cfg.event_handler = store_http_event;
    cfg.user_data = out;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "User-Agent", "MiniHub-miot-spec/1.0");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free(out->body);
        memset(out, 0, sizeof(*out));
        return err;
    }
    if (status < 200 || status >= 300 || !out->body) {
        free(out->body);
        memset(out, 0, sizeof(*out));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t apply_store_profiles(const char *base, spec_scan_t *sc, cJSON *index, size_t *ok_out)
{
    if (!base || !base[0] || !sc || !index) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!req || !arr) {
        cJSON_Delete(req);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(req, "models", arr);
    for (size_t i = 0; i < sc->n; i++) {
        if (sc->versions[i] < 0) {
            continue;
        }
        cJSON_AddItemToArray(arr, cJSON_CreateString(sc->models[i]));
    }
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(req);
        if (ok_out) {
            *ok_out = 0;
        }
        return ESP_OK;
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    char url[384];
    if (snprintf(url, sizeof(url), "%s/api/miot-spec/profiles", base) >= (int)sizeof(url)) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    store_http_buf_t resp = {0};
    esp_err_t err = store_post_json(url, body, &resp);
    free(body);
    if (err != ESP_OK || !resp.body) {
        free(resp.body);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp.body);
    free(resp.body);
    if (!root) {
        return ESP_FAIL;
    }
    cJSON *profiles = cJSON_GetObjectItem(root, "profiles");
    size_t ok = 0;
    if (cJSON_IsObject(profiles)) {
        for (size_t i = 0; i < sc->n; i++) {
            if (sc->versions[i] < 0) {
                continue;
            }
            cJSON *slim = cJSON_GetObjectItem(profiles, sc->models[i]);
            if (!cJSON_IsObject(slim)) {
                continue;
            }
            /* Stamp rev4 so ensure does not re-fetch every sync after Store pull. */
            cJSON_DeleteItemFromObject(slim, "profile_rev");
            cJSON_AddNumberToObject(slim, "profile_rev", 11);
            char *printed = cJSON_PrintUnformatted(slim);
            if (!printed) {
                continue;
            }
            char path[160];
            model_path(sc->models[i], path, sizeof(path));
            if (write_file(path, printed, strlen(printed)) != ESP_OK) {
                free(printed);
                continue;
            }
            free(printed);

            miot_spec_profile_t prof;
            if (miot_spec_load_profile(sc->models[i], &prof) != ESP_OK || !prof.valid) {
                continue;
            }
            index_put(index, &prof, 1);
            if (prof.urn[0]) {
                strncpy(sc->urns[i], prof.urn, MIOT_SPEC_URN_MAX - 1);
            }
            sc->versions[i] = -1;
            ok++;
            ESP_LOGI(TAG, "store Spec profile %s → device=%s color=%d ct=%d", prof.model, prof.device,
                     (int)prof.supports_color, (int)prof.supports_color_temp);
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "store Spec profiles %s → %u", base, (unsigned)ok);
    if (ok_out) {
        *ok_out = ok;
    }
    return ok > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t ensure_via_store_profiles(spec_scan_t *sc, cJSON *index, size_t *ok_out)
{
    char bases[3][96];
    size_t nb = 0;
    load_store_bases(bases, &nb, 3);
    size_t total = 0;
    for (size_t i = 0; i < nb; i++) {
        size_t got = 0;
        if (apply_store_profiles(bases[i], sc, index, &got) == ESP_OK) {
            total += got;
        }
        bool pending = false;
        for (size_t j = 0; j < sc->n; j++) {
            if (sc->versions[j] >= 0) {
                pending = true;
                break;
            }
        }
        if (!pending) {
            break;
        }
    }
    if (ok_out) {
        *ok_out = total;
    }
    return total > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t resolve_via_store_base(const char *base, spec_scan_t *sc)
{
    if (!base || !base[0] || !sc) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!req || !arr) {
        cJSON_Delete(req);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(req, "models", arr);
    for (size_t i = 0; i < sc->n; i++) {
        if (!sc->urns[i][0]) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(sc->models[i]));
        }
    }
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(req);
        return ESP_OK;
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    char url[384];
    if (snprintf(url, sizeof(url), "%s/api/miot-spec/resolve", base) >= (int)sizeof(url)) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    miot_http_buf_t resp = {0};
    esp_err_t err = miot_http_request(url, HTTP_METHOD_POST, body, "MiniHub-miot-spec/1.0", NULL, 0,
                                      &resp, NULL, 0);
    free(body);
    if (err != ESP_OK || !resp.body) {
        free(resp.body);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp.body);
    free(resp.body);
    if (!root) {
        return ESP_FAIL;
    }
    cJSON *urns = cJSON_GetObjectItem(root, "urns");
    size_t got = 0;
    if (cJSON_IsObject(urns)) {
        for (size_t i = 0; i < sc->n; i++) {
            if (sc->urns[i][0]) {
                continue;
            }
            cJSON *ent = cJSON_GetObjectItem(urns, sc->models[i]);
            if (!cJSON_IsObject(ent)) {
                continue;
            }
            cJSON *type = cJSON_GetObjectItem(ent, "type");
            cJSON *ver = cJSON_GetObjectItem(ent, "version");
            if (cJSON_IsString(type) && type->valuestring[0]) {
                strncpy(sc->urns[i], type->valuestring, MIOT_SPEC_URN_MAX - 1);
                sc->versions[i] = cJSON_IsNumber(ver) ? ver->valueint : 1;
                got++;
            }
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "store Spec resolve %s → %u URN(s)", base, (unsigned)got);
    return got > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}



static void store_bases_add(char bases[][96], size_t *nb, size_t max_nb, const char *url)
{
    if (!url || !url[0] || !nb || *nb >= max_nb) {
        return;
    }
    char tmp[96];
    strncpy(tmp, url, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t n = strlen(tmp);
    while (n > 0 && tmp[n - 1] == '/') {
        tmp[--n] = '\0';
    }
    for (size_t i = 0; i < *nb; i++) {
        if (strcmp(bases[i], tmp) == 0) {
            return;
        }
    }
    strncpy(bases[*nb], tmp, sizeof(bases[0]) - 1);
    bases[*nb][sizeof(bases[0]) - 1] = '\0';
    (*nb)++;
}

static void load_store_bases(char bases[][96], size_t *nb, size_t max_nb)
{
    *nb = 0;
    if (max_nb == 0) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("mh_store", NVS_READONLY, &h) == ESP_OK) {
        char custom[96];
        size_t len = sizeof(custom);
        if (nvs_get_str(h, "custom_url", custom, &len) == ESP_OK && custom[0]) {
            store_bases_add(bases, nb, max_nb, custom);
        }
        nvs_close(h);
    }
    /* Lab Store used by this project’s LAN testing (current Mac LAN). */
    store_bases_add(bases, nb, max_nb, "http://192.168.3.47:8080");
    store_bases_add(bases, nb, max_nb, "http://192.168.3.110:8080");
#if defined(CONFIG_MINIHUB_STORE_SERVER) && defined(CONFIG_MINIHUB_STORE_PORT)
    {
        char def[96];
        snprintf(def, sizeof(def), "http://%s:%d", CONFIG_MINIHUB_STORE_SERVER,
                 CONFIG_MINIHUB_STORE_PORT);
        /* Skip known offline factory default that blackholes for 60s+. */
        if (strncmp(def, "http://172.", 11) != 0) {
            store_bases_add(bases, nb, max_nb, def);
        }
    }
#endif
    ESP_LOGI(TAG, "Spec Store bases: %u", (unsigned)*nb);
}

static esp_err_t resolve_urns(spec_scan_t *sc)
{
    bool any = false;
    for (size_t i = 0; i < sc->n; i++) {
        if (!sc->urns[i][0]) {
            any = true;
            break;
        }
    }
    if (!any) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "streaming Spec instances index for %u model(s)", (unsigned)sc->n);
    sc->win = calloc(1, SPEC_SCAN_WIN);
    if (!sc->win) {
        return ESP_ERR_NO_MEM;
    }
    sc->win_cap = SPEC_SCAN_WIN;
    sc->win_len = 0;

    esp_http_client_config_t cfg = {0};
    miot_http_client_config_init(&cfg, SPEC_INSTANCES_URL);
    cfg.url = SPEC_INSTANCES_URL;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 120000;
    cfg.event_handler = http_event_scan;
    cfg.user_data = sc;
    cfg.buffer_size = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(sc->win);
        sc->win = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    scan_process_window(sc);
    free(sc->win);
    sc->win = NULL;
    sc->win_len = 0;
    sc->win_cap = 0;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "instances fetch failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "instances HTTP %d", status);
        return ESP_FAIL;
    }
    size_t got = 0;
    for (size_t i = 0; i < sc->n; i++) {
        if (sc->urns[i][0]) {
            got++;
        }
    }
    ESP_LOGI(TAG, "resolved %u/%u Spec URNs", (unsigned)got, (unsigned)sc->n);
    return ESP_OK;
}

static esp_err_t resolve_urns_prefer_store(spec_scan_t *sc)
{
    bool need = false;
    for (size_t i = 0; i < sc->n; i++) {
        if (!sc->urns[i][0]) {
            need = true;
            break;
        }
    }
    if (!need) {
        return ESP_OK;
    }

    char bases[3][96];
    size_t nb = 0;
    load_store_bases(bases, &nb, 3);

    for (size_t i = 0; i < nb; i++) {
        if (resolve_via_store_base(bases[i], sc) == ESP_OK) {
            need = false;
            for (size_t j = 0; j < sc->n; j++) {
                if (!sc->urns[j][0]) {
                    need = true;
                    break;
                }
            }
            if (!need) {
                return ESP_OK;
            }
        }
    }
    /* Streaming the multi-MB instances index on-device often exceeds HTTP sync timeout.
     * Prefer Store; only stream when no Store base is configured. */
    if (nb == 0) {
        ESP_LOGI(TAG, "falling back to streaming Spec instances index");
        return resolve_urns(sc);
    }
    ESP_LOGW(TAG, "Spec URN resolve incomplete via Store (%u base(s)); skipping stream",
             (unsigned)nb);
    return ESP_OK;
}

static esp_err_t fetch_instance(const char *urn, cJSON **out_root)
{
    if (!urn || !urn[0] || !out_root) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_root = NULL;
    char url[512];
    char enc[384];
    size_t ei = 0;
    for (size_t i = 0; urn[i] && ei + 4 < sizeof(enc); i++) {
        unsigned char c = (unsigned char)urn[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            enc[ei++] = (char)c;
        } else {
            ei += (size_t)snprintf(enc + ei, sizeof(enc) - ei, "%%%02X", c);
        }
    }
    enc[ei] = '\0';
    if (snprintf(url, sizeof(url), SPEC_INSTANCE_FMT, enc) >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    miot_http_buf_t resp = {0};
    esp_err_t err = miot_http_request(url, HTTP_METHOD_GET, NULL, "MiniHub-miot-spec/1.0", NULL, 0,
                                      &resp, NULL, 0);
    if (err != ESP_OK || !resp.body) {
        free(resp.body);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp.body);
    free(resp.body);
    if (!root) {
        return ESP_FAIL;
    }
    *out_root = root;
    return ESP_OK;
}

esp_err_t miot_spec_ensure_models(const char *const *models, size_t n_models)
{
    if (!models || n_models == 0) {
        return ESP_OK;
    }

    spec_scan_t sc;
    memset(&sc, 0, sizeof(sc));

    cJSON *index = load_index();
    if (!index) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < n_models && sc.n < SPEC_MAX_MODELS; i++) {
        const char *m = models[i];
        if (!m || !m[0] || strlen(m) >= MIOT_SPEC_MODEL_MAX) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < sc.n; j++) {
            if (strcmp(sc.models[j], m) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        miot_spec_profile_t existing;
        if (miot_spec_load_profile(m, &existing) == ESP_OK && existing.valid &&
            existing.profile_rev >= 11) {
            const cJSON *ent = cJSON_GetObjectItem(index, m);
            if (cJSON_IsObject(ent)) {
                continue;
            }
        }

        strncpy(sc.models[sc.n], m, MIOT_SPEC_MODEL_MAX - 1);
        sc.n++;
    }

    if (sc.n == 0) {
        cJSON_Delete(index);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Spec ensure %u model(s) via Store profiles", (unsigned)sc.n);
    size_t ok = 0;
    esp_err_t err = ensure_via_store_profiles(&sc, index, &ok);
    (void)save_index(index);
    cJSON_Delete(index);
    ESP_LOGI(TAG, "Spec cache updated (%u profiles, err=%s)", (unsigned)ok, esp_err_to_name(err));
    /* Never fall back to on-device instance streaming — exceeds sync HTTP budget. */
    return ESP_OK;
}
