/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_MATTER_ADAPTER

#include "minihub_adapter.h"
#include "minihub_core.h"
#include "minihub_matter.h"
#include "minihub_module.h"
#include "matter_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "adapter_matter";

#define MAX_MATTER_DEVICES 16

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    char transport[16];
    char commission_phase[16];
    char commission_error[96];
    uint64_t node_id;
    uint16_t endpoint_id;
} matter_device_t;

static matter_device_t s_devices[MAX_MATTER_DEVICES];
static size_t s_device_count;
static bool s_started;

/* Basic Information is only on the fabric, so a republish would otherwise have to
 * re-interview every node — and an interview blocks on the CHIP stack lock, which
 * commissioning holds. Cache it per node so publish_points can rebuild props locally. */
static struct {
    uint64_t node_id;
    minihub_matter_identity_t id;
} s_identity[MAX_MATTER_DEVICES];
static size_t s_identity_count;

static const minihub_matter_identity_t *cached_identity(uint64_t node_id)
{
    for (size_t i = 0; i < s_identity_count; i++) {
        if (s_identity[i].node_id == node_id) {
            return &s_identity[i].id;
        }
    }
    return NULL;
}

static uint64_t parse_node_id(const char *s)
{
    if (!s || !s[0]) {
        return 1;
    }
    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0) {
        return strtoull(s, NULL, 16);
    }
    return strtoull(s, NULL, 10);
}

static matter_device_t *find_device(const char *point_id)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].point_id, point_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static void apply_identity_props(const matter_device_t *dev, const minihub_matter_identity_t *id)
{
    if (!dev || !dev->point_id[0]) {
        return;
    }
    cJSON *patch = cJSON_CreateObject();
    if (!patch) {
        return;
    }
    char nid[24];
    snprintf(nid, sizeof(nid), "%llu", (unsigned long long)dev->node_id);
    cJSON_AddStringToObject(patch, "node_id", nid);
    if (id) {
        if (id->manufacturer[0]) {
            cJSON_AddStringToObject(patch, "manufacturer", id->manufacturer);
        }
        if (id->model[0]) {
            cJSON_AddStringToObject(patch, "model", id->model);
        }
        if (id->sw_version[0]) {
            cJSON_AddStringToObject(patch, "sw_version", id->sw_version);
        }
        if (id->hw_version[0]) {
            cJSON_AddStringToObject(patch, "hw_version", id->hw_version);
        }
    }
    (void)minihub_point_merge_props(dev->point_id, NULL, patch);
    cJSON_Delete(patch);
}

static void remember_identity(uint64_t node_id, const minihub_matter_identity_t *id)
{
    for (size_t i = 0; i < s_identity_count; i++) {
        if (s_identity[i].node_id == node_id) {
            s_identity[i].id = *id;
            return;
        }
    }
    if (s_identity_count < MAX_MATTER_DEVICES) {
        s_identity[s_identity_count].node_id = node_id;
        s_identity[s_identity_count].id = *id;
        s_identity_count++;
    }
}

static void on_matter_identity(uint64_t node_id, const minihub_matter_identity_t *id, void *ctx)
{
    (void)ctx;
    if (!id) {
        return;
    }
    remember_identity(node_id, id);
    for (size_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].node_id == node_id) {
            apply_identity_props(&s_devices[i], id);
        }
    }
}

static void interview_unique_nodes(void)
{
    for (size_t i = 0; i < s_device_count; i++) {
        bool already = false;
        for (size_t j = 0; j < i; j++) {
            if (s_devices[j].node_id == s_devices[i].node_id) {
                already = true;
                break;
            }
        }
        if (already || s_devices[i].node_id < 2 || s_devices[i].commission_phase[0]) {
            continue;
        }
        (void)minihub_matter_interview_identity(s_devices[i].node_id);
    }
}

static esp_err_t parse_devices(const cJSON *cfg)
{
    const cJSON *devices = cJSON_GetObjectItem(cfg, "devices");
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        if (s_device_count >= MAX_MATTER_DEVICES) {
            break;
        }
        matter_device_t *dev = &s_devices[s_device_count];
        memset(dev, 0, sizeof(*dev));

        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *node_id = cJSON_GetObjectItem(item, "node_id");
        const cJSON *endpoint_id = cJSON_GetObjectItem(item, "endpoint_id");
        const cJSON *transport = cJSON_GetObjectItem(item, "transport");
        const cJSON *cphase = cJSON_GetObjectItem(item, "commission_phase");
        const cJSON *cerr = cJSON_GetObjectItem(item, "commission_error");

        if (!cJSON_IsString(point_id)) {
            continue;
        }
        strncpy(dev->point_id, point_id->valuestring, sizeof(dev->point_id) - 1);
        if (cJSON_IsString(type)) {
            strncpy(dev->type, type->valuestring, sizeof(dev->type) - 1);
        } else {
            strncpy(dev->type, "light", sizeof(dev->type) - 1);
        }
        if (cJSON_IsString(transport) && transport->valuestring[0]) {
            strncpy(dev->transport, transport->valuestring, sizeof(dev->transport) - 1);
        }
        if (cJSON_IsString(cphase) && cphase->valuestring[0]) {
            strncpy(dev->commission_phase, cphase->valuestring, sizeof(dev->commission_phase) - 1);
        }
        if (cJSON_IsString(cerr) && cerr->valuestring[0]) {
            strncpy(dev->commission_error, cerr->valuestring, sizeof(dev->commission_error) - 1);
        }
        dev->node_id = cJSON_IsString(node_id) ? parse_node_id(node_id->valuestring) : 1;
        dev->endpoint_id = cJSON_IsNumber(endpoint_id) ? (uint16_t)endpoint_id->valueint : 1;
        s_device_count++;
    }
    return ESP_OK;
}

static esp_err_t matter_adapter_init(const cJSON *config)
{
    s_device_count = 0;
    s_started = false;
    if (config) {
        parse_devices(config);
    }
    /* Host-static RPC → Store 「已安装」shows as 内置 (managed=false). */
    (void)minihub_module_register_rpc("matter", matter_module_rpc);
    (void)minihub_matter_set_identity_cb(on_matter_identity, NULL);
    return ESP_OK;
}

/* Register or refresh every configured device's point in place. minihub_point_register()
 * overwrites an existing id, so calling this again never removes and re-adds a card. */
static void publish_points(void)
{
    for (size_t i = 0; i < s_device_count; i++) {
        minihub_point_t point = {0};
        strncpy(point.point_id, s_devices[i].point_id, sizeof(point.point_id) - 1);
        strncpy(point.type, s_devices[i].type, sizeof(point.type) - 1);
        strncpy(point.adapter, "matter", sizeof(point.adapter) - 1);
        const bool commissioning = s_devices[i].commission_phase[0] != '\0';
        strncpy(point.value, commissioning ? "…" : "off", sizeof(point.value) - 1);
        /* Pending/failed commission: show as offline so controls stay disabled. */
        point.online = commissioning ? false : minihub_matter_ready();
        cJSON *props = cJSON_CreateObject();
        if (props) {
            char name[48];
            snprintf(name, sizeof(name), "node%llu·ep%u",
                     (unsigned long long)s_devices[i].node_id,
                     (unsigned)s_devices[i].endpoint_id);
            cJSON_AddStringToObject(props, "name", name);
            if (s_devices[i].transport[0]) {
                cJSON_AddStringToObject(props, "transport", s_devices[i].transport);
            }
            if (commissioning) {
                cJSON_AddTrueToObject(props, "commissioning");
                cJSON_AddStringToObject(props, "commission_phase", s_devices[i].commission_phase);
                if (s_devices[i].commission_error[0]) {
                    cJSON_AddStringToObject(props, "commission_error", s_devices[i].commission_error);
                }
                cJSON_AddTrueToObject(props, "no_control");
            }
            point.props = props;
        }
        minihub_point_register(&point);
        if (point.props) {
            cJSON_Delete(point.props);
            point.props = NULL;
        }
        apply_identity_props(&s_devices[i], cached_identity(s_devices[i].node_id));
        /* apply_identity_props() writes props via minihub_point_set(), which marks
         * any write as online — re-assert the state the row actually earned. */
        (void)minihub_point_set_online(s_devices[i].point_id,
                                       !commissioning && minihub_matter_ready());
    }
}

static esp_err_t matter_adapter_start(void)
{
    esp_err_t err = minihub_matter_init();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        if (minihub_matter_ready()) {
            err = ESP_OK;
        } else {
            /* Still register configured points so overview/delete stay usable. */
            ESP_LOGE(TAG, "Matter init failed: %s (registering points anyway)",
                     esp_err_to_name(err));
        }
    }

    publish_points();

    s_started = true;
    if (minihub_matter_ready()) {
        interview_unique_nodes();
    }
    ESP_LOGI(TAG, "Matter adapter started (%u devices, commissioner=%d)",
             (unsigned)s_device_count, minihub_matter_ready() ? 1 : 0);
    /* Adapter stays loaded even if commissioner setup failed this pass. */
    return ESP_OK;
}

static esp_err_t matter_adapter_stop(void)
{
    s_started = false;
    minihub_point_unregister_by_adapter("matter");
    return ESP_OK;
}

/* Config-only change — a row appeared, or its commission_phase moved. Republish in
 * place so the console never sees the card disappear. Deliberately does not interview:
 * the caller is usually an httpd task, and Basic Information reads are best-effort
 * blocking work for the adapter_poll/start path. Rows deleted from the config are
 * unregistered by the delete path, not here. */
static esp_err_t matter_adapter_resync(const cJSON *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_device_count = 0;
    parse_devices(cfg);
    publish_points();
    return ESP_OK;
}

static esp_err_t matter_adapter_handle_command(const minihub_command_t *cmd)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    matter_device_t *dev = find_device(cmd->point_id);
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(cmd->cmd, "ota") == 0 || strcmp(cmd->cmd, "start_ota") == 0) {
        const char *path = NULL;
        const cJSON *pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "path") : NULL;
        if (!cJSON_IsString(pj) || !pj->valuestring) {
            pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "file_path") : NULL;
        }
        if (!cJSON_IsString(pj) || !pj->valuestring) {
            pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "url") : NULL;
        }
        if (cJSON_IsString(pj) && pj->valuestring) {
            path = pj->valuestring;
        }
        return minihub_matter_ota_offer(cmd->point_id, path);
    }

    if (!minihub_matter_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    bool target_on = false;
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        target_on = true;
    } else if (strcmp(cmd->cmd, "turn_off") == 0) {
        target_on = false;
    } else if (strcmp(cmd->cmd, "toggle") == 0) {
        minihub_point_t snap = {0};
        minihub_point_get_snapshot(cmd->point_id, &snap);
        bool on = (strcmp(snap.value, "on") == 0) || (atoi(snap.value) > 0);
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        target_on = !on;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = minihub_matter_onoff(dev->node_id, dev->endpoint_id, target_on);
    if (err == ESP_OK) {
        (void)minihub_point_set(dev->point_id, target_on ? "on" : "off", NULL);
    }
    return err;
}

static esp_err_t matter_adapter_poll(void)
{
    if (!s_started || !minihub_matter_ready()) {
        return ESP_OK;
    }
    /* Matter read/invoke needs deep CHIP stack; adapter_poll runs on a small task.
     * Retry Basic Information if CASE was not ready at start. */
    static int s_ticks;
    s_ticks++;
    if (s_ticks <= 60 && (s_ticks % 20) == 0) {
        interview_unique_nodes();
    }
    return ESP_OK;
}

const minihub_adapter_t matter_adapter = {
    .name = "matter",
    .version = "0.1.0",
    .init = matter_adapter_init,
    .start = matter_adapter_start,
    .stop = matter_adapter_stop,
    .handle_command = matter_adapter_handle_command,
    .poll = matter_adapter_poll,
    .resync = matter_adapter_resync,
};

REGISTER_ADAPTER(matter_adapter);

#else /* !CONFIG_MINIHUB_MATTER_ADAPTER */

/* Translation unit kept empty when Matter is disabled. */

#endif /* CONFIG_MINIHUB_MATTER_ADAPTER */
