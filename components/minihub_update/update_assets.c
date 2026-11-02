/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device-class icons, fetched one file per device type.
 *
 * There is deliberately NO metadata request here. The avatar is <type>.svg by
 * convention, so GET /files/Assets/images/devices/<type>.svg can be asked for
 * directly. The previous design asked GET /api/assets/device/<type> first, which
 * only learned a derivable name -- and, because that route is driven by the
 * shared model (model/devices.csv), it 404'd for a plugin-owned type, so such a
 * type could never get an icon at all.
 *
 * An icon already on the device is left alone (stat), so the hourly pass costs
 * nothing once a type has been fetched. Repainting artwork on the server
 * therefore does not reach devices that already have the file.
 *
 * Per-parameter icons and styles are NOT fetched, and have no channel:
 *   - a control is a CSS widget. The firmware deliberately reports an empty
 *     image for every parameter (see handle_api_device_widgets) and the console
 *     never requests /img/parameters mockups.
 *   - the `css` column of parameters.csv holds a CLASS NAME, not CSS text; the
 *     rules live in the bundled device_widgets.css and arrive inline in
 *     /api/device_widgets. So there is no per-parameter stylesheet to fetch.
 * A stylesheet under /data/styles can still be pushed by hand through the dev
 * panel (PUT /api/dev/assets/styles/...) for experiments.
 */

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"

#include "minihub_device_widget.h"
#include "minihub_update.h"
#include "update_internal.h"

static const char *TAG = "minihub_update";

/* A type is a plain token: no '/', no '.', so it cannot escape the icon
 * directory or the request path. */
static bool asset_type_token_ok(const char *t)
{
    if (!t || !t[0] || strlen(t) >= 64) {
        return false;
    }
    for (const char *p = t; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t minihub_update_fetch_device_icon(const char *base_url, const char *type)
{
    if (!base_url || !base_url[0] || !asset_type_token_ok(type)) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[MH_UPDATE_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s/files/Assets/images/devices/%s.svg", base_url, type);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char dest[MH_UPDATE_URL_MAX];
    n = snprintf(dest, sizeof(dest), "%s/%s/%s.svg", MINIHUB_DW_IMAGES_DIR,
                 MINIHUB_DW_IMAGES_DEVICES_SUBDIR, type);
    if (n < 0 || (size_t)n >= sizeof(dest)) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Already have it? This runs hourly for every in-use type, and an avatar is
     * a few hundred bytes, so re-downloading it every pass is pure waste. Skip
     * on any non-empty regular file. The trade-off: re-painting artwork on the
     * server does NOT reach a device that already has the file -- delete it
     * (dev panel DELETE /api/dev/assets/...) to force a refresh. */
    struct stat st;
    if (stat(dest, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        ESP_LOGD(TAG, "device icon present: %s", dest);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "device icon -> %s", dest);
    /* No sha/size: a type with no artwork is a 404, which the download helper
     * rejects before it creates any file, so the tree is left untouched. */
    return mh_update_download_verified(url, dest, NULL, 0);
}
