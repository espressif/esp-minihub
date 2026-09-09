/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#define MIOT_CLOUD_USER_ID_MAX       32
#define MIOT_CLOUD_TOKEN_MAX         256
#define MIOT_CLOUD_SSECURITY_MAX     128
#define MIOT_CLOUD_DEVICE_ID_MAX     32
#define MIOT_CLOUD_REGION_MAX        8

/** Xiaomi 2FA authStart URL (context token can exceed 500 chars) */
#define MIOT_2FA_URL_MAX             1536
#define MIOT_2FA_REQ_URL_MAX         (MIOT_2FA_URL_MAX + 128)

/** Passport login (MiService / Mi Home iOS SDK alignment) */
#define MIOT_PASSPORT_SDK_VERSION    "3.9"
#define MIOT_PASSPORT_UA \
    "APP/com.xiaomi.mihome APPV/11.3.203 iosPassportSDK/4.2.50 iOS/26.3.1 " \
    "MK/aVBob25lMTcsMg== DEVT/aVBob25l DEVS/aU9T BRA/QXBwbGU= L/zh_CN"

typedef struct {
    char user_id[MIOT_CLOUD_USER_ID_MAX];
    char service_token[MIOT_CLOUD_TOKEN_MAX];
    char ssecurity[MIOT_CLOUD_SSECURITY_MAX];
    char device_id[MIOT_CLOUD_DEVICE_ID_MAX];
    char region[MIOT_CLOUD_REGION_MAX];
    bool valid;
    /** true when device_id came from login or MiService import (safe for PassportDeviceId cookie) */
    bool device_id_pinned;
} miot_cloud_session_t;

typedef struct {
    char nonce[64];
    char signature[128];
} miot_cloud_sign_t;

esp_err_t miot_cloud_session_load(miot_cloud_session_t *session);
esp_err_t miot_cloud_session_save(const miot_cloud_session_t *session);
/** Fill missing device_id before save or cloud API use */
esp_err_t miot_cloud_session_finalize(miot_cloud_session_t *session);
void miot_cloud_session_clear(miot_cloud_session_t *session);

/** Clear cloud credentials without wiping pending 2FA / device maps. */
void miot_cloud_session_invalidate(miot_cloud_session_t *session);

bool miot_cloud_session_valid(const miot_cloud_session_t *session);

esp_err_t miot_cloud_gen_nonce(char *out, size_t out_len);
esp_err_t miot_cloud_sign_request(const char *uri, const char *data_json,
                                  const char *ssecurity, miot_cloud_sign_t *sign);

/** capt_code: login-step captcha (Xiaomi field captCode); NULL on first attempt. */
esp_err_t miot_cloud_login(const char *username, const char *password, const char *capt_code,
                           miot_cloud_session_t *session, char *verify_url, size_t verify_url_len);

/** True when login step2 asked for a captcha and credentials are held in RAM. */
bool miot_cloud_has_pending_login_captcha(void);
void miot_cloud_clear_pending_login_captcha(void);

/** Retry serviceLoginAuth2 with captCode after miot_cloud_login returned captcha. */
esp_err_t miot_cloud_login_captcha_retry(const char *capt_code, miot_cloud_session_t *session,
                                         char *verify_url, size_t verify_url_len);

/** GET account.xiaomi.com captcha path; stores JPEG/PNG in last_2fa_captcha_b64(). */
esp_err_t miot_cloud_fetch_passport_captcha(miot_cloud_session_t *session, const char *captcha_path,
                                            char *jar, size_t jar_len);

esp_err_t miot_cloud_request(const miot_cloud_session_t *session, const char *path,
                             const char *data_json, cJSON **out_resp);

/**
 * RC4-encrypted cloud API (MIOT-ENCRYPT-ALGORITHM: ENCRYPT-RC4).
 * Response is decrypted JSON (caller cJSON_Delete).
 */
esp_err_t miot_cloud_request_rc4(const miot_cloud_session_t *session, const char *path,
                                 const char *data_json, cJSON **out_resp);

esp_err_t miot_cloud_device_list(const miot_cloud_session_t *session, cJSON **out_list);

/**
 * Homes + rooms (POST /v2/homeroom/gethome).
 * On success *out_result is the cloud `result` object (caller cJSON_Delete).
 */
esp_err_t miot_cloud_homeroom_gethome(const miot_cloud_session_t *session, cJSON **out_result);

/**
 * BLE bindkey for did starting with "blt." → out_key (hex string, typically 32 chars).
 * Uses /v2/device/blt_get_beaconkey (RC4).
 */
esp_err_t miot_cloud_blt_get_beaconkey(const miot_cloud_session_t *session, const char *did,
                                       char *out_key, size_t out_len);
esp_err_t miot_cloud_prop_get(const miot_cloud_session_t *session,
                              const char *did, int siid, int piid, cJSON **out_value);
/**
 * Batch /miotspec/prop/get. `pairs` is JSON array of objects with siid/piid
 * (and optional did; missing did uses `did`). On success `*out_result` is the
 * raw `result` array (caller deletes); each item has code and optional value.
 */
esp_err_t miot_cloud_prop_get_multi(const miot_cloud_session_t *session, const char *did,
                                    const cJSON *pairs, cJSON **out_result);
esp_err_t miot_cloud_prop_set(const miot_cloud_session_t *session,
                              const char *did, int siid, int piid, cJSON *value);
esp_err_t miot_cloud_action(const miot_cloud_session_t *session,
                            const char *did, int siid, int aiid,
                            const cJSON *in_args);

/**
 * Device property/event history: POST /user/get_user_device_data.
 * On success *out_result is cloud `result` (array or object); caller cJSON_Delete.
 */
esp_err_t miot_cloud_get_user_device_data(const miot_cloud_session_t *session,
                                          const char *did, const char *key, const char *type,
                                          int limit, cJSON **out_result);

/**
 * Chronological device log: POST /v2/user/get_user_device_log (no key).
 * On success *out_result is cloud `result`; caller cJSON_Delete.
 */
esp_err_t miot_cloud_get_user_device_log(const miot_cloud_session_t *session,
                                         const char *did, int limit, cJSON **out_result);

#define MIOT_CAM_PREVIEW_PATH "/data/tmp/miot_cam_preview.jpg"

typedef struct {
    char event_type[48];
    char create_time[24];
    size_t jpeg_bytes;
} miot_camera_preview_info_t;

/**
 * Latest 看家助手 event JPEG (HA still-frame). Writes `out_path`.
 * ESP_ERR_NOT_FOUND = no events (empty preview).
 */
esp_err_t miot_cloud_camera_preview_jpeg(const miot_cloud_session_t *session,
                                         const char *did, const char *model,
                                         const char *out_path,
                                         miot_camera_preview_info_t *info);

/** 保存待完成的 2FA URL 及登录 cookie jar（浏览器无法直接打开 verify_url） */
esp_err_t miot_cloud_save_pending_2fa(const char *notification_url, const char *cookie_jar);
/** Save serviceLogin qs/_sign for 2FA result/check (NVS, cleared with pending 2FA). */
esp_err_t miot_cloud_save_pending_login_params(const char *qs, const char *sign);

/** 清除 NVS 中未完成的 2FA 状态（session 导入/登录成功后调用） */
void miot_cloud_clear_pending_2fa(void);

/** True when NVS still has a pending 2FA notification URL from the last login. */
bool miot_cloud_has_pending_2fa(void);

/**
 * Last 2FA notification URL from this login attempt (RAM).
 * Survives http_worker task hand-off even if the caller buffer was not filled.
 */
const char *miot_cloud_last_verify_url(void);

/** 最近一次云登录/API 失败的人类可读原因（线程内静态缓冲） */
const char *miot_cloud_last_error(void);
void miot_cloud_set_last_error(const char *message);

/** 向绑定邮箱/手机发送 2FA 验证码（需先 login 返回 verify）；icode 为图形验证码（可选） */
esp_err_t miot_cloud_send_2fa_email(miot_cloud_session_t *session, const char *icode);

/** send 返回需要图形验证码时，最近一次 captcha 图片的 base64（JPEG/PNG） */
const char *miot_cloud_last_2fa_captcha_b64(void);
const char *miot_cloud_last_2fa_captcha_mime(void);
/** 最近一次提交的图形验证码（经清洗后，供调试核对） */
const char *miot_cloud_last_2fa_icode_sent(void);

/** Decode last captcha JPEG/PNG to a file (e.g. /data/captcha.jpg for LCD lv_img). */
esp_err_t miot_cloud_last_2fa_captcha_save_file(const char *path);

/** 提交邮件验证码完成 2FA 登录（需先 send） */
esp_err_t miot_cloud_verify_2fa_ticket(const char *ticket, miot_cloud_session_t *session);

/** 2FA 浏览器验证后重试登录（同账号密码） */
esp_err_t miot_cloud_login_retry(const char *username, const char *password,
                                 miot_cloud_session_t *session);
