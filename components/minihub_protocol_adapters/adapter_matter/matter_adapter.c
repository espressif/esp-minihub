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

#ifndef CONFIG_MINIHUB_MATTER_MAX_NODES
#define CONFIG_MINIHUB_MATTER_MAX_NODES 128
#endif
/* Capacity comes from Kconfig (MINIHUB_MATTER_MAX_NODES). It is only a hard
 * safety ceiling — the live tables are singly-linked lists grown per node, so
 * memory tracks the actual device count and a node is freed the moment it leaves
 * the commissioned set. Nothing is reserved up front. */
#define MAX_MATTER_DEVICES ((int)CONFIG_MINIHUB_MATTER_MAX_NODES)

/* OnOff cluster (0x0006), attribute OnOff (0x0000). A subscription can carry other
 * attributes, so the report sink filters on both ids rather than trusting the path. */
#define MATTER_ONOFF_CLUSTER 0x0006
#define MATTER_ONOFF_ATTR    0x0000

/* Re-arm pacing for minihub_matter_subscribe_node(). An arm that is still in flight
 * must not be re-sent (see subscribe_commissioned() below), so a node that never came
 * up is retried at most every SUB_ARM_MIN_MS, doubling up to SUB_ARM_MAX_MS. */
#define SUB_ARM_MIN_MS  30000
#define SUB_ARM_MAX_MS  600000

typedef struct matter_device_s {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    char transport[16];
    char commission_phase[16];
    char commission_error[96];
    uint64_t node_id;
    uint16_t endpoint_id;
    struct matter_device_s *next;
} matter_device_t;

/* s_dev_head is the live device table, a singly-linked list. A node is a single
 * malloc and is freed as soon as its device leaves the commissioned set, so memory
 * tracks the real count. The head pointer is only swapped under s_devices_mux, and
 * the old list is freed AFTER the swap, so a reader that copied under the mux is
 * never torn or freed underneath. */
static matter_device_t *s_dev_head = NULL;
static size_t s_device_count;
static bool s_started;

/* s_dev_head / s_device_count are read on the CHIP event task (on_matter_attr,
 * on_matter_identity) while matter_adapter_init() and matter_adapter_resync() rebuild
 * the list from an httpd / lifecycle task. The writers parse into a temporary list and
 * publish (swap the head) under this mux with the count written LAST, so a reader never
 * sees a half-built list; the readers copy what they need under the same mux and
 * release it BEFORE calling any minihub_point_* (a portMUX is a spinlock and must not
 * be held across a blocking call). Callers of publish_points/interview/subscribe_*
 * are serialized by the adapter lifecycle lock, so s_snap is single-owner scratch. */
static portMUX_TYPE s_devices_mux = portMUX_INITIALIZER_UNLOCKED;

/* s_snap / s_snap_count: a transient snapshot of the live table, allocated to the
 * actual device count (not the cap) and reused between calls. Refreshed per call and
 * needed because publish_points/subscribe_commissioned do cross-row dedup on it. */
static matter_device_t *s_snap = NULL;
static size_t s_snap_cap;
static size_t s_snap_count;

/* Refresh s_snap to a fresh copy of the live list at the actual device count. Grows
 * the scratch buffer only as needed; the previous buffer is freed on grow. */
static void snapshot_refresh(void)
{
    size_t n = s_device_count;
    if (n == 0) {
        s_snap_count = 0;
        return;
    }
    if (n > s_snap_cap) {
        free(s_snap);
        s_snap = (matter_device_t *)calloc(n + 4, sizeof(*s_snap));
        s_snap_cap = n + 4;
    }
    portENTER_CRITICAL(&s_devices_mux);
    size_t i = 0;
    for (matter_device_t *d = s_dev_head; d && i < n; d = d->next) {
        s_snap[i] = *d;
        s_snap[i].next = NULL;
        i++;
    }
    s_snap_count = i;
    portEXIT_CRITICAL(&s_devices_mux);
}

/* Publish a freshly-built list (build_head, count) as the live table. The build list
 * becomes the live list (ownership transferred — no per-node copy); the old list is
 * swapped out under the mux and freed only after the swap, so a concurrent reader
 * under the mux is never left pointing at freed memory. */
static void publish_device_table(matter_device_t *build_head, size_t n)
{
    if (n > (size_t)MAX_MATTER_DEVICES) {
        /* Visible instead of silently clamping. parse_devices already stops at the
         * cap, so this only trips on a programming error. */
        ESP_LOGE(TAG, "device table overflow: %u devices, cap %d — extra dropped",
                 (unsigned)n, MAX_MATTER_DEVICES);
        n = (size_t)MAX_MATTER_DEVICES;
    }
    matter_device_t *old = NULL;
    portENTER_CRITICAL(&s_devices_mux);
    old = s_dev_head;
    s_dev_head = build_head;
    s_device_count = n;
    portEXIT_CRITICAL(&s_devices_mux);
    while (old) {
        matter_device_t *nx = old->next;
        free(old);
        old = nx;
    }
}

/* Basic Information is only on the fabric, so a republish would otherwise have to
 * re-interview every node — and an interview blocks on the CHIP stack lock, which
 * commissioning holds. Cache it per node so publish_points can rebuild props locally. */
typedef struct matter_identity_cache_s {
    uint64_t node_id;
    minihub_matter_identity_t id;
    struct matter_identity_cache_s *next;
} matter_identity_cache_t;
static matter_identity_cache_t *s_identity_head = NULL;
static size_t s_identity_count;
/* Guards the identity list head/metadata. Readers copy by value under this mux (never
 * hold a pointer into the list), so an append (link a new node) or a removal (unlink +
 * free outside the mux) is safe with respect to concurrent readers. */
static portMUX_TYPE s_identity_mux = portMUX_INITIALIZER_UNLOCKED;

static bool cached_identity(uint64_t node_id, minihub_matter_identity_t *out)
{
    bool found = false;
    portENTER_CRITICAL(&s_identity_mux);
    for (matter_identity_cache_t *c = s_identity_head; c; c = c->next) {
        if (c->node_id == node_id) {
            *out = c->id;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_identity_mux);
    return found;
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
    if (!s_dev_head) {
        return false;
    }
    matter_device_t tmp;
    portENTER_CRITICAL(&s_devices_mux);
    for (matter_device_t *d = s_dev_head; d; d = d->next) {
        if (strcmp(d->point_id, point_id) == 0) {
            tmp = *d;
            tmp.next = NULL;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_devices_mux);
    if (found) {
        *out = tmp;
    }
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

/* remember_identity()/cached_identity()/forget_identity() protect the identity list
 * with s_identity_mux. remember appends a new node (malloc outside the mux, link under
 * it); forget unlinks under the mux and frees the node only after releasing it, so a
 * reader copying under the mux is never torn. */
static void remember_identity(uint64_t node_id, const minihub_matter_identity_t *id)
{
    portENTER_CRITICAL(&s_identity_mux);
    for (matter_identity_cache_t *c = s_identity_head; c; c = c->next) {
        if (c->node_id == node_id) {
            c->id = *id;
            portEXIT_CRITICAL(&s_identity_mux);
            return;
        }
    }
    if (s_identity_count >= (size_t)MAX_MATTER_DEVICES) {
        portEXIT_CRITICAL(&s_identity_mux);
        /* identity cache full — log instead of silently dropping. */
        ESP_LOGE(TAG, "identity cache full (cap %d): node%llu identity dropped",
                 MAX_MATTER_DEVICES, (unsigned long long)node_id);
        return;
    }
    portEXIT_CRITICAL(&s_identity_mux);

    matter_identity_cache_t *c = (matter_identity_cache_t *)calloc(1, sizeof(*c));
    if (!c) {
        ESP_LOGE(TAG, "identity cache alloc failed");
        return;
    }
    c->node_id = node_id;
    c->id = *id;
    portENTER_CRITICAL(&s_identity_mux);
    c->next = s_identity_head;
    s_identity_head = c;
    s_identity_count++;
    portEXIT_CRITICAL(&s_identity_mux);
}

/* the identity cache has no invalidation path — resync must keep it (so it never
 * re-interviews), which means a re-commissioned / factory-reset device would show stale
 * manufacturer/model forever. Drop a node's cached identity when it leaves the
 * commissioned set or fabric so the next commission interviews fresh. */
static void forget_identity(uint64_t node_id)
{
    matter_identity_cache_t *to_free = NULL;
    portENTER_CRITICAL(&s_identity_mux);
    matter_identity_cache_t *prev = NULL;
    for (matter_identity_cache_t *c = s_identity_head; c; c = c->next) {
        if (c->node_id == node_id) {
            if (prev) {
                prev->next = c->next;
            } else {
                s_identity_head = c->next;
            }
            s_identity_count--;
            to_free = c;
            break;
        }
        prev = c;
    }
    portEXIT_CRITICAL(&s_identity_mux);
    if (to_free) {
        free(to_free);
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
     * critical section. This runs on the CHIP event task, so keep the copy small.
     * Size the scratch to the actual device count, not the cap. */
    size_t devcnt = s_device_count;
    if (devcnt == 0) {
        return;
    }
    char (*ids)[MINIHUB_POINT_ID_MAX] =
        (char (*)[MINIHUB_POINT_ID_MAX])calloc(devcnt, sizeof(*ids));
    if (!ids) {
        ESP_LOGE(TAG, "ids scratch alloc failed");
        return;
    }
    size_t m = 0;
    portENTER_CRITICAL(&s_devices_mux);
    for (matter_device_t *d = s_dev_head; d && m < devcnt; d = d->next) {
        if (d->node_id == node_id) {
            strncpy(ids[m], d->point_id, sizeof(ids[m]) - 1);
            ids[m][sizeof(ids[m]) - 1] = '\0';
            m++;
        }
    }
    portEXIT_CRITICAL(&s_devices_mux);
    for (size_t k = 0; k < m; k++) {
        apply_identity_props(ids[k], node_id, id);
    }
    free(ids);
}

static void interview_unique_nodes(void)
{
    snapshot_refresh();
    for (size_t i = 0; i < s_snap_count; i++) {
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
    /* Copy the matching row under the mux; the list may be rebuilt from httpd/poll. */
    matter_device_t match;
    bool found = false;
    portENTER_CRITICAL(&s_devices_mux);
    for (matter_device_t *d = s_dev_head; d; d = d->next) {
        if (d->node_id == node_id && d->endpoint_id == endpoint_id) {
            match = *d;
            match.next = NULL;
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
    /* do NOT call minihub_point_get_snapshot() here — it cJSON_Duplicate()s the
     * whole props object and allocates on the CHIP stack lock. minihub_point_get()
     * returns the Core-owned pointer without duplicating, which is enough to learn
     * whether this write will change the value (minihub_point_set() early-returns on
     * no change, so it is the proof a report landed). Do not free the returned ptr. */
    bool will_change = true;
    minihub_point_t *cur = minihub_point_get(match.point_id);
    if (cur) {
        will_change = strcmp(cur->value, nv) != 0;
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
    /* A real OnOff report proves the device is reachable — mark the card
     * online. The subscription-termination path clears it. */
    (void)minihub_point_set_online(match.point_id, true);
}

/* Per-node subscription bookkeeping. The watch table is a singly-linked list, touched
 * only from the adapter_poll task (start and resync also call subscribe_commissioned(),
 * and hub_reload's lifecycle lock holds adapter_poll out of that window — see
 * minihub_adapter_lifecycle_lock). matter_ctrl only short-circuits
 * minihub_matter_subscribe_node() while a node's subscription is LIVE: re-sending while
 * the arm is still in flight leaks the previous subscribe_command (it owns itself and
 * is deleted in OnDone/OnError) and makes the terminated callback clear live for the
 * wrong row — so a reachable device would show offline in the card and stop reporting
 * until a later re-send. Hence: arm once, then only retry after a backoff that doubles
 * while the node keeps failing to come up, reset when it is live. */
typedef struct matter_sub_watch_s {
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
    struct matter_sub_watch_s *next;
} matter_sub_watch_t;

/* s_sub_watch_head is grown per node (no cap reservation, no realloc of an array). It
 * is only ever touched from the adapter_poll / start / resync paths, which are
 * serialized by the adapter lifecycle lock, so a node pointer returned by sub_watch()
 * stays valid for the rest of the call (nodes are only unlinked in reclaim, before the
 * node in question is used). */
static matter_sub_watch_t *s_sub_watch_head = NULL;
static size_t s_sub_watch_count;

static matter_sub_watch_t *sub_watch(uint64_t node_id)
{
    for (matter_sub_watch_t *w = s_sub_watch_head; w; w = w->next) {
        if (w->node_id == node_id) {
            return w;
        }
    }
    if (s_sub_watch_count >= (size_t)MAX_MATTER_DEVICES) {
        return NULL;
    }
    matter_sub_watch_t *w = (matter_sub_watch_t *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->node_id = node_id;
    w->next = s_sub_watch_head;
    s_sub_watch_head = w;
    s_sub_watch_count++;
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
 * freed) is dropped via minihub_matter_drop_node() and its watch node removed. A node
 * whose endpoint set changed is also dropped, then re-armed — a live subscription never
 * re-reads its endpoint list, and arming a second one while the first is live is the
 * double-arm problem, so the old one is torn down before re-arming. */
static void subscribe_commissioned(bool rearm_now)
{
    if (!minihub_matter_ready()) {
        return;
    }
    snapshot_refresh();

    /* Phase A — reclaim watched nodes no longer commissioned. */
    {
        matter_sub_watch_t *prev = NULL;
        matter_sub_watch_t *w = s_sub_watch_head;
        while (w) {
            matter_sub_watch_t *nx = w->next;
            uint16_t want[16];
            bool still = node_wanted_eps(w->node_id, want, 16) != 0;
            if (!still) {
                (void)minihub_matter_drop_node(w->node_id);
                forget_identity(w->node_id); /* drop stale identity with the node */
                ESP_LOGI(TAG, "reclaimed watch for departed node%llu",
                         (unsigned long long)w->node_id);
                if (prev) {
                    prev->next = nx;
                } else {
                    s_sub_watch_head = nx;
                }
                s_sub_watch_count--;
                free(w);
            } else {
                prev = w;
            }
            w = nx;
        }
    }

    /* Size the liveness view to the actual watch count, not the cap. */
    size_t live_cap = s_sub_watch_count + 16;
    bool *live = (bool *)calloc(live_cap, sizeof(bool));
    uint64_t *live_node = (uint64_t *)calloc(live_cap, sizeof(uint64_t));
    if (!live || !live_node) {
        ESP_LOGE(TAG, "sub live snapshot alloc failed");
        free(live);
        free(live_node);
        return;
    }
    size_t live_count = sub_live_snapshot(live, live_node, live_cap);
    const int64_t now = esp_timer_get_time() / 1000;

    /* Phase B — arm / backoff per commissioned node. */
    for (size_t i = 0; i < s_snap_count; i++) {
        if (s_snap[i].commission_phase[0] || s_snap[i].node_id < 2) {
            continue;
        }
        /* one call per node, not per endpoint — see the subscribe_node contract */
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
            continue; /* skip only this node; never abort the whole re-arm loop (was `return`) */
        }

        bool is_live = false;
        for (size_t r = 0; r < live_count; r++) {
            if (live_node[r] == node_id) {
                is_live = live[r];
                break;
            }
        }
        /* online reflects this device's real subscription liveness, not just
         * commissioner readiness — a dead or never-established subscription shows the
         * card offline. (on_matter_attr also flips it online on a real report.) */
        if (!s_snap[i].commission_phase[0]) {
            (void)minihub_point_set_online(s_snap[i].point_id, is_live);
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
        /* after (re)arming, re-read the OnOff value so a subscription drop
         * that clobbered the live value self-heals instead of sticking at "off". */
        if (err == ESP_OK || err == ESP_ERR_NOT_FINISHED) {
            bool onoff_dummy = false;
            (void)minihub_matter_read_onoff(node_id, eps[0], &onoff_dummy);
        }
        if (err == ESP_ERR_NOT_FINISHED) {
            ESP_LOGI(TAG, "subscribing node%llu (%u endpoint(s)), retry in %lldms",
                     (unsigned long long)node_id, (unsigned)n, (long long)sub_backoff_ms(w));
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "subscribe node%llu not accepted: %s",
                     (unsigned long long)node_id, esp_err_to_name(err));
        }
    }
    free(live);
    free(live_node);
}

/* Parse the configured devices into a freshly built linked list (no cap reservation).
 * The caller publishes it (taking ownership) and the previous live list is freed then.
 * On an allocation failure the partially-built list is freed and ESP_ERR_NO_MEM. */
static esp_err_t parse_devices(const cJSON *cfg, matter_device_t **out_head, size_t *out_count)
{
    *out_head = NULL;
    *out_count = 0;
    const cJSON *devices = cJSON_GetObjectItem(cfg, "devices");
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }

    matter_device_t *head = NULL;
    matter_device_t *tail = NULL;
    size_t n = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        if (!cJSON_IsString(point_id)) {
            continue;
        }
        if (n >= (size_t)MAX_MATTER_DEVICES) {
            ESP_LOGE(TAG, "device table full (cap %d): extra configured device(s) dropped",
                     MAX_MATTER_DEVICES);
            break;
        }
        matter_device_t *dev = (matter_device_t *)calloc(1, sizeof(*dev));
        if (!dev) {
            while (head) {
                matter_device_t *nx = head->next;
                free(head);
                head = nx;
            }
            return ESP_ERR_NO_MEM;
        }

        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *node_id = cJSON_GetObjectItem(item, "node_id");
        const cJSON *endpoint_id = cJSON_GetObjectItem(item, "endpoint_id");
        const cJSON *transport = cJSON_GetObjectItem(item, "transport");
        const cJSON *cphase = cJSON_GetObjectItem(item, "commission_phase");
        const cJSON *cerr = cJSON_GetObjectItem(item, "commission_error");

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

        /* append, preserving config order */
        dev->next = NULL;
        if (tail) {
            tail->next = dev;
        } else {
            head = dev;
        }
        tail = dev;
        n++;
    }
    *out_head = head;
    *out_count = n;
    return ESP_OK;
}

static esp_err_t matter_adapter_init(const cJSON *config)
{
    s_started = false;
    matter_device_t *build = NULL;
    size_t n = 0;
    if (config) {
        parse_devices(config, &build, &n);
    }
    publish_device_table(build, n);
    /* Host-static RPC → Store 「已安装」shows as 内置 (managed=false). */
    (void)minihub_module_register_rpc("matter", matter_module_rpc);
    (void)minihub_matter_set_identity_cb(on_matter_identity, NULL);
    /* Registered once for the life of the process and never cleared: the Matter side
     * keeps the pair non-atomically, so clearing it from stop() would race a report. */
    (void)minihub_matter_set_attr_cb(on_matter_attr, NULL);
    return ESP_OK;
}

/* Reconcile persisted commission_phase at boot — a phase left dangling from a previous
 * run means the card would be stuck in 配网中 forever. Clearing it here (and persisting
 * the clear) lets the card come up as a real device. */
static void reconcile_commission_phases(void)
{
    for (size_t i = 0; i < s_snap_count; i++) {
        if (s_snap[i].commission_phase[0]) {
            ESP_LOGW(TAG, "node%llu point %s: clearing stale commission_phase '%s'",
                     (unsigned long long)s_snap[i].node_id, s_snap[i].point_id,
                     s_snap[i].commission_phase);
            (void)minihub_matter_devices_set_commission_phase(s_snap[i].node_id, NULL, NULL);
            s_snap[i].commission_phase[0] = '\0';
        }
    }
}

/* Register or refresh every configured device's point in place.
 * minihub_point_register() overwrites an existing id, so calling this again never
 * removes and re-adds a card. transport lives in props, not on minihub_point_t. */
static void publish_points(void)
{
    snapshot_refresh();
    for (size_t i = 0; i < s_snap_count; i++) {
        minihub_point_t point = {0};
        strncpy(point.point_id, s_snap[i].point_id, sizeof(point.point_id) - 1);
        strncpy(point.type, s_snap[i].type[0] ? s_snap[i].type : "light", sizeof(point.type) - 1);
        strncpy(point.adapter, "matter", sizeof(point.adapter) - 1);
        const bool commissioning = s_snap[i].commission_phase[0] != '\0';
        /* Resync must not blow away a live device's last-known value. */
        minihub_point_t *cur = minihub_point_get(s_snap[i].point_id);
        if (cur && !commissioning && cur->online && strcmp(cur->value, "off") != 0) {
            strncpy(point.value, cur->value, sizeof(point.value) - 1);
            point.updated_ms = cur->updated_ms;
        } else {
            strncpy(point.value, commissioning ? "…" : "off", sizeof(point.value) - 1);
        }
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
        minihub_matter_identity_t idbuf;
        const minihub_matter_identity_t *pid =
            cached_identity(s_snap[i].node_id, &idbuf) ? &idbuf : NULL;
        apply_identity_props(s_snap[i].point_id, s_snap[i].node_id, pid);
        (void)minihub_point_set_online(s_snap[i].point_id,
                                       !commissioning && minihub_matter_ready());
    }
}

/* Drop matter-adapter points whose device is no longer in the configured table. */
static void deregister_vanished_points(void)
{
    minihub_point_t *list = NULL;
    size_t count = 0;
    if (minihub_point_list_by_adapter("matter", &list, &count) != ESP_OK || count == 0) {
        if (list) {
            minihub_point_list_free(list, count);
        }
        return;
    }
    for (size_t i = 0; i < count; i++) {
        bool still_there = false;
        for (size_t k = 0; k < s_snap_count; k++) {
            if (strcmp(list[i].point_id, s_snap[k].point_id) == 0) {
                still_there = true;
                break;
            }
        }
        if (!still_there) {
            ESP_LOGI(TAG, "deregistering vanished matter point %s", list[i].point_id);
            (void)minihub_point_unregister(list[i].point_id);
        }
    }
    minihub_point_list_free(list, count);
}

esp_err_t matter_adapter_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    /* Commissioner lives in minihub_matter_init(). Without this call s_started
     * in matter_ctrl stays false and every commission returns
     * "matter commissioner not ready". */
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

    snapshot_refresh();
    reconcile_commission_phases();
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

esp_err_t matter_adapter_poll(void)
{
    if (!s_started || !minihub_matter_ready()) {
        return ESP_OK;
    }
    /* Matter read/invoke needs the CHIP stack lock; adapter_poll must not take
     * it on every 5 s tick. Retry Basic Information if CASE was not ready at
     * start. %20 is about 100 s; %12 is about one minute — long enough for an
     * arm to establish before subscribe_commissioned() decides to re-arm. */
    static int s_ticks;
    s_ticks++;
    if (s_ticks <= 60 && (s_ticks % 20) == 0) {
        interview_unique_nodes();
    }
    if ((s_ticks % 12) == 0) {
        subscribe_commissioned(false);
    }
    return ESP_OK;
}

esp_err_t matter_adapter_resync(const cJSON *cfg)
{
    /* config may have removed devices → publish the new set (old list freed in
     * publish). Then republish points, drop vanished ones, re-arm subscriptions. */
    matter_device_t *build = NULL;
    size_t n = 0;
    if (cfg) {
        parse_devices(cfg, &build, &n);
    }
    publish_device_table(build, n);
    publish_points();
    deregister_vanished_points();
    subscribe_commissioned(true);
    return ESP_OK;
}

esp_err_t matter_adapter_stop(void)
{
    s_started = false;
    /* The identity/attr callbacks stay registered for the process lifetime (they are
     * never cleared), so the live lists are intentionally kept alive here. */
    return ESP_OK;
}

static esp_err_t matter_adapter_handle_command(const minihub_command_t *cmd)
{
    if (!s_started || !cmd) {
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

#endif /* CONFIG_MINIHUB_MATTER_ADAPTER */
