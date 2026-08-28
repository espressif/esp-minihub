/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zigbee_stack.h"
#include "zigbee_map.h"
#include "minihub_product_params.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_MINIHUB_ZIGBEE_SDK

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "minihub_task.h"

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
#include "esp_coexist.h"
#endif

#if CONFIG_MINIHUB_BLE_MESH_SDK
void ble_mesh_stack_yield_rf(void);
bool ble_mesh_stack_rf_critical(void);
#endif

#include "esp_zigbee.h"
#include "ezbee/af.h"
#include "ezbee/aps.h"
#include "ezbee/nwk.h"
#include "ezbee/secur.h"
#include "ezbee/zha.h"
#include "ezbee/platform/log.h"

#include <stdarg.h>

/* Optional coexistence hook (custom lib builds); weak so stock Registry lib links. */
void nwk_data_frame_disable_extaddr(void) __attribute__((weak));

static const char *TAG = "zb_stack";
static const char *EZB_TAG = "ezb";

/*
 * Default ezb_plat_log is weak and maps DEBUG/VERBOSE below the project's
 * CONFIG_LOG_MAXIMUM_LEVEL (INFO) — so TransportKey / UpdateDevice never show.
 * Override: promote all stack traces to INFO under tag "ezb".
 */
void ezb_plat_log(ezb_log_level_t log_level, const char *format, ...)
{
    if (!format || log_level <= EZB_LOG_LEVEL_NONE) {
        return;
    }
    char buf[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (log_level == EZB_LOG_LEVEL_ERROR) {
        ESP_LOGE(EZB_TAG, "%s", buf);
    } else if (log_level == EZB_LOG_LEVEL_WARN) {
        ESP_LOGW(EZB_TAG, "%s", buf);
    } else {
        ESP_LOGI(EZB_TAG, "%s", buf);
    }
}

static const char *aps_addr_mode_str(uint8_t mode)
{
    switch (mode) {
    case EZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT:
        return "none";
    case EZB_APS_ADDR_MODE_16_GROUP_ENDP_NOT_PRESENT:
        return "group";
    case EZB_APS_ADDR_MODE_16_ENDP_PRESENT:
        return "short";
    case EZB_APS_ADDR_MODE_64_ENDP_PRESENT:
        return "ext";
    default:
        return "?";
    }
}

static void log_aps_addr(const char *what, const ezb_address_t *addr, uint8_t ep)
{
    if (!addr) {
        return;
    }
    if (addr->addr_mode == EZB_APS_ADDR_MODE_16_ENDP_PRESENT) {
        ESP_LOGD(TAG, "%s mode=%s short=0x%04x ep=%u", what, aps_addr_mode_str(addr->addr_mode),
                 addr->u.short_addr, ep);
    } else if (addr->addr_mode == EZB_APS_ADDR_MODE_16_GROUP_ENDP_NOT_PRESENT) {
        ESP_LOGD(TAG, "%s mode=%s group=0x%04x ep=%u", what, aps_addr_mode_str(addr->addr_mode),
                 addr->u.group_addr.group, ep);
    } else if (addr->addr_mode == EZB_APS_ADDR_MODE_64_ENDP_PRESENT) {
        ESP_LOGD(TAG, "%s mode=%s ieee=0x%016llx ep=%u", what, aps_addr_mode_str(addr->addr_mode),
                 (unsigned long long)addr->u.extended_addr.u64, ep);
    } else {
        ESP_LOGD(TAG, "%s mode=%s ep=%u", what, aps_addr_mode_str(addr->addr_mode), ep);
    }
}

static void aps_data_confirm_cb(const ezb_apsde_data_confirm_t *confirm)
{
    if (!confirm) {
        return;
    }
    ESP_LOGD(TAG, "APS confirm status=%u cluster=0x%04x profile=0x%04x src_ep=%u dst_ep=%u",
             (unsigned)confirm->status, confirm->cluster_id, confirm->profile_id, confirm->src_endpoint,
             confirm->dst_endpoint);
    log_aps_addr("APS confirm dst", &confirm->dst_address, confirm->dst_endpoint);
}

static bool aps_data_indication_cb(const ezb_apsde_data_ind_t *ind)
{
    if (!ind) {
        return false;
    }
    ESP_LOGD(TAG,
             "APS ind status=%u secur=%u key_idx=%u lqi=%u rssi=%d cluster=0x%04x profile=0x%04x "
             "len=%u src_ep=%u dst_ep=%u",
             (unsigned)ind->status, (unsigned)ind->security_status, (unsigned)ind->key_index,
             (unsigned)ind->lqi, (int)ind->rssi, ind->cluster_id, ind->profile_id,
             (unsigned)ind->asdu_length, ind->src_endpoint, ind->dst_endpoint);
    log_aps_addr("APS ind src", &ind->src_address, ind->src_endpoint);
    log_aps_addr("APS ind dst", &ind->dst_address, ind->dst_endpoint);
    return false; /* let stack continue processing */
}

/* Well-known Zigbee HA global TC link key: ASCII "ZigBeeAlliance09" */
static const uint8_t k_zb_global_tclk[EZB_CCM_KEY_SIZE] = {
    0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C, 0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39,
};

/*
 * Manual apsme_transport_key_request was removed: sniffer showed retries often
 * became NWK-key Data that joining EDs cannot decrypt, while the stack already
 * emits a correct APS Transport Key (Key-Transport Key) on the first poll.
 * On DEVICE_UPDATE we only ensure ZigBeeAlliance09 TCLK for that path.
 */
void *aps_secur_key_pair_find_or_create(const ezb_extaddr_t *addr);
int aps_secur_joiner_setup_key_pair(void *key_pair);
void *aps_secur_get_key_pair_by_addr(const ezb_extaddr_t *addr);

#define ZB_COORD_ENDPOINT 1
#define ZB_STORAGE_PARTITION "zb_storage"
#define DISCOVER_STEP_TIMEOUT_MS 12000
#define BDB_RETRY_MS 1000
#define DISC_QUEUE_DEPTH 4
#define DISC_MAX_EPS 8
#define DISC_MAX_ATTEMPTS 6
#define DISC_START_DELAY_MS 2500
#define DISC_RETRY_DELAY_MS 3000

#define REPORT_MIN_INTERVAL_S 10
#define REPORT_MAX_INTERVAL_S 300
#define ENERGY_REPORT_MAX_INTERVAL_S 600
#define METER_POLL_INTERVAL_S 15
#define TEMP_CHANGE_CENTI 50   /* 0.5 °C */
#define POWER_CHANGE_RAW 1
#define VOLTAGE_CHANGE_RAW 1
#define CURRENT_CHANGE_RAW 1
#define ENERGY_CHANGE_RAW 1

typedef struct {
    uint16_t short_addr;
    uint16_t ac_v_m;
    uint16_t ac_v_d;
    uint16_t ac_i_m;
    uint16_t ac_i_d;
    uint16_t ac_p_m;
    uint16_t ac_p_d;
    uint32_t energy_m;
    uint32_t energy_d;
} meter_scale_t;

typedef struct {
    bool in_use;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t caps;
} meter_sub_t;

static meter_scale_t s_meter_scale[MINIHUB_ZB_MAX_DEVICES];
static size_t s_meter_scale_count;
static meter_sub_t s_meter_subs[MINIHUB_ZB_MAX_DEVICES];
static esp_timer_handle_t s_meter_poll_timer;
#define HUM_CHANGE_CENTI 100   /* 1 %RH */

/* Prefer ch 20–26 for form (less overlap with common Wi-Fi 1/6/11).
 * Full 11–26 remains as secondary fallback during energy scan. */
#define ZB_PRIMARY_CHANNEL_MASK 0x07E00000U   /* channels 20–26 */
#define ZB_SECONDARY_CHANNEL_MASK 0x001FF800U /* channels 11–19 */

#define ZB_MANUFACTURER_NAME "\x07" "MiniHub"
#define ZB_MODEL_IDENTIFIER "\x07" CONFIG_IDF_TARGET

#define ZB_ZC_DEVICE_CONFIG()                               \
    {                                                       \
        .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,     \
        .install_code_policy = false,                       \
        .zczr_config = {                                    \
            .max_children = 32,                             \
        },                                                  \
    }

#define ZB_PLATFORM_CONFIG()                                \
    {                                                       \
        .storage_partition_name = ZB_STORAGE_PARTITION,     \
        .radio_config = {                                   \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,     \
        },                                                  \
    }

#define ZB_DEFAULT_CONFIG()                                 \
    {                                                       \
        .device_config = ZB_ZC_DEVICE_CONFIG(),             \
        .platform_config = ZB_PLATFORM_CONFIG(),            \
    }

static bool s_task_started;
static bool s_stack_ready;
static int64_t s_permit_until_ms;
static minihub_zigbee_device_cb_t s_on_device;
static minihub_zigbee_attr_cb_t s_on_attr;
static zigbee_stack_ready_cb_t s_on_ready;
static void *s_cb_ctx;

typedef enum {
    DISC_PHASE_IDLE = 0,
    DISC_PHASE_ACTIVE_EP,
    DISC_PHASE_MATCH_SENSOR,
    DISC_PHASE_SIMPLE_DESC,
} disc_phase_t;

typedef struct {
    bool in_use;
    disc_phase_t phase;
    uint16_t short_addr;
    uint64_t ieee;
    uint8_t eps[DISC_MAX_EPS];
    uint8_t ep_count;
    uint8_t ep_index;
    uint8_t best_ep;
    uint8_t best_caps;
    uint16_t best_device_id;
    uint8_t first_ep;
    bool interview_ok;
    uint8_t attempt;
} discover_job_t;

static discover_job_t s_disc_jobs[DISC_QUEUE_DEPTH];
static discover_job_t *s_disc_active;
static esp_timer_handle_t s_fallback_timer;
static esp_timer_handle_t s_disc_delay_timer;
static esp_timer_handle_t s_bdb_retry_timer;
static uint8_t s_bdb_retry_mode;
static discover_job_t *s_disc_delayed_job;

typedef enum {
    DISC_DEFER_NONE = 0,
    DISC_DEFER_KICK_NEXT,
    DISC_DEFER_MATCH_SENSOR,
    DISC_DEFER_SIMPLE_DESC,
    DISC_DEFER_FINALIZE,
} disc_defer_action_t;

static esp_timer_handle_t s_disc_defer_timer;
static discover_job_t *s_disc_defer_job;
static disc_defer_action_t s_disc_defer_action;

void zigbee_stack_set_hooks(minihub_zigbee_device_cb_t on_device,
                            minihub_zigbee_attr_cb_t on_attr,
                            zigbee_stack_ready_cb_t on_ready,
                            void *ctx)
{
    s_on_device = on_device;
    s_on_attr = on_attr;
    s_on_ready = on_ready;
    s_cb_ctx = ctx;
}

void zigbee_stack_notify_device(const minihub_zigbee_device_t *dev)
{
    if (s_on_device && dev) {
        s_on_device(dev, s_cb_ctx);
    }
}

void zigbee_stack_notify_attr(const minihub_zigbee_attr_evt_t *evt)
{
    if (s_on_attr && evt) {
        s_on_attr(evt, s_cb_ctx);
    }
}

static void log_coordinator_identity(const char *why)
{
    ezb_extaddr_t ieee = {0};
    ezb_nwk_get_extended_address(&ieee);
    ESP_LOGI(TAG, "%s: short=0x%04x ieee=0x%016llx PAN=0x%04x ch=%d", why, ezb_nwk_get_short_address(),
             (unsigned long long)ieee.u64, ezb_nwk_get_panid(), ezb_nwk_get_current_channel());
}

static void mark_stack_ready(void);
static void log_network_key_for_sniffer(void);

static void mark_stack_ready(void)
{
    if (s_stack_ready) {
        return;
    }
    s_stack_ready = true;
    log_coordinator_identity("coordinator ready");
    log_network_key_for_sniffer();
    if (s_on_ready) {
        s_on_ready(s_cb_ctx);
    }
}

static const char *signal_name(ezb_app_signal_type_t t)
{
    switch (t) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        return "SKIP_STARTUP";
    case EZB_ZDO_SIGNAL_LEAVE:
        return "LEAVE";
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION:
        return "LEAVE_INDICATION";
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE:
        return "DEVICE_ANNCE";
    case EZB_ZDO_SIGNAL_DEVICE_UNAVAILABLE:
        return "DEVICE_UNAVAILABLE";
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE:
        return "DEVICE_UPDATE";
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED:
        return "DEVICE_AUTHORIZED";
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        return "BDB_FIRST_START";
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        return "BDB_REBOOT";
    case EZB_BDB_SIGNAL_STEERING:
        return "BDB_STEERING";
    case EZB_BDB_SIGNAL_FORMATION:
        return "BDB_FORMATION";
    case EZB_NWK_SIGNAL_DEVICE_ASSOCIATED:
        return "NWK_ASSOCIATED";
    case EZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        return "NWK_NO_ACTIVE_LINKS";
    case EZB_NWK_SIGNAL_PANID_CONFLICT_DETECTED:
        return "NWK_PANID_CONFLICT";
    case EZB_NWK_SIGNAL_NETWORK_STATUS:
        return "NWK_STATUS";
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        return "PERMIT_JOIN_STATUS";
    default:
        return "OTHER";
    }
}

static const char *upddev_status_str(ezb_zdo_upddev_status_t s)
{
    switch (s) {
    case EZB_ZDO_UPDDEV_SECURE_REJOIN:
        return "secure_rejoin";
    case EZB_ZDO_UPDDEV_UNSECURE_JOIN:
        return "unsecure_join";
    case EZB_ZDO_UPDDEV_DEVICE_LEFT:
        return "left";
    case EZB_ZDO_UPDDEV_TC_REJOIN:
        return "tc_rejoin";
    default:
        return "unknown";
    }
}

static const char *auth_status_str(ezb_zdo_auth_status_t s)
{
    switch (s) {
    case EZB_ZDO_AUTH_STATUS_SUCCESS:
        return "success";
    case EZB_ZDO_AUTH_STATUS_TIMEOUT:
        return "timeout";
    case EZB_ZDO_AUTH_STATUS_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static uint8_t caps_from_simple_desc(const ezb_af_simple_desc_t *simple_desc)
{
    uint8_t caps = 0;
    if (!simple_desc || !simple_desc->app_cluster_list) {
        return 0;
    }
    uint16_t cluster_count =
        (uint16_t)simple_desc->app_input_cluster_count + simple_desc->app_output_cluster_count;
    for (uint16_t i = 0; i < cluster_count; i++) {
        uint16_t cid = simple_desc->app_cluster_list[i];
        if (cid == EZB_ZCL_CLUSTER_ID_ON_OFF) {
            caps |= MINIHUB_ZB_CAP_ONOFF;
        } else if (cid == EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT) {
            caps |= MINIHUB_ZB_CAP_TEMP;
        } else if (cid == EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT) {
            caps |= MINIHUB_ZB_CAP_HUMIDITY;
        } else if (cid == EZB_ZCL_CLUSTER_ID_IAS_ZONE) {
            caps |= MINIHUB_ZB_CAP_IAS_ZONE;
        } else if (cid == EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT) {
            caps |= MINIHUB_ZB_CAP_ELECTRICAL;
        } else if (cid == EZB_ZCL_CLUSTER_ID_METERING) {
            caps |= MINIHUB_ZB_CAP_ENERGY;
        }
    }
    return caps;
}

static void log_simple_desc_clusters(uint16_t short_addr, uint8_t ep, const ezb_af_simple_desc_t *desc)
{
    if (!desc || !desc->app_cluster_list) {
        return;
    }
    uint16_t n = (uint16_t)desc->app_input_cluster_count + desc->app_output_cluster_count;
    /* Compact dump — enough to see sensor vs light without flooding. */
    char buf[160];
    size_t off = 0;
    for (uint16_t i = 0; i < n && off + 8 < sizeof(buf); i++) {
        int w = snprintf(buf + off, sizeof(buf) - off, "%s0x%04x", i ? "," : "", desc->app_cluster_list[i]);
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
    ESP_LOGI(TAG, "simple desc 0x%04x ep=%u in=%u out=%u profile=0x%04x clusters=[%s]", short_addr, ep,
             desc->app_input_cluster_count, desc->app_output_cluster_count, desc->app_profile_id, buf);
}

static bool caps_better(uint8_t candidate, uint8_t current)
{
    bool cand_sensor = (candidate & MINIHUB_ZB_CAP_SENSOR_MASK) != 0;
    bool cur_sensor = (current & MINIHUB_ZB_CAP_SENSOR_MASK) != 0;
    if (cand_sensor != cur_sensor) {
        return cand_sensor;
    }
    if ((candidate & MINIHUB_ZB_CAP_ONOFF) != (current & MINIHUB_ZB_CAP_ONOFF)) {
        return (candidate & MINIHUB_ZB_CAP_ONOFF) != 0;
    }
    /* Prefer richer caps (e.g. onoff+metering over onoff-only). */
    if (__builtin_popcount(candidate) != __builtin_popcount(current)) {
        return __builtin_popcount(candidate) > __builtin_popcount(current);
    }
    return candidate != 0 && current == 0;
}

static void emit_classified(uint16_t short_addr, uint64_t ieee, uint8_t endpoint, uint8_t caps,
                            uint16_t app_device_id)
{
    minihub_zigbee_device_t dev = {0};
    const char *type = minihub_zb_classify_type(caps, app_device_id);
    minihub_zb_format_point_id(dev.point_id, sizeof(dev.point_id), type, short_addr);
    strncpy(dev.type, type, sizeof(dev.type) - 1);
    dev.ieee = ieee;
    dev.short_addr = short_addr;
    dev.endpoint = endpoint ? endpoint : 1;
    /* Keep caps as discovered — do not invent ONOFF for unknown devices. */
    dev.caps = caps;
    ESP_LOGI(TAG, "device 0x%04x type=%s caps=0x%02x ep=%u device_id=0x%04x ieee=0x%016llx", short_addr,
             type, caps, dev.endpoint, app_device_id, (unsigned long long)ieee);
    if (caps == 0) {
        ESP_LOGW(TAG, "skip point register for 0x%04x — interview found no clusters yet", short_addr);
        return;
    }
    zigbee_stack_notify_device(&dev);
    if ((dev.caps & MINIHUB_ZB_CAP_SENSOR_MASK) || (dev.caps & MINIHUB_ZB_CAP_METERING_MASK)) {
        (void)zigbee_stack_subscribe(dev.short_addr, dev.endpoint, dev.ieee, dev.caps);
    }
    zigbee_stack_subscribe_product_params(dev.short_addr, dev.endpoint, dev.ieee);
}

static void arm_disc_timer(void)
{
    if (!s_fallback_timer) {
        return;
    }
    esp_timer_stop(s_fallback_timer);
    esp_timer_start_once(s_fallback_timer, DISCOVER_STEP_TIMEOUT_MS * 1000ULL);
}

static void disc_job_release(discover_job_t *job)
{
    if (!job) {
        return;
    }
    memset(job, 0, sizeof(*job));
}

static void disc_kick_next(void);
static void disc_defer_step(discover_job_t *job, disc_defer_action_t action);
static void disc_request_simple_desc(discover_job_t *job);
static void disc_request_match_sensor(discover_job_t *job);
static void schedule_disc_delay(discover_job_t *job, int delay_ms);
static void active_ep_cb(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx);
static void simple_desc_cb(const ezb_zdo_simple_desc_req_result_t *result, void *user_ctx);
static void match_sensor_cb(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx);

static void disc_finalize(discover_job_t *job)
{
    if (!job || !job->in_use) {
        return;
    }
    esp_timer_stop(s_fallback_timer);
    uint8_t ep = job->best_caps ? job->best_ep : (job->first_ep ? job->first_ep : 1);
    uint8_t caps = job->best_caps;
    uint16_t device_id = job->best_device_id;
    uint16_t short_addr = job->short_addr;
    uint64_t ieee = job->ieee;
    bool ok = job->interview_ok && caps != 0;
    uint8_t attempt = job->attempt;

    ESP_LOGI(TAG, "discover done 0x%04x → ep=%u caps=0x%02x device_id=0x%04x ok=%d attempt=%u",
             short_addr, ep, caps, device_id, (int)ok, attempt);

    if (s_disc_active == job) {
        s_disc_active = NULL;
    }

    if (!ok) {
        if (attempt + 1 < DISC_MAX_ATTEMPTS) {
            job->attempt = (uint8_t)(attempt + 1);
            job->phase = DISC_PHASE_IDLE;
            job->ep_count = 0;
            job->ep_index = 0;
            job->best_ep = 0;
            job->best_caps = 0;
            job->best_device_id = 0;
            job->first_ep = 0;
            job->interview_ok = false;
            ESP_LOGW(TAG, "interview incomplete for 0x%04x — retry %u/%u in %d ms (sleepy ED)",
                     short_addr, job->attempt + 1, DISC_MAX_ATTEMPTS, DISC_RETRY_DELAY_MS);
            schedule_disc_delay(job, DISC_RETRY_DELAY_MS);
            disc_defer_step(NULL, DISC_DEFER_KICK_NEXT);
            return;
        }
        ESP_LOGW(TAG, "interview failed for 0x%04x after %u attempts — not registering as switch",
                 short_addr, DISC_MAX_ATTEMPTS);
        disc_job_release(job);
        disc_defer_step(NULL, DISC_DEFER_KICK_NEXT);
        return;
    }

    disc_job_release(job);
    emit_classified(short_addr, ieee, ep, caps, device_id);
    disc_defer_step(NULL, DISC_DEFER_KICK_NEXT);
}

static void disc_request_match_sensor(discover_job_t *job)
{
    job->phase = DISC_PHASE_MATCH_SENSOR;
    static uint16_t s_in_clusters[2];
    s_in_clusters[0] = EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT;
    s_in_clusters[1] = EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
    ESP_LOGI(TAG, "discover → MatchDesc (temp+humidity) 0x%04x", job->short_addr);
    ezb_zdo_match_desc_req_t req = {
        .dst_nwk_addr = job->short_addr,
        .field =
            {
                .nwk_addr_of_interest = job->short_addr,
                .profile_id = EZB_AF_HA_PROFILE_ID,
                .num_in_clusters = 2,
                .num_out_clusters = 0,
                .cluster_list = s_in_clusters,
            },
        .cb = match_sensor_cb,
        .user_ctx = job,
    };
    if (ezb_zdo_match_desc_req(&req) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "match_desc_req failed for 0x%04x — try EP1 SimpleDesc", job->short_addr);
        job->eps[0] = 1;
        job->ep_count = 1;
        job->first_ep = 1;
        job->ep_index = 0;
        disc_request_simple_desc(job);
        return;
    }
    arm_disc_timer();
}

static void disc_request_active_ep(discover_job_t *job)
{
    job->phase = DISC_PHASE_ACTIVE_EP;
    ESP_LOGI(TAG, "discover → ActiveEPReq 0x%04x (attempt %u)", job->short_addr, job->attempt + 1);
    ezb_zdo_active_ep_req_t req = {
        .dst_nwk_addr = job->short_addr,
        .field = {.nwk_addr_of_interest = job->short_addr},
        .cb = active_ep_cb,
        .user_ctx = job,
    };
    if (ezb_zdo_active_ep_req(&req) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "active_ep_req send failed for 0x%04x — MatchDesc", job->short_addr);
        disc_request_match_sensor(job);
        return;
    }
    arm_disc_timer();
}

static void disc_request_simple_desc(discover_job_t *job)
{
    if (job->ep_index >= job->ep_count) {
        disc_finalize(job);
        return;
    }
    job->phase = DISC_PHASE_SIMPLE_DESC;
    uint8_t ep = job->eps[job->ep_index];
    ESP_LOGI(TAG, "discover → SimpleDescReq 0x%04x ep=%u (%u/%u)", job->short_addr, ep,
             job->ep_index + 1, job->ep_count);
    ezb_zdo_simple_desc_req_t req = {
        .dst_nwk_addr = job->short_addr,
        .field =
            {
                .nwk_addr_of_interest = job->short_addr,
                .endpoint = ep,
            },
        .cb = simple_desc_cb,
        .user_ctx = job,
    };
    if (ezb_zdo_simple_desc_req(&req) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "simple_desc_req failed ep=%u", ep);
        job->ep_index++;
        disc_request_simple_desc(job);
        return;
    }
    arm_disc_timer();
}

static void active_ep_cb(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx)
{
    discover_job_t *job = user_ctx;
    if (!job || !job->in_use || job != s_disc_active || job->phase != DISC_PHASE_ACTIVE_EP) {
        ESP_LOGD(TAG, "active_ep_cb ignored (stale/mismatch)");
        return;
    }

    job->ep_count = 0;
    if (result && result->error == EZB_ERR_NONE && result->rsp &&
        result->rsp->status == EZB_ZDP_STATUS_SUCCESS && result->rsp->active_ep_list &&
        result->rsp->active_ep_count > 0) {
        uint8_t n = result->rsp->active_ep_count;
        if (n > DISC_MAX_EPS) {
            n = DISC_MAX_EPS;
        }
        memcpy(job->eps, result->rsp->active_ep_list, n);
        job->ep_count = n;
        job->first_ep = job->eps[0];
        char eps_buf[48];
        size_t off = 0;
        for (uint8_t i = 0; i < n && off + 4 < sizeof(eps_buf); i++) {
            int w = snprintf(eps_buf + off, sizeof(eps_buf) - off, "%s%u", i ? "," : "", job->eps[i]);
            if (w <= 0) {
                break;
            }
            off += (size_t)w;
        }
        ESP_LOGI(TAG, "active eps for 0x%04x: count=%u [%s]", job->short_addr, job->ep_count, eps_buf);
    } else {
        ESP_LOGW(TAG, "active_ep failed for 0x%04x err=%d zdp=0x%02x — MatchDesc temp/humidity",
                 job->short_addr, result ? (int)result->error : -1,
                 (result && result->rsp) ? (unsigned)result->rsp->status : 0xffu);
        disc_defer_step(job, DISC_DEFER_MATCH_SENSOR);
        return;
    }
    job->ep_index = 0;
    disc_defer_step(job, DISC_DEFER_SIMPLE_DESC);
}

static void match_sensor_cb(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    discover_job_t *job = user_ctx;
    if (!job || !job->in_use || job != s_disc_active || job->phase != DISC_PHASE_MATCH_SENSOR) {
        ESP_LOGD(TAG, "match_sensor_cb ignored (stale/mismatch)");
        return;
    }

    if (result && result->error == EZB_ERR_NONE && result->rsp &&
        result->rsp->status == EZB_ZDP_STATUS_SUCCESS && result->rsp->match_length > 0 &&
        result->rsp->match_list) {
        uint8_t ep = result->rsp->match_list[0];
        job->best_ep = ep;
        job->first_ep = ep;
        job->best_caps = MINIHUB_ZB_CAP_TEMP | MINIHUB_ZB_CAP_HUMIDITY;
        job->interview_ok = true;
        ESP_LOGI(TAG, "MatchDesc hit 0x%04x ep=%u → treat as temp/humidity sensor", job->short_addr, ep);
        /* Still try SimpleDesc to refine clusters; fall through with known EP. */
        job->eps[0] = ep;
        job->ep_count = 1;
        job->ep_index = 0;
        disc_defer_step(job, DISC_DEFER_SIMPLE_DESC);
        return;
    }

    ESP_LOGW(TAG, "MatchDesc miss for 0x%04x err=%d — try EP1 SimpleDesc", job->short_addr,
             result ? (int)result->error : -1);
    job->eps[0] = 1;
    job->ep_count = 1;
    job->first_ep = 1;
    job->ep_index = 0;
    disc_defer_step(job, DISC_DEFER_SIMPLE_DESC);
}

static void simple_desc_cb(const ezb_zdo_simple_desc_req_result_t *result, void *user_ctx)
{
    discover_job_t *job = user_ctx;
    if (!job || !job->in_use || job != s_disc_active || job->phase != DISC_PHASE_SIMPLE_DESC) {
        ESP_LOGD(TAG, "simple_desc_cb ignored (stale/mismatch)");
        return;
    }

    uint8_t ep = job->eps[job->ep_index];
    if (result && result->error == EZB_ERR_NONE && result->rsp &&
        result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        const ezb_af_simple_desc_t *desc = &result->rsp->desc;
        uint8_t caps = caps_from_simple_desc(desc);
        uint8_t endpoint = desc->ep_id ? desc->ep_id : ep;
        log_simple_desc_clusters(job->short_addr, endpoint, desc);
        ESP_LOGI(TAG, "simple desc 0x%04x ep=%u caps=0x%02x device_id=0x%04x", job->short_addr, endpoint,
                 caps, desc->app_device_id);
        job->interview_ok = true;
        if (caps_better(caps, job->best_caps)) {
            job->best_caps = caps;
            job->best_ep = endpoint;
            job->best_device_id = desc->app_device_id;
        }
        if (!job->first_ep) {
            job->first_ep = endpoint;
        }
    } else {
        ESP_LOGW(TAG, "simple desc failed 0x%04x ep=%u err=%d zdp=0x%02x", job->short_addr, ep,
                 result ? (int)result->error : -1,
                 (result && result->rsp) ? (unsigned)result->rsp->status : 0xffu);
    }

    job->ep_index++;
    if (job->ep_index >= job->ep_count) {
        disc_defer_step(job, DISC_DEFER_FINALIZE);
    } else {
        disc_defer_step(job, DISC_DEFER_SIMPLE_DESC);
    }
}

static discover_job_t *disc_alloc_job(void)
{
    for (int i = 0; i < DISC_QUEUE_DEPTH; i++) {
        if (!s_disc_jobs[i].in_use) {
            memset(&s_disc_jobs[i], 0, sizeof(s_disc_jobs[i]));
            s_disc_jobs[i].in_use = true;
            return &s_disc_jobs[i];
        }
    }
    return NULL;
}

static void disc_kick_next(void)
{
    if (s_disc_active) {
        return;
    }
    for (int i = 0; i < DISC_QUEUE_DEPTH; i++) {
        if (s_disc_jobs[i].in_use && s_disc_jobs[i].phase == DISC_PHASE_IDLE &&
            s_disc_delayed_job != &s_disc_jobs[i]) {
            s_disc_active = &s_disc_jobs[i];
            disc_request_active_ep(s_disc_active);
            return;
        }
    }
}

static void disc_defer_timer_cb(void *arg)
{
    (void)arg;
    discover_job_t *job = s_disc_defer_job;
    disc_defer_action_t action = s_disc_defer_action;
    s_disc_defer_job = NULL;
    s_disc_defer_action = DISC_DEFER_NONE;
    if (action == DISC_DEFER_NONE) {
        return;
    }

    esp_zigbee_lock_acquire(portMAX_DELAY);
    switch (action) {
    case DISC_DEFER_KICK_NEXT:
        disc_kick_next();
        break;
    case DISC_DEFER_MATCH_SENSOR:
        if (job && job->in_use) {
            disc_request_match_sensor(job);
        }
        break;
    case DISC_DEFER_SIMPLE_DESC:
        if (job && job->in_use) {
            disc_request_simple_desc(job);
        }
        break;
    case DISC_DEFER_FINALIZE:
        if (job && job->in_use) {
            disc_finalize(job);
        }
        break;
    default:
        break;
    }
    esp_zigbee_lock_release();
}

static void disc_defer_step(discover_job_t *job, disc_defer_action_t action)
{
    if (!s_disc_defer_timer || action == DISC_DEFER_NONE) {
        return;
    }
    s_disc_defer_job = job;
    s_disc_defer_action = action;
    esp_timer_stop(s_disc_defer_timer);
    esp_timer_start_once(s_disc_defer_timer, 1);
}

static void disc_delay_timer_cb(void *arg)
{
    (void)arg;
    discover_job_t *job = s_disc_delayed_job;
    s_disc_delayed_job = NULL;
    if (!job || !job->in_use) {
        return;
    }
    ESP_LOGI(TAG, "discover delay elapsed → start 0x%04x", job->short_addr);
    job->phase = DISC_PHASE_IDLE;
    disc_defer_step(NULL, DISC_DEFER_KICK_NEXT);
}

static void schedule_disc_delay(discover_job_t *job, int delay_ms)
{
    if (!job || !s_disc_delay_timer) {
        return;
    }
    if (s_disc_delayed_job && s_disc_delayed_job != job) {
        /* One delayed slot: kick previous immediately so it is not lost. */
        discover_job_t *prev = s_disc_delayed_job;
        s_disc_delayed_job = NULL;
        esp_timer_stop(s_disc_delay_timer);
        prev->phase = DISC_PHASE_IDLE;
    }
    s_disc_delayed_job = job;
    job->phase = DISC_PHASE_IDLE;
    esp_timer_stop(s_disc_delay_timer);
    esp_timer_start_once(s_disc_delay_timer, (uint64_t)delay_ms * 1000ULL);
}

static void log_neighbor_for_ieee(const ezb_extaddr_t *ieee, uint16_t short_addr)
{
    ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t nbr;
    while (ezb_nwk_get_next_neighbor(&it, &nbr) == EZB_ERR_NONE) {
        if (nbr.short_addr == short_addr || (ieee && nbr.ieee_addr.u64 == ieee->u64)) {
            ESP_LOGI(TAG,
                     "neighbor short=0x%04x ieee=0x%016llx type=%u rx_idle=%u rel=%u lqi=%u rssi=%d "
                     "timeout=%lu age=%u",
                     nbr.short_addr, (unsigned long long)nbr.ieee_addr.u64, (unsigned)nbr.device_type,
                     (unsigned)nbr.rx_on_when_idle, (unsigned)nbr.relationship, (unsigned)nbr.lqi,
                     (int)nbr.rssi, (unsigned long)nbr.device_timeout, (unsigned)nbr.age);
            return;
        }
    }
    ESP_LOGW(TAG, "neighbor not found for short=0x%04x ieee=0x%016llx", short_addr,
             ieee ? (unsigned long long)ieee->u64 : 0ULL);
}

#define ZB_IC_SLOT_MAX 8
static uint64_t s_ic_ieee[ZB_IC_SLOT_MAX];
static size_t s_ic_count;

static bool joiner_has_install_code(uint64_t ieee)
{
    for (size_t i = 0; i < s_ic_count; i++) {
        if (s_ic_ieee[i] == ieee) {
            return true;
        }
    }
    return false;
}

static void remember_install_code_ieee(uint64_t ieee)
{
    if (joiner_has_install_code(ieee)) {
        return;
    }
    if (s_ic_count >= ZB_IC_SLOT_MAX) {
        /* Drop oldest; rare for hub debug use. */
        memmove(&s_ic_ieee[0], &s_ic_ieee[1], (ZB_IC_SLOT_MAX - 1) * sizeof(s_ic_ieee[0]));
        s_ic_count = ZB_IC_SLOT_MAX - 1;
    }
    s_ic_ieee[s_ic_count++] = ieee;
}

static void ensure_joiner_global_tclk(const ezb_extaddr_t *ieee)
{
    if (!ieee) {
        return;
    }
    if (joiner_has_install_code(ieee->u64)) {
        ESP_LOGI(TAG, "skip global TCLK — install code registered for ieee=0x%016llx",
                 (unsigned long long)ieee->u64);
        return;
    }
    void *kp = aps_secur_key_pair_find_or_create(ieee);
    if (!kp) {
        ESP_LOGW(TAG, "key_pair_find_or_create failed for ieee=0x%016llx",
                 (unsigned long long)ieee->u64);
        return;
    }
    int setup = aps_secur_joiner_setup_key_pair(kp);
    void *got = aps_secur_get_key_pair_by_addr(ieee);
    ESP_LOGI(TAG, "joiner global TCLK setup ieee=0x%016llx setup=%d kp=%p got=%p",
             (unsigned long long)ieee->u64, setup, kp, got);
}

/* Full NWK key for Wireshark (join debug only — local network). */
static void log_network_key_for_sniffer(void)
{
    uint8_t nwk_key[EZB_CCM_KEY_SIZE] = {0};
    ezb_err_t key_err = ezb_secur_get_network_key(nwk_key);
    if (key_err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "get_network_key failed err=%d", (int)key_err);
        return;
    }
    ESP_LOGI(TAG,
             "NWK key (Wireshark): %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:"
             "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             nwk_key[0], nwk_key[1], nwk_key[2], nwk_key[3], nwk_key[4], nwk_key[5], nwk_key[6],
             nwk_key[7], nwk_key[8], nwk_key[9], nwk_key[10], nwk_key[11], nwk_key[12], nwk_key[13],
             nwk_key[14], nwk_key[15]);
}

static void on_unsecure_join(const ezb_extaddr_t *ieee, uint16_t short_addr)
{
    if (!ieee || short_addr == 0 || short_addr == 0xfffe) {
        return;
    }
    ESP_LOGI(TAG, "unsecure_join 0x%04x — TCLK only; stack owns APS Transport Key", short_addr);
    log_neighbor_for_ieee(ieee, short_addr);
    ensure_joiner_global_tclk(ieee);
    log_network_key_for_sniffer();
}

static void start_discover(uint16_t short_addr, uint64_t ieee)
{
    /* Refresh existing in-progress job for same short addr or ieee */
    for (int i = 0; i < DISC_QUEUE_DEPTH; i++) {
        if (s_disc_jobs[i].in_use &&
            (s_disc_jobs[i].short_addr == short_addr || s_disc_jobs[i].ieee == ieee)) {
            /* Update short addr if device re-associated with a new NWK address. */
            if (s_disc_jobs[i].short_addr != short_addr) {
                ESP_LOGI(TAG, "discover update ieee=0x%016llx short 0x%04x → 0x%04x",
                         (unsigned long long)ieee, s_disc_jobs[i].short_addr, short_addr);
                s_disc_jobs[i].short_addr = short_addr;
            }
            ESP_LOGW(TAG, "discover already queued for 0x%04x (phase=%d attempt=%u)", short_addr,
                     (int)s_disc_jobs[i].phase, s_disc_jobs[i].attempt);
            return;
        }
    }

    discover_job_t *job = disc_alloc_job();
    if (!job) {
        ESP_LOGW(TAG, "discover queue full — drop 0x%04x", short_addr);
        return;
    }
    job->short_addr = short_addr;
    job->ieee = ieee;
    job->phase = DISC_PHASE_IDLE;
    job->attempt = 0;
    job->interview_ok = false;
    ESP_LOGI(TAG, "discover queued short=0x%04x ieee=0x%016llx (delay %d ms)", short_addr,
             (unsigned long long)ieee, DISC_START_DELAY_MS);
    schedule_disc_delay(job, DISC_START_DELAY_MS);
}

void zigbee_stack_rediscover(uint16_t short_addr, uint64_t ieee)
{
    start_discover(short_addr, ieee);
}

static void fallback_timer_cb(void *arg)
{
    (void)arg;
    if (!s_disc_active || !s_disc_active->in_use) {
        return;
    }
    ESP_LOGW(TAG, "discover timeout for 0x%04x phase=%d", s_disc_active->short_addr,
             (int)s_disc_active->phase);
    if (s_disc_active->phase == DISC_PHASE_ACTIVE_EP) {
        disc_defer_step(s_disc_active, DISC_DEFER_MATCH_SENSOR);
        return;
    }
    if (s_disc_active->phase == DISC_PHASE_MATCH_SENSOR) {
        s_disc_active->eps[0] = 1;
        s_disc_active->ep_count = 1;
        s_disc_active->first_ep = 1;
        s_disc_active->ep_index = 0;
        disc_defer_step(s_disc_active, DISC_DEFER_SIMPLE_DESC);
        return;
    }
    disc_defer_step(s_disc_active, DISC_DEFER_FINALIZE);
}

static void bdb_retry_timer_cb(void *arg)
{
    (void)arg;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(s_bdb_retry_mode);
    esp_zigbee_lock_release();
}

static void schedule_bdb_retry(uint8_t mode)
{
    ESP_LOGW(TAG, "schedule BDB retry mode=0x%02x in %d ms", mode, BDB_RETRY_MS);
    s_bdb_retry_mode = mode;
    if (s_bdb_retry_timer) {
        esp_timer_stop(s_bdb_retry_timer);
        esp_timer_start_once(s_bdb_retry_timer, BDB_RETRY_MS * 1000ULL);
    }
}

static void fill_zcl_cmd_ctrl(ezb_zcl_cmd_ctrl_t *ctrl, uint16_t short_addr, uint8_t dst_ep,
                              uint16_t cluster_id)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ezb_address_set_short(&ctrl->dst_addr, short_addr);
    ctrl->dst_ep = dst_ep;
    ctrl->src_ep = ZB_COORD_ENDPOINT;
    ctrl->cluster_id = cluster_id;
    ctrl->manuf_code = EZB_ZCL_STD_MANUF_CODE;
    ctrl->fc.direction = 0; /* to server */
}

static void fill_cluster_cmd_ctrl(ezb_zcl_cluster_cmd_ctrl_t *ctrl, uint16_t short_addr, uint8_t dst_ep)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ezb_address_set_short(&ctrl->dst_addr, short_addr);
    ctrl->dst_ep = dst_ep;
    ctrl->src_ep = ZB_COORD_ENDPOINT;
}

static void bind_cb(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    uint16_t cluster = (uint16_t)(uintptr_t)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp ||
        result->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "bind cluster 0x%04x failed err=%d zdp=0x%02x", cluster,
                 result ? (int)result->error : -1,
                 (result && result->rsp) ? (unsigned)result->rsp->status : 0xffu);
        return;
    }
    ESP_LOGI(TAG, "bound cluster 0x%04x", cluster);
}

static esp_err_t bind_cluster(uint16_t short_addr, uint8_t endpoint, uint64_t ieee, uint16_t cluster_id)
{
    ezb_extaddr_t local = {0};
    ezb_nwk_get_extended_address(&local);

    ESP_LOGI(TAG, "bind 0x%04x ep=%u cluster=0x%04x → coord", short_addr, endpoint, cluster_id);
    ezb_zdo_bind_req_t req = {
        .dst_nwk_addr = short_addr,
        .field =
            {
                .src_addr = {.u64 = ieee},
                .src_ep = endpoint,
                .cluster_id = cluster_id,
                .dst_addr_mode = EZB_ADDR_MODE_EXT,
                .dst_addr = {.extended_addr = local},
                .dst_ep = ZB_COORD_ENDPOINT,
            },
        .cb = bind_cb,
        .user_ctx = (void *)(uintptr_t)cluster_id,
    };
    return esp_zigbee_err_to_esp(ezb_zdo_bind_req(&req));
}

static esp_err_t config_report_attr(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id,
                                    uint16_t attr_id, uint8_t attr_type, uint16_t change)
{
    ESP_LOGI(TAG, "config_report 0x%04x ep=%u cluster=0x%04x attr=0x%04x change=%u", short_addr, endpoint,
             cluster_id, attr_id, change);
    ezb_zcl_config_report_record_t rec = {
        .direction = EZB_ZCL_REPORTING_SEND,
        .attr_id = attr_id,
        .client =
            {
                .attr_type = attr_type,
                .min_interval = REPORT_MIN_INTERVAL_S,
                .max_interval = (cluster_id == EZB_ZCL_CLUSTER_ID_METERING) ? ENERGY_REPORT_MAX_INTERVAL_S
                                                                             : REPORT_MAX_INTERVAL_S,
            },
    };
    if (attr_type == EZB_ZCL_ATTR_TYPE_INT16) {
        rec.client.reportable_change.s16 = (int16_t)change;
    } else if (attr_type == EZB_ZCL_ATTR_TYPE_UINT48) {
        rec.client.reportable_change.u48 = change;
    } else {
        rec.client.reportable_change.u16 = change;
    }
    ezb_zcl_config_report_cmd_t cmd = {0};
    fill_zcl_cmd_ctrl(&cmd.cmd_ctrl, short_addr, endpoint, cluster_id);
    cmd.payload.record_number = 1;
    cmd.payload.record_field = &rec;
    return esp_zigbee_err_to_esp(ezb_zcl_config_report_cmd_req(&cmd));
}

static meter_scale_t *meter_scale_get(uint16_t short_addr)
{
    for (size_t i = 0; i < s_meter_scale_count; i++) {
        if (s_meter_scale[i].short_addr == short_addr) {
            return &s_meter_scale[i];
        }
    }
    if (s_meter_scale_count >= MINIHUB_ZB_MAX_DEVICES) {
        return &s_meter_scale[0];
    }
    meter_scale_t *s = &s_meter_scale[s_meter_scale_count++];
    memset(s, 0, sizeof(*s));
    s->short_addr = short_addr;
    s->ac_v_m = s->ac_v_d = 1;
    s->ac_i_m = 1;
    /* Tuya-style plugs report RMSCurrent in mA; ZCL divisor is often missing/1. */
    s->ac_i_d = 1000;
    s->ac_p_m = s->ac_p_d = 1;
    s->energy_m = s->energy_d = 1;
    return s;
}

static float scale_u16(uint16_t raw, uint16_t mult, uint16_t div)
{
    if (div == 0) {
        div = 1;
    }
    if (mult == 0) {
        mult = 1;
    }
    return ((float)raw * (float)mult) / (float)div;
}

static float scale_s16(int16_t raw, uint16_t mult, uint16_t div)
{
    if (div == 0) {
        div = 1;
    }
    if (mult == 0) {
        mult = 1;
    }
    return ((float)raw * (float)mult) / (float)div;
}

static uint32_t read_u24_le(const void *value)
{
    const uint8_t *p = (const uint8_t *)value;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint64_t read_u48_le(const void *value)
{
    const uint8_t *p = (const uint8_t *)value;
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40);
}

static esp_err_t subscribe_note(esp_err_t *first_err, esp_err_t err)
{
    if (err != ESP_OK && *first_err == ESP_OK) {
        *first_err = err;
    }
    return err;
}

static esp_err_t read_attr(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id)
{
    ESP_LOGD(TAG, "read_attr 0x%04x ep=%u cluster=0x%04x attr=0x%04x", short_addr, endpoint, cluster_id,
             attr_id);
    uint16_t attrs[1] = {attr_id};
    ezb_zcl_read_attr_cmd_t cmd = {0};
    fill_zcl_cmd_ctrl(&cmd.cmd_ctrl, short_addr, endpoint, cluster_id);
    cmd.payload.attr_number = 1;
    cmd.payload.attr_field = attrs;
    return esp_zigbee_err_to_esp(ezb_zcl_read_attr_cmd_req(&cmd));
}

static esp_err_t read_attrs(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id,
                            uint16_t *attrs, uint16_t attr_count)
{
    if (!attrs || attr_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGD(TAG, "read_attrs 0x%04x ep=%u cluster=0x%04x n=%u", short_addr, endpoint, cluster_id,
             (unsigned)attr_count);
    ezb_zcl_read_attr_cmd_t cmd = {0};
    fill_zcl_cmd_ctrl(&cmd.cmd_ctrl, short_addr, endpoint, cluster_id);
    cmd.payload.attr_number = attr_count;
    cmd.payload.attr_field = attrs;
    return esp_zigbee_err_to_esp(ezb_zcl_read_attr_cmd_req(&cmd));
}

static void meter_sub_add(uint16_t short_addr, uint8_t endpoint, uint8_t caps)
{
    if (!(caps & MINIHUB_ZB_CAP_METERING_MASK)) {
        return;
    }
    for (size_t i = 0; i < MINIHUB_ZB_MAX_DEVICES; i++) {
        if (s_meter_subs[i].in_use && s_meter_subs[i].short_addr == short_addr &&
            s_meter_subs[i].endpoint == endpoint) {
            s_meter_subs[i].caps = caps;
            return;
        }
    }
    for (size_t i = 0; i < MINIHUB_ZB_MAX_DEVICES; i++) {
        if (!s_meter_subs[i].in_use) {
            s_meter_subs[i].in_use = true;
            s_meter_subs[i].short_addr = short_addr;
            s_meter_subs[i].endpoint = endpoint;
            s_meter_subs[i].caps = caps;
            ESP_LOGI(TAG, "meter poll track 0x%04x ep=%u caps=0x%02x", short_addr, endpoint, caps);
            return;
        }
    }
}

static void meter_poll_timer_cb(void *arg)
{
    (void)arg;
    if (!s_stack_ready) {
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    for (size_t i = 0; i < MINIHUB_ZB_MAX_DEVICES; i++) {
        if (!s_meter_subs[i].in_use) {
            continue;
        }
        uint16_t short_addr = s_meter_subs[i].short_addr;
        uint8_t endpoint = s_meter_subs[i].endpoint;
        uint8_t caps = s_meter_subs[i].caps;
        if (caps & MINIHUB_ZB_CAP_ELECTRICAL) {
            static uint16_t em_attrs[] = {
                EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID,
                EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_ID,
                EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_ID,
            };
            (void)read_attrs(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT, em_attrs,
                             sizeof(em_attrs) / sizeof(em_attrs[0]));
        }
        if (caps & MINIHUB_ZB_CAP_ENERGY) {
            (void)read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_METERING,
                            EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID);
        }
    }
    esp_zigbee_lock_release();
}

static void meter_poll_ensure_started(void)
{
    if (!s_meter_poll_timer) {
        return;
    }
    esp_timer_stop(s_meter_poll_timer);
    esp_err_t err = esp_timer_start_periodic(s_meter_poll_timer, METER_POLL_INTERVAL_S * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "meter poll start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "meter poll every %ds", METER_POLL_INTERVAL_S);
    }
}

static esp_err_t write_cie_address(uint16_t short_addr, uint8_t endpoint)
{
    ezb_extaddr_t local = {0};
    ezb_nwk_get_extended_address(&local);
    /* Keep stable storage: write_attr may send asynchronously. */
    static uint64_t s_cie_ieee;
    s_cie_ieee = local.u64;

    ESP_LOGI(TAG, "IAS write CIE 0x%04x ep=%u cie=0x%016llx", short_addr, endpoint,
             (unsigned long long)s_cie_ieee);
    ezb_zcl_attribute_t attr = {
        .id = EZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID,
        .data =
            {
                .type = EZB_ZCL_ATTR_TYPE_EUI64,
                .size = sizeof(s_cie_ieee),
                .value = &s_cie_ieee,
            },
    };
    ezb_zcl_write_attr_cmd_t cmd = {0};
    fill_zcl_cmd_ctrl(&cmd.cmd_ctrl, short_addr, endpoint, EZB_ZCL_CLUSTER_ID_IAS_ZONE);
    cmd.payload.attr_number = 1;
    cmd.payload.attr_field = &attr;
    return esp_zigbee_err_to_esp(ezb_zcl_write_attr_cmd_req(&cmd));
}

esp_err_t zigbee_stack_subscribe(uint16_t short_addr, uint8_t endpoint, uint64_t ieee, uint8_t caps)
{
    if (!s_stack_ready) {
        ESP_LOGW(TAG, "subscribe rejected: stack not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (!(caps & (MINIHUB_ZB_CAP_SENSOR_MASK | MINIHUB_ZB_CAP_METERING_MASK))) {
        ESP_LOGI(TAG, "subscribe skip 0x%04x (no sensor/metering caps 0x%02x)", short_addr, caps);
        return ESP_OK;
    }
    if (endpoint == 0) {
        endpoint = 1;
    }

    ESP_LOGI(TAG, "subscribe begin 0x%04x ep=%u ieee=0x%016llx caps=0x%02x", short_addr, endpoint,
             (unsigned long long)ieee, caps);
    esp_err_t first_err = ESP_OK;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (caps & MINIHUB_ZB_CAP_TEMP) {
        subscribe_note(&first_err,
                       bind_cluster(short_addr, endpoint, ieee, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT));
        subscribe_note(&first_err,
                       config_report_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
                                          EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID,
                                          EZB_ZCL_ATTR_TYPE_INT16, TEMP_CHANGE_CENTI));
        subscribe_note(&first_err, read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
                                             EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID));
    }
    if (caps & MINIHUB_ZB_CAP_HUMIDITY) {
        subscribe_note(&first_err,
                       bind_cluster(short_addr, endpoint, ieee, EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT));
        subscribe_note(&first_err,
                       config_report_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                                          EZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_ID,
                                          EZB_ZCL_ATTR_TYPE_UINT16, HUM_CHANGE_CENTI));
        subscribe_note(&first_err, read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                                             EZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_ID));
    }
    if (caps & MINIHUB_ZB_CAP_IAS_ZONE) {
        subscribe_note(&first_err, bind_cluster(short_addr, endpoint, ieee, EZB_ZCL_CLUSTER_ID_IAS_ZONE));
        subscribe_note(&first_err, write_cie_address(short_addr, endpoint));
        subscribe_note(&first_err, read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_IAS_ZONE,
                                             EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATUS_ID));
    }
    if (caps & MINIHUB_ZB_CAP_ELECTRICAL) {
        (void)meter_scale_get(short_addr);
        subscribe_note(&first_err,
                       bind_cluster(short_addr, endpoint, ieee, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT));
        subscribe_note(&first_err,
                       config_report_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                          EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID,
                                          EZB_ZCL_ATTR_TYPE_INT16, POWER_CHANGE_RAW));
        subscribe_note(&first_err,
                       config_report_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                          EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_ID,
                                          EZB_ZCL_ATTR_TYPE_UINT16, VOLTAGE_CHANGE_RAW));
        subscribe_note(&first_err,
                       config_report_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                          EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_ID,
                                          EZB_ZCL_ATTR_TYPE_UINT16, CURRENT_CHANGE_RAW));
        /* Scaling factors first, then live values. */
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_MULTIPLIER_ID));
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_DIVISOR_ID));
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_MULTIPLIER_ID));
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_DIVISOR_ID));
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_POWER_MULTIPLIER_ID));
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_POWER_DIVISOR_ID));
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID));
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_ID));
        subscribe_note(&first_err,
                       read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                                 EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_ID));
    }
    if (caps & MINIHUB_ZB_CAP_ENERGY) {
        (void)meter_scale_get(short_addr);
        subscribe_note(&first_err, bind_cluster(short_addr, endpoint, ieee, EZB_ZCL_CLUSTER_ID_METERING));
        subscribe_note(&first_err,
                       config_report_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_METERING,
                                          EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                                          EZB_ZCL_ATTR_TYPE_UINT48, ENERGY_CHANGE_RAW));
        subscribe_note(&first_err, read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_METERING,
                                             EZB_ZCL_ATTR_METERING_MULTIPLIER_ID));
        subscribe_note(&first_err, read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_METERING,
                                             EZB_ZCL_ATTR_METERING_DIVISOR_ID));
        subscribe_note(&first_err, read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_METERING,
                                             EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID));
    }
    meter_sub_add(short_addr, endpoint, caps);
    meter_poll_ensure_started();
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "subscribe 0x%04x ep=%u caps=0x%02x → %s", short_addr, endpoint, caps,
             esp_err_to_name(first_err));
    return first_err;
}

void zigbee_stack_subscribe_product_params(uint16_t short_addr, uint8_t endpoint, uint64_t ieee)
{
    if (!s_stack_ready) {
        return;
    }
    if (endpoint == 0) {
        endpoint = 1;
    }
    minihub_pp_zb_param_t params[MINIHUB_PP_MAX_PARAMS];
    size_t n = minihub_pp_zigbee_list_params(ieee, NULL, NULL, params, MINIHUB_PP_MAX_PARAMS);
    if (n == 0) {
        return;
    }
    ESP_LOGI(TAG, "product_params subscribe 0x%04x ep=%u n=%u", short_addr, endpoint, (unsigned)n);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    /* Static/info attrs (e.g. Basic): read only — many devices reject ZDO bind on 0x0000. */
    for (size_t i = 0; i < n; i++) {
        (void)read_attr(short_addr, endpoint, params[i].cluster, params[i].attr_id);
    }
    esp_zigbee_lock_release();
}

esp_err_t zigbee_stack_write_attr(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id,
                                  uint16_t attr_id, uint8_t attr_type, void *value, uint16_t value_size)
{
    if (!s_stack_ready || !value || value_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (endpoint == 0) {
        endpoint = 1;
    }
    ezb_zcl_attribute_t attr = {
        .id = attr_id,
        .data =
            {
                .type = attr_type,
                .size = value_size,
                .value = value,
            },
    };
    ezb_zcl_write_attr_cmd_t cmd = {0};
    fill_zcl_cmd_ctrl(&cmd.cmd_ctrl, short_addr, endpoint, cluster_id);
    cmd.payload.attr_number = 1;
    cmd.payload.attr_field = &attr;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zigbee_err_to_esp(ezb_zcl_write_attr_cmd_req(&cmd));
    esp_zigbee_lock_release();
    return err;
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);
    ESP_LOGI(TAG, "signal %s (0x%04x)", signal_name(signal_type), (unsigned)signal_type);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        /* Register 802.15.4 with coex only when 2.4 GHz Wi-Fi is in use (minihub_net
         * STA or external stack via MINIHUB_NET_WIFI_SOFTAP, e.g. Claw wifi_manager).
         * BLE Mesh may be compiled in but disabled at runtime — do not force
         * coex schemes that steal RX from sleepy end devices. */
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED && \
    (CONFIG_MINIHUB_NET_WIFI_STA || CONFIG_MINIHUB_NET_WIFI_SOFTAP)
        {
            esp_err_t coex_err = esp_coex_wifi_i154_enable();
            if (coex_err != ESP_OK) {
                ESP_LOGW(TAG, "802.15.4 coexistence enable failed: %s", esp_err_to_name(coex_err));
            } else {
                ESP_LOGI(TAG, "802.15.4 coexistence enabled (wifi share RF)");
            }
        }
#endif
        ESP_LOGI(TAG, "Zigbee stack init → BDB INITIALIZATION");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        ESP_LOGI(TAG, "BDB start status=0x%02x factory_new=%d", (unsigned)status,
                 (int)ezb_bdb_is_factory_new());
        if (status == EZB_BDB_STATUS_SUCCESS) {
            if (ezb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "factory-new — form network (primary_mask=0x%08lx secondary=0x%08lx)",
                         (unsigned long)ZB_PRIMARY_CHANNEL_MASK, (unsigned long)ZB_SECONDARY_CHANNEL_MASK);
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ezb_nwk_set_min_join_lqi(0); /* battery EDs often join with weak LQI */
                ESP_LOGI(TAG, "network restored from NVRAM (PAN 0x%04x ch=%d min_join_lqi=%u)",
                         ezb_nwk_get_panid(), ezb_nwk_get_current_channel(),
                         (unsigned)ezb_nwk_get_min_join_lqi());
                mark_stack_ready();
            }
        } else {
            ESP_LOGW(TAG, "BDB start status=0x%02x — retry", (unsigned)status);
            schedule_bdb_retry(EZB_BDB_MODE_INITIALIZATION);
        }
        break;
    }
    case EZB_BDB_SIGNAL_FORMATION: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_nwk_set_min_join_lqi(0);
            ESP_LOGI(TAG, "formed PAN 0x%04x ch=%d min_join_lqi=%u → start STEERING", ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(), (unsigned)ezb_nwk_get_min_join_lqi());
            mark_stack_ready();
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "form failed status=0x%02x — retry", (unsigned)status);
            schedule_bdb_retry(EZB_BDB_MODE_NETWORK_FORMATION);
        }
        break;
    }
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "network steering completed (permit may be open via BDB)");
        } else {
            ESP_LOGW(TAG, "steering status=0x%02x", (unsigned)status);
        }
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *params =
            (const ezb_zdo_signal_device_annce_params_t *)ezb_app_signal_get_params(app_signal);
        if (params) {
            ESP_LOGI(TAG, "device annce short=0x%04x ieee=0x%016llx mac_cap=0x%02x", params->short_addr,
                     (unsigned long long)params->device_addr.u64, params->capability);
            start_discover(params->short_addr, params->device_addr.u64);
        } else {
            ESP_LOGW(TAG, "device annce with null params");
        }
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const ezb_zdo_signal_device_update_params_t *p =
            (const ezb_zdo_signal_device_update_params_t *)ezb_app_signal_get_params(app_signal);
        if (p) {
            ESP_LOGI(TAG,
                     "device update short=0x%04x ieee=0x%016llx status=%u(%s) tc_action=%u parent=0x%04x",
                     p->short_addr, (unsigned long long)p->device_addr.u64, (unsigned)p->status,
                     upddev_status_str(p->status), (unsigned)p->tc_action, p->parent_short);
            if (p->status == EZB_ZDO_UPDDEV_UNSECURE_JOIN && p->tc_action == EZB_ZDO_TC_ACTION_ACCEPT &&
                p->short_addr != 0 && p->short_addr != 0xfffe) {
                on_unsecure_join(&p->device_addr, p->short_addr);
            }
        }
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *p =
            (const ezb_zdo_signal_device_authorized_params_t *)ezb_app_signal_get_params(app_signal);
        if (p) {
            ESP_LOGI(TAG, "device authorized short=0x%04x ieee=0x%016llx type=%u status=%u(%s)",
                     p->short_addr, (unsigned long long)p->device_addr.u64, (unsigned)p->type,
                     (unsigned)p->status, auth_status_str(p->status));
            if (p->status == EZB_ZDO_AUTH_STATUS_SUCCESS && p->short_addr != 0 &&
                p->short_addr != 0xfffe) {
                start_discover(p->short_addr, p->device_addr.u64);
            }
        }
        break;
    }
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *p =
            (const ezb_zdo_signal_leave_indication_params_t *)ezb_app_signal_get_params(app_signal);
        if (p) {
            ESP_LOGI(TAG, "leave indication short=0x%04x ieee=0x%016llx type=%u", p->short_addr,
                     (unsigned long long)p->device_addr.u64, (unsigned)p->leave_type);
        }
        break;
    }
    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *p =
            (const ezb_zdo_signal_leave_params_t *)ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "self leave type=%u", p ? (unsigned)p->leave_type : 0xffu);
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_UNAVAILABLE: {
        const ezb_zdo_signal_device_unavailable_params_t *p =
            (const ezb_zdo_signal_device_unavailable_params_t *)ezb_app_signal_get_params(app_signal);
        if (p) {
            ESP_LOGW(TAG, "device unavailable short=0x%04x ieee=0x%016llx", p->short_addr,
                     (unsigned long long)p->device_addr.u64);
        }
        break;
    }
    case EZB_NWK_SIGNAL_DEVICE_ASSOCIATED: {
        ESP_LOGI(TAG, "nwk device associated (legacy signal)");
        break;
    }
    case EZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        ESP_LOGW(TAG, "nwk: no active links left");
        break;
    case EZB_NWK_SIGNAL_PANID_CONFLICT_DETECTED:
        ESP_LOGW(TAG, "nwk: PAN ID conflict detected");
        break;
    case EZB_NWK_SIGNAL_NETWORK_STATUS: {
        const ezb_nwk_signal_network_status_params_t *p =
            (const ezb_nwk_signal_network_status_params_t *)ezb_app_signal_get_params(app_signal);
        if (p) {
            ESP_LOGW(TAG, "nwk status=0x%02x addr=0x%04x unk_cmd=0x%02x", (unsigned)p->status,
                     p->network_addr, (unsigned)p->unknown_command_id);
        }
        break;
    }
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t sec = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "permit join window → %u sec (PAN 0x%04x ch=%d)", sec, ezb_nwk_get_panid(),
                 ezb_nwk_get_current_channel());
        if (sec == 0) {
            s_permit_until_ms = 0;
        }
        break;
    }
    default:
        ESP_LOGI(TAG, "unhandled app signal 0x%04x", (unsigned)signal_type);
        break;
    }
    return true;
}

static uint16_t short_from_header(const ezb_zcl_cmd_hdr_t *hdr)
{
    if (!hdr) {
        return 0;
    }
    if (hdr->src_addr.addr_mode == EZB_ADDR_MODE_SHORT) {
        return hdr->src_addr.u.short_addr;
    }
    return 0;
}

static void try_product_params_attr(uint16_t short_addr, uint8_t endpoint, uint16_t cluster,
                                    uint16_t attr_id, uint8_t attr_type, const void *value)
{
    if (!value) {
        return;
    }
    const minihub_zigbee_device_t *dev = minihub_zigbee_find_by_addr(short_addr, endpoint);
    if (!dev && short_addr) {
        /* Endpoint mismatch — try ep from table via short only. */
        for (uint8_t ep = 1; ep < 8; ep++) {
            dev = minihub_zigbee_find_by_addr(short_addr, ep);
            if (dev) {
                break;
            }
        }
    }
    if (!dev || !dev->point_id[0]) {
        return;
    }
    size_t len = 1;
    if (attr_type == EZB_ZCL_ATTR_TYPE_UINT16 || attr_type == EZB_ZCL_ATTR_TYPE_INT16) {
        len = 2;
    } else if (attr_type == EZB_ZCL_ATTR_TYPE_UINT32 || attr_type == EZB_ZCL_ATTR_TYPE_INT32) {
        len = 4;
    }
    (void)minihub_pp_zigbee_apply(dev->point_id, dev->ieee, NULL, NULL, cluster, attr_id, attr_type, value,
                                  len);
}

static void emit_attr_from_value(uint16_t short_addr, uint8_t endpoint, uint16_t cluster,
                                 uint16_t attr_id, uint8_t attr_type, const void *value)
{
    if (!value) {
        return;
    }
    minihub_zigbee_attr_evt_t evt = {
        .short_addr = short_addr,
        .endpoint = endpoint,
    };
    if (cluster == EZB_ZCL_CLUSTER_ID_ON_OFF && attr_id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        evt.kind = MINIHUB_ZB_ATTR_ONOFF;
        evt.u.onoff = *(const uint8_t *)value != 0;
        ESP_LOGD(TAG, "attr onoff 0x%04x ep=%u → %s", short_addr, endpoint, evt.u.onoff ? "on" : "off");
        zigbee_stack_notify_attr(&evt);
    } else if (cluster == EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT &&
               attr_id == EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID) {
        int16_t raw = *(const int16_t *)value;
        evt.kind = MINIHUB_ZB_ATTR_TEMP_C;
        evt.u.temp_c = (float)raw / 100.0f;
        ESP_LOGD(TAG, "attr temp 0x%04x ep=%u → %.2f C (raw=%d)", short_addr, endpoint,
                 (double)evt.u.temp_c, (int)raw);
        zigbee_stack_notify_attr(&evt);
    } else if (cluster == EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT &&
               attr_id == EZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_ID) {
        uint16_t raw = *(const uint16_t *)value;
        evt.kind = MINIHUB_ZB_ATTR_HUMIDITY_PCT;
        evt.u.humidity_pct = (float)raw / 100.0f;
        ESP_LOGD(TAG, "attr humidity 0x%04x ep=%u → %.2f %% (raw=%u)", short_addr, endpoint,
                 (double)evt.u.humidity_pct, (unsigned)raw);
        zigbee_stack_notify_attr(&evt);
    } else if (cluster == EZB_ZCL_CLUSTER_ID_IAS_ZONE &&
               attr_id == EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATUS_ID) {
        uint16_t status = *(const uint16_t *)value;
        evt.kind = MINIHUB_ZB_ATTR_CONTACT;
        evt.u.contact.open = (status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM1) != 0;
        evt.u.contact.tamper = (status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_TAMPER) != 0;
        evt.u.contact.battery_low = (status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_BATTERY) != 0;
        evt.u.contact.zone_status = status;
        ESP_LOGD(TAG, "attr contact 0x%04x ep=%u status=0x%04x open=%d", short_addr, endpoint, status,
                 (int)evt.u.contact.open);
        zigbee_stack_notify_attr(&evt);
    } else if (cluster == EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT) {
        meter_scale_t *sc = meter_scale_get(short_addr);
        if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_MULTIPLIER_ID) {
            sc->ac_v_m = *(const uint16_t *)value;
            ESP_LOGI(TAG, "scale 0x%04x ac_v_m=%u", short_addr, sc->ac_v_m);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_DIVISOR_ID) {
            sc->ac_v_d = *(const uint16_t *)value;
            ESP_LOGI(TAG, "scale 0x%04x ac_v_d=%u", short_addr, sc->ac_v_d);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_MULTIPLIER_ID) {
            sc->ac_i_m = *(const uint16_t *)value;
            ESP_LOGI(TAG, "scale 0x%04x ac_i_m=%u", short_addr, sc->ac_i_m);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_DIVISOR_ID) {
            uint16_t d = *(const uint16_t *)value;
            /* Ignore 0/1 — many plugs advertise 1 but still send milliamps. */
            if (d > 1) {
                sc->ac_i_d = d;
            }
            ESP_LOGI(TAG, "scale 0x%04x ac_i_d=%u (raw_attr=%u)", short_addr, sc->ac_i_d, (unsigned)d);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_POWER_MULTIPLIER_ID) {
            sc->ac_p_m = *(const uint16_t *)value;
            ESP_LOGI(TAG, "scale 0x%04x ac_p_m=%u", short_addr, sc->ac_p_m);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_POWER_DIVISOR_ID) {
            sc->ac_p_d = *(const uint16_t *)value;
            ESP_LOGI(TAG, "scale 0x%04x ac_p_d=%u", short_addr, sc->ac_p_d);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID) {
            int16_t raw = *(const int16_t *)value;
            if (raw == (int16_t)0x8000) {
                return;
            }
            evt.kind = MINIHUB_ZB_ATTR_POWER_W;
            evt.u.power_w = scale_s16(raw, sc->ac_p_m, sc->ac_p_d);
            ESP_LOGD(TAG, "attr power 0x%04x ep=%u → %.1f W (raw=%d)", short_addr, endpoint,
                     (double)evt.u.power_w, (int)raw);
            zigbee_stack_notify_attr(&evt);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_ID) {
            uint16_t raw = *(const uint16_t *)value;
            if (raw == 0xffff) {
                return;
            }
            evt.kind = MINIHUB_ZB_ATTR_VOLTAGE_V;
            evt.u.voltage_v = scale_u16(raw, sc->ac_v_m, sc->ac_v_d);
            ESP_LOGD(TAG, "attr voltage 0x%04x ep=%u → %.1f V (raw=%u)", short_addr, endpoint,
                     (double)evt.u.voltage_v, (unsigned)raw);
            zigbee_stack_notify_attr(&evt);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_ID) {
            uint16_t raw = *(const uint16_t *)value;
            if (raw == 0xffff) {
                return;
            }
            evt.kind = MINIHUB_ZB_ATTR_CURRENT_A;
            evt.u.current_a = scale_u16(raw, sc->ac_i_m, sc->ac_i_d);
            ESP_LOGD(TAG, "attr current 0x%04x ep=%u → %.3f A (raw=%u)", short_addr, endpoint,
                     (double)evt.u.current_a, (unsigned)raw);
            zigbee_stack_notify_attr(&evt);
        } else {
            ESP_LOGD(TAG, "attr 0x%04x ep=%u cluster=0x%04x attr=0x%04x (em unmapped)", short_addr, endpoint,
                     cluster, attr_id);
            try_product_params_attr(short_addr, endpoint, cluster, attr_id, attr_type, value);
        }
    } else if (cluster == EZB_ZCL_CLUSTER_ID_METERING) {
        meter_scale_t *sc = meter_scale_get(short_addr);
        if (attr_id == EZB_ZCL_ATTR_METERING_MULTIPLIER_ID) {
            sc->energy_m = read_u24_le(value);
            if (sc->energy_m == 0) {
                sc->energy_m = 1;
            }
            ESP_LOGI(TAG, "scale 0x%04x energy_m=%lu", short_addr, (unsigned long)sc->energy_m);
        } else if (attr_id == EZB_ZCL_ATTR_METERING_DIVISOR_ID) {
            sc->energy_d = read_u24_le(value);
            if (sc->energy_d == 0) {
                sc->energy_d = 1;
            }
            ESP_LOGI(TAG, "scale 0x%04x energy_d=%lu", short_addr, (unsigned long)sc->energy_d);
        } else if (attr_id == EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID) {
            uint64_t raw = read_u48_le(value);
            evt.kind = MINIHUB_ZB_ATTR_ENERGY_WH;
            evt.u.energy_wh = ((float)raw * (float)sc->energy_m) / (float)sc->energy_d;
            ESP_LOGD(TAG, "attr energy 0x%04x ep=%u → %.1f Wh (raw=%llu)", short_addr, endpoint,
                     (double)evt.u.energy_wh, (unsigned long long)raw);
            zigbee_stack_notify_attr(&evt);
        } else {
            ESP_LOGD(TAG, "attr 0x%04x ep=%u cluster=0x%04x attr=0x%04x (metering unmapped)", short_addr,
                     endpoint, cluster, attr_id);
            try_product_params_attr(short_addr, endpoint, cluster, attr_id, attr_type, value);
        }
    } else {
        ESP_LOGD(TAG, "attr 0x%04x ep=%u cluster=0x%04x attr=0x%04x (unmapped)", short_addr, endpoint,
                 cluster, attr_id);
        try_product_params_attr(short_addr, endpoint, cluster, attr_id, attr_type, value);
    }
}

static void emit_contact_from_zone_status(uint16_t short_addr, uint8_t endpoint, uint16_t zone_status)
{
    minihub_zigbee_attr_evt_t evt = {
        .short_addr = short_addr,
        .endpoint = endpoint,
        .kind = MINIHUB_ZB_ATTR_CONTACT,
    };
    evt.u.contact.open = (zone_status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM1) != 0;
    evt.u.contact.tamper = (zone_status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_TAMPER) != 0;
    evt.u.contact.battery_low = (zone_status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_BATTERY) != 0;
    evt.u.contact.zone_status = zone_status;
    zigbee_stack_notify_attr(&evt);
}

static void handle_ias_enroll(ezb_zcl_ias_zone_enroll_req_message_t *msg)
{
    if (!msg || !msg->in.header) {
        return;
    }
    uint16_t short_addr = short_from_header(msg->in.header);
    uint8_t endpoint = msg->in.header->src_ep;
    static uint8_t s_next_zone_id = 1;
    uint8_t zone_id = s_next_zone_id++;
    if (s_next_zone_id == 0xff) {
        s_next_zone_id = 1;
    }

    ezb_zcl_ias_zone_enroll_rsp_cmd_t rsp = {0};
    fill_cluster_cmd_ctrl(&rsp.cmd_ctrl, short_addr, endpoint);
    rsp.payload.enroll_rsp_code = EZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS;
    rsp.payload.zone_id = zone_id;

    ezb_err_t zerr = ezb_zcl_ias_zone_enroll_cmd_resp(&rsp);
    ESP_LOGI(TAG, "IAS enroll 0x%04x ep=%u zone_type=0x%04x → zone_id=%u (%s)", short_addr, endpoint,
             msg->in.payload.zone_type, zone_id, esp_err_to_name(esp_zigbee_err_to_esp(zerr)));
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;

    /* Re-read status after enroll so UI gets an initial open/closed. */
    (void)read_attr(short_addr, endpoint, EZB_ZCL_CLUSTER_ID_IAS_ZONE,
                    EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATUS_ID);
}

static void handle_ias_status_change(ezb_zcl_ias_zone_status_change_notif_message_t *msg)
{
    if (!msg || !msg->in.header) {
        return;
    }
    uint16_t short_addr = short_from_header(msg->in.header);
    uint8_t endpoint = msg->in.header->src_ep;
    uint16_t status = msg->in.payload.zone_status;
    ESP_LOGI(TAG, "IAS status 0x%04x ep=%u status=0x%04x", short_addr, endpoint, status);
    emit_contact_from_zone_status(short_addr, endpoint, status);
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
}

static void handle_report_attr(ezb_zcl_cmd_report_attr_message_t *msg)
{
    if (!msg || !msg->in.header || !msg->in.variables) {
        return;
    }
    uint16_t short_addr = short_from_header(msg->in.header);
    uint8_t endpoint = msg->in.header->src_ep;
    uint16_t cluster = msg->info.cluster_id;
    ESP_LOGD(TAG, "ZCL report_attr from 0x%04x ep=%u cluster=0x%04x", short_addr, endpoint, cluster);

    for (ezb_zcl_report_attr_variable_t *var = msg->in.variables; var; var = var->next) {
        emit_attr_from_value(short_addr, endpoint, cluster, var->attr_id, var->attr_type, var->attr_value);
    }
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
}

static void handle_read_attr_rsp(ezb_zcl_cmd_read_attr_rsp_message_t *msg)
{
    if (!msg || !msg->in.header || !msg->in.variables) {
        return;
    }
    uint16_t short_addr = short_from_header(msg->in.header);
    uint8_t endpoint = msg->in.header->src_ep;
    uint16_t cluster = msg->info.cluster_id;
    ESP_LOGD(TAG, "ZCL read_attr_rsp from 0x%04x ep=%u cluster=0x%04x", short_addr, endpoint, cluster);

    for (ezb_zcl_read_attr_rsp_variable_t *var = msg->in.variables; var; var = var->next) {
        if (var->status != EZB_ZCL_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "read_attr_rsp 0x%04x attr=0x%04x status=0x%02x", short_addr, var->attr_id,
                     (unsigned)var->status);
            continue;
        }
        emit_attr_from_value(short_addr, endpoint, cluster, var->attr_id, var->attr_type, var->attr_value);
    }
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
}

static void zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID:
        handle_report_attr((ezb_zcl_cmd_report_attr_message_t *)message);
        break;
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID:
        handle_read_attr_rsp((ezb_zcl_cmd_read_attr_rsp_message_t *)message);
        break;
    case EZB_ZCL_CORE_IAS_ZONE_ENROLL_CB_ID:
        handle_ias_enroll((ezb_zcl_ias_zone_enroll_req_message_t *)message);
        break;
    case EZB_ZCL_CORE_IAS_ZONE_STATUS_CHANGE_NOTIF_CB_ID:
        handle_ias_status_change((ezb_zcl_ias_zone_status_change_notif_message_t *)message);
        break;
    default:
        ESP_LOGD(TAG, "zcl core cb id=%u ignored", (unsigned)callback_id);
        break;
    }
}

static esp_err_t create_coordinator_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_on_off_switch_config_t switch_cfg = EZB_ZHA_ON_OFF_SWITCH_CONFIG();
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_on_off_switch(ZB_COORD_ENDPOINT, &switch_cfg);
    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    if (basic_desc) {
        ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                            (void *)ZB_MANUFACTURER_NAME);
        ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                            (void *)ZB_MODEL_IDENTIFIER);
    }

    /* Client clusters so attribute reports / bind targets / IAS CIE are accepted */
    ezb_zcl_cluster_desc_t temp_cli =
        ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    ezb_zcl_cluster_desc_t hum_cli =
        ezb_zcl_rel_humidity_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    ezb_zcl_cluster_desc_t ias_cli = ezb_zcl_ias_zone_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    ezb_zcl_cluster_desc_t em_cli =
        ezb_zcl_electrical_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    ezb_zcl_cluster_desc_t meter_cli = ezb_zcl_metering_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    if (temp_cli) {
        ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(ep_desc, temp_cli)), TAG,
                            "add temp client");
    }
    if (hum_cli) {
        ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(ep_desc, hum_cli)), TAG,
                            "add humidity client");
    }
    if (ias_cli) {
        ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(ep_desc, ias_cli)), TAG,
                            "add IAS Zone client");
    }
    if (em_cli) {
        ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(ep_desc, em_cli)), TAG,
                            "add electrical measurement client");
    }
    if (meter_cli) {
        ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(ep_desc, meter_cli)), TAG,
                            "add metering client");
    }

    ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc)), TAG,
                        "add ep");
    ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_device_desc_register(dev_desc)), TAG, "register device");
    ezb_zcl_core_action_handler_register(zcl_core_action_handler);
    return ESP_OK;
}

static esp_err_t setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    /* HA 1.2 / Tuya sensors expect the well-known global TC link key. */
    ezb_secur_set_global_link_key(k_zb_global_tclk);
    ESP_LOGI(TAG, "global TC link key set to ZigBeeAlliance09");
    /* Keep sleepy joiners in the neighbor table long enough for NWK-key poll. */
    ezb_nwk_set_ed_timeout(EZB_NWK_ED_TIMEOUT_2MIN);
    ESP_LOGI(TAG, "ED timeout=%u (2 min)", (unsigned)ezb_nwk_get_ed_timeout());
    /* Battery EDs (Tuya/eWeLink sensors) often fail unique TCLK exchange while sleepy;
     * allow them to stay after association using the global TC link key. */
    ezb_err_t tclk_err = ezb_secur_set_tclk_exchange_required(false);
    if (tclk_err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "set_tclk_exchange_required(false) failed: %d", (int)tclk_err);
    } else {
        ESP_LOGI(TAG, "TCLK exchange not required (sleepy ED join friendly)");
    }
    ESP_LOGI(TAG, "commissioning: distributed_security=0 primary=0x%08lx secondary=0x%08lx",
             (unsigned long)ZB_PRIMARY_CHANNEL_MASK, (unsigned long)ZB_SECONDARY_CHANNEL_MASK);
    ezb_apsde_data_confirm_handler_register(aps_data_confirm_cb);
    ezb_apsde_data_indication_handler_register(aps_data_indication_cb);
    ESP_LOGI(TAG, "APS confirm/indication hooks registered (deep join logs → tags zb_stack + ezb)");
    ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_bdb_set_primary_channel_set(ZB_PRIMARY_CHANNEL_MASK)), TAG,
                        "primary channel");
    ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_bdb_set_secondary_channel_set(ZB_SECONDARY_CHANNEL_MASK)), TAG,
                        "secondary channel");
    ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_app_signal_add_handler(app_signal_handler)), TAG,
                        "signal handler");
    return ESP_OK;
}

static void esp_zb_task(void *arg)
{
    (void)arg;
    esp_zigbee_config_t config = ZB_DEFAULT_CONFIG();

    ESP_LOGI(TAG, "zb_main: esp_zigbee_init…");
    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    if (nwk_data_frame_disable_extaddr) {
        nwk_data_frame_disable_extaddr();
        ESP_LOGI(TAG, "zb_main: nwk_data_frame_disable_extaddr()");
    } else {
        ESP_LOGW(TAG, "zb_main: nwk_data_frame_disable_extaddr unavailable (stock lib?)");
    }
    ESP_ERROR_CHECK(setup_commissioning());
    ESP_LOGI(TAG, "zb_main: register coordinator endpoint + client clusters");
    ESP_ERROR_CHECK(create_coordinator_device());
    ESP_LOGI(TAG, "zb_main: esp_zigbee_start(false) → mainloop");
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    ESP_LOGW(TAG, "zb_main: mainloop exited — deinit");
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

esp_err_t zigbee_stack_start(void)
{
    if (s_task_started) {
        return ESP_OK;
    }

    /* Join/debug: keep zb_stack + ezb + adapter_zigbee at INFO (filter serial on these). */
    esp_log_level_set("zb_stack", ESP_LOG_INFO);
    esp_log_level_set("ezb", ESP_LOG_INFO);
    esp_log_level_set("adapter_zigbee", ESP_LOG_INFO);
    ESP_LOGI(TAG, "join debug logs ON (tags: zb_stack, ezb, adapter_zigbee)");

    esp_err_t err = nvs_flash_init_partition(ZB_STORAGE_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(ZB_STORAGE_PARTITION));
        err = nvs_flash_init_partition(ZB_STORAGE_PARTITION);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zb_storage NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_fallback_timer) {
        const esp_timer_create_args_t args = {
            .callback = fallback_timer_cb,
            .name = "zb_disc_fb",
        };
        err = esp_timer_create(&args, &s_fallback_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!s_disc_delay_timer) {
        const esp_timer_create_args_t args = {
            .callback = disc_delay_timer_cb,
            .name = "zb_disc_delay",
        };
        err = esp_timer_create(&args, &s_disc_delay_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!s_disc_defer_timer) {
        const esp_timer_create_args_t args = {
            .callback = disc_defer_timer_cb,
            .name = "zb_disc_def",
        };
        err = esp_timer_create(&args, &s_disc_defer_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!s_bdb_retry_timer) {
        const esp_timer_create_args_t args = {
            .callback = bdb_retry_timer_cb,
            .name = "zb_bdb_retry",
        };
        err = esp_timer_create(&args, &s_bdb_retry_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!s_meter_poll_timer) {
        const esp_timer_create_args_t args = {
            .callback = meter_poll_timer_cb,
            .name = "zb_meter_poll",
        };
        err = esp_timer_create(&args, &s_meter_poll_timer);
        if (err != ESP_OK) {
            return err;
        }
    }

    BaseType_t ok = minihub_task_create_psram(esp_zb_task, "zb_main", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_task_started = true;
    ESP_LOGI(TAG, "coordinator task started");
    return ESP_OK;
}

esp_err_t zigbee_stack_stop(void)
{
    /* zb_main is not tear-downable; keep formed network across hub reload. */
    s_permit_until_ms = 0;
    return ESP_OK;
}

esp_err_t zigbee_stack_permit_join(int seconds)
{
    if (seconds < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Close window (idempotent). */
    if (seconds == 0) {
        s_permit_until_ms = 0;
        if (!s_stack_ready) {
            ESP_LOGI(TAG, "permit_join close (stack not ready — local only)");
            return ESP_OK;
        }
        esp_zigbee_lock_acquire(portMAX_DELAY);
        ezb_err_t zb_err = ezb_bdb_open_network(0);
        esp_zigbee_lock_release();
        if (zb_err != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "close permit_join: %d (cleared local window anyway)", (int)zb_err);
        } else {
            ESP_LOGI(TAG, "permit_join closed");
        }
        return ESP_OK;
    }

    if (!s_stack_ready) {
        ESP_LOGW(TAG, "permit_join rejected: coordinator not ready (waiting for form/restore)");
        return ESP_ERR_INVALID_STATE;
    }
    if (zigbee_stack_permit_join_active()) {
        int64_t remain_ms = s_permit_until_ms - (esp_timer_get_time() / 1000);
        ESP_LOGW(TAG, "permit_join rejected: already open (~%lld ms left)", (long long)remain_ms);
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_MINIHUB_BLE_MESH_SDK
    if (ble_mesh_stack_rf_critical()) {
        ESP_LOGW(TAG, "permit_join rejected: BLE Mesh provisioning active");
        return ESP_ERR_INVALID_STATE;
    }
    /* Sleepy end devices (sensors) need Zigbee RX during association; free Mesh scan. */
    ble_mesh_stack_yield_rf();
#endif
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_MINIHUB_BLE_MESH_SDK
    if (!ble_mesh_stack_rf_critical()) {
        (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_CONFIG);
        (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_TRAFFIC);
        (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_STANDBY);
        (void)esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_STANDBY);
    }
#endif
    if (seconds > 254) {
        seconds = 254;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t zb_err = ezb_bdb_open_network((uint8_t)seconds);
    esp_zigbee_lock_release();
    if (zb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "ezb_bdb_open_network(%d) failed: %d", seconds, (int)zb_err);
        return esp_zigbee_err_to_esp(zb_err);
    }
    s_permit_until_ms = esp_timer_get_time() / 1000 + (int64_t)seconds * 1000;
    ESP_LOGI(TAG, "permit_join open %d s (PAN 0x%04x ch=%d short=0x%04x)", seconds, ezb_nwk_get_panid(),
             ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
    return ESP_OK;
}

bool zigbee_stack_permit_join_active(void)
{
    return s_stack_ready && s_permit_until_ms > (esp_timer_get_time() / 1000);
}

esp_err_t zigbee_stack_add_install_code(uint64_t ieee, const uint8_t *ic, size_t ic_len)
{
    if (!ic || ieee == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ezb_secur_ic_type_t ic_type;
    switch (ic_len) {
    case 8:
        ic_type = EZB_SECUR_IC_TYPE_48;
        break;
    case 10:
        ic_type = EZB_SECUR_IC_TYPE_64;
        break;
    case 14:
        ic_type = EZB_SECUR_IC_TYPE_96;
        break;
    case 18:
        ic_type = EZB_SECUR_IC_TYPE_128;
        break;
    default:
        ESP_LOGW(TAG, "install code length %u invalid (need 8/10/14/18)", (unsigned)ic_len);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_stack_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_extaddr_t addr = {.u64 = ieee};
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t zb_err = ezb_secur_ic_add(&addr, ic_type, ic);
    esp_zigbee_lock_release();
    if (zb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "ezb_secur_ic_add ieee=0x%016llx len=%u → %d", (unsigned long long)ieee,
                 (unsigned)ic_len, (int)zb_err);
        return esp_zigbee_err_to_esp(zb_err);
    }
    remember_install_code_ieee(ieee);
    ESP_LOGI(TAG, "install code added ieee=0x%016llx type=%u len=%u", (unsigned long long)ieee,
             (unsigned)ic_type, (unsigned)ic_len);
    return ESP_OK;
}

bool zigbee_stack_running(void)
{
    return s_stack_ready;
}

esp_err_t zigbee_stack_onoff(uint16_t short_addr, uint8_t endpoint, minihub_zigbee_onoff_cmd_t cmd)
{
    if (!s_stack_ready) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_MINIHUB_BLE_MESH_SDK
    /* Zigbee Router + BLE Mesh scan is unsupported; free Mesh RF before TX. */
    ble_mesh_stack_yield_rf();
#endif

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_MINIHUB_BLE_MESH_SDK
    /* Do not demote BLE while Mesh is provisioning / joining. */
    if (!ble_mesh_stack_rf_critical()) {
        (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_CONFIG);
        (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_TRAFFIC);
        (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_STANDBY);
        (void)esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_STANDBY);
    }
#endif

    ezb_zcl_on_off_cmd_t req = {0};
    req.cmd_ctrl.src_ep = ZB_COORD_ENDPOINT;
    req.cmd_ctrl.dst_ep = endpoint ? endpoint : 1;
    ezb_address_set_short(&req.cmd_ctrl.dst_addr, short_addr);

    ezb_err_t zb_err = EZB_ERR_INV_ARG;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    switch (cmd) {
    case MINIHUB_ZB_ONOFF_ON:
        zb_err = ezb_zcl_on_off_on_cmd_req(&req);
        break;
    case MINIHUB_ZB_ONOFF_OFF:
        zb_err = ezb_zcl_on_off_off_cmd_req(&req);
        break;
    case MINIHUB_ZB_ONOFF_TOGGLE:
        zb_err = ezb_zcl_on_off_toggle_cmd_req(&req);
        break;
    default:
        break;
    }
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "onoff 0x%04x ep=%u cmd=%d → zb_err=%d", short_addr, endpoint ? endpoint : 1, (int)cmd,
             (int)zb_err);
    return esp_zigbee_err_to_esp(zb_err);
}

#endif /* CONFIG_MINIHUB_ZIGBEE_SDK */
