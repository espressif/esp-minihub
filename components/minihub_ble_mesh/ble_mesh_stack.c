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
static SemaphoreHandle_t s_appkey_gate;
static esp_timer_handle_t s_permit_timer;
/** Return BLE Mesh coex scheme to STANDBY after a short TRAFFIC window. */
static esp_timer_handle_t s_coex_idle_timer;
#endif

#if CONFIG_MINIHUB_BLE_MESH_SDK

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_lighting_model_api.h"
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
#include "esp_ble_mesh_proxy_api.h"
#endif

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

typedef struct {
    uint8_t uuid[16];
    uint8_t bd_addr[BD_ADDR_LEN];
    esp_ble_mesh_addr_type_t addr_type;
    bool have_addr;
    uint16_t unicast;
    uint8_t elem_num;
    uint8_t onoff;
    uint8_t caps;
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
static bind_job_t s_bind_jobs[MINIHUB_BM_BIND_MODEL_MAX];
static size_t s_bind_job_count;
static minihub_bm_provision_policy_t s_policy;
static bool s_policy_set;
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

static void policy_apply_defaults(void)
{
    memset(&s_policy, 0, sizeof(s_policy));
    /* Open until packs / adapters.json inject uuid_filters. */
    s_policy.filter_count = 0;
    s_policy.accept_all_uuid = true;
    static const uint16_t k_default_binds[] = {
        ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV,
        ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_SRV,
        ESP_BLE_MESH_MODEL_ID_LIGHT_LIGHTNESS_SRV,
        ESP_BLE_MESH_MODEL_ID_LIGHT_CTL_SRV,
    };
    for (size_t i = 0; i < ARRAY_SIZE(k_default_binds); i++) {
        s_policy.bind_models[i].model_id = k_default_binds[i];
        s_policy.bind_models[i].company_id = MINIHUB_BM_CID_SIG;
    }
    s_policy.bind_model_count = ARRAY_SIZE(k_default_binds);
}

esp_err_t ble_mesh_stack_set_provision_policy(const minihub_bm_provision_policy_t *policy)
{
    if (!policy) {
        return ESP_ERR_INVALID_ARG;
    }
    s_policy = *policy;
    if (s_policy.bind_model_count == 0) {
        static const uint16_t k_default_binds[] = {
            ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV,
            ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_SRV,
            ESP_BLE_MESH_MODEL_ID_LIGHT_LIGHTNESS_SRV,
            ESP_BLE_MESH_MODEL_ID_LIGHT_CTL_SRV,
        };
        for (size_t i = 0; i < ARRAY_SIZE(k_default_binds); i++) {
            s_policy.bind_models[i].model_id = k_default_binds[i];
            s_policy.bind_models[i].company_id = MINIHUB_BM_CID_SIG;
        }
        s_policy.bind_model_count = ARRAY_SIZE(k_default_binds);
    }
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
};

static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, ESP_BLE_MESH_MODEL_NONE),
};

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
    uint8_t caps = node->caps ? node->caps : MINIHUB_BM_CAP_ONOFF;
    minihub_ble_mesh_device_t dev = {0};
    const char *type = minihub_bm_classify_type(caps);
    strncpy(dev.type, type, sizeof(dev.type) - 1);
    (void)minihub_bm_format_point_id(dev.point_id, sizeof(dev.point_id), type, node->unicast);
    (void)minihub_bm_uuid_to_hex(node->uuid, dev.uuid_hex, sizeof(dev.uuid_hex));
    dev.unicast = node->unicast;
    dev.elem_num = node->elem_num;
    dev.caps = caps;
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

static uint8_t model_id_to_cap(uint16_t model_id)
{
    switch (model_id) {
    case ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV:
        return MINIHUB_BM_CAP_ONOFF;
    case ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_SRV:
        return MINIHUB_BM_CAP_LEVEL;
    case ESP_BLE_MESH_MODEL_ID_LIGHT_LIGHTNESS_SRV:
        return MINIHUB_BM_CAP_LIGHTNESS;
    case ESP_BLE_MESH_MODEL_ID_LIGHT_CTL_SRV:
        return (uint8_t)(MINIHUB_BM_CAP_CTL | MINIHUB_BM_CAP_LIGHTNESS);
    default:
        return 0;
    }
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
    ESP_LOGI(TAG, "composition 0x%04x caps=0x%02x bind_jobs=%u",
             node->unicast, node->caps, (unsigned)s_bind_job_count);
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

static void finish_model_binds(node_info_t *node)
{
    s_cfg_pending_unicast = 0;
    ESP_LOGI(TAG, "model binds complete on 0x%04x (%u jobs) caps=0x%02x",
             node->unicast, (unsigned)s_bind_job_count, node->caps);
    notify_device_ready(node);

    bool bound_onoff = false;
    for (size_t i = 0; i < s_bind_job_count; i++) {
        if (s_bind_jobs[i].model_id == ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV) {
            bound_onoff = true;
            break;
        }
    }
    if (!bound_onoff) {
        return;
    }
    ESP_LOGI(TAG, "OnOff Server bound on 0x%04x", node->unicast);
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_generic_client_get_state_t get_state = {0};
    set_msg_common(&common, node, s_onoff_client.model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET);
    int err = esp_ble_mesh_generic_client_get_state(&common, &get_state);
    if (err) {
        ESP_LOGW(TAG, "OnOff Get failed: %d", err);
    }
}

static void schedule_cfg_timer(int action, int64_t delay_us);

/** Bind AppKey to local client models — required after every boot for OnOff Set. */
static esp_err_t bind_local_client_models(void)
{
    static const uint16_t k_local_binds[] = {
        ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI,
        ESP_BLE_MESH_MODEL_ID_GEN_LEVEL_CLI,
        ESP_BLE_MESH_MODEL_ID_LIGHT_LIGHTNESS_CLI,
        ESP_BLE_MESH_MODEL_ID_LIGHT_CTL_CLI,
    };
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
    for (size_t i = 0; i < ARRAY_SIZE(k_local_binds); i++) {
        esp_err_t err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
            PROV_OWN_ADDR, s_prov_key.app_idx, k_local_binds[i], ESP_BLE_MESH_CID_NVAL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bind local model 0x%04x queue: %s", k_local_binds[i],
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
    err = bind_local_client_models();
    if (err != ESP_OK) {
        return err;
    }
    coex_mesh_traffic();
    return ESP_OK;
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

static void permit_timer_cb(void *arg)
{
    (void)arg;
    /* PB-GATT may still be in progress when the window expires. */
    if (s_prov_busy && s_permit_timer) {
        ESP_LOGW(TAG, "permit expired but provision busy — extend 30s");
        s_permit_until_ms = esp_timer_get_time() / 1000 + 30000;
        (void)esp_timer_start_once(s_permit_timer, 30 * 1000000ULL);
        return;
    }
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

    if (!uuid_matches_policy(dev_uuid)) {
        ESP_LOGI(TAG, "unprov uuid=%02x%02x%02x%02x... skipped (uuid filter)",
                 dev_uuid[0], dev_uuid[1], dev_uuid[2], dev_uuid[3]);
        return;
    }

    /* Only purge ghosts when idle — never during an active provision attempt. */
    if (esp_ble_mesh_provisioner_get_node_with_uuid(dev_uuid)) {
        if (s_cfg_pending_unicast != 0) {
            return; /* post-prov config in flight */
        }
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
    coex_mesh_config();
    int err = esp_ble_mesh_provisioner_add_unprov_dev(
        &add_dev,
        (esp_ble_mesh_dev_add_flag_t)(ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG |
                                     ADD_DEV_FLUSHABLE_DEV_FLAG));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        s_prov_busy = false;
        s_prov_uuid_valid = false;
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
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        s_prov_busy = true;
        coex_mesh_config();
#if CONFIG_BLE_MESH_GATT_PROXY_CLIENT
        s_prov_link_was_gatt =
            (param->provisioner_prov_link_open.bearer == ESP_BLE_MESH_PROV_GATT);
#endif
        ESP_LOGI(TAG, "prov link open bearer=%s",
                 (param->provisioner_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV)
                     ? "PB-ADV"
                     : "PB-GATT");
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT: {
        uint8_t reason = param->provisioner_prov_link_close.reason;
        bool gatt = (param->provisioner_prov_link_close.bearer == ESP_BLE_MESH_PROV_GATT);
        /* PB-GATT: close_link() always disconnects locally → HCI 0x16 for both
         * success (after PROV_COMPLETE) and failure. Mesh CLOSE_REASON 0/1/2
         * only appear on PB-ADV. */
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
        s_prov_busy = false;
        s_prov_succeeded = false;
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
        s_prov_busy = false;
        s_prov_succeeded = true;
        s_prov_uuid_valid = false;
        s_prov_retry_after_ms = 0;
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
                if (!node->caps) {
                    node->caps = MINIHUB_BM_CAP_ONOFF;
                }
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
        }
        break;

    case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
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
    if (!s_running) {
        return ESP_OK;
    }
    if (s_cfg_delay_timer) {
        esp_timer_stop(s_cfg_delay_timer);
    }
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

    /* Close permit window (idempotent). Do not abort PB-GATT. */
    if (seconds == 0) {
        if (s_permit_timer) {
            (void)esp_timer_stop(s_permit_timer);
        }
        s_permit_until_ms = 0;
        if (!s_prov_busy) {
            if (s_coex_idle_timer) {
                esp_timer_stop(s_coex_idle_timer);
            }
            (void)prov_network_set(false);
        }
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
    return esp_ble_mesh_generic_client_set_state(&common, &set_state);
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
    return esp_ble_mesh_light_client_set_state(&common, &set_state);
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
    return esp_ble_mesh_light_client_set_state(&common, &set_state);
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

esp_err_t ble_mesh_stack_restore_node(const uint8_t uuid[16], uint16_t unicast, uint8_t elem_num,
                                      uint8_t caps)
{
    if (!ESP_BLE_MESH_ADDR_IS_UNICAST(unicast)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t uuid_buf[16] = {0};
    if (uuid) {
        memcpy(uuid_buf, uuid, 16);
    }
    if (elem_num == 0) {
        elem_num = 1;
    }
    node_info_t *existing = get_node_info(unicast);
    if (existing) {
        existing->elem_num = elem_num;
        existing->caps = caps ? caps : MINIHUB_BM_CAP_ONOFF;
        if (uuid) {
            memcpy(existing->uuid, uuid_buf, 16);
        }
        return ESP_OK;
    }
    esp_err_t err = store_node_info(uuid_buf, unicast, elem_num, 0);
    if (err != ESP_OK) {
        return err;
    }
    node_info_t *node = get_node_info(unicast);
    if (node) {
        node->caps = caps ? caps : MINIHUB_BM_CAP_ONOFF;
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

esp_err_t ble_mesh_stack_restore_node(const uint8_t uuid[16], uint16_t unicast, uint8_t elem_num,
                                      uint8_t caps)
{
    (void)uuid;
    (void)unicast;
    (void)elem_num;
    (void)caps;
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
