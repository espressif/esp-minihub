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

/* One fixed offset, in minutes east of UTC: whole hours from -12 to +14, plus
 * the half and quarter hours that places actually run on. No place names and no
 * daylight saving, so a log line and a schedule agree on what the number means.
 * The POSIX string inverts the sign: 5 hours 30 minutes east is "UTC-5:30". */
#define TZ_OFF_MIN (-12 * 60)
#define TZ_OFF_MAX (14 * 60)

static const int TZ_PART_HOURS[] = {
    -570, /*  Marquesas -09:30 */
    -210, /*  Newfoundland -03:30 */
     210, /*  Iran +03:30 */
     270, /*  Afghanistan +04:30 */
     330, /*  India, Sri Lanka +05:30 */
     345, /*  Nepal +05:45 */
     390, /*  Myanmar +06:30 */
     525, /*  Eucla +08:45 */
     570, /*  Central Australia +09:30 */
     630, /*  Lord Howe +10:30 */
     765, /*  Chatham +12:45 */
};

static char s_id[16] = "UTC";
static bool s_ready;

/* "UTC", "UTC+8", "UTC-8", "UTC+5:30" -> minutes east. The minute field is
 * restricted to quarter-hour steps, and ":00" is accepted but not canonical. */
static bool off_from_id(const char *id, int *off_out)
{
    if (!id || !off_out) {
        return false;
    }
    if (strcmp(id, "UTC") == 0) {
        *off_out = 0;
        return true;
    }
    if (id[0] != 'U' || id[1] != 'T' || id[2] != 'C' || (id[3] != '+' && id[3] != '-')) {
        return false;
    }
    int sign = id[3] == '+' ? 1 : -1;
    const char *p = id + 4;
    if (*p < '1' || *p > '9') {
        return false;
    }
    int hour = 0;
    while (*p >= '0' && *p <= '9') {
        hour = hour * 10 + (*p++ - '0');
        if (hour > 14) {
            return false;
        }
    }
    int min = 0;
    if (*p == ':') {
        if (p[1] < '0' || p[1] > '5' || p[2] < '0' || p[2] > '9' || p[3] != '\0') {
            return false;
        }
        min = (p[1] - '0') * 10 + (p[2] - '0');
        if (min % 15) {
            return false;
        }
        p += 3;
    }
    if (*p != '\0') {
        return false;
    }
    int off = sign * (hour * 60 + min);
    if (off < TZ_OFF_MIN || off > TZ_OFF_MAX) {
        return false;
    }
    *off_out = off;
    return true;
}

static void id_for_off(int off, char *out, size_t n)
{
    int sign = off < 0 ? -1 : 1;
    int mag = off < 0 ? -off : off;
    int hour = mag / 60, min = mag % 60;
    if (off == 0) {
        snprintf(out, n, "UTC");
    } else if (min) {
        snprintf(out, n, "UTC%c%d:%02d", sign > 0 ? '+' : '-', hour, min);
    } else {
        snprintf(out, n, "UTC%c%d", sign > 0 ? '+' : '-', hour);
    }
}

static void posix_for_off(int off, char *out, size_t n)
{
    int sign = off > 0 ? -1 : (off < 0 ? 1 : 0);
    int mag = off < 0 ? -off : off;
    int hour = mag / 60, min = mag % 60;
    if (off == 0) {
        snprintf(out, n, "UTC0");
    } else if (min) {
        snprintf(out, n, "UTC%c%d:%02d", sign > 0 ? '+' : '-', hour, min);
    } else {
        snprintf(out, n, "UTC%c%d", sign > 0 ? '+' : '-', hour);
    }
}

static void apply_off(int off)
{
    char posix[16];
    posix_for_off(off, posix, sizeof(posix));
    setenv("TZ", posix, 1);
    tzset();
    id_for_off(off, s_id, sizeof(s_id));
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
    int off = 0;
    if (!off_from_id(stored, &off)) {
        off = 0;
    }
    apply_off(off);
}

esp_err_t minihub_tz_set(const char *id)
{
    int off = 0;
    if (!off_from_id(id, &off)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char canon[16];
    id_for_off(off, canon, sizeof(canon));
    err = nvs_set_str(handle, NVS_KEY, canon);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    apply_off(off);
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
        int list[32 + (int)(sizeof(TZ_PART_HOURS) / sizeof(TZ_PART_HOURS[0]))];
        int n = 0;
        for (int hour = -12; hour <= 14; hour++) {
            list[n++] = hour * 60;
        }
        for (size_t i = 0; i < sizeof(TZ_PART_HOURS) / sizeof(TZ_PART_HOURS[0]); i++) {
            list[n++] = TZ_PART_HOURS[i];
        }
        for (int i = 1; i < n; i++) { /* insertion sort: the picker reads top down */
            int v = list[i], j = i - 1;
            while (j >= 0 && list[j] > v) {
                list[j + 1] = list[j];
                j--;
            }
            list[j + 1] = v;
        }
        for (int i = 0; i < n; i++) {
            char id[16];
            id_for_off(list[i], id, sizeof(id));
            cJSON_AddItemToArray(zones, cJSON_CreateString(id));
        }
    }
    return root;
}
