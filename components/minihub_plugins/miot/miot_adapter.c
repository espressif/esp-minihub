/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "minihub_miot_auth.h"
#include "minihub_net.h"
#include "miio_lan.h"
#include "miot_car_actions.h"
#include "miot_rpc.h"
#include "miot_spec.h"
#include "sdkconfig.h"
#include "minihub_module.h"
#include "module_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "adapter_miot";

#define MAX_MIOT_DEVICES 48

typedef enum {
    MIOT_DEV_AUTO = 0,
    MIOT_DEV_LAN,
    MIOT_DEV_CLOUD,
} miot_dev_mode_t;

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    char did[32];
    char model[64];
    char mac[24];
    char ip[48];
    uint8_t token[16];
    bool has_token;
    uint32_t miio_id;
    miot_dev_mode_t mode;
    int siid;
    int piid_on;
    int piid_bright;
    int bright_max;
    int piid_ct;
    int piid_color;
    int piid_mode;
    int piid_target_temp;
    int siid_fan;
    int piid_fan;
    int siid_env;
    int piid_temp;
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
    bool no_control;
    bool supports_brightness;
    bool supports_color_temp;
    bool supports_color;
    bool invert_contact;
    bool controllable;
    miot_spec_extra_t extras[MIOT_SPEC_EXTRAS_MAX];
    size_t extras_n;
    bool lan_ready;
    uint32_t lan_probe_at_ms;
    /* Consecutive get_prop failures; mark point offline at >= 3. */
    uint8_t prop_fail_streak;
    /* LWW reconcile vs paired xiaomi_ble binary_sensor (blt. magnets) */
    bool lww_paired;
    char local_point_id[MINIHUB_POINT_ID_MAX];
    char local_value[MINIHUB_VALUE_MAX];
    char local_prev[MINIHUB_VALUE_MAX];
    char cloud_value[MINIHUB_VALUE_MAX];
    char cloud_prev[MINIHUB_VALUE_MAX];
    int64_t local_ms;
    int64_t cloud_ms;
    char last_write_value[MINIHUB_VALUE_MAX];
    int64_t last_write_attempt_ms;
    bool cloud_write_ok;
    char cloud_write_error[48];
    char display_name[64];
    char room_id[32];
    char room_name[48];
    char home_name[48];
} miot_device_t;

#define MIOT_LWW_WRITE_RETRY_MS  60000

static miot_device_t s_devices[MAX_MIOT_DEVICES];
static size_t s_device_count;
static miot_dev_mode_t s_default_mode = MIOT_DEV_AUTO;

static esp_err_t set_bool_prop(miot_device_t *dev, bool on);

static miio_discovered_t s_discovered[MIIO_DISCOVER_MAX];
static size_t s_discovered_count;
static uint32_t s_discovered_at_ms;

#define MIOT_DISCOVER_TTL_MS   120000
#define MIOT_DISCOVER_WAIT_MS  2500
#define MIOT_LAN_REPROBE_MS    120000

static miot_device_t *find_device(const char *point_id);
static bool cloud_path_available(const miot_device_t *dev);

static miot_device_t *find_device(const char *point_id)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].point_id, point_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

/** Xiaomi BLE (did blt.*) never speaks miIO UDP 54321 — cloud / FE95 only. */
static bool device_is_ble(const miot_device_t *dev)
{
    return dev && dev->did[0] && strncmp(dev->did, "blt.", 4) == 0;
}

/**
 * Models that sync a miIO token/localip but rarely answer UDP 54321
 * (camera / TV / speaker / lock), plus Spec cloud-only types.
 */
static bool model_cloud_only(const char *model)
{
    if (!model || !model[0]) {
        return false;
    }
    if (strstr(model, ".camera.") || strstr(model, "camera.")) {
        return true;
    }
    if (strstr(model, ".tv.") || strncmp(model, "mitv.", 5) == 0) {
        return true;
    }
    if (strstr(model, "wifispeaker") || strstr(model, ".speaker.")) {
        return true;
    }
    if (strstr(model, ".lock.")) {
        return true;
    }
    return false;
}

static bool device_cloud_only(const miot_device_t *dev)
{
    if (!dev) {
        return false;
    }
    /* Gateway child / VRF indoor units only speak Spec via cloud. */
    if (dev->did[0] && strncmp(dev->did, "proxy.", 6) == 0) {
        return true;
    }
    if (device_is_ble(dev) || model_cloud_only(dev->model)) {
        return true;
    }
    return strcmp(dev->type, "camera") == 0 ||
           strcmp(dev->type, "television") == 0 ||
           strcmp(dev->type, "lock") == 0 ||
           strcmp(dev->type, "speaker") == 0 ||
           strcmp(dev->type, "car") == 0 ||
           strcmp(dev->type, "air-conditioner") == 0;
}

static bool device_has_lan(const miot_device_t *dev)
{
    /* Ignore leftover tokens on BLE / cloud-only entries. */
    return dev && dev->has_token && !device_cloud_only(dev);
}

static bool device_wants_lan(const miot_device_t *dev)
{
    if (!device_has_lan(dev) || device_cloud_only(dev)) {
        return false;
    }
    if (dev->mode == MIOT_DEV_LAN) {
        return true;
    }
    if (dev->mode == MIOT_DEV_CLOUD) {
        return false;
    }
    return true;
}

static bool device_can_try_lan(const miot_device_t *dev)
{
    return device_wants_lan(dev);
}

static void refresh_lan_discover(bool force)
{
    uint32_t now = (uint32_t)esp_log_timestamp();
    if (!force && s_discovered_count > 0 &&
        (now - s_discovered_at_ms) < MIOT_DISCOVER_TTL_MS) {
        return;
    }
    s_discovered_count = 0;
    if (miio_lan_discover(s_discovered, MIIO_DISCOVER_MAX, &s_discovered_count,
                          MIOT_DISCOVER_WAIT_MS) != ESP_OK) {
        s_discovered_count = 0;
    }
    s_discovered_at_ms = now;
}

static esp_err_t ensure_lan_ip(miot_device_t *dev, bool force_discover)
{
    if (!device_has_lan(dev)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dev->ip[0] && !force_discover) {
        esp_err_t probe_err = miio_lan_probe(dev->ip, dev->token, &dev->miio_id);
        if (probe_err == ESP_OK) {
            return ESP_OK;
        }
        /* auto + cloud: do not walk discover candidates (each miss ≈3–6s). */
        if (dev->mode == MIOT_DEV_AUTO && cloud_path_available(dev)) {
            return probe_err;
        }
    }

    refresh_lan_discover(force_discover || s_discovered_count == 0);
    if (s_discovered_count == 0) {
        return dev->ip[0] ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FOUND;
    }

    char ip[sizeof(dev->ip)];
    esp_err_t err = miio_lan_bind_ip(dev->token, dev->model, dev->mac,
                                     s_discovered, s_discovered_count,
                                     ip, sizeof(ip), &dev->miio_id);
    if (err != ESP_OK) {
        return err;
    }
    if (strcmp(dev->ip, ip) != 0) {
        ESP_LOGI(TAG, "%s resolved lan ip %s -> %s", dev->point_id,
                 dev->ip[0] ? dev->ip : "(none)", ip);
    }
    strncpy(dev->ip, ip, sizeof(dev->ip) - 1);
    dev->ip[sizeof(dev->ip) - 1] = '\0';
    return ESP_OK;
}

/** AUTO: use LAN only after a successful probe; LAN mode always tries; cloud mode never. */
static bool lan_path_eligible(const miot_device_t *dev)
{
    if (!dev || strcmp(dev->type, "car") == 0) {
        return false;
    }
    if (!device_has_lan(dev) || dev->mode == MIOT_DEV_CLOUD) {
        return false;
    }
    if (dev->mode == MIOT_DEV_LAN) {
        return true;
    }
    return dev->lan_ready;
}

static void mark_lan_probe(miot_device_t *dev, bool ok)
{
    dev->lan_ready = ok;
    dev->lan_probe_at_ms = (uint32_t)esp_log_timestamp();
}

static esp_err_t lan_ensure_ready(miot_device_t *dev)
{
    esp_err_t err = ensure_lan_ip(dev, false);
    mark_lan_probe(dev, err == ESP_OK);
    return err;
}

static bool cloud_path_available(const miot_device_t *dev)
{
    if (!dev || dev->mode == MIOT_DEV_LAN) {
        return false;
    }
    return minihub_miot_auth_has_session() && dev->did[0] != '\0';
}

/** Background LAN bind / re-probe (rate-limited; runs from poll task). */
static void maybe_reprobe_lan(miot_device_t *dev)
{
    if (!device_can_try_lan(dev)) {
        return;
    }
    if (dev->lan_ready && dev->ip[0]) {
        return;
    }
    uint32_t now = (uint32_t)esp_log_timestamp();
    /* When cloud can serve state, defer LAN bind so poll is not starved by
     * UDP timeouts (each miss ≈3–6s; with 16+ LAN devices AC/proxy never update). */
    if (cloud_path_available(dev)) {
        if (dev->lan_probe_at_ms == 0) {
            dev->lan_probe_at_ms = now;
            return;
        }
        if ((now - dev->lan_probe_at_ms) < MIOT_LAN_REPROBE_MS) {
            return;
        }
    } else if (dev->lan_probe_at_ms != 0 &&
               (now - dev->lan_probe_at_ms) < MIOT_LAN_REPROBE_MS) {
        /* First attempt ASAP when cloud is unavailable. */
        return;
    }
    if (lan_ensure_ready(dev) == ESP_OK) {
        ESP_LOGI(TAG, "%s lan probe ok", dev->point_id);
    }
}

static void log_control_path(const miot_device_t *dev, const char *action, bool via_lan)
{
    ESP_LOGI(TAG, "%s %s via %s", dev->point_id, action, via_lan ? "LAN" : "cloud");
}

static bool device_online(const miot_device_t *dev)
{
    if (dev->mode == MIOT_DEV_LAN) {
        return device_can_try_lan(dev);
    }
    if (dev->mode == MIOT_DEV_CLOUD) {
        return cloud_path_available(dev);
    }
    return device_can_try_lan(dev) || cloud_path_available(dev);
}

static esp_err_t value_to_string(cJSON *value, char *out, size_t out_len)
{
    if (!value || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsBool(value)) {
        strncpy(out, cJSON_IsTrue(value) ? "on" : "off", out_len - 1);
    } else if (cJSON_IsNumber(value)) {
        snprintf(out, out_len, "%.1f", value->valuedouble);
    } else if (cJSON_IsString(value)) {
        strncpy(out, value->valuestring, out_len - 1);
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    out[out_len - 1] = '\0';
    return ESP_OK;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool contact_is_known(const char *v)
{
    return v && (strcmp(v, "open") == 0 || strcmp(v, "closed") == 0 ||
                 strcmp(v, "left_open") == 0);
}

static bool model_is_magnet(const char *model)
{
    if (!model || !model[0]) {
        return false;
    }
    if (strstr(model, "sensor_magnet") || strstr(model, ".magnet.")) {
        return true;
    }
    return strstr(model, "magnet") && !strstr(model, "vacuum");
}

static bool lww_eligible(const miot_device_t *dev)
{
    return dev && strcmp(dev->type, "binary_sensor") == 0 &&
           strncmp(dev->did, "blt.", 4) == 0 && dev->mac[0] &&
           model_is_magnet(dev->model);
}

/** Normalize to lowercase aa:bb:cc:dd:ee:ff (or empty on failure). */
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

static bool normalize_mac(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len < 18) {
        return false;
    }
    uint8_t bytes[6];
    size_t n = 0;
    const char *p = in;
    while (*p && n < 6) {
        while (*p == ':' || *p == '-' || *p == ' ') {
            p++;
        }
        int hi = hex_nibble(p[0]);
        int lo = hex_nibble(p[1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    if (n != 6) {
        return false;
    }
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return true;
}

/** Printed MAC → xiaomi_ble mac4 (byte1||byte0), matching NimBLE LE addr[4]/addr[5]. */
static bool mac_to_xiaomi_point_id(const char *mac, char *out, size_t out_len)
{
    char norm[24];
    if (!normalize_mac(mac, norm, sizeof(norm))) {
        return false;
    }
    /* norm[0..1]=byte0, [3..4]=byte1 */
    snprintf(out, out_len, "binary_sensor/xiaomi_%c%c%c%c",
             norm[3], norm[4], norm[0], norm[1]);
    return true;
}

/** Hub contact → cloud bool (true=closed unless invert_contact). */
static bool contact_to_cloud_bool(const miot_device_t *dev, const char *contact)
{
    bool closed = (strcmp(contact, "closed") == 0);
    if (dev->invert_contact) {
        closed = !closed;
    }
    return closed;
}

static void accept_side_change(char *cur, char *prev, size_t n, int64_t *ms,
                               const char *next, int64_t next_ms)
{
    if (!next || !next[0]) {
        return;
    }
    if (cur[0] && strcmp(cur, next) == 0) {
        return;
    }
    if (cur[0]) {
        strncpy(prev, cur, n - 1);
        prev[n - 1] = '\0';
    } else {
        prev[0] = '\0';
    }
    strncpy(cur, next, n - 1);
    cur[n - 1] = '\0';
    *ms = next_ms > 0 ? next_ms : now_ms();
}

static void set_cloud_write_props(miot_device_t *dev, bool ok, const char *err_msg)
{
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(dev->point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }
    cJSON_DeleteItemFromObject(props, "cloud_write_ok");
    cJSON_DeleteItemFromObject(props, "cloud_write_error");
    cJSON_AddBoolToObject(props, "cloud_write_ok", ok);
    if (!ok && err_msg && err_msg[0]) {
        cJSON_AddStringToObject(props, "cloud_write_error", err_msg);
    }
    minihub_point_set(dev->point_id, snap.value[0] ? snap.value : dev->cloud_value, props);
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
    dev->cloud_write_ok = ok;
    if (ok) {
        dev->cloud_write_error[0] = '\0';
    } else if (err_msg) {
        strncpy(dev->cloud_write_error, err_msg, sizeof(dev->cloud_write_error) - 1);
        dev->cloud_write_error[sizeof(dev->cloud_write_error) - 1] = '\0';
    }
}

static void lww_try_cloud_write(miot_device_t *dev, const char *winner_value)
{
    if (!contact_is_known(winner_value)) {
        return;
    }
    int64_t t = now_ms();
    if (dev->last_write_value[0] && strcmp(dev->last_write_value, winner_value) == 0) {
        if (dev->cloud_write_ok) {
            return;
        }
        if (dev->last_write_attempt_ms &&
            (t - dev->last_write_attempt_ms) < MIOT_LWW_WRITE_RETRY_MS) {
            return;
        }
    }
    if (!cloud_path_available(dev)) {
        set_cloud_write_props(dev, false, "no_session");
        return;
    }
    bool cloud_bool = contact_to_cloud_bool(dev, winner_value);
    ESP_LOGI(TAG, "lww write-back %s local=%s -> cloud bool=%s",
             dev->point_id, winner_value, cloud_bool ? "true" : "false");
    esp_err_t err = set_bool_prop(dev, cloud_bool);
    strncpy(dev->last_write_value, winner_value, sizeof(dev->last_write_value) - 1);
    dev->last_write_value[sizeof(dev->last_write_value) - 1] = '\0';
    dev->last_write_attempt_ms = t;
    if (err == ESP_OK) {
        set_cloud_write_props(dev, true, NULL);
        /* Align tracked cloud value without inventing a newer cloud event. */
        strncpy(dev->cloud_value, winner_value, sizeof(dev->cloud_value) - 1);
        dev->cloud_value[sizeof(dev->cloud_value) - 1] = '\0';
    } else {
        set_cloud_write_props(dev, false, esp_err_to_name(err));
        ESP_LOGW(TAG, "lww write-back %s failed: %s (local remains authority)",
                 dev->point_id, esp_err_to_name(err));
    }
}

static void reconcile_lww(miot_device_t *dev)
{
    if (!dev->lww_paired || !dev->local_point_id[0]) {
        return;
    }

    minihub_point_t local = {0};
    if (minihub_point_get_snapshot(dev->local_point_id, &local) == ESP_OK) {
        accept_side_change(dev->local_value, dev->local_prev, sizeof(dev->local_value),
                           &dev->local_ms, local.value, local.updated_ms);
        if (local.props) {
            cJSON_Delete(local.props);
        }
    }

    const bool local_known = contact_is_known(dev->local_value);
    const bool cloud_known = contact_is_known(dev->cloud_value);
    if (!local_known && !cloud_known) {
        return;
    }

    enum { WIN_NONE, WIN_LOCAL, WIN_CLOUD } win = WIN_NONE;
    if (local_known && cloud_known) {
        if (dev->local_ms > dev->cloud_ms) {
            win = WIN_LOCAL;
        } else if (dev->cloud_ms > dev->local_ms) {
            win = WIN_CLOUD;
        } else {
            win = WIN_LOCAL; /* tie → local */
        }
    } else if (local_known) {
        win = WIN_LOCAL;
    } else {
        win = WIN_CLOUD;
    }

    const char *winner = (win == WIN_LOCAL) ? dev->local_value : dev->cloud_value;
    const char *winner_id = (win == WIN_LOCAL) ? dev->local_point_id : dev->point_id;
    const char *loser_id = (win == WIN_LOCAL) ? dev->point_id : dev->local_point_id;
    char *loser_cur = (win == WIN_LOCAL) ? dev->cloud_value : dev->local_value;

    if (!contact_is_known(winner)) {
        return;
    }

    /* Align both Hub points to the winner (poll path does not blindly point_set). */
    (void)minihub_point_set(winner_id, winner, NULL);
    if (strcmp(loser_cur, winner) != 0) {
        if (minihub_point_set(loser_id, winner, NULL) == ESP_OK) {
            strncpy(loser_cur, winner, MINIHUB_VALUE_MAX - 1);
            loser_cur[MINIHUB_VALUE_MAX - 1] = '\0';
            ESP_LOGI(TAG, "lww %s wins -> set %s=%s",
                     win == WIN_LOCAL ? "local" : "cloud", loser_id, winner);
        }
    }

    if (win == WIN_LOCAL) {
        lww_try_cloud_write(dev, winner);
    }
}

/** Cover → position; speaker → volume; lock → door-state; else on/power. */
static int primary_piid(const miot_device_t *dev)
{
    if (!dev) {
        return 0;
    }
    if (strcmp(dev->type, "cover") == 0 && dev->piid_position > 0) {
        return dev->piid_position;
    }
    if (dev->piid_on > 0) {
        return dev->piid_on;
    }
    if (strcmp(dev->type, "speaker") == 0 && dev->piid_volume > 0) {
        return dev->piid_volume;
    }
    if (strcmp(dev->type, "lock") == 0 && dev->piid_door > 0) {
        return dev->piid_door;
    }
    return 0;
}

static const char *lock_door_label(int state)
{
    switch (state) {
    case 1:
        return "已锁";
    case 2:
        return "未锁";
    case 3:
        return "超时未关";
    case 4:
        return "虚掩";
    case 5:
        return "童锁";
    case 6:
        return "反锁";
    default:
        return NULL;
    }
}

static esp_err_t map_poll_value(const miot_device_t *dev, cJSON *value,
                                char *out, size_t out_len)
{
    /* Magnet/contact: Spec bool true → closed, false → open (Web cardHtml).
     * Per-device invert_contact flips that mapping. */
    if (strcmp(dev->type, "binary_sensor") == 0 && cJSON_IsBool(value)) {
        bool closed = cJSON_IsTrue(value);
        if (dev->invert_contact) {
            closed = !closed;
        }
        strncpy(out, closed ? "closed" : "open", out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    if (strcmp(dev->type, "binary_sensor") == 0 && cJSON_IsNumber(value)) {
        /* Some gateways return 0/1 instead of bool for contact-state. */
        bool closed = value->valuedouble != 0;
        if (dev->invert_contact) {
            closed = !closed;
        }
        strncpy(out, closed ? "closed" : "open", out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    if (strcmp(dev->type, "smoke") == 0 && cJSON_IsNumber(value)) {
        int s = value->valueint;
        const char *lab = (s == 2) ? "fault" : "ok";
        strncpy(out, lab, out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    /* Curtain position 0–100 — integer string, not "%.1f". */
    if (strcmp(dev->type, "cover") == 0 && cJSON_IsNumber(value)) {
        int pos = value->valueint;
        if (pos < 0) {
            pos = 0;
        }
        if (pos > 100) {
            pos = 100;
        }
        snprintf(out, out_len, "%d", pos);
        return ESP_OK;
    }
    if (strcmp(dev->type, "cover") == 0 && cJSON_IsString(value) && value->valuestring) {
        int pos = atoi(value->valuestring);
        if (pos < 0) {
            pos = 0;
        }
        if (pos > 100) {
            pos = 100;
        }
        snprintf(out, out_len, "%d", pos);
        return ESP_OK;
    }
    /* Lock door-state enum. */
    if (strcmp(dev->type, "lock") == 0 && cJSON_IsNumber(value)) {
        const char *lab = lock_door_label(value->valueint);
        if (lab) {
            strncpy(out, lab, out_len - 1);
            out[out_len - 1] = '\0';
        } else {
            snprintf(out, out_len, "%d", value->valueint);
        }
        return ESP_OK;
    }
    /* Speaker volume (no power on). */
    if (strcmp(dev->type, "speaker") == 0 && dev->piid_on <= 0 &&
        cJSON_IsNumber(value)) {
        snprintf(out, out_len, "音量%d", value->valueint);
        return ESP_OK;
    }
    if ((strcmp(dev->type, "router") == 0 || strcmp(dev->type, "switch") == 0 ||
         strcmp(dev->type, "light") == 0 ||
         strcmp(dev->type, "air-conditioner") == 0 ||
         strcmp(dev->type, "camera") == 0 ||
         strcmp(dev->type, "television") == 0) &&
        cJSON_IsBool(value)) {
        bool on = cJSON_IsTrue(value);
        if (dev->invert_on) {
            on = !on;
        }
        strncpy(out, on ? "on" : "off", out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    /* Some devices return 0/1 instead of bool (incl. TV sleep-mode). */
    if ((strcmp(dev->type, "switch") == 0 || strcmp(dev->type, "light") == 0 ||
         strcmp(dev->type, "air-conditioner") == 0 ||
         strcmp(dev->type, "camera") == 0 ||
         strcmp(dev->type, "television") == 0) &&
        cJSON_IsNumber(value) &&
        (value->valuedouble == 0.0 || value->valuedouble == 1.0)) {
        bool on = value->valuedouble != 0.0;
        if (dev->invert_on) {
            on = !on;
        }
        strncpy(out, on ? "on" : "off", out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    return value_to_string(value, out, out_len);
}


/**
 * Motion 2S etc.: no pollable occupancy. Mi Home / HA use no-motion-duration —
 * cloud code -704220043 means occupied ("有人移动"); a numeric value means clear.
 */
static esp_err_t get_motion_occupancy(miot_device_t *dev, char *out, size_t out_len)
{
    if (!dev || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int piid = dev->piid_no_motion > 0 ? dev->piid_no_motion : 1024;
    int siid = dev->siid > 0 ? dev->siid : 2;
    if (!cloud_path_available(dev) || !minihub_net_has_ip()) {
        /* Keep last known soft state while offline. */
        strncpy(out, "clear", out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }

    cJSON *pairs = cJSON_CreateArray();
    cJSON *pair = cJSON_CreateObject();
    if (!pairs || !pair ||
        !cJSON_AddNumberToObject(pair, "siid", siid) ||
        !cJSON_AddNumberToObject(pair, "piid", piid) ||
        !cJSON_AddItemToArray(pairs, pair)) {
        cJSON_Delete(pair);
        cJSON_Delete(pairs);
        return ESP_ERR_NO_MEM;
    }

    cJSON *result = NULL;
    esp_err_t err = minihub_miot_auth_prop_get_multi(dev->did, pairs, &result);
    cJSON_Delete(pairs);
    if (err != ESP_OK || !cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) {
        cJSON_Delete(result);
        /* Network/API failure — do not flap offline on motion sensors. */
        strncpy(out, "clear", out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }

    const cJSON *item = cJSON_GetArrayItem(result, 0);
    const cJSON *code = cJSON_GetObjectItem(item, "code");
    int code_i = cJSON_IsNumber(code) ? code->valueint : 0;
    const char *state = "clear";
    if (code_i == -704220043) {
        /* Property unavailable while motion active (Mi Home "有人移动"). */
        state = "detected";
    } else if (code_i == 0 || code_i == 1) {
        state = "clear";
    } else {
        ESP_LOGD(TAG, "motion %s no-motion-duration code=%d", dev->point_id, code_i);
        state = "clear";
    }
    strncpy(out, state, out_len - 1);
    out[out_len - 1] = '\0';
    cJSON_Delete(result);
    return ESP_OK;
}

static esp_err_t get_prop(miot_device_t *dev, char *out, size_t out_len)
{
    /* Motion occupancy via no-motion-duration (before primary_piid soft path). */
    if (strcmp(dev->type, "binary_sensor") == 0 && dev->piid_on <= 0 &&
        (dev->piid_no_motion > 0 ||
         (dev->model[0] && strstr(dev->model, "motion")))) {
        return get_motion_occupancy(dev, out, out_len);
    }

    const int piid = primary_piid(dev);
    if (dev->siid <= 0 || piid <= 0) {
        /* Event-only remotes: stay online without fake power polls. */
        if (strcmp(dev->type, "button") == 0) {
            strncpy(out, "idle", out_len - 1);
        } else if (strcmp(dev->type, "binary_sensor") == 0) {
            strncpy(out, "clear", out_len - 1);
        } else {
            strncpy(out, "unknown", out_len - 1);
        }
        out[out_len - 1] = '\0';
        return ESP_OK;
    }

    cJSON *value = NULL;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    bool lan_attempted = false;

    if (lan_path_eligible(dev)) {
        lan_attempted = true;
        if (dev->mode == MIOT_DEV_AUTO && dev->lan_ready && dev->ip[0]) {
            err = miio_lan_get_property(dev->ip, dev->token, &dev->miio_id,
                                        dev->siid, piid, &value);
        } else {
            err = lan_ensure_ready(dev);
            if (err == ESP_OK) {
                err = miio_lan_get_property(dev->ip, dev->token, &dev->miio_id,
                                            dev->siid, piid, &value);
            }
        }
        if (!(err == ESP_OK && value) && dev->mode == MIOT_DEV_AUTO) {
            mark_lan_probe(dev, false);
        }
    } else if (dev->mode == MIOT_DEV_LAN && device_has_lan(dev)) {
        lan_attempted = true;
        err = lan_ensure_ready(dev);
        if (err == ESP_OK) {
            err = miio_lan_get_property(dev->ip, dev->token, &dev->miio_id,
                                        dev->siid, piid, &value);
        }
    }

    if (!value && dev->mode != MIOT_DEV_LAN && cloud_path_available(dev)) {
        if (!minihub_net_has_ip()) {
            /* Poll starts before STA GOT_IP; skip cloud to avoid DNS spam. */
            return ESP_ERR_INVALID_STATE;
        }
        err = minihub_miot_auth_prop_get(dev->did, dev->siid, piid, &value);
    } else if (!value && !lan_attempted) {
        return ESP_ERR_INVALID_STATE;
    }

    if (err != ESP_OK || !value) {
        return err != ESP_OK ? err : ESP_FAIL;
    }
    err = map_poll_value(dev, value, out, out_len);
    cJSON_Delete(value);
    return err;
}

/** Best-effort property read on an arbitrary siid/piid (fan/env may differ). */
static esp_err_t get_siid_piid_json(miot_device_t *dev, int siid, int piid,
                                   cJSON **out_value)
{
    if (!dev || siid <= 0 || piid <= 0 || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = NULL;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (lan_path_eligible(dev) && dev->lan_ready && dev->ip[0]) {
        err = miio_lan_get_property(dev->ip, dev->token, &dev->miio_id,
                                    siid, piid, out_value);
        if (err == ESP_OK && *out_value) {
            return ESP_OK;
        }
        if (*out_value) {
            cJSON_Delete(*out_value);
            *out_value = NULL;
        }
    }
    if (dev->mode != MIOT_DEV_LAN && cloud_path_available(dev)) {
        if (!minihub_net_has_ip()) {
            return ESP_ERR_INVALID_STATE;
        }
        return minihub_miot_auth_prop_get(dev->did, siid, piid, out_value);
    }
    return err != ESP_OK ? err : ESP_FAIL;
}

/** Best-effort extra piid read on primary siid (same path preference as on/off). */
static esp_err_t get_piid_json(miot_device_t *dev, int piid, cJSON **out_value)
{
    return get_siid_piid_json(dev, dev ? dev->siid : 0, piid, out_value);
}

static size_t s_poll_idx;
static uint32_t s_light_attr_at_ms[MAX_MIOT_DEVICES];
static uint32_t s_climate_attr_at_ms[MAX_MIOT_DEVICES];
static uint32_t s_cover_attr_at_ms[MAX_MIOT_DEVICES];
static uint32_t s_meter_attr_at_ms[MAX_MIOT_DEVICES];
static uint32_t s_extra_attr_at_ms[MAX_MIOT_DEVICES];
static uint32_t s_misc_attr_at_ms[MAX_MIOT_DEVICES];

static const char *climate_mode_label(int mode)
{
    switch (mode) {
    case 1:
        return "制冷";
    case 2:
        return "除湿";
    case 4:
        return "送风";
    case 8:
        return "制热";
    default:
        return NULL;
    }
}

static const char *climate_fan_label(int fan)
{
    switch (fan) {
    case 0:
        return "自动";
    case 1:
        return "低";
    case 2:
        return "中";
    case 4:
        return "高";
    default:
        return NULL;
    }
}

static int json_number_as_int(const cJSON *v)
{
    if (!v) {
        return 0;
    }
    if (cJSON_IsNumber(v)) {
        return v->valueint;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        return atoi(v->valuestring);
    }
    return 0;
}

static bool json_to_bool(const cJSON *v, bool *out)
{
    if (!v || !out) {
        return false;
    }
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
        return true;
    }
    if (cJSON_IsNumber(v)) {
        *out = v->valuedouble != 0.0;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        const char *s = v->valuestring;
        *out = (strcmp(s, "true") == 0 || strcmp(s, "on") == 0 || strcmp(s, "1") == 0);
        return true;
    }
    return false;
}

static const miot_spec_extra_t *find_extra_by_key(const miot_device_t *dev, const char *key)
{
    if (!dev || !key || !key[0]) {
        return NULL;
    }
    for (size_t i = 0; i < dev->extras_n; i++) {
        if (strcmp(dev->extras[i].key, key) == 0) {
            return &dev->extras[i];
        }
    }
    return NULL;
}

static const miot_spec_extra_t *find_extra_by_siid_piid(const miot_device_t *dev, int siid, int piid)
{
    if (!dev || siid <= 0 || piid <= 0) {
        return NULL;
    }
    for (size_t i = 0; i < dev->extras_n; i++) {
        if (dev->extras[i].siid == siid && dev->extras[i].piid == piid) {
            return &dev->extras[i];
        }
    }
    return NULL;
}

static cJSON *device_extras_to_json(const miot_device_t *dev)
{
    if (!dev || dev->extras_n == 0) {
        return NULL;
    }
    size_t n = dev->extras_n;
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
        cJSON_AddStringToObject(o, "key", dev->extras[i].key);
        cJSON_AddStringToObject(o, "kind", dev->extras[i].kind);
        cJSON_AddNumberToObject(o, "siid", dev->extras[i].siid);
        cJSON_AddNumberToObject(o, "piid", dev->extras[i].piid);
        cJSON_AddNumberToObject(o, "aiid", dev->extras[i].aiid);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}


/** Hub UI uses 1–100%; Spec may use 1–65535 (Yeelight etc.). */
static int bright_scale_max(const miot_device_t *dev)
{
    if (!dev || dev->bright_max <= 0) {
        return 100;
    }
    return dev->bright_max;
}

static int bright_pct_to_raw(const miot_device_t *dev, int pct)
{
    int maxv = bright_scale_max(dev);
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    if (maxv <= 100) {
        return pct;
    }
    if (pct <= 0) {
        return 0;
    }
    int raw = (int)(((long)pct * maxv + 50) / 100);
    if (raw < 1) {
        raw = 1;
    }
    if (raw > maxv) {
        raw = maxv;
    }
    return raw;
}

static int bright_raw_to_pct(const miot_device_t *dev, int raw)
{
    int maxv = bright_scale_max(dev);
    if (raw < 0) {
        raw = 0;
    }
    if (maxv <= 100) {
        if (raw > 100) {
            raw = 100;
        }
        return raw;
    }
    int pct = (int)(((long)raw * 100 + maxv / 2) / maxv);
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    return pct;
}

static void apply_light_attr_props(miot_device_t *dev, const char *value)
{
    if (!dev || strcmp(dev->type, "light") != 0) {
        return;
    }
    if (dev->piid_bright <= 0 && dev->piid_ct <= 0) {
        return;
    }
    size_t idx = (size_t)(dev - s_devices);
    if (idx >= MAX_MIOT_DEVICES) {
        return;
    }
    uint32_t now = (uint32_t)esp_log_timestamp();
    /* Bright/ct are extra HTTPS/LAN reads; refresh at most every 30s. */
    if (s_light_attr_at_ms[idx] != 0 && (now - s_light_attr_at_ms[idx]) < 30000) {
        return;
    }

    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(dev->point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }

    bool touched = false;
    if (dev->piid_bright > 0) {
        cJSON *bv = NULL;
        if (get_piid_json(dev, dev->piid_bright, &bv) == ESP_OK && bv) {
            int bright = 0;
            if (cJSON_IsNumber(bv)) {
                bright = bv->valueint;
            } else if (cJSON_IsString(bv) && bv->valuestring) {
                bright = atoi(bv->valuestring);
            }
            bright = bright_raw_to_pct(dev, bright);
            cJSON_DeleteItemFromObject(props, "brightness");
            cJSON_AddNumberToObject(props, "brightness", bright);
            touched = true;
            cJSON_Delete(bv);
        }
    }
    if (dev->piid_ct > 0) {
        cJSON *cv = NULL;
        if (get_piid_json(dev, dev->piid_ct, &cv) == ESP_OK && cv) {
            int ct = 0;
            if (cJSON_IsNumber(cv)) {
                ct = cv->valueint;
            } else if (cJSON_IsString(cv) && cv->valuestring) {
                ct = atoi(cv->valuestring);
            }
            if (ct > 0) {
                cJSON_DeleteItemFromObject(props, "color_temp");
                cJSON_AddNumberToObject(props, "color_temp", ct);
                touched = true;
            }
            cJSON_Delete(cv);
        }
    }

    if (touched) {
        s_light_attr_at_ms[idx] = now;
        const char *use_val = (value && value[0]) ? value
                             : (snap.value[0] ? snap.value : "off");
        minihub_point_set(dev->point_id, use_val, props);
    }
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

static void apply_climate_attr_props(miot_device_t *dev, const char *value)
{
    if (!dev || strcmp(dev->type, "air-conditioner") != 0) {
        return;
    }
    if (dev->piid_mode <= 0 && dev->piid_target_temp <= 0 &&
        dev->piid_fan <= 0 && dev->piid_temp <= 0) {
        return;
    }
    size_t idx = (size_t)(dev - s_devices);
    if (idx >= MAX_MIOT_DEVICES) {
        return;
    }
    uint32_t now = (uint32_t)esp_log_timestamp();
    if (s_climate_attr_at_ms[idx] != 0 && (now - s_climate_attr_at_ms[idx]) < 30000) {
        return;
    }

    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(dev->point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }

    bool touched = false;
    if (dev->piid_mode > 0) {
        cJSON *mv = NULL;
        if (get_piid_json(dev, dev->piid_mode, &mv) == ESP_OK && mv) {
            int mode = json_number_as_int(mv);
            cJSON_DeleteItemFromObject(props, "mode");
            cJSON_AddNumberToObject(props, "mode", mode);
            cJSON_DeleteItemFromObject(props, "mode_label");
            const char *lab = climate_mode_label(mode);
            if (lab) {
                cJSON_AddStringToObject(props, "mode_label", lab);
            }
            touched = true;
            cJSON_Delete(mv);
        }
    }
    if (dev->piid_target_temp > 0) {
        cJSON *tv = NULL;
        if (get_piid_json(dev, dev->piid_target_temp, &tv) == ESP_OK && tv) {
            if (cJSON_IsNumber(tv)) {
                cJSON_DeleteItemFromObject(props, "target_temp");
                cJSON_AddNumberToObject(props, "target_temp", tv->valuedouble);
                touched = true;
            } else if (cJSON_IsString(tv) && tv->valuestring) {
                cJSON_DeleteItemFromObject(props, "target_temp");
                cJSON_AddNumberToObject(props, "target_temp", atof(tv->valuestring));
                touched = true;
            }
            cJSON_Delete(tv);
        }
    }
    if (dev->piid_fan > 0) {
        int fan_siid = dev->siid_fan > 0 ? dev->siid_fan : dev->siid;
        cJSON *fv = NULL;
        if (get_siid_piid_json(dev, fan_siid, dev->piid_fan, &fv) == ESP_OK && fv) {
            int fan = json_number_as_int(fv);
            cJSON_DeleteItemFromObject(props, "fan_level");
            cJSON_AddNumberToObject(props, "fan_level", fan);
            cJSON_DeleteItemFromObject(props, "fan_label");
            const char *lab = climate_fan_label(fan);
            if (lab) {
                cJSON_AddStringToObject(props, "fan_label", lab);
            }
            touched = true;
            cJSON_Delete(fv);
        }
    }
    if (dev->piid_temp > 0) {
        int env_siid = dev->siid_env > 0 ? dev->siid_env : dev->siid;
        cJSON *ev = NULL;
        if (get_siid_piid_json(dev, env_siid, dev->piid_temp, &ev) == ESP_OK && ev) {
            if (cJSON_IsNumber(ev)) {
                cJSON_DeleteItemFromObject(props, "temperature");
                cJSON_AddNumberToObject(props, "temperature", ev->valuedouble);
                touched = true;
            } else if (cJSON_IsString(ev) && ev->valuestring) {
                cJSON_DeleteItemFromObject(props, "temperature");
                cJSON_AddNumberToObject(props, "temperature", atof(ev->valuestring));
                touched = true;
            }
            cJSON_Delete(ev);
        }
    }

    if (touched) {
        s_climate_attr_at_ms[idx] = now;
        const char *use_val = (value && value[0]) ? value
                             : (snap.value[0] ? snap.value : "off");
        minihub_point_set(dev->point_id, use_val, props);
    }
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

static void apply_cover_attr_props(miot_device_t *dev, const char *value)
{
    if (!dev || strcmp(dev->type, "cover") != 0) {
        return;
    }
    if (dev->piid_position <= 0 && dev->piid_target_position <= 0) {
        return;
    }
    size_t idx = (size_t)(dev - s_devices);
    if (idx >= MAX_MIOT_DEVICES) {
        return;
    }
    uint32_t now = (uint32_t)esp_log_timestamp();
    if (s_cover_attr_at_ms[idx] != 0 && (now - s_cover_attr_at_ms[idx]) < 30000) {
        return;
    }

    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(dev->point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }

    bool touched = false;
    const char *pos_src = (value && value[0]) ? value
                        : (snap.value[0] ? snap.value : NULL);
    if (pos_src) {
        int pos = atoi(pos_src);
        if (pos < 0) {
            pos = 0;
        }
        if (pos > 100) {
            pos = 100;
        }
        cJSON_DeleteItemFromObject(props, "position");
        cJSON_AddNumberToObject(props, "position", pos);
        touched = true;
    }
    if (dev->piid_target_position > 0) {
        cJSON *tv = NULL;
        if (get_piid_json(dev, dev->piid_target_position, &tv) == ESP_OK && tv) {
            int tpos = json_number_as_int(tv);
            if (tpos < 0) {
                tpos = 0;
            }
            if (tpos > 100) {
                tpos = 100;
            }
            cJSON_DeleteItemFromObject(props, "target_position");
            cJSON_AddNumberToObject(props, "target_position", tpos);
            touched = true;
            cJSON_Delete(tv);
        }
    }

    if (touched) {
        s_cover_attr_at_ms[idx] = now;
        const char *use_val = (value && value[0]) ? value
                             : (snap.value[0] ? snap.value : "0");
        minihub_point_set(dev->point_id, use_val, props);
    }
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

static bool meter_merge_number(cJSON *props, const char *key, cJSON *jv)
{
    if (!props || !key || !jv) {
        return false;
    }
    if (cJSON_IsNumber(jv)) {
        cJSON_DeleteItemFromObject(props, key);
        cJSON_AddNumberToObject(props, key, jv->valuedouble);
        return true;
    }
    if (cJSON_IsString(jv) && jv->valuestring) {
        cJSON_DeleteItemFromObject(props, key);
        cJSON_AddNumberToObject(props, key, atof(jv->valuestring));
        return true;
    }
    return false;
}

/** Poll voltage/current/power/energy/temp for metering switches (breakers). */
static void apply_meter_attr_props(miot_device_t *dev, const char *value)
{
    if (!dev || strcmp(dev->type, "switch") != 0) {
        return;
    }
    if (dev->piid_voltage <= 0 && dev->piid_current <= 0 &&
        dev->piid_power <= 0 && dev->piid_energy <= 0 &&
        dev->piid_temp <= 0 && dev->piid_fault <= 0) {
        return;
    }
    size_t idx = (size_t)(dev - s_devices);
    if (idx >= MAX_MIOT_DEVICES) {
        return;
    }
    uint32_t now = (uint32_t)esp_log_timestamp();
    if (s_meter_attr_at_ms[idx] != 0 && (now - s_meter_attr_at_ms[idx]) < 30000) {
        return;
    }

    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(dev->point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }

    bool touched = false;
    if (dev->no_control) {
        cJSON_DeleteItemFromObject(props, "no_control");
        cJSON_AddBoolToObject(props, "no_control", true);
        touched = true;
    }

    int power_siid = dev->siid_power > 0 ? dev->siid_power : dev->siid;
    if (dev->piid_voltage > 0) {
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, power_siid, dev->piid_voltage, &jv) == ESP_OK && jv) {
            touched |= meter_merge_number(props, "voltage", jv);
            cJSON_Delete(jv);
        }
    }
    if (dev->piid_current > 0) {
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, power_siid, dev->piid_current, &jv) == ESP_OK && jv) {
            touched |= meter_merge_number(props, "current", jv);
            cJSON_Delete(jv);
        }
    }
    if (dev->piid_power > 0) {
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, power_siid, dev->piid_power, &jv) == ESP_OK && jv) {
            touched |= meter_merge_number(props, "power", jv);
            cJSON_Delete(jv);
        }
    }
    if (dev->piid_energy > 0) {
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, power_siid, dev->piid_energy, &jv) == ESP_OK && jv) {
            touched |= meter_merge_number(props, "energy", jv);
            cJSON_Delete(jv);
        }
    }
    if (dev->piid_temp > 0) {
        int env_siid = dev->siid_env > 0 ? dev->siid_env : dev->siid;
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, env_siid, dev->piid_temp, &jv) == ESP_OK && jv) {
            touched |= meter_merge_number(props, "temperature", jv);
            cJSON_Delete(jv);
        }
    }
    if (dev->piid_fault > 0) {
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, dev->siid, dev->piid_fault, &jv) == ESP_OK && jv) {
            int fault = json_number_as_int(jv);
            cJSON_DeleteItemFromObject(props, "fault");
            cJSON_AddNumberToObject(props, "fault", fault);
            touched = true;
            cJSON_Delete(jv);
        }
    }

    if (touched) {
        s_meter_attr_at_ms[idx] = now;
        const char *use_val = (value && value[0]) ? value
                             : (snap.value[0] ? snap.value : "off");
        minihub_point_set(dev->point_id, use_val, props);
    }
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

/** Speaker volume, magnet/motion illum, battery, camera stream codecs. */
static void apply_misc_attr_props(miot_device_t *dev, const char *value)
{
    if (!dev) {
        return;
    }
    const bool want_vol = (strcmp(dev->type, "speaker") == 0);
    const bool want_illum = (strcmp(dev->type, "binary_sensor") == 0 &&
                             dev->piid_illum > 0);
    const bool want_batt = (dev->piid_battery > 0 &&
                            (strcmp(dev->type, "button") == 0 ||
                             strcmp(dev->type, "binary_sensor") == 0));
    const bool want_stream = (strcmp(dev->type, "camera") == 0 &&
                              (dev->piid_video_codec > 0 ||
                               dev->piid_audio_codec > 0));
    if (!want_vol && !want_illum && !want_batt && !want_stream) {
        return;
    }
    size_t idx = (size_t)(dev - s_devices);
    if (idx >= MAX_MIOT_DEVICES) {
        return;
    }
    uint32_t now = (uint32_t)esp_log_timestamp();
    if (s_misc_attr_at_ms[idx] != 0 && (now - s_misc_attr_at_ms[idx]) < 15000) {
        return;
    }

    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(dev->point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }

    bool touched = false;
    if (want_vol) {
        int vol = -1;
        if (value && value[0]) {
            const char *p = value;
            while (*p && (*p < '0' || *p > '9')) {
                p++;
            }
            if (*p) {
                vol = atoi(p);
            }
        }
        if (vol < 0) {
            const cJSON *vj = cJSON_GetObjectItem(props, "volume");
            if (cJSON_IsNumber(vj)) {
                vol = vj->valueint;
            }
        }
        if (vol >= 0) {
            if (vol < 5) {
                vol = 5;
            }
            if (vol > 100) {
                vol = 100;
            }
            cJSON_DeleteItemFromObject(props, "volume");
            cJSON_AddNumberToObject(props, "volume", vol);
            touched = true;
        }
    }
    if (want_illum) {
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, dev->siid, dev->piid_illum, &jv) == ESP_OK && jv) {
            double illf = cJSON_IsNumber(jv) ? jv->valuedouble : (double)json_number_as_int(jv);
            char lab[24];
            if (illf > 2.5) {
                /* Motion lux float */
                snprintf(lab, sizeof(lab), "%.0f lx", illf);
                cJSON_DeleteItemFromObject(props, "illumination");
                cJSON_AddNumberToObject(props, "illumination", illf);
            } else {
                int ill = (int)illf;
                const char *w = (ill <= 1) ? "弱光" : "强光";
                strncpy(lab, w, sizeof(lab) - 1);
                lab[sizeof(lab) - 1] = '\0';
                cJSON_DeleteItemFromObject(props, "illumination");
                cJSON_AddNumberToObject(props, "illumination", ill);
            }
            cJSON_DeleteItemFromObject(props, "illumination_label");
            cJSON_AddStringToObject(props, "illumination_label", lab);
            touched = true;
            cJSON_Delete(jv);
        }
    }
    if (want_batt) {
        int bsiid = dev->siid_battery > 0 ? dev->siid_battery : 3;
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, bsiid, dev->piid_battery, &jv) == ESP_OK && jv) {
            int bat = json_number_as_int(jv);
            if (bat < 0) {
                bat = 0;
            }
            if (bat > 100) {
                bat = 100;
            }
            cJSON_DeleteItemFromObject(props, "battery");
            cJSON_AddNumberToObject(props, "battery", bat);
            touched = true;
            cJSON_Delete(jv);
        }
    }
    if (want_stream) {
        int ssiid = dev->siid_stream > 0 ? dev->siid_stream : 9;
        if (dev->piid_video_codec > 0) {
            cJSON *jv = NULL;
            if (get_siid_piid_json(dev, ssiid, dev->piid_video_codec, &jv) == ESP_OK && jv) {
                int vc = json_number_as_int(jv);
                cJSON_DeleteItemFromObject(props, "stream_video_codec");
                cJSON_AddNumberToObject(props, "stream_video_codec", vc);
                touched = true;
                cJSON_Delete(jv);
            }
        }
        if (dev->piid_audio_codec > 0) {
            cJSON *jv = NULL;
            if (get_siid_piid_json(dev, ssiid, dev->piid_audio_codec, &jv) == ESP_OK && jv) {
                int ac = json_number_as_int(jv);
                cJSON_DeleteItemFromObject(props, "stream_audio_codec");
                cJSON_AddNumberToObject(props, "stream_audio_codec", ac);
                touched = true;
                cJSON_Delete(jv);
            }
        }
    }

    s_misc_attr_at_ms[idx] = now;
    if (touched) {
        const char *use_val = (value && value[0]) ? value
                             : (snap.value[0] ? snap.value : "unknown");
        minihub_point_set(dev->point_id, use_val, props);
    }
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

/** Poll Spec switch extras into props.<key> (≤12; no child points). Volume stays on existing path. */
static void apply_extra_attr_props(miot_device_t *dev, const char *value)
{
    if (!dev || dev->extras_n == 0) {
        return;
    }
    size_t idx = (size_t)(dev - s_devices);
    if (idx >= MAX_MIOT_DEVICES) {
        return;
    }
    uint32_t now = (uint32_t)esp_log_timestamp();
    if (s_extra_attr_at_ms[idx] != 0 && (now - s_extra_attr_at_ms[idx]) < 30000) {
        return;
    }

    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(dev->point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }

    bool touched = false;
    if (!cJSON_GetObjectItem(props, "extras")) {
        cJSON *arr = device_extras_to_json(dev);
        if (arr) {
            cJSON_AddItemToObject(props, "extras", arr);
            touched = true;
        }
    }

    size_t n = dev->extras_n;
    if (n > MIOT_SPEC_EXTRAS_MAX) {
        n = MIOT_SPEC_EXTRAS_MAX;
    }
    bool any_switch = false;
    for (size_t i = 0; i < n; i++) {
        const miot_spec_extra_t *e = &dev->extras[i];
        if (strcmp(e->kind, "switch") == 0 && e->siid > 0 && e->piid > 0) {
            any_switch = true;
            break;
        }
    }
    if (!any_switch) {
        /* Actions / volume extras: catalog only. Do not create child points. */
        if (touched) {
            s_extra_attr_at_ms[idx] = now;
            const char *use_val = (value && value[0]) ? value
                                 : (snap.value[0] ? snap.value : "off");
            minihub_point_set(dev->point_id, use_val, props);
        } else {
            s_extra_attr_at_ms[idx] = now;
        }
        cJSON_Delete(props);
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const miot_spec_extra_t *e = &dev->extras[i];
        if (strcmp(e->kind, "switch") != 0 || !e->key[0]) {
            continue;
        }
        if (strcmp(e->key, "volume") == 0) {
            continue;
        }
        if (e->siid <= 0 || e->piid <= 0) {
            continue;
        }
        cJSON *jv = NULL;
        if (get_siid_piid_json(dev, e->siid, e->piid, &jv) != ESP_OK || !jv) {
            continue;
        }
        bool on = false;
        if (json_to_bool(jv, &on)) {
            cJSON_DeleteItemFromObject(props, e->key);
            cJSON_AddBoolToObject(props, e->key, on);
            touched = true;
        }
        cJSON_Delete(jv);
    }

    if (touched) {
        s_extra_attr_at_ms[idx] = now;
        const char *use_val = (value && value[0]) ? value
                             : (snap.value[0] ? snap.value : "off");
        minihub_point_set(dev->point_id, use_val, props);
    }
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

/** Write property on arbitrary siid/piid (LAN preferred, cloud fallback). */
static esp_err_t set_prop_siid_piid(miot_device_t *dev, int siid, int piid, cJSON *val,
                                    bool *out_lan_ok)
{
    if (!dev || siid <= 0 || piid <= 0 || !val) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    bool lan_attempted = false;
    bool lan_ok = false;

    if (lan_path_eligible(dev)) {
        lan_attempted = true;
        if (dev->mode == MIOT_DEV_AUTO && dev->lan_ready && dev->ip[0]) {
            err = miio_lan_set_property(dev->ip, dev->token, &dev->miio_id,
                                        siid, piid, val);
        } else {
            err = lan_ensure_ready(dev);
            if (err == ESP_OK) {
                err = miio_lan_set_property(dev->ip, dev->token, &dev->miio_id,
                                            siid, piid, val);
            }
        }
        if (err == ESP_OK) {
            lan_ok = true;
        } else if (dev->mode == MIOT_DEV_AUTO) {
            mark_lan_probe(dev, false);
        }
    } else if (dev->mode == MIOT_DEV_LAN && device_has_lan(dev)) {
        lan_attempted = true;
        err = lan_ensure_ready(dev);
        if (err == ESP_OK) {
            err = miio_lan_set_property(dev->ip, dev->token, &dev->miio_id,
                                        siid, piid, val);
            if (err == ESP_OK) {
                lan_ok = true;
            }
        }
    }

    if (!lan_ok && dev->mode != MIOT_DEV_LAN && cloud_path_available(dev)) {
        if (!minihub_net_has_ip()) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = minihub_miot_auth_prop_set(dev->did, siid, piid, val);
        }
    } else if (!lan_attempted) {
        err = ESP_ERR_INVALID_STATE;
    }

    if (out_lan_ok) {
        *out_lan_ok = lan_ok;
    }
    return err;
}

static esp_err_t set_bool_prop(miot_device_t *dev, bool on)
{
    if (!dev || dev->piid_on <= 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool prop_on = on;
    if (dev->invert_on) {
        /* sleep-mode: Hub on ⇒ write false (awake). */
        prop_on = !on;
    }
    cJSON *val = cJSON_CreateBool(prop_on);
    if (!val) {
        return ESP_ERR_NO_MEM;
    }
    bool lan_ok = false;
    esp_err_t err = set_prop_siid_piid(dev, dev->siid, dev->piid_on, val, &lan_ok);
    if (err == ESP_OK) {
        log_control_path(dev, on ? "on" : "off", lan_ok);
    }
    cJSON_Delete(val);
    return err;
}

static esp_err_t set_number_prop_siid(miot_device_t *dev, int siid, int piid, int number)
{
    cJSON *val = cJSON_CreateNumber(number);
    if (!val) {
        return ESP_ERR_NO_MEM;
    }
    bool lan_ok = false;
    esp_err_t err = set_prop_siid_piid(dev, siid, piid, val, &lan_ok);
    if (err == ESP_OK) {
        char action[32];
        snprintf(action, sizeof(action), "s%d.p%d=%d", siid, piid, number);
        log_control_path(dev, action, lan_ok);
    }
    cJSON_Delete(val);
    return err;
}

static esp_err_t set_number_prop(miot_device_t *dev, int piid, int number)
{
    return set_number_prop_siid(dev, dev ? dev->siid : 0, piid, number);
}

static esp_err_t set_double_prop_siid(miot_device_t *dev, int siid, int piid, double number)
{
    cJSON *val = cJSON_CreateNumber(number);
    if (!val) {
        return ESP_ERR_NO_MEM;
    }
    bool lan_ok = false;
    esp_err_t err = set_prop_siid_piid(dev, siid, piid, val, &lan_ok);
    if (err == ESP_OK) {
        char action[40];
        snprintf(action, sizeof(action), "s%d.p%d=%.1f", siid, piid, number);
        log_control_path(dev, action, lan_ok);
    }
    cJSON_Delete(val);
    return err;
}

static esp_err_t set_motor(miot_device_t *dev, uint8_t motor_val)
{
    if (!dev || dev->piid_motor <= 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    int siid = dev->siid_motor > 0 ? dev->siid_motor : dev->siid;
    return set_number_prop_siid(dev, siid, dev->piid_motor, (int)motor_val);
}

static int parse_climate_mode_param(const cJSON *v)
{
    if (cJSON_IsNumber(v)) {
        return v->valueint;
    }
    if (!cJSON_IsString(v) || !v->valuestring) {
        return -1;
    }
    const char *s = v->valuestring;
    if (strcmp(s, "cool") == 0 || strcmp(s, "制冷") == 0) {
        return 1;
    }
    if (strcmp(s, "dry") == 0 || strcmp(s, "dehumidify") == 0 ||
        strcmp(s, "除湿") == 0) {
        return 2;
    }
    if (strcmp(s, "fan") == 0 || strcmp(s, "送风") == 0) {
        return 4;
    }
    if (strcmp(s, "heat") == 0 || strcmp(s, "制热") == 0) {
        return 8;
    }
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (end && end != s && *end == '\0') {
        return (int)n;
    }
    return -1;
}

static int parse_climate_fan_param(const cJSON *v)
{
    if (cJSON_IsNumber(v)) {
        return v->valueint;
    }
    if (!cJSON_IsString(v) || !v->valuestring) {
        return -1;
    }
    const char *s = v->valuestring;
    if (strcmp(s, "auto") == 0 || strcmp(s, "自动") == 0) {
        return 0;
    }
    if (strcmp(s, "low") == 0 || strcmp(s, "低") == 0) {
        return 1;
    }
    if (strcmp(s, "medium") == 0 || strcmp(s, "mid") == 0 ||
        strcmp(s, "中") == 0) {
        return 2;
    }
    if (strcmp(s, "high") == 0 || strcmp(s, "高") == 0) {
        return 4;
    }
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (end && end != s && *end == '\0') {
        return (int)n;
    }
    return -1;
}

static void optimistic_merge_prop_number(const char *point_id, const char *key,
                                         double number)
{
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }
    cJSON_DeleteItemFromObject(props, key);
    cJSON_AddNumberToObject(props, key, number);
    minihub_point_set(point_id, snap.value[0] ? snap.value : "", props);
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

static void optimistic_merge_prop_string(const char *point_id, const char *key,
                                         const char *str)
{
    if (!str) {
        return;
    }
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }
    cJSON_DeleteItemFromObject(props, key);
    cJSON_AddStringToObject(props, key, str);
    minihub_point_set(point_id, snap.value[0] ? snap.value : "", props);
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

static void optimistic_merge_prop_bool(const char *point_id, const char *key, bool v)
{
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(point_id, &snap) != ESP_OK) {
        return;
    }
    cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (!props) {
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return;
    }
    cJSON_DeleteItemFromObject(props, key);
    cJSON_AddBoolToObject(props, key, v);
    minihub_point_set(point_id, snap.value[0] ? snap.value : "", props);
    cJSON_Delete(props);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
}

static void optimistic_merge_from_cjson(const char *point_id, const char *key, const cJSON *val)
{
    bool b = false;
    if (json_to_bool(val, &b) && (cJSON_IsBool(val) ||
                                  (cJSON_IsNumber(val) &&
                                   (val->valuedouble == 0.0 || val->valuedouble == 1.0)))) {
        optimistic_merge_prop_bool(point_id, key, b);
        return;
    }
    if (cJSON_IsNumber(val)) {
        optimistic_merge_prop_number(point_id, key, val->valuedouble);
        return;
    }
    if (cJSON_IsString(val) && val->valuestring) {
        optimistic_merge_prop_string(point_id, key, val->valuestring);
    }
}

static miot_dev_mode_t parse_mode_str(const char *s)
{
    if (!s) {
        return MIOT_DEV_AUTO;
    }
    if (strcmp(s, "lan") == 0) {
        return MIOT_DEV_LAN;
    }
    if (strcmp(s, "cloud") == 0) {
        return MIOT_DEV_CLOUD;
    }
    return MIOT_DEV_AUTO;
}

static esp_err_t parse_devices(const cJSON *cfg)
{
    const cJSON *devices = cJSON_GetObjectItem(cfg, "devices");
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }

    const cJSON *mode_j = cJSON_GetObjectItem(cfg, "mode");
    if (cJSON_IsString(mode_j)) {
        s_default_mode = parse_mode_str(mode_j->valuestring);
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        if (s_device_count >= MAX_MIOT_DEVICES) {
            break;
        }
        miot_device_t *dev = &s_devices[s_device_count];
        memset(dev, 0, sizeof(*dev));
        dev->siid = 2;
        dev->piid_on = 1;
        dev->miio_id = 0xFFFFFFFFU;
        dev->mode = s_default_mode;

        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *did = cJSON_GetObjectItem(item, "did");
        const cJSON *ip = cJSON_GetObjectItem(item, "ip");
        const cJSON *token = cJSON_GetObjectItem(item, "token");
        const cJSON *model = cJSON_GetObjectItem(item, "model");
        const cJSON *mac = cJSON_GetObjectItem(item, "mac");
        const cJSON *mode = cJSON_GetObjectItem(item, "mode");
        const cJSON *siid = cJSON_GetObjectItem(item, "siid");
        const cJSON *piid_on = cJSON_GetObjectItem(item, "piid_on");
        const cJSON *invert_contact = cJSON_GetObjectItem(item, "invert_contact");

        if (!cJSON_IsString(point_id)) {
            continue;
        }
        if (!cJSON_IsString(did) && !cJSON_IsString(token)) {
            continue;
        }

        strncpy(dev->point_id, point_id->valuestring, sizeof(dev->point_id) - 1);
        if (cJSON_IsString(type)) {
            strncpy(dev->type, type->valuestring, sizeof(dev->type) - 1);
        } else {
            strncpy(dev->type, "light", sizeof(dev->type) - 1);
        }
        if (cJSON_IsString(did)) {
            strncpy(dev->did, did->valuestring, sizeof(dev->did) - 1);
        }
        if (cJSON_IsString(ip)) {
            strncpy(dev->ip, ip->valuestring, sizeof(dev->ip) - 1);
        }
        if (cJSON_IsString(model)) {
            strncpy(dev->model, model->valuestring, sizeof(dev->model) - 1);
        }
        if (cJSON_IsString(mac)) {
            strncpy(dev->mac, mac->valuestring, sizeof(dev->mac) - 1);
        }
        if (cJSON_IsString(token) &&
            miio_token_from_hex(token->valuestring, dev->token) == ESP_OK) {
            dev->has_token = true;
        }
        if (cJSON_IsString(mode)) {
            dev->mode = parse_mode_str(mode->valuestring);
        }
        if (cJSON_IsNumber(siid)) {
            dev->siid = siid->valueint;
        } else if (cJSON_IsString(siid)) {
            dev->siid = atoi(siid->valuestring);
        }
        if (cJSON_IsNumber(piid_on)) {
            dev->piid_on = piid_on->valueint;
        } else if (cJSON_IsString(piid_on)) {
            dev->piid_on = atoi(piid_on->valuestring);
        }
        const cJSON *piid_bright = cJSON_GetObjectItem(item, "piid_bright");
        if (cJSON_IsNumber(piid_bright)) {
            dev->piid_bright = piid_bright->valueint;
        } else if (cJSON_IsString(piid_bright)) {
            dev->piid_bright = atoi(piid_bright->valuestring);
        }
        const cJSON *bright_max = cJSON_GetObjectItem(item, "bright_max");
        if (cJSON_IsNumber(bright_max)) {
            dev->bright_max = bright_max->valueint;
        } else if (cJSON_IsString(bright_max)) {
            dev->bright_max = atoi(bright_max->valuestring);
        }
        /* Yeelight Spec often uses 1..65535 even when slim lacks bright_max. */
        if (dev->bright_max <= 0 && dev->model[0] && strstr(dev->model, "yeelink.light.")) {
            dev->bright_max = 65535;
        }
        const cJSON *piid_ct = cJSON_GetObjectItem(item, "piid_ct");
        if (cJSON_IsNumber(piid_ct)) {
            dev->piid_ct = piid_ct->valueint;
        }
        const cJSON *piid_color = cJSON_GetObjectItem(item, "piid_color");
        if (cJSON_IsNumber(piid_color)) {
            dev->piid_color = piid_color->valueint;
        }
        const cJSON *piid_mode = cJSON_GetObjectItem(item, "piid_mode");
        if (cJSON_IsNumber(piid_mode)) {
            dev->piid_mode = piid_mode->valueint;
        } else if (cJSON_IsString(piid_mode)) {
            dev->piid_mode = atoi(piid_mode->valuestring);
        }
        const cJSON *piid_target_temp = cJSON_GetObjectItem(item, "piid_target_temp");
        if (cJSON_IsNumber(piid_target_temp)) {
            dev->piid_target_temp = piid_target_temp->valueint;
        } else if (cJSON_IsString(piid_target_temp)) {
            dev->piid_target_temp = atoi(piid_target_temp->valuestring);
        }
        const cJSON *siid_fan = cJSON_GetObjectItem(item, "siid_fan");
        if (cJSON_IsNumber(siid_fan)) {
            dev->siid_fan = siid_fan->valueint;
        } else if (cJSON_IsString(siid_fan)) {
            dev->siid_fan = atoi(siid_fan->valuestring);
        }
        const cJSON *piid_fan = cJSON_GetObjectItem(item, "piid_fan");
        if (cJSON_IsNumber(piid_fan)) {
            dev->piid_fan = piid_fan->valueint;
        } else if (cJSON_IsString(piid_fan)) {
            dev->piid_fan = atoi(piid_fan->valuestring);
        }
        const cJSON *siid_env = cJSON_GetObjectItem(item, "siid_env");
        if (cJSON_IsNumber(siid_env)) {
            dev->siid_env = siid_env->valueint;
        } else if (cJSON_IsString(siid_env)) {
            dev->siid_env = atoi(siid_env->valuestring);
        }
        const cJSON *piid_temp = cJSON_GetObjectItem(item, "piid_temp");
        if (cJSON_IsNumber(piid_temp)) {
            dev->piid_temp = piid_temp->valueint;
        } else if (cJSON_IsString(piid_temp)) {
            dev->piid_temp = atoi(piid_temp->valuestring);
        }
        const cJSON *piid_position = cJSON_GetObjectItem(item, "piid_position");
        if (cJSON_IsNumber(piid_position)) {
            dev->piid_position = piid_position->valueint;
        } else if (cJSON_IsString(piid_position)) {
            dev->piid_position = atoi(piid_position->valuestring);
        }
        const cJSON *piid_target_position = cJSON_GetObjectItem(item, "piid_target_position");
        if (cJSON_IsNumber(piid_target_position)) {
            dev->piid_target_position = piid_target_position->valueint;
        } else if (cJSON_IsString(piid_target_position)) {
            dev->piid_target_position = atoi(piid_target_position->valuestring);
        }
        const cJSON *siid_motor = cJSON_GetObjectItem(item, "siid_motor");
        if (cJSON_IsNumber(siid_motor)) {
            dev->siid_motor = siid_motor->valueint;
        } else if (cJSON_IsString(siid_motor)) {
            dev->siid_motor = atoi(siid_motor->valuestring);
        }
        const cJSON *piid_motor = cJSON_GetObjectItem(item, "piid_motor");
        if (cJSON_IsNumber(piid_motor)) {
            dev->piid_motor = piid_motor->valueint;
        } else if (cJSON_IsString(piid_motor)) {
            dev->piid_motor = atoi(piid_motor->valuestring);
        }
        const cJSON *motor_open = cJSON_GetObjectItem(item, "motor_open");
        if (cJSON_IsNumber(motor_open)) {
            dev->motor_open = (uint8_t)motor_open->valueint;
        }
        const cJSON *motor_close = cJSON_GetObjectItem(item, "motor_close");
        if (cJSON_IsNumber(motor_close)) {
            dev->motor_close = (uint8_t)motor_close->valueint;
        }
        const cJSON *motor_pause = cJSON_GetObjectItem(item, "motor_pause");
        if (cJSON_IsNumber(motor_pause)) {
            dev->motor_pause = (uint8_t)motor_pause->valueint;
        }
        const cJSON *invert_on = cJSON_GetObjectItem(item, "invert_on");
        dev->invert_on = cJSON_IsTrue(invert_on);
        const cJSON *piid_volume = cJSON_GetObjectItem(item, "piid_volume");
        if (cJSON_IsNumber(piid_volume)) {
            dev->piid_volume = piid_volume->valueint;
        } else if (cJSON_IsString(piid_volume)) {
            dev->piid_volume = atoi(piid_volume->valuestring);
        }
        const cJSON *piid_door = cJSON_GetObjectItem(item, "piid_door");
        if (cJSON_IsNumber(piid_door)) {
            dev->piid_door = piid_door->valueint;
        } else if (cJSON_IsString(piid_door)) {
            dev->piid_door = atoi(piid_door->valuestring);
        }
        const cJSON *piid_illum = cJSON_GetObjectItem(item, "piid_illum");
        if (cJSON_IsNumber(piid_illum)) {
            dev->piid_illum = piid_illum->valueint;
        } else if (cJSON_IsString(piid_illum)) {
            dev->piid_illum = atoi(piid_illum->valuestring);
        }
        const cJSON *piid_no_motion = cJSON_GetObjectItem(item, "piid_no_motion");
        if (cJSON_IsNumber(piid_no_motion)) {
            dev->piid_no_motion = piid_no_motion->valueint;
        } else if (cJSON_IsString(piid_no_motion)) {
            dev->piid_no_motion = atoi(piid_no_motion->valuestring);
        }
        if (dev->piid_no_motion <= 0 && strcmp(dev->type, "binary_sensor") == 0 &&
            dev->model[0] && strstr(dev->model, "motion")) {
            dev->piid_no_motion = 1024;
        }
        const cJSON *siid_battery = cJSON_GetObjectItem(item, "siid_battery");
        if (cJSON_IsNumber(siid_battery)) {
            dev->siid_battery = siid_battery->valueint;
        } else if (cJSON_IsString(siid_battery)) {
            dev->siid_battery = atoi(siid_battery->valuestring);
        }
        const cJSON *piid_battery = cJSON_GetObjectItem(item, "piid_battery");
        if (cJSON_IsNumber(piid_battery)) {
            dev->piid_battery = piid_battery->valueint;
        } else if (cJSON_IsString(piid_battery)) {
            dev->piid_battery = atoi(piid_battery->valuestring);
        }
        const cJSON *siid_stream = cJSON_GetObjectItem(item, "siid_stream");
        if (cJSON_IsNumber(siid_stream)) {
            dev->siid_stream = siid_stream->valueint;
        } else if (cJSON_IsString(siid_stream)) {
            dev->siid_stream = atoi(siid_stream->valuestring);
        }
        const cJSON *piid_video_codec = cJSON_GetObjectItem(item, "piid_video_codec");
        if (cJSON_IsNumber(piid_video_codec)) {
            dev->piid_video_codec = piid_video_codec->valueint;
        } else if (cJSON_IsString(piid_video_codec)) {
            dev->piid_video_codec = atoi(piid_video_codec->valuestring);
        }
        const cJSON *piid_audio_codec = cJSON_GetObjectItem(item, "piid_audio_codec");
        if (cJSON_IsNumber(piid_audio_codec)) {
            dev->piid_audio_codec = piid_audio_codec->valueint;
        } else if (cJSON_IsString(piid_audio_codec)) {
            dev->piid_audio_codec = atoi(piid_audio_codec->valuestring);
        }
        const cJSON *siid_power = cJSON_GetObjectItem(item, "siid_power");
        if (cJSON_IsNumber(siid_power)) {
            dev->siid_power = siid_power->valueint;
        } else if (cJSON_IsString(siid_power)) {
            dev->siid_power = atoi(siid_power->valuestring);
        }
        const cJSON *piid_energy = cJSON_GetObjectItem(item, "piid_energy");
        if (cJSON_IsNumber(piid_energy)) {
            dev->piid_energy = piid_energy->valueint;
        } else if (cJSON_IsString(piid_energy)) {
            dev->piid_energy = atoi(piid_energy->valuestring);
        }
        const cJSON *piid_current = cJSON_GetObjectItem(item, "piid_current");
        if (cJSON_IsNumber(piid_current)) {
            dev->piid_current = piid_current->valueint;
        } else if (cJSON_IsString(piid_current)) {
            dev->piid_current = atoi(piid_current->valuestring);
        }
        const cJSON *piid_power = cJSON_GetObjectItem(item, "piid_power");
        if (cJSON_IsNumber(piid_power)) {
            dev->piid_power = piid_power->valueint;
        } else if (cJSON_IsString(piid_power)) {
            dev->piid_power = atoi(piid_power->valuestring);
        }
        const cJSON *piid_voltage = cJSON_GetObjectItem(item, "piid_voltage");
        if (cJSON_IsNumber(piid_voltage)) {
            dev->piid_voltage = piid_voltage->valueint;
        } else if (cJSON_IsString(piid_voltage)) {
            dev->piid_voltage = atoi(piid_voltage->valuestring);
        }
        const cJSON *piid_fault = cJSON_GetObjectItem(item, "piid_fault");
        if (cJSON_IsNumber(piid_fault)) {
            dev->piid_fault = piid_fault->valueint;
        } else if (cJSON_IsString(piid_fault)) {
            dev->piid_fault = atoi(piid_fault->valuestring);
        }
        const cJSON *no_control = cJSON_GetObjectItem(item, "no_control");
        /* Metering alone does not imply read-only; sync sets no_control only when no piid_on. */
        dev->no_control = cJSON_IsTrue(no_control);
        const cJSON *sb = cJSON_GetObjectItem(item, "supports_brightness");
        const cJSON *sct = cJSON_GetObjectItem(item, "supports_color_temp");
        const cJSON *sc = cJSON_GetObjectItem(item, "supports_color");
        dev->supports_brightness = cJSON_IsTrue(sb) || dev->piid_bright > 0;
        dev->supports_color_temp = cJSON_IsTrue(sct) || dev->piid_ct > 0;
        dev->supports_color = cJSON_IsTrue(sc) || dev->piid_color > 0;
        if (cJSON_IsBool(invert_contact)) {
            dev->invert_contact = cJSON_IsTrue(invert_contact);
        }
        if (strcmp(dev->type, "car") == 0 || device_cloud_only(dev)) {
            /* BLE / camera / TV / speaker / lock / proxy AC: cloud only (no UDP spam). */
            dev->mode = MIOT_DEV_CLOUD;
            dev->has_token = false;
            dev->lan_ready = false;
            dev->ip[0] = '\0';
        }
        if (lww_eligible(dev) &&
            mac_to_xiaomi_point_id(dev->mac, dev->local_point_id,
                                   sizeof(dev->local_point_id))) {
            dev->lww_paired = true;
        }

        const cJSON *disp_name = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(disp_name) && disp_name->valuestring[0]) {
            strncpy(dev->display_name, disp_name->valuestring,
                    sizeof(dev->display_name) - 1);
        }
        const cJSON *room_id = cJSON_GetObjectItem(item, "room_id");
        if (cJSON_IsString(room_id) && room_id->valuestring[0]) {
            strncpy(dev->room_id, room_id->valuestring, sizeof(dev->room_id) - 1);
        } else if (cJSON_IsNumber(room_id)) {
            snprintf(dev->room_id, sizeof(dev->room_id), "%d", room_id->valueint);
        }
        const cJSON *room_name = cJSON_GetObjectItem(item, "room_name");
        if (cJSON_IsString(room_name) && room_name->valuestring[0]) {
            strncpy(dev->room_name, room_name->valuestring, sizeof(dev->room_name) - 1);
        }
        const cJSON *home_name = cJSON_GetObjectItem(item, "home_name");
        if (cJSON_IsString(home_name) && home_name->valuestring[0]) {
            strncpy(dev->home_name, home_name->valuestring, sizeof(dev->home_name) - 1);
        }

        const cJSON *extras = cJSON_GetObjectItem(item, "extras");
        if (cJSON_IsArray(extras)) {
            const cJSON *ent = NULL;
            cJSON_ArrayForEach(ent, extras) {
                if (dev->extras_n >= MIOT_SPEC_EXTRAS_MAX) {
                    break;
                }
                if (!cJSON_IsObject(ent)) {
                    continue;
                }
                const cJSON *ekey = cJSON_GetObjectItem(ent, "key");
                const cJSON *ekind = cJSON_GetObjectItem(ent, "kind");
                if (!cJSON_IsString(ekey) || !ekey->valuestring || !ekey->valuestring[0] ||
                    !cJSON_IsString(ekind) || !ekind->valuestring || !ekind->valuestring[0]) {
                    continue;
                }
                if (strcmp(ekey->valuestring, "motion_tracking") == 0) {
                    continue;
                }
                miot_spec_extra_t *e = &dev->extras[dev->extras_n];
                memset(e, 0, sizeof(*e));
                strncpy(e->key, ekey->valuestring, sizeof(e->key) - 1);
                strncpy(e->kind, ekind->valuestring, sizeof(e->kind) - 1);
                e->siid = json_number_as_int(cJSON_GetObjectItem(ent, "siid"));
                e->piid = json_number_as_int(cJSON_GetObjectItem(ent, "piid"));
                e->aiid = json_number_as_int(cJSON_GetObjectItem(ent, "aiid"));
                dev->extras_n++;
            }
        }

        const char *path = (dev->mode == MIOT_DEV_CLOUD) ? "cloud"
                         : (dev->mode == MIOT_DEV_LAN) ? "lan"
                         : (device_has_lan(dev) ? "auto" : "cloud");
        ESP_LOGI(TAG, "device %s path=%s ip=%s did=%s model=%s extras=%u%s",
                 dev->point_id, path,
                 dev->ip[0] ? dev->ip : "(discover)",
                 dev->did[0] ? dev->did : "-",
                 dev->model[0] ? dev->model : "-",
                 (unsigned)dev->extras_n,
                 dev->lww_paired ? " lww" : "");
        s_device_count++;
    }
    return ESP_OK;
}

static esp_err_t miot_adapter_init(const cJSON *config)
{
    esp_err_t err = minihub_adapter_check_core_api(1, 1);
    if (err != ESP_OK) {
        return err;
    }

    /* Static link (claw_minihub bringup): module_entry() is not called, so wire host
     * config/RPC hooks here — required for cloud sync (adapters.json) and NVS session load. */
    minihub_module_host_api_t host_api = {
        .struct_size = sizeof(host_api),
    };
    err = minihub_module_host_api_fill(&host_api);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "module host api fill failed: %s", esp_err_to_name(err));
    } else {
        minihub_miot_module_set_host(&host_api); /* copied into static storage */
    }
    err = minihub_miot_auth_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "miot auth init: %s", esp_err_to_name(err));
    }

    s_device_count = 0;
    s_default_mode = MIOT_DEV_AUTO;
    if (config) {
        parse_devices(config);
    }
    /* RPC is registered for both Host-static bringup and Store .mhub load. */
    (void)minihub_module_register_rpc("miot", miot_module_rpc);
    /* Prefer ui.json from installed Store module path. */
    static const char *const ui_paths[] = {
        "/store/modules/miot/ui.json",
        "/fatfs/minihub/modules/miot/ui.json",
        NULL,
    };
    for (size_t i = 0; ui_paths[i]; i++) {
        esp_err_t ui_err = minihub_module_set_ui_file("miot", ui_paths[i]);
        if (ui_err == ESP_OK) {
            ESP_LOGI(TAG, "miot ui from %s", ui_paths[i]);
            break;
        }
    }
    return ESP_OK;
}

static const char *initial_value_for_type(const char *type)
{
    if (strcmp(type, "button") == 0) {
        return "idle";
    }
    if (strcmp(type, "binary_sensor") == 0) {
        return "clear";
    }
    if (strcmp(type, "car") == 0 || strcmp(type, "smoke") == 0 ||
        strcmp(type, "lock") == 0) {
        return "unknown";
    }
    if (strcmp(type, "cover") == 0 || strcmp(type, "speaker") == 0) {
        return "0";
    }
    return "off";
}

static void register_miot_point(const miot_device_t *dev, bool online, const char *value)
{
    minihub_point_t point = {0};
    strncpy(point.point_id, dev->point_id, sizeof(point.point_id) - 1);
    strncpy(point.type, dev->type, sizeof(point.type) - 1);
    strncpy(point.adapter, "miot", sizeof(point.adapter) - 1);
    strncpy(point.value, value ? value : "off", sizeof(point.value) - 1);
    point.online = online;
    cJSON *props = cJSON_CreateObject();
    if (props) {
        if (dev->display_name[0]) {
            cJSON_AddStringToObject(props, "name", dev->display_name);
        }
        if (dev->model[0]) {
            cJSON_AddStringToObject(props, "model", dev->model);
            /* Brand = first segment of MIoT model (yeelink.light.xxx → yeelink). */
            char brand[40];
            size_t n = 0;
            while (dev->model[n] && dev->model[n] != '.' && n + 1 < sizeof(brand)) {
                brand[n] = dev->model[n];
                n++;
            }
            brand[n] = '\0';
            if (n > 0) {
                cJSON_AddStringToObject(props, "manufacturer", brand);
            }
        }
        if (dev->room_name[0]) {
            cJSON_AddStringToObject(props, "room_name", dev->room_name);
        }
        if (dev->room_id[0]) {
            cJSON_AddStringToObject(props, "room_id", dev->room_id);
        }
        if (dev->home_name[0]) {
            cJSON_AddStringToObject(props, "home_name", dev->home_name);
        }
        if (strcmp(dev->type, "light") == 0) {
            if (dev->supports_brightness) {
                cJSON_AddNumberToObject(props, "brightness", 100);
            }
            if (dev->supports_color_temp) {
                cJSON_AddNumberToObject(props, "color_temp", 2700);
            }
            cJSON_AddBoolToObject(props, "supports_brightness", dev->supports_brightness);
            cJSON_AddBoolToObject(props, "supports_color_temp", dev->supports_color_temp);
            cJSON_AddBoolToObject(props, "supports_color", dev->supports_color);
            if (dev->supports_color) {
                cJSON_AddStringToObject(props, "color", "#ffffff");
                cJSON_AddStringToObject(props, "color_mode", "rgb");
            } else if (dev->supports_color_temp) {
                cJSON_AddStringToObject(props, "color_mode", "ct");
            }
        }
        if (dev->no_control) {
            cJSON_AddBoolToObject(props, "no_control", true);
        }
        if (dev->extras_n > 0) {
            cJSON *arr = device_extras_to_json(dev);
            if (arr) {
                cJSON_AddItemToObject(props, "extras", arr);
            }
        }
        point.props = props;
    }
    minihub_point_register(&point);
    if (point.props) {
        cJSON_Delete(point.props);
    }
}

static esp_err_t miot_adapter_start(void)
{
    if (s_device_count == 0) {
        ESP_LOGW(TAG, "no miot devices configured; sync cloud or add token in adapters.json");
        return ESP_OK;
    }

    /* Do not block hub boot / HTTP on LAN discover+bind (can take minutes when
     * many miIO hosts are visible but the token does not match). Register
     * points immediately; poll / command paths call lan_ensure_ready later. */
    size_t lan_n = 0, cloud_n = 0;
    for (size_t i = 0; i < s_device_count; i++) {
        miot_device_t *dev = &s_devices[i];
        if (device_can_try_lan(dev)) {
            lan_n++;
        } else if (cloud_path_available(dev)) {
            cloud_n++;
        }
        register_miot_point(dev, device_online(dev),
                            initial_value_for_type(dev->type));
    }
    s_poll_idx = 0;
    ESP_LOGI(TAG, "started %u miot devices (lan=%u cloud=%u; lan bind deferred)",
             (unsigned)s_device_count, (unsigned)lan_n, (unsigned)cloud_n);
    return ESP_OK;
}

static esp_err_t miot_adapter_poll_one(miot_device_t *dev)
{
    maybe_reprobe_lan(dev);

    if (!device_online(dev)) {
        return ESP_OK;
    }

    /* Cloud Spec poll before Wi-Fi has an IP only produces getaddrinfo floods. */
    const bool net_up = minihub_net_has_ip();
    if (!net_up && !lan_path_eligible(dev)) {
        return ESP_OK;
    }

    char val[32] = {0};
    if (dev->siid <= 0) {
        if (dev->lww_paired) {
            reconcile_lww(dev);
        } else {
            minihub_point_set(dev->point_id, "unknown", NULL);
        }
        minihub_point_set_online(dev->point_id, cloud_path_available(dev));
        return ESP_OK;
    }
    esp_err_t perr = get_prop(dev, val, sizeof(val));
    if (perr == ESP_OK) {
        dev->prop_fail_streak = 0;
        if (dev->lww_paired) {
            /* Stale cloud vs newer local must not mint a winning cloud event. */
            if (contact_is_known(dev->local_value) &&
                dev->local_ms > dev->cloud_ms &&
                strcmp(val, dev->local_value) != 0) {
                ESP_LOGD(TAG, "lww ignore stale cloud poll %s=%s (local=%s)",
                         dev->point_id, val, dev->local_value);
            } else {
                accept_side_change(dev->cloud_value, dev->cloud_prev,
                                   sizeof(dev->cloud_value), &dev->cloud_ms,
                                   val, now_ms());
            }
            minihub_point_set_online(dev->point_id, true);
            reconcile_lww(dev);
            /* Illumination etc. still come from cloud Spec props. */
            apply_misc_attr_props(dev, val);
        } else {
            minihub_point_set(dev->point_id, val, NULL);
            minihub_point_set_online(dev->point_id, true);
            apply_light_attr_props(dev, val);
            apply_climate_attr_props(dev, val);
            apply_cover_attr_props(dev, val);
            apply_meter_attr_props(dev, val);
            apply_extra_attr_props(dev, val);
            apply_misc_attr_props(dev, val);
        }
    } else if (!net_up) {
        /* Keep last known online/value while waiting for uplink. */
    } else {
        if (dev->prop_fail_streak < 255) {
            dev->prop_fail_streak++;
        }
        if (dev->prop_fail_streak >= 3) {
            minihub_point_set_online(dev->point_id, false);
        }
        if (dev->lww_paired) {
            reconcile_lww(dev);
        }
    }
    return ESP_OK;
}

static esp_err_t miot_adapter_poll(void)
{
    if (s_device_count == 0) {
        return ESP_OK;
    }

    /* Host poll task wakes ~5s; with 32 devices one-per-tick left ACs stale for
     * minutes after reload. Advance several devices each tick. */
    const size_t per_tick = s_device_count < 8 ? s_device_count : 8;
    for (size_t n = 0; n < per_tick; n++) {
        miot_device_t *dev = &s_devices[s_poll_idx % s_device_count];
        s_poll_idx++;
        (void)miot_adapter_poll_one(dev);
    }
    return ESP_OK;
}

/** Cloud expects plain in values; Spec action `in` lists property iids only. */
static cJSON *build_tts_in(const miot_spec_extra_t *ex, const char *text, const cJSON *silent_param)
{
    if (!text) {
        return NULL;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 8192) {
        ESP_LOGW(TAG, "skip TTS buffer alloc (internal heap < 8192)");
        return NULL;
    }
    cJSON *in = cJSON_CreateArray();
    if (!in) {
        return NULL;
    }
    cJSON *t = cJSON_CreateString(text);
    if (!t) {
        cJSON_Delete(in);
        return NULL;
    }
    cJSON_AddItemToArray(in, t);

    bool add_silent = false;
    bool silent_val = true;
    if (cJSON_IsBool(silent_param)) {
        add_silent = true;
        silent_val = cJSON_IsTrue(silent_param);
    } else if (ex && strcmp(ex->key, "execute_text") == 0) {
        add_silent = true;
        silent_val = true;
    }
    if (add_silent) {
        cJSON *s = cJSON_CreateBool(silent_val);
        if (s) {
            cJSON_AddItemToArray(in, s);
        }
    }
    return in;
}

static esp_err_t handle_set_prop(miot_device_t *dev, const minihub_command_t *cmd)
{
    if (!cmd->params) {
        return ESP_ERR_INVALID_ARG;
    }
    int siid = json_number_as_int(cJSON_GetObjectItem(cmd->params, "siid"));
    int piid = json_number_as_int(cJSON_GetObjectItem(cmd->params, "piid"));
    cJSON *val = cJSON_GetObjectItem(cmd->params, "value");
    if (!val || cJSON_IsNull(val)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsBool(val) && !cJSON_IsNumber(val) && !cJSON_IsString(val)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (siid <= 0 || piid <= 0) {
        const cJSON *key_j = cJSON_GetObjectItem(cmd->params, "key");
        if (cJSON_IsString(key_j) && key_j->valuestring) {
            const miot_spec_extra_t *ex = find_extra_by_key(dev, key_j->valuestring);
            if (ex) {
                if (siid <= 0) {
                    siid = ex->siid;
                }
                if (piid <= 0) {
                    piid = ex->piid;
                }
            }
        }
    }
    if (siid <= 0) {
        siid = dev->siid;
    }
    if (siid <= 0 || piid <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bool lan_ok = false;
    esp_err_t err = set_prop_siid_piid(dev, siid, piid, val, &lan_ok);
    if (err == ESP_OK) {
        log_control_path(dev, "set_prop", lan_ok);
        const miot_spec_extra_t *ex = find_extra_by_siid_piid(dev, siid, piid);
        if (ex && ex->key[0]) {
            if (strcmp(ex->kind, "switch") == 0) {
                bool on = false;
                if (json_to_bool(val, &on)) {
                    optimistic_merge_prop_bool(cmd->point_id, ex->key, on);
                }
            } else {
                optimistic_merge_from_cjson(cmd->point_id, ex->key, val);
            }
        }
    }
    return err;
}

static esp_err_t handle_call_action(miot_device_t *dev, const minihub_command_t *cmd)
{
    if (!cmd->params) {
        return ESP_ERR_INVALID_ARG;
    }
    int siid = json_number_as_int(cJSON_GetObjectItem(cmd->params, "siid"));
    int aiid = json_number_as_int(cJSON_GetObjectItem(cmd->params, "aiid"));
    const cJSON *key_j = cJSON_GetObjectItem(cmd->params, "key");
    const miot_spec_extra_t *ex = NULL;
    if (cJSON_IsString(key_j) && key_j->valuestring) {
        ex = find_extra_by_key(dev, key_j->valuestring);
        if (!ex && strcmp(key_j->valuestring, "play_text") == 0) {
            ex = find_extra_by_key(dev, "execute_text");
        }
        if (ex) {
            if (siid <= 0) {
                siid = ex->siid;
            }
            if (aiid <= 0) {
                aiid = ex->aiid;
            }
        }
    }
    if (siid <= 0 || aiid <= 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!dev->did[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    const cJSON *in_j = cJSON_GetObjectItem(cmd->params, "in");
    cJSON *in_owned = NULL;
    const cJSON *in_use = NULL;
    if (cJSON_IsArray(in_j)) {
        in_use = in_j;
    } else {
        const cJSON *text_j = cJSON_GetObjectItem(cmd->params, "text");
        if (!cJSON_IsString(text_j) || !text_j->valuestring) {
            text_j = cJSON_GetObjectItem(cmd->params, "message");
        }
        if (cJSON_IsString(text_j) && text_j->valuestring && text_j->valuestring[0]) {
            in_owned = build_tts_in(ex, text_j->valuestring,
                                    cJSON_GetObjectItem(cmd->params, "silent"));
            if (!in_owned) {
                return ESP_ERR_NO_MEM;
            }
            in_use = in_owned;
        }
    }

    bool tts = ex && (strcmp(ex->key, "play_text") == 0 ||
                      strcmp(ex->key, "execute_text") == 0);
    if (tts && !in_use) {
        cJSON_Delete(in_owned);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = minihub_miot_auth_action(dev->did, siid, aiid, in_use);
    cJSON_Delete(in_owned);
    return err;
}

static bool volume_ids(const miot_device_t *dev, int *siid, int *piid)
{
    if (!dev || !siid || !piid) {
        return false;
    }
    if (dev->piid_volume > 0 && dev->siid > 0) {
        *siid = dev->siid;
        *piid = dev->piid_volume;
        return true;
    }
    const miot_spec_extra_t *ex = find_extra_by_key(dev, "volume");
    if (ex && ex->siid > 0 && ex->piid > 0) {
        *siid = ex->siid;
        *piid = ex->piid;
        return true;
    }
    return false;
}

static esp_err_t handle_set_volume(miot_device_t *dev, const minihub_command_t *cmd)
{
    int siid = 0;
    int piid = 0;
    if (!volume_ids(dev, &siid, &piid)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    int vol = -1;
    if (cmd->params) {
        const cJSON *v = cJSON_GetObjectItem(cmd->params, "volume");
        if (cJSON_IsNumber(v)) {
            vol = v->valueint;
        } else if (cJSON_IsString(v) && v->valuestring) {
            vol = atoi(v->valuestring);
        }
    }
    if (vol < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* oh2p / common Xiaoai Spec volume value-range starts at 5. */
    if (vol < 5) {
        vol = 5;
    }
    if (vol > 100) {
        vol = 100;
    }
    esp_err_t err = set_number_prop_siid(dev, siid, piid, vol);
    if (err != ESP_OK) {
        return err;
    }
    if (strcmp(dev->type, "speaker") == 0) {
        char label[32];
        snprintf(label, sizeof(label), "音量%d", vol);
        minihub_point_t snap = {0};
        if (minihub_point_get_snapshot(cmd->point_id, &snap) == ESP_OK) {
            cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true)
                                      : cJSON_CreateObject();
            if (props) {
                cJSON_DeleteItemFromObject(props, "volume");
                cJSON_AddNumberToObject(props, "volume", vol);
                minihub_point_set(cmd->point_id, label, props);
                cJSON_Delete(props);
            }
            if (snap.props) {
                cJSON_Delete(snap.props);
            }
        }
    } else {
        optimistic_merge_prop_number(cmd->point_id, "volume", vol);
    }
    return err;
}

static esp_err_t miot_adapter_handle_command(const minihub_command_t *cmd)
{
    miot_device_t *dev = find_device(cmd->point_id);
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!device_online(dev)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Cover motor / position commands (and turn_on/off → open/close). */
    if (strcmp(dev->type, "cover") == 0) {
        if (strcmp(cmd->cmd, "open") == 0 || strcmp(cmd->cmd, "turn_on") == 0) {
            return set_motor(dev, dev->motor_open);
        }
        if (strcmp(cmd->cmd, "close") == 0 || strcmp(cmd->cmd, "turn_off") == 0) {
            return set_motor(dev, dev->motor_close);
        }
        if (strcmp(cmd->cmd, "stop") == 0) {
            return set_motor(dev, dev->motor_pause);
        }
        if (strcmp(cmd->cmd, "set_position") == 0) {
            if (dev->piid_target_position <= 0) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            const cJSON *v = NULL;
            if (cmd->params) {
                v = cJSON_GetObjectItem(cmd->params, "position");
                if (!cJSON_IsNumber(v) && !(cJSON_IsString(v) && v->valuestring)) {
                    v = cJSON_GetObjectItem(cmd->params, "target_position");
                }
            }
            int pos = -1;
            if (cJSON_IsNumber(v)) {
                pos = v->valueint;
            } else if (cJSON_IsString(v) && v->valuestring) {
                pos = atoi(v->valuestring);
            }
            if (pos < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            if (pos > 100) {
                pos = 100;
            }
            esp_err_t err = set_number_prop_siid(dev, dev->siid, dev->piid_target_position, pos);
            if (err == ESP_OK) {
                optimistic_merge_prop_number(cmd->point_id, "target_position", pos);
            }
            return err;
        }
        if (strcmp(cmd->cmd, "toggle") == 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    /* AC climate commands. */
    if (strcmp(cmd->cmd, "set_mode") == 0) {
        if (dev->piid_mode <= 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        int mode = -1;
        if (cmd->params) {
            mode = parse_climate_mode_param(cJSON_GetObjectItem(cmd->params, "mode"));
        }
        if (mode < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = set_number_prop(dev, dev->piid_mode, mode);
        if (err == ESP_OK) {
            optimistic_merge_prop_number(cmd->point_id, "mode", mode);
            const char *lab = climate_mode_label(mode);
            if (lab) {
                optimistic_merge_prop_string(cmd->point_id, "mode_label", lab);
            }
        }
        return err;
    }
    if (strcmp(cmd->cmd, "set_temperature") == 0) {
        if (dev->piid_target_temp <= 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        double temp = 26.0;
        bool have = false;
        if (cmd->params) {
            const cJSON *v = cJSON_GetObjectItem(cmd->params, "temperature");
            if (!cJSON_IsNumber(v)) {
                v = cJSON_GetObjectItem(cmd->params, "target_temp");
            }
            if (cJSON_IsNumber(v)) {
                temp = v->valuedouble;
                have = true;
            }
        }
        if (!have) {
            return ESP_ERR_INVALID_ARG;
        }
        if (temp < 16.0) {
            temp = 16.0;
        }
        if (temp > 30.0) {
            temp = 30.0;
        }
        esp_err_t err = set_double_prop_siid(dev, dev->siid, dev->piid_target_temp, temp);
        if (err == ESP_OK) {
            optimistic_merge_prop_number(cmd->point_id, "target_temp", temp);
        }
        return err;
    }
    if (strcmp(cmd->cmd, "set_fan_level") == 0) {
        if (dev->piid_fan <= 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        int fan = -1;
        if (cmd->params) {
            const cJSON *v = cJSON_GetObjectItem(cmd->params, "fan_level");
            if (!v) {
                v = cJSON_GetObjectItem(cmd->params, "fan");
            }
            fan = parse_climate_fan_param(v);
        }
        if (fan < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        int fan_siid = dev->siid_fan > 0 ? dev->siid_fan : dev->siid;
        esp_err_t err = set_number_prop_siid(dev, fan_siid, dev->piid_fan, fan);
        if (err == ESP_OK) {
            optimistic_merge_prop_number(cmd->point_id, "fan_level", fan);
            const char *lab = climate_fan_label(fan);
            if (lab) {
                optimistic_merge_prop_string(cmd->point_id, "fan_label", lab);
            }
        }
        return err;
    }

    if (strcmp(cmd->cmd, "set_prop") == 0) {
        return handle_set_prop(dev, cmd);
    }
    if (strcmp(cmd->cmd, "call_action") == 0) {
        return handle_call_action(dev, cmd);
    }
    if (strcmp(cmd->cmd, "set_volume") == 0) {
        return handle_set_volume(dev, cmd);
    }

    bool target_on = false;
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        target_on = true;
    } else if (strcmp(cmd->cmd, "turn_off") == 0) {
        target_on = false;
    } else if (strcmp(cmd->cmd, "toggle") == 0) {
        minihub_point_t snap = {0};
        minihub_point_get_snapshot(cmd->point_id, &snap);
        target_on = (strcmp(snap.value, "on") != 0);
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
    } else if (strcmp(cmd->cmd, "set_color_temp") == 0 || strcmp(cmd->cmd, "set_ct") == 0) {
        if (dev->piid_ct <= 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        int kelvin = 2700;
        if (cmd->params) {
            const cJSON *v = cJSON_GetObjectItem(cmd->params, "color_temp");
            if (!cJSON_IsNumber(v)) {
                v = cJSON_GetObjectItem(cmd->params, "temperature");
            }
            if (cJSON_IsNumber(v)) {
                kelvin = v->valueint;
            }
        }
        return set_number_prop(dev, dev->piid_ct, kelvin);
    } else if (strcmp(cmd->cmd, "set_color") == 0) {
        if (!dev->supports_color || dev->piid_color <= 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        const char *hex = NULL;
        if (cmd->params) {
            const cJSON *c = cJSON_GetObjectItem(cmd->params, "color");
            if (cJSON_IsString(c)) {
                hex = c->valuestring;
            }
        }
        if (!hex || hex[0] != '#' || strlen(hex) < 7) {
            return ESP_ERR_INVALID_ARG;
        }
        unsigned r = 0, g = 0, b = 0;
        if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3) {
            return ESP_ERR_INVALID_ARG;
        }
        int rgb = (int)((r << 16) | (g << 8) | b);
        esp_err_t err = set_number_prop(dev, dev->piid_color, rgb);
        if (err == ESP_OK) {
            minihub_point_t snap = {0};
            if (minihub_point_get_snapshot(cmd->point_id, &snap) == ESP_OK) {
                cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true)
                                          : cJSON_CreateObject();
                if (props) {
                    cJSON_DeleteItemFromObject(props, "color");
                    cJSON_AddStringToObject(props, "color", hex);
                    const char *v = snap.value[0] ? snap.value : "on";
                    minihub_point_set(cmd->point_id, v, props);
                    cJSON_Delete(props);
                }
                if (snap.props) {
                    cJSON_Delete(snap.props);
                }
            }
        }
        return err;
    } else if (strcmp(cmd->cmd, "set_brightness") == 0 && dev->piid_bright > 0) {
        int bright = 50;
        if (cmd->params) {
            const cJSON *v = cJSON_GetObjectItem(cmd->params, "brightness");
            if (cJSON_IsNumber(v)) {
                bright = v->valueint;
            }
        }
        if (bright < 0) {
            bright = 0;
        }
        if (bright > 100) {
            bright = 100;
        }
        int raw = bright_pct_to_raw(dev, bright);
        esp_err_t err = set_number_prop(dev, dev->piid_bright, raw);
        if (err == ESP_OK) {
            minihub_point_t snap = {0};
            if (minihub_point_get_snapshot(cmd->point_id, &snap) == ESP_OK) {
                cJSON *props = snap.props ? cJSON_Duplicate(snap.props, true)
                                          : cJSON_CreateObject();
                if (props) {
                    cJSON_DeleteItemFromObject(props, "brightness");
                    cJSON_AddNumberToObject(props, "brightness", bright);
                    const char *v = snap.value[0] ? snap.value : "on";
                    minihub_point_set(cmd->point_id, v, props);
                    cJSON_Delete(props);
                }
                if (snap.props) {
                    cJSON_Delete(snap.props);
                }
            }
        }
        return err;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (strcmp(dev->type, "binary_sensor") == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (strcmp(dev->type, "smoke") == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (dev->no_control) {
        /* Metering / breaker switches: read-only on Hub. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (strcmp(dev->type, "speaker") == 0 || strcmp(dev->type, "lock") == 0) {
        /* Volume / door-state are not power toggles. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (strcmp(dev->type, "car") == 0) {
        int siid = 0;
        int aiid = 0;
        if (!miot_car_lookup_ac(dev->model, target_on, &siid, &aiid)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        esp_err_t err = minihub_miot_auth_action(dev->did, siid, aiid, NULL);
        if (err == ESP_OK) {
            minihub_point_set(cmd->point_id, target_on ? "on" : "off", NULL);
        }
        return err;
    }

    esp_err_t err = set_bool_prop(dev, target_on);
    if (err == ESP_OK) {
        minihub_point_set(cmd->point_id, target_on ? "on" : "off", NULL);
    }
    return err;
}

const minihub_adapter_t miot_adapter = {
    .name = "miot",
    .version = "0.1.0",
    .init = miot_adapter_init,
    .start = miot_adapter_start,
    .poll = miot_adapter_poll,
    .handle_command = miot_adapter_handle_command,
};

/* Store-first: with DYNAMIC_MODULES, Host loads miot via .mhub (not .minihub_adapters). */
#if !CONFIG_MINIHUB_DYNAMIC_MODULES
REGISTER_ADAPTER(miot_adapter);
#endif
