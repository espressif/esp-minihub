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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "cJSON.h"

static const char *TAG = "adapter_matter";

#define MAX_MATTER_DEVICES 16

/* OnOff cluster (0x0006), attribute OnOff (0x0000). A subscription can carry other
 * attributes, so the report sink filters on both ids rather than trusting the path. */
#define MATTER_ONOFF_CLUSTER 0x0006
#define MATTER_ONOFF_ATTR    0x0000

/* Re-arm pacing for minihub_matter_subscribe_node(). An arm that is still in flight
 * must not be re-sent (see subscribe_commissioned() below), so a node that never came
 * up is retried at most every SUB_ARM_MIN_MS, doubling up to SUB_ARM_MAX_MS. */
#define SUB_ARM_MIN_MS  30000
#define SUB_ARM_MAX_MS  600000
/* Mirrors MATTER_SUB_MAX in matter_ctrl.cpp — more rows would only ever be dropped. */
#define MAX_MATTER_SUB_NODES 8

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

/* s_devices / s_device_count are read on the CHIP event task (on_matter_attr,
 * on_matter_identity) while matter_adapter_init() and matter_adapter_resync() rebuild
 * them from an httpd / lifecycle task. The writers parse into s_build and publish under
 * this mux with the count written LAST, so a reader never sees a half-built array; the
 * readers copy what they need under the same mux and release it BEFORE calling any
 * minihub_point_* (a portMUX is a spinlock and must not be held across a blocking call).
 * Callers of publish_points/interview/subscribe_commissioned are serialized by the
 * adapter lifecycle lock, so s_snap/s_build are single-owner scratch. */
static portMUX_TYPE s_devices_mux = portMUX_INITIALIZER_UNLOCKED;
static matter_device_t s_build[MAX_MATTER_DEVICES];
static matter_device_t s_snap[MAX_MATTER_DEVICES];
static size_t s_snap_count;

/* Copy the published table into dst and return how many rows it holds. */
static size_t snapshot_devices(matter_device_t *dst, size_t max)
{
    portENTER_CRITICAL(&s_devices_mux);
    size_t n = s_device_count < max ? s_device_count : max;
    memcpy(dst, s_devices, n * sizeof(*dst));
    portEXIT_CRITICAL(&s_devices_mux);
    return n;
}

/* Publish a fully-built s_build[0..n) as the live table (count written LAST). */
static void publish_device_table(size_t n)
{
    if (n > MAX_MATTER_DEVICES) {
        n = MAX_MATTER_DEVICES;
    }
    portENTER_CRITICAL(&s_devices_mux);
    if (n > 0) {
        memcpy(s_devices, s_build, n * sizeof(*s_devices));
    }
    s_device_count = n;
    portEXIT_CRITICAL(&s_devices_mux);
}

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

static bool find_device_copy(const char *point_id, matter_device_t *out)
{
    bool found = false;
    portENTER_CRITICAL(&s_devices_mux);
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].point_id, point_id) == 0) {
            *out = s_devices[i];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_devices_mux);
    return found;
}

static void apply_identity_props(const char *point_id, uint64_t node_id,
                                 const minihub_matter_identity_t *id)
{
    if (!point_id || !point_id[0]) {
        return;
    }
    cJSON *patch = cJSON_CreateObject();
    if (!patch) {
        return;
    }
    char nid[24];
    snprintf(nid, sizeof(nid), "%llu", (unsigned long long)node_id);
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
    (void)minihub_point_merge_props(point_id, NULL, patch);
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
    /* Copy the matching point ids under the mux, then merge props outside it —
     * minihub_point_merge_props takes the registry mutex, which cannot run in a
     * critical section. This runs on the CHIP event task, so keep the copy small. */
    char ids[MAX_MATTER_DEVICES][MINIHUB_POINT_ID_MAX];
    size_t m = 0;
    portENTER_CRITICAL(&s_devices_mux);
    for (size_t i = 0; i < s_device_count && m < MAX_MATTER_DEVICES; i++) {
        if (s_devices[i].node_id == node_id) {
            strncpy(ids[m], s_devices[i].point_id, sizeof(ids[m]) - 1);
            ids[m][sizeof(ids[m]) - 1] = '\0';
            m++;
        }
    }
    portEXIT_CRITICAL(&s_devices_mux);
    for (size_t k = 0; k < m; k++) {
        apply_identity_props(ids[k], node_id, id);
    }
}

static void interview_unique_nodes(void)
{
    size_t n = snapshot_devices(s_snap, MAX_MATTER_DEVICES);
    s_snap_count = n;
    for (size_t i = 0; i < n; i++) {
        bool already = false;
        for (size_t j = 0; j < i; j++) {
            if (s_snap[j].node_id == s_snap[i].node_id) {
                already = true;
                break;
            }
        }
        if (already || s_snap[i].node_id < 2 || s_snap[i].commission_phase[0]) {
            continue;
        }
        (void)minihub_matter_interview_identity(s_snap[i].node_id);
    }
}

/* Report sink: the device's own OnOff value, straight into the point registry.
 * Runs on the CHIP event task with the stack lock held, so this must stay cheap and
 * must not call back into Matter. minihub_point_set() takes the registry mutex (held
 * only inside state_store, never across an adapter call) and publishes after dropping
 * it, and history is a RAM ring with a 50 ms timeout — no lock order to invert here. */
/* Rate-limit for the adapter-side "report matches no config row" WARN (CHIP event task
 * only, so a plain static is fine). */
static int64_t s_nomatch_warn_ms;

static void on_matter_attr(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id,
                           uint32_t attribute_id, bool value, void *ctx)
{
    (void)ctx;
    if (cluster_id != MATTER_ONOFF_CLUSTER || attribute_id != MATTER_ONOFF_ATTR) {
        return;
    }
    /* Copy the matching row under the mux; the array may be rebuilt from httpd/poll. */
    matter_device_t match;
    bool found = false;
    portENTER_CRITICAL(&s_devices_mux);
    for (size_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].node_id == node_id && s_devices[i].endpoint_id == endpoint_id) {
            match = s_devices[i];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_devices_mux);

    if (!found) {
        /* A report arrived for a (node, endpoint) we do not publish — the classic case
         * is an aggregator whose OnOff endpoint differs from what adapters.json says.
         * Without this the drop is invisible and the card just looks fine but stale. */
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_nomatch_warn_ms >= 5000) {
            s_nomatch_warn_ms = now;
            ESP_LOGW(TAG, "report node%llu ep%u matches no published matter device",
                     (unsigned long long)node_id, (unsigned)endpoint_id);
        }
        return;
    }
    /* A pending row is not the device's real state — never let a report
     * overwrite 配网中, and never let it mark a pending card online. */
    if (match.commission_phase[0]) {
        return;
    }

    const char *nv = value ? "on" : "off";
    /* minihub_point_set() early-returns when the value is unchanged, so "changed" is not
     * proof a report arrived and an unconditional INFO would desync log from state.
     * Read the current value and log INFO only when this write actually lands. */
    bool will_change = true;
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(match.point_id, &snap) == ESP_OK) {
        will_change = strcmp(snap.value, nv) != 0;
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
    }
    if (will_change) {
        ESP_LOGI(TAG, "report node%llu ep%u onoff=%d -> %s",
                 (unsigned long long)node_id, (unsigned)endpoint_id,
                 value ? 1 : 0, match.point_id);
    } else {
        ESP_LOGD(TAG, "report node%llu ep%u onoff=%d (unchanged) -> %s",
                 (unsigned long long)node_id, (unsigned)endpoint_id,
                 value ? 1 : 0, match.point_id);
    }
    (void)minihub_point_set(match.point_id, nv, NULL);
}

/* Per-node subscription bookkeeping, touched only from the adapter_poll task (start
 * and resync also call subscribe_commissioned(), and hub_reload's lifecycle lock holds
 * adapter_poll out of that window — see minihub_adapter_lifecycle_lock).
 * matter_ctrl only short-circuits minihub_matter_subscribe_node() while a node's
 * subscription is LIVE: re-sending while the arm is still in flight leaks the previous
 * subscribe_command (it owns itself and is deleted in OnDone/OnError) and makes the
 * terminated callback clear live for the wrong row — so a reachable device would show
 * offline in the card and stop reporting until a later re-send. Hence: arm once, then
 * only retry after a backoff that doubles while the node keeps failing to come up,
 * reset when it is live. */
typedef struct {
    uint64_t node_id;
    int64_t  armed_ms;   /* esp_timer ms of the last arm; 0 = never armed */
    uint8_t  tries;      /* arms that never came up live since the last success */
    bool     live_seen;  /* this node's subscription was live at some point */
    /* The endpoint set we last armed for this node. A live subscription does NOT
     * re-read its endpoint list (see subscribe_node contract), so when the config's
     * set for this node differs we must drop the node and re-arm — hence storing it. */
    uint16_t armed_eps[16];
    size_t   armed_count;
    bool     armed_valid;
} matter_sub_watch_t;

static matter_sub_watch_t s_sub_watch[MAX_MATTER_SUB_NODES];
static size_t s_sub_watch_count;

static matter_sub_watch_t *sub_watch(uint64_t node_id)
{
    for (size_t i = 0; i < s_sub_watch_count; i++) {
        if (s_sub_watch[i].node_id == node_id) {
            return &s_sub_watch[i];
        }
    }
    if (s_sub_watch_count >= MAX_MATTER_SUB_NODES) {
        return NULL;
    }
    matter_sub_watch_t *w = &s_sub_watch[s_sub_watch_count++];
    memset(w, 0, sizeof(*w));
    w->node_id = node_id;
    return w;
}

static int64_t sub_backoff_ms(const matter_sub_watch_t *w)
{
    int64_t ms = SUB_ARM_MIN_MS;
    for (uint8_t i = 0; i < w->tries && ms < SUB_ARM_MAX_MS; i++) {
        ms *= 2;
    }
    return ms > SUB_ARM_MAX_MS ? SUB_ARM_MAX_MS : ms;
}

/* One-shot liveness view of the Matter subscription table. Returns the number of
 * rows copied; an allocation failure reads as "no row", i.e. not live, which is safe
 * because a live node is then merely re-armed and matter_ctrl answers ESP_OK without
 * touching the table. */
static size_t sub_live_snapshot(bool *out_live, uint64_t *out_node, size_t max)
{
    cJSON *arr = minihub_matter_subscriptions_json();
    if (!arr) {
        return 0;
    }
    size_t n = 0;
    cJSON *row = NULL;
    cJSON_ArrayForEach(row, arr) {
        if (n >= max) {
            break;
        }
        const cJSON *nid = cJSON_GetObjectItem(row, "node_id");
        if (!cJSON_IsNumber(nid)) {
            continue;
        }
        const cJSON *lv = cJSON_GetObjectItem(row, "live");
        out_node[n] = (uint64_t)nid->valuedouble;
        out_live[n] = cJSON_IsTrue(lv);
        n++;
    }
    cJSON_Delete(arr);
    return n;
}

/* Endpoints the current (snapshotted) config wants subscribed for one node, from
 * non-pending rows only. Returns 0 when the node is not commissioned in this pass. */
static size_t node_wanted_eps(uint64_t node_id, uint16_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < s_snap_count && n < max; i++) {
        if (s_snap[i].node_id == node_id && !s_snap[i].commission_phase[0]) {
            out[n++] = s_snap[i].endpoint_id;
        }
    }
    return n;
}

/* Arm OnOff reports for every commissioned endpoint this adapter publishes.
 * ``rearm_now`` is for the paths where a row just became real — boot and a
 * commissioning that turned 配网中 into a live device — and skips the backoff for
 * nodes that have never been live. The poll pass must not pass it.
 *
 * Reclamation runs first: a node that is watched but no longer in the commissioned set
 * (a re-commissioned bulb gets a NEW node id; its old row would otherwise never be
 * freed) is dropped via minihub_matter_drop_node() and its watch slot removed. A node
 * whose endpoint set changed is also dropped, then re-armed — a live subscription never
 * re-reads its endpoint list, and arming a second one while the first is live is the
 * double-arm the review warned about, so the old one is torn down before re-arming. */
static void subscribe_commissioned(bool rearm_now)
{
    if (!minihub_matter_ready()) {
        return;
    }
    s_snap_count = snapshot_devices(s_snap, MAX_MATTER_DEVICES);

    /* Phase A — reclaim watched nodes no longer commissioned. */
    for (size_t i = 0; i < s_sub_watch_count; i++) {
        uint16_t want[16];
        if (node_wanted_eps(s_sub_watch[i].node_id, want, 16) != 0) {
            continue; /* still commissioned */
        }
        (void)minihub_matter_drop_node(s_sub_watch[i].node_id);
        ESP_LOGI(TAG, "reclaimed watch for departed node%llu",
                 (unsigned long long)s_sub_watch[i].node_id);
        /* swap-remove: order is irrelevant, re-examine the swapped-in slot. */
        s_sub_watch[i] = s_sub_watch[s_sub_watch_count - 1];
        s_sub_watch_count--;
        i--;
    }

    bool live[MAX_MATTER_SUB_NODES];
    uint64_t live_node[MAX_MATTER_SUB_NODES];
    size_t live_count = sub_live_snapshot(live, live_node, MAX_MATTER_SUB_NODES);
    const int64_t now = esp_timer_get_time() / 1000;

    /* Phase B — arm / backoff per commissioned node. */
    for (size_t i = 0; i < s_snap_count; i++) {
        if (s_snap[i].commission_phase[0] || s_snap[i].node_id < 2) {
            continue;
        }
        /* one call per node, not per endpoint — see Task 2's subscribe_node contract */
        bool first_of_node = true;
        for (size_t k = 0; k < i; k++) {
            if (s_snap[k].node_id == s_snap[i].node_id && !s_snap[k].commission_phase[0]) {
                first_of_node = false;
                break;
            }
        }
        if (!first_of_node) {
            continue;
        }
        uint16_t eps[16];
        size_t n = node_wanted_eps(s_snap[i].node_id, eps, 16);
        if (n == 0) {
            continue;
        }

        const uint64_t node_id = s_snap[i].node_id;
        matter_sub_watch_t *w = sub_watch(node_id);
        if (!w) {
            /* More commissioned nodes than Matter tracks (MATTER_SUB_MAX). */
            ESP_LOGW(TAG, "no subscription watch slot for node%llu", (unsigned long long)node_id);
            return;
        }

        bool is_live = false;
        for (size_t r = 0; r < live_count; r++) {
            if (live_node[r] == node_id) {
                is_live = live[r];
                break;
            }
        }

        /* Config moved this node's endpoints. A live row will not re-read them and a
         * second arm would double-subscribe, so tear the old one down and re-arm. */
        bool eps_changed = w->armed_valid &&
                           (w->armed_count != n || memcmp(w->armed_eps, eps, n * sizeof(uint16_t)) != 0);
        if (eps_changed) {
            (void)minihub_matter_drop_node(node_id);
            is_live = false;
            w->live_seen = false;
            w->armed_ms = 0;
            w->tries = 0;
            ESP_LOGI(TAG, "endpoint set changed for node%llu (%u -> %u), re-arming",
                     (unsigned long long)node_id, (unsigned)w->armed_count, (unsigned)n);
        }

        if (is_live) {
            w->live_seen = true;
            w->armed_ms = 0;
            w->tries = 0;
            memcpy(w->armed_eps, eps, n * sizeof(uint16_t));
            w->armed_count = n;
            w->armed_valid = true;
            continue;
        }
        if (w->armed_ms != 0 && (now - w->armed_ms) < sub_backoff_ms(w)) {
            continue; /* arm still in flight, or backing off an unreachable node */
        }
        if (rearm_now && w->live_seen) {
            continue; /* a resync must not clobber a subscription that already came up */
        }

        esp_err_t err = minihub_matter_subscribe_node(node_id, eps, n);
        memcpy(w->armed_eps, eps, n * sizeof(uint16_t));
        w->armed_count = n;
        w->armed_valid = true;
        w->armed_ms = now;
        if (w->tries < 8) {
            w->tries++;
        }
        if (err == ESP_ERR_NOT_FINISHED) {
            ESP_LOGI(TAG, "subscribing node%llu (%u endpoint(s)), retry in %lldms",
                     (unsigned long long)node_id, (unsigned)n, (long long)sub_backoff_ms(w));
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "subscribe node%llu not accepted: %s",
                     (unsigned long long)node_id, esp_err_to_name(err));
        }
    }
}

static esp_err_t parse_devices(const cJSON *cfg, size_t *out_count)
{
    *out_count = 0;
    const cJSON *devices = cJSON_GetObjectItem(cfg, "devices");
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        if (*out_count >= MAX_MATTER_DEVICES) {
            break;
        }
        matter_device_t *dev = &s_build[*out_count];
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
        (*out_count)++;
    }
    return ESP_OK;
}

static esp_err_t matter_adapter_init(const cJSON *config)
{
    s_started = false;
    size_t n = 0;
    if (config) {
        parse_devices(config, &n);
    }
    publish_device_table(n);
    /* Host-static RPC → Store 「已安装」shows as 内置 (managed=false). */
    (void)minihub_module_register_rpc("matter", matter_module_rpc);
    (void)minihub_matter_set_identity_cb(on_matter_identity, NULL);
    /* Registered once for the life of the process and never cleared: the Matter side
     * keeps the pair non-atomically, so clearing it from stop() would race a report. */
    (void)minihub_matter_set_attr_cb(on_matter_attr, NULL);
    return ESP_OK;
}

/* Register or refresh every configured device's point in place. minihub_point_register()
 * overwrites an existing id, so calling this again never removes and re-adds a card. */
static void publish_points(void)
{
    size_t n = snapshot_devices(s_snap, MAX_MATTER_DEVICES);
    s_snap_count = n;
    for (size_t i = 0; i < n; i++) {
        minihub_point_t point = {0};
        strncpy(point.point_id, s_snap[i].point_id, sizeof(point.point_id) - 1);
        strncpy(point.type, s_snap[i].type, sizeof(point.type) - 1);
        strncpy(point.adapter, "matter", sizeof(point.adapter) - 1);
        const bool commissioning = s_snap[i].commission_phase[0] != '\0';
        strncpy(point.value, commissioning ? "…" : "off", sizeof(point.value) - 1);
        /* Pending/failed commission: show as offline so controls stay disabled. */
        point.online = commissioning ? false : minihub_matter_ready();
        cJSON *props = cJSON_CreateObject();
        if (props) {
            char name[48];
            snprintf(name, sizeof(name), "node%llu·ep%u",
                     (unsigned long long)s_snap[i].node_id,
                     (unsigned)s_snap[i].endpoint_id);
            cJSON_AddStringToObject(props, "name", name);
            if (s_snap[i].transport[0]) {
                cJSON_AddStringToObject(props, "transport", s_snap[i].transport);
            }
            if (commissioning) {
                cJSON_AddTrueToObject(props, "commissioning");
                cJSON_AddStringToObject(props, "commission_phase", s_snap[i].commission_phase);
                if (s_snap[i].commission_error[0]) {
                    cJSON_AddStringToObject(props, "commission_error", s_snap[i].commission_error);
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
        apply_identity_props(s_snap[i].point_id, s_snap[i].node_id,
                             cached_identity(s_snap[i].node_id));
        /* apply_identity_props() writes props via minihub_point_set(), which marks
         * any write as online — re-assert the state the row actually earned. */
        (void)minihub_point_set_online(s_snap[i].point_id,
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
        subscribe_commissioned(true);
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
    size_t n = 0;
    parse_devices(cfg, &n);
    publish_device_table(n);
    publish_points();
    /* A successful commissioning turns a 配网中 row into a live one that has no
     * subscription yet. subscribe_node() only hands a GetConnectedDevice to CHIP and
     * does not wait for the answer, so it stays safe on this httpd task. */
    subscribe_commissioned(true);
    return ESP_OK;
}

static esp_err_t matter_adapter_handle_command(const minihub_command_t *cmd)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    matter_device_t dev;
    if (!find_device_copy(cmd->point_id, &dev)) {
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

    esp_err_t err = minihub_matter_onoff(dev.node_id, dev.endpoint_id, target_on);
    if (err == ESP_OK) {
        (void)minihub_point_set(dev.point_id, target_on ? "on" : "off", NULL);
    }
    return err;
}

static esp_err_t matter_adapter_poll(void)
{
    if (!s_started || !minihub_matter_ready()) {
        return ESP_OK;
    }
    /* Matter read/invoke needs deep CHIP stack; adapter_poll runs on a small task.
     * Retry Basic Information if CASE was not ready at start. The tick is 5 s
     * (adapter_registry.c:598), so %12 is a one-minute pass — enough time for an arm
     * to establish before subscribe_commissioned() decides whether to re-arm. */
    static int s_ticks;
    s_ticks++;
    if (s_ticks <= 60 && (s_ticks % 20) == 0) {
        interview_unique_nodes();
    }
    if ((s_ticks % 12) == 0) {
        /* Re-arms rows the terminated callback dropped; never re-sends an arm that is
         * still in flight (per-node backoff, see sub_backoff_ms). */
        subscribe_commissioned(false);
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
