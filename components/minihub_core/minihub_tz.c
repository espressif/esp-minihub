/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_tz.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "minihub_tz";
static const char *NVS_NS = "minihub";
static const char *NVS_KEY = "tz";

/* Whole hours only, from 西十二区 to 东十四区. No place names.
 * POSIX offset sign is inverted: UTC-8 means 8 hours east of UTC. */
#define TZ_HOUR_MIN (-12)
#define TZ_HOUR_MAX 14

static char s_id[16] = "UTC";
static bool s_ready;

static bool hour_from_id(const char *id, int *hour_out)
{
    if (!id || !hour_out) {
        return false;
    }
    if (strcmp(id, "UTC") == 0) {
        *hour_out = 0;
        return true;
    }
    int sign = 0;
    if (strncmp(id, "UTC+", 4) == 0) {
        sign = 1;
    } else if (strncmp(id, "UTC-", 4) == 0) {
        sign = -1;
    } else {
        return false;
    }
    const char *num = id + 4;
    if (num[0] < '1' || num[0] > '9') {
        return false;
    }
    int hour = 0;
    for (const char *p = num; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        hour = hour * 10 + (*p - '0');
        if (hour > TZ_HOUR_MAX) {
            return false;
        }
    }
    hour *= sign;
    if (hour < TZ_HOUR_MIN || hour > TZ_HOUR_MAX || hour == 0) {
        return false;
    }
    *hour_out = hour;
    return true;
}

static void id_for_hour(int hour, char *out, size_t n)
{
    if (hour > 0) {
        snprintf(out, n, "UTC+%d", hour);
    } else if (hour < 0) {
        snprintf(out, n, "UTC%d", hour);
    } else {
        snprintf(out, n, "UTC");
    }
}

/* East of UTC is a negative POSIX offset. */
static void posix_for_hour(int hour, char *out, size_t n)
{
    int posix = -hour;
    if (posix > 0) {
        snprintf(out, n, "UTC+%d", posix);
    } else if (posix < 0) {
        snprintf(out, n, "UTC%d", posix);
    } else {
        snprintf(out, n, "UTC0");
    }
}

static void apply_hour(int hour)
{
    char posix[16];
    posix_for_hour(hour, posix, sizeof(posix));
    setenv("TZ", posix, 1);
    tzset();
    id_for_hour(hour, s_id, sizeof(s_id));
    s_ready = true;
    ESP_LOGI(TAG, "timezone %s (%s)", s_id, posix);
}

void minihub_tz_apply(void)
{
    char stored[sizeof(s_id)];
    stored[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(stored);
        if (nvs_get_str(handle, NVS_KEY, stored, &len) != ESP_OK) {
            stored[0] = '\0';
        }
        nvs_close(handle);
    }
    int hour = 0;
    if (!hour_from_id(stored, &hour)) {
        hour = 0;
    }
    apply_hour(hour);
}

esp_err_t minihub_tz_set(const char *id)
{
    int hour = 0;
    if (!hour_from_id(id, &hour)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char canon[16];
    id_for_hour(hour, canon, sizeof(canon));
    err = nvs_set_str(handle, NVS_KEY, canon);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    apply_hour(hour);
    return ESP_OK;
}

const char *minihub_tz_id(void)
{
    if (!s_ready) {
        minihub_tz_apply();
    }
    return s_id;
}

int minihub_tz_offset_min(void)
{
    if (!s_ready) {
        minihub_tz_apply();
    }
    time_t now = time(NULL);
    struct tm local_tm;
    struct tm utc_tm;
    if (!localtime_r(&now, &local_tm) || !gmtime_r(&now, &utc_tm)) {
        return 0;
    }
    int day = local_tm.tm_yday - utc_tm.tm_yday;
    if (local_tm.tm_year != utc_tm.tm_year) {
        day = (local_tm.tm_year > utc_tm.tm_year) ? 1 : -1;
    }
    return day * 1440 + (local_tm.tm_hour - utc_tm.tm_hour) * 60 +
           (local_tm.tm_min - utc_tm.tm_min);
}

cJSON *minihub_tz_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "id", minihub_tz_id());
    cJSON_AddNumberToObject(root, "offset_min", minihub_tz_offset_min());
    cJSON *zones = cJSON_AddArrayToObject(root, "zones");
    if (zones) {
        for (int hour = TZ_HOUR_MIN; hour <= TZ_HOUR_MAX; hour++) {
            char id[16];
            id_for_hour(hour, id, sizeof(id));
            cJSON_AddItemToArray(zones, cJSON_CreateString(id));
        }
    }
    return root;
}
