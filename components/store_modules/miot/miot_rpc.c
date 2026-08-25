/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MIoT 模块 RPC（第三方生态实现，Host 通过 /api/modules/miot/rpc 调用）
 */

#include "minihub_miot_auth.h"
#include "minihub_module_abi.h"
#include "module_bridge.h"
#include "miot_cloud.h"
#include "miot_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "miot_rpc";

static void request_adapter_soft_reload(const char *name)
{
    const minihub_module_host_api_t *host = minihub_miot_module_host();
    if (!host || !name) {
        return;
    }
    if (host->struct_size >= sizeof(minihub_module_host_api_t) && host->adapter_soft_reload) {
        (void)host->adapter_soft_reload(name);
    }
}

static void request_miot_reload(void)
{
    const minihub_module_host_api_t *host = minihub_miot_module_host();
    if (!host) {
        return;
    }
    /* Prefer soft reload: avoid dlclose + Zigbee/Wi-Fi radio tear-down during sync. */
    if (host->struct_size >= sizeof(minihub_module_host_api_t) && host->adapter_soft_reload) {
        host->adapter_soft_reload("miot");
        /* Only reload xiaomi_ble when that adapter exists (avoids noisy NOT_FOUND). */
        if (host->config_get_adapters) {
            cJSON *root = host->config_get_adapters();
            const cJSON *adapters = root ? cJSON_GetObjectItem(root, "adapters") : NULL;
            if (adapters && cJSON_GetObjectItem(adapters, "xiaomi_ble")) {
                host->adapter_soft_reload("xiaomi_ble");
            }
        }
    } else if (host->hub_reload) {
        host->hub_reload();
    }
}

typedef struct {
    bool merge;
    bool reload;
    char home_id[64];
    char home_name[64];
    bool explicit_home;
    bool auto_picked_home;
    esp_err_t sync_err;
    size_t synced;
    size_t lan_tokens;
    size_t bindkeys;
} miot_sync_job_t;

static void json_home_id_to_str(const cJSON *id, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return;
    }
    buf[0] = '\0';
    if (cJSON_IsString(id) && id->valuestring[0]) {
        strncpy(buf, id->valuestring, buf_size - 1);
    } else if (cJSON_IsNumber(id)) {
        snprintf(buf, buf_size, "%d", id->valueint);
    }
}

static bool first_home_from_gethome_list(const cJSON *list, char *id_out, size_t id_size,
                                          char *name_out, size_t name_size)
{
    const cJSON *home = NULL;

    if (!id_out || id_size == 0 || !cJSON_IsArray(list)) {
        return false;
    }
    id_out[0] = '\0';
    if (name_out && name_size > 0) {
        name_out[0] = '\0';
    }

    cJSON_ArrayForEach(home, list) {
        char id_buf[64];
        const cJSON *id = cJSON_GetObjectItem(home, "id");
        const cJSON *name = cJSON_GetObjectItem(home, "name");

        json_home_id_to_str(id, id_buf, sizeof(id_buf));
        if (!id_buf[0]) {
            continue;
        }
        strncpy(id_out, id_buf, id_size - 1);
        id_out[id_size - 1] = '\0';
        if (name_out && name_size > 0) {
            if (cJSON_IsString(name) && name->valuestring[0]) {
                strncpy(name_out, name->valuestring, name_size - 1);
            } else {
                strncpy(name_out, id_buf, name_size - 1);
            }
            name_out[name_size - 1] = '\0';
        }
        return true;
    }
    return false;
}

static bool first_home_from_gethome(cJSON *gethome, char *id_out, size_t id_size, char *name_out,
                                    size_t name_size)
{
    if (!gethome) {
        return false;
    }
    if (first_home_from_gethome_list(cJSON_GetObjectItem(gethome, "homelist"), id_out, id_size,
                                     name_out, name_size)) {
        return true;
    }
    return first_home_from_gethome_list(cJSON_GetObjectItem(gethome, "share_home_list"), id_out,
                                        id_size, name_out, name_size);
}

static esp_err_t miot_read_saved_home(char *id_out, size_t id_size, char *name_out, size_t name_size)
{
    const minihub_module_host_api_t *host = minihub_miot_module_host();

    if (!id_out || id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    id_out[0] = '\0';
    if (name_out && name_size > 0) {
        name_out[0] = '\0';
    }
    if (!host || !host->config_get_adapters) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = host->config_get_adapters();
    const cJSON *adapters = root ? cJSON_GetObjectItem(root, "adapters") : NULL;
    const cJSON *miot = adapters ? cJSON_GetObjectItem(adapters, "miot") : NULL;
    const cJSON *hid = miot ? cJSON_GetObjectItem(miot, "home_id") : NULL;
    const cJSON *hname = miot ? cJSON_GetObjectItem(miot, "home_name") : NULL;

    json_home_id_to_str(hid, id_out, id_size);
    if (!id_out[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    if (name_out && name_size > 0 && cJSON_IsString(hname) && hname->valuestring[0]) {
        strncpy(name_out, hname->valuestring, name_size - 1);
        name_out[name_size - 1] = '\0';
    }
    return ESP_OK;
}

static esp_err_t miot_resolve_sync_home_id(char *id_out, size_t id_size, char *name_out,
                                           size_t name_size, bool *auto_picked_out)
{
    if (!id_out || id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (auto_picked_out) {
        *auto_picked_out = false;
    }

    if (miot_read_saved_home(id_out, id_size, name_out, name_size) == ESP_OK) {
        return ESP_OK;
    }

    /* Default: sync all cloud devices without homeroom/gethome (avoids 2FA token edge cases).
     * User can list/switch homes later via /miot homes + /miot home <id>. */
    strncpy(id_out, "0", id_size - 1);
    id_out[id_size - 1] = '\0';
    if (name_out && name_size > 0) {
        strncpy(name_out, "全部设备", name_size - 1);
        name_out[name_size - 1] = '\0';
    }
    if (auto_picked_out) {
        *auto_picked_out = true;
    }
    return ESP_OK;
}

static void miot_sync_job_add_home_echo(cJSON *resp, const miot_sync_job_t *job)
{
    if (!resp || !job) {
        return;
    }
    if (job->home_id[0]) {
        cJSON_AddStringToObject(resp, "home_id", job->home_id);
    }
    if (job->home_name[0]) {
        cJSON_AddStringToObject(resp, "home_name", job->home_name);
    }
    if (job->auto_picked_home) {
        cJSON_AddTrueToObject(resp, "auto_picked_home");
    }
}

static esp_err_t miot_sync_job_worker(void *arg)
{
    miot_sync_job_t *job = arg;
    const char *sync_home = NULL;

    if (!job) {
        return ESP_ERR_INVALID_ARG;
    }

    if (job->explicit_home && job->home_id[0]) {
        sync_home = job->home_id;
    } else if (job->home_id[0]) {
        sync_home = job->home_id;
    } else {
        esp_err_t pick = miot_resolve_sync_home_id(job->home_id, sizeof(job->home_id),
                                                   job->home_name, sizeof(job->home_name),
                                                   &job->auto_picked_home);
        if (pick != ESP_OK) {
            ESP_LOGW(TAG, "sync home resolve failed: %s", esp_err_to_name(pick));
            job->sync_err = pick;
            return ESP_OK;
        }
        sync_home = job->home_id;
    }

    job->sync_err = minihub_miot_sync_devices(job->merge, sync_home, &job->synced, &job->lan_tokens,
                                              &job->bindkeys);
    if (job->sync_err == ESP_OK && !job->home_name[0]) {
        (void)miot_read_saved_home(job->home_id, sizeof(job->home_id), job->home_name,
                                   sizeof(job->home_name));
    }
    return ESP_OK;
}

static esp_err_t miot_run_sync_job(miot_sync_job_t *job)
{
    esp_err_t run_err;

    if (!job) {
        return ESP_ERR_INVALID_ARG;
    }
    job->sync_err = ESP_FAIL;
    job->synced = 0;
    job->lan_tokens = 0;
    job->bindkeys = 0;
    run_err = miot_http_run_worker(miot_sync_job_worker, job);
    if (run_err != ESP_OK) {
        return run_err;
    }
    return ESP_OK;
}

static void auto_sync_to_response(cJSON *resp)
{
    miot_sync_job_t job = {
        .merge = true,
        .reload = true,
    };
    bool session_ok = minihub_miot_auth_has_session();

    if (!resp) {
        return;
    }

    /* Cloud sync is heavy (HTTPS + adapters.json); never run on httpd/event_router stack. */
    esp_err_t run_err = miot_run_sync_job(&job);
    if (run_err != ESP_OK) {
        cJSON_AddStringToObject(resp, "sync", "fail");
        cJSON_AddStringToObject(resp, "sync_error", esp_err_to_name(run_err));
        return;
    }
    if (job.sync_err == ESP_ERR_NOT_FOUND) {
        cJSON_AddStringToObject(resp, "sync", "need_home");
        return;
    }
    if (job.sync_err == ESP_ERR_INVALID_STATE) {
        if (!session_ok && !minihub_miot_auth_has_session()) {
            cJSON_AddStringToObject(resp, "sync", "need_login");
            cJSON_AddStringToObject(resp, "sync_error", "ESP_ERR_INVALID_STATE");
        } else {
            cJSON_AddStringToObject(resp, "sync", "fail");
            cJSON_AddStringToObject(resp, "sync_error", "ESP_ERR_INVALID_STATE");
            if (const char *msg = minihub_miot_auth_last_error()) {
                if (msg[0]) {
                    cJSON_AddStringToObject(resp, "message", msg);
                }
            }
        }
        return;
    }
    if (job.sync_err != ESP_OK) {
        cJSON_AddStringToObject(resp, "sync", "fail");
        cJSON_AddStringToObject(resp, "sync_error", esp_err_to_name(job.sync_err));
        return;
    }
    request_miot_reload();
    cJSON_AddStringToObject(resp, "sync", "ok");
    cJSON_AddNumberToObject(resp, "synced", (double)job.synced);
    cJSON_AddNumberToObject(resp, "lan_tokens", (double)job.lan_tokens);
    cJSON_AddNumberToObject(resp, "bindkeys", (double)job.bindkeys);
    cJSON_AddBoolToObject(resp, "reloaded", true);
    miot_sync_job_add_home_echo(resp, &job);
}

static esp_err_t rpc_status(const cJSON *params, cJSON **out)
{
    (void)params;
    cJSON *status = minihub_miot_auth_status_json();
    if (!status) {
        return ESP_ERR_NO_MEM;
    }
    *out = status;
    return ESP_OK;
}

/**
 * After login reaches pending 2FA, try sendTicket once so clients can skip an
 * explicit verify_send. On captcha / rate-limit, leave result for manual send.
 * Assumes resp already has result=verify (may rewrite to captcha / ok).
 */
static void rpc_try_auto_send_2fa(cJSON *resp)
{
    if (!resp) {
        return;
    }

    esp_err_t send_err = minihub_miot_auth_send_2fa_email(NULL);
    if (send_err == ESP_OK) {
        if (minihub_miot_auth_has_session()) {
            cJSON_ReplaceItemInObject(resp, "result", cJSON_CreateString("ok"));
            cJSON_DeleteItemFromObject(resp, "verify_url");
            cJSON_DeleteItemFromObject(resp, "pending_2fa");
            cJSON_DeleteItemFromObject(resp, "hint");
            auto_sync_to_response(resp);
            return;
        }
        cJSON_AddTrueToObject(resp, "code_sent");
        cJSON_AddStringToObject(resp, "message", "SMS verification code sent");
        cJSON_ReplaceItemInObject(
            resp, "hint",
            cJSON_CreateString("短信验证码已发送，请填写验证码后登录"));
        return;
    }

    const char *captcha_b64 = miot_cloud_last_2fa_captcha_b64();
    if (send_err == ESP_ERR_INVALID_STATE && captcha_b64 && captcha_b64[0]) {
        cJSON_ReplaceItemInObject(resp, "result", cJSON_CreateString("captcha"));
        cJSON_AddStringToObject(resp, "captcha_b64", captcha_b64);
        if (const char *mime = miot_cloud_last_2fa_captcha_mime()) {
            cJSON_AddStringToObject(resp, "captcha_mime", mime);
        }
        cJSON_ReplaceItemInObject(
            resp, "hint",
            cJSON_CreateString("需要图形验证码，请输入后点击「发送验证码」"));
        if (const char *msg = minihub_miot_auth_last_error()) {
            cJSON_AddStringToObject(resp, "message", msg);
        }
        return;
    }

    cJSON_AddFalseToObject(resp, "code_sent");
    cJSON_ReplaceItemInObject(
        resp, "hint",
        cJSON_CreateString("自动发送失败，请点击「发送验证码」后再填写短信码登录"));
    if (const char *msg = minihub_miot_auth_last_error()) {
        cJSON_AddStringToObject(resp, "message", msg);
    } else {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(send_err));
    }
}

static esp_err_t rpc_login(const cJSON *params, cJSON **out)
{
    const cJSON *username = params ? cJSON_GetObjectItem(params, "username") : NULL;
    const cJSON *password = params ? cJSON_GetObjectItem(params, "password") : NULL;
    if (!cJSON_IsString(username) || !cJSON_IsString(password)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Default true for WeChat /miot login; Web passes auto_send:false so login
     * returns verify/captcha quickly and the page owns "发送验证码". */
    bool auto_send = true;
    const cJSON *as = params ? cJSON_GetObjectItem(params, "auto_send") : NULL;
    if (cJSON_IsBool(as)) {
        auto_send = cJSON_IsTrue(as);
    }

    char *verify_url = calloc(1, MIOT_2FA_URL_MAX);
    if (!verify_url) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = minihub_miot_auth_login(username->valuestring, password->valuestring,
                                            verify_url, MIOT_2FA_URL_MAX);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        free(verify_url);
        return ESP_ERR_NO_MEM;
    }

    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "result", "ok");
        auto_sync_to_response(resp);
    } else if (err == ESP_ERR_INVALID_STATE) {
        const char *captcha_b64 = miot_cloud_last_2fa_captcha_b64();
        if (miot_cloud_has_pending_login_captcha() && captcha_b64 && captcha_b64[0]) {
            cJSON_AddStringToObject(resp, "result", "captcha");
            cJSON_AddStringToObject(resp, "captcha_b64", captcha_b64);
            if (const char *mime = miot_cloud_last_2fa_captcha_mime()) {
                cJSON_AddStringToObject(resp, "captcha_mime", mime);
            }
            cJSON_AddStringToObject(resp, "hint",
                                    "输入图形验证码后点击「发送验证码」");
        } else {
            /* 2FA required — never treat as hard fail on login screen. */
            const char *vu = verify_url[0] ? verify_url : miot_cloud_last_verify_url();
            cJSON_AddStringToObject(resp, "result", "verify");
            if (vu && vu[0]) {
                cJSON_AddStringToObject(resp, "verify_url", vu);
            } else if (miot_cloud_has_pending_2fa()) {
                cJSON_AddTrueToObject(resp, "pending_2fa");
            }
            if (auto_send) {
                cJSON_AddStringToObject(resp, "hint", "正在发送短信验证码…");
                rpc_try_auto_send_2fa(resp);
            } else {
                cJSON_AddStringToObject(resp, "hint",
                                        "请点击「发送验证码」，收到短信后填写并点登录");
            }
        }
    } else {
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        if (const char *msg = minihub_miot_auth_last_error()) {
            cJSON_AddStringToObject(resp, "message", msg);
        }
    }
    free(verify_url);
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_session(const cJSON *params, cJSON **out)
{
    const cJSON *user_id = params ? cJSON_GetObjectItem(params, "user_id") : NULL;
    const cJSON *service_token = params ? cJSON_GetObjectItem(params, "serviceToken") : NULL;
    const cJSON *ssecurity = params ? cJSON_GetObjectItem(params, "ssecurity") : NULL;
    const cJSON *device_id = params ? cJSON_GetObjectItem(params, "device_id") : NULL;
    if (!cJSON_IsString(device_id)) {
        device_id = params ? cJSON_GetObjectItem(params, "deviceId") : NULL;
    }
    const cJSON *region = params ? cJSON_GetObjectItem(params, "region") : NULL;

    if (!cJSON_IsString(user_id) || !cJSON_IsString(service_token) || !cJSON_IsString(ssecurity)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = minihub_miot_auth_import_session(
        user_id->valuestring,
        service_token->valuestring,
        ssecurity->valuestring,
        cJSON_IsString(device_id) ? device_id->valuestring : "",
        cJSON_IsString(region) ? region->valuestring : "cn");

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    if (err == ESP_OK) {
        auto_sync_to_response(resp);
    } else {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    }
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_logout(const cJSON *params, cJSON **out)
{
    (void)params;
    esp_err_t err = minihub_miot_auth_logout();
    if (err == ESP_OK) {
        request_miot_reload();
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    cJSON_AddBoolToObject(resp, "reloaded", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    }
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_sync(const cJSON *params, cJSON **out)
{
    bool merge = true;
    bool reload = true;
    miot_sync_job_t job = {
        .merge = true,
        .reload = true,
    };

    if (params) {
        const cJSON *m = cJSON_GetObjectItem(params, "merge");
        const cJSON *r = cJSON_GetObjectItem(params, "reload");
        const cJSON *hid = cJSON_GetObjectItem(params, "home_id");
        if (cJSON_IsBool(m)) {
            merge = cJSON_IsTrue(m);
        }
        if (cJSON_IsBool(r)) {
            reload = cJSON_IsTrue(r);
        }
        if (cJSON_IsString(hid) && hid->valuestring[0]) {
            strncpy(job.home_id, hid->valuestring, sizeof(job.home_id) - 1);
            job.explicit_home = true;
        } else if (cJSON_IsNumber(hid)) {
            snprintf(job.home_id, sizeof(job.home_id), "%d", hid->valueint);
            job.explicit_home = true;
        }
    }
    job.merge = merge;
    job.reload = reload;

    bool session_ok = minihub_miot_auth_has_session();
    esp_err_t run_err = miot_run_sync_job(&job);
    if (run_err != ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(run_err));
        *out = resp;
        return ESP_OK;
    }
    if (job.sync_err == ESP_ERR_NOT_FOUND) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(resp, "result", "need_home");
        *out = resp;
        return ESP_OK;
    }
    if (job.sync_err == ESP_ERR_INVALID_STATE) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return ESP_ERR_NO_MEM;
        }
        if (!session_ok && !minihub_miot_auth_has_session()) {
            cJSON_AddStringToObject(resp, "result", "fail");
            cJSON_AddStringToObject(resp, "error", "ESP_ERR_INVALID_STATE");
            cJSON_AddStringToObject(resp, "message", "未登录或登录已过期，请重新登录米家");
            cJSON_AddBoolToObject(resp, "need_login", true);
        } else {
            cJSON_AddStringToObject(resp, "result", "fail");
            cJSON_AddStringToObject(resp, "error", "ESP_ERR_INVALID_STATE");
            if (const char *msg = minihub_miot_auth_last_error()) {
                if (msg[0]) {
                    cJSON_AddStringToObject(resp, "message", msg);
                }
            }
        }
        *out = resp;
        return ESP_OK;
    }
    if (job.sync_err != ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(job.sync_err));
        if (const char *msg = minihub_miot_auth_last_error()) {
            if (msg[0]) {
                cJSON_AddStringToObject(resp, "message", msg);
            }
        }
        *out = resp;
        return ESP_OK;
    }

    if (reload) {
        request_miot_reload();
    } else {
        /* Still reload xiaomi_ble so new bindkeys take effect without full miot restart. */
        request_adapter_soft_reload("xiaomi_ble");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddNumberToObject(resp, "synced", (double)job.synced);
    cJSON_AddNumberToObject(resp, "lan_tokens", (double)job.lan_tokens);
    cJSON_AddNumberToObject(resp, "bindkeys", (double)job.bindkeys);
    cJSON_AddBoolToObject(resp, "reloaded", reload);
    miot_sync_job_add_home_echo(resp, &job);
    *out = resp;
    return ESP_OK;
}

static void homes_append_from_list(cJSON *homes_out, const cJSON *list)
{
    if (!homes_out || !cJSON_IsArray(list)) {
        return;
    }
    const cJSON *home = NULL;
    cJSON_ArrayForEach(home, list) {
        if (!cJSON_IsObject(home)) {
            continue;
        }
        const cJSON *id = cJSON_GetObjectItem(home, "id");
        const cJSON *name = cJSON_GetObjectItem(home, "name");
        char id_buf[64];
        id_buf[0] = '\0';
        if (cJSON_IsString(id)) {
            strncpy(id_buf, id->valuestring, sizeof(id_buf) - 1);
        } else if (cJSON_IsNumber(id)) {
            snprintf(id_buf, sizeof(id_buf), "%d", id->valueint);
        }
        if (!id_buf[0]) {
            continue;
        }

        size_t room_count = 0;
        size_t device_count = 0;
        const cJSON *rooms = cJSON_GetObjectItem(home, "roomlist");
        if (cJSON_IsArray(rooms)) {
            room_count = (size_t)cJSON_GetArraySize(rooms);
            const cJSON *room = NULL;
            cJSON_ArrayForEach(room, rooms) {
                const cJSON *dids = cJSON_GetObjectItem(room, "dids");
                if (cJSON_IsArray(dids)) {
                    device_count += (size_t)cJSON_GetArraySize(dids);
                }
            }
        }
        const cJSON *home_dids = cJSON_GetObjectItem(home, "dids");
        if (cJSON_IsArray(home_dids)) {
            device_count += (size_t)cJSON_GetArraySize(home_dids);
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "id", id_buf);
        cJSON_AddStringToObject(item, "name",
                                cJSON_IsString(name) && name->valuestring
                                    ? name->valuestring
                                    : id_buf);
        cJSON_AddNumberToObject(item, "room_count", (double)room_count);
        cJSON_AddNumberToObject(item, "device_count", (double)device_count);
        cJSON_AddItemToArray(homes_out, item);
    }
}

static esp_err_t rpc_homes(const cJSON *params, cJSON **out)
{
    (void)params;
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }

    if (!minihub_miot_auth_has_session()) {
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", "ESP_ERR_INVALID_STATE");
        cJSON_AddStringToObject(resp, "message", "未登录或登录已过期，请重新登录米家");
        cJSON_AddBoolToObject(resp, "need_login", true);
        *out = resp;
        return ESP_OK;
    }

    cJSON *result = NULL;
    esp_err_t err = minihub_miot_auth_homeroom_gethome(&result);
    if (err != ESP_OK || !result) {
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
        const char *msg = minihub_miot_auth_last_error();
        cJSON_AddStringToObject(resp, "message",
                                (msg && msg[0]) ? msg : "加载家庭失败，请重新登录米家");
        if (!minihub_miot_auth_has_session() ||
            (msg && strstr(msg, "过期"))) {
            cJSON_AddBoolToObject(resp, "need_login", true);
        }
        *out = resp;
        return ESP_OK;
    }

    cJSON *homes = cJSON_CreateArray();
    if (!homes) {
        cJSON_Delete(resp);
        cJSON_Delete(result);
        return ESP_ERR_NO_MEM;
    }
    homes_append_from_list(homes, cJSON_GetObjectItem(result, "homelist"));
    homes_append_from_list(homes, cJSON_GetObjectItem(result, "share_home_list"));
    cJSON_Delete(result);

    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddItemToObject(resp, "homes", homes);
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_devices(const cJSON *params, cJSON **out)
{
    (void)params;
    cJSON *list = NULL;
    esp_err_t err = minihub_miot_auth_device_list(&list);
    if (err != ESP_OK || !list) {
        return err != ESP_OK ? err : ESP_FAIL;
    }
    *out = list;
    return ESP_OK;
}

static esp_err_t rpc_prop_get(const cJSON *params, cJSON **out)
{
    const cJSON *did = params ? cJSON_GetObjectItem(params, "did") : NULL;
    const cJSON *siid = params ? cJSON_GetObjectItem(params, "siid") : NULL;
    const cJSON *piid = params ? cJSON_GetObjectItem(params, "piid") : NULL;
    if (!cJSON_IsString(did) || !cJSON_IsNumber(siid) || !cJSON_IsNumber(piid)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pairs = cJSON_CreateArray();
    cJSON *pair = cJSON_CreateObject();
    if (!pairs || !pair ||
        !cJSON_AddNumberToObject(pair, "siid", siid->valuedouble) ||
        !cJSON_AddNumberToObject(pair, "piid", piid->valuedouble) ||
        !cJSON_AddItemToArray(pairs, pair)) {
        cJSON_Delete(pair);
        cJSON_Delete(pairs);
        return ESP_ERR_NO_MEM;
    }

    cJSON *result = NULL;
    esp_err_t err = minihub_miot_auth_prop_get_multi(did->valuestring, pairs, &result);
    cJSON_Delete(pairs);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        cJSON_Delete(result);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddItemToObject(resp, "items", result);
    *out = resp;
    return ESP_OK;
}

/** Read-only siid×piid scan via batch prop/get. Caps keep HTTPS worker bounded. */
static esp_err_t rpc_prop_probe(const cJSON *params, cJSON **out)
{
    const cJSON *did = params ? cJSON_GetObjectItem(params, "did") : NULL;
    if (!cJSON_IsString(did) || !did->valuestring[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    int siid_min = 1;
    int siid_max = 12;
    int piid_min = 1;
    int piid_max = 8;
    int batch = 16;
    if (params) {
        const cJSON *v;
        if (cJSON_IsNumber((v = cJSON_GetObjectItem(params, "siid_min")))) {
            siid_min = v->valueint;
        }
        if (cJSON_IsNumber((v = cJSON_GetObjectItem(params, "siid_max")))) {
            siid_max = v->valueint;
        }
        if (cJSON_IsNumber((v = cJSON_GetObjectItem(params, "piid_min")))) {
            piid_min = v->valueint;
        }
        if (cJSON_IsNumber((v = cJSON_GetObjectItem(params, "piid_max")))) {
            piid_max = v->valueint;
        }
        if (cJSON_IsNumber((v = cJSON_GetObjectItem(params, "batch")))) {
            batch = v->valueint;
        }
    }
    if (siid_min < 1) {
        siid_min = 1;
    }
    if (piid_min < 1) {
        piid_min = 1;
    }
    if (siid_max > 32) {
        siid_max = 32;
    }
    if (piid_max > 16) {
        piid_max = 16;
    }
    if (siid_max < siid_min || piid_max < piid_min) {
        return ESP_ERR_INVALID_ARG;
    }
    if (batch < 1) {
        batch = 1;
    }
    if (batch > 24) {
        batch = 24;
    }

    cJSON *hits = cJSON_CreateArray();
    cJSON *codes = cJSON_CreateObject();
    cJSON *resp = cJSON_CreateObject();
    if (!hits || !codes || !resp) {
        cJSON_Delete(hits);
        cJSON_Delete(codes);
        cJSON_Delete(resp);
        return ESP_ERR_NO_MEM;
    }

    int probed = 0;
    int ok_n = 0;
    cJSON *pairs = cJSON_CreateArray();
    if (!pairs) {
        cJSON_Delete(hits);
        cJSON_Delete(codes);
        cJSON_Delete(resp);
        return ESP_ERR_NO_MEM;
    }

    for (int s = siid_min; s <= siid_max; s++) {
        for (int p = piid_min; p <= piid_max; p++) {
            cJSON *pair = cJSON_CreateObject();
            if (!pair || !cJSON_AddNumberToObject(pair, "siid", s) ||
                !cJSON_AddNumberToObject(pair, "piid", p) ||
                !cJSON_AddItemToArray(pairs, pair)) {
                cJSON_Delete(pair);
                cJSON_Delete(pairs);
                cJSON_Delete(hits);
                cJSON_Delete(codes);
                cJSON_Delete(resp);
                return ESP_ERR_NO_MEM;
            }
            probed++;

            bool flush = (cJSON_GetArraySize(pairs) >= batch) ||
                         (s == siid_max && p == piid_max);
            if (!flush) {
                continue;
            }

            cJSON *result = NULL;
            esp_err_t err = minihub_miot_auth_prop_get_multi(did->valuestring, pairs, &result);
            cJSON_Delete(pairs);
            pairs = cJSON_CreateArray();
            if (!pairs) {
                cJSON_Delete(result);
                cJSON_Delete(hits);
                cJSON_Delete(codes);
                cJSON_Delete(resp);
                return ESP_ERR_NO_MEM;
            }
            if (err != ESP_OK || !result) {
                cJSON_Delete(result);
                cJSON_Delete(pairs);
                cJSON_AddStringToObject(resp, "result", "fail");
                cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
                cJSON_AddNumberToObject(resp, "probed", probed);
                cJSON_AddItemToObject(resp, "hits", hits);
                cJSON_AddItemToObject(resp, "code_counts", codes);
                *out = resp;
                return ESP_OK;
            }

            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, result) {
                const cJSON *code = cJSON_GetObjectItem(item, "code");
                int c = cJSON_IsNumber(code) ? code->valueint : -1;
                char ck[16];
                snprintf(ck, sizeof(ck), "%d", c);
                cJSON *cnt = cJSON_GetObjectItem(codes, ck);
                int n = cJSON_IsNumber(cnt) ? (cnt->valueint + 1) : 1;
                cJSON_DeleteItemFromObject(codes, ck);
                cJSON_AddNumberToObject(codes, ck, n);
                if (c != 0) {
                    continue;
                }
                ok_n++;
                cJSON *hit = cJSON_Duplicate(item, true);
                if (hit) {
                    cJSON_AddItemToArray(hits, hit);
                }
            }
            cJSON_Delete(result);
        }
    }
    cJSON_Delete(pairs);

    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "did", did->valuestring);
    cJSON_AddNumberToObject(resp, "probed", probed);
    cJSON_AddNumberToObject(resp, "ok", ok_n);
    cJSON_AddItemToObject(resp, "hits", hits);
    cJSON_AddItemToObject(resp, "code_counts", codes);
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_login_retry(const cJSON *params, cJSON **out)
{
    const cJSON *username = params ? cJSON_GetObjectItem(params, "username") : NULL;
    const cJSON *password = params ? cJSON_GetObjectItem(params, "password") : NULL;
    if (!cJSON_IsString(username) || !cJSON_IsString(password)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *verify_url = calloc(1, MIOT_2FA_URL_MAX);
    if (!verify_url) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = minihub_miot_auth_login_retry(username->valuestring, password->valuestring,
                                                  verify_url, MIOT_2FA_URL_MAX);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        free(verify_url);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    if (err == ESP_OK) {
        auto_sync_to_response(resp);
    } else {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        if (const char *msg = minihub_miot_auth_last_error()) {
            cJSON_AddStringToObject(resp, "message", msg);
        }
        if (verify_url[0]) {
            cJSON_AddStringToObject(resp, "verify_url", verify_url);
        }
    }
    free(verify_url);
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_verify_send(const cJSON *params, cJSON **out)
{
    const char *icode = NULL;
    char icode_buf[16] = {0};
    if (params) {
        const cJSON *ic = cJSON_GetObjectItem(params, "icode");
        if (cJSON_IsString(ic) && ic->valuestring[0]) {
            strncpy(icode_buf, ic->valuestring, sizeof(icode_buf) - 1);
            icode = icode_buf;
        }
    }

    esp_err_t err = minihub_miot_auth_send_2fa_email(icode);
    const char *captcha_b64 = miot_cloud_last_2fa_captcha_b64();
    const char *icode_sent = miot_cloud_last_2fa_icode_sent();

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    if (icode_sent && icode_sent[0]) {
        cJSON_AddStringToObject(resp, "icode_used", icode_sent);
    }
    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "result", "ok");
        if (minihub_miot_auth_has_session()) {
            cJSON_AddStringToObject(resp, "message", "logged in");
            auto_sync_to_response(resp);
        } else {
            cJSON_AddStringToObject(resp, "message", "SMS verification code sent");
        }
    } else if (err == ESP_ERR_INVALID_STATE) {
        /* Prefer captcha over pending verify_url — sendTicket often leaves both set. */
        if (captcha_b64 && captcha_b64[0]) {
            cJSON_AddStringToObject(resp, "result", "captcha");
            cJSON_AddStringToObject(resp, "captcha_b64", captcha_b64);
            if (const char *mime = miot_cloud_last_2fa_captcha_mime()) {
                cJSON_AddStringToObject(resp, "captcha_mime", mime);
            }
            if (const char *msg = minihub_miot_auth_last_error()) {
                cJSON_AddStringToObject(resp, "message", msg);
            } else {
                cJSON_AddStringToObject(resp, "message", "请输入图形验证码后再次发送");
            }
            cJSON_AddStringToObject(resp, "hint", "请输入图形验证码后再次点击发送");
        } else {
            const char *vu = miot_cloud_last_verify_url();
            if (vu && vu[0]) {
                cJSON_AddStringToObject(resp, "result", "verify");
                cJSON_AddStringToObject(resp, "verify_url", vu);
                cJSON_AddStringToObject(resp, "message",
                                        "需要二次验证，请先发送验证码，再填写短信码登录");
            } else {
                cJSON_AddStringToObject(resp, "result", "fail");
                cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
                if (const char *msg = minihub_miot_auth_last_error()) {
                    cJSON_AddStringToObject(resp, "message", msg);
                }
            }
        }
    } else {
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        if (const char *msg = minihub_miot_auth_last_error()) {
            cJSON_AddStringToObject(resp, "message", msg);
        }
    }
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_verify(const cJSON *params, cJSON **out)
{
    const cJSON *ticket = params ? cJSON_GetObjectItem(params, "ticket") : NULL;
    if (!cJSON_IsString(ticket)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = minihub_miot_auth_verify_2fa(ticket->valuestring);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    if (err == ESP_OK) {
        auto_sync_to_response(resp);
    } else {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        if (const char *msg = minihub_miot_auth_last_error()) {
            cJSON_AddStringToObject(resp, "message", msg);
        }
    }
    *out = resp;
    return ESP_OK;
}

typedef struct {
    const char *key;
    const char *type; /* prop | event */
} device_log_key_t;

static size_t device_log_candidates(const char *model, const char *dtype, const char *did,
                                    device_log_key_t *out, size_t max_out, const char **out_kind)
{
    size_t n = 0;
    if (!out || max_out == 0 || !out_kind) {
        return 0;
    }
    *out_kind = "generic";
#define ADD_KEY(k, t) do { if (n < max_out) { out[n].key = (k); out[n].type = (t); n++; } } while (0)

    bool ble = did && strncmp(did, "blt.", 4) == 0;
    if (model && strstr(model, "motion")) {
        *out_kind = "motion";
        /* Spec: Motion Detected event siid=2 eiid=1008 */
        ADD_KEY("2.1008", "event");
        ADD_KEY("2.1", "event");
        if (ble) {
            /* MiBeacon object ids (decimal): motion / motion-with-light */
            ADD_KEY("15", "event");
            ADD_KEY("15", "prop");
            ADD_KEY("3", "event");
            ADD_KEY("1008", "event");
        }
    } else if (model && (strstr(model, "magnet") || strstr(model, "sensor_magnet"))) {
        *out_kind = "magnet";
        /* Lab isa.magnet.dw2hl stores history under BLE object 4120. */
        if (ble) {
            ADD_KEY("4120", "prop");
            ADD_KEY("4119", "prop");
            ADD_KEY("7", "prop");
            ADD_KEY("7", "event");
            ADD_KEY("1006", "prop");
            ADD_KEY("1006", "event");
        }
        ADD_KEY("2.2", "prop");
        ADD_KEY("2.2", "event");
        ADD_KEY("2.1", "prop");
        ADD_KEY("2.1", "event");
    } else if ((dtype && strcmp(dtype, "button") == 0) || (model && strstr(model, "remote"))) {
        *out_kind = "button";
        /* Spec: Click / Double / Long Press */
        ADD_KEY("3.1012", "event");
        ADD_KEY("3.1013", "event");
        ADD_KEY("3.1014", "event");
        ADD_KEY("3.1", "event");
        if (ble) {
            ADD_KEY("4097", "event"); /* 0x1001 button */
            ADD_KEY("4097", "prop");
            ADD_KEY("1001", "event");
        }
    } else if (dtype && strcmp(dtype, "binary_sensor") == 0) {
        *out_kind = "motion";
        ADD_KEY("2.1008", "event");
        ADD_KEY("2.1", "event");
    }
#undef ADD_KEY
    return n;
}

static size_t device_log_result_count(const cJSON *result)
{
    if (!result) {
        return 0;
    }
    if (cJSON_IsArray(result)) {
        return (size_t)cJSON_GetArraySize(result);
    }
    if (cJSON_IsObject(result)) {
        const cJSON *inner = cJSON_GetObjectItem(result, "data");
        if (!inner) {
            inner = cJSON_GetObjectItem(result, "list");
        }
        if (cJSON_IsArray(inner)) {
            return (size_t)cJSON_GetArraySize(inner);
        }
    }
    return 0;
}

static const char *device_log_label(const char *kind, const cJSON *value)
{
    if (kind && strcmp(kind, "motion") == 0) {
        return "有人移动";
    }
    if (kind && strcmp(kind, "magnet") == 0) {
        bool closed = false;
        if (cJSON_IsBool(value)) {
            closed = cJSON_IsTrue(value);
        } else if (cJSON_IsNumber(value)) {
            closed = value->valueint != 0;
        } else if (cJSON_IsString(value)) {
            const char *s = value->valuestring;
            closed = (strcmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
                      strcmp(s, "01") == 0 || strcmp(s, "closed") == 0);
        } else if (cJSON_IsArray(value) && cJSON_GetArraySize(value) > 0) {
            const cJSON *first = cJSON_GetArrayItem(value, 0);
            if (cJSON_IsString(first)) {
                const char *s = first->valuestring;
                closed = (strcmp(s, "1") == 0 || strcmp(s, "01") == 0 ||
                          strcmp(s, "true") == 0);
            } else if (cJSON_IsNumber(first)) {
                closed = first->valueint != 0;
            }
        }
        return closed ? "关闭" : "打开";
    }
    if (kind && strcmp(kind, "button") == 0) {
        if (cJSON_IsNumber(value)) {
            int v = value->valueint;
            if (v == 1) {
                return "双击";
            }
            if (v == 2) {
                return "长按";
            }
        }
        return "单击";
    }
    return "事件";
}

static void device_log_append_cloud_items(cJSON *items, const cJSON *result, const char *kind)
{
    if (!items || !result) {
        return;
    }
    const cJSON *arr = result;
    if (cJSON_IsObject(result)) {
        const cJSON *inner = cJSON_GetObjectItem(result, "data");
        if (!inner) {
            inner = cJSON_GetObjectItem(result, "list");
        }
        if (cJSON_IsArray(inner)) {
            arr = inner;
        }
    }
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, arr) {
        if (!cJSON_IsObject(row)) {
            continue;
        }
        const cJSON *t = cJSON_GetObjectItem(row, "time");
        if (!t) {
            t = cJSON_GetObjectItem(row, "timestamp");
        }
        if (!t) {
            t = cJSON_GetObjectItem(row, "t");
        }
        int64_t ts_ms = 0;
        if (cJSON_IsNumber(t)) {
            int64_t tv = (int64_t)t->valueint;
            if (tv == 0 && t->valuedouble != 0.0) {
                tv = (int64_t)t->valuedouble;
            }
            if (tv > 0 && tv < 100000000000LL) {
                tv = tv * 1000LL;
            }
            ts_ms = tv;
        } else if (cJSON_IsString(t) && t->valuestring[0]) {
            /* Parse decimal seconds/ms without atoll (not in host ELF exports). */
            int64_t tv = 0;
            const char *s = t->valuestring;
            while (*s >= '0' && *s <= '9') {
                tv = tv * 10 + (*s - '0');
                s++;
            }
            if (tv > 0 && tv < 100000000000LL) {
                tv *= 1000LL;
            }
            ts_ms = tv;
        }
        const cJSON *val = cJSON_GetObjectItem(row, "value");
        if (!val) {
            val = cJSON_GetObjectItem(row, "v");
        }
        if (!val) {
            val = cJSON_GetObjectItem(row, "data");
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddNumberToObject(item, "ts_ms", (double)ts_ms);
        cJSON_AddStringToObject(item, "label", device_log_label(kind, val));
        cJSON_AddStringToObject(item, "source", "cloud");
        if (val) {
            char *printed = cJSON_PrintUnformatted(val);
            if (printed) {
                cJSON_AddStringToObject(item, "raw", printed);
                free(printed);
            }
        }
        cJSON_AddItemToArray(items, item);
    }
}

static esp_err_t rpc_device_log(const cJSON *params, cJSON **out)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    cJSON *tried = cJSON_CreateArray();
    if (!resp || !items || !tried) {
        cJSON_Delete(resp);
        cJSON_Delete(items);
        cJSON_Delete(tried);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(resp, "items", items);
    cJSON_AddItemToObject(resp, "tried", tried);

    if (!minihub_miot_auth_has_session()) {
        cJSON_AddStringToObject(resp, "cloud_error", "not_logged_in");
        *out = resp;
        return ESP_OK;
    }

    const cJSON *pid = params ? cJSON_GetObjectItem(params, "point_id") : NULL;
    if (!cJSON_IsString(pid) || !pid->valuestring[0]) {
        cJSON_AddStringToObject(resp, "cloud_error", "missing_point_id");
        *out = resp;
        return ESP_OK;
    }
    int limit = 20;
    const cJSON *lim = cJSON_GetObjectItem(params, "limit");
    if (cJSON_IsNumber(lim) && lim->valueint > 0) {
        limit = lim->valueint;
        if (limit > 50) {
            limit = 50;
        }
    }

    const char *did = NULL;
    const char *model = NULL;
    const char *dtype = NULL;
    const minihub_module_host_api_t *host = minihub_miot_module_host();
    if (host && host->config_get_adapters) {
        cJSON *root = host->config_get_adapters();
        const cJSON *adapters = root ? cJSON_GetObjectItem(root, "adapters") : NULL;
        const cJSON *miot = adapters ? cJSON_GetObjectItem(adapters, "miot") : NULL;
        const cJSON *devices = miot ? cJSON_GetObjectItem(miot, "devices") : NULL;
        if (cJSON_IsArray(devices)) {
            const cJSON *dev = NULL;
            cJSON_ArrayForEach(dev, devices) {
                const cJSON *p = cJSON_GetObjectItem(dev, "point_id");
                if (cJSON_IsString(p) && strcmp(p->valuestring, pid->valuestring) == 0) {
                    const cJSON *d = cJSON_GetObjectItem(dev, "did");
                    const cJSON *m = cJSON_GetObjectItem(dev, "model");
                    const cJSON *t = cJSON_GetObjectItem(dev, "type");
                    if (cJSON_IsString(d)) {
                        did = d->valuestring;
                    }
                    if (cJSON_IsString(m)) {
                        model = m->valuestring;
                    }
                    if (cJSON_IsString(t)) {
                        dtype = t->valuestring;
                    }
                    break;
                }
            }
        }
    }
    if (!did || !did[0]) {
        cJSON_AddStringToObject(resp, "cloud_error", "device_not_found");
        *out = resp;
        return ESP_OK;
    }

    const char *kind = "generic";
    device_log_key_t cands[16];
    size_t ncand = 0;
    const cJSON *ov_key = cJSON_GetObjectItem(params, "key");
    const cJSON *ov_type = cJSON_GetObjectItem(params, "type");
    if (cJSON_IsString(ov_key) && ov_key->valuestring[0] &&
        cJSON_IsString(ov_type) && ov_type->valuestring[0]) {
        cands[0].key = ov_key->valuestring;
        cands[0].type = ov_type->valuestring;
        ncand = 1;
        if (model && strstr(model, "motion")) {
            kind = "motion";
        } else if (model && strstr(model, "magnet")) {
            kind = "magnet";
        } else if (dtype && strcmp(dtype, "button") == 0) {
            kind = "button";
        }
    } else {
        ncand = device_log_candidates(model, dtype, did, cands, 16, &kind);
    }
    if (ncand == 0) {
        cJSON_AddStringToObject(resp, "cloud_error", "no_key_mapping");
        *out = resp;
        return ESP_OK;
    }

    const char *used_key = NULL;
    const char *used_type = NULL;
    esp_err_t last_err = ESP_FAIL;
    for (size_t i = 0; i < ncand; i++) {
        cJSON *result = NULL;
        esp_err_t err = minihub_miot_auth_get_user_device_data(did, cands[i].key, cands[i].type,
                                                               limit, &result);
        cJSON *tr = cJSON_CreateObject();
        if (tr) {
            cJSON_AddStringToObject(tr, "key", cands[i].key);
            cJSON_AddStringToObject(tr, "type", cands[i].type);
            cJSON_AddStringToObject(tr, "err", err == ESP_OK ? "ok" : esp_err_to_name(err));
            cJSON_AddNumberToObject(tr, "n", (double)device_log_result_count(result));
            cJSON_AddItemToArray(tried, tr);
        }
        if (err != ESP_OK) {
            last_err = err;
            continue;
        }
        size_t n = device_log_result_count(result);
        if (n == 0) {
            cJSON_Delete(result);
            continue;
        }
        device_log_append_cloud_items(items, result, kind);
        used_key = cands[i].key;
        used_type = cands[i].type;
        cJSON_Delete(result);
        break;
    }

    if (used_key) {
        cJSON_AddStringToObject(resp, "key", used_key);
        cJSON_AddStringToObject(resp, "type", used_type);
    } else {
        /* Fallback: chronological log without key (helps some BLE magnets). */
        cJSON *log_result = NULL;
        esp_err_t log_err = minihub_miot_auth_get_user_device_log(did, limit, &log_result);
        cJSON *tr = cJSON_CreateObject();
        if (tr) {
            cJSON_AddStringToObject(tr, "key", "v2_log");
            cJSON_AddStringToObject(tr, "type", "log");
            cJSON_AddStringToObject(tr, "err", log_err == ESP_OK ? "ok" : esp_err_to_name(log_err));
            cJSON_AddNumberToObject(tr, "n", (double)device_log_result_count(log_result));
            cJSON_AddItemToArray(tried, tr);
        }
        if (log_err == ESP_OK && device_log_result_count(log_result) > 0) {
            device_log_append_cloud_items(items, log_result, kind);
            cJSON_AddStringToObject(resp, "key", "v2_log");
            cJSON_AddStringToObject(resp, "type", "log");
            used_key = "v2_log";
        }
        cJSON_Delete(log_result);
    }
    if (!used_key && cJSON_GetArraySize(items) == 0) {
        cJSON_AddStringToObject(resp, "cloud_error",
                                last_err != ESP_OK ? esp_err_to_name(last_err) : "empty");
        const char *msg = minihub_miot_auth_last_error();
        if (msg && msg[0]) {
            cJSON_AddStringToObject(resp, "message", msg);
        }
    }
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_camera_preview(const cJSON *params, cJSON **out)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }

    if (!minihub_miot_auth_has_session()) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "not_logged_in");
        *out = resp;
        return ESP_OK;
    }

    const cJSON *pid = params ? cJSON_GetObjectItem(params, "point_id") : NULL;
    if (!cJSON_IsString(pid) || !pid->valuestring[0]) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "missing_point_id");
        *out = resp;
        return ESP_OK;
    }

    const char *did = NULL;
    const char *model = NULL;
    const minihub_module_host_api_t *host = minihub_miot_module_host();
    if (host && host->config_get_adapters) {
        cJSON *root = host->config_get_adapters();
        const cJSON *adapters = root ? cJSON_GetObjectItem(root, "adapters") : NULL;
        const cJSON *miot = adapters ? cJSON_GetObjectItem(adapters, "miot") : NULL;
        const cJSON *devices = miot ? cJSON_GetObjectItem(miot, "devices") : NULL;
        if (cJSON_IsArray(devices)) {
            const cJSON *dev = NULL;
            cJSON_ArrayForEach(dev, devices) {
                const cJSON *p = cJSON_GetObjectItem(dev, "point_id");
                if (cJSON_IsString(p) && strcmp(p->valuestring, pid->valuestring) == 0) {
                    const cJSON *d = cJSON_GetObjectItem(dev, "did");
                    const cJSON *m = cJSON_GetObjectItem(dev, "model");
                    if (cJSON_IsString(d)) {
                        did = d->valuestring;
                    }
                    if (cJSON_IsString(m)) {
                        model = m->valuestring;
                    }
                    break;
                }
            }
        }
    }
    if (!did || !did[0]) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "device_not_found");
        *out = resp;
        return ESP_OK;
    }

    miot_camera_preview_info_t info;
    memset(&info, 0, sizeof(info));
    esp_err_t err = minihub_miot_auth_camera_preview(did, model, MIOT_CAM_PREVIEW_PATH, &info);
    if (err == ESP_ERR_NOT_FOUND) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddBoolToObject(resp, "empty", true);
        *out = resp;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        const char *msg = minihub_miot_auth_last_error();
        if (msg && msg[0]) {
            cJSON_AddStringToObject(resp, "message", msg);
        }
        *out = resp;
        return ESP_OK;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "path", MIOT_CAM_PREVIEW_PATH);
    cJSON_AddNumberToObject(resp, "bytes", (double)info.jpeg_bytes);
    if (info.event_type[0]) {
        cJSON_AddStringToObject(resp, "event_type", info.event_type);
    }
    *out = resp;
    return ESP_OK;
}

__attribute__((visibility("default")))
esp_err_t miot_module_rpc(const char *method, const cJSON *params, cJSON **out)
{
    if (!method || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (strcmp(method, "status") == 0) {
        return rpc_status(params, out);
    }
    if (strcmp(method, "login") == 0) {
        return rpc_login(params, out);
    }
    if (strcmp(method, "session") == 0) {
        return rpc_session(params, out);
    }
    if (strcmp(method, "logout") == 0) {
        return rpc_logout(params, out);
    }
    if (strcmp(method, "sync") == 0) {
        return rpc_sync(params, out);
    }
    if (strcmp(method, "homes") == 0) {
        return rpc_homes(params, out);
    }
    if (strcmp(method, "devices") == 0) {
        return rpc_devices(params, out);
    }
    if (strcmp(method, "prop_get") == 0) {
        return rpc_prop_get(params, out);
    }
    if (strcmp(method, "prop_probe") == 0) {
        return rpc_prop_probe(params, out);
    }
    if (strcmp(method, "device_log") == 0) {
        return rpc_device_log(params, out);
    }
    if (strcmp(method, "camera_preview") == 0) {
        return rpc_camera_preview(params, out);
    }
    if (strcmp(method, "login_retry") == 0) {
        return rpc_login_retry(params, out);
    }
    if (strcmp(method, "verify_send") == 0) {
        return rpc_verify_send(params, out);
    }
    if (strcmp(method, "verify") == 0) {
        return rpc_verify(params, out);
    }

    return ESP_ERR_NOT_FOUND;
}

/* ABI export for .mhub dlsym(MINIHUB_MODULE_RPC_SYMBOL). Host register uses miot_module_rpc. */
__attribute__((visibility("default")))
esp_err_t minihub_module_rpc(const char *method, const cJSON *params, cJSON **out)
{
    return miot_module_rpc(method, params, out);
}
