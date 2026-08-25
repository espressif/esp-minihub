/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal hooks between RTSP recorder (upstream) and RTSP relay server.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t minihub_rtsp_cam_ensure_upstream(void);
void minihub_rtsp_cam_viewer_attach(int sock);
void minihub_rtsp_cam_viewer_detach(int sock);
bool minihub_rtsp_cam_viewer_busy(void);
bool minihub_rtsp_cam_upstream_streaming(void);
const char *minihub_rtsp_cam_cached_sdp(void);
const char *minihub_rtsp_cam_id(void);
int minihub_rtsp_cam_relay_port(void);

esp_err_t minihub_rtsp_cam_relay_start(void);

#ifdef __cplusplus
}
#endif
