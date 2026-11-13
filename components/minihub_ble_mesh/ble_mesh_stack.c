/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-BLE-Mesh Provisioner + OnOff / Level / Lightness / CTL clients.
 */

#include "ble_mesh_stack.h"
#include "ble_mesh_map.h"
#include "ble_mesh_bt.h"
#include "ble_mesh_sensor.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif

static minihub_ble_mesh_device_cb_t s_on_device;
static minihub_ble_mesh_attr_cb_t s_on_attr;
static ble_mesh_stack_ready_cb_t s_on_ready;
static void *s_cb_ctx;
static int64_t s_permit_until_ms;
static bool s_running;
#if CONFIG_MINIHUB_BLE_MESH_SDK
static const char *TAG = "bm_stack";

static bool s_mesh_inited; /* esp_ble_mesh_init done; survives soft disable */
/** True while provisioner is enabled (VALID_PROV). Idle=off so Zigbee can TX. */
static bool s_prov_scan_on;
static SemaphoreHandle_t s_net_gate;
static volatile int s_net_comp_err;
static SemaphoreHandle_t s_bind_gate;
static volatile int s_bind_comp_err;
static bool s_local_models_bound;
static SemaphoreHandle_t s_appkey_gate;
static esp_timer_handle_t s_permit_timer;
/** Return BLE Mesh coex scheme to STANDBY after a short TRAFFIC window. */
static esp_timer_handle_t s_coex_idle_timer;
/** Clears s_prov_busy if add_unprov never reaches LINK_CLOSE. */
static esp_timer_handle_t s_prov_busy_timer;
static uint8_t s_permit_busy_extends;
#endif

#if CONFIG_MINIHUB_BLE_MESH_SDK

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_lighting_model_api.h"
#include "esp_ble_mesh_sensor_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
#include "esp_ble_mesh_proxy_api.h"
#endif

/* ESP-BLE-Mesh core (not public) — used to hold LE scan during PB-GATT if the
 * NimBLE host adapter still restarts discovery after connect. */
extern int bt_mesh_scan_disable(void);
extern int bt_mesh_scan_enable(void);

#define CID_ESP       0x02E5
#define PROV_OWN_ADDR 0x0001
#define MSG_SEND_TTL  3
#define MSG_TIMEOUT   0
#define MSG_ROLE      ROLE_PROVISIONER
#define COMP_DATA_PAGE_0 0x00
#define APP_KEY_IDX   0x0000
#define APP_KEY_OCTET 0x12
/** Wait for PB-ADV settle before Composition Get. */
#define CFG_AFTER_PROV_DELAY_US (2500 * 1000LL)
/** PB-GATT must fully release the ACL slot before Proxy Client can connect. */
#define CFG_AFTER_PROV_GATT_DELAY_US (6000 * 1000LL)
/** After Proxy CCC: wait for Secure Beacon SAR to settle before Composition Get. */
#define CFG_AFTER_PROXY_SETTLE_US (800 * 1000LL)
#define CFG_PROXY_RECONNECT_DELAY_US (2500 * 1000LL)
#define CFG_TIMEOUT_RETRIES_MAX 5
#define CFG_PROXY_RECONNECT_MAX 6
/** Publication is an enhancement, so it gets a shorter budget than binding and
 *  never purges the node when it runs out. */
#define PUB_TIMEOUT_RETRIES_MAX 2
/** PB-GATT connect can hang without LINK_OPEN/CLOSE — escape hatch. */
#define PROV_BUSY_WATCHDOG_US (45 * 1000000ULL)
/** Cap "busy → extend permit" so a stuck busy cannot hold the window forever. */
#define PERMIT_BUSY_EXTEND_MAX 1

typedef struct {
    uint8_t uuid[16];
    uint8_t bd_addr[BD_ADDR_LEN];
    esp_ble_mesh_addr_type_t addr_type;
    bool have_addr;
    uint16_t unicast;
    uint8_t elem_num;
    uint8_t onoff;
    uint16_t caps;
    uint16_t cid;
    uint16_t pid;
    uint16_t vid;
    /** Sensor property ids seen in the Sensor Descriptor (classification input). */
    uint16_t props[MINIHUB_BM_MAP_MAX_PROPS];
    size_t prop_count;
    /** Snapshot of s_bind_jobs for this node (survives sensor-delayed notify). */
    minihub_bm_bound_model_t binds[MINIHUB_BM_BIND_MODEL_MAX];
    size_t bind_count;
} node_info_t;

typedef struct {
    uint16_t elem_addr;
    uint16_t model_id;
    uint16_t company_id;
} bind_job_t;

enum {
    CFG_ACT_PROXY_CONNECT = 1,
    CFG_ACT_COMP_GET,
    CFG_ACT_PROXY_RECONNECT,
};

static uint8_t s_dev_uuid[16];
static node_info_t s_nodes[CONFIG_BLE_MESH_MAX_PROV_NODES];
static uint8_t s_tid;
static esp_timer_handle_t s_cfg_delay_timer;
static uint16_t s_cfg_pending_unicast;
static uint8_t s_cfg_timeout_retries;
static int s_cfg_timer_action;
static size_t s_bind_model_idx;
static size_t s_pub_model_idx;
/** This model already had its Publication Set retried with period 0. */
static bool s_pub_no_period;
static uint8_t s_pub_retries;
static bind_job_t s_bind_jobs[MINIHUB_BM_BIND_MODEL_MAX];
static size_t s_bind_job_count;
static minihub_bm_provision_policy_t s_policy;
static bool s_policy_set;
/** Nodes that matched no classify rule — diagnostic counter for D4a. */
static uint32_t s_unclassified_nodes;
/** True from add_unprov until link close / prov complete — blocks re-entry and
 *  keeps BLE coex priority (PB-GATT close_link always reports HCI 0x16). */
static bool s_prov_busy;
/** Set on PROV_COMPLETE so the following PB-GATT ACL teardown (HCI 0x16) is not
 *  treated as a failed provision. */
static bool s_prov_succeeded;
static uint8_t s_prov_uuid[16];
static bool s_prov_uuid_valid;
static int64_t s_prov_retry_after_ms;
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
static bool s_cfg_waiting_proxy;
static bool s_cfg_proxy_ready;
static uint8_t s_proxy_reconnects;
static uint8_t s_pending_bd_addr[BD_ADDR_LEN];
static esp_ble_mesh_addr_type_t s_pending_addr_type;
static bool s_pending_have_addr;
/** Last open prov link was PB-GATT (needs longer ACL teardown before Proxy). */
static bool s_prov_link_was_gatt;
#endif

/** The server models the map table knows — the bind list an OEM overlay can extend. */
static size_t map_bind_models(minihub_bm_bind_model_t *out, size_t max)
{
    size_t n = 0;
    size_t models = minihub_bm_model_count();
    for (size_t i = 0; i < models && n < max; i++) {
        const minihub_bm_map_model_t *m = minihub_bm_model_at(i);
        if (!m || !m->model_id) {
            continue;
        }
        out[n].model_id = m->model_id;
        out[n].company_id = MINIHUB_BM_CID_SIG;
        n++;
    }
    return n;
}

static void policy_apply_defaults(void)
{
    memset(&s_policy, 0, sizeof(s_policy));
    /* Open until packs / adapters.json inject uuid_filters. */
    s_policy.filter_count = 0;
    s_policy.accept_all_uuid = true;
    s_policy.bind_model_count = map_bind_models(s_policy.bind_models,
                                                 MINIHUB_BM_BIND_MODEL_MAX);
}

esp_err_t ble_mesh_stack_set_provision_policy(const minihub_bm_provision_policy_t *policy)
{
    if (!policy) {
        return ESP_ERR_INVALID_ARG;
    }
    s_policy = *policy;
    /* The map owns the models the Host binds; a policy (adapters.json or a
     * pack) adds to that list instead of replacing it. The map models come
     * first so a full policy list cannot push them out of the bind set. */
    minihub_bm_bind_model_t map_models[MINIHUB_BM_BIND_MODEL_MAX];
    size_t map_count = map_bind_models(map_models, ARRAY_SIZE(map_models));
    minihub_bm_bind_model_t merged[MINIHUB_BM_BIND_MODEL_MAX];
    size_t merged_count = 0;
    for (size_t i = 0; i < map_count; i++) {
        merged[merged_count++] = map_models[i];
    }
    for (size_t i = 0; i < s_policy.bind_model_count; i++) {
        bool known = false;
        for (size_t j = 0; j < merged_count; j++) {
            if (merged[j].model_id == s_policy.bind_models[i].model_id) {
                known = true;
                break;
            }
        }
        if (!known && merged_count < MINIHUB_BM_BIND_MODEL_MAX) {
            merged[merged_count++] = s_policy.bind_models[i];
        }
    }
    memcpy(s_policy.bind_models, merged, sizeof(merged));
    s_policy.bind_model_count = merged_count;
    s_policy_set = true;
    ESP_LOGI(TAG, "provision policy: filters=%u accept_all=%d bind_models=%u%s",
             (unsigned)s_policy.filter_count, (int)s_policy.accept_all_uuid,
             (unsigned)s_policy.bind_model_count,
             s_running ? " (live)" : "");
    return ESP_OK;
}

static bool uuid_matches_policy(const uint8_t uuid[16])
{
    if (!uuid) {
        return false;
    }
    if (s_policy.accept_all_uuid || s_policy.filter_count == 0) {
        return true;
    }
    for (size_t i = 0; i < s_policy.filter_count; i++) {
        const minihub_bm_uuid_filter_t *f = &s_policy.filters[i];
        if (f->len == 0) {
            continue;
        }
        if ((size_t)f->offset + f->len > 16) {
            continue;
        }
        if (memcmp(uuid + f->offset, f->bytes, f->len) == 0) {
            return true;
        }
    }
    return false;
}

static struct {
    uint16_t net_idx;
    uint16_t app_idx;
    uint8_t app_key[16];
} s_prov_key;

static esp_ble_mesh_client_t s_config_client;
static esp_ble_mesh_client_t s_onoff_client;
static esp_ble_mesh_client_t s_level_client;
static esp_ble_mesh_client_t s_lightness_client;
static esp_ble_mesh_client_t s_ctl_client;
static esp_ble_mesh_client_t s_hsl_client;
static esp_ble_mesh_client_t s_sensor_client;

static esp_ble_mesh_cfg_srv_t s_config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    /* Gateway/provisioner: no Secure Network Beacon — periodic BLE adv starves Zigbee RX. */
    .beacon = ESP_BLE_MESH_BEACON_DISABLED,
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
};

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&s_config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(NULL, &s_onoff_client),
    ESP_BLE_MESH_MODEL_GEN_LEVEL_CLI(NULL, &s_level_client),
    ESP_BLE_MESH_MODEL_LIGHT_LIGHTNESS_CLI(NULL, &s_lightness_client),
    ESP_BLE_MESH_MODEL_LIGHT_CTL_CLI(NULL, &s_ctl_client),
    ESP_BLE_MESH_MODEL_LIGHT_HSL_CLI(NULL, &s_hsl_client),
    ESP_BLE_MESH_MODEL_SENSOR_CLI(NULL, &s_sensor_client),
};

static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, ESP_BLE_MESH_MODEL_NONE),
};

/**
 * The client models the app key has to be bound to: the ones the map pairs
 * with a server model and that this element actually carries.
 */
static size_t map_local_client_models(uint16_t *out, size_t max)
{
    size_t n = 0;
    size_t models = minihub_bm_model_count();
    for (size_t i = 0; i < models && n < max; i++) {
        const minihub_bm_map_model_t *m = minihub_bm_model_at(i);
        if (!m || !m->client_id) {
            continue;
        }
        bool carried = false;
        for (size_t r = 0; r < ARRAY_SIZE(s_root_models); r++) {
            if (s_root_models[r].model_id == m->client_id) {
                carried = true;
                break;
            }
        }
        if (!carried) {
            continue;
        }
        bool seen = false;
        for (size_t k = 0; k < n; k++) {
            if (out[k] == m->client_id) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            out[n++] = m->client_id;
        }
    }
    return n;
}

static esp_ble_mesh_comp_t s_composition = {
    .cid = CID_ESP,
    .element_count = ARRAY_SIZE(s_elements),
    .elements = s_elements,
};

static esp_ble_mesh_prov_t s_provision = {
    .prov_uuid = s_dev_uuid,
    .prov_unicast_addr = PROV_OWN_ADDR,
    .prov_start_address = 0x0005,
    .prov_attention = 0x00,
    .prov_algorithm = 0x00,
    .prov_pub_key_oob = 0x00,
    .prov_static_oob_val = NULL,
    .prov_static_oob_len = 0x00,
    .flags = 0x00,
    .iv_index = 0x00,
};

static esp_err_t store_node_info(const uint8_t uuid[16], uint16_t unicast, uint8_t elem_num, uint8_t onoff)
{
    if (!uuid || !ESP_BLE_MESH_ADDR_IS_UNICAST(unicast)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < ARRAY_SIZE(s_nodes); i++) {
        if (memcmp(s_nodes[i].uuid, uuid, 16) == 0) {
            s_nodes[i].unicast = unicast;
            s_nodes[i].elem_num = elem_num;
            s_nodes[i].onoff = onoff;
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
            if (s_pending_have_addr) {
                memcpy(s_nodes[i].bd_addr, s_pending_bd_addr, BD_ADDR_LEN);
                s_nodes[i].addr_type = s_pending_addr_type;
                s_nodes[i].have_addr = true;
            }
#endif
            return ESP_OK;
        }
    }
    for (int i = 0; i < ARRAY_SIZE(s_nodes); i++) {
        if (s_nodes[i].unicast == ESP_BLE_MESH_ADDR_UNASSIGNED) {
            memcpy(s_nodes[i].uuid, uuid, 16);
            s_nodes[i].unicast = unicast;
            s_nodes[i].elem_num = elem_num;
            s_nodes[i].onoff = onoff;
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
            if (s_pending_have_addr) {
                memcpy(s_nodes[i].bd_addr, s_pending_bd_addr, BD_ADDR_LEN);
                s_nodes[i].addr_type = s_pending_addr_type;
                s_nodes[i].have_addr = true;
            } else {
                s_nodes[i].have_addr = false;
            }
#endif
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static node_info_t *get_node_info(uint16_t unicast)
{
    if (!ESP_BLE_MESH_ADDR_IS_UNICAST(unicast)) {
        return NULL;
    }
    for (int i = 0; i < ARRAY_SIZE(s_nodes); i++) {
        if (s_nodes[i].unicast <= unicast &&
            s_nodes[i].unicast + s_nodes[i].elem_num > unicast) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static esp_err_t set_msg_common(esp_ble_mesh_client_common_param_t *common, node_info_t *node,
                                esp_ble_mesh_model_t *model, uint32_t opcode)
{
    if (!common || !node || !model) {
        return ESP_ERR_INVALID_ARG;
    }
    common->opcode = opcode;
    common->model = model;
    common->ctx.net_idx = s_prov_key.net_idx;
    /* Config Client always uses Device Key; OnOff Client uses AppKey. */
    if (model == s_config_client.model) {
        common->ctx.app_idx = ESP_BLE_MESH_KEY_DEV;
    } else {
        common->ctx.app_idx = s_prov_key.app_idx;
    }
    common->ctx.addr = node->unicast;
    common->ctx.send_ttl = MSG_SEND_TTL;
    common->msg_timeout = MSG_TIMEOUT;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
    common->msg_role = MSG_ROLE;
#endif
    return ESP_OK;
}

static void notify_device_ready(const node_info_t *node)
{
    if (!node || !s_on_device) {
        return;
    }
    uint16_t caps = node->caps;
    const char *type = minihub_bm_classify(caps, node->props, node->prop_count);
    if (!type) {
        if ((caps & MINIHUB_BM_CAP_SENSOR) && node->prop_count == 0) {
            /* A sensor node is typed by the properties it reports, so it waits
             * for its descriptor instead of being written off as unknown. */
            ESP_LOGI(TAG, "node 0x%04x: sensor node, waiting for the descriptor",
                     node->unicast);
            return;
        }
        /* D4a: an unmatched node gets no point — it must not masquerade as a
         * controllable switch. It stays in s_nodes so a later re-classify can
         * still find it. */
        s_unclassified_nodes++;
        ESP_LOGW(TAG, "node 0x%04x: no classify rule (caps=0x%04x), not registered (unclassified=%u)",
                 node->unicast, caps, (unsigned)s_unclassified_nodes);
        return;
    }
    minihub_ble_mesh_device_t dev = {0};
    strncpy(dev.type, type, sizeof(dev.type) - 1);
    (void)minihub_bm_format_point_id(dev.point_id, sizeof(dev.point_id), node->unicast);
    (void)minihub_bm_uuid_to_hex(node->uuid, dev.uuid_hex, sizeof(dev.uuid_hex));
    dev.unicast = node->unicast;
    dev.elem_num = node->elem_num;
    dev.caps = caps;
    dev.cid = node->cid;
    dev.pid = node->pid;
    dev.vid = node->vid;
    dev.prop_count = node->prop_count;
    for (size_t i = 0; i < node->prop_count && i < MINIHUB_BM_MAX_PROPS; i++) {
        dev.props[i] = node->props[i];
    }
    dev.bind_count = node->bind_count;
    for (size_t i = 0; i < node->bind_count && i < MINIHUB_BM_BIND_MODEL_MAX; i++) {
        dev.binds[i] = node->binds[i];
    }
    s_on_device(&dev, s_cb_ctx);
}

static void notify_onoff(uint16_t unicast, bool on)
{
    if (!s_on_attr) {
        return;
    }
    minihub_ble_mesh_attr_evt_t evt = {
        .unicast = unicast,
        .kind = MINIHUB_BM_ATTR_ONOFF,
        .u.onoff = on,
    };
    s_on_attr(&evt, s_cb_ctx);
}

static uint16_t model_id_to_cap(uint16_t model_id)
{
    uint16_t caps = 0;
    if (!minihub_bm_model_cap(model_id, &caps, NULL)) {
        return 0;
    }
    return caps;
}

static bool policy_wants_model(uint16_t model_id)
{
    if (s_policy.bind_model_count == 0) {
        return model_id_to_cap(model_id) != 0;
    }
    for (size_t i = 0; i < s_policy.bind_model_count; i++) {
        if (s_policy.bind_models[i].model_id == model_id &&
            s_policy.bind_models[i].company_id == MINIHUB_BM_CID_SIG) {
            return true;
        }
    }
    return false;
}

static void bind_job_add(uint16_t elem_addr, uint16_t model_id)
{
    if (s_bind_job_count >= MINIHUB_BM_BIND_MODEL_MAX) {
        return;
    }
    for (size_t i = 0; i < s_bind_job_count; i++) {
        if (s_bind_jobs[i].elem_addr == elem_addr && s_bind_jobs[i].model_id == model_id) {
            return;
        }
    }
    s_bind_jobs[s_bind_job_count].elem_addr = elem_addr;
    s_bind_jobs[s_bind_job_count].model_id = model_id;
    s_bind_jobs[s_bind_job_count].company_id = MINIHUB_BM_CID_SIG;
    s_bind_job_count++;
}

static void ingest_composition(node_info_t *node, const uint8_t *data, uint16_t len)
{
    s_bind_job_count = 0;
    if (!node || !data || len < 10) {
        return;
    }
    node->caps = 0;
    node->cid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    node->pid = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    node->vid = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    size_t off = 10; /* CID PID VID CRPL Features */
    uint16_t elem_addr = node->unicast;
    while (off + 4 <= len) {
        uint8_t num_s = data[off + 2];
        uint8_t num_v = data[off + 3];
        off += 4;
        for (uint8_t i = 0; i < num_s; i++) {
            if (off + 2 > len) {
                return;
            }
            uint16_t mid = (uint16_t)data[off] | ((uint16_t)data[off + 1] << 8);
            off += 2;
            uint8_t cap = model_id_to_cap(mid);
            if (cap) {
                node->caps |= cap;
            }
            if (policy_wants_model(mid)) {
                bind_job_add(elem_addr, mid);
            }
        }
        size_t vend = (size_t)num_v * 4;
        if (off + vend > len) {
            return;
        }
        off += vend;
        elem_addr++;
    }
    if (s_bind_job_count == 0) {
        for (size_t i = 0; i < s_policy.bind_model_count; i++) {
            bind_job_add(node->unicast, s_policy.bind_models[i].model_id);
        }
    }
    ESP_LOGI(TAG, "composition 0x%04x cid=0x%04x pid=0x%04x vid=0x%04x caps=0x%02x bind_jobs=%u",
             node->unicast, node->cid, node->pid, node->vid, node->caps,
             (unsigned)s_bind_job_count);
}

static esp_err_t send_comp_data_get(node_info_t *node)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_get_state_t get_state = {0};
    set_msg_common(&common, node, s_config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
    get_state.comp_data_get.page = COMP_DATA_PAGE_0;
    int err = esp_ble_mesh_config_client_get_state(&common, &get_state);
    if (err) {
        ESP_LOGE(TAG, "Composition Data Get failed: %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_app_key_add(node_info_t *node)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_set_state_t set_state = {0};
    set_msg_common(&common, node, s_config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
    set_state.app_key_add.net_idx = s_prov_key.net_idx;
    set_state.app_key_add.app_idx = s_prov_key.app_idx;
    memcpy(set_state.app_key_add.app_key, s_prov_key.app_key, 16);
    int err = esp_ble_mesh_config_client_set_state(&common, &set_state);
    if (err) {
        ESP_LOGE(TAG, "AppKey Add failed: %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_model_app_bind_at(node_info_t *node, size_t idx)
{
    if (!node || idx >= s_bind_job_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const bind_job_t *job = &s_bind_jobs[idx];
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_set_state_t set_state = {0};
    set_msg_common(&common, node, s_config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
    set_state.model_app_bind.element_addr = job->elem_addr;
    set_state.model_app_bind.model_app_idx = s_prov_key.app_idx;
    set_state.model_app_bind.model_id = job->model_id;
    set_state.model_app_bind.company_id =
        (job->company_id == MINIHUB_BM_CID_SIG) ? ESP_BLE_MESH_CID_NVAL : job->company_id;
    ESP_LOGI(TAG, "Model App Bind[%u] model=0x%04x elem=0x%04x",
             (unsigned)idx, job->model_id, job->elem_addr);
    int err = esp_ble_mesh_config_client_set_state(&common, &set_state);
    if (err) {
        ESP_LOGE(TAG, "Model App Bind failed: %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_model_app_bind(node_info_t *node)
{
    return send_model_app_bind_at(node, s_bind_model_idx);
}

/** Ask a node which sensors it carries: the descriptor names the property ids. */
static esp_err_t send_sensor_descriptor_get(node_info_t *node)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_sensor_client_get_state_t get_state = {0};
    set_msg_common(&common, node, s_sensor_client.model, ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_GET);
    get_state.descriptor_get.op_en = false;
    esp_err_t err = esp_ble_mesh_sensor_client_get_state(&common, &get_state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sensor Descriptor Get failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/** Ask a node for the sensor values it currently holds. */
static esp_err_t send_sensor_get(node_info_t *node)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_sensor_client_get_state_t get_state = {0};
    set_msg_common(&common, node, s_sensor_client.model, ESP_BLE_MESH_MODEL_OP_SENSOR_GET);
    get_state.sensor_get.op_en = false;
    esp_err_t err = esp_ble_mesh_sensor_client_get_state(&common, &get_state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sensor Get failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* One Publication Set at a time: the config client answers a second outstanding
 * acknowledged message to the same node with -EBUSY, so a loop over the models
 * would configure only the first one. */
static esp_err_t send_model_pub_set_at(node_info_t *node, size_t idx)
{
    const minihub_bm_report_t *rep = minihub_bm_report();
    if (!node || idx >= s_bind_job_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const bind_job_t *job = &s_bind_jobs[idx];
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_set_state_t set_state = {0};
    set_msg_common(&common, node, s_config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET);
    set_state.model_pub_set.element_addr = job->elem_addr;
    set_state.model_pub_set.publish_addr = rep->group;
    set_state.model_pub_set.publish_app_idx = s_prov_key.app_idx;
    set_state.model_pub_set.cred_flag = false;
    set_state.model_pub_set.publish_ttl = rep->ttl;
    set_state.model_pub_set.publish_period = s_pub_no_period ? 0 : rep->period_octet;
    /* The macro's interval field is (ms / 50) - 1, so a zero count must not
     * go through it. */
    set_state.model_pub_set.publish_retransmit = rep->retransmit_count
        ? ESP_BLE_MESH_PUBLISH_TRANSMIT(rep->retransmit_count, rep->retransmit_ms)
        : 0;
    set_state.model_pub_set.model_id = job->model_id;
    set_state.model_pub_set.company_id =
        (job->company_id == MINIHUB_BM_CID_SIG) ? ESP_BLE_MESH_CID_NVAL : job->company_id;
    int err = esp_ble_mesh_config_client_set_state(&common, &set_state);
    if (err) {
        ESP_LOGW(TAG, "pub set model 0x%04x elem 0x%04x -> 0x%04x: %d",
                 job->model_id, job->elem_addr, rep->group, err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void configure_status_reporting(node_info_t *node)
{
    const minihub_bm_report_t *rep = minihub_bm_report();
    if (!rep->enabled || !ESP_BLE_MESH_ADDR_IS_GROUP(rep->group)) {
        return;
    }
    s_pub_model_idx = 0;
    s_pub_no_period = false;
    s_pub_retries = 0;
    if (s_bind_job_count == 0) {
        return;
    }
    ESP_LOGI(TAG, "publication: %u server model(s) of 0x%04x -> group 0x%04x period 0x%02x",
             (unsigned)s_bind_job_count, node->unicast, (unsigned)rep->group,
             (unsigned)rep->period_octet);
    (void)send_model_pub_set_at(node, 0);
}

static void advance_status_reporting(node_info_t *node)
{
    s_pub_model_idx++;
    s_pub_no_period = false;
    s_pub_retries = 0;
    if (s_pub_model_idx < s_bind_job_count) {
        (void)send_model_pub_set_at(node, s_pub_model_idx);
        return;
    }
    ESP_LOGI(TAG, "publication finished on 0x%04x (%u server model(s))",
             (unsigned)s_bind_job_count, node->unicast);
}

static void snapshot_bind_jobs(node_info_t *node)
{
    if (!node) {
        return;
    }
    node->bind_count = 0;
    for (size_t i = 0; i < s_bind_job_count && i < MINIHUB_BM_BIND_MODEL_MAX; i++) {
        node->binds[node->bind_count].model_id = s_bind_jobs[i].model_id;
        node->binds[node->bind_count].elem_addr = s_bind_jobs[i].elem_addr;
        node->bind_count++;
    }
}

/**
 * Ask every bound model that declares a Get for its state. The point registers
 * with the map's init values, so a model that is never asked leaves the UI
 * showing a default as if the node had reported it. Each client model owns its
 * own outstanding-message queue, so these go back to back.
 */
static void read_back_bound_state(node_info_t *node, const minihub_bm_bound_model_t *binds,
                                  size_t bind_count)
{
    if (!node || !binds || bind_count == 0) {
        return;
    }
    size_t sent = 0;
    bool bound_sensor = false;
    for (size_t i = 0; i < bind_count; i++) {
        const minihub_bm_bound_model_t *job = &binds[i];
        if (job->model_id == ESP_BLE_MESH_MODEL_ID_SENSOR_SRV) {
            bound_sensor = true;
        }
        const minihub_bm_map_model_t *m = minihub_bm_model_by_id(job->model_id);
        if (!m || !m->get_opcode || !m->client_id) {
            continue;
        }
        esp_ble_mesh_model_t *client = NULL;
        for (size_t r = 0; r < ARRAY_SIZE(s_root_models); r++) {
            if (s_root_models[r].model_id == m->client_id) {
                client = &s_root_models[r];
                break;
            }
        }
        if (!client || !client->user_data) {
            continue;
        }
        esp_ble_mesh_client_common_param_t common = {0};
        esp_ble_mesh_generic_client_get_state_t generic_get = {0};
        set_msg_common(&common, node, client, m->get_opcode);
        common.ctx.addr = job->elem_addr;
        bool lighting = client->user_data == &s_lightness_client ||
                        client->user_data == &s_ctl_client ||
                        client->user_data == &s_hsl_client;
        int err = lighting ? esp_ble_mesh_light_client_get_state(&common, NULL)
                           : esp_ble_mesh_generic_client_get_state(&common, &generic_get);
        if (err) {
            ESP_LOGW(TAG, "state get model 0x%04x elem 0x%04x op=0x%04x: %d",
                     job->model_id, job->elem_addr, (unsigned)m->get_opcode, err);
            continue;
        }
        sent++;
    }
    ESP_LOGI(TAG, "state read-back: %u Get(s) sent to 0x%04x", (unsigned)sent, node->unicast);
    /* First provision: Descriptor fills props then sends Sensor Get. Reboot:
     * props were restored from NVS — Get values without another Descriptor. */
    if ((bound_sensor || (node->caps & MINIHUB_BM_CAP_SENSOR)) && node->prop_count > 0) {
        (void)send_sensor_get(node);
    }
}

static void finish_model_binds(node_info_t *node)
{
    s_cfg_pending_unicast = 0;
    snapshot_bind_jobs(node);
    ESP_LOGI(TAG, "model binds complete on 0x%04x (%u jobs) caps=0x%02x",
             node->unicast, (unsigned)node->bind_count, node->caps);
    notify_device_ready(node);

    read_back_bound_state(node, node->binds, node->bind_count);

    bool bound_sensor = false;
    for (size_t i = 0; i < node->bind_count; i++) {
        if (node->binds[i].model_id == ESP_BLE_MESH_MODEL_ID_SENSOR_SRV) {
            bound_sensor = true;
        }
    }
    if (bound_sensor) {
        /* The property ids decide the device type, so the node is registered
         * only once its descriptor arrives. */
        ESP_LOGI(TAG, "Sensor Server bound on 0x%04x", node->unicast);
        (void)send_sensor_descriptor_get(node);
    }
    configure_status_reporting(node);
}

static void schedule_cfg_timer(int action, int64_t delay_us);

/** Bind AppKey to local client models — required after every boot for OnOff Set. */
static esp_err_t bind_local_client_models(void)
{
    uint16_t local_binds[MINIHUB_BM_BIND_MODEL_MAX];
    size_t local_bind_count = map_local_client_models(local_binds, ARRAY_SIZE(local_binds));
    if (!s_bind_gate) {
        s_bind_gate = xSemaphoreCreateCounting(16, 0);
    }
    if (!s_bind_gate) {
        return ESP_ERR_NO_MEM;
    }
    /* Drain stale completions from a previous attempt. */
    while (xSemaphoreTake(s_bind_gate, 0) == pdTRUE) {
    }
    s_bind_comp_err = 0;

    size_t queued = 0;
    for (size_t i = 0; i < local_bind_count; i++) {
        esp_err_t err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
            PROV_OWN_ADDR, s_prov_key.app_idx, local_binds[i], ESP_BLE_MESH_CID_NVAL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bind local model 0x%04x queue: %s", local_binds[i],
                     esp_err_to_name(err));
            continue;
        }
        queued++;
    }
    for (size_t i = 0; i < queued; i++) {
        if (xSemaphoreTake(s_bind_gate, pdMS_TO_TICKS(1500)) != pdTRUE) {
            ESP_LOGE(TAG, "bind local models timeout (%u/%u)", (unsigned)i, (unsigned)queued);
            return ESP_ERR_TIMEOUT;
        }
    }
    if (s_bind_comp_err != 0) {
        ESP_LOGW(TAG, "bind local models comp err=%d (may already be bound)", s_bind_comp_err);
    }
    ESP_LOGI(TAG, "local client models bound to app_idx=%u (%u)", (unsigned)s_prov_key.app_idx,
             (unsigned)queued);
    /* The binding is persisted and provisioner disable does not drop it, so a
     * clean pass is needed only once; anything short of it is retried on the next TX. */
    s_local_models_bound = (queued == local_bind_count && s_bind_comp_err == 0);

    /* A published Status is delivered only to models subscribed to its group, so
     * the listener has to be in place before any node publishes anything. */
    const minihub_bm_report_t *rep = minihub_bm_report();
    if (rep->enabled) {
        if (!ESP_BLE_MESH_ADDR_IS_GROUP(rep->group)) {
            ESP_LOGW(TAG, "report group 0x%04x is not a group address — reporting stays off",
                     (unsigned)rep->group);
        } else {
            for (size_t i = 0; i < local_bind_count; i++) {
                esp_err_t serr = esp_ble_mesh_model_subscribe_group_addr(
                    PROV_OWN_ADDR, ESP_BLE_MESH_CID_NVAL, local_binds[i], rep->group);
                if (serr != ESP_OK) {
                    ESP_LOGW(TAG, "subscribe local model 0x%04x to 0x%04x: %s",
                             (unsigned)local_binds[i], (unsigned)rep->group,
                             esp_err_to_name(serr));
                }
            }
            ESP_LOGI(TAG, "local client models subscribed to status group 0x%04x",
                     (unsigned)rep->group);
        }
    }
    return ESP_OK;
}

static const esp_ble_mesh_prov_bearer_t s_prov_bearers =
    (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
/* Coex API accepts one scheme bit at a time; OR-ing bits logs
 * "Incorrect coexistence BLE status: 56/24". */
static void coex_mesh_clear_all(void)
{
    (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_CONFIG);
    (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_TRAFFIC);
    (void)esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, ESP_COEX_BLE_ST_MESH_STANDBY);
}

static void coex_mesh_set(uint32_t bit)
{
    coex_mesh_clear_all();
    (void)esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BLE, bit);
}

static void coex_idle_timer_cb(void *arg)
{
    (void)arg;
    /* Never demote BLE while joining / mid PB-GATT — STANDBY starves the ACL
     * and provisioner close_link then surfaces as HCI 0x16. */
    if (s_prov_busy || ble_mesh_stack_permit_provision_active()) {
        coex_mesh_set(ESP_COEX_BLE_ST_MESH_CONFIG);
        return;
    }
    /* After Mesh TX window: drop provisioner so Zigbee can use the RF. */
    if (s_prov_scan_on) {
        esp_err_t err = esp_ble_mesh_provisioner_prov_disable(s_prov_bearers);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "idle prov disable: %s", esp_err_to_name(err));
        }
        /* s_prov_scan_on cleared in PROV_DISABLE_COMP */
    }
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    coex_mesh_set(ESP_COEX_BLE_ST_MESH_STANDBY);
#endif
}

static void coex_mesh_standby(void)
{
    if (s_coex_idle_timer) {
        esp_timer_stop(s_coex_idle_timer);
    }
    coex_mesh_set(ESP_COEX_BLE_ST_MESH_STANDBY);
}

static void coex_mesh_config(void)
{
    if (s_coex_idle_timer) {
        esp_timer_stop(s_coex_idle_timer);
    }
    coex_mesh_set(ESP_COEX_BLE_ST_MESH_CONFIG);
}

static void coex_mesh_traffic(void)
{
    coex_mesh_set(ESP_COEX_BLE_ST_MESH_TRAFFIC);
    if (s_coex_idle_timer) {
        esp_timer_stop(s_coex_idle_timer);
        /* Keep Mesh RF priority long enough for OnOff Set + optional Status. */
        (void)esp_timer_start_once(s_coex_idle_timer, 1500 * 1000);
    }
}

static esp_err_t ensure_coex_idle_timer(void)
{
    if (s_coex_idle_timer) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback = &coex_idle_timer_cb,
        .name = "bm_coex_idle",
    };
    return esp_timer_create(&args, &s_coex_idle_timer);
}
#else
static void coex_mesh_standby(void) {}
static void coex_mesh_config(void) {}
static void coex_mesh_traffic(void) {}
static esp_err_t ensure_coex_idle_timer(void) { return ESP_OK; }
#endif

static void net_gate_ensure(void)
{
    if (!s_net_gate) {
        s_net_gate = xSemaphoreCreateBinary();
    }
}

static void net_gate_signal(int err_code)
{
    s_net_comp_err = err_code;
    net_gate_ensure();
    if (s_net_gate) {
        xSemaphoreGive(s_net_gate);
    }
}

/**
 * Enable/disable provisioner network.
 * Full disable clears VALID_PROV (OnOff then fails with EADDRNOTAVAIL).
 * Idle path keeps it disabled so Zigbee can TX; Mesh TX re-enables briefly.
 */
static esp_err_t prov_network_set(bool enable)
{
    if (enable == s_prov_scan_on) {
        return ESP_OK;
    }
    net_gate_ensure();
    if (s_net_gate) {
        (void)xSemaphoreTake(s_net_gate, 0); /* drain */
    }

    esp_err_t err;
    if (enable) {
        coex_mesh_config();
        err = esp_ble_mesh_provisioner_prov_enable(s_prov_bearers);
        if (err == ESP_ERR_INVALID_STATE) {
            s_prov_scan_on = true;
            ESP_LOGI(TAG, "provisioner network already on");
            return ESP_OK;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "prov enable: %s", esp_err_to_name(err));
            coex_mesh_standby();
            return err;
        }
        if (!s_net_gate || xSemaphoreTake(s_net_gate, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "prov enable completion timeout");
            return ESP_ERR_TIMEOUT;
        }
        if (s_net_comp_err != 0 && s_net_comp_err != -EALREADY) {
            ESP_LOGE(TAG, "prov enable comp err=%d", s_net_comp_err);
            return ESP_FAIL;
        }
        s_prov_scan_on = true;
        ESP_LOGI(TAG, "provisioner network on");
    } else {
        err = esp_ble_mesh_provisioner_prov_disable(s_prov_bearers);
        if (err == ESP_ERR_INVALID_STATE) {
            s_prov_scan_on = false;
            coex_mesh_standby();
            return ESP_OK;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "prov disable: %s", esp_err_to_name(err));
            return err;
        }
        if (s_net_gate) {
            (void)xSemaphoreTake(s_net_gate, pdMS_TO_TICKS(3000));
        }
        s_prov_scan_on = false;
        coex_mesh_standby();
        ESP_LOGI(TAG, "provisioner network off (Zigbee RF free)");
    }
    return ESP_OK;
}

/** Bring Mesh network up for a client message; schedule idle release. */
static esp_err_t mesh_prepare_tx(void)
{
    esp_err_t err = prov_network_set(true);
    if (err != ESP_OK) {
        return err;
    }
    /* Bind is async (BTC); wait for COMP or OnOff fails with EINVAL (-22). */
    if (!s_local_models_bound) {
        err = bind_local_client_models();
        if (err != ESP_OK) {
            return err;
        }
    }
    coex_mesh_traffic();
    return ESP_OK;
}

/*
 * Control Sets over the ADV bearer get no link-layer retry: a lost packet is a
 * lost command. Each Set is therefore repeated as the Unack opcode with the
 * same TID. A server drops a message whose (src, dst, TID) matches the last one
 * it handled within 6 s, so a repeat that follows a
 * delivered Set changes nothing, and one that follows a lost Set applies it.
 * A newer Set of the same kind to the same node replaces the pending repeats,
 * so a stale value is never replayed over it. Repeats stay inside the 1.5 s
 * TRAFFIC coex window opened by mesh_prepare_tx().
 */
#define TX_REPEAT_COUNT       2
#define TX_REPEAT_INTERVAL_US (150 * 1000LL)
#define TX_REPEAT_TICK_US     (50 * 1000LL)
#define TX_REPEAT_SLOTS       8

typedef struct {
    bool used;
    bool light; /* light client API vs generic client API */
    uint8_t left;
    int64_t due_us;
    esp_ble_mesh_client_common_param_t common;
    union {
        esp_ble_mesh_generic_client_set_state_t gen;
        esp_ble_mesh_light_client_set_state_t light;
    } set;
} tx_repeat_t;

static tx_repeat_t s_tx_repeat[TX_REPEAT_SLOTS];
static portMUX_TYPE s_tx_repeat_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_tx_repeat_timer;

static uint32_t tx_unack_opcode(uint32_t opcode)
{
    switch (opcode) {
    case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET:
        return ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK;
    case ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_SET:
        return ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_SET_UNACK;
    case ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET:
        return ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK;
    case ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET:
        return ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET_UNACK;
    case ESP_BLE_MESH_MODEL_OP_LIGHT_HSL_SET:
        return ESP_BLE_MESH_MODEL_OP_LIGHT_HSL_SET_UNACK;
    default:
        return opcode;
    }
}

static void tx_repeat_timer_cb(void *arg)
{
    (void)arg;
    tx_repeat_t due[TX_REPEAT_SLOTS];
    size_t n_due = 0;
    bool pending = false;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_tx_repeat_lock);
    for (size_t i = 0; i < TX_REPEAT_SLOTS; i++) {
        tx_repeat_t *r = &s_tx_repeat[i];
        if (!r->used) {
            continue;
        }
        if (now >= r->due_us) {
            due[n_due++] = *r;
            if (--r->left == 0) {
                r->used = false;
                continue;
            }
            r->due_us = now + TX_REPEAT_INTERVAL_US;
        }
        pending = true;
    }
    if (!pending) {
        (void)esp_timer_stop(s_tx_repeat_timer);
    }
    portEXIT_CRITICAL(&s_tx_repeat_lock);

    for (size_t i = 0; i < n_due; i++) {
        esp_err_t err = due[i].light
                            ? esp_ble_mesh_light_client_set_state(&due[i].common, &due[i].set.light)
                            : esp_ble_mesh_generic_client_set_state(&due[i].common, &due[i].set.gen);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "repeat 0x%04" PRIx32 " -> 0x%04x: %s", due[i].common.opcode,
                     due[i].common.ctx.addr, esp_err_to_name(err));
        }
    }
}

static void tx_repeat_queue(const esp_ble_mesh_client_common_param_t *common, const void *set,
                            bool light)
{
    if (!s_tx_repeat_timer) {
        const esp_timer_create_args_t args = {
            .callback = &tx_repeat_timer_cb,
            .name = "bm_tx_repeat",
        };
        if (esp_timer_create(&args, &s_tx_repeat_timer) != ESP_OK) {
            s_tx_repeat_timer = NULL;
            return;
        }
    }

    tx_repeat_t r = {
        .used = true,
        .light = light,
        .left = TX_REPEAT_COUNT,
        .due_us = esp_timer_get_time() + TX_REPEAT_INTERVAL_US,
        .common = *common,
    };
    r.common.opcode = tx_unack_opcode(common->opcode);
    if (light) {
        r.set.light = *(const esp_ble_mesh_light_client_set_state_t *)set;
    } else {
        r.set.gen = *(const esp_ble_mesh_generic_client_set_state_t *)set;
    }

    portENTER_CRITICAL(&s_tx_repeat_lock);
    size_t slot = TX_REPEAT_SLOTS;
    size_t soonest = 0;
    for (size_t i = 0; i < TX_REPEAT_SLOTS; i++) {
        tx_repeat_t *cur = &s_tx_repeat[i];
        if (cur->used && cur->common.ctx.addr == r.common.ctx.addr &&
            cur->common.opcode == r.common.opcode) {
            slot = i;
            break;
        }
        if (slot == TX_REPEAT_SLOTS && !cur->used) {
            slot = i;
        }
        if (cur->left < s_tx_repeat[soonest].left) {
            soonest = i;
        }
    }
    /* All slots busy with other nodes: drop the one closest to finishing. */
    s_tx_repeat[slot < TX_REPEAT_SLOTS ? slot : soonest] = r;
    if (!esp_timer_is_active(s_tx_repeat_timer)) {
        (void)esp_timer_start_periodic(s_tx_repeat_timer, TX_REPEAT_TICK_US);
    }
    portEXIT_CRITICAL(&s_tx_repeat_lock);
}

static void tx_repeat_cancel_all(void)
{
    if (!s_tx_repeat_timer) {
        return;
    }
    portENTER_CRITICAL(&s_tx_repeat_lock);
    memset(s_tx_repeat, 0, sizeof(s_tx_repeat));
    (void)esp_timer_stop(s_tx_repeat_timer);
    portEXIT_CRITICAL(&s_tx_repeat_lock);
}

bool ble_mesh_stack_rf_critical(void)
{
    return s_running && (s_prov_busy || ble_mesh_stack_permit_provision_active());
}

void ble_mesh_stack_yield_rf(void)
{
    if (!s_running) {
        return;
    }
    /* Full no-op while joining or mid-provision — do not touch coex either. */
    if (ble_mesh_stack_rf_critical()) {
        return;
    }
    (void)prov_network_set(false);
}

static void prov_queue_forget_uuid(const uint8_t uuid[16]);

static void prov_busy_disarm(void)
{
    if (s_prov_busy_timer) {
        (void)esp_timer_stop(s_prov_busy_timer);
    }
}

static void prov_busy_abort(const char *why)
{
    ESP_LOGW(TAG, "abort stuck provision (%s)", why ? why : "?");
    if (s_prov_uuid_valid) {
        prov_queue_forget_uuid(s_prov_uuid);
    }
    s_prov_busy = false;
    s_prov_succeeded = false;
    s_prov_uuid_valid = false;
    s_prov_retry_after_ms = esp_timer_get_time() / 1000 + 8000;
    prov_busy_disarm();
}

static void prov_busy_timer_cb(void *arg)
{
    (void)arg;
    if (s_prov_busy) {
        prov_busy_abort("busy watchdog — no LINK_CLOSE");
    }
}

static esp_err_t ensure_prov_busy_timer(void)
{
    if (s_prov_busy_timer) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback = &prov_busy_timer_cb,
        .name = "bm_prov_busy",
    };
    return esp_timer_create(&args, &s_prov_busy_timer);
}

static void prov_busy_arm(void)
{
    if (ensure_prov_busy_timer() != ESP_OK) {
        return;
    }
    (void)esp_timer_stop(s_prov_busy_timer);
    (void)esp_timer_start_once(s_prov_busy_timer, PROV_BUSY_WATCHDOG_US);
}

static void permit_timer_cb(void *arg)
{
    (void)arg;
    /* PB-GATT may still be in progress when the window expires. */
    if (s_prov_busy && s_permit_timer) {
        if (s_permit_busy_extends < PERMIT_BUSY_EXTEND_MAX) {
            s_permit_busy_extends++;
            ESP_LOGW(TAG, "permit expired but provision busy — extend 30s (%u/%u)",
                     (unsigned)s_permit_busy_extends, (unsigned)PERMIT_BUSY_EXTEND_MAX);
            s_permit_until_ms = esp_timer_get_time() / 1000 + 30000;
            (void)esp_timer_start_once(s_permit_timer, 30 * 1000000ULL);
            return;
        }
        ESP_LOGW(TAG, "permit expired with stuck busy — abort and close window");
        prov_busy_abort("permit expired");
    }
    s_permit_busy_extends = 0;
    s_permit_until_ms = 0;
    /* Close join window and free RF unless a Mesh TX window is still open. */
    if (s_coex_idle_timer) {
        esp_timer_stop(s_coex_idle_timer);
    }
    (void)prov_network_set(false);
    ESP_LOGI(TAG, "permit provision window closed");
}

static void prov_queue_forget_uuid(const uint8_t uuid[16])
{
    if (!uuid) {
        return;
    }
    esp_ble_mesh_device_delete_t del = {0};
    memcpy(del.uuid, uuid, 16);
    del.flag = DEL_DEV_UUID_FLAG;
    esp_err_t err = esp_ble_mesh_provisioner_delete_dev(&del);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "delete unprov queue: %s", esp_err_to_name(err));
    }
}

static esp_err_t ensure_permit_timer(void)
{
    if (s_permit_timer) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback = &permit_timer_cb,
        .name = "bm_permit",
    };
    return esp_timer_create(&args, &s_permit_timer);
}

#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
static esp_err_t try_proxy_connect(node_info_t *node)
{
    if (!node || !node->have_addr) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg_waiting_proxy = true;
    s_cfg_proxy_ready = false;
    ESP_LOGI(TAG,
             "Proxy Client connect %02x:%02x:%02x:%02x:%02x:%02x net=0x%04x for cfg 0x%04x",
             node->bd_addr[0], node->bd_addr[1], node->bd_addr[2], node->bd_addr[3],
             node->bd_addr[4], node->bd_addr[5], s_prov_key.net_idx, node->unicast);
    esp_err_t err = esp_ble_mesh_proxy_client_connect(node->bd_addr, node->addr_type,
                                                     s_prov_key.net_idx);
    if (err != ESP_OK) {
        s_cfg_waiting_proxy = false;
        ESP_LOGW(TAG, "Proxy connect failed: %s (fall back to ADV)", esp_err_to_name(err));
    }
    return err;
}
#endif

static void cfg_timer_cb(void *arg)
{
    (void)arg;
    node_info_t *node = get_node_info(s_cfg_pending_unicast);
    if (!node) {
        ESP_LOGW(TAG, "cfg timer: node 0x%04x missing", s_cfg_pending_unicast);
        return;
    }

    switch (s_cfg_timer_action) {
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
    case CFG_ACT_PROXY_CONNECT:
    case CFG_ACT_PROXY_RECONNECT:
        /* Do not open Proxy while another PB-GATT provision holds the ACL. */
        if (s_prov_busy) {
            ESP_LOGW(TAG, "Proxy deferred — provision busy (cfg 0x%04x)", node->unicast);
            schedule_cfg_timer(CFG_ACT_PROXY_RECONNECT, CFG_PROXY_RECONNECT_DELAY_US);
            return;
        }
        if (try_proxy_connect(node) == ESP_OK) {
            return;
        }
        /* ENOMEM (-12): PB-GATT ACL slot still held — wait, do not fall to ADV yet. */
        if (node->have_addr && s_proxy_reconnects < CFG_PROXY_RECONNECT_MAX) {
            s_proxy_reconnects++;
            ESP_LOGW(TAG, "Proxy connect busy, retry %u/%u in %lld ms",
                     s_proxy_reconnects, CFG_PROXY_RECONNECT_MAX,
                     CFG_PROXY_RECONNECT_DELAY_US / 1000);
            schedule_cfg_timer(CFG_ACT_PROXY_RECONNECT, CFG_PROXY_RECONNECT_DELAY_US);
            return;
        }
        ESP_LOGW(TAG, "Proxy exhausted, Composition Get over ADV -> 0x%04x", node->unicast);
        (void)send_comp_data_get(node);
        break;
    case CFG_ACT_COMP_GET:
        if (!s_cfg_proxy_ready) {
            ESP_LOGW(TAG, "Proxy not ready, skip Comp Get");
            return;
        }
        ESP_LOGI(TAG, "start Composition Data Get (via Proxy) -> 0x%04x", node->unicast);
        (void)send_comp_data_get(node);
        break;
#endif
    default:
        ESP_LOGI(TAG, "start Composition Data Get -> 0x%04x", node->unicast);
        (void)send_comp_data_get(node);
        break;
    }
}

static void schedule_cfg_timer(int action, int64_t delay_us)
{
    s_cfg_timer_action = action;
    if (!s_cfg_delay_timer) {
        cfg_timer_cb(NULL);
        return;
    }
    (void)esp_timer_stop(s_cfg_delay_timer);
    esp_err_t err = esp_timer_start_once(s_cfg_delay_timer, delay_us);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cfg timer start failed: %s", esp_err_to_name(err));
        cfg_timer_cb(NULL);
    }
}

static esp_err_t prov_complete(int node_idx, const esp_ble_mesh_octet16_t uuid,
                               uint16_t unicast, uint8_t elem_num, uint16_t net_idx)
{
    char name[16];
    int err;

    ESP_LOGI(TAG, "prov complete idx=%d unicast=0x%04x elems=%u net=0x%04x",
             node_idx, unicast, elem_num, net_idx);

    snprintf(name, sizeof(name), "NODE-%d", node_idx);
    err = esp_ble_mesh_provisioner_set_node_name(node_idx, name);
    if (err) {
        ESP_LOGW(TAG, "set node name failed: %d", err);
    }

    err = store_node_info(uuid, unicast, elem_num, 0);
    if (err) {
        ESP_LOGE(TAG, "store node failed");
        return ESP_FAIL;
    }

    node_info_t *node = get_node_info(unicast);
    if (!node) {
        return ESP_FAIL;
    }

    /* Do not notify Hub until AppKey + model binds finish — early expose lets
     * RainMaker send OnOff before the node can decrypt AppKey messages. */

    s_cfg_pending_unicast = unicast;
    s_cfg_timeout_retries = 0;
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
    s_proxy_reconnects = 0;
    s_cfg_proxy_ready = false;
#endif
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
    {
        int64_t delay = s_prov_link_was_gatt ? CFG_AFTER_PROV_GATT_DELAY_US
                                             : CFG_AFTER_PROV_DELAY_US;
        ESP_LOGI(TAG, "config after settle in %lld ms (%s)", delay / 1000,
                 s_prov_link_was_gatt ? "post-PB-GATT" : "post-PB-ADV");
        schedule_cfg_timer(CFG_ACT_PROXY_CONNECT, delay);
    }
#else
    ESP_LOGI(TAG, "config after settle in %lld ms", CFG_AFTER_PROV_DELAY_US / 1000);
    schedule_cfg_timer(CFG_ACT_COMP_GET, CFG_AFTER_PROV_DELAY_US);
#endif
    return ESP_OK;
}

static void recv_unprov_adv_pkt(uint8_t dev_uuid[16], uint8_t addr[BD_ADDR_LEN],
                                esp_ble_mesh_addr_type_t addr_type, uint16_t oob_info,
                                uint8_t adv_type, esp_ble_mesh_prov_bearer_t bearer)
{
    (void)adv_type;
    if (!ble_mesh_stack_permit_provision_active()) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    /* A second add_unprov while PB-GATT is live cancels the ACL (HCI 0x16). */
    if (s_prov_busy || now_ms < s_prov_retry_after_ms) {
        return;
    }
    /* Proxy Client / AppKey bind needs the GATT ACL. Starting another PB-GATT
     * (or even PB-ADV) during post-prov config causes slot clash (-ENOMEM) and
     * local-host terminate (0x16 / HCI 19) on the in-flight provision. */
    if (s_cfg_pending_unicast != 0) {
        return;
    }

    if (!uuid_matches_policy(dev_uuid)) {
        ESP_LOGI(TAG, "unprov uuid=%02x%02x%02x%02x... skipped (uuid filter)",
                 dev_uuid[0], dev_uuid[1], dev_uuid[2], dev_uuid[3]);
        return;
    }

    /* Only purge ghosts when idle — never during an active provision attempt. */
    if (esp_ble_mesh_provisioner_get_node_with_uuid(dev_uuid)) {
        ESP_LOGW(TAG, "uuid=%02x%02x... still unprov on air — purge stale stack node",
                 dev_uuid[0], dev_uuid[1]);
        (void)esp_ble_mesh_provisioner_delete_node_with_uuid(dev_uuid);
        for (int i = 0; i < ARRAY_SIZE(s_nodes); i++) {
            if (memcmp(s_nodes[i].uuid, dev_uuid, 16) == 0) {
                memset(&s_nodes[i], 0, sizeof(s_nodes[i]));
                s_nodes[i].unicast = ESP_BLE_MESH_ADDR_UNASSIGNED;
                break;
            }
        }
    }

#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
    memcpy(s_pending_bd_addr, addr, BD_ADDR_LEN);
    s_pending_addr_type = addr_type;
    s_pending_have_addr = true;
#endif

    /* Prefer PB-ADV when both are offered; fall back to PB-GATT (many nodes are GATT-only). */
    esp_ble_mesh_prov_bearer_t use_bearer;
    if (bearer & ESP_BLE_MESH_PROV_ADV) {
        use_bearer = ESP_BLE_MESH_PROV_ADV;
    } else if (bearer & ESP_BLE_MESH_PROV_GATT) {
        use_bearer = ESP_BLE_MESH_PROV_GATT;
    } else {
        return;
    }

    ESP_LOGI(TAG, "unprov uuid=%02x%02x... bearer=%s — start provision",
             dev_uuid[0], dev_uuid[1],
             (use_bearer == ESP_BLE_MESH_PROV_ADV) ? "PB-ADV" : "PB-GATT");

    esp_ble_mesh_unprov_dev_add_t add_dev = {0};
    memcpy(add_dev.addr, addr, BD_ADDR_LEN);
    add_dev.addr_type = (esp_ble_mesh_addr_type_t)addr_type;
    memcpy(add_dev.uuid, dev_uuid, 16);
    add_dev.oob_info = oob_info;
    add_dev.bearer = use_bearer;

    /* Arm busy before the API call so overlapping adv callbacks cannot re-enter.
     * Hold MESH_CONFIG for the whole PB-GATT session (Zigbee must not STANDBY us). */
    s_prov_busy = true;
    s_prov_succeeded = false;
    memcpy(s_prov_uuid, dev_uuid, 16);
    s_prov_uuid_valid = true;
    prov_busy_arm();
    coex_mesh_config();
    int err = esp_ble_mesh_provisioner_add_unprov_dev(
        &add_dev,
        (esp_ble_mesh_dev_add_flag_t)(ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG |
                                     ADD_DEV_FLUSHABLE_DEV_FLAG));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        s_prov_busy = false;
        s_prov_uuid_valid = false;
        prov_busy_disarm();
        ESP_LOGE(TAG, "add unprov failed: %s", esp_err_to_name(err));
    }
}

static void provisioning_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "prov enable comp err=%d", param->provisioner_prov_enable_comp.err_code);
        if (param->provisioner_prov_enable_comp.err_code == 0 ||
            param->provisioner_prov_enable_comp.err_code == -EALREADY) {
            s_prov_scan_on = true;
        }
        net_gate_signal(param->provisioner_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT:
        ESP_LOGI(TAG, "prov disable comp err=%d", param->provisioner_prov_disable_comp.err_code);
        s_prov_scan_on = false;
        net_gate_signal(param->provisioner_prov_disable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT: {
        bool gatt_open =
            (param->provisioner_prov_link_open.bearer == ESP_BLE_MESH_PROV_GATT);
        s_prov_busy = true;
        prov_busy_arm();
        coex_mesh_config();
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
        s_prov_link_was_gatt = gatt_open;
#endif
        /* Belt-and-suspenders: keep LE scan off for the Invite→Data window.
         * IDF NimBLE adapter also skips post-connect discovery for PB-GATT. */
        if (gatt_open) {
            int scan_err = bt_mesh_scan_disable();
            if (scan_err && scan_err != -EALREADY) {
                ESP_LOGW(TAG, "PB-GATT scan hold failed: %d", scan_err);
            } else {
                ESP_LOGI(TAG, "PB-GATT: mesh scan held for provisioning ACL");
            }
        }
        ESP_LOGI(TAG, "prov link open bearer=%s", gatt_open ? "PB-GATT" : "PB-ADV");
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT: {
        uint8_t reason = param->provisioner_prov_link_close.reason;
        bool gatt = (param->provisioner_prov_link_close.bearer == ESP_BLE_MESH_PROV_GATT);
        /* PB-GATT: provisioner close_link() → ble_gap_terminate(REM_USER=0x13);
         * disconnect event then reports HCI 0x16 (local-host) to the app for
         * BOTH success (after PROV_COMPLETE) and failure. Mesh CLOSE_REASON
         * 0/1/2 only appear on PB-ADV. Enable Mesh logs (unset NO_LOG) to see
         * the real BT_ERR (confirm/random/send fail) before terminate. */
        const char *why = (reason == 0x00) ? "success"
                          : (reason == 0x01) ? "timeout"
                          : (reason == 0x02) ? "failed"
                          : (reason == 0x08) ? "conn-timeout"
                          : (reason == 0x13) ? "remote-user-terminated"
                          : (reason == 0x16) ? "local-host-terminated"
                                             : "hci/other";
        bool ok = s_prov_succeeded || (reason == 0x00);
        if (ok) {
            ESP_LOGI(TAG, "prov link close bearer=%s reason=0x%02x (%s)%s",
                     gatt ? "PB-GATT" : "PB-ADV", reason, why,
                     s_prov_succeeded ? " after complete" : "");
        } else {
            ESP_LOGW(TAG, "prov link close bearer=%s reason=0x%02x (%s) — device NOT provisioned",
                     gatt ? "PB-GATT" : "PB-ADV", reason, why);
        }
        /* Resume Mesh/unprov scan after PB-GATT (IDF also resumes on disconnect). */
        if (gatt && ble_mesh_stack_permit_provision_active()) {
            int scan_err = bt_mesh_scan_enable();
            if (scan_err && scan_err != -EALREADY) {
                ESP_LOGW(TAG, "PB-GATT scan resume failed: %d", scan_err);
            }
        }
        s_prov_busy = false;
        s_prov_succeeded = false;
        prov_busy_disarm();
        if (!ok) {
            /* Drop queue entry so stack auto-start on leftover ads cannot thrash
             * reconnect (0x3e) before our cooldown allows a clean retry. */
            if (s_prov_uuid_valid) {
                prov_queue_forget_uuid(s_prov_uuid);
            }
            /* GATT ACL teardown needs longer than ADV before the next connect. */
            s_prov_retry_after_ms =
                esp_timer_get_time() / 1000 + (gatt ? 8000 : 3000);
        }
        s_prov_uuid_valid = false;
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        if (param->provisioner_bind_app_key_to_model_comp.err_code != 0) {
            int berr = param->provisioner_bind_app_key_to_model_comp.err_code;
            ESP_LOGW(TAG, "bind model 0x%04x comp err=%d",
                     param->provisioner_bind_app_key_to_model_comp.model_id, berr);
            if (s_bind_comp_err == 0) {
                s_bind_comp_err = berr;
            }
        } else {
            ESP_LOGI(TAG, "bind model 0x%04x ok",
                     param->provisioner_bind_app_key_to_model_comp.model_id);
        }
        if (s_bind_gate) {
            xSemaphoreGive(s_bind_gate);
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
        recv_unprov_adv_pkt(param->provisioner_recv_unprov_adv_pkt.dev_uuid,
                            param->provisioner_recv_unprov_adv_pkt.addr,
                            param->provisioner_recv_unprov_adv_pkt.addr_type,
                            param->provisioner_recv_unprov_adv_pkt.oob_info,
                            param->provisioner_recv_unprov_adv_pkt.adv_type,
                            param->provisioner_recv_unprov_adv_pkt.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        s_prov_succeeded = true;
        s_prov_uuid_valid = false;
        s_prov_retry_after_ms = 0;
        /* PB-GATT: keep s_prov_busy until LINK_CLOSE so ACL teardown is not
         * overlapped by a new PB-GATT or an early Proxy Client connect.
         * PB-ADV frees the air immediately after complete. */
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
        if (!s_prov_link_was_gatt) {
            s_prov_busy = false;
            prov_busy_disarm();
        }
#else
        s_prov_busy = false;
        prov_busy_disarm();
#endif
        prov_complete(param->provisioner_prov_complete.node_idx,
                      param->provisioner_prov_complete.device_uuid,
                      param->provisioner_prov_complete.unicast_addr,
                      param->provisioner_prov_complete.element_num,
                      param->provisioner_prov_complete.netkey_idx);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        /* Always adopt app_idx from completion (exists path updates index). */
        s_prov_key.app_idx = param->provisioner_add_app_key_comp.app_idx;
        if (param->provisioner_add_app_key_comp.err_code == ESP_OK) {
            ESP_LOGI(TAG, "local AppKey ready idx=%u", (unsigned)s_prov_key.app_idx);
        } else {
            ESP_LOGW(TAG, "add local AppKey comp err=%d idx=%u",
                     param->provisioner_add_app_key_comp.err_code,
                     (unsigned)s_prov_key.app_idx);
        }
        if (!s_appkey_gate) {
            s_appkey_gate = xSemaphoreCreateBinary();
        }
        if (s_appkey_gate) {
            xSemaphoreGive(s_appkey_gate);
        }
        break;
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
    case ESP_BLE_MESH_PROXY_CLIENT_CONNECTED_EVT:
        ESP_LOGI(TAG, "Proxy Client connected handle=%u",
                 param->proxy_client_connected.conn_handle);
        s_cfg_waiting_proxy = false;
        s_cfg_proxy_ready = true;
        if (s_cfg_pending_unicast) {
            ESP_LOGI(TAG, "Composition Data Get in %lld ms (Proxy settle)",
                     CFG_AFTER_PROXY_SETTLE_US / 1000);
            schedule_cfg_timer(CFG_ACT_COMP_GET, CFG_AFTER_PROXY_SETTLE_US);
        }
        break;
    case ESP_BLE_MESH_PROXY_CLIENT_CONNECT_COMP_EVT:
        if (param->proxy_client_connect_comp.err_code != ESP_OK) {
            int perr = param->proxy_client_connect_comp.err_code;
            ESP_LOGW(TAG, "Proxy connect comp err=%d%s", perr,
                     (perr == -ENOMEM) ? " (GATT slots full — wait for PB-GATT teardown)"
                                       : "");
            s_cfg_waiting_proxy = false;
            if (s_cfg_pending_unicast && s_proxy_reconnects < CFG_PROXY_RECONNECT_MAX) {
                s_proxy_reconnects++;
                schedule_cfg_timer(CFG_ACT_PROXY_RECONNECT, CFG_PROXY_RECONNECT_DELAY_US);
            } else if (s_cfg_pending_unicast) {
                node_info_t *node = get_node_info(s_cfg_pending_unicast);
                if (node) {
                    ESP_LOGI(TAG, "start Composition Data Get (ADV) -> 0x%04x", node->unicast);
                    (void)send_comp_data_get(node);
                }
            }
        }
        break;
    case ESP_BLE_MESH_PROXY_CLIENT_DISCONNECTED_EVT:
        ESP_LOGW(TAG, "Proxy Client disconnected reason=0x%02x",
                 param->proxy_client_disconnected.reason);
        s_cfg_waiting_proxy = false;
        s_cfg_proxy_ready = false;
        if (s_cfg_pending_unicast && s_proxy_reconnects < CFG_PROXY_RECONNECT_MAX) {
            s_proxy_reconnects++;
            ESP_LOGI(TAG, "Proxy reconnect %u/%u in %lld ms",
                     s_proxy_reconnects, CFG_PROXY_RECONNECT_MAX,
                     CFG_PROXY_RECONNECT_DELAY_US / 1000);
            schedule_cfg_timer(CFG_ACT_PROXY_RECONNECT, CFG_PROXY_RECONNECT_DELAY_US);
        }
        break;
    case ESP_BLE_MESH_PROXY_CLIENT_RECV_ADV_PKT_EVT:
        if (s_cfg_pending_unicast && !s_cfg_waiting_proxy && !s_cfg_proxy_ready) {
            node_info_t *node = get_node_info(s_cfg_pending_unicast);
            if (node) {
                memcpy(node->bd_addr, param->proxy_client_recv_adv_pkt.addr, BD_ADDR_LEN);
                node->addr_type = param->proxy_client_recv_adv_pkt.addr_type;
                node->have_addr = true;
                if (s_proxy_reconnects < CFG_PROXY_RECONNECT_MAX) {
                    (void)try_proxy_connect(node);
                }
            }
        }
        break;
#endif
    default:
        break;
    }
}

static void config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                             esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (!param || !param->params) {
        return;
    }

    uint32_t opcode = param->params->opcode;
    uint16_t addr = param->params->ctx.addr;
    ESP_LOGI(TAG, "cfg cli evt=0x%02x err=0x%02x addr=0x%04x op=0x%04" PRIx32,
             event, param->error_code, addr, opcode);

    if (param->error_code) {
        ESP_LOGE(TAG, "config client send failed op=0x%04" PRIx32, opcode);
        return;
    }

    node_info_t *node = get_node_info(addr);
    if (!node) {
        ESP_LOGW(TAG, "cfg cli: unknown node 0x%04x", addr);
        return;
    }

    switch (event) {
    case ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT:
        if (opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) {
            s_cfg_timeout_retries = 0;
            struct net_buf_simple *comp =
                param->status_cb.comp_data_status.composition_data;
            if (comp && comp->data && comp->len) {
                ingest_composition(node, comp->data, comp->len);
            } else {
                s_bind_job_count = 0;
                for (size_t i = 0; i < s_policy.bind_model_count; i++) {
                    bind_job_add(node->unicast, s_policy.bind_models[i].model_id);
                }
                /* Unknown caps stay zero: classify decides, and an unclassifiable
                 * node must not masquerade as a controllable switch. */
            }
            ESP_LOGI(TAG, "Composition Data ok, AppKey Add -> 0x%04x", node->unicast);
            (void)send_app_key_add(node);
        }
        break;

    case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
        if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            s_cfg_timeout_retries = 0;
            s_bind_model_idx = 0;
            if (s_bind_job_count == 0) {
                for (size_t i = 0; i < s_policy.bind_model_count; i++) {
                    bind_job_add(node->unicast, s_policy.bind_models[i].model_id);
                }
            }
            ESP_LOGI(TAG, "AppKey Add ok, Model App Bind (%u) -> 0x%04x",
                     (unsigned)s_bind_job_count, node->unicast);
            if (s_bind_job_count == 0) {
                finish_model_binds(node);
            } else {
                (void)send_model_app_bind(node);
            }
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            s_cfg_timeout_retries = 0;
            s_bind_model_idx++;
            if (s_bind_model_idx < s_bind_job_count) {
                (void)send_model_app_bind(node);
            } else {
                finish_model_binds(node);
            }
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
            const esp_ble_mesh_cfg_model_pub_status_cb_t *st = &param->status_cb.model_pub_status;
            s_pub_retries = 0;
            if (st->status == ESP_BLE_MESH_CFG_STATUS_SUCCESS) {
                advance_status_reporting(node);
            } else if (st->status == ESP_BLE_MESH_CFG_STATUS_INVALID_PUBLISH_PARAMETERS &&
                       !s_pub_no_period) {
                /* A model whose publish-update callback is absent rejects a
                 * non-zero period outright; without one it still publishes on
                 * every state change, which is the half that matters. */
                ESP_LOGW(TAG, "pub set model 0x%04x refused the period — retrying without one",
                         st->model_id);
                s_pub_no_period = true;
                (void)send_model_pub_set_at(node, s_pub_model_idx);
            } else {
                ESP_LOGW(TAG, "pub set model 0x%04x elem 0x%04x status 0x%02x — model skipped",
                         st->model_id, st->element_addr, st->status);
                advance_status_reporting(node);
            }
        }
        break;

    case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
        if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
            /* Reporting is optional: a node that stops answering here keeps its
             * provisioning and its binds, unlike the binding path below. */
            if (s_pub_retries >= PUB_TIMEOUT_RETRIES_MAX) {
                ESP_LOGW(TAG,
                         "publication: node 0x%04x stopped answering at model %u/%u"
                         " — reporting left partly configured",
                         node->unicast, (unsigned)(s_pub_model_idx + 1),
                         (unsigned)s_bind_job_count);
                break;
            }
            s_pub_retries++;
            ESP_LOGW(TAG, "pub set timeout retry %u/%u", s_pub_retries, PUB_TIMEOUT_RETRIES_MAX);
            (void)send_model_pub_set_at(node, s_pub_model_idx);
            break;
        }
        if (s_cfg_timeout_retries >= CFG_TIMEOUT_RETRIES_MAX) {
            ESP_LOGE(TAG,
                     "config timeout exhausted op=0x%04" PRIx32
                     " addr=0x%04x — purge incomplete node (re-permit to retry)",
                     opcode, node->unicast);
            s_cfg_pending_unicast = 0;
            (void)esp_ble_mesh_provisioner_delete_node_with_addr(node->unicast);
            memset(node, 0, sizeof(*node));
            node->unicast = ESP_BLE_MESH_ADDR_UNASSIGNED;
            break;
        }
        s_cfg_timeout_retries++;
        ESP_LOGW(TAG, "config timeout op=0x%04" PRIx32 " retry %u/%u",
                 opcode, s_cfg_timeout_retries, CFG_TIMEOUT_RETRIES_MAX);
        if (opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) {
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
            if (!s_cfg_proxy_ready && node->have_addr &&
                s_proxy_reconnects < CFG_PROXY_RECONNECT_MAX) {
                s_proxy_reconnects++;
                schedule_cfg_timer(CFG_ACT_PROXY_RECONNECT, CFG_PROXY_RECONNECT_DELAY_US);
                break;
            }
#endif
            (void)send_comp_data_get(node);
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            (void)send_app_key_add(node);
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            (void)send_model_app_bind(node);
        }
        break;

    default:
        break;
    }
}

static void generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
                              esp_ble_mesh_generic_client_cb_param_t *param)
{
    if (!param || !param->params) {
        return;
    }
    if (event == ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT || param->error_code != 0) {
        const char *why = "see errno";
        if (param->error_code == -EINVAL) {
            /* Stack ready_to_send() or model_has_key() — often "dst not in mesh_nodes". */
            why = esp_ble_mesh_provisioner_get_node_with_addr(param->params->ctx.addr)
                      ? "EINVAL/model-not-bound?"
                      : "EINVAL/dst-unknown-to-stack";
        } else if (param->error_code == -EADDRNOTAVAIL) {
            why = "EADDRNOTAVAIL/no-subnet";
        } else if (param->error_code == -ETIMEDOUT) {
            why = "ETIMEDOUT";
        }
        ESP_LOGW(TAG, "generic client evt=%d err=%d (%s) op=0x%04" PRIx32 " addr=0x%04x",
                 (int)event, param->error_code, why,
                 param->params->opcode, param->params->ctx.addr);
        return;
    }
    node_info_t *node = get_node_info(param->params->ctx.addr);
    if (!node) {
        return;
    }
    uint32_t opcode = param->params->opcode;
    if (!(event == ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT ||
          event == ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT ||
          event == ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT)) {
        return;
    }
    if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET ||
        opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
        opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK ||
        opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
        node->onoff = param->status_cb.onoff_status.present_onoff;
        notify_onoff(node->unicast, node->onoff != 0);
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_GET ||
               opcode == ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_SET ||
               opcode == ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_STATUS) {
        if (s_on_attr) {
            minihub_ble_mesh_attr_evt_t evt = {
                .unicast = node->unicast,
                .kind = MINIHUB_BM_ATTR_LEVEL,
                .u.level = param->status_cb.level_status.present_level,
            };
            s_on_attr(&evt, s_cb_ctx);
        }
    }
}

static void light_client_cb(esp_ble_mesh_light_client_cb_event_t event,
                            esp_ble_mesh_light_client_cb_param_t *param)
{
    if (!param || !param->params || param->error_code || !s_on_attr) {
        return;
    }
    node_info_t *node = get_node_info(param->params->ctx.addr);
    if (!node) {
        return;
    }
    if (!(event == ESP_BLE_MESH_LIGHT_CLIENT_GET_STATE_EVT ||
          event == ESP_BLE_MESH_LIGHT_CLIENT_SET_STATE_EVT ||
          event == ESP_BLE_MESH_LIGHT_CLIENT_PUBLISH_EVT)) {
        return;
    }
    uint32_t opcode = param->params->opcode;
    if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET ||
        opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET ||
        opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
        minihub_ble_mesh_attr_evt_t evt = {
            .unicast = node->unicast,
            .kind = MINIHUB_BM_ATTR_LIGHTNESS,
            .u.lightness = param->status_cb.lightness_status.present_lightness,
        };
        s_on_attr(&evt, s_cb_ctx);
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET ||
               opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET ||
               opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_STATUS) {
        minihub_ble_mesh_attr_evt_t evt = {
            .unicast = node->unicast,
            .kind = MINIHUB_BM_ATTR_CTL,
            .u.ctl.lightness = param->status_cb.ctl_status.present_ctl_lightness,
            .u.ctl.temperature = param->status_cb.ctl_status.present_ctl_temperature,
            .u.ctl.delta_uv = 0,
        };
        s_on_attr(&evt, s_cb_ctx);
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_HSL_GET ||
               opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_HSL_SET ||
               opcode == ESP_BLE_MESH_MODEL_OP_LIGHT_HSL_STATUS) {
        minihub_ble_mesh_attr_evt_t evt = {
            .unicast = node->unicast,
            .kind = MINIHUB_BM_ATTR_HSL,
            .u.hsl.lightness = param->status_cb.hsl_status.hsl_lightness,
            .u.hsl.hue = param->status_cb.hsl_status.hsl_hue,
            .u.hsl.saturation = param->status_cb.hsl_status.hsl_saturation,
        };
        s_on_attr(&evt, s_cb_ctx);
    }
}

static void sensor_client_cb(esp_ble_mesh_sensor_client_cb_event_t event,
                             esp_ble_mesh_sensor_client_cb_param_t *param)
{
    (void)event;
    if (!param || !param->params) {
        return;
    }
    if (param->error_code != 0) {
        ESP_LOGW(TAG, "sensor client err=%d op=0x%04" PRIx32 " addr=0x%04x", param->error_code,
                 param->params->opcode, param->params->ctx.addr);
        return;
    }
    node_info_t *node = get_node_info(param->params->ctx.addr);
    if (!node) {
        return;
    }
    uint32_t opcode = param->params->opcode;
    if (opcode == ESP_BLE_MESH_MODEL_OP_SENSOR_DESCRIPTOR_STATUS) {
        const struct net_buf_simple *desc = param->status_cb.descriptor_status.descriptor;
        uint16_t ids[MINIHUB_BM_MAX_PROPS];
        size_t n = minihub_bm_descriptor_props(desc ? desc->data : NULL,
                                               desc ? desc->len : 0, ids, ARRAY_SIZE(ids));
        node->prop_count = n;
        for (size_t i = 0; i < n; i++) {
            node->props[i] = ids[i];
        }
        ESP_LOGI(TAG, "node 0x%04x: descriptor lists %u propert(ies)", node->unicast,
                 (unsigned)n);
        /* The properties decide the type, so the node is registered now. */
        notify_device_ready(node);
        (void)send_sensor_get(node);
        return;
    }
    if (opcode == ESP_BLE_MESH_MODEL_OP_SENSOR_STATUS) {
        const struct net_buf_simple *data = param->status_cb.sensor_status.marshalled_sensor_data;
        if (!data || !data->data || data->len == 0) {
            return;
        }
        size_t off = 0;
        minihub_bm_mpid_t mpid;
        while (minihub_bm_mpid_next(data->data, data->len, &off, &mpid)) {
            const minihub_bm_sensor_t *row = minihub_bm_sensor_by_property(mpid.property_id);
            if (!row) {
                continue;
            }
            double value = 0;
            if (!minihub_bm_sensor_value(row, mpid.data, mpid.len, &value)) {
                continue;
            }
            if (!s_on_attr) {
                return;
            }
            minihub_ble_mesh_attr_evt_t evt = {
                .unicast = node->unicast,
                .kind = MINIHUB_BM_ATTR_SENSOR,
                .u.sensor.property_id = mpid.property_id,
                .u.sensor.value = value,
            };
            s_on_attr(&evt, s_cb_ctx);
        }
    }
}

esp_err_t ble_mesh_stack_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    /* Re-open after adapters.json disable without full mesh re-init. */
    if (s_mesh_inited) {
        s_running = true;
        s_prov_scan_on = false;
        coex_mesh_standby();
        ESP_LOGI(TAG, "BLE Mesh ready (idle; TX/permit re-enables network)");
        return ESP_OK;
    }

    esp_err_t err = ble_mesh_host_bluetooth_init();
    if (err != ESP_OK) {
        return err;
    }
    memset(s_dev_uuid, 0, sizeof(s_dev_uuid));
    ble_mesh_host_get_dev_uuid(s_dev_uuid);
    /* UUID bytes [2..7] come from BLE addr; all-zero means host addr not ready. */
    bool uuid_empty = true;
    for (size_t i = 2; i < sizeof(s_dev_uuid); i++) {
        if (s_dev_uuid[i] != 0) {
            uuid_empty = false;
            break;
        }
    }
    if (uuid_empty) {
        ESP_LOGE(TAG, "BLE identity address not ready — defer mesh start");
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_nodes, 0, sizeof(s_nodes));
    s_prov_key.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
    s_prov_key.app_idx = APP_KEY_IDX;
    memset(s_prov_key.app_key, APP_KEY_OCTET, sizeof(s_prov_key.app_key));

    esp_ble_mesh_register_prov_callback(provisioning_cb);
    esp_ble_mesh_register_config_client_callback(config_client_cb);
    esp_ble_mesh_register_generic_client_callback(generic_client_cb);
    esp_ble_mesh_register_light_client_callback(light_client_cb);
    esp_ble_mesh_register_sensor_client_callback(sensor_client_cb);

    if (!s_cfg_delay_timer) {
        const esp_timer_create_args_t args = {
            .callback = &cfg_timer_cb,
            .name = "bm_cfg_delay",
        };
        err = esp_timer_create(&args, &s_cfg_delay_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cfg delay timer create failed: %s", esp_err_to_name(err));
            s_cfg_delay_timer = NULL;
        }
    }
    if (ensure_permit_timer() != ESP_OK) {
        ESP_LOGW(TAG, "permit timer create failed");
    }
    if (ensure_coex_idle_timer() != ESP_OK) {
        ESP_LOGW(TAG, "coex idle timer create failed");
    }

    err = esp_ble_mesh_init(&s_provision, &s_composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_mesh_init: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_policy_set) {
        policy_apply_defaults();
        s_policy_set = true;
    }
    /* Stack-level match accepts all; OR filters applied in recv_unprov_adv_pkt. */
    err = esp_ble_mesh_provisioner_set_dev_uuid_match(NULL, 0, 0x0, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uuid match clear: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "uuid policy: accept_all=%d filters=%u",
             (int)s_policy.accept_all_uuid, (unsigned)s_policy.filter_count);

    /* Enable to create/restore NetKey + AppKey, then idle-disable for Zigbee. */
    err = prov_network_set(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prov enable: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_appkey_gate) {
        s_appkey_gate = xSemaphoreCreateBinary();
    }
    if (s_appkey_gate) {
        (void)xSemaphoreTake(s_appkey_gate, 0);
    }
    err = esp_ble_mesh_provisioner_add_local_app_key(s_prov_key.app_key, s_prov_key.net_idx,
                                                     s_prov_key.app_idx);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "add local app key: %s", esp_err_to_name(err));
        (void)prov_network_set(false);
        return err;
    }
    if (s_appkey_gate &&
        xSemaphoreTake(s_appkey_gate, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "add local AppKey completion timeout — bind anyway");
    }
    err = bind_local_client_models();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "initial local model bind: %s", esp_err_to_name(err));
    }
    /* Free RF for Zigbee until Mesh TX / permit_provision. */
    (void)prov_network_set(false);

    s_mesh_inited = true;
    s_running = true;
    if (s_on_ready) {
        s_on_ready(s_cb_ctx);
    }
    ESP_LOGI(TAG, "BLE Mesh Provisioner ready (idle; TX/permit re-enables network)");
    return ESP_OK;
}

esp_err_t ble_mesh_stack_stop(void)
{
    /* Disable provisioner bearers (stops scan/adv) but keep BT + mesh init.
     * Full controller teardown/re-init is unreliable on this target. Hub reload
     * does not call stop on radio hosts; this path is for adapters.json disable. */
    s_permit_until_ms = 0;
    if (s_permit_timer) {
        esp_timer_stop(s_permit_timer);
    }
    if (s_prov_busy) {
        prov_busy_abort("stack stop");
    }
    if (!s_running) {
        return ESP_OK;
    }
    if (s_cfg_delay_timer) {
        esp_timer_stop(s_cfg_delay_timer);
    }
    tx_repeat_cancel_all();
    (void)prov_network_set(false);
    ESP_LOGI(TAG, "provisioner stopped (BT controller kept)");
    s_running = false;
    return ESP_OK;
}

#if CONFIG_MINIHUB_ZIGBEE_SDK
bool zigbee_stack_permit_join_active(void);
#endif

esp_err_t ble_mesh_stack_permit_provision(int seconds)
{
    if (seconds < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Close permit window (idempotent). Abort a stuck busy so UI "stop join"
     * cannot leave s_prov_busy wedged after a failed PB-GATT connect. */
    if (seconds == 0) {
        if (s_permit_timer) {
            (void)esp_timer_stop(s_permit_timer);
        }
        s_permit_until_ms = 0;
        s_permit_busy_extends = 0;
        if (s_prov_busy) {
            prov_busy_abort("manual close");
        }
        if (s_coex_idle_timer) {
            esp_timer_stop(s_coex_idle_timer);
        }
        (void)prov_network_set(false);
        ESP_LOGI(TAG, "permit provision window closed (manual)");
        return ESP_OK;
    }

    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ble_mesh_stack_rf_critical()) {
        ESP_LOGW(TAG, "permit_provision rejected: already provisioning");
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_MINIHUB_ZIGBEE_SDK
    if (zigbee_stack_permit_join_active()) {
        ESP_LOGW(TAG, "permit_provision rejected: Zigbee permit-join active");
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (ensure_permit_timer() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = prov_network_set(true);
    if (err != ESP_OK) {
        return err;
    }
    coex_mesh_config();

    s_permit_until_ms = esp_timer_get_time() / 1000 + (int64_t)seconds * 1000;
    s_permit_busy_extends = 0;
    (void)esp_timer_stop(s_permit_timer);
    err = esp_timer_start_once(s_permit_timer, (uint64_t)seconds * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "permit timer start failed: %s", esp_err_to_name(err));
        s_permit_until_ms = 0;
        return err;
    }
    ESP_LOGI(TAG, "permit provision %d s (accept_all=%d filters=%u)",
             seconds, (int)s_policy.accept_all_uuid, (unsigned)s_policy.filter_count);
    return ESP_OK;
}

bool ble_mesh_stack_permit_provision_active(void)
{
    return s_permit_until_ms > (esp_timer_get_time() / 1000);
}

bool ble_mesh_stack_running(void)
{
    return s_running;
}

esp_err_t ble_mesh_stack_onoff(uint16_t unicast, minihub_ble_mesh_onoff_cmd_t cmd)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    node_info_t *node = get_node_info(unicast);
    if (!node) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t onoff = node->onoff;
    if (cmd == MINIHUB_BM_ONOFF_ON) {
        onoff = 1;
    } else if (cmd == MINIHUB_BM_ONOFF_OFF) {
        onoff = 0;
    } else {
        onoff = !onoff;
    }

    esp_err_t err = mesh_prepare_tx();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mesh prepare tx: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_onoff_client.model) {
        ESP_LOGE(TAG, "OnOff client model not ready");
        return ESP_ERR_INVALID_STATE;
    }
    /* App cache alone is not enough: stack ready_to_send() requires mesh_nodes[]. */
    if (!esp_ble_mesh_provisioner_get_node_with_addr(unicast)) {
        ESP_LOGE(TAG,
                 "OnOff 0x%04x: not in BLE Mesh stack node table "
                 "(re-provision after enabling CONFIG_BLE_MESH_SETTINGS)",
                 unicast);
        return ESP_ERR_NOT_FOUND;
    }

    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_generic_client_set_state_t set_state = {0};
    /* UNACK: under Zigbee coex, Status often times out even when Set is delivered. */
    set_msg_common(&common, node, s_onoff_client.model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK);
    set_state.onoff_set.op_en = false;
    set_state.onoff_set.onoff = onoff;
    set_state.onoff_set.tid = ++s_tid;
    ESP_LOGI(TAG, "OnOff SetUnack 0x%04x -> %u (app_idx=%u net=%u)", unicast, (unsigned)onoff,
             (unsigned)s_prov_key.app_idx, (unsigned)s_prov_key.net_idx);
    err = esp_ble_mesh_generic_client_set_state(&common, &set_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OnOff SetUnack failed: %s", esp_err_to_name(err));
        return err;
    }
    tx_repeat_queue(&common, &set_state, false);
    node->onoff = onoff;
    notify_onoff(unicast, onoff != 0);
    return ESP_OK;
}

esp_err_t ble_mesh_stack_level(uint16_t unicast, int16_t level)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    node_info_t *node = get_node_info(unicast);
    if (!node) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = mesh_prepare_tx();
    if (err != ESP_OK) {
        return err;
    }
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_generic_client_set_state_t set_state = {0};
    set_msg_common(&common, node, s_level_client.model, ESP_BLE_MESH_MODEL_OP_GEN_LEVEL_SET);
    set_state.level_set.op_en = false;
    set_state.level_set.level = level;
    set_state.level_set.tid = ++s_tid;
    err = esp_ble_mesh_generic_client_set_state(&common, &set_state);
    if (err == ESP_OK) {
        tx_repeat_queue(&common, &set_state, false);
    }
    return err;
}

esp_err_t ble_mesh_stack_lightness(uint16_t unicast, uint16_t lightness)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    node_info_t *node = get_node_info(unicast);
    if (!node) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = mesh_prepare_tx();
    if (err != ESP_OK) {
        return err;
    }
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_light_client_set_state_t set_state = {0};
    set_msg_common(&common, node, s_lightness_client.model, ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET);
    set_state.lightness_set.op_en = false;
    set_state.lightness_set.lightness = lightness;
    set_state.lightness_set.tid = ++s_tid;
    err = esp_ble_mesh_light_client_set_state(&common, &set_state);
    if (err == ESP_OK) {
        tx_repeat_queue(&common, &set_state, true);
    }
    return err;
}

esp_err_t ble_mesh_stack_ctl(uint16_t unicast, uint16_t lightness, uint16_t temperature,
                             int16_t delta_uv)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    node_info_t *node = get_node_info(unicast);
    if (!node) {
        return ESP_ERR_NOT_FOUND;
    }
    if (temperature < 800) {
        temperature = 800;
    } else if (temperature > 20000) {
        temperature = 20000;
    }
    esp_err_t err = mesh_prepare_tx();
    if (err != ESP_OK) {
        return err;
    }
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_light_client_set_state_t set_state = {0};
    set_msg_common(&common, node, s_ctl_client.model, ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET);
    set_state.ctl_set.op_en = false;
    set_state.ctl_set.ctl_lightness = lightness;
    set_state.ctl_set.ctl_temperature = temperature;
    set_state.ctl_set.ctl_delta_uv = delta_uv;
    set_state.ctl_set.tid = ++s_tid;
    err = esp_ble_mesh_light_client_set_state(&common, &set_state);
    if (err == ESP_OK) {
        tx_repeat_queue(&common, &set_state, true);
    }
    return err;
}

esp_err_t ble_mesh_stack_hsl(uint16_t unicast, uint16_t hue, uint16_t saturation, uint16_t lightness)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    node_info_t *node = get_node_info(unicast);
    if (!node) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = mesh_prepare_tx();
    if (err != ESP_OK) {
        return err;
    }
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_light_client_set_state_t set_state = {0};
    set_msg_common(&common, node, s_hsl_client.model, ESP_BLE_MESH_MODEL_OP_LIGHT_HSL_SET);
    set_state.hsl_set.op_en = false;
    set_state.hsl_set.hsl_lightness = lightness;
    set_state.hsl_set.hsl_hue = hue;
    set_state.hsl_set.hsl_saturation = saturation;
    set_state.hsl_set.tid = ++s_tid;
    err = esp_ble_mesh_light_client_set_state(&common, &set_state);
    if (err == ESP_OK) {
        tx_repeat_queue(&common, &set_state, true);
    }
    return err;
}

esp_err_t ble_mesh_stack_delete_node(uint16_t unicast)
{
    if (!ESP_BLE_MESH_ADDR_IS_UNICAST(unicast)) {
        return ESP_ERR_INVALID_ARG;
    }
    node_info_t *node = get_node_info(unicast);
    esp_err_t err = ESP_OK;
    if (s_running) {
        err = esp_ble_mesh_provisioner_delete_node_with_addr(unicast);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete_node 0x%04x: %s", unicast, esp_err_to_name(err));
        }
    }
    if (node) {
        memset(node, 0, sizeof(*node));
        node->unicast = ESP_BLE_MESH_ADDR_UNASSIGNED;
    }
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t ble_mesh_stack_restore_node(const minihub_ble_mesh_device_t *dev)
{
    if (!dev || !ESP_BLE_MESH_ADDR_IS_UNICAST(dev->unicast)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t uuid_buf[16] = {0};
    if (dev->uuid_hex[0] && minihub_bm_hex_to_uuid(dev->uuid_hex, uuid_buf) != 0) {
        ESP_LOGW(TAG, "restore 0x%04x: bad uuid, continuing by unicast", dev->unicast);
        memset(uuid_buf, 0, sizeof(uuid_buf));
    }
    uint8_t elem_num = dev->elem_num ? dev->elem_num : 1;
    node_info_t *node = get_node_info(dev->unicast);
    if (!node) {
        esp_err_t err = store_node_info(uuid_buf, dev->unicast, elem_num, 0);
        if (err != ESP_OK) {
            return err;
        }
        node = get_node_info(dev->unicast);
        if (!node) {
            return ESP_FAIL;
        }
    } else {
        node->elem_num = elem_num;
        memcpy(node->uuid, uuid_buf, 16);
    }
    node->caps = dev->caps;
    node->cid = dev->cid;
    node->pid = dev->pid;
    node->vid = dev->vid;
    node->prop_count = 0;
    for (size_t i = 0; i < dev->prop_count && i < MINIHUB_BM_MAX_PROPS; i++) {
        node->props[i] = dev->props[i];
        node->prop_count++;
    }
    node->bind_count = 0;
    for (size_t i = 0; i < dev->bind_count && i < MINIHUB_BM_BIND_MODEL_MAX; i++) {
        node->binds[i] = dev->binds[i];
        node->bind_count++;
    }
    if (node->bind_count > 0 || node->prop_count > 0) {
        read_back_bound_state(node, node->binds, node->bind_count);
    }
    return ESP_OK;
}

#else /* !CONFIG_MINIHUB_BLE_MESH_SDK */

esp_err_t ble_mesh_stack_set_provision_policy(const minihub_bm_provision_policy_t *policy)
{
    (void)policy;
    return ESP_OK;
}

esp_err_t ble_mesh_stack_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ble_mesh_stack_stop(void)
{
    s_running = false;
    s_permit_until_ms = 0;
    return ESP_OK;
}
esp_err_t ble_mesh_stack_permit_provision(int seconds)
{
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
}
bool ble_mesh_stack_permit_provision_active(void)
{
    return false;
}
bool ble_mesh_stack_running(void)
{
    return false;
}
esp_err_t ble_mesh_stack_onoff(uint16_t unicast, minihub_ble_mesh_onoff_cmd_t cmd)
{
    (void)unicast;
    (void)cmd;
    return ESP_ERR_NOT_SUPPORTED;
}
void ble_mesh_stack_yield_rf(void) {}
bool ble_mesh_stack_rf_critical(void) { return false; }
esp_err_t ble_mesh_stack_level(uint16_t unicast, int16_t level)
{
    (void)unicast;
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ble_mesh_stack_lightness(uint16_t unicast, uint16_t lightness)
{
    (void)unicast;
    (void)lightness;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ble_mesh_stack_hsl(uint16_t unicast, uint16_t hue, uint16_t saturation, uint16_t lightness)
{
    (void)unicast;
    (void)hue;
    (void)saturation;
    (void)lightness;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_mesh_stack_ctl(uint16_t unicast, uint16_t lightness, uint16_t temperature,
                             int16_t delta_uv)
{
    (void)unicast;
    (void)lightness;
    (void)temperature;
    (void)delta_uv;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_mesh_stack_delete_node(uint16_t unicast)
{
    (void)unicast;
    return ESP_OK;
}

esp_err_t ble_mesh_stack_restore_node(const minihub_ble_mesh_device_t *dev)
{
    (void)dev;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_MINIHUB_BLE_MESH_SDK */

void ble_mesh_stack_set_hooks(minihub_ble_mesh_device_cb_t on_device,
                              minihub_ble_mesh_attr_cb_t on_attr,
                              ble_mesh_stack_ready_cb_t on_ready,
                              void *ctx)
{
    s_on_device = on_device;
    s_on_attr = on_attr;
    s_on_ready = on_ready;
    s_cb_ctx = ctx;
}
