/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t minihub_rtsp_cam_init(void);

esp_err_t minihub_rtsp_cam_config_set(const char *url, const char *cam_id, int segment_sec);
void minihub_rtsp_cam_config_get(char *url, size_t url_len, char *cam_id, size_t cam_len,
                                 int *segment_sec);

esp_err_t minihub_rtsp_cam_start(void);
esp_err_t minihub_rtsp_cam_stop(void);

bool minihub_rtsp_cam_recording(void);
const char *minihub_rtsp_cam_last_error(void);

/**
 * @brief Caller owns returned object.
 */
cJSON *minihub_rtsp_cam_status_json(void);
cJSON *minihub_rtsp_cam_list_files_json(int limit);
/**
 * @brief Params: JSON array of basenames. Caller owns returned object.
 */
cJSON *minihub_rtsp_cam_delete_files_json(const cJSON *names);

/**
 * @brief Resolve a safe basename under the cam record dir into an absolute path.
 */
esp_err_t minihub_rtsp_cam_resolve_file(const char *name, char *path_out, size_t path_len);

esp_err_t minihub_rtsp_cam_rpc(const char *method, const cJSON *params, cJSON **out);
esp_err_t minihub_rtsp_cam_register_rpc(void);
/**
 * @brief Register GET download URI on the Host httpd (files under cam record dir).
 */
esp_err_t minihub_rtsp_cam_register_http(void *httpd_handle);

#ifdef __cplusplus
}
#endif
