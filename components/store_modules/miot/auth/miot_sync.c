/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_miot_auth.h"
#include "miot_cloud.h"
#include "miot_http.h"
#include "miot_spec.h"
#include "module_bridge.h"
#include "minihub_core.h"
#include "minihub_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "miot_sync";

static int miot_tolower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

static bool miot_isalnum(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool miot_isxdigit(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void json_num_or_str(const cJSON *j, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return;
    }
    buf[0] = '\0';
    if (!j) {
        return;
    }
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(buf, j->valuestring, buflen - 1);
        buf[buflen - 1] = '\0';
        return;
    }
    if (cJSON_IsNumber(j)) {
        /* Prefer valueint — avoid (long long)valuedouble (__fixdfdi) for module ABI. */
        snprintf(buf, buflen, "%d", j->valueint);
    }
}


typedef struct {
    char models[64][MIOT_SPEC_MODEL_MAX];
    size_t n;
} spec_ensure_job_t;

static esp_err_t spec_ensure_worker(void *ctx)
{
    spec_ensure_job_t *job = ctx;
    if (!job || job->n == 0) {
        return ESP_OK;
    }
    const char *ptrs[64];
    for (size_t i = 0; i < job->n; i++) {
        ptrs[i] = job->models[i];
    }
    return miot_spec_ensure_models(ptrs, job->n);
}

typedef struct {
    char type[16];
    int siid;
    int piid_on;
    int piid_bright;
    int bright_max;
    int piid_ct;
    int piid_color;
    bool supports_brightness;
    bool supports_color_temp;
    bool supports_color;
    int siid_fan;
    int piid_fan;
    int siid_env;
    int piid_temp;
    int piid_mode;
    int piid_target_temp;
    int piid_position;
    int piid_target_position;
    int siid_motor;
    int piid_motor;
    uint8_t motor_open;
    uint8_t motor_close;
    uint8_t motor_pause;
    bool invert_on;
    int piid_volume;
    int piid_door;
    int piid_illum;
    int piid_no_motion;
    int siid_battery;
    int piid_battery;
    int siid_stream;
    int piid_video_codec;
    int piid_audio_codec;
    int siid_power;
    int piid_energy;
    int piid_current;
    int piid_power;
    int piid_voltage;
    int piid_fault;
    miot_spec_extra_t extras[MIOT_SPEC_EXTRAS_MAX];
    size_t extras_n;
    bool cloud_only;
    bool from_spec;
} miot_profile_t;

static void profile_set_type(miot_profile_t *p, const char *type)
{
    if (!p) {
        return;
    }
    if (!type || !type[0]) {
        type = "switch";
    }
    strncpy(p->type, type, sizeof(p->type) - 1);
    p->type[sizeof(p->type) - 1] = '\0';
}

static miot_profile_t profile_fallback(const char *model)
{
    miot_profile_t p = {0};
    profile_set_type(&p, "switch");
    p.siid = 2;
    p.piid_on = 1;
    if (!model) {
        return p;
    }
    if (strstr(model, "sensor_magnet") || strstr(model, ".magnet.") ||
        (strstr(model, "magnet") && !strstr(model, "vacuum"))) {
        profile_set_type(&p, "binary_sensor");
        p.siid = 2;
        p.piid_on = strstr(model, "isa.magnet") ? 2 : 1;
        return p;
    }
    if (strstr(model, "sensor_smoke") || strstr(model, "smoke")) {
        profile_set_type(&p, "smoke");
        p.siid = 2;
        p.piid_on = 1;
        return p;
    }
    if (strncmp(model, "micar.car.", 10) == 0 || strstr(model, ".car.")) {
        profile_set_type(&p, "car");
        p.siid = 0;
        p.piid_on = 0;
        p.cloud_only = true;
        return p;
    }
    if (strstr(model, "router")) {
        profile_set_type(&p, "router");
        p.siid = 3;
        p.piid_on = 3;
        return p;
    }
    if (strstr(model, ".camera.") || strstr(model, "camera.")) {
        profile_set_type(&p, "camera");
        p.siid = 2;
        p.piid_on = 1; /* camera-control.on — not indicator-light */
        p.cloud_only = true;
        return p;
    }
    if (strstr(model, ".tv.") || strncmp(model, "mitv.", 5) == 0) {
        profile_set_type(&p, "television");
        p.siid = 2;
        p.piid_on = 2; /* sleep-mode */
        p.invert_on = true;
        p.cloud_only = true;
        return p;
    }
    if (strstr(model, "wifispeaker") || strstr(model, ".speaker.")) {
        profile_set_type(&p, "speaker");
        p.siid = 2;
        p.piid_on = 0;
        p.piid_volume = 1;
        p.cloud_only = true;
        return p;
    }
    if (strstr(model, ".lock.")) {
        profile_set_type(&p, "lock");
        p.siid = 3;
        p.piid_on = 0;
        p.piid_door = 1021;
        p.cloud_only = true;
        return p;
    }
    if (strstr(model, ".light.") || strstr(model, "lamp") || strstr(model, "ceiling")) {
        profile_set_type(&p, "light");
        p.siid = 2;
        p.piid_on = 1;
        p.piid_bright = 2;
        p.supports_brightness = true;
        return p;
    }
    if (strstr(model, "sensor") || strstr(model, ".ht.") || strstr(model, "temp")) {
        profile_set_type(&p, "sensor");
        p.siid = 3;
        p.piid_on = 7;
        return p;
    }
    if (strstr(model, "curtain") || strstr(model, "airer")) {
        profile_set_type(&p, "cover");
        p.siid = 2;
        /* Curtain siid=2/piid=1 is fault — never treat as on. */
        p.piid_on = 0;
        return p;
    }
    if (strstr(model, ".remote.") || strstr(model, "remote-control") ||
        (strstr(model, "remote") && !strstr(model, "charger"))) {
        /* Wireless / BLE remotes — no power on; Hub button (not controllable switch). */
        profile_set_type(&p, "button");
        p.siid = 2;
        p.piid_on = 0;
        return p;
    }
    if (strstr(model, "plug") || strstr(model, "switch") || strstr(model, "outlet")) {
        profile_set_type(&p, "switch");
        p.siid = 2;
        p.piid_on = 1;
        return p;
    }
    return p;
}

static miot_profile_t profile_for_model(const char *model)
{
    miot_spec_profile_t sp;
    if (model && miot_spec_load_profile(model, &sp) == ESP_OK && sp.valid) {
        miot_profile_t p = {0};
        /* Copy — hub_type may return pointer into stack-local sp.device. */
        profile_set_type(&p, miot_spec_hub_type(sp.device));
        p.siid = sp.siid > 0 ? sp.siid : 2;
        /* Valid Spec profile: keep piid_on as parsed (covers use 0). */
        p.piid_on = sp.piid_on;
        p.piid_bright = sp.piid_bright;
        p.bright_max = sp.bright_max;
        p.piid_ct = sp.piid_ct;
        p.piid_color = sp.piid_color;
        p.supports_brightness = sp.supports_brightness;
        p.supports_color_temp = sp.supports_color_temp;
        p.supports_color = sp.supports_color;
        p.siid_fan = sp.siid_fan;
        p.piid_fan = sp.piid_fan;
        p.siid_env = sp.siid_env;
        p.piid_temp = sp.piid_temp;
        p.piid_mode = sp.piid_mode;
        p.piid_target_temp = sp.piid_target_temp;
        p.piid_position = sp.piid_position;
        p.piid_target_position = sp.piid_target_position;
        p.siid_motor = sp.siid_motor;
        p.piid_motor = sp.piid_motor;
        p.motor_open = sp.motor_open;
        p.motor_close = sp.motor_close;
        p.motor_pause = sp.motor_pause;
        p.invert_on = sp.invert_on;
        p.piid_volume = sp.piid_volume;
        p.piid_door = sp.piid_door;
        p.piid_illum = sp.piid_illum;
        p.piid_no_motion = sp.piid_no_motion;
        p.siid_battery = sp.siid_battery;
        p.piid_battery = sp.piid_battery;
        p.siid_stream = sp.siid_stream;
        p.piid_video_codec = sp.piid_video_codec;
        p.piid_audio_codec = sp.piid_audio_codec;
        p.siid_power = sp.siid_power;
        p.piid_energy = sp.piid_energy;
        p.piid_current = sp.piid_current;
        p.piid_power = sp.piid_power;
        p.piid_voltage = sp.piid_voltage;
        p.piid_fault = sp.piid_fault;
        {
            size_t n = sp.extras_n;
            if (n > MIOT_SPEC_EXTRAS_MAX) {
                n = MIOT_SPEC_EXTRAS_MAX;
            }
            p.extras_n = n;
            if (n > 0) {
                memcpy(p.extras, sp.extras, n * sizeof(p.extras[0]));
            }
        }
        p.cloud_only = miot_spec_device_cloud_only(sp.device);
        p.from_spec = true;
        /* Recover metering outlets/switches with missing on (stale Spec slim).
         * Do not convert these to button — they remain controllable plugs. */
        if (p.piid_on <= 0 &&
            (strcmp(sp.device, "outlet") == 0 || strcmp(p.type, "switch") == 0) &&
            (p.siid_power > 0 || p.piid_voltage > 0 || p.piid_power > 0)) {
            p.piid_on = 1;
            if (p.siid <= 0) {
                p.siid = 2;
            }
        }
        /* Magnets: primary is contact-state (often piid 2), not on. */
        if (p.piid_on <= 0 && strcmp(sp.device, "magnet-sensor") == 0) {
            p.siid = p.siid > 0 ? p.siid : 2;
            p.piid_on = (model && strstr(model, "isa.magnet")) ? 2 : 1;
        }
        /* Magnets often also expose illumination (weak/strong) as piid 1. */
        if (p.piid_illum <= 0 && strcmp(sp.device, "magnet-sensor") == 0) {
            p.piid_illum = (sp.piid_illum > 0) ? sp.piid_illum : 1;
        }
        /* Motion: occupancy via no-motion-duration (HA: error=-704220043 ⇒ detected). */
        if (strcmp(sp.device, "motion-sensor") == 0) {
            profile_set_type(&p, "binary_sensor");
            if (p.siid <= 0) {
                p.siid = 2;
            }
            if (p.piid_no_motion <= 0) {
                p.piid_no_motion = (sp.piid_no_motion > 0) ? sp.piid_no_motion : 1024;
            }
        }
        /* Event-only remotes / wireless switches: no writable on → Hub button. */
        if (p.piid_on <= 0 &&
            (strcmp(sp.device, "remote-control") == 0 ||
             strcmp(sp.device, "switch") == 0)) {
            profile_set_type(&p, "button");
        }
        return p;
    }
    return profile_fallback(model);
}

static bool profile_force_rewrite(const miot_profile_t *profile)
{
    if (!profile || !profile->type[0]) {
        return false;
    }
    return strcmp(profile->type, "car") == 0 ||
           strcmp(profile->type, "router") == 0 ||
           strcmp(profile->type, "binary_sensor") == 0 ||
           strcmp(profile->type, "smoke") == 0 ||
           strcmp(profile->type, "camera") == 0 ||
           strcmp(profile->type, "television") == 0 ||
           strcmp(profile->type, "lock") == 0 ||
           strcmp(profile->type, "speaker") == 0 ||
           strcmp(profile->type, "light") == 0 ||
           strcmp(profile->type, "button") == 0 ||
           profile->from_spec;
}

static bool should_skip_device(const cJSON *dev)
{
    const cJSON *model = cJSON_GetObjectItem(dev, "model");
    const cJSON *did = cJSON_GetObjectItem(dev, "did");
    if (!cJSON_IsString(model) || !cJSON_IsString(did)) {
        return true;
    }
    const char *m = model->valuestring;
    if (strstr(m, "phone") || strstr(m, "vacuum.map")) {
        return true;
    }
    return false;
}

static void slugify(const char *name, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (miot_isalnum(c)) {
            out[j++] = (char)miot_tolower(c);
        } else if (c == ' ' || c == '-' || c == '_') {
            if (j > 0 && out[j - 1] != '_') {
                out[j++] = '_';
            }
        }
    }
    while (j > 0 && out[j - 1] == '_') {
        j--;
    }
    out[j] = '\0';
    if (j == 0) {
        strncpy(out, "device", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

/** Stable English label for known models (UI); NULL = keep cloud name. */
static const char *preferred_display_name(const char *model)
{
    if (model && strstr(model, "cariot.holder.")) {
        return "Wireless Charger";
    }
    return NULL;
}

/** Model-based slug stem (avoids Chinese→"device" collisions); NULL = slugify(name). */
static const char *preferred_slug_base(const char *model)
{
    if (model && strstr(model, "cariot.holder.")) {
        return "wireless_charger";
    }
    return NULL;
}

/** Append "_<last alnum of did>" so two same-model devices stay unique. */
static void append_did_tail(char *slug, size_t slug_len, const char *did)
{
    if (!slug || slug_len < 4 || !did || !did[0]) {
        return;
    }
    size_t n = strlen(slug);
    const char *p = did + strlen(did);
    char tail[7];
    size_t t = 0;
    while (p > did && t < sizeof(tail) - 1) {
        p--;
        unsigned char c = (unsigned char)*p;
        if (miot_isalnum(c)) {
            tail[t++] = (char)miot_tolower(c);
        }
    }
    if (t == 0) {
        return;
    }
    /* reverse into place after '_' */
    if (n + 1 + t >= slug_len) {
        return;
    }
    slug[n++] = '_';
    for (size_t i = 0; i < t; i++) {
        slug[n++] = tail[t - 1 - i];
    }
    slug[n] = '\0';
}

static void build_device_slug(const char *model, const char *name, const char *did,
                              char *out, size_t out_len)
{
    const char *pref = preferred_slug_base(model);
    if (pref) {
        strncpy(out, pref, out_len - 1);
        out[out_len - 1] = '\0';
    } else {
        slugify(name && name[0] ? name : "device", out, out_len);
    }
    /* Chinese names collapse to "device"; always suffix did so siblings stay unique. */
    append_did_tail(out, out_len, did);
}

static void build_point_id(const char *type, const char *slug, char *out, size_t out_len)
{
    size_t n = 0;
    size_t type_len = type ? strlen(type) : 0;
    if (type_len > out_len - 1) {
        type_len = out_len - 1;
    }
    if (type_len > 0) {
        memcpy(out, type, type_len);
        n = type_len;
    }
    static const char mid[] = "/miot_";
    if (n + sizeof(mid) - 1 < out_len) {
        memcpy(out + n, mid, sizeof(mid) - 1);
        n += sizeof(mid) - 1;
    }
    size_t room = (n < out_len) ? (out_len - 1 - n) : 0;
    size_t slug_len = slug ? strlen(slug) : 0;
    if (slug_len > room) {
        slug_len = room;
    }
    if (slug_len > 0) {
        memcpy(out + n, slug, slug_len);
        n += slug_len;
    }
    out[n] = '\0';
}

/** True when point_id is missing the did unique suffix (legacy collision). */
static bool point_id_needs_slug_fix(const char *point_id, const char *model, const char *did)
{
    (void)model;
    if (!point_id || !point_id[0]) {
        return true;
    }
    /* Exact legacy collapse: type/miot_device */
    const char *slash = strrchr(point_id, '/');
    if (slash && strcmp(slash, "/miot_device") == 0) {
        return true;
    }
    if (!did || !did[0]) {
        return false;
    }
    /* Expect "_<did alnum tail>" at end of point_id. */
    char expect[16];
    expect[0] = '\0';
    append_did_tail(expect, sizeof(expect), did);
    if (!expect[0]) {
        return false;
    }
    size_t elen = strlen(expect);
    size_t plen = strlen(point_id);
    if (plen < elen) {
        return true;
    }
    return strncmp(point_id + plen - elen, expect, elen) != 0;
}

static cJSON *find_existing_by_did(const cJSON *devices, const char *did)
{
    if (!cJSON_IsArray(devices)) {
        return NULL;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        const cJSON *existing_did = cJSON_GetObjectItem(item, "did");
        if (cJSON_IsString(existing_did) && strcmp(existing_did->valuestring, did) == 0) {
            return item;
        }
    }
    return NULL;
}

static bool extract_miio_token_hex(const cJSON *dev, char *out, size_t out_len)
{
    if (!dev || !out || out_len < 33) {
        return false;
    }
    const cJSON *token = cJSON_GetObjectItem(dev, "token");
    if (!cJSON_IsString(token) || !token->valuestring[0]) {
        return false;
    }
    const char *s = token->valuestring;
    if (strlen(s) != 32) {
        return false;
    }
    for (size_t i = 0; s[i]; i++) {
        if (!miot_isxdigit((unsigned char)s[i])) {
            return false;
        }
        out[i] = (char)miot_tolower((unsigned char)s[i]);
    }
    out[32] = '\0';
    return true;
}

static bool cloud_did_is_ble(const cJSON *cloud_dev)
{
    const cJSON *did = cJSON_GetObjectItem(cloud_dev, "did");
    return cJSON_IsString(did) && did->valuestring[0] &&
           strncmp(did->valuestring, "blt.", 4) == 0;
}

/** Camera / TV / speaker / lock: Spec via cloud; UDP miIO usually times out. */
static bool model_cloud_only(const char *model)
{
    miot_profile_t p = profile_for_model(model);
    return p.cloud_only;
}

static void append_lan_fields(cJSON *entry, const cJSON *cloud_dev, const cJSON *existing,
                              bool *has_lan_token)
{
    if (!entry || !cloud_dev) {
        return;
    }

    const cJSON *mac = cJSON_GetObjectItem(cloud_dev, "mac");
    if (cJSON_IsString(mac) && mac->valuestring[0]) {
        cJSON_AddStringToObject(entry, "mac", mac->valuestring);
    } else if (existing) {
        const cJSON *old_mac = cJSON_GetObjectItem(existing, "mac");
        if (cJSON_IsString(old_mac) && old_mac->valuestring[0]) {
            cJSON_AddStringToObject(entry, "mac", old_mac->valuestring);
        }
    }

    const cJSON *model = cJSON_GetObjectItem(cloud_dev, "model");
    const char *model_str = cJSON_IsString(model) ? model->valuestring : "";

    const cJSON *did_j = cJSON_GetObjectItem(cloud_dev, "did");
    const char *did_str = cJSON_IsString(did_j) ? did_j->valuestring : "";
    /* BLE / cloud-only / gateway proxy children: Spec via cloud only. */
    if (cloud_did_is_ble(cloud_dev) || model_cloud_only(model_str) ||
        (did_str[0] && strncmp(did_str, "proxy.", 6) == 0)) {
        cJSON_AddStringToObject(entry, "mode", "cloud");
        return;
    }

    char token_hex[33];
    bool got_token = extract_miio_token_hex(cloud_dev, token_hex, sizeof(token_hex));
    if (got_token) {
        cJSON_AddStringToObject(entry, "token", token_hex);
        if (has_lan_token) {
            (*has_lan_token) = true;
        }
    } else if (existing) {
        const cJSON *old_token = cJSON_GetObjectItem(existing, "token");
        if (cJSON_IsString(old_token) && old_token->valuestring[0]) {
            cJSON_AddStringToObject(entry, "token", old_token->valuestring);
            if (has_lan_token) {
                (*has_lan_token) = true;
            }
        }
    }

    const cJSON *localip = cJSON_GetObjectItem(cloud_dev, "localip");
    if (cJSON_IsString(localip) && localip->valuestring[0]) {
        cJSON_AddStringToObject(entry, "ip", localip->valuestring);
    } else if (existing) {
        const cJSON *old_ip = cJSON_GetObjectItem(existing, "ip");
        if (cJSON_IsString(old_ip) && old_ip->valuestring[0]) {
            cJSON_AddStringToObject(entry, "ip", old_ip->valuestring);
        }
    }

    if (cJSON_GetObjectItem(entry, "token")) {
        const cJSON *mode = existing ? cJSON_GetObjectItem(existing, "mode") : NULL;
        if (cJSON_IsString(mode)) {
            cJSON_AddStringToObject(entry, "mode", mode->valuestring);
        } else {
            cJSON_AddStringToObject(entry, "mode", "auto");
        }
    }
}

/* Place info from gethome for one did (may be NULL). */
typedef struct {
    const char *home_id;
    const char *home_name;
    const char *room_id;
    const char *room_name;
} miot_place_t;

static void place_map_put(cJSON *map, const char *did, const char *home_id,
                          const char *home_name, const char *room_id,
                          const char *room_name)
{
    if (!map || !did || !did[0]) {
        return;
    }
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    if (home_id && home_id[0]) {
        cJSON_AddStringToObject(obj, "home_id", home_id);
    }
    if (home_name && home_name[0]) {
        cJSON_AddStringToObject(obj, "home_name", home_name);
    }
    if (room_id && room_id[0]) {
        cJSON_AddStringToObject(obj, "room_id", room_id);
    }
    cJSON_AddStringToObject(obj, "room_name",
                            (room_name && room_name[0]) ? room_name : "未分配");
    cJSON_DeleteItemFromObject(map, did);
    cJSON_AddItemToObject(map, did, obj);
}

static void place_map_add_dids(cJSON *map, const cJSON *dids, const char *home_id,
                               const char *home_name, const char *room_id,
                               const char *room_name)
{
    if (!cJSON_IsArray(dids)) {
        return;
    }
    const cJSON *d = NULL;
    cJSON_ArrayForEach(d, dids) {
        if (cJSON_IsString(d) && d->valuestring[0]) {
            place_map_put(map, d->valuestring, home_id, home_name, room_id, room_name);
        }
    }
}

/**
 * Build did → {home_*, room_*} for the selected home.
 * Returns NULL on OOM. *found_home set when home_id matches a home entry.
 * out_home_name receives the selected home's name when found.
 */
static cJSON *build_home_place_map(const cJSON *gethome, const char *home_id,
                                   bool *found_home, char *out_home_name,
                                   size_t out_home_name_len)
{
    if (found_home) {
        *found_home = false;
    }
    if (out_home_name && out_home_name_len) {
        out_home_name[0] = '\0';
    }
    if (!cJSON_IsObject(gethome) || !home_id || !home_id[0]) {
        return NULL;
    }

    cJSON *map = cJSON_CreateObject();
    if (!map) {
        return NULL;
    }

    const char *lists[] = {"homelist", "share_home_list"};
    for (size_t li = 0; li < sizeof(lists) / sizeof(lists[0]); li++) {
        const cJSON *arr = cJSON_GetObjectItem(gethome, lists[li]);
        if (!cJSON_IsArray(arr)) {
            continue;
        }
        const cJSON *home = NULL;
        cJSON_ArrayForEach(home, arr) {
            if (!cJSON_IsObject(home)) {
                continue;
            }
            const cJSON *id = cJSON_GetObjectItem(home, "id");
            char id_buf[64];
            json_num_or_str(id, id_buf, sizeof(id_buf));
            if (!id_buf[0] || strcmp(id_buf, home_id) != 0) {
                continue;
            }
            if (found_home) {
                *found_home = true;
            }
            const cJSON *hname = cJSON_GetObjectItem(home, "name");
            const char *home_name =
                cJSON_IsString(hname) && hname->valuestring[0] ? hname->valuestring : "";
            if (out_home_name && out_home_name_len && home_name[0]) {
                strncpy(out_home_name, home_name, out_home_name_len - 1);
                out_home_name[out_home_name_len - 1] = '\0';
            }

            const cJSON *rooms = cJSON_GetObjectItem(home, "roomlist");
            if (cJSON_IsArray(rooms)) {
                const cJSON *room = NULL;
                cJSON_ArrayForEach(room, rooms) {
                    if (!cJSON_IsObject(room)) {
                        continue;
                    }
                    const cJSON *rid = cJSON_GetObjectItem(room, "id");
                    const cJSON *rname = cJSON_GetObjectItem(room, "name");
                    char rid_buf[64];
                    json_num_or_str(rid, rid_buf, sizeof(rid_buf));
                    const char *room_name =
                        cJSON_IsString(rname) && rname->valuestring[0]
                            ? rname->valuestring
                            : "未分配";
                    place_map_add_dids(map, cJSON_GetObjectItem(room, "dids"), home_id,
                                       home_name, rid_buf, room_name);
                }
            }
            /* Home-level dids (unassigned room). Do not overwrite room assignments. */
            const cJSON *home_dids = cJSON_GetObjectItem(home, "dids");
            if (cJSON_IsArray(home_dids)) {
                const cJSON *d = NULL;
                cJSON_ArrayForEach(d, home_dids) {
                    if (!cJSON_IsString(d) || !d->valuestring[0]) {
                        continue;
                    }
                    if (cJSON_GetObjectItem(map, d->valuestring)) {
                        continue;
                    }
                    place_map_put(map, d->valuestring, home_id, home_name, "", "未分配");
                }
            }
            return map;
        }
    }
    return map;
}

static bool place_from_map(const cJSON *map, const char *did, miot_place_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!map || !did) {
        return false;
    }
    const cJSON *obj = cJSON_GetObjectItem(map, did);
    if (!cJSON_IsObject(obj)) {
        return false;
    }
    const cJSON *hid = cJSON_GetObjectItem(obj, "home_id");
    const cJSON *hname = cJSON_GetObjectItem(obj, "home_name");
    const cJSON *rid = cJSON_GetObjectItem(obj, "room_id");
    const cJSON *rname = cJSON_GetObjectItem(obj, "room_name");
    out->home_id = cJSON_IsString(hid) ? hid->valuestring : NULL;
    out->home_name = cJSON_IsString(hname) ? hname->valuestring : NULL;
    out->room_id = cJSON_IsString(rid) ? rid->valuestring : NULL;
    out->room_name = cJSON_IsString(rname) ? rname->valuestring : "未分配";
    return true;
}

static void apply_name_and_place(cJSON *entry, const cJSON *existing,
                                 const char *model_str, const char *cloud_name,
                                 const miot_place_t *place)
{
    bool name_user_set = false;
    if (existing) {
        const cJSON *nus = cJSON_GetObjectItem(existing, "name_user_set");
        name_user_set = cJSON_IsTrue(nus);
    }
    if (name_user_set) {
        const cJSON *old_name = existing ? cJSON_GetObjectItem(existing, "name") : NULL;
        if (cJSON_IsString(old_name) && old_name->valuestring[0]) {
            cJSON_AddStringToObject(entry, "name", old_name->valuestring);
        } else if (cloud_name && cloud_name[0]) {
            cJSON_AddStringToObject(entry, "name", cloud_name);
        }
        cJSON_AddTrueToObject(entry, "name_user_set");
    } else {
        const char *disp = preferred_display_name(model_str);
        if (disp) {
            cJSON_AddStringToObject(entry, "name", disp);
        } else if (cloud_name && cloud_name[0]) {
            cJSON_AddStringToObject(entry, "name", cloud_name);
        }
        cJSON_AddBoolToObject(entry, "name_user_set", false);
    }

    if (place && place->home_id && place->home_id[0]) {
        cJSON_AddStringToObject(entry, "home_id", place->home_id);
    }
    if (place && place->home_name && place->home_name[0]) {
        cJSON_AddStringToObject(entry, "home_name", place->home_name);
    }
    if (place && place->room_id && place->room_id[0]) {
        cJSON_AddStringToObject(entry, "room_id", place->room_id);
    }
    if (place && place->room_name && place->room_name[0]) {
        cJSON_AddStringToObject(entry, "room_name", place->room_name);
    }
}

static void entry_sync_profile_int(cJSON *entry, const char *key, int value)
{
    if (value <= 0) {
        return;
    }
    cJSON_DeleteItemFromObject(entry, key);
    cJSON_AddNumberToObject(entry, key, value);
}

static int entry_int_override(const cJSON *existing, const char *key, int profile_val)
{
    if (existing) {
        const cJSON *ex = cJSON_GetObjectItem(existing, key);
        if (cJSON_IsNumber(ex) && ex->valueint > 0) {
            return ex->valueint;
        }
    }
    return profile_val;
}

static void entry_sync_extras(cJSON *entry, const miot_profile_t *profile)
{
    cJSON_DeleteItemFromObject(entry, "extras");
    if (!profile || profile->extras_n == 0) {
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 8192) {
        ESP_LOGW(TAG, "skip extras persist (internal heap < 8192)");
        return;
    }
    size_t n = profile->extras_n;
    if (n > MIOT_SPEC_EXTRAS_MAX) {
        n = MIOT_SPEC_EXTRAS_MAX;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const miot_spec_extra_t *e = &profile->extras[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            cJSON_Delete(arr);
            return;
        }
        cJSON_AddStringToObject(o, "key", e->key);
        cJSON_AddStringToObject(o, "kind", e->kind);
        cJSON_AddNumberToObject(o, "siid", e->siid);
        cJSON_AddNumberToObject(o, "piid", e->piid);
        cJSON_AddNumberToObject(o, "aiid", e->aiid);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(entry, "extras", arr);
}

static cJSON *device_entry_from_cloud(const cJSON *cloud_dev, const cJSON *existing,
                                      const miot_place_t *place, bool *has_lan_token)
{
    const cJSON *name = cJSON_GetObjectItem(cloud_dev, "name");
    const cJSON *did = cJSON_GetObjectItem(cloud_dev, "did");
    const cJSON *model = cJSON_GetObjectItem(cloud_dev, "model");
    if (!cJSON_IsString(did)) {
        return NULL;
    }

    const char *model_str = cJSON_IsString(model) ? model->valuestring : "";
    const char *name_str = cJSON_IsString(name) ? name->valuestring : NULL;
    miot_profile_t profile = profile_for_model(model_str);

    cJSON *entry = cJSON_CreateObject();
    if (existing) {
        const cJSON *point_id = cJSON_GetObjectItem(existing, "point_id");
        const cJSON *type = cJSON_GetObjectItem(existing, "type");
        const cJSON *siid = cJSON_GetObjectItem(existing, "siid");
        const cJSON *piid_on = cJSON_GetObjectItem(existing, "piid_on");
        const cJSON *piid_bright = cJSON_GetObjectItem(existing, "piid_bright");

        if (cJSON_IsString(point_id)) {
            cJSON_AddStringToObject(entry, "point_id", point_id->valuestring);
        }

        bool force_profile = profile_force_rewrite(&profile);
        bool was_bogus_switch =
            cJSON_IsString(type) && strcmp(type->valuestring, "switch") == 0 &&
            strcmp(profile.type, "switch") != 0;
        bool fix_slug =
            cJSON_IsString(point_id) &&
            point_id_needs_slug_fix(point_id->valuestring, model_str, did->valuestring);

        cJSON_AddStringToObject(entry, "did", did->valuestring);
        if (force_profile || was_bogus_switch || !cJSON_IsString(type)) {
            cJSON_AddStringToObject(entry, "type", profile.type);
            cJSON_AddNumberToObject(entry, "siid", profile.siid);
            cJSON_AddNumberToObject(entry, "piid_on", profile.piid_on);
            /* Rewrite point_id prefix when type was wrong (e.g. switch/miot_yu7 → car/…).
             * Wireless remotes → button: keep point_id for binding stability. */
            if (cJSON_IsString(point_id) && !fix_slug &&
                strcmp(profile.type, "button") != 0) {
                const char *slash = strchr(point_id->valuestring, '/');
                if (slash && slash[1]) {
                    char new_pid[MINIHUB_POINT_ID_MAX];
                    size_t n = 0;
                    size_t type_len = strlen(profile.type);
                    if (type_len > sizeof(new_pid) - 1) {
                        type_len = sizeof(new_pid) - 1;
                    }
                    memcpy(new_pid, profile.type, type_len);
                    n = type_len;
                    size_t rest_len = strlen(slash);
                    if (n + rest_len > sizeof(new_pid) - 1) {
                        rest_len = sizeof(new_pid) - 1 - n;
                    }
                    memcpy(new_pid + n, slash, rest_len);
                    n += rest_len;
                    new_pid[n] = '\0';
                    cJSON_ReplaceItemInObject(entry, "point_id", cJSON_CreateString(new_pid));
                }
            }
        } else {
            cJSON_AddStringToObject(entry, "type", type->valuestring);
            if (cJSON_IsNumber(siid)) {
                cJSON_AddNumberToObject(entry, "siid", siid->valueint);
            } else {
                cJSON_AddNumberToObject(entry, "siid", profile.siid);
            }
            if (cJSON_IsNumber(piid_on)) {
                cJSON_AddNumberToObject(entry, "piid_on", piid_on->valueint);
            } else {
                cJSON_AddNumberToObject(entry, "piid_on", profile.piid_on);
            }
        }
        if (fix_slug) {
            char slug[48];
            char new_pid[MINIHUB_POINT_ID_MAX];
            const cJSON *type_j = cJSON_GetObjectItem(entry, "type");
            const char *etype =
                cJSON_IsString(type_j) ? type_j->valuestring : profile.type;
            build_device_slug(model_str, name_str, did->valuestring, slug, sizeof(slug));
            build_point_id(etype, slug, new_pid, sizeof(new_pid));
            cJSON_ReplaceItemInObject(entry, "point_id", cJSON_CreateString(new_pid));
        }
        if (cJSON_IsNumber(piid_bright) && piid_bright->valueint > 0) {
            cJSON_AddNumberToObject(entry, "piid_bright", piid_bright->valueint);
        } else if (profile.piid_bright > 0) {
            cJSON_AddNumberToObject(entry, "piid_bright", profile.piid_bright);
            if (profile.bright_max > 0) {
                cJSON_AddNumberToObject(entry, "bright_max", profile.bright_max);
            }
        }
        {
            int v = entry_int_override(existing, "piid_mode", profile.piid_mode);
            if (v > 0) {
                cJSON_AddNumberToObject(entry, "piid_mode", v);
            }
            v = entry_int_override(existing, "piid_target_temp", profile.piid_target_temp);
            if (v > 0) {
                cJSON_AddNumberToObject(entry, "piid_target_temp", v);
            }
            v = entry_int_override(existing, "siid_fan", profile.siid_fan);
            if (v > 0) {
                cJSON_AddNumberToObject(entry, "siid_fan", v);
            }
            v = entry_int_override(existing, "piid_fan", profile.piid_fan);
            if (v > 0) {
                cJSON_AddNumberToObject(entry, "piid_fan", v);
            }
            v = entry_int_override(existing, "siid_env", profile.siid_env);
            if (v > 0) {
                cJSON_AddNumberToObject(entry, "siid_env", v);
            }
            v = entry_int_override(existing, "piid_temp", profile.piid_temp);
            if (v > 0) {
                cJSON_AddNumberToObject(entry, "piid_temp", v);
            }
            v = entry_int_override(existing, "piid_position", profile.piid_position);
            if (v > 0) {
                cJSON_AddNumberToObject(entry, "piid_position", v);
            }
            v = entry_int_override(existing, "piid_target_position", profile.piid_target_position);
            if (v > 0) {
                cJSON_AddNumberToObject(entry, "piid_target_position", v);
            }
            v = entry_int_override(existing, "piid_motor", profile.piid_motor);
            if (v > 0) {
                int siid_motor = entry_int_override(existing, "siid_motor",
                                                    profile.siid_motor > 0 ? profile.siid_motor
                                                                           : profile.siid);
                cJSON_AddNumberToObject(entry, "siid_motor", siid_motor);
                cJSON_AddNumberToObject(entry, "piid_motor", v);
                cJSON_AddNumberToObject(entry, "motor_open", profile.motor_open);
                cJSON_AddNumberToObject(entry, "motor_close", profile.motor_close);
                cJSON_AddNumberToObject(entry, "motor_pause", profile.motor_pause);
            }
        }
    } else {
        char slug[48];
        char point_id[MINIHUB_POINT_ID_MAX];
        build_device_slug(model_str, name_str, did->valuestring, slug, sizeof(slug));
        build_point_id(profile.type, slug, point_id, sizeof(point_id));

        cJSON_AddStringToObject(entry, "point_id", point_id);
        cJSON_AddStringToObject(entry, "type", profile.type);
        cJSON_AddStringToObject(entry, "did", did->valuestring);
        cJSON_AddNumberToObject(entry, "siid", profile.siid);
        cJSON_AddNumberToObject(entry, "piid_on", profile.piid_on);
        if (profile.piid_bright > 0) {
            cJSON_AddNumberToObject(entry, "piid_bright", profile.piid_bright);
        }
        if (profile.piid_mode > 0) {
            cJSON_AddNumberToObject(entry, "piid_mode", profile.piid_mode);
        }
        if (profile.piid_target_temp > 0) {
            cJSON_AddNumberToObject(entry, "piid_target_temp", profile.piid_target_temp);
        }
        if (profile.siid_fan > 0) {
            cJSON_AddNumberToObject(entry, "siid_fan", profile.siid_fan);
        }
        if (profile.piid_fan > 0) {
            cJSON_AddNumberToObject(entry, "piid_fan", profile.piid_fan);
        }
        if (profile.siid_env > 0) {
            cJSON_AddNumberToObject(entry, "siid_env", profile.siid_env);
        }
        if (profile.piid_temp > 0) {
            cJSON_AddNumberToObject(entry, "piid_temp", profile.piid_temp);
        }
        if (profile.piid_illum > 0) {
            cJSON_AddNumberToObject(entry, "piid_illum", profile.piid_illum);
        }
        if (profile.piid_battery > 0) {
            cJSON_AddNumberToObject(entry, "siid_battery",
                                    profile.siid_battery > 0 ? profile.siid_battery : 3);
            cJSON_AddNumberToObject(entry, "piid_battery", profile.piid_battery);
        }
        if (profile.piid_video_codec > 0 || profile.piid_audio_codec > 0) {
            cJSON_AddNumberToObject(entry, "siid_stream",
                                    profile.siid_stream > 0 ? profile.siid_stream : 9);
            if (profile.piid_video_codec > 0) {
                cJSON_AddNumberToObject(entry, "piid_video_codec", profile.piid_video_codec);
            }
            if (profile.piid_audio_codec > 0) {
                cJSON_AddNumberToObject(entry, "piid_audio_codec", profile.piid_audio_codec);
            }
        }
        if (profile.piid_position > 0) {
            cJSON_AddNumberToObject(entry, "piid_position", profile.piid_position);
        }
        if (profile.piid_target_position > 0) {
            cJSON_AddNumberToObject(entry, "piid_target_position", profile.piid_target_position);
        }
        if (profile.piid_motor > 0) {
            cJSON_AddNumberToObject(entry, "siid_motor",
                                    profile.siid_motor > 0 ? profile.siid_motor : profile.siid);
            cJSON_AddNumberToObject(entry, "piid_motor", profile.piid_motor);
            cJSON_AddNumberToObject(entry, "motor_open", profile.motor_open);
            cJSON_AddNumberToObject(entry, "motor_close", profile.motor_close);
            cJSON_AddNumberToObject(entry, "motor_pause", profile.motor_pause);
        }
    }

    apply_name_and_place(entry, existing, model_str, name_str, place);
    if (cJSON_IsString(model)) {
        cJSON_AddStringToObject(entry, "model", model->valuestring);
    }
    append_lan_fields(entry, cloud_dev, existing, has_lan_token);

    /* Spec-derived light capabilities (Web uses supports_color for RGB wheel). */
    if (profile.piid_bright > 0 && !cJSON_GetObjectItem(entry, "piid_bright")) {
        cJSON_AddNumberToObject(entry, "piid_bright", profile.piid_bright);
    }
    if (profile.piid_ct > 0) {
        cJSON_DeleteItemFromObject(entry, "piid_ct");
        cJSON_AddNumberToObject(entry, "piid_ct", profile.piid_ct);
    }
    if (profile.piid_color > 0) {
        cJSON_DeleteItemFromObject(entry, "piid_color");
        cJSON_AddNumberToObject(entry, "piid_color", profile.piid_color);
    }
    entry_sync_profile_int(entry, "piid_mode", profile.piid_mode);
    entry_sync_profile_int(entry, "piid_target_temp", profile.piid_target_temp);
    entry_sync_profile_int(entry, "siid_fan", profile.siid_fan);
    entry_sync_profile_int(entry, "piid_fan", profile.piid_fan);
    entry_sync_profile_int(entry, "siid_env", profile.siid_env);
    entry_sync_profile_int(entry, "piid_temp", profile.piid_temp);
    entry_sync_profile_int(entry, "piid_position", profile.piid_position);
    entry_sync_profile_int(entry, "piid_target_position", profile.piid_target_position);
    if (profile.piid_motor > 0) {
        entry_sync_profile_int(entry, "siid_motor",
                               profile.siid_motor > 0 ? profile.siid_motor : profile.siid);
        entry_sync_profile_int(entry, "piid_motor", profile.piid_motor);
        cJSON_DeleteItemFromObject(entry, "motor_open");
        cJSON_AddNumberToObject(entry, "motor_open", profile.motor_open);
        cJSON_DeleteItemFromObject(entry, "motor_close");
        cJSON_AddNumberToObject(entry, "motor_close", profile.motor_close);
        cJSON_DeleteItemFromObject(entry, "motor_pause");
        cJSON_AddNumberToObject(entry, "motor_pause", profile.motor_pause);
    }
    entry_sync_profile_int(entry, "piid_volume", profile.piid_volume);
    entry_sync_profile_int(entry, "piid_door", profile.piid_door);
    entry_sync_profile_int(entry, "bright_max", profile.bright_max);
    entry_sync_profile_int(entry, "piid_illum", profile.piid_illum);
    entry_sync_profile_int(entry, "piid_no_motion", profile.piid_no_motion);
    entry_sync_profile_int(entry, "siid_battery", profile.siid_battery);
    entry_sync_profile_int(entry, "piid_battery", profile.piid_battery);
    entry_sync_profile_int(entry, "siid_stream", profile.siid_stream);
    entry_sync_profile_int(entry, "piid_video_codec", profile.piid_video_codec);
    entry_sync_profile_int(entry, "piid_audio_codec", profile.piid_audio_codec);
    entry_sync_profile_int(entry, "siid_power", profile.siid_power);
    entry_sync_profile_int(entry, "piid_energy", profile.piid_energy);
    entry_sync_profile_int(entry, "piid_current", profile.piid_current);
    entry_sync_profile_int(entry, "piid_power", profile.piid_power);
    entry_sync_profile_int(entry, "piid_voltage", profile.piid_voltage);
    entry_sync_profile_int(entry, "piid_fault", profile.piid_fault);
    cJSON_DeleteItemFromObject(entry, "invert_on");
    if (profile.invert_on) {
        cJSON_AddBoolToObject(entry, "invert_on", true);
    }
    cJSON_DeleteItemFromObject(entry, "no_control");
    /* Read-only only when Spec has no writable power switch.
     * After profile_for_model recovery, metering plugs have piid_on > 0 and
     * must not get no_control. */
    if (profile.from_spec && profile.piid_on <= 0 &&
        (profile.siid_power > 0 || profile.piid_voltage > 0 || profile.piid_power > 0)) {
        cJSON_AddBoolToObject(entry, "no_control", true);
    }
    if (profile.from_spec && profile.piid_on == 0) {
        cJSON_DeleteItemFromObject(entry, "piid_on");
        cJSON_AddNumberToObject(entry, "piid_on", 0);
    }
    cJSON_DeleteItemFromObject(entry, "supports_brightness");
    cJSON_DeleteItemFromObject(entry, "supports_color_temp");
    cJSON_DeleteItemFromObject(entry, "supports_color");
    if (strcmp(profile.type, "light") == 0 || profile.supports_brightness ||
        profile.supports_color_temp || profile.supports_color) {
        cJSON_AddBoolToObject(entry, "supports_brightness",
                              profile.supports_brightness || profile.piid_bright > 0);
        cJSON_AddBoolToObject(entry, "supports_color_temp",
                              profile.supports_color_temp || profile.piid_ct > 0);
        cJSON_AddBoolToObject(entry, "supports_color",
                              profile.supports_color || profile.piid_color > 0);
    }
    if (profile.from_spec) {
        cJSON_DeleteItemFromObject(entry, "spec");
        cJSON_AddTrueToObject(entry, "spec");
    }
    entry_sync_extras(entry, &profile);

    return entry;
}

static cJSON *module_config_get_adapters(void)
{
    const minihub_module_host_api_t *host = minihub_miot_module_host();
    if (host && host->config_get_adapters) {
        return host->config_get_adapters();
    }
    return NULL;
}

static esp_err_t module_config_save_adapters(cJSON *root)
{
    const minihub_module_host_api_t *host = minihub_miot_module_host();
    if (!host || !host->config_save_adapters) {
        return ESP_ERR_INVALID_STATE;
    }
    return host->config_save_adapters(root);
}

esp_err_t minihub_miot_sync_devices(bool merge_existing, const char *home_id_opt,
                                  size_t *synced_count, size_t *lan_token_count,
                                  size_t *bindkey_count)
{
    if (synced_count) {
        *synced_count = 0;
    }
    if (lan_token_count) {
        *lan_token_count = 0;
    }
    if (bindkey_count) {
        *bindkey_count = 0;
    }

    cJSON *adapters_root = module_config_get_adapters();
    if (!adapters_root) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    if (!adapters) {
        adapters = cJSON_CreateObject();
        cJSON_AddItemToObject(adapters_root, "adapters", adapters);
    }

    cJSON *miot = cJSON_GetObjectItem(adapters, "miot");
    if (!miot) {
        miot = cJSON_CreateObject();
        cJSON_AddItemToObject(adapters, "miot", miot);
    }
    cJSON_DeleteItemFromObject(miot, "enabled");
    cJSON_AddTrueToObject(miot, "enabled");

    char home_id[64];
    home_id[0] = '\0';
    if (home_id_opt && home_id_opt[0]) {
        strncpy(home_id, home_id_opt, sizeof(home_id) - 1);
    } else {
        const cJSON *saved = cJSON_GetObjectItem(miot, "home_id");
        json_num_or_str(saved, home_id, sizeof(home_id));
    }
    if (!home_id[0]) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *cloud_list = NULL;
    esp_err_t err = minihub_miot_auth_device_list(&cloud_list);
    if (err != ESP_OK || !cloud_list) {
        ESP_LOGW(TAG, "device_list failed: %s (%s)", esp_err_to_name(err != ESP_OK ? err : ESP_FAIL),
                 minihub_miot_auth_last_error() ? minihub_miot_auth_last_error() : "-");
        return err != ESP_OK ? err : ESP_FAIL;
    }

    /* Pull Spec profiles for all models (cached under /data/config/).
     * Run on HTTPS worker — must not nest esp_http_client on the httpd RPC thread. */
    {
        spec_ensure_job_t job;
        memset(&job, 0, sizeof(job));
        cJSON *cd = NULL;
        cJSON_ArrayForEach(cd, cloud_list) {
            if (job.n >= 64 || should_skip_device(cd)) {
                continue;
            }
            const cJSON *mj = cJSON_GetObjectItem(cd, "model");
            if (!cJSON_IsString(mj) || !mj->valuestring[0] ||
                strlen(mj->valuestring) >= MIOT_SPEC_MODEL_MAX) {
                continue;
            }
            bool dup = false;
            for (size_t i = 0; i < job.n; i++) {
                if (strcmp(job.models[i], mj->valuestring) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                strncpy(job.models[job.n], mj->valuestring, MIOT_SPEC_MODEL_MAX - 1);
                job.n++;
            }
        }
        if (job.n > 0) {
            esp_err_t spec_err = miot_http_run_worker(spec_ensure_worker, &job);
            if (spec_err != ESP_OK) {
                ESP_LOGW(TAG, "Spec ensure incomplete: %s (using cache/heuristics)",
                         esp_err_to_name(spec_err));
            }
        }
    }

    cJSON *gethome = NULL;
    cJSON *place_map = NULL;
    bool filter_by_home = false;
    char home_name[64];
    home_name[0] = '\0';
    esp_err_t home_err = minihub_miot_auth_homeroom_gethome(&gethome);
    if (home_err == ESP_OK && gethome) {
        bool found = false;
        place_map = build_home_place_map(gethome, home_id, &found, home_name, sizeof(home_name));
        cJSON_Delete(gethome);
        gethome = NULL;
        if (!found) {
            if (strcmp(home_id, "0") == 0) {
                ESP_LOGW(TAG, "home_id 0 fallback; sync without room filter");
                cJSON_Delete(place_map);
                place_map = NULL;
                filter_by_home = false;
            } else {
                ESP_LOGW(TAG, "home_id %s not in gethome; need_home", home_id);
                cJSON_Delete(place_map);
                cJSON_Delete(cloud_list);
                cJSON_DeleteItemFromObject(miot, "home_id");
                cJSON_DeleteItemFromObject(miot, "home_name");
                (void)module_config_save_adapters(adapters_root);
                return ESP_ERR_NOT_FOUND;
            }
        } else {
            filter_by_home = true;
        }
        cJSON_DeleteItemFromObject(miot, "home_id");
        cJSON_DeleteItemFromObject(miot, "home_name");
        cJSON_AddStringToObject(miot, "home_id", home_id);
        if (home_name[0]) {
            cJSON_AddStringToObject(miot, "home_name", home_name);
        }
    } else {
        ESP_LOGW(TAG, "gethome failed (%s); sync without room filter",
                 esp_err_to_name(home_err));
        cJSON_DeleteItemFromObject(miot, "home_id");
        cJSON_DeleteItemFromObject(miot, "home_name");
        cJSON_AddStringToObject(miot, "home_id", home_id);
    }

    cJSON *existing_devices = cJSON_GetObjectItem(miot, "devices");
    cJSON *new_devices = cJSON_CreateArray();
    if (!new_devices) {
        cJSON_Delete(place_map);
        cJSON_Delete(cloud_list);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    size_t tokens = 0;
    cJSON *cloud_dev = NULL;
    cJSON_ArrayForEach(cloud_dev, cloud_list) {
        if (should_skip_device(cloud_dev)) {
            continue;
        }
        const cJSON *did = cJSON_GetObjectItem(cloud_dev, "did");
        if (!cJSON_IsString(did)) {
            continue;
        }
        miot_place_t place;
        bool in_home = place_from_map(place_map, did->valuestring, &place);
        if (filter_by_home && !in_home) {
            continue;
        }
        if (!in_home) {
            place.home_id = home_id;
            place.home_name = home_name[0] ? home_name : NULL;
            place.room_id = NULL;
            place.room_name = NULL;
        }
        cJSON *existing =
            merge_existing ? find_existing_by_did(existing_devices, did->valuestring) : NULL;
        bool entry_has_token = false;
        cJSON *entry =
            device_entry_from_cloud(cloud_dev, existing, &place, &entry_has_token);
        if (entry) {
            cJSON_AddItemToArray(new_devices, entry);
            count++;
            if (entry_has_token) {
                tokens++;
            }
        }
    }

    cJSON_DeleteItemFromObject(miot, "devices");
    cJSON_AddItemToObject(miot, "devices", new_devices);
    cJSON_Delete(place_map);

    /* BLE bindkeys for xiaomi_ble (local FE95 decrypt). Failures do not fail sync. */
    size_t bk_ok = 0;
    size_t bk_fail = 0;
    bool has_blt = false;
    cJSON_ArrayForEach(cloud_dev, cloud_list) {
        const cJSON *did = cJSON_GetObjectItem(cloud_dev, "did");
        if (cJSON_IsString(did) && strncmp(did->valuestring, "blt.", 4) == 0) {
            has_blt = true;
            break;
        }
    }
    if (has_blt) {
        cJSON *xble = cJSON_GetObjectItem(adapters, "xiaomi_ble");
        if (!xble) {
            xble = cJSON_CreateObject();
            if (xble) {
                cJSON_AddItemToObject(adapters, "xiaomi_ble", xble);
            }
        }
        if (xble) {
            cJSON_DeleteItemFromObject(xble, "enabled");
            cJSON_AddTrueToObject(xble, "enabled");
            cJSON *bindkeys = cJSON_GetObjectItem(xble, "bindkeys");
            if (!cJSON_IsObject(bindkeys)) {
                cJSON_DeleteItemFromObject(xble, "bindkeys");
                bindkeys = cJSON_CreateObject();
                if (bindkeys) {
                    cJSON_AddItemToObject(xble, "bindkeys", bindkeys);
                }
            }
            if (bindkeys) {
                esp_err_t bk_err = minihub_miot_auth_fetch_ble_bindkeys(cloud_list, bindkeys,
                                                                       &bk_ok, &bk_fail);
                if (bk_err != ESP_OK) {
                    ESP_LOGW(TAG, "ble bindkey sync skipped: %s", esp_err_to_name(bk_err));
                } else if (bk_ok || bk_fail) {
                    ESP_LOGI(TAG, "ble bindkeys: %u ok, %u fail",
                             (unsigned)bk_ok, (unsigned)bk_fail);
                }
            }
        }
    }

    cJSON_Delete(cloud_list);

    err = module_config_save_adapters(adapters_root);
    if (err != ESP_OK) {
        return err;
    }

    if (synced_count) {
        *synced_count = count;
    }
    if (lan_token_count) {
        *lan_token_count = tokens;
    }
    if (bindkey_count) {
        *bindkey_count = bk_ok;
    }
    ESP_LOGI(TAG, "synced %u miot devices home=%s (%u lan token, %u ble bindkeys)",
             (unsigned)count, home_id, (unsigned)tokens, (unsigned)bk_ok);
    return ESP_OK;
}

esp_err_t minihub_miot_devices_clear(size_t *removed_count)
{
    if (removed_count) {
        *removed_count = 0;
    }

    cJSON *adapters_root = module_config_get_adapters();
    if (!adapters_root) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    if (!adapters) {
        return ESP_OK;
    }

    cJSON *miot = cJSON_GetObjectItem(adapters, "miot");
    if (!miot) {
        return ESP_OK;
    }

    cJSON *devices = cJSON_GetObjectItem(miot, "devices");
    size_t count = cJSON_IsArray(devices) ? (size_t)cJSON_GetArraySize(devices) : 0;

    cJSON_DeleteItemFromObject(miot, "devices");
    cJSON_AddItemToObject(miot, "devices", cJSON_CreateArray());
    cJSON_DeleteItemFromObject(miot, "home_id");
    cJSON_DeleteItemFromObject(miot, "home_name");

    esp_err_t err = module_config_save_adapters(adapters_root);
    if (err != ESP_OK) {
        return err;
    }

    if (removed_count) {
        *removed_count = count;
    }
    ESP_LOGI(TAG, "cleared %u miot device(s) from %s", (unsigned)count, MINIHUB_ADAPTERS_JSON);
    return ESP_OK;
}
