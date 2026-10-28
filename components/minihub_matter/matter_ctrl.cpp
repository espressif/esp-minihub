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
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "minihub_provision.h"

/* Linked by minihub_hub; weak so matter can build without a hard REQUIRES cycle. */
extern "C" esp_err_t minihub_hub_reload_async(void) __attribute__((weak));
extern "C" esp_err_t minihub_hub_resync_adapter_async(const char *adapter_name) __attribute__((weak));
#if CONFIG_MINIHUB_THREAD_ADAPTER
#include "minihub_thread.h"
#endif
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_subscribe_command.h>
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
static SemaphoreHandle_t s_commission_sem;
static portMUX_TYPE s_commission_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_err_t s_commission_result = ESP_OK;
/* Commission watchdog timeout. Operational DNS-SD (resolving the joined device
 * on the fabric) can be slow on first browse — the local OTBR record may not be
 * visible immediately after commissioning — so this is Kconfig-tunable and
 * defaults to 300 s rather than the old hard 180 s. See MINIHUB_MATTER_COMMISSION_WAIT_MS. */
#ifndef CONFIG_MINIHUB_MATTER_COMMISSION_WAIT_MS
#define CONFIG_MINIHUB_MATTER_COMMISSION_WAIT_MS 300000
#endif
static constexpr uint32_t k_commission_wait_ms = CONFIG_MINIHUB_MATTER_COMMISSION_WAIT_MS;
static constexpr size_t k_pending_ep_max = 16;

enum {
    COMMISSION_PHASE_IDLE = 0,
    COMMISSION_PHASE_WAITING = 1,
    COMMISSION_PHASE_DONE = 2,
};
static int s_commission_phase = COMMISSION_PHASE_IDLE;
static uint64_t s_commission_node_id;

/* Sub-state of COMMISSION_PHASE_WAITING. esp-matter only hands us PASE,
 * success and failure, so this is as fine-grained as the signals allow. */
enum {
    COMMISSION_STAGE_NONE = 0,
    COMMISSION_STAGE_DISCOVERING = 1,  /* finding the beacon, PASE handshake */
    COMMISSION_STAGE_BINDING = 2,      /* PASE up: NOC + network credentials */
    COMMISSION_STAGE_ACTIVATING = 3,   /* on the fabric: resolve + first read */
};
static int s_commission_stage = COMMISSION_STAGE_NONE;

static const char *commission_stage_name(int stage)
{
    switch (stage) {
    case COMMISSION_STAGE_DISCOVERING: return "discovering";
    case COMMISSION_STAGE_BINDING: return "binding";
    case COMMISSION_STAGE_ACTIVATING: return "activating";
    default: return "idle";
    }
}

typedef struct {
    bool valid;
    uint64_t node_id;
    uint16_t endpoints[k_pending_ep_max];
    size_t endpoint_count;
    bool merge_existing;
    bool do_reload;
    char transport[16];
} pending_device_t;

static pending_device_t s_pending_device;
static esp_timer_handle_t s_commission_watchdog;
static bool s_persist_busy;

static bool ensure_pairing_slot_free(void);
static void schedule_finalize(esp_err_t result);

typedef struct {
    uint64_t node_id;
    uint16_t endpoint_id;
    bool on;
    bool valid;
} matter_read_slot_t;

/* Optimistic last-issued state, written by minihub_matter_onoff() only. Nothing
 * invalidates it, so read_onoff must not consume it — see the Task 1 ruling. */
static matter_read_slot_t s_last_read;

#ifndef CONFIG_MINIHUB_MATTER_MAX_NODES
#define CONFIG_MINIHUB_MATTER_MAX_NODES 128
#endif
/* Capacity from Kconfig (MINIHUB_MATTER_MAX_NODES, default 128). Tables are
 * heap-allocated from PSRAM at init; overflow is logged + rejected, never
 * silently dropped. Allocated once, never freed -> no use-after-free surface. */
#define IDENTITY_SLOT_MAX ((int)CONFIG_MINIHUB_MATTER_MAX_NODES)

/* Forward declaration: allocate the tables once at init (idempotent). */
static esp_err_t matter_ctrl_tables_ensure(void);

typedef struct {
    uint64_t node_id;
    minihub_matter_identity_t id;
    char sw_num[16];
    char hw_num[16];
    bool in_use;
} identity_slot_t;

static identity_slot_t *s_id_slots = NULL;
static minihub_matter_identity_cb_t s_identity_cb;
static void *s_identity_ctx;

static minihub_matter_attr_bool_cb_t s_attr_cb;
static void *s_attr_ctx;

esp_err_t minihub_matter_set_attr_cb(minihub_matter_attr_bool_cb_t cb, void *ctx)
{
    s_attr_cb = cb;
    s_attr_ctx = ctx;
    return ESP_OK;
}

/* Bookkeeping for a report about to reach the consumer sink. Defined after the
 * subscription table (declared forward here). Counts reports per node so
 * /api/matter/status can prove a report arrived even when the value did not
 * change, and warns (rate-limited) when the (node, endpoint) matches no armed
 * row — that is the silent-drop case when config and the real endpoint differ.
 * Runs on the CHIP event task inside the stack lock. */
static void note_sub_report(uint64_t node_id, uint16_t endpoint_id);

/* esp_matter::controller::attribute_report_cb_t target. Runs on the CHIP event
 * task inside the stack lock. */
static void on_attr_report(uint64_t node_id, const chip::app::ConcreteDataAttributePath &path,
                           chip::TLV::TLVReader *data, const chip::app::StatusIB &status)
{
    if (!s_attr_cb || data == nullptr || !status.IsSuccess()) {
        return;
    }
    bool val = false;
    if (chip::app::DataModel::Decode(*data, val) != CHIP_NO_ERROR) {
        return; /* non-boolean attribute, or decode failure */
    }
    note_sub_report(node_id, path.mEndpointId);
    s_attr_cb(node_id, path.mEndpointId, path.mClusterId, path.mAttributeId, val, s_attr_ctx);
}

static void set_error(const char *msg)
{
    /* s_last_error is written here on the commissioning task and read on the
     * httpd task (minihub_matter_last_error / status_json) — guard it. */
    portENTER_CRITICAL(&s_commission_mux);
    if (!msg) {
        s_last_error[0] = '\0';
        portEXIT_CRITICAL(&s_commission_mux);
        return;
    }
    strncpy(s_last_error, msg, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
    portEXIT_CRITICAL(&s_commission_mux);
}

static identity_slot_t *identity_slot(uint64_t node_id, bool create)
{
    matter_ctrl_tables_ensure();
    if (!s_id_slots) {
        return NULL;
    }
    identity_slot_t *empty = NULL;
    for (size_t i = 0; i < (size_t)IDENTITY_SLOT_MAX; i++) {
        if (s_id_slots[i].in_use && s_id_slots[i].node_id == node_id) {
            return &s_id_slots[i];
        }
        if (!s_id_slots[i].in_use && !empty) {
            empty = &s_id_slots[i];
        }
    }
    if (!create || !empty) {
        if (create && !empty) {
            ESP_LOGE(TAG, "identity slot table full (cap %d): node%" PRIu64 " dropped",
                     IDENTITY_SLOT_MAX, (unsigned long long)node_id);
        }
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

static void clear_pending_device_locked(void)
{
    s_pending_device.valid = false;
    s_pending_device.endpoint_count = 0;
    s_pending_device.transport[0] = '\0';
}

static void stop_commission_watchdog(void)
{
    if (s_commission_watchdog) {
        (void)esp_timer_stop(s_commission_watchdog);
    }
}

static void signal_commission_done(esp_err_t err)
{
    bool give = false;
    stop_commission_watchdog();
    portENTER_CRITICAL(&s_commission_mux);
    if (s_commission_phase == COMMISSION_PHASE_WAITING) {
        s_commission_phase = COMMISSION_PHASE_DONE;
        s_commission_result = err;
        give = true;
        if (err == ESP_OK) {
            s_commission_stage = COMMISSION_STAGE_NONE;
        }
        /* On failure keep the stage: the UI says where it stopped. */
        if (err != ESP_OK) {
            clear_pending_device_locked();
        }
    }
    portEXIT_CRITICAL(&s_commission_mux);
    if (give && s_commission_sem) {
        xSemaphoreGive(s_commission_sem);
    }
}

static void commission_watchdog_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "commission watchdog: timed out after %u ms", (unsigned)k_commission_wait_ms);
    (void)ensure_pairing_slot_free();
    s_pairing_in_progress = false;
    set_error("commission timed out");
    schedule_finalize(ESP_ERR_TIMEOUT);
}

static void arm_commission_watchdog(void)
{
    if (!s_commission_watchdog) {
        const esp_timer_create_args_t args = {
            .callback = &commission_watchdog_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "matter_comm",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_commission_watchdog) != ESP_OK) {
            ESP_LOGW(TAG, "commission watchdog create failed");
            s_commission_watchdog = nullptr;
            return;
        }
    }
    (void)esp_timer_stop(s_commission_watchdog);
    if (esp_timer_start_once(s_commission_watchdog, (uint64_t)k_commission_wait_ms * 1000ULL) != ESP_OK) {
        ESP_LOGW(TAG, "commission watchdog start failed");
    }
}

static bool arm_commission_wait(void)
{
    if (!s_commission_sem) {
        s_commission_sem = xSemaphoreCreateBinary();
    }
    if (!s_commission_sem) {
        return false;
    }
    while (xSemaphoreTake(s_commission_sem, 0) == pdTRUE) {
    }
    portENTER_CRITICAL(&s_commission_mux);
    s_commission_phase = COMMISSION_PHASE_WAITING;
    s_commission_result = ESP_OK;
    s_commission_stage = COMMISSION_STAGE_DISCOVERING;
    clear_pending_device_locked();
    portEXIT_CRITICAL(&s_commission_mux);
    arm_commission_watchdog();
    return true;
}

static void read_commission_phase(int *phase_out, esp_err_t *result_out)
{
    portENTER_CRITICAL(&s_commission_mux);
    if (phase_out) {
        *phase_out = s_commission_phase;
    }
    if (result_out) {
        *result_out = s_commission_result;
    }
    portEXIT_CRITICAL(&s_commission_mux);
}

static void finalize_commission_task(void *arg)
{
    const esp_err_t result = static_cast<esp_err_t>(reinterpret_cast<uintptr_t>(arg));
    pending_device_t pending = {};
    portENTER_CRITICAL(&s_commission_mux);
    if (s_pending_device.valid) {
        pending = s_pending_device;
        clear_pending_device_locked();
    }
    portEXIT_CRITICAL(&s_commission_mux);

    if (pending.valid) {
        const char *phase = (result == ESP_OK) ? nullptr : "failed";
        const char *err_msg = (result == ESP_OK) ? nullptr : minihub_matter_last_error();
        esp_err_t perr = minihub_matter_devices_set_commission_phase(pending.node_id, phase, err_msg);
        ESP_LOGI(TAG, "post-commission phase node=%" PRIu64 " result=%s phase_err=%s", pending.node_id,
                 esp_err_to_name(result), esp_err_to_name(perr));
        if (pending.do_reload) {
            /* Republish the matter points in place. A full hub reload stops every
             * adapter, and each one unregisters its points, so the whole console
             * device list disappears for the ~1.7 s that restart takes. */
            if (minihub_hub_resync_adapter_async) {
                (void)minihub_hub_resync_adapter_async("matter");
            } else if (minihub_hub_reload_async) {
                (void)minihub_hub_reload_async();
            }
        }
    }

    s_persist_busy = false;
    signal_commission_done(result);
    vTaskDelete(nullptr);
}

static void schedule_finalize(esp_err_t result)
{
    bool has_pending = false;
    portENTER_CRITICAL(&s_commission_mux);
    has_pending = s_pending_device.valid;
    portEXIT_CRITICAL(&s_commission_mux);

    if (!has_pending) {
        signal_commission_done(result);
        return;
    }
    if (s_persist_busy) {
        signal_commission_done(ESP_ERR_INVALID_STATE);
        return;
    }
    s_persist_busy = true;
    if (xTaskCreate(finalize_commission_task, "matter_finalize", 4096,
                    reinterpret_cast<void *>(static_cast<uintptr_t>(result)), 5, nullptr) != pdPASS) {
        s_persist_busy = false;
        set_error("finalize task create failed");
        signal_commission_done(ESP_ERR_NO_MEM);
    }
}

static void on_pairing_complete(CHIP_ERROR err)
{
    if (err != CHIP_NO_ERROR) {
        char buf[96];
        snprintf(buf, sizeof(buf), "PASE session failed: %s", chip::ErrorStr(err));
        set_error(buf);
        s_pairing_in_progress = false;
        schedule_finalize(ESP_FAIL);
        return;
    }
    s_commission_stage = COMMISSION_STAGE_BINDING;
    ESP_LOGI(TAG, "commission stage -> binding (PASE up)");
}

static void on_commissioning_success(chip::ScopedNodeId peer_id)
{
    ESP_LOGI(TAG, "Commissioned node 0x%016" PRIx64, peer_id.GetNodeId());
    s_commissioned = true;
    s_pairing_in_progress = false;
    set_error(NULL);
    s_commission_node_id = peer_id.GetNodeId();
    /* Credentials are on the device; still resolving it operationally and
     * reading BasicInformation, which is what "activating" means here. */
    s_commission_stage = COMMISSION_STAGE_ACTIVATING;
    (void)minihub_matter_interview_identity(peer_id.GetNodeId());
    /* Stay in WAITING until phase clear (+ optional reload) finishes. */
    schedule_finalize(ESP_OK);
}

static void on_commissioning_failure(chip::ScopedNodeId peer_id, CHIP_ERROR error,
                                     chip::Controller::CommissioningStage stage,
                                     std::optional<chip::Credentials::AttestationVerificationResult> additional)
{
    (void)peer_id;
    (void)additional;
    /* commission_error is 96 bytes in adapters.json, so the card gets the compact
     * form and the actionable hint goes to the log only. */
    if (error == CHIP_ERROR_WRONG_NODE_ID) {
        ESP_LOGW(TAG, "device rejected the operational credential (kInvalidNodeOpId): node id "
                      "%" PRIu64 " is already on this fabric, or the device still holds an old "
                      "fabric — factory-reset it or commission under another node id",
                 peer_id.GetNodeId());
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "commission failed at %s (%s): %s",
             commission_stage_name(s_commission_stage), chip::Controller::StageToString(stage),
             chip::ErrorStr(error));
    set_error(buf);
    s_pairing_in_progress = false;
    schedule_finalize(ESP_FAIL);
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
    if (!s_pairing_in_progress && commissioner->GetPairingDelegate() == nullptr &&
        commissioner->GetCommissioningStage() == chip::Controller::CommissioningStage::kSecurePairing) {
        return true;
    }
    ESP_LOGW(TAG, "clearing sticky pairing state (delegate=%d flag=%d stage=%s)",
             commissioner->GetPairingDelegate() != nullptr, s_pairing_in_progress,
             chip::Controller::StageToString(commissioner->GetCommissioningStage()));
    /* Drop the delegate first so StopPairing's completion does not finalize the
     * device card we are about to commission. */
    commissioner->RegisterPairingDelegate(nullptr);
    s_pairing_in_progress = false;
    /* A commissioning left at FindOperationalForStayActive makes the next
     * PairDevice() complete PASE synchronously and then fail Commission().
     * SetUpCodePairer treats that failure as "PASE never started" and calls
     * PASEEstablishmentComplete() again, which asserts. StopPairing resets the
     * stage to kSecurePairing. The first call may only stop discovery; the
     * second cancels the commissioning itself. */
    if (s_commission_node_id >= 2 &&
        commissioner->GetCommissioningStage() != chip::Controller::CommissioningStage::kSecurePairing) {
        (void)commissioner->StopPairing(static_cast<chip::NodeId>(s_commission_node_id));
        if (commissioner->GetCommissioningStage() != chip::Controller::CommissioningStage::kSecurePairing) {
            (void)commissioner->StopPairing(static_cast<chip::NodeId>(s_commission_node_id));
        }
    }
    return true;
}

static void resync_matter_points(void)
{
    /* Republish in place so the overview card leaves "等待入网中" without a
     * full hub reload (that blanks every other adapter for ~1.7 s). */
    if (minihub_hub_resync_adapter_async) {
        (void)minihub_hub_resync_adapter_async("matter");
    } else if (minihub_hub_reload_async) {
        (void)minihub_hub_reload_async();
    }
}

esp_err_t minihub_matter_commission_cancel(uint64_t node_id)
{
    /* Card button: this node only. Do not signal_commission_done — that is the
     * header "cancel all" path and would drop every other waiting node. */
    bool active = false;
    portENTER_CRITICAL(&s_commission_mux);
    if (node_id >= 2 && s_commission_node_id == node_id) {
        active = s_pending_device.valid ? (s_pending_device.node_id == node_id)
                                        : true;
        if (active && s_commission_phase == COMMISSION_PHASE_WAITING) {
            clear_pending_device_locked();
            s_commission_stage = COMMISSION_STAGE_NONE;
        }
    }
    portEXIT_CRITICAL(&s_commission_mux);

    esp_err_t perr = minihub_matter_devices_fail_if_waiting(node_id, "commissioning cancelled");
    if (perr != ESP_OK && !active) {
        return ESP_ERR_NOT_FOUND;
    }
    if (active) {
        /* Stop this node's pairing so it does not finish in the background.
         * Leave the phase WAITING: the header button is still "cancel all". */
        stop_commission_watchdog();
        (void)ensure_pairing_slot_free();
        set_error("commissioning cancelled");
    }
    if (perr == ESP_OK) {
        resync_matter_points();
    }
    ESP_LOGW(TAG, "commissioning cancelled for node %" PRIu64 " (session still %s)", node_id,
             active ? "stopped for this node" : "untouched");
    return ESP_OK;
}

esp_err_t minihub_matter_commission_cancel_all(void)
{
    esp_err_t perr = minihub_matter_devices_fail_all_waiting("commissioning cancelled");
    stop_commission_watchdog();
    (void)ensure_pairing_slot_free();
    set_error("commissioning cancelled");
    s_persist_busy = false;
    portENTER_CRITICAL(&s_commission_mux);
    clear_pending_device_locked();
    s_commission_stage = COMMISSION_STAGE_NONE;
    portEXIT_CRITICAL(&s_commission_mux);
    signal_commission_done(ESP_FAIL);
    portENTER_CRITICAL(&s_commission_mux);
    if (s_commission_phase != COMMISSION_PHASE_IDLE) {
        s_commission_phase = COMMISSION_PHASE_DONE;
        s_commission_result = ESP_FAIL;
    }
    portEXIT_CRITICAL(&s_commission_mux);
    if (perr == ESP_OK) {
        resync_matter_points();
    }
    ESP_LOGW(TAG, "commissioning cancelled for every waiting node");
    return ESP_OK;
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

#if CONFIG_ENABLE_MATTER_OVER_THREAD
/* CHIP must own the otInstance before DeviceControllerFactory::Init runs: the
 * controller factory initializes DNS-SD, which derives the SRP hostname from the
 * 802.15.4 MAC and clears the SRP host — both fail with CHIP_ERROR_INCORRECT_STATE
 * while mOTInst is null, and OpenThreadDnssdInit propagates that out as
 * CHIP_ERROR_NOT_FOUND, aborting the whole commissioner init. esp_matter::start()
 * skips the attach for us because CONFIG_ESP_MATTER_ENABLE_OPENTHREAD is off
 * (MiniHub owns the Border Router), so do it here — outside the CHIP lock, since
 * InitThreadStack takes the OpenThread lock and its state callback posts CHIP
 * events (deadlock under PlatformMgr's lock). */
static esp_err_t matter_thread_stack_attach(void)
{
    static bool s_attached;
    if (s_attached) {
        return ESP_OK;
    }
    if (!minihub_thread_ready()) {
        ESP_LOGW(TAG, "thread attach: Thread BR not ready");
        return ESP_ERR_INVALID_STATE;
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

    CHIP_ERROR cerr = chip::DeviceLayer::ThreadStackMgr().InitThreadStack();
    if (cerr != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ThreadStackMgr InitThreadStack: %" CHIP_ERROR_FORMAT, cerr.Format());
        return ESP_FAIL;
    }

    s_attached = true;
    ESP_LOGI(TAG, "CHIP ThreadStack attached (provisioned=%d)",
             chip::DeviceLayer::ConnectivityMgr().IsThreadProvisioned() ? 1 : 0);
    return ESP_OK;
}
#endif /* CONFIG_ENABLE_MATTER_OVER_THREAD */

esp_err_t minihub_matter_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    matter_ctrl_tables_ensure();

    /* On WIFI_STA builds: seed credentials for CHIP. On ETH-only: no-op. */
    esp_err_t err = minihub_matter_wifi_bootstrap();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "network bootstrap: %s", esp_err_to_name(err));
    }

    /* esp_matter::start() is process-once. A prior partial init (or an early
     * bootstrap that failed after start) leaves is_started() true while our
     * s_started is still false — treat that as continue, not fatal. */
    if (!esp_matter::is_started()) {
        err = esp_matter::start(app_event_cb);
        if (err != ESP_OK) {
            set_error("esp_matter start failed");
            ESP_LOGE(TAG, "esp_matter::start: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        ESP_LOGW(TAG, "esp_matter already started; finishing commissioner setup");
    }

#if CONFIG_ENABLE_MATTER_OVER_THREAD
    err = matter_thread_stack_attach();
    if (err != ESP_OK) {
        set_error("thread stack attach failed");
        ESP_LOGW(TAG, "commissioner init will fail without CHIP ThreadStack: %s",
                 esp_err_to_name(err));
    }
#endif

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &client = esp_matter::controller::matter_controller_client::get_instance();
    const bool commissioner_ready = (client.get_fabric_index() != chip::kUndefinedFabricIndex);

    if (!commissioner_ready) {
        err = client.init(k_controller_node_id, k_controller_fabric_id, k_controller_port);
        if (err != ESP_OK) {
            /* Factory may already be inited from a prior partial attempt. */
            ESP_LOGW(TAG, "controller init: %s (continuing to setup_commissioner)",
                     esp_err_to_name(err));
        }
        err = client.setup_commissioner();
        if (err != ESP_OK) {
            set_error("setup_commissioner failed");
            ESP_LOGE(TAG, "setup_commissioner: %s", esp_err_to_name(err));
            return err;
        }
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
    /* minihub_matter_init() already attached; this is the retry hook for a hub
     * reload where the Border Router came up after Matter gave up. */
    return matter_thread_stack_attach();
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
    /* EspDnssdInit calls mdns_init(), which fails on a second call. Do not
     * re-Init an already-live resolver — that leaves IsInitialized() false
     * and used to abort commissioning before PairDevice. */
    if (chip::Dnssd::Resolver::Instance().IsInitialized()) {
        ESP_LOGI(TAG, "pre-commission Dnssd Resolver already initialized");
        return;
    }

    /* Init under the CHIP lock, then wait OUTSIDE the lock for the async
     * platform callback. Holding the lock while waiting deadlocks the
     * CHIP event loop that flips IsInitialized(). */
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        CHIP_ERROR err = chip::Dnssd::Resolver::Instance().Init(chip::DeviceLayer::UDPEndPointManager());
        ESP_LOGI(TAG, "pre-commission Dnssd Resolver::Init -> %" CHIP_ERROR_FORMAT, err.Format());
        err = chip::Dnssd::ServiceAdvertiser::Instance().Init(chip::DeviceLayer::UDPEndPointManager());
        ESP_LOGI(TAG, "pre-commission Advertiser inited=%d -> %" CHIP_ERROR_FORMAT,
                 chip::Dnssd::ServiceAdvertiser::Instance().IsInitialized() ? 1 : 0, err.Format());
        err = chip::AddressResolve::Resolver::Instance().Init(&chip::DeviceLayer::SystemLayer());
        ESP_LOGI(TAG, "pre-commission AddressResolve::Init -> %" CHIP_ERROR_FORMAT, err.Format());
    }

    constexpr int k_wait_ms = 3000;
    constexpr int k_step_ms = 20;
    int waited = 0;
    while (!chip::Dnssd::Resolver::Instance().IsInitialized() && waited < k_wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(k_step_ms));
        waited += k_step_ms;
    }
    if (chip::Dnssd::Resolver::Instance().IsInitialized()) {
        ESP_LOGI(TAG, "pre-commission Dnssd Resolver initialized after %d ms", waited);
    } else {
        /* Thread SRP callback can lag, and a repeat mdns_init() never sets
         * the flag. BLE+Thread still needs PairDevice to run. */
        ESP_LOGW(TAG, "pre-commission Dnssd Resolver not initialized after %d ms; continuing", waited);
    }
}

/**
 * Can this commissioning request start at all? Same conditions
 * minihub_matter_commission_code_ex() enforces, answered before it consumes a
 * node id or writes config. Handlers call this first: creating the eager
 * 配网中 card and only then being refused leaves a permanent
 * commission_phase:"failed" row for a device that was never attempted.
 *
 * Note minihub_matter_ready() alone is NOT the predicate — the commissioner can
 * stay started while the Thread BR is off, which is exactly the case that
 * produces a phantom card.
 */
esp_err_t minihub_matter_commission_preflight(minihub_matter_discovery_t discovery,
                                              const char *wifi_ssid)
{
    if (!s_started) {
        set_error("matter commissioner not ready");
        return ESP_ERR_INVALID_STATE;
    }
#if !CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION
    if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD) {
        set_error("ble_thread disabled (CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION)");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI) {
        if (wifi_ssid && wifi_ssid[0]) {
            return ESP_OK;
        }
        wifi_config_t cfg = {};
        if (minihub_provision_load_credentials(&cfg) != ESP_OK || !cfg.sta.ssid[0]) {
            set_error("ble_wifi needs wifi_ssid (or Hub provision Wi-Fi)");
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }
    if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD) {
#if CONFIG_MINIHUB_THREAD_ADAPTER
        if (!minihub_thread_ready()) {
            set_error("ble_thread needs Thread BR running (adapters.thread.enabled)");
            return ESP_ERR_INVALID_STATE;
        }
        uint8_t buf[256];
        size_t len = 0;
        if (minihub_thread_dataset_copy(buf, sizeof(buf), &len) != ESP_OK || len == 0) {
            set_error("ble_thread: no active Thread dataset");
            return ESP_ERR_INVALID_STATE;
        }
#else
        set_error("ble_thread requires CONFIG_MINIHUB_THREAD_ADAPTER");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_OK;
}

esp_err_t minihub_matter_commission_code_ex(uint64_t node_id, const char *pairing_code,
                                            minihub_matter_discovery_t discovery,
                                            const char *wifi_ssid, const char *wifi_password)
{
    if (!pairing_code || !pairing_code[0]) {
        set_error("empty pairing code");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t perr = minihub_matter_commission_preflight(discovery, wifi_ssid);
    if (perr != ESP_OK) {
        return perr;
    }
    if (!s_started) {
        set_error("matter commissioner not ready");
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
#if CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION
    uint8_t dataset_buf[256];
    size_t dataset_len = 0;
#endif

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
#if CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION
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
        set_error("ble_thread disabled (CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION)");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

#if CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION && CONFIG_ENABLE_MATTER_OVER_THREAD
    if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD) {
        esp_err_t aerr = minihub_matter_attach_thread();
        if (aerr != ESP_OK) {
            set_error("ble_thread: CHIP ThreadStack attach failed");
            ESP_LOGW(TAG, "attach_thread before commission: %s", esp_err_to_name(aerr));
            return aerr;
        }
    }
#endif

    if (!arm_commission_wait()) {
        set_error("commission wait alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_commission_node_id = node_id;
    s_pairing_in_progress = true;
    set_error(NULL);

    ensure_dnssd_ready();

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI) {
        ESP_LOGI(TAG, "commission BLE+WiFi node=%" PRIu64, node_id);
        err = esp_matter::controller::pairing_code_wifi(static_cast<chip::NodeId>(node_id), ssid, pass,
                                                        code.c_str());
#if CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION
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
        portENTER_CRITICAL(&s_commission_mux);
        if (s_commission_phase == COMMISSION_PHASE_WAITING) {
            s_commission_phase = COMMISSION_PHASE_IDLE;
            s_commission_stage = COMMISSION_STAGE_NONE;
        }
        portEXIT_CRITICAL(&s_commission_mux);
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

esp_err_t minihub_matter_set_pending_device(uint64_t node_id, const uint16_t *endpoints,
                                            size_t endpoint_count, bool merge_existing,
                                            bool do_reload, const char *transport)
{
    if (node_id < 2 || !endpoints || endpoint_count == 0 || endpoint_count > k_pending_ep_max) {
        return ESP_ERR_INVALID_ARG;
    }
    int phase = COMMISSION_PHASE_IDLE;
    read_commission_phase(&phase, nullptr);
    if (phase != COMMISSION_PHASE_WAITING) {
        set_error("no commission in progress");
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_commission_mux);
    s_pending_device.valid = true;
    s_pending_device.node_id = node_id;
    s_pending_device.endpoint_count = endpoint_count;
    s_pending_device.merge_existing = merge_existing;
    s_pending_device.do_reload = do_reload;
    memcpy(s_pending_device.endpoints, endpoints, endpoint_count * sizeof(uint16_t));
    s_pending_device.transport[0] = '\0';
    if (transport && transport[0]) {
        strncpy(s_pending_device.transport, transport, sizeof(s_pending_device.transport) - 1);
        s_pending_device.transport[sizeof(s_pending_device.transport) - 1] = '\0';
    }
    portEXIT_CRITICAL(&s_commission_mux);
    s_commission_node_id = node_id;
    return ESP_OK;
}

esp_err_t minihub_matter_commission_wait(uint32_t timeout_ms)
{
    int phase = COMMISSION_PHASE_IDLE;
    esp_err_t result = ESP_FAIL;
    read_commission_phase(&phase, &result);
    if (phase == COMMISSION_PHASE_DONE) {
        return result;
    }
    if (phase != COMMISSION_PHASE_WAITING || !s_commission_sem) {
        set_error("no commission in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) {
        timeout_ms = k_commission_wait_ms;
    }
    ESP_LOGI(TAG, "waiting for commission result (%u ms)", (unsigned)timeout_ms);
    if (xSemaphoreTake(s_commission_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        read_commission_phase(&phase, &result);
        ESP_LOGI(TAG, "commission wait -> %s", esp_err_to_name(result));
        return result;
    }

    read_commission_phase(&phase, &result);
    if (phase == COMMISSION_PHASE_DONE) {
        ESP_LOGI(TAG, "commission wait -> %s", esp_err_to_name(result));
        return result;
    }

    /* DNS-SD timeout often never calls OnPairingComplete / OnCommissioningFailure. */
    (void)ensure_pairing_slot_free();
    s_pairing_in_progress = false;
    set_error("commission timed out");
    schedule_finalize(ESP_ERR_TIMEOUT);
    /* Best-effort: wait briefly for finalize to mark phase + signal. */
    if (s_commission_sem) {
        (void)xSemaphoreTake(s_commission_sem, pdMS_TO_TICKS(2000));
    }
    read_commission_phase(&phase, &result);
    ESP_LOGW(TAG, "commission wait -> %s", esp_err_to_name(result));
    return result == ESP_OK ? ESP_ERR_TIMEOUT : result;
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

/* Async failure sinks for a read issued by minihub_matter_read_onoff(): without
 * these the object just deletes itself and nothing is visible anywhere.
 *
 * on_connect_failure_cb_t gets the read_command as its context and
 * esp_matter deletes that object right after the call
 * (esp_matter_controller_read_command.cpp:58), so ctx is never dereferenced.
 * These run on the CHIP event task, like the commissioning callbacks that also
 * call set_error(). */
static void on_read_connect_failure(void *ctx, const chip::ScopedNodeId &peer_id, CHIP_ERROR error)
{
    (void)ctx;
    ESP_LOGW(TAG, "read: node %" PRIu64 " connect failed: %s", peer_id.GetNodeId(), chip::ErrorStr(error));
    char buf[96];
    snprintf(buf, sizeof(buf), "read connect failed: %s", chip::ErrorStr(error));
    set_error(buf);
}

static void on_read_error(uint64_t node_id, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "read: node %" PRIu64 " failed: %s", node_id, chip::ErrorStr(error));
    char buf[96];
    snprintf(buf, sizeof(buf), "attribute read failed: %s", chip::ErrorStr(error));
    set_error(buf);
}

esp_err_t minihub_matter_read_onoff(uint64_t node_id, uint16_t endpoint_id, bool *out_on)
{
    if (!out_on) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    /* The 11-arg single-path ctor is esp_matter_controller_read_command.h:75
     * (only fabric_filtered defaults); read_cb_done stays nullptr because the
     * value path is on_attr_report. */
    auto *cmd = chip::Platform::New<esp_matter::controller::read_command>(
        node_id, endpoint_id, k_onoff_cluster, k_onoff_attr,
        esp_matter::controller::READ_ATTRIBUTE, on_attr_report, nullptr, nullptr,
        on_read_connect_failure, on_read_error);
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }
    /* read_command owns itself (deleted in OnDone, on a connect failure at
     * esp_matter_controller_read_command.cpp:58, or inside send_command() itself
     * at :86) — never free cmd here and never touch it after send_command(): an
     * accepted send may already be a dangling pointer. send_command() returning
     * ESP_OK only means the request was handed to CHIP, so report it as
     * ESP_ERR_NOT_FINISHED: *out_on was NOT written, the value arrives through
     * on_attr_report. */
    esp_err_t serr = cmd->send_command();
    return serr == ESP_OK ? ESP_ERR_NOT_FINISHED : serr;
}

#define MATTER_SUB_MAX ((int)CONFIG_MINIHUB_MATTER_MAX_NODES)
#define MATTER_SUB_MIN_INTERVAL 5
#define MATTER_SUB_MAX_INTERVAL 30
/* One commissioning can carry up to k_pending_ep_max endpoints for a single
 * node, so the per-node endpoint list must not be smaller; MATTER_SUB_MAX
 * nodes is the real cap and is documented in the header. */
static constexpr size_t k_sub_ep_max = k_pending_ep_max;

typedef struct {
    uint64_t node_id; /* 0 = free row; commissioned node ids are >= 2 */
    uint16_t endpoints[k_sub_ep_max];
    size_t endpoint_count;
    bool live;        /* per node: the esp-matter callbacks are node-granular */
    uint32_t subscription_id;
    /* Boolean reports counted for this node row by note_sub_report(): cumulative
     * per row, cleared only when the row is freed (the drop_node() memset, or the
     * failed-fresh-arm one) before reallocation to another node — re-arming an
     * existing row preserves it. Not filtered by cluster, attribute or endpoint:
     * any decoded boolean report about the node bumps it. */
    uint32_t reports;
} matter_sub_slot_t;

static matter_sub_slot_t *s_subs = NULL;

/* A subscription that established after its row was already dropped. drop_node()
 * cannot tear down an arm that has not reported an id yet (subscription_id == 0),
 * so if that arm later succeeds nothing tracks it and it keeps writing points.
 * Those are parked here and shut down from the next subscribe call, which runs
 * outside the CHIP event task. Guarded by s_sub_mux. */
typedef struct {
    uint64_t node_id;
    uint32_t sub_id;
} matter_sub_orphan_t;
static matter_sub_orphan_t *s_sub_orphans = NULL;
static size_t s_sub_orphan_count;

static portMUX_TYPE s_tables_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_tables_inited = false;

/* Allocate the device/subscription/identity tables once from PSRAM at init.
 * They are never freed (the commissioner lives for the process), so there is
 * no use-after-free surface. Idempotent + double-checked so callers may call
 * it defensively; only the first call (from minihub_matter_init, off any lock)
 * actually allocates. */
static esp_err_t matter_ctrl_tables_ensure(void)
{
    if (s_tables_inited) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_tables_mux);
    if (s_tables_inited) {
        portEXIT_CRITICAL(&s_tables_mux);
        return ESP_OK;
    }
    const int slot_cap = IDENTITY_SLOT_MAX;
    const int sub_cap = MATTER_SUB_MAX;
    s_id_slots = (identity_slot_t *)heap_caps_calloc((size_t)slot_cap, sizeof(identity_slot_t),
                        MALLOC_CAP_SPIRAM);
    if (!s_id_slots) s_id_slots = (identity_slot_t *)calloc((size_t)slot_cap, sizeof(identity_slot_t));
    s_subs = (matter_sub_slot_t *)heap_caps_calloc((size_t)sub_cap, sizeof(matter_sub_slot_t),
                        MALLOC_CAP_SPIRAM);
    if (!s_subs) s_subs = (matter_sub_slot_t *)calloc((size_t)sub_cap, sizeof(matter_sub_slot_t));
    s_sub_orphans = (matter_sub_orphan_t *)heap_caps_calloc((size_t)sub_cap, sizeof(matter_sub_orphan_t),
                        MALLOC_CAP_SPIRAM);
    if (!s_sub_orphans) s_sub_orphans = (matter_sub_orphan_t *)calloc((size_t)sub_cap, sizeof(matter_sub_orphan_t));
    if (!s_id_slots || !s_subs || !s_sub_orphans) {
        ESP_LOGE(TAG, "matter ctrl tables alloc failed");
        heap_caps_free(s_id_slots); s_id_slots = NULL;
        heap_caps_free(s_subs); s_subs = NULL;
        heap_caps_free(s_sub_orphans); s_sub_orphans = NULL;
        s_tables_inited = true; /* do not retry under a lock */
        portEXIT_CRITICAL(&s_tables_mux);
        return ESP_ERR_NO_MEM;
    }
    s_tables_inited = true;
    portEXIT_CRITICAL(&s_tables_mux);
    return ESP_OK;
}

/* Readers run on the httpd task (status JSON), writers on the CHIP event task
 * (established/terminated) and on the subscribe_node caller. Same
 * reader/writer pairing as s_commission_mux, so the same protection. */
static portMUX_TYPE s_sub_mux = portMUX_INITIALIZER_UNLOCKED;

/* Caller must hold s_sub_mux. */
static int sub_slot(uint64_t node_id, bool alloc)
{
    matter_ctrl_tables_ensure();
    if (!s_subs) {
        return -1;
    }
    for (int i = 0; i < MATTER_SUB_MAX; i++) {
        if (s_subs[i].node_id == node_id) {
            return i;
        }
    }
    if (!alloc) {
        return -1;
    }
    for (int i = 0; i < MATTER_SUB_MAX; i++) {
        if (s_subs[i].node_id == 0) {
            s_subs[i].node_id = node_id;
            return i;
        }
    }
    ESP_LOGE(TAG, "subscription table full (cap %d): node%" PRIu64 " not tracked",
             MATTER_SUB_MAX, (unsigned long long)node_id);
    return -1;
}

static int64_t s_sub_nomatch_warn_ms; /* rate-limit, CHIP event task only */

/* Called from on_attr_report before forwarding to the sink. The row lookup and the
 * counter bump happen under the mux; the WARN is emitted outside it (logging must not
 * run in a critical section). Reports are counted per node whenever the node has a
 * tracked row; a (node, endpoint) that matches no armed row is the signal that config
 * and the device disagree — live can read true while reports stays 0 (nothing armed
 * matches what the device reports), which is exactly what the status counter exposes. */
static void note_sub_report(uint64_t node_id, uint16_t endpoint_id)
{
    if (!s_subs) {
        return;
    }
    bool have_row = false;
    bool matched = false;
    portENTER_CRITICAL(&s_sub_mux);
    for (int i = 0; i < MATTER_SUB_MAX; i++) {
        if (s_subs[i].node_id != node_id) {
            continue;
        }
        have_row = true;
        s_subs[i].reports++;
        for (size_t e = 0; e < s_subs[i].endpoint_count; e++) {
            if (s_subs[i].endpoints[e] == endpoint_id) {
                matched = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_sub_mux);
    if (matched) {
        return;
    }
    /* No armed row carries this endpoint (either no row for the node at all — e.g. an
     * on-demand read of an unsubscribed node — or a stale endpoint list after config
     * moved). Rate-limit so a chatty mismatch cannot flood the log. */
    (void)have_row;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_sub_nomatch_warn_ms >= 5000) {
        s_sub_nomatch_warn_ms = now;
        ESP_LOGW(TAG, "report node=%" PRIu64 " ep=%u matches no armed subscription", node_id,
                 (unsigned)endpoint_id);
    }
}

static void on_sub_established(uint64_t node_id, uint32_t subscription_id)
{
    if (!s_subs || !s_sub_orphans) {
        return;
    }
    bool tracked = false;
    portENTER_CRITICAL(&s_sub_mux);
    for (int i = 0; i < MATTER_SUB_MAX; i++) {
        if (s_subs[i].node_id == node_id) {
            s_subs[i].live = true;
            s_subs[i].subscription_id = subscription_id;
            tracked = true;
        }
    }
    if (!tracked) {
        /* The row was dropped while this arm was in flight. Queue it: it has no
         * owner otherwise, and it will keep delivering reports. */
        bool dup = false;
        for (size_t i = 0; i < s_sub_orphan_count; i++) {
            if (s_sub_orphans[i].node_id == node_id && s_sub_orphans[i].sub_id == subscription_id) {
                dup = true;
                break;
            }
        }
        if (!dup && s_sub_orphan_count < MATTER_SUB_MAX) {
            s_sub_orphans[s_sub_orphan_count].node_id = node_id;
            s_sub_orphans[s_sub_orphan_count].sub_id = subscription_id;
            s_sub_orphan_count++;
        }
    }
    portEXIT_CRITICAL(&s_sub_mux);
    if (tracked) {
        ESP_LOGI(TAG, "subscription up node=%" PRIu64 " id=0x%" PRIx32, node_id, subscription_id);
    } else {
        ESP_LOGW(TAG, "subscription up for untracked node=%" PRIu64 " id=0x%" PRIx32
                      " (row dropped mid-arm); queued for shutdown",
                 node_id, subscription_id);
    }
}

/* Runs on the caller's task (adapter poll/resync via subscribe_node), never on the
 * CHIP event task, so taking the stack lock here is acceptable. */
static void reap_sub_orphans(void)
{
    matter_sub_orphan_t *pending = (matter_sub_orphan_t *)heap_caps_calloc((size_t)MATTER_SUB_MAX,
                                    sizeof(matter_sub_orphan_t), MALLOC_CAP_SPIRAM);
    if (!pending) {
        pending = (matter_sub_orphan_t *)calloc((size_t)MATTER_SUB_MAX, sizeof(matter_sub_orphan_t));
    }
    if (!pending) {
        ESP_LOGE(TAG, "orphan reap alloc failed");
        return;
    }
    size_t n = 0;

    portENTER_CRITICAL(&s_sub_mux);
    n = s_sub_orphan_count;
    if (n > 0) {
        memcpy(pending, s_sub_orphans, sizeof(matter_sub_orphan_t) * n);
        s_sub_orphan_count = 0;
        memset(s_sub_orphans, 0, sizeof(matter_sub_orphan_t) * (size_t)MATTER_SUB_MAX);
    }
    portEXIT_CRITICAL(&s_sub_mux);
    if (n == 0) {
        heap_caps_free(pending);
        return;
    }

    /* Runs on the caller's task (adapter poll/resync via subscribe_node), never on
     * the CHIP event task, so taking the stack lock here is acceptable. */
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = esp_matter::controller::send_shutdown_subscription(pending[i].node_id,
                                                                          pending[i].sub_id);
        ESP_LOGW(TAG, "reaped orphan subscription node=%" PRIu64 " id=0x%" PRIx32 ": %s",
                 pending[i].node_id, pending[i].sub_id, esp_err_to_name(err));
    }
    heap_caps_free(pending);
}

static void on_sub_terminated(uint64_t node_id, uint32_t subscription_id)
{
    if (!s_subs) {
        return;
    }
    portENTER_CRITICAL(&s_sub_mux);
    for (int i = 0; i < MATTER_SUB_MAX; i++) {
        if (s_subs[i].node_id == node_id && s_subs[i].subscription_id == subscription_id) {
            s_subs[i].live = false;
            s_subs[i].subscription_id = 0;
        }
    }
    portEXIT_CRITICAL(&s_sub_mux);
    ESP_LOGW(TAG, "subscription down node=%" PRIu64 " id=0x%" PRIx32, node_id, subscription_id);
}

/* esp-matter deletes the subscribe_command right after this callback
 * (esp_matter_controller_subscribe_command.cpp:53-63), so ctx is never
 * dereferenced — same discipline as on_read_connect_failure. */
static void on_subscribe_connect_failure(void *ctx, const chip::ScopedNodeId &peer_id, CHIP_ERROR error)
{
    (void)ctx;
    ESP_LOGW(TAG, "subscribe: node %" PRIu64 " connect failed: %s", peer_id.GetNodeId(), chip::ErrorStr(error));
    char buf[96];
    snprintf(buf, sizeof(buf), "subscribe connect failed: %s", chip::ErrorStr(error));
    set_error(buf);
}

esp_err_t minihub_matter_subscribe_node(uint64_t node_id, const uint16_t *endpoint_ids, size_t count)
{
    if (!endpoint_ids || count == 0 || count > k_sub_ep_max) {
        set_error("subscribe: bad endpoint list");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || node_id < 2) {
        set_error("subscribe: commissioner not ready");
        return ESP_ERR_INVALID_STATE;
    }
    /* Opportunistic: shut down any subscription that established after its row was
     * dropped. Returns without touching the stack lock when there is nothing queued. */
    reap_sub_orphans();

    /* Claim (or refresh) the node's row under the mux, but send outside it:
     * portENTER_CRITICAL is a spinlock and the CHIP stack lock may block. */
    matter_sub_slot_t prior = {};
    int idx;
    bool fresh;
    portENTER_CRITICAL(&s_sub_mux);
    idx = sub_slot(node_id, false);
    fresh = (idx < 0);
    if (fresh) {
        idx = sub_slot(node_id, true);
    } else {
        prior = s_subs[idx];
    }
    if (idx < 0) {
        portEXIT_CRITICAL(&s_sub_mux);
        set_error("subscribe: table full"); /* the poll path will not re-arm this node */
        return ESP_ERR_NO_MEM;
    }
    if (s_subs[idx].live) {
        const uint32_t live_id = s_subs[idx].subscription_id;
        portEXIT_CRITICAL(&s_sub_mux);
        /* Silent today: the adapter only logs a NOT_FINISHED arm, so a short-circuit
         * here is indistinguishable from "never attempted" in the console log. */
        ESP_LOGI(TAG, "subscribe: node %" PRIu64 " already live (id=0x%" PRIx32 "), not re-arming",
                 node_id, live_id);
        return ESP_OK;
    }
    memcpy(s_subs[idx].endpoints, endpoint_ids, count * sizeof(uint16_t));
    s_subs[idx].endpoint_count = count;
    s_subs[idx].live = false;
    s_subs[idx].subscription_id = 0;
    portEXIT_CRITICAL(&s_sub_mux);

    esp_err_t err;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> attr_paths;
        chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> event_paths;
        if (!attr_paths.Alloc(count)) {
            err = ESP_ERR_NO_MEM;
            set_error("subscribe: attribute path alloc failed");
        } else {
            for (size_t i = 0; i < count; i++) {
                attr_paths[i] = chip::app::AttributePathParams(endpoint_ids[i], k_onoff_cluster, k_onoff_attr);
            }
            /* subscribe_command deletes itself in OnDone (auto_resubscribe re-arms
             * first) and on every synchronous failure path
             * (esp_matter_controller_subscribe_command.cpp:47-49, :53-63, :88). */
            auto *cmd = chip::Platform::New<esp_matter::controller::subscribe_command>(
                node_id, std::move(attr_paths), std::move(event_paths),
                MATTER_SUB_MIN_INTERVAL, MATTER_SUB_MAX_INTERVAL, true /* auto_resubscribe */,
                on_attr_report, nullptr, on_sub_established, on_sub_terminated,
                on_subscribe_connect_failure, true /* keep_subscription */);
            if (!cmd) {
                err = ESP_ERR_NO_MEM;
                set_error("subscribe: command alloc failed");
            } else {
                /* send_command() returning ESP_OK only means the request was handed
                 * to CHIP, and the object may already have deleted itself on a
                 * synchronous failure path — do not touch cmd afterwards. */
                esp_err_t serr = cmd->send_command();
                if (serr == ESP_OK) {
                    return ESP_ERR_NOT_FINISHED;
                }
                err = serr;
                set_error("subscribe request failed");
            }
        }
    }

    /* Synchronous failure: roll the row back (fresh → free the slot, existing
     * → restore its prior list) so failed attempts cannot fill the table.
     * Asynchronous silent failures (self-delete after connect) leave the row
     * not-live, which is exactly what the poll path re-arms on. */
    portENTER_CRITICAL(&s_sub_mux);
    if (fresh) {
        memset(&s_subs[idx], 0, sizeof(s_subs[idx]));
    } else {
        s_subs[idx] = prior;
    }
    portEXIT_CRITICAL(&s_sub_mux);
    return err;
}

/* Tear down a node's subscription and free its row(s). A re-commissioned device gets
 * a NEW node id; rows keyed by the old id are otherwise never reclaimed, so the old
 * endpoint list stays armed forever while /api/matter/status still reads healthy. This
 * is the truthful drop the adapter's subscribe_commissioned() calls before re-arming.
 *
 * Order per the ruling: send the real esp-matter teardown first, then free the table
 * row. send_shutdown_subscription() needs the stack lock (a blocking mutex) so it runs
 * OUTSIDE s_sub_mux; the subscription_id is captured under the mux first. Only one
 * caller (subscribe_commissioned, under the adapter lifecycle lock) runs at a time, so
 * no concurrent re-arm slips in during the teardown window. */
esp_err_t minihub_matter_drop_node(uint64_t node_id)
{
    if (node_id < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_subs) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t sub_id = 0;
    portENTER_CRITICAL(&s_sub_mux);
    for (int i = 0; i < MATTER_SUB_MAX; i++) {
        if (s_subs[i].node_id == node_id && s_subs[i].subscription_id != 0) {
            sub_id = s_subs[i].subscription_id;
        }
    }
    portEXIT_CRITICAL(&s_sub_mux);

    if (sub_id != 0) {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        esp_err_t terr = esp_matter::controller::send_shutdown_subscription(node_id, sub_id);
        if (terr != ESP_OK) {
            ESP_LOGW(TAG, "drop node=%" PRIu64 " shutdown sub=0x%" PRIx32 " failed: %s", node_id, sub_id,
                     esp_err_to_name(terr));
        }
    }

    bool found = false;
    portENTER_CRITICAL(&s_sub_mux);
    for (int i = 0; i < MATTER_SUB_MAX; i++) {
        if (s_subs[i].node_id == node_id) {
            memset(&s_subs[i], 0, sizeof(s_subs[i]));
            found = true;
        }
    }
    portEXIT_CRITICAL(&s_sub_mux);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "dropped subscription row node=%" PRIu64 " (sub=0x%" PRIx32 ")", node_id, sub_id);
    return ESP_OK;
}

cJSON *minihub_matter_subscriptions_json(void)
{
    /* Snapshot under the mux, build cJSON outside it: cJSON_* allocates and
     * must not run inside a critical section. */
    matter_sub_slot_t *snap = (matter_sub_slot_t *)heap_caps_calloc((size_t)MATTER_SUB_MAX,
                                sizeof(matter_sub_slot_t), MALLOC_CAP_SPIRAM);
    if (!snap) {
        snap = (matter_sub_slot_t *)calloc((size_t)MATTER_SUB_MAX, sizeof(matter_sub_slot_t));
    }
    if (!snap) {
        ESP_LOGE(TAG, "sub snapshot alloc failed");
        return NULL;
    }
    if (!s_subs) {
        heap_caps_free(snap);
        return NULL;
    }
    portENTER_CRITICAL(&s_sub_mux);
    memcpy(snap, s_subs, sizeof(matter_sub_slot_t) * (size_t)MATTER_SUB_MAX);
    portEXIT_CRITICAL(&s_sub_mux);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        heap_caps_free(snap);
        return NULL;
    }
    for (int i = 0; i < MATTER_SUB_MAX; i++) {
        if (snap[i].node_id == 0) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddNumberToObject(item, "node_id", (double)snap[i].node_id);
        cJSON *eps = cJSON_CreateArray();
        if (eps) {
            for (size_t e = 0; e < snap[i].endpoint_count; e++) {
                cJSON_AddItemToArray(eps, cJSON_CreateNumber((double)snap[i].endpoints[e]));
            }
            cJSON_AddItemToObject(item, "endpoints", eps);
        }
        cJSON_AddBoolToObject(item, "live", snap[i].live);
        cJSON_AddNumberToObject(item, "reports", (double)snap[i].reports);
        cJSON_AddItemToArray(arr, item);
    }
    heap_caps_free(snap);
    return arr;
}

cJSON *minihub_matter_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    int phase = COMMISSION_PHASE_IDLE;
    esp_err_t result = ESP_OK;
    read_commission_phase(&phase, &result);
    const char *commission = "idle";
    /* s_persist_busy only covers the post-success config write. Once the phase
     * has left WAITING (including a cancel), the header must stop offering
     * 取消入网 — otherwise a second click has nothing to abort and returns
     * ESP_ERR_NOT_FOUND while the button stays on screen. */
    if (phase == COMMISSION_PHASE_WAITING) {
        commission = "running";
    } else if (phase == COMMISSION_PHASE_DONE) {
        commission = (result == ESP_OK) ? "ok" : "failed";
    }

    cJSON_AddBoolToObject(root, "ready", s_started);
    cJSON_AddBoolToObject(root, "commissioned", s_commissioned);
    cJSON_AddBoolToObject(root, "pairing", s_pairing_in_progress);
    cJSON_AddStringToObject(root, "mode", "commissioner");
    cJSON_AddStringToObject(root, "commission", commission);
    if (s_commission_stage != COMMISSION_STAGE_NONE) {
        cJSON_AddStringToObject(root, "commission_stage", commission_stage_name(s_commission_stage));
    }
    if (s_commission_node_id >= 2) {
        cJSON_AddNumberToObject(root, "commission_node_id", (double)s_commission_node_id);
    }
    if (phase == COMMISSION_PHASE_DONE && result != ESP_OK) {
        cJSON_AddStringToObject(root, "commission_result", esp_err_to_name(result));
    }
#if CONFIG_MINIHUB_THREAD_ADAPTER
    cJSON_AddBoolToObject(root, "thread_br_ready", minihub_thread_ready());
#else
    cJSON_AddBoolToObject(root, "thread_br_ready", false);
#endif
#if CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION
    cJSON_AddBoolToObject(root, "ble_thread", true);
#else
    cJSON_AddBoolToObject(root, "ble_thread", false);
#endif
    /* snapshot under the commission mux so we never read a half-written buffer. */
    char le[sizeof(s_last_error)];
    bool has_err = false;
    portENTER_CRITICAL(&s_commission_mux);
    if (s_last_error[0]) {
        strncpy(le, s_last_error, sizeof(le) - 1);
        le[sizeof(le) - 1] = '\0';
        has_err = true;
    }
    portEXIT_CRITICAL(&s_commission_mux);
    if (has_err) {
        cJSON_AddStringToObject(root, "last_error", le);
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

    cJSON *subs = minihub_matter_subscriptions_json();
    if (subs) {
        cJSON_AddItemToObject(root, "subscriptions", subs);
    }
    return root;
}

const char *minihub_matter_last_error(void)
{
    /* read under the commission mux so we never hand back a torn buffer. */
    portENTER_CRITICAL(&s_commission_mux);
    const char *p = s_last_error[0] ? s_last_error : NULL;
    portEXIT_CRITICAL(&s_commission_mux);
    return p;
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
    /* matter_hub (lf_core_chip_read_attribute) takes the stack lock before
     * GetConnectedDevice. send_command() arms AddressResolve, which
     * CancelTimer()s on this thread. SystemLayerImplFreeRTOS asserts the
     * caller holds PlatformMgr's lock and chipDie()s otherwise.
     * From a CHIP callback the scoped lock is ALREADY_TAKEN and does not
     * unlock on the way out. */
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
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
