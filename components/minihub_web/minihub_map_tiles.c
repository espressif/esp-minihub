/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_map_tiles.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)

static void *map_heap_alloc(size_t size);
static void *map_heap_realloc(void *ptr, size_t size);
static void map_heap_free(void *ptr);

#define STBI_MALLOC(sz) map_heap_alloc(sz)
#define STBI_REALLOC(p, sz) map_heap_realloc(p, sz)
#define STBI_FREE(p) map_heap_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STBIW_MALLOC(sz) map_heap_alloc(sz)
#define STBIW_REALLOC(p, sz) map_heap_realloc(p, sz)
#define STBIW_FREE(p) map_heap_free(p)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const char *TAG = "minihub_map_tiles";

#define MAP_TILE_PX 256
#define MAP_HTTP_UA "MiniHub/1.0 (ESP32-S3; +https://github.com/espressif/esp-brookesia)"
#define MAP_BG_R 0x1a
#define MAP_BG_G 0x22
#define MAP_BG_B 0x38

static void *map_heap_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *map_heap_realloc(void *ptr, size_t size)
{
    void *grown = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (grown == NULL) {
        grown = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return grown;
}

static void map_heap_free(void *ptr)
{
    heap_caps_free(ptr);
}

static int floor_div(int a, int b)
{
    int q = a / b;
    const int r = a % b;
    if (r != 0 && ((r > 0) != (b > 0))) {
        q--;
    }
    return q;
}

static void latlon_to_world_pixel(double lat, double lon, int zoom, double *world_x, double *world_y)
{
    const double n = (double)(1 << zoom);
    *world_x = (lon + 180.0) / 360.0 * n * (double)MAP_TILE_PX;
    const double lat_rad = lat * M_PI / 180.0;
    *world_y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n * (double)MAP_TILE_PX;
}

#define MAP_GCJ_A 6378245.0
#define MAP_GCJ_EE 0.00669342162296594323

bool minihub_map_in_china(double lat, double lon)
{
    return lon >= 72.004 && lon <= 137.8347 && lat >= 0.8293 && lat <= 55.8271;
}

static double transform_lat(double x, double y)
{
    double ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.2 * x * y + 0.2 * sqrt(fabs(x));
    ret += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    ret += (20.0 * sin(y * M_PI) + 40.0 * sin(y / 3.0 * M_PI)) * 2.0 / 3.0;
    ret += (160.0 * sin(y / 12.0 * M_PI) + 320.0 * sin(y * M_PI / 30.0)) * 2.0 / 3.0;
    return ret;
}

static double transform_lon(double x, double y)
{
    double ret = 300.0 + x + 2.0 * y + 0.2 * x * x + 0.2 * x * y + 0.2 * sqrt(fabs(x));
    ret += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    ret += (20.0 * sin(x * M_PI) + 40.0 * sin(x / 3.0 * M_PI)) * 2.0 / 3.0;
    ret += (150.0 * sin(x / 12.0 * M_PI) + 300.0 * sin(x / 30.0 * M_PI)) * 2.0 / 3.0;
    return ret;
}

void minihub_map_wgs84_to_gcj02(double wgs_lat, double wgs_lon, double *gcj_lat, double *gcj_lon)
{
    if (!gcj_lat || !gcj_lon) {
        return;
    }
    if (!minihub_map_in_china(wgs_lat, wgs_lon)) {
        *gcj_lat = wgs_lat;
        *gcj_lon = wgs_lon;
        return;
    }
    const double dlat = transform_lat(wgs_lon - 105.0, wgs_lat - 35.0);
    const double dlon = transform_lon(wgs_lon - 105.0, wgs_lat - 35.0);
    const double radlat = wgs_lat / 180.0 * M_PI;
    double magic = sin(radlat);
    magic = 1.0 - MAP_GCJ_EE * magic * magic;
    const double sqrtmagic = sqrt(magic);
    *gcj_lat = wgs_lat + (dlat * 180.0) / ((MAP_GCJ_A * (1.0 - MAP_GCJ_EE)) / (magic * sqrtmagic) * M_PI);
    *gcj_lon = wgs_lon + (dlon * 180.0) / (MAP_GCJ_A / sqrtmagic * cos(radlat) * M_PI);
}

static bool buffer_is_png(const unsigned char *data, size_t len)
{
    static const unsigned char kPngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return data != NULL && len >= sizeof(kPngSig) && memcmp(data, kPngSig, sizeof(kPngSig)) == 0;
}

static bool buffer_is_image(const unsigned char *data, size_t len)
{
    return buffer_is_png(data, len) || (data != NULL && len >= 2 && data[0] == 0xff && data[1] == 0xd8);
}

static esp_err_t http_download_to_buffer(const char *url, const char *http_host, unsigned char **out_buf,
                                         size_t *out_len)
{
    if (!url || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = http_host,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "User-Agent", MAP_HTTP_UA);
    esp_http_client_set_header(client, "Accept", "image/png,image/jpeg,*/*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (http_host != NULL) {
        esp_http_client_set_header(client, "Host", http_host);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed (%s): %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t cap = 65536;
    size_t len = 0;
    unsigned char *buf = map_heap_alloc(cap);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    char chunk[4096];
    while (true) {
        const int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        if (len + (size_t)n > cap) {
            cap *= 2;
            unsigned char *grown = map_heap_realloc(buf, cap);
            if (!grown) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            buf = grown;
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || len < 128 || !buffer_is_image(buf, len)) {
        ESP_LOGW(TAG, "invalid image (%u bytes) from %s", (unsigned)len, url);
        map_heap_free(buf);
        return ESP_FAIL;
    }
    *out_buf = buf;
    *out_len = len;
    return ESP_OK;
}

static void fill_canvas_bg(unsigned char *canvas, int canvas_w, int canvas_h)
{
    for (int y = 0; y < canvas_h; ++y) {
        unsigned char *row = canvas + ((size_t)y * (size_t)canvas_w) * 4;
        for (int x = 0; x < canvas_w; ++x) {
            unsigned char *px = row + (size_t)x * 4;
            px[0] = MAP_BG_R;
            px[1] = MAP_BG_G;
            px[2] = MAP_BG_B;
            px[3] = 0xff;
        }
    }
}

static esp_err_t blit_tile_into_canvas(unsigned char *canvas, int canvas_w, int canvas_h, int crop_x, int crop_y,
                                       int tile_x, int tile_y, const unsigned char *tile_rgba)
{
    const int tile_origin_x = tile_x * MAP_TILE_PX;
    const int tile_origin_y = tile_y * MAP_TILE_PX;
    for (int row = 0; row < MAP_TILE_PX; ++row) {
        const int world_y = tile_origin_y + row;
        const int out_y = world_y - crop_y;
        if (out_y < 0 || out_y >= canvas_h) {
            continue;
        }
        const unsigned char *tile_row = tile_rgba + (size_t)row * (size_t)MAP_TILE_PX * 4;
        unsigned char *out_row = canvas + ((size_t)out_y * (size_t)canvas_w) * 4;
        for (int col = 0; col < MAP_TILE_PX; ++col) {
            const int world_x = tile_origin_x + col;
            const int out_x = world_x - crop_x;
            if (out_x < 0 || out_x >= canvas_w) {
                continue;
            }
            memcpy(out_row + (size_t)out_x * 4, tile_row + (size_t)col * 4, 4);
        }
    }
    return ESP_OK;
}

static esp_err_t format_tile_url(char *url, size_t url_len, const minihub_map_tile_source_t *source, int zoom,
                                 int tile_x, int tile_y)
{
    int n;
    if (source->format == MINIHUB_MAP_TILE_FMT_AMAP_XYZ) {
        n = snprintf(url, url_len, source->url_template, tile_x, tile_y, zoom);
    } else {
        n = snprintf(url, url_len, source->url_template, zoom, tile_x, tile_y);
    }
    if (n < 0 || n >= (int)url_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t download_tile_rgba(const minihub_map_tile_source_t *source, int zoom, int tile_x, int tile_y,
                                    unsigned char **out_rgba)
{
    char url[256];
    if (format_tile_url(url, sizeof(url), source, zoom, tile_x, tile_y) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    unsigned char *png = NULL;
    size_t png_len = 0;
    esp_err_t err = http_download_to_buffer(url, source->http_host, &png, &png_len);
    if (err != ESP_OK) {
        return err;
    }

    int png_w = 0;
    int png_h = 0;
    int channels = 0;
    unsigned char *rgba = stbi_load_from_memory(png, (int)png_len, &png_w, &png_h, &channels, 4);
    map_heap_free(png);
    if (!rgba || png_w != MAP_TILE_PX || png_h != MAP_TILE_PX) {
        stbi_image_free(rgba);
        ESP_LOGW(TAG, "tile decode failed (%dx%d) %s", png_w, png_h, url);
        return ESP_FAIL;
    }
    *out_rgba = rgba;
    return ESP_OK;
}

static esp_err_t write_png_snapshot(const char *path, const unsigned char *rgba, int width_px, int height_px)
{
    const int stride = width_px * 4;
    if (!stbi_write_png(path, width_px, height_px, 4, rgba, stride)) {
        unlink(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void set_canvas_pixel(unsigned char *canvas, int canvas_w, int canvas_h, int x, int y, uint8_t r, uint8_t g,
                             uint8_t b)
{
    if (x < 0 || x >= canvas_w || y < 0 || y >= canvas_h) {
        return;
    }
    unsigned char *px = canvas + ((size_t)y * (size_t)canvas_w + (size_t)x) * 4;
    px[0] = r;
    px[1] = g;
    px[2] = b;
    px[3] = 0xff;
}

static void draw_hub_marker(unsigned char *canvas, int canvas_w, int canvas_h, int cx, int cy)
{
    const int outer_r = 7;
    const int inner_r = 4;
    for (int dy = -outer_r; dy <= outer_r; ++dy) {
        for (int dx = -outer_r; dx <= outer_r; ++dx) {
            const int dist_sq = dx * dx + dy * dy;
            if (dist_sq > outer_r * outer_r) {
                continue;
            }
            if (dist_sq <= inner_r * inner_r) {
                set_canvas_pixel(canvas, canvas_w, canvas_h, cx + dx, cy + dy, 0x21, 0x96, 0xf3);
            } else {
                set_canvas_pixel(canvas, canvas_w, canvas_h, cx + dx, cy + dy, 0xff, 0xff, 0xff);
            }
        }
    }
}

esp_err_t minihub_map_build_tile_snapshot(double lat, double lon, int zoom, int width_px, int height_px,
                                          const minihub_map_tile_source_t *source, const char *out_path)
{
    if (!source || !source->url_template || !out_path || width_px <= 0 || height_px <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    double map_lat = lat;
    double map_lon = lon;
    if (source->use_gcj02) {
        minihub_map_wgs84_to_gcj02(lat, lon, &map_lat, &map_lon);
    }

    double hub_wx = 0;
    double hub_wy = 0;
    latlon_to_world_pixel(map_lat, map_lon, zoom, &hub_wx, &hub_wy);

    const int crop_x = (int)floor(hub_wx - (double)width_px / 2.0);
    const int crop_y = (int)floor(hub_wy - (double)height_px / 2.0);
    const int tile_x0 = floor_div(crop_x, MAP_TILE_PX);
    const int tile_y0 = floor_div(crop_y, MAP_TILE_PX);
    const int tile_x1 = floor_div(crop_x + width_px - 1, MAP_TILE_PX);
    const int tile_y1 = floor_div(crop_y + height_px - 1, MAP_TILE_PX);
    const int tile_count = (tile_x1 - tile_x0 + 1) * (tile_y1 - tile_y0 + 1);

    const size_t canvas_bytes = (size_t)width_px * (size_t)height_px * 4;
    unsigned char *canvas = map_heap_alloc(canvas_bytes);
    if (!canvas) {
        return ESP_ERR_NO_MEM;
    }
    fill_canvas_bg(canvas, width_px, height_px);

    int tiles_ok = 0;
    for (int ty = tile_y0; ty <= tile_y1; ++ty) {
        for (int tx = tile_x0; tx <= tile_x1; ++tx) {
            unsigned char *tile = NULL;
            if (download_tile_rgba(source, zoom, tx, ty, &tile) != ESP_OK) {
                continue;
            }
            tiles_ok++;
            (void)blit_tile_into_canvas(canvas, width_px, height_px, crop_x, crop_y, tx, ty, tile);
            stbi_image_free(tile);
        }
    }
    if (tiles_ok != tile_count) {
        ESP_LOGW(TAG, "incomplete mosaic %d/%d tiles", tiles_ok, tile_count);
        map_heap_free(canvas);
        return ESP_FAIL;
    }

    const int hub_x = (int)(hub_wx - (double)crop_x + 0.5);
    const int hub_y = (int)(hub_wy - (double)crop_y + 0.5);
    draw_hub_marker(canvas, width_px, height_px, hub_x, hub_y);

    const esp_err_t save_err = write_png_snapshot(out_path, canvas, width_px, height_px);
    map_heap_free(canvas);
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "png encode failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "tile mosaic %dx%d (%d tiles) -> %s", width_px, height_px, tile_count, out_path);
    return ESP_OK;
}
