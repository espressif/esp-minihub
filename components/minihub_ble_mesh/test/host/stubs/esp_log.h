/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host-test stand-in for esp_log.h. Logs go to stderr only when
 * MINIHUB_HOST_TEST_LOG is set, so a passing run stays quiet. */

#pragma once

void minihub_host_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) minihub_host_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) minihub_host_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) minihub_host_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) minihub_host_log('D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) minihub_host_log('V', tag, fmt, ##__VA_ARGS__)
