/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_matter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "esp_log.h"
#include "minihub_provision.h"
#if CONFIG_MINIHUB_THREAD_ADAPTER
#include "minihub_thread.h"
#endif
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_utils.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/AttributePathParams.h>
#include <app/EventPathParams.h>
#include <app/data-model/Decode.h>
#include <lib/support/Span.h>
#include <lib/core/TLV.h>
#include <controller/CHIPDeviceController.h>
#include <controller/CommissioningDelegate.h>
#include <controller/SetUpCodePairer.h>
#include <lib/address_resolve/AddressResolve.h>
#include <lib/dnssd/Advertiser.h>
#include <lib/dnssd/Resolver.h>
#include <platform/CHIPDeviceLayer.h>
#if CONFIG_ENABLE_MATTER_OVER_THREAD
#include <platform/ThreadStackManager.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include "esp_ot_config.h"
#include "esp_openthread.h"
#include "esp_openthread_types.h"
#endif

static const char *TAG = "matter_ctrl";

static constexpr uint64_t k_controller_node_id = 0x1ULL;
static constexpr uint64_t k_controller_fabric_id = 1;
static constexpr uint16_t k_controller_port = 5580;

static constexpr uint32_t k_onoff_cluster = chip::app::Clusters::OnOff::Id;
static constexpr uint32_t k_onoff_on_cmd = chip::app::Clusters::OnOff::Commands::On::Id;
static constexpr uint32_t k_onoff_off_cmd = chip::app::Clusters::OnOff::Commands::Off::Id;
static constexpr uint32_t k_onoff_attr = chip::app::Clusters::OnOff::Attributes::OnOff::Id;
static constexpr uint32_t k_basic_cluster = chip::app::Clusters::BasicInformation::Id;

static char s_last_error[128];
static bool s_started;
static bool s_commissioned;
static bool s_pairing_in_progress;

typedef struct {
    uint64_t node_id;
    uint16_t endpoint_id;
    bool on;
    bool valid;
} matter_read_slot_t;

static matter_read_slot_t s_last_read;

#define IDENTITY_SLOT_MAX 16

typedef struct {
    uint64_t node_id;
    minihub_matter_identity_t id;
    char sw_num[16];
    char hw_num[16];
    bool in_use;
} identity_slot_t;

static identity_slot_t s_id_slots[IDENTITY_SLOT_MAX];
static minihub_matter_identity_cb_t s_identity_cb;
static void *s_identity_ctx;

static void set_error(const char *msg)
{
    if (!msg) {
        s_last_error[0] = '\0';
        return;
    }
    strncpy(s_last_error, msg, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
}

static identity_slot_t *identity_slot(uint64_t node_id, bool create)
{
    identity_slot_t *empty = NULL;
    for (size_t i = 0; i < IDENTITY_SLOT_MAX; i++) {
        if (s_id_slots[i].in_use && s_id_slots[i].node_id == node_id) {
            return &s_id_slots[i];
        }
        if (!s_id_slots[i].in_use && !empty) {
            empty = &s_id_slots[i];
        }
    }
    if (!create || !empty) {
        return NULL;
    }
    memset(empty, 0, sizeof(*empty));
    empty->node_id = node_id;
    empty->in_use = true;
    return empty;
}

static bool tlv_copy_string(chip::TLV::TLVReader *data, char *out, size_t out_len)
{
    if (!data || !out || out_len == 0) {
        return false;
    }
    chip::CharSpan span;
    CHIP_ERROR err = chip::app::DataModel::Decode(*data, span);
    if (err != CHIP_NO_ERROR) {
        err = data->Get(span);
    }
    if (err != CHIP_NO_ERROR || span.size() == 0) {
        return false;
    }
    size_t n = span.size() < out_len - 1 ? span.size() : out_len - 1;
    memcpy(out, span.data(), n);
    out[n] = '\0';
    return true;
}

static void on_identity_attr(uint64_t node_id, const chip::app::ConcreteDataAttributePath &path,
                             chip::TLV::TLVReader *data, const chip::app::StatusIB &status)
{
    if (status.ToChipError() != CHIP_NO_ERROR || !data) {
        return;
    }
    if (path.mClusterId != k_basic_cluster) {
        return;
    }
    identity_slot_t *slot = identity_slot(node_id, true);
    if (!slot) {
        return;
    }
    using namespace chip::app::Clusters::BasicInformation::Attributes;
    if (path.mAttributeId == VendorName::Id) {
        (void)tlv_copy_string(data, slot->id.manufacturer, sizeof(slot->id.manufacturer));
    } else if (path.mAttributeId == ProductName::Id) {
        (void)tlv_copy_string(data, slot->id.model, sizeof(slot->id.model));
    } else if (path.mAttributeId == SoftwareVersionString::Id) {
        (void)tlv_copy_string(data, slot->id.sw_version, sizeof(slot->id.sw_version));
    } else if (path.mAttributeId == HardwareVersionString::Id) {
        (void)tlv_copy_string(data, slot->id.hw_version, sizeof(slot->id.hw_version));
    } else if (path.mAttributeId == SoftwareVersion::Id) {
        uint32_t v = 0;
        if (chip::app::DataModel::Decode(*data, v) != CHIP_NO_ERROR) {
            (void)data->Get(v);
        }
        if (v || data) {
            snprintf(slot->sw_num, sizeof(slot->sw_num), "%" PRIu32, v);
        }
    } else if (path.mAttributeId == HardwareVersion::Id) {
        uint16_t v = 0;
        if (chip::app::DataModel::Decode(*data, v) != CHIP_NO_ERROR) {
            uint32_t w = 0;
            if (data->Get(w) == CHIP_NO_ERROR) {
                v = (uint16_t)w;
            }
        }
        snprintf(slot->hw_num, sizeof(slot->hw_num), "%u", (unsigned)v);
    }
}

static void on_identity_done(uint64_t node_id,
                             const chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &attr_paths,
                             const chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> &event_paths)
{
    (void)attr_paths;
    (void)event_paths;
    identity_slot_t *slot = identity_slot(node_id, false);
    if (!slot) {
        return;
    }
    if (!slot->id.sw_version[0] && slot->sw_num[0]) {
        strncpy(slot->id.sw_version, slot->sw_num, sizeof(slot->id.sw_version) - 1);
    }
    if (!slot->id.hw_version[0] && slot->hw_num[0]) {
        strncpy(slot->id.hw_version, slot->hw_num, sizeof(slot->id.hw_version) - 1);
    }
    ESP_LOGI(TAG, "Basic Information node=%" PRIu64 " vendor=%s model=%s sw=%s hw=%s", node_id,
             slot->id.manufacturer[0] ? slot->id.manufacturer : "-",
             slot->id.model[0] ? slot->id.model : "-",
             slot->id.sw_version[0] ? slot->id.sw_version : "-",
             slot->id.hw_version[0] ? slot->id.hw_version : "-");
    if (s_identity_cb) {
        s_identity_cb(node_id, &slot->id, s_identity_ctx);
    }
}

static void on_pairing_complete(CHIP_ERROR err)
{
    if (err != CHIP_NO_ERROR) {
        set_error("PASE session failed");
        s_pairing_in_progress = false;
    }
}

static void on_commissioning_success(chip::ScopedNodeId peer_id)
{
    ESP_LOGI(TAG, "Commissioned node 0x%016" PRIx64, peer_id.GetNodeId());
    s_commissioned = true;
    s_pairing_in_progress = false;
    set_error(NULL);
    (void)minihub_matter_interview_identity(peer_id.GetNodeId());
}

static void on_commissioning_failure(chip::ScopedNodeId peer_id, CHIP_ERROR error,
                                     chip::Controller::CommissioningStage stage,
                                     std::optional<chip::Credentials::AttestationVerificationResult> additional)
{
    (void)peer_id;
    (void)stage;
    (void)additional;
    char buf[96];
    snprintf(buf, sizeof(buf), "commission failed: %s", chip::ErrorStr(error));
    set_error(buf);
    s_pairing_in_progress = false;
}

static void register_pairing_callbacks(void)
{
    esp_matter::controller::pairing_command_callbacks_t cb = {
        .pase_callback = on_pairing_complete,
        .commissioning_success_callback = on_commissioning_success,
        .commissioning_failure_callback = on_commissioning_failure,
        .unpair_complete_callback = nullptr,
    };
    auto &pairing = esp_matter::controller::pairing_command::get_instance();
    pairing.set_callbacks(cb);
    /* Hub does not run ICD client storage; default esp-matter enables ICD registration and fails commissioning. */
    pairing.set_icd_registration(false);
}

static esp_err_t validate_commission_node_id(uint64_t node_id)
{
    if (node_id == 0 || node_id == k_controller_node_id) {
        set_error("node_id must be >= 2 (commissioner uses 1)");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/**
 * DNS-SD discovery timeout often leaves PairingDelegate set without
 * OnCommissioningFailure / OnPairingComplete — force-clear so the UI can retry.
 */
static bool ensure_pairing_slot_free(void)
{
    auto &client = esp_matter::controller::matter_controller_client::get_instance();
    auto *commissioner = client.get_commissioner();
    if (!commissioner) {
        set_error("commissioner not ready");
        return false;
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (!s_pairing_in_progress && commissioner->GetPairingDelegate() == nullptr) {
        return true;
    }
    ESP_LOGW(TAG, "clearing sticky pairing state (delegate=%d flag=%d)",
             commissioner->GetPairingDelegate() != nullptr, s_pairing_in_progress);
    commissioner->RegisterPairingDelegate(nullptr);
    s_pairing_in_progress = false;
    return true;
}

static std::string normalize_pairing_code(const char *pairing_code)
{
    if (!pairing_code || !pairing_code[0]) {
        return {};
    }
    if (strncmp(pairing_code, "MT:", 3) != 0) {
        return pairing_code;
    }
    const char *rest = pairing_code + 3;
    for (const char *p = rest; *p != '\0'; p++) {
        if (*p != '-' && (*p < '0' || *p > '9')) {
            return pairing_code;
        }
    }
    ESP_LOGD(TAG, "manual pairing code: strip MT: prefix");
    return rest;
}

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    if (event->Type == chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged) {
        ESP_LOGD(TAG, "Matter interface IP changed");
    }
}

extern "C" esp_err_t minihub_matter_wifi_bootstrap(void);

esp_err_t minihub_matter_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* On WIFI_STA builds: seed credentials for CHIP. On ETH-only: no-op. */
    esp_err_t err = minihub_matter_wifi_bootstrap();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "network bootstrap: %s", esp_err_to_name(err));
    }

    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        set_error("esp_matter start failed");
        ESP_LOGE(TAG, "esp_matter::start: %s", esp_err_to_name(err));
        return err;
    }

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &client = esp_matter::controller::matter_controller_client::get_instance();
    err = client.init(k_controller_node_id, k_controller_fabric_id, k_controller_port);
    if (err != ESP_OK) {
        set_error("controller init failed");
        return err;
    }
    err = client.setup_commissioner();
    if (err != ESP_OK) {
        set_error("setup_commissioner failed");
        return err;
    }

    /* Commissioner-only builds skip Matter Server / DnssdServer::StartServer().
     * With chip_mdns=platform, FindOperational needs DiscoveryImplPlatform Init
     * or ResolveNodeId returns CHIP_ERROR_INCORRECT_STATE immediately. */
    {
        CHIP_ERROR cerr = chip::Dnssd::Resolver::Instance().Init(chip::DeviceLayer::UDPEndPointManager());
        if (cerr != CHIP_NO_ERROR && cerr != CHIP_ERROR_INCORRECT_STATE) {
            ESP_LOGW(TAG, "Dnssd Resolver::Init: %" CHIP_ERROR_FORMAT, cerr.Format());
        }
        cerr = chip::Dnssd::ServiceAdvertiser::Instance().Init(chip::DeviceLayer::UDPEndPointManager());
        if (cerr != CHIP_NO_ERROR && cerr != CHIP_ERROR_INCORRECT_STATE) {
            ESP_LOGW(TAG, "Dnssd Advertiser::Init: %" CHIP_ERROR_FORMAT, cerr.Format());
        }
        /* CASESessionManager::Init normally does this; re-assert after Dnssd Init. */
        cerr = chip::AddressResolve::Resolver::Instance().Init(&chip::DeviceLayer::SystemLayer());
        if (cerr != CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "AddressResolve::Init: %" CHIP_ERROR_FORMAT, cerr.Format());
        }
    }

    register_pairing_callbacks();
    {
        extern esp_err_t minihub_matter_ota_provider_install(void);
        esp_err_t ota_err = minihub_matter_ota_provider_install();
        if (ota_err != ESP_OK && ota_err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "OTA Provider install: %s", esp_err_to_name(ota_err));
        }
    }
    s_started = true;
    set_error(NULL);
    ESP_LOGI(TAG, "Matter commissioner ready");
    return ESP_OK;
}

esp_err_t minihub_matter_attach_thread(void)
{
#if !CONFIG_ENABLE_MATTER_OVER_THREAD
    return ESP_ERR_NOT_SUPPORTED;
#else
    static bool s_attached;
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    /* esp_openthread_get_instance() is always non-NULL (static Instance). */
    if (!minihub_thread_ready()) {
        ESP_LOGW(TAG, "attach_thread: Thread BR not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_attached && chip::DeviceLayer::ConnectivityMgr().IsThreadProvisioned()) {
        return ESP_OK;
    }

    /* openthread_init_stack() asserts s_platform_config before noticing OT is
     * already up — seed the same platform config MiniHub used to start BR. */
    static esp_openthread_platform_config_t s_plat = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    esp_err_t err = set_openthread_platform_config(&s_plat);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_openthread_platform_config: %s", esp_err_to_name(err));
        return err;
    }

    /* Do not hold CHIP stack lock here: InitThreadStack takes the OT lock and
     * may post platform events that need the CHIP lock. */
    CHIP_ERROR cerr = chip::DeviceLayer::ThreadStackMgr().InitThreadStack();
    if (cerr != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "ThreadStackMgr InitThreadStack: %" CHIP_ERROR_FORMAT, cerr.Format());
        set_error("thread stack attach failed");
        return ESP_FAIL;
    }

    s_attached = true;
    bool provisioned = chip::DeviceLayer::ConnectivityMgr().IsThreadProvisioned();
    ESP_LOGI(TAG, "CHIP ThreadStack attached (provisioned=%d)", provisioned ? 1 : 0);
    return ESP_OK;
#endif
}

bool minihub_matter_ready(void)
{
    return s_started;
}

static esp_err_t pairing_code_ble_ethernet(chip::NodeId node_id, const char *payload)
{
    /* BLE discovery, no Wi-Fi/Thread credentials — Ethernet (or already-linked) devices. */
    chip::Controller::CommissioningParameters params;
    auto &client = esp_matter::controller::matter_controller_client::get_instance();
    auto *commissioner = client.get_commissioner();
    if (!commissioner) {
        return ESP_ERR_INVALID_STATE;
    }
    if (commissioner->GetPairingDelegate() != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    commissioner->RegisterPairingDelegate(&esp_matter::controller::pairing_command::get_instance());
    CHIP_ERROR cerr = commissioner->PairDevice(node_id, payload, params, chip::Controller::DiscoveryType::kAll);
    return (cerr == CHIP_NO_ERROR) ? ESP_OK : ESP_FAIL;
}

static void ensure_dnssd_ready(void)
{
    CHIP_ERROR err = chip::Dnssd::Resolver::Instance().Init(chip::DeviceLayer::UDPEndPointManager());
    ESP_LOGI(TAG, "pre-commission Dnssd Resolver::Init -> %" CHIP_ERROR_FORMAT, err.Format());
    err = chip::Dnssd::ServiceAdvertiser::Instance().Init(chip::DeviceLayer::UDPEndPointManager());
    ESP_LOGI(TAG, "pre-commission Advertiser inited=%d -> %" CHIP_ERROR_FORMAT,
             chip::Dnssd::ServiceAdvertiser::Instance().IsInitialized() ? 1 : 0, err.Format());
    err = chip::AddressResolve::Resolver::Instance().Init(&chip::DeviceLayer::SystemLayer());
    ESP_LOGI(TAG, "pre-commission AddressResolve::Init -> %" CHIP_ERROR_FORMAT " thread_prov=%d", err.Format(),
             chip::DeviceLayer::ConnectivityMgr().IsThreadProvisioned() ? 1 : 0);
}

esp_err_t minihub_matter_commission_code_ex(uint64_t node_id, const char *pairing_code,
                                            minihub_matter_discovery_t discovery,
                                            const char *wifi_ssid, const char *wifi_password)
{
    if (!s_started || !pairing_code || !pairing_code[0]) {
        set_error(!s_started ? "matter commissioner not ready" : "empty pairing code");
        ESP_LOGW(TAG, "commission refused: %s", minihub_matter_last_error());
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = validate_commission_node_id(node_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commission refused: bad node_id");
        return err;
    }
    if (!ensure_pairing_slot_free()) {
        return ESP_ERR_INVALID_STATE;
    }

    std::string code = normalize_pairing_code(pairing_code);
    if (code.empty()) {
        set_error("empty pairing code");
        return ESP_ERR_INVALID_ARG;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    uint8_t dataset_buf[256];
    size_t dataset_len = 0;

    if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI) {
        if (wifi_ssid && wifi_ssid[0]) {
            strncpy(ssid, wifi_ssid, sizeof(ssid) - 1);
        }
        if (wifi_password && wifi_password[0]) {
            strncpy(pass, wifi_password, sizeof(pass) - 1);
        }
        if (!ssid[0] || !pass[0]) {
            wifi_config_t wifi_cfg = {};
            if (minihub_provision_load_credentials(&wifi_cfg) == ESP_OK) {
                if (!ssid[0] && wifi_cfg.sta.ssid[0]) {
                    strncpy(ssid, (const char *)wifi_cfg.sta.ssid, sizeof(ssid) - 1);
                }
                if (!pass[0] && wifi_cfg.sta.password[0]) {
                    strncpy(pass, (const char *)wifi_cfg.sta.password, sizeof(pass) - 1);
                }
            }
        }
        if (!ssid[0]) {
            set_error("ble_wifi needs wifi_ssid (or Hub provision Wi-Fi)");
            return ESP_ERR_INVALID_ARG;
        }
    } else if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD) {
#if CONFIG_MINIHUB_THREAD_ADAPTER
        if (!minihub_thread_ready()) {
            set_error("ble_thread needs Thread BR running (adapters.thread.enabled)");
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t derr = minihub_thread_dataset_copy(dataset_buf, sizeof(dataset_buf), &dataset_len);
        if (derr != ESP_OK || dataset_len == 0) {
            set_error("ble_thread: no active Thread dataset");
            return derr == ESP_OK ? ESP_ERR_INVALID_STATE : derr;
        }
#else
        set_error("ble_thread requires CONFIG_MINIHUB_THREAD_ADAPTER");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    s_pairing_in_progress = true;
    set_error(NULL);

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    ensure_dnssd_ready();
    if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI) {
        ESP_LOGI(TAG, "commission BLE+WiFi node=%" PRIu64, node_id);
        err = esp_matter::controller::pairing_code_wifi(static_cast<chip::NodeId>(node_id), ssid, pass,
                                                        code.c_str());
#if CONFIG_MINIHUB_THREAD_ADAPTER
    } else if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD) {
        ESP_LOGI(TAG, "commission BLE+Thread node=%" PRIu64 " dataset_len=%u", node_id,
                 (unsigned)dataset_len);
        err = esp_matter::controller::pairing_code_thread(static_cast<chip::NodeId>(node_id), code.c_str(),
                                                          dataset_buf, static_cast<uint8_t>(dataset_len));
#endif
    } else if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET) {
        ESP_LOGI(TAG, "commission BLE+Ethernet node=%" PRIu64, node_id);
        err = pairing_code_ble_ethernet(static_cast<chip::NodeId>(node_id), code.c_str());
    } else {
        ESP_LOGI(TAG, "commission network-only node=%" PRIu64, node_id);
        err = esp_matter::controller::pairing_code(static_cast<chip::NodeId>(node_id), code.c_str());
    }
    if (err != ESP_OK) {
        s_pairing_in_progress = false;
        if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI) {
            set_error("pairing_code_wifi failed");
        } else if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD) {
            set_error("pairing_code_thread failed");
        } else if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET) {
            set_error("pairing_code_ble_ethernet failed");
        } else {
            set_error("pairing_code failed");
        }
        return err;
    }
    return ESP_OK;
}

esp_err_t minihub_matter_commission_code(uint64_t node_id, const char *pairing_code)
{
    return minihub_matter_commission_code_ex(node_id, pairing_code, MINIHUB_MATTER_DISCOVERY_NETWORK,
                                             nullptr, nullptr);
}

esp_err_t minihub_matter_commission_pin(uint64_t node_id, uint32_t setup_pin)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = validate_commission_node_id(node_id);
    if (err != ESP_OK) {
        return err;
    }
    if (!ensure_pairing_slot_free()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pairing_in_progress = true;
    set_error(NULL);
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    err = esp_matter::controller::pairing_on_network(node_id, setup_pin);
    if (err != ESP_OK) {
        s_pairing_in_progress = false;
        set_error("pairing_on_network failed");
    }
    return err;
}

esp_err_t minihub_matter_onoff(uint64_t node_id, uint16_t endpoint_id, bool on)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t cmd = on ? k_onoff_on_cmd : k_onoff_off_cmd;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    esp_err_t err = esp_matter::controller::send_invoke_cluster_command(node_id, endpoint_id, k_onoff_cluster, cmd,
                                                                        nullptr);
    if (err != ESP_OK) {
        set_error("onoff command failed");
        return err;
    }
    s_last_read.node_id = node_id;
    s_last_read.endpoint_id = endpoint_id;
    s_last_read.on = on;
    s_last_read.valid = true;
    set_error(NULL);
    return ESP_OK;
}

esp_err_t minihub_matter_read_onoff(uint64_t node_id, uint16_t endpoint_id, bool *out_on)
{
    if (!out_on) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_last_read.valid && s_last_read.node_id == node_id && s_last_read.endpoint_id == endpoint_id) {
        *out_on = s_last_read.on;
        return ESP_OK;
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    (void)esp_matter::controller::send_read_attr_command(node_id, endpoint_id, k_onoff_cluster, k_onoff_attr);
    return ESP_ERR_NOT_FINISHED;
}

cJSON *minihub_matter_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ready", s_started);
    cJSON_AddBoolToObject(root, "commissioned", s_commissioned);
    cJSON_AddBoolToObject(root, "pairing", s_pairing_in_progress);
    cJSON_AddStringToObject(root, "mode", "commissioner");
#if CONFIG_MINIHUB_THREAD_ADAPTER
    cJSON_AddBoolToObject(root, "thread_br_ready", minihub_thread_ready());
#else
    cJSON_AddBoolToObject(root, "thread_br_ready", false);
#endif
    if (s_last_error[0]) {
        cJSON_AddStringToObject(root, "last_error", s_last_error);
    }

    cJSON *prefill = cJSON_CreateObject();
    if (prefill) {
        wifi_config_t wifi_cfg = {};
        bool has_ssid = false;
        bool has_password = false;
        if (minihub_provision_load_credentials(&wifi_cfg) == ESP_OK) {
            if (wifi_cfg.sta.ssid[0]) {
                cJSON_AddStringToObject(prefill, "ssid", (const char *)wifi_cfg.sta.ssid);
                has_ssid = true;
            }
            has_password = wifi_cfg.sta.password[0] != '\0';
        }
        if (!has_ssid) {
            cJSON_AddStringToObject(prefill, "ssid", "");
        }
        cJSON_AddBoolToObject(prefill, "has_password", has_password);
        cJSON_AddItemToObject(root, "wifi_prefill", prefill);
    }
    return root;
}

const char *minihub_matter_last_error(void)
{
    return s_last_error[0] ? s_last_error : NULL;
}

esp_err_t minihub_matter_set_identity_cb(minihub_matter_identity_cb_t cb, void *ctx)
{
    s_identity_cb = cb;
    s_identity_ctx = ctx;
    return ESP_OK;
}

esp_err_t minihub_matter_interview_identity(uint64_t node_id)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (node_id < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    using chip::Platform::ScopedMemoryBufferWithSize;
    using chip::app::AttributePathParams;
    using chip::app::EventPathParams;
    using namespace chip::app::Clusters::BasicInformation::Attributes;

    const uint32_t attrs[] = {
        VendorName::Id,
        ProductName::Id,
        SoftwareVersionString::Id,
        SoftwareVersion::Id,
        HardwareVersionString::Id,
        HardwareVersion::Id,
    };
    const size_t n = sizeof(attrs) / sizeof(attrs[0]);
    ScopedMemoryBufferWithSize<AttributePathParams> attr_paths;
    ScopedMemoryBufferWithSize<EventPathParams> event_paths;
    attr_paths.Alloc(n);
    if (!attr_paths.Get()) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < n; i++) {
        attr_paths[i] = AttributePathParams(0, k_basic_cluster, attrs[i]);
    }
    auto *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        node_id, std::move(attr_paths), std::move(event_paths), on_identity_attr, on_identity_done, nullptr);
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "interview Basic Information node=%" PRIu64, node_id);
    return cmd->send_command();
}
