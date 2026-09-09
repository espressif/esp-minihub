/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_miot_auth.h"
#include "module_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "miot_http.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "cJSON.h"

static const char *TAG = "miot_auth";
static miot_cloud_session_t s_session;

static int miot_tolower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

static bool miot_isxdigit(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

typedef struct {
    const char *username;
    const char *password;
    const char *capt_code;
    char *verify_url;
    size_t verify_url_len;
} miot_login_ctx_t;

typedef struct {
    const char *ticket;
} miot_verify_ctx_t;

typedef struct {
    cJSON **out_list;
    esp_err_t err;
} miot_device_list_ctx_t;

typedef struct {
    const char *did;
    int siid;
    int piid;
    cJSON **out_value;
    const cJSON *value;
    bool is_set;
    esp_err_t err;
} miot_prop_ctx_t;

typedef struct {
    const char *did;
    const cJSON *pairs;
    cJSON **out_result;
    esp_err_t err;
} miot_prop_multi_ctx_t;

typedef struct {
    const char *did;
    int siid;
    int aiid;
    const cJSON *in;
    esp_err_t err;
} miot_action_ctx_t;

typedef struct {
    const char *did;
    const char *key;
    const char *type;
    int limit;
    cJSON **out_result;
    esp_err_t err;
} miot_device_data_ctx_t;

typedef struct {
    const char *did;
    int limit;
    cJSON **out_result;
    esp_err_t err;
} miot_device_log_ctx_t;

typedef struct {
    const char *did;
    const char *model;
    const char *out_path;
    miot_camera_preview_info_t *info;
    esp_err_t err;
} miot_camera_preview_ctx_t;

typedef struct {
    const cJSON *cloud_list;
    cJSON *bindkeys;
    size_t ok;
    size_t fail;
} miot_bindkeys_ctx_t;

/** Normalize MAC to lowercase aa:bb:cc:dd:ee:ff (12 hex → colon form). */
static bool normalize_mac_str(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len < 18) {
        return false;
    }
    uint8_t bytes[6];
    int n = 0;
    for (const char *p = in; *p && n < 12; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') {
            continue;
        }
        char c = (char)miot_tolower((unsigned char)*p);
        if (!miot_isxdigit((unsigned char)c)) {
            return false;
        }
        int v = (c <= '9') ? (c - '0') : (c - 'a' + 10);
        if ((n & 1) == 0) {
            bytes[n / 2] = (uint8_t)(v << 4);
        } else {
            bytes[n / 2] |= (uint8_t)v;
        }
        n++;
    }
    if (n != 12) {
        return false;
    }
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return true;
}

static esp_err_t bindkeys_worker(void *arg)
{
    miot_bindkeys_ctx_t *ctx = arg;
    ctx->ok = 0;
    ctx->fail = 0;
    if (!miot_cloud_session_valid(&s_session) || !cJSON_IsArray(ctx->cloud_list) ||
        !cJSON_IsObject(ctx->bindkeys)) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *dev = NULL;
    cJSON_ArrayForEach(dev, ctx->cloud_list) {
        const cJSON *did = cJSON_GetObjectItem(dev, "did");
        const cJSON *mac = cJSON_GetObjectItem(dev, "mac");
        if (!cJSON_IsString(did) || strncmp(did->valuestring, "blt.", 4) != 0) {
            continue;
        }
        if (!cJSON_IsString(mac) || !mac->valuestring[0]) {
            ESP_LOGW(TAG, "blt device %s has no mac, skip bindkey", did->valuestring);
            ctx->fail++;
            continue;
        }
        char mac_key[24];
        if (!normalize_mac_str(mac->valuestring, mac_key, sizeof(mac_key))) {
            ESP_LOGW(TAG, "blt device %s bad mac, skip bindkey", did->valuestring);
            ctx->fail++;
            continue;
        }

        char key[48];
        esp_err_t err = miot_cloud_blt_get_beaconkey(&s_session, did->valuestring, key, sizeof(key));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "beaconkey fetch failed for %s: %s", mac_key, esp_err_to_name(err));
            ctx->fail++;
            continue;
        }
        cJSON_DeleteItemFromObject(ctx->bindkeys, mac_key);
        if (!cJSON_AddStringToObject(ctx->bindkeys, mac_key, key)) {
            ctx->fail++;
            continue;
        }
        ctx->ok++;
        ESP_LOGI(TAG, "bindkey synced for %s", mac_key);
    }
    return ESP_OK;
}

static esp_err_t prop_worker(void *arg)
{
    miot_prop_ctx_t *ctx = arg;
    if (!miot_cloud_session_valid(&s_session)) {
        ctx->err = ESP_ERR_INVALID_STATE;
        return ctx->err;
    }
    if (ctx->is_set) {
        ctx->err = miot_cloud_prop_set(&s_session, ctx->did, ctx->siid, ctx->piid,
                                       (cJSON *)ctx->value);
    } else {
        ctx->err = miot_cloud_prop_get(&s_session, ctx->did, ctx->siid, ctx->piid, ctx->out_value);
    }
    return ctx->err;
}

static esp_err_t prop_multi_worker(void *arg)
{
    miot_prop_multi_ctx_t *ctx = arg;
    if (!miot_cloud_session_valid(&s_session)) {
        ctx->err = ESP_ERR_INVALID_STATE;
        return ctx->err;
    }
    ctx->err = miot_cloud_prop_get_multi(&s_session, ctx->did, ctx->pairs, ctx->out_result);
    return ctx->err;
}

static esp_err_t action_worker(void *arg)
{
    miot_action_ctx_t *ctx = arg;
    if (!miot_cloud_session_valid(&s_session)) {
        ctx->err = ESP_ERR_INVALID_STATE;
        return ctx->err;
    }
    ctx->err = miot_cloud_action(&s_session, ctx->did, ctx->siid, ctx->aiid, ctx->in);
    return ctx->err;
}

static esp_err_t device_data_worker(void *arg)
{
    miot_device_data_ctx_t *ctx = arg;
    if (!miot_cloud_session_valid(&s_session)) {
        ctx->err = ESP_ERR_INVALID_STATE;
        return ctx->err;
    }
    ctx->err = miot_cloud_get_user_device_data(&s_session, ctx->did, ctx->key, ctx->type,
                                              ctx->limit, ctx->out_result);
    return ctx->err;
}

static esp_err_t device_log_worker(void *arg)
{
    miot_device_log_ctx_t *ctx = arg;
    if (!miot_cloud_session_valid(&s_session)) {
        ctx->err = ESP_ERR_INVALID_STATE;
        return ctx->err;
    }
    ctx->err = miot_cloud_get_user_device_log(&s_session, ctx->did, ctx->limit, ctx->out_result);
    return ctx->err;
}

static esp_err_t camera_preview_worker(void *arg)
{
    miot_camera_preview_ctx_t *ctx = arg;
    if (!miot_cloud_session_valid(&s_session)) {
        ctx->err = ESP_ERR_INVALID_STATE;
        return ctx->err;
    }
    ctx->err = miot_cloud_camera_preview_jpeg(&s_session, ctx->did, ctx->model, ctx->out_path, ctx->info);
    return ctx->err;
}

static esp_err_t login_worker(void *arg)
{
    miot_login_ctx_t *ctx = arg;
    return miot_cloud_login(ctx->username, ctx->password, ctx->capt_code, &s_session,
                            ctx->verify_url, ctx->verify_url_len);
}

typedef struct {
    char icode[16];
} miot_send_2fa_ctx_t;

static esp_err_t send_2fa_worker(void *arg)
{
    miot_send_2fa_ctx_t *ctx = arg;
    const char *icode = (ctx && ctx->icode[0]) ? ctx->icode : NULL;
    if (miot_cloud_has_pending_login_captcha()) {
        if (!icode || !icode[0]) {
            return miot_cloud_last_2fa_captcha_b64() ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_STATE;
        }
        char verify_url[MIOT_2FA_URL_MAX] = {0};
        esp_err_t err =
            miot_cloud_login_captcha_retry(icode, &s_session, verify_url, sizeof(verify_url));
        /* Passport captcha cleared into 2FA — send SMS in the same step. */
        if (err == ESP_ERR_INVALID_STATE && miot_cloud_has_pending_2fa() &&
            !miot_cloud_last_2fa_captcha_b64()) {
            ESP_LOGI(TAG, "login captcha ok → auto-send 2FA SMS");
            return miot_cloud_send_2fa_email(&s_session, NULL);
        }
        return err;
    }
    return miot_cloud_send_2fa_email(&s_session, icode);
}

static esp_err_t verify_worker(void *arg)
{
    miot_verify_ctx_t *ctx = arg;
    return miot_cloud_verify_2fa_ticket(ctx->ticket, &s_session);
}

static esp_err_t device_list_worker(void *arg)
{
    miot_device_list_ctx_t *ctx = arg;
    ctx->err = miot_cloud_device_list(&s_session, ctx->out_list);
    return ctx->err;
}

esp_err_t minihub_miot_auth_init(void)
{
    memset(&s_session, 0, sizeof(s_session));
    strncpy(s_session.region, "cn", sizeof(s_session.region) - 1);

    /* Session comes from NVS / Web login only — no Host Kconfig cloud tokens. */

    esp_err_t err = miot_cloud_session_load(&s_session);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MIoT cloud session loaded (user %s)", s_session.user_id);
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "MIoT session load failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

bool minihub_miot_auth_has_session(void)
{
    return miot_cloud_session_valid(&s_session);
}

const miot_cloud_session_t *minihub_miot_auth_session(void)
{
    return miot_cloud_session_valid(&s_session) ? &s_session : NULL;
}

esp_err_t minihub_miot_auth_login(const char *username, const char *password,
                                  char *verify_url, size_t verify_url_len)
{
    miot_login_ctx_t ctx = {
        .username = username,
        .password = password,
        .capt_code = NULL,
        .verify_url = verify_url,
        .verify_url_len = verify_url_len,
    };
    esp_err_t err = miot_http_run_worker(login_worker, &ctx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MIoT cloud login ok, user %s", s_session.user_id);
    } else if (const char *msg = miot_cloud_last_error()) {
        ESP_LOGW(TAG, "MIoT login failed: %s (%s)", msg, esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "MIoT login failed: %s", esp_err_to_name(err));
    }
    return err;
}

const char *minihub_miot_auth_last_error(void)
{
    return miot_cloud_last_error();
}

esp_err_t minihub_miot_auth_import_session(const char *user_id, const char *service_token,
                                           const char *ssecurity, const char *device_id,
                                           const char *region)
{
    if (!user_id || !service_token || !ssecurity) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_session, 0, sizeof(s_session));
    strncpy(s_session.user_id, user_id, sizeof(s_session.user_id) - 1);
    strncpy(s_session.service_token, service_token, sizeof(s_session.service_token) - 1);
    strncpy(s_session.ssecurity, ssecurity, sizeof(s_session.ssecurity) - 1);
    if (device_id && device_id[0]) {
        strncpy(s_session.device_id, device_id, sizeof(s_session.device_id) - 1);
        s_session.device_id_pinned = true;
    } else {
        s_session.device_id_pinned = false;
    }
    if (region && region[0]) {
        strncpy(s_session.region, region, sizeof(s_session.region) - 1);
    } else {
        strncpy(s_session.region, "cn", sizeof(s_session.region) - 1);
    }
    s_session.valid = true;
    esp_err_t err = miot_cloud_session_save(&s_session);
    if (err == ESP_OK) {
        miot_cloud_clear_pending_2fa();
        miot_cloud_clear_pending_login_captcha();
    }
    return err;
}

esp_err_t minihub_miot_auth_logout(void)
{
    size_t removed = 0;
    esp_err_t err = minihub_miot_devices_clear(&removed);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "logout: clear devices failed: %s", esp_err_to_name(err));
    }
    miot_cloud_session_clear(&s_session);
    miot_cloud_clear_pending_2fa();
    miot_cloud_clear_pending_login_captcha();
    ESP_LOGI(TAG, "MIoT logout ok, removed %u device(s)", (unsigned)removed);
    return ESP_OK;
}

esp_err_t minihub_miot_auth_device_list(cJSON **out_list)
{
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_device_list_ctx_t ctx = { .out_list = out_list, .err = ESP_FAIL };
    return miot_http_run_worker(device_list_worker, &ctx);
}

typedef struct {
    cJSON **out_result;
    esp_err_t err;
} miot_homeroom_ctx_t;

static esp_err_t homeroom_worker(void *arg)
{
    miot_homeroom_ctx_t *ctx = arg;
    ctx->err = miot_cloud_homeroom_gethome(&s_session, ctx->out_result);
    return ctx->err;
}

esp_err_t minihub_miot_auth_homeroom_gethome(cJSON **out_result)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = NULL;
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_homeroom_ctx_t ctx = { .out_result = out_result, .err = ESP_FAIL };
    esp_err_t err = miot_http_run_worker(homeroom_worker, &ctx);
    esp_err_t result = err != ESP_OK ? err : ctx.err;
    if (result == ESP_ERR_INVALID_STATE) {
        const char *msg = miot_cloud_last_error();
        if (msg && strstr(msg, "过期")) {
            ESP_LOGW(TAG, "cloud session expired — invalidating");
            miot_cloud_session_invalidate(&s_session);
        }
    }
    return result;
}

esp_err_t minihub_miot_auth_prop_get(const char *did, int siid, int piid, cJSON **out_value)
{
    if (!did || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_prop_ctx_t ctx = {
        .did = did,
        .siid = siid,
        .piid = piid,
        .out_value = out_value,
        .is_set = false,
        .err = ESP_FAIL,
    };
    return miot_http_run_worker(prop_worker, &ctx);
}

esp_err_t minihub_miot_auth_prop_get_multi(const char *did, const cJSON *pairs, cJSON **out_result)
{
    if (!did || !cJSON_IsArray(pairs) || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_prop_multi_ctx_t ctx = {
        .did = did,
        .pairs = pairs,
        .out_result = out_result,
        .err = ESP_FAIL,
    };
    return miot_http_run_worker(prop_multi_worker, &ctx);
}

esp_err_t minihub_miot_auth_prop_set(const char *did, int siid, int piid, const cJSON *value)
{
    if (!did || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_prop_ctx_t ctx = {
        .did = did,
        .siid = siid,
        .piid = piid,
        .value = value,
        .is_set = true,
        .err = ESP_FAIL,
    };
    return miot_http_run_worker(prop_worker, &ctx);
}

esp_err_t minihub_miot_auth_action(const char *did, int siid, int aiid, const cJSON *in)
{
    if (!did) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in && !cJSON_IsArray(in)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_action_ctx_t ctx = {
        .did = did,
        .siid = siid,
        .aiid = aiid,
        .in = in,
        .err = ESP_FAIL,
    };
    return miot_http_run_worker(action_worker, &ctx);
}

esp_err_t minihub_miot_auth_get_user_device_data(const char *did, const char *key, const char *type,
                                                 int limit, cJSON **out_result)
{
    if (!did || !key || !type || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_device_data_ctx_t ctx = {
        .did = did,
        .key = key,
        .type = type,
        .limit = limit,
        .out_result = out_result,
        .err = ESP_FAIL,
    };
    return miot_http_run_worker(device_data_worker, &ctx);
}

esp_err_t minihub_miot_auth_get_user_device_log(const char *did, int limit, cJSON **out_result)
{
    if (!did || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_device_log_ctx_t ctx = {
        .did = did,
        .limit = limit,
        .out_result = out_result,
        .err = ESP_FAIL,
    };
    return miot_http_run_worker(device_log_worker, &ctx);
}

esp_err_t minihub_miot_auth_camera_preview(const char *did, const char *model,
                                           const char *out_path, miot_camera_preview_info_t *info)
{
    if (!did || !did[0] || !out_path || !out_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_camera_preview_ctx_t ctx = {
        .did = did,
        .model = model,
        .out_path = out_path,
        .info = info,
        .err = ESP_FAIL,
    };
    return miot_http_run_worker(camera_preview_worker, &ctx);
}

esp_err_t minihub_miot_auth_fetch_ble_bindkeys(const cJSON *cloud_list, cJSON *bindkeys_obj,
                                               size_t *ok_count, size_t *fail_count)
{
    if (!cJSON_IsArray(cloud_list) || !cJSON_IsObject(bindkeys_obj)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!miot_cloud_session_valid(&s_session)) {
        return ESP_ERR_INVALID_STATE;
    }
    miot_bindkeys_ctx_t ctx = {
        .cloud_list = cloud_list,
        .bindkeys = bindkeys_obj,
        .ok = 0,
        .fail = 0,
    };
    esp_err_t err = miot_http_run_worker(bindkeys_worker, &ctx);
    if (ok_count) {
        *ok_count = ctx.ok;
    }
    if (fail_count) {
        *fail_count = ctx.fail;
    }
    return err;
}

esp_err_t minihub_miot_auth_send_2fa_email(const char *icode)
{
    miot_send_2fa_ctx_t ctx = {0};
    if (icode && icode[0]) {
        strncpy(ctx.icode, icode, sizeof(ctx.icode) - 1);
    }
    esp_err_t err = miot_http_run_worker(send_2fa_worker, &ctx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "2FA code sent");
    } else if (err == ESP_ERR_INVALID_STATE && miot_cloud_last_2fa_captcha_b64()) {
        ESP_LOGI(TAG, "2FA send needs captcha");
    } else {
        ESP_LOGW(TAG, "2FA send code failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t minihub_miot_auth_verify_2fa(const char *ticket)
{
    miot_verify_ctx_t ctx = { .ticket = ticket };
    esp_err_t err = miot_http_run_worker(verify_worker, &ctx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MIoT 2FA verify ok, user %s", s_session.user_id);
    }
    return err;
}

esp_err_t minihub_miot_auth_login_retry(const char *username, const char *password,
                                        char *verify_url, size_t verify_url_len)
{
    miot_login_ctx_t ctx = {
        .username = username,
        .password = password,
        .capt_code = NULL,
        .verify_url = verify_url,
        .verify_url_len = verify_url_len,
    };
    esp_err_t err = miot_http_run_worker(login_worker, &ctx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MIoT login retry ok, user %s", s_session.user_id);
    } else if (const char *msg = miot_cloud_last_error()) {
        ESP_LOGW(TAG, "MIoT login retry failed: %s (%s)", msg, esp_err_to_name(err));
    }
    return err;
}

cJSON *minihub_miot_auth_status_json(void)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "logged_in", minihub_miot_auth_has_session());
    if (s_session.user_id[0]) {
        cJSON_AddStringToObject(obj, "user_id", s_session.user_id);
    }
    if (s_session.region[0]) {
        cJSON_AddStringToObject(obj, "region", s_session.region);
    }
    cJSON_AddStringToObject(obj, "mode", "cloud");

    nvs_handle_t nvs;
    bool pending_2fa = false;
    bool logged_in = minihub_miot_auth_has_session();
    if (!logged_in && nvs_open("miot_cloud", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_str(nvs, "pending_2fa", NULL, &len) == ESP_OK && len > 1) {
            pending_2fa = true;
        }
        nvs_close(nvs);
    }
    cJSON_AddBoolToObject(obj, "pending_2fa", pending_2fa);

    /* Selected Mi Home (persisted by sync). */
    const minihub_module_host_api_t *host = minihub_miot_module_host();
    if (host && host->config_get_adapters) {
        cJSON *root = host->config_get_adapters();
        const cJSON *adapters = root ? cJSON_GetObjectItem(root, "adapters") : NULL;
        const cJSON *miot = adapters ? cJSON_GetObjectItem(adapters, "miot") : NULL;
        const cJSON *hid = miot ? cJSON_GetObjectItem(miot, "home_id") : NULL;
        const cJSON *hname = miot ? cJSON_GetObjectItem(miot, "home_name") : NULL;
        if (cJSON_IsString(hid) && hid->valuestring[0]) {
            cJSON_AddStringToObject(obj, "home_id", hid->valuestring);
        } else if (cJSON_IsNumber(hid)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", hid->valueint);
            cJSON_AddStringToObject(obj, "home_id", buf);
        }
        if (cJSON_IsString(hname) && hname->valuestring[0]) {
            cJSON_AddStringToObject(obj, "home_name", hname->valuestring);
        }
    }
    return obj;
}
