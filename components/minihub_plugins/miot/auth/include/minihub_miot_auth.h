/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include "miot_cloud.h"

esp_err_t minihub_miot_auth_init(void);
bool minihub_miot_auth_has_session(void);
const miot_cloud_session_t *minihub_miot_auth_session(void);

/** 小米账号密码登录（可能返回 ESP_ERR_INVALID_STATE + verify_url 需二次验证） */
esp_err_t minihub_miot_auth_login(const char *username, const char *password,
                                  char *verify_url, size_t verify_url_len);

/** 导入已有云会话（PC 上 miservice 等工具获取） */
esp_err_t minihub_miot_auth_import_session(const char *user_id, const char *service_token,
                                           const char *ssecurity, const char *device_id,
                                           const char *region);

esp_err_t minihub_miot_auth_logout(void);

/** 拉取米家设备列表（调用方 cJSON_Delete） */
esp_err_t minihub_miot_auth_device_list(cJSON **out_list);

/**
 * Homes + rooms (caller cJSON_Delete).
 * Object is cloud `result` from /v2/homeroom/gethome.
 */
esp_err_t minihub_miot_auth_homeroom_gethome(cJSON **out_result);

/** 云 miotspec 属性读/写（在 HTTPS worker 任务中执行，勿直接调 miot_cloud_prop_*） */
esp_err_t minihub_miot_auth_prop_get(const char *did, int siid, int piid, cJSON **out_value);
/** Batch prop get; `pairs` = [{siid,piid},...]; `*out_result` = cloud result array. */
esp_err_t minihub_miot_auth_prop_get_multi(const char *did, const cJSON *pairs, cJSON **out_result);
esp_err_t minihub_miot_auth_prop_set(const char *did, int siid, int piid, const cJSON *value);
esp_err_t minihub_miot_auth_action(const char *did, int siid, int aiid, const cJSON *in);

/**
 * Cloud device history (/user/get_user_device_data). Caller cJSON_Delete(*out_result).
 */
esp_err_t minihub_miot_auth_get_user_device_data(const char *did, const char *key, const char *type,
                                                 int limit, cJSON **out_result);

/** Chronological log without key (/v2/user/get_user_device_log). Caller cJSON_Delete. */
esp_err_t minihub_miot_auth_get_user_device_log(const char *did, int limit, cJSON **out_result);

/**
 * Latest camera event JPEG to `out_path` (default MIOT_CAM_PREVIEW_PATH).
 * ESP_ERR_NOT_FOUND = no cloud events.
 */
esp_err_t minihub_miot_auth_camera_preview(const char *did, const char *model,
                                           const char *out_path, miot_camera_preview_info_t *info);

/**
 * For each cloud device with did "blt.*" and a MAC, fetch beaconkey into bindkeys_obj
 * (keys = normalized MAC, values = hex bindkey). Runs on HTTPS worker.
 */
esp_err_t minihub_miot_auth_fetch_ble_bindkeys(const cJSON *cloud_list, cJSON *bindkeys_obj,
                                               size_t *ok_count, size_t *fail_count);

/** 从云端同步设备到 adapters.json（merge 保留已有 siid/piid；写入 token/mac/ip 供 LAN）。
 * home_id_opt: selected Mi Home id; NULL uses saved adapters.miot.home_id.
 * Returns ESP_ERR_INVALID_STATE when no home selected (RPC maps to need_home).
 */
esp_err_t minihub_miot_sync_devices(bool merge_existing, const char *home_id_opt,
                                  size_t *synced_count, size_t *lan_token_count,
                                  size_t *bindkey_count);

/** 清空 adapters.json 中 miot.devices[]（退出账号时调用） */
esp_err_t minihub_miot_devices_clear(size_t *removed_count);

/** 向绑定邮箱/手机发送 2FA 验证码；icode 为图形验证码（小米要求时） */
esp_err_t minihub_miot_auth_send_2fa_email(const char *icode);

/** 提交 2FA 验证码（需先 send） */
esp_err_t minihub_miot_auth_verify_2fa(const char *ticket);

/** 浏览器完成 2FA 后重试登录 */
esp_err_t minihub_miot_auth_login_retry(const char *username, const char *password,
                                        char *verify_url, size_t verify_url_len);

/** 会话状态 JSON（调用方 cJSON_Delete） */
cJSON *minihub_miot_auth_status_json(void);

/** 最近一次登录失败原因（可能为空） */
const char *minihub_miot_auth_last_error(void);
