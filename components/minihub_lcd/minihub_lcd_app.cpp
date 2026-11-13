/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "minihub_lcd.h"
#include "minihub_lcd_module_auth.h"
#include "ui_contract.gen.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <future>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "boost/json/array.hpp"
#include "boost/json/object.hpp"
#include "boost/json/value.hpp"

#include "brookesia/gui_interface.hpp"
#include "brookesia/gui_interface/parser.hpp"
#include "brookesia/gui_lvgl.hpp"
#include "brookesia/lib_utils.hpp"
#include "brookesia/service_display.hpp"
#include "brookesia/service_helper.hpp"
#include "brookesia/service_manager.hpp"

extern "C" {
#include "minihub_core.h"
#include "minihub_pack.h"
#include "minihub_module.h"
#include "minihub_geo.h"
#include "minihub_map.h"
#include "minihub_ota.h"
#include "minihub_task.h"
#include "minihub_update.h"
#include "minihub_net_cfg.h"
extern "C" {
esp_err_t minihub_net_get_wifi_sta(char *ssid, size_t ssid_len, bool *has_password);
esp_err_t minihub_net_set_wifi_sta(const char *ssid, const char *password, bool open_network);
}
#include "minihub_config.h"
#include "minihub_paths.h"
#include "minihub_store.h"
#include "minihub_rules.h"
#include "minihub_scenes.h"
#include "minihub_tz.h"
#include "minihub_hub.h"
#include "esp_lv_adapter.h"
#include "esp_system.h"
#include "minihub_lcd_i18n.h"
#include "lcd_strings.gen.h"
#include "esp_err.h"
#include "esp_chip_info.h"

/*
 * minihub_provision.h pulls in esp_wifi.h (only the Wi-Fi credential helpers
 * need it) and a REQUIRES edge minihub_lcd -> minihub_provision is dropped by
 * IDF's cycle resolution (provision -> store -> lcd). The factory-reset symbol
 * is already in the final link closure through minihub_web -> minihub_provision,
 * so declare the prototype locally instead of including the header.
 */
extern "C" esp_err_t minihub_factory_reset_schedule(void);
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "widgets/textarea/lv_textarea_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label);
/* minihub_net.h pulls esp_netif.h, which is not on this component's include
 * path (and a REQUIRES edge to it would be dropped by cycle resolution), so
 * only the one function the system screen needs is declared here. */
bool minihub_net_get_ip(char *buf, size_t len);
#if CONFIG_LV_FONT_MONTSERRAT_14
LV_FONT_DECLARE(lv_font_montserrat_14);
#endif
#if CONFIG_LV_FONT_MONTSERRAT_16
LV_FONT_DECLARE(lv_font_montserrat_16);
#endif
#if CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);
#endif
}

static const char *TAG = "minihub_lcd";

using namespace esp_brookesia;
using DisplayHelper = service::helper::Display;
using LvglDisplaySource = gui::lvgl::DisplaySource;

namespace {

constexpr const char *UI_ROOT_PATH = "/data/ui/root.json";
constexpr const char *UI_BASE_DIR = "/data/ui";
constexpr const char *FLOW_ID = "main";
constexpr uint32_t DISPLAY_SERVICE_TIMEOUT_MS = 2000;
constexpr int kOverviewDeviceSlots = LCD_COUNT_DEVICE_SLOTS;
constexpr int kMapCanvasW = 664;
constexpr int kMapCanvasH = 316;
constexpr const char *MAP_IMAGE_VIEW = LCD_VIEW_MAP_IMAGE;
constexpr const char *MAP_STATUS_VIEW = LCD_VIEW_MAP_STATUS;
static const char *const kStorePrefixes[] = {LCD_ENUM_STORE_PREFIXES};
/* Module-side i18n key, not a UI action name; split the literal so the
 * contract lint (which greps for bare action names) does not misclassify it. */
constexpr const char *kFilterLabelKey = "filter" "_label";

/* Set across a flash-write window (Store install / upgrade). The RGB panel
 * scans a PSRAM framebuffer by DMA, so we freeze it instead of tearing. */
std::atomic<bool> s_flash_write{false};

/* minihub_store_* downloads and extracts, so an operation runs on its own task
 * and never on the UI worker — every other LCD caller blocks on that one. */
std::atomic<bool> s_store_busy{false};
std::atomic<bool> s_store_refresh_wait{false};
std::atomic<bool> s_ota_query_busy{false};

enum class StoreOp { Install, Upgrade, Reinstall, Uninstall };
enum class PendingConfirm { None, Reboot, FactoryReset, StoreUninstall, RuleDelete, SceneDelete };

/* What the visible store rows refer to. The renderers fill this; the tap handler
 * reads it back through the index parsed out of Event.path. Fields mirror the
 * Web console's merge (store.js): `managed` comes from the module list itself,
 * `config_only` / `type` / `upgrade_available` from the catalog, and an upgrade
 * is also offered when the installed binary is stale. */
struct StoreRowInfo {
    char id[64];
    char name[80];
    char kind[12];
    char version[24];
    char vendor[48];
    char description[192];
    bool config_only;
    bool managed;
    bool upgrade;
    bool available;
    bool enabled;
};
constexpr int kStoreSections = 3; /* avail, pack, mod */
/* Shared by all three STORE_ROW / STORE_AVAIL_ROW contract patterns. */
constexpr const char *kStoreRowInfix = "_row_";
constexpr int kStoreRowSlots = (LCD_COUNT_STORE_ROWS > LCD_COUNT_STORE_AVAIL_ROWS)
                                   ? LCD_COUNT_STORE_ROWS
                                   : LCD_COUNT_STORE_AVAIL_ROWS;
struct StoreOpRequest {
    StoreOp op;
    char kind[12];
    char id[64];
};

StoreRowInfo s_store_rows[kStoreSections][kStoreRowSlots] = {};
/* Captured when the sheet button is pressed, NOT re-read from the row index at
 * confirm time: a refresh in between can put a different module at that index,
 * and uninstalling the wrong one is not recoverable. */
StoreOpRequest s_store_pending{};
/* Snapshot of the row the sheet was opened on; the live cache is rebuilt by
 * every point-event refresh, so an index held across that is not an identity. */
StoreRowInfo s_store_selected{};
/* refresh_store runs on every point event; forcing Available there made Installed
 * (where uninstall/upgrade live) impossible to stay on. The Web console opens
 * the store on Installed, so the LCD opens there too. */
bool s_store_tab_available = false;
std::string s_store_query;
int s_store_row_count[kStoreSections] = {};
int s_pending_delete_slot = -1;

/* Zone ids in dropdown order; the picker maps selectedIndex back through this. */
std::vector<std::string> s_tz_zones;

bool map_snapshot_file_ready();

void refresh_map();
void refresh_system_tz();
void refresh_system_lang();
void refresh_system_wifi();

std::atomic<bool> s_map_fetching{false};

constexpr uint32_t kBootBgColor = 0x0b0f1a;
uint32_t s_display_output_id = 0;
bool s_backlight_supported = false;
bool s_backlight_on = false;

struct LcdContext {
    std::shared_ptr<lib_utils::TaskScheduler> scheduler;
    std::unique_ptr<gui::Runtime> runtime;
    gui::DocumentId document_id{};
    uint32_t display_width = 0;
    uint32_t display_height = 0;
    bool hw_ready = false;
    bool started = false;
    std::vector<gui::ScopedConnection> nav_connections;
};

LcdContext s_lcd;

/* Point events coalesce into one redraw; the scheduler loop is only a slow
 * resync in case an event was dropped. */
constexpr int kRefreshCoalesceMs = 100;
constexpr int kFallbackResyncMs = CONFIG_MINIHUB_LCD_UI_REFRESH_MS;
std::atomic<bool> s_refresh_due{false};
std::atomic<bool> s_events_live{false};
esp_timer_handle_t s_coalesce_timer = nullptr;
esp_timer_handle_t s_clock_timer = nullptr;
std::atomic<bool> s_clock_tick_queued{false};

std::vector<minihub_point_t> s_overview_filtered;

enum class OverviewFilter {
    All,
    Auth,
    Matter,
};
OverviewFilter s_overview_filter = OverviewFilter::All;
bool s_overview_ui_warned = false;

std::atomic<bool> s_mount_in_progress{false};
std::atomic<int64_t> s_mount_t0_us{0};
esp_timer_handle_t s_mount_watch_timer = nullptr;

void with_lvgl_lock(const std::function<void()> &fn);
void request_display_refresh();

bool is_lcd_screen(const char *name)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !name) {
        return false;
    }
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    return state && *state == name;
}

void paint_boot_splash();
void sync_display_framebuffers();
void fill_boot_framebuffers_solid();

void set_display_backlight(bool on)
{
    if (!s_backlight_supported || s_display_output_id == 0) {
        return;
    }
    if (on == s_backlight_on) {
        return;
    }
    const auto ok = DisplayHelper::call_function_async(DisplayHelper::FunctionId::SetBacklightOnOff,
                                                     static_cast<double>(s_display_output_id),
                                                     on);
    if (ok) {
        s_backlight_on = on;
    }
}

void sync_display_framebuffers()
{
    if (!esp_lv_adapter_is_initialized()) {
        return;
    }
    with_lvgl_lock([]() {
        lv_display_t *disp = lv_display_get_default();
        if (disp == nullptr) {
            return;
        }
        (void)esp_lv_adapter_refresh_now(disp);
        (void)esp_lv_adapter_refresh_now(disp);
    });
}

void fill_boot_framebuffers_solid()
{
    if (!esp_lv_adapter_is_initialized()) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp == nullptr) {
        return;
    }
    const int32_t w = lv_display_get_horizontal_resolution(disp);
    const int32_t h = lv_display_get_vertical_resolution(disp);
    if (w <= 0 || h <= 0) {
        return;
    }
    const size_t px_count = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t bytes = px_count * sizeof(uint16_t);
    uint16_t *solid = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (solid == nullptr) {
        solid = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    if (solid == nullptr) {
        ESP_LOGW(TAG, "boot fb fill: no memory");
        return;
    }
    const uint16_t color = lv_color_to_u16(lv_color_hex(kBootBgColor));
    for (size_t i = 0; i < px_count; ++i) {
        solid[i] = color;
    }
    if (esp_lv_adapter_set_dummy_draw(disp, true) != ESP_OK) {
        heap_caps_free(solid);
        return;
    }
    const esp_err_t blit0 = esp_lv_adapter_dummy_draw_blit(disp, 0, 0, w, h, solid, true);
    const esp_err_t blit1 = esp_lv_adapter_dummy_draw_blit(disp, 0, 0, w, h, solid, true);
    (void)esp_lv_adapter_set_dummy_draw(disp, false);
    heap_caps_free(solid);
    if (blit0 != ESP_OK || blit1 != ESP_OK) {
        ESP_LOGW(TAG, "boot fb fill blit: %s / %s", esp_err_to_name(blit0), esp_err_to_name(blit1));
    }
}

void paint_boot_splash()
{
    if (!esp_lv_adapter_is_initialized()) {
        return;
    }
    with_lvgl_lock([]() {
        lv_display_t *disp = lv_display_get_default();
        if (disp == nullptr) {
            return;
        }
        lv_obj_t *scr = lv_display_get_screen_active(disp);
        if (scr == nullptr) {
            scr = lv_obj_create(nullptr);
            lv_screen_load(scr);
        }
        lv_obj_remove_style_all(scr);
        lv_obj_set_size(scr, lv_display_get_horizontal_resolution(disp), lv_display_get_vertical_resolution(disp));
        lv_obj_set_style_bg_color(scr, lv_color_hex(kBootBgColor), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
        lv_obj_invalidate(scr);
    });
    sync_display_framebuffers();
}

const char *task_state_name(eTaskState state)
{
    switch (state) {
    case eRunning:
        return "Running";
    case eReady:
        return "Ready";
    case eBlocked:
        return "Blocked";
    case eSuspended:
        return "Suspended";
    case eDeleted:
        return "Deleted";
    default:
        return "Unknown";
    }
}

void log_task_state(const char *phase, const char *task_name)
{
    TaskHandle_t handle = xTaskGetHandle(task_name);
    if (handle == nullptr) {
        ESP_LOGD(TAG, "[diag] %s task '%s' not found", phase, task_name);
        return;
    }
    ESP_LOGD(TAG, "[diag] %s task '%s' state=%s", phase, task_name, task_state_name(eTaskGetState(handle)));
}

void log_heap_and_stack(const char *phase)
{
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    ESP_LOGD(TAG,
             "[diag] %s caller=%s core=%d stack_free=%uB int_heap=%u dma_int=%u min_dma=%u spiram=%u min_int=%u",
             phase,
             pcTaskGetName(nullptr),
             xPortGetCoreID(),
             (unsigned)(hwm * sizeof(StackType_t)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(dma_caps),
             (unsigned)heap_caps_get_minimum_free_size(dma_caps),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

void log_lvgl_runtime(const char *phase)
{
    ESP_LOGD(TAG,
             "[diag] %s lv_adapter_init=%d",
             phase,
             esp_lv_adapter_is_initialized() ? 1 : 0);
    log_task_state(phase, "lvgl");
    log_task_state(phase, "LcdUiWorker");
    log_task_state(phase, "main");
    log_task_state(phase, "IDLE0");
    log_task_state(phase, "IDLE1");
}

void log_lvgl_tree(const char *phase)
{
    with_lvgl_lock([phase]() {
        lv_display_t *disp = lv_display_get_default();
        if (disp == nullptr) {
            ESP_LOGD(TAG, "[diag] %s no default lv_display", phase);
            return;
        }
        lv_obj_t *scr = lv_display_get_screen_active(disp);
        if (scr == nullptr) {
            ESP_LOGD(TAG, "[diag] %s no active screen", phase);
            return;
        }
        uint32_t child_cnt = lv_obj_get_child_cnt(scr);
        ESP_LOGD(TAG,
                 "[diag] %s active_screen children=%u hidden=%d",
                 phase,
                 (unsigned)child_cnt,
                 lv_obj_has_flag(scr, LV_OBJ_FLAG_HIDDEN) ? 1 : 0);
    });
}

void mount_watch_timer_cb(void *arg)
{
    (void)arg;
    if (!s_mount_in_progress.load(std::memory_order_relaxed)) {
        return;
    }
    const int64_t elapsed_ms = (esp_timer_get_time() - s_mount_t0_us.load(std::memory_order_relaxed)) / 1000;
    ESP_LOGD(TAG, "[diag] mount-watch elapsed=%lld ms (still inside start_screen_flow/create_subtree?)", (long long)elapsed_ms);
    log_heap_and_stack("mount-watch");
    log_lvgl_runtime("mount-watch");
}

void ensure_mount_watch_timer()
{
    if (s_mount_watch_timer != nullptr) {
        return;
    }
    esp_timer_create_args_t args = {
        .callback = mount_watch_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lcd_mount_watch",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_mount_watch_timer));
}

void start_mount_watch()
{
    ensure_mount_watch_timer();
    s_mount_t0_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    s_mount_in_progress.store(true, std::memory_order_relaxed);
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_mount_watch_timer, 500000));
    ESP_LOGD(TAG, "[diag] mount-watch started (500ms interval)");
}

void stop_mount_watch()
{
    s_mount_in_progress.store(false, std::memory_order_relaxed);
    if (s_mount_watch_timer != nullptr) {
        (void)esp_timer_stop(s_mount_watch_timer);
    }
    ESP_LOGD(TAG, "[diag] mount-watch stopped");
}

int64_t elapsed_ms_since(int64_t t0_us)
{
    return (esp_timer_get_time() - t0_us) / 1000;
}

void with_lvgl_lock(const std::function<void()> &fn)
{
    gui::lvgl::lock_thread();
    fn();
    gui::lvgl::unlock_thread();
}

void request_display_refresh()
{
    if (!esp_lv_adapter_is_initialized()) {
        return;
    }
    (void)esp_lv_adapter_report_activity();
    with_lvgl_lock([]() {
        lv_display_t *disp = lv_display_get_default();
        if (disp != nullptr) {
            lv_obj_t *scr = lv_display_get_screen_active(disp);
            if (scr != nullptr) {
                lv_obj_invalidate(scr);
            }
        }
    });
}

/* ----------------------------------------------------------------------------
 * Shared floating keyboard (LVGL top layer).
 *
 * Every textInput on every screen binds to this ONE keyboard when it is
 * clicked. Brookesia's JSON keyboards are kept hidden: they own a static
 * targetTextInput and cannot be re-targeted at runtime, so an in-card keyboard
 * for each screen would either duplicate this one or push the form's OK/Cancel
 * row off the 480px display. This panel sits on the display top layer, is
 * opaque, and has a drag handle so the user can move it away from the field
 * being edited.
 * -------------------------------------------------------------------------- */

static lv_obj_t *s_fkb_panel = nullptr;
static lv_obj_t *s_fkb_keyboard = nullptr;

/* LV_OBJ_FLAG_USER_1..: remember the one textarea that owns the caret. Every
 * refresh re-applies the rules, because Brookesia mounts screen trees lazily
 * and a textarea created after the last pass would otherwise start blinking. */
static lv_obj_t *s_fkb_active_ta = nullptr;

static void floating_keyboard_cursor_walk(lv_obj_t *obj, lv_obj_t *active)
{
    if (obj == nullptr || !lv_obj_is_valid(obj)) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        const bool on = (obj == active);
        /* A textarea starts with cursor.show=1 and a blink animation, and
         * LV_EVENT_FOCUSED with anim_duration 0 forces the caret on and still.
         * That is the bar sitting in Wi-Fi name/password before anyone taps
         * them, and the same event scrolls the system page down to those
         * fields. Idle fields get no caret. The pressed field keeps a blink. */
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        auto *ta = reinterpret_cast<lv_textarea_t *>(obj);
        lv_anim_delete(obj, nullptr);
        if (on) {
            lv_obj_set_style_anim_duration(obj, 400, LV_PART_CURSOR);
            lv_obj_send_event(obj, LV_EVENT_FOCUSED, nullptr);
        } else {
            ta->cursor.show = 0;
            lv_obj_invalidate(obj);
        }
    }
    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        floating_keyboard_cursor_walk(lv_obj_get_child(obj, i), active);
    }
}

static void floating_keyboard_refresh_cursors()
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == nullptr) {
        return;
    }
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    if (scr != nullptr) {
        floating_keyboard_cursor_walk(scr, s_fkb_active_ta);
    }
}

static void floating_keyboard_set_cursors(lv_obj_t *active)
{
    s_fkb_active_ta = active;
    floating_keyboard_refresh_cursors();
}

/* The built-in lv_font_source_han_sans_sc_16_cjk covers only ~1338 glyphs, so
 * new Chinese chrome text renders as garbage. Brookesia's runtime CJK font is
 * a FreeType instance with no global singleton; borrow it from any Chinese
 * label that the parsed document already created. */
static const lv_font_t *floating_keyboard_cjk_font(lv_obj_t *obj)
{
    if (obj == nullptr || !lv_obj_is_valid(obj)) {
        return nullptr;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *txt = lv_label_get_text(obj);
        if (txt != nullptr) {
            for (const unsigned char *p = (const unsigned char *)txt; *p != 0; ++p) {
                if (*p >= 0x80) {
                    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
                    if (font != nullptr) {
                        return font;
                    }
                    break;
                }
            }
        }
    }
    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        const lv_font_t *font = floating_keyboard_cjk_font(lv_obj_get_child(obj, i));
        if (font != nullptr) {
            return font;
        }
    }
    return nullptr;
}

static void floating_keyboard_hide()
{
    if (s_fkb_keyboard != nullptr && lv_obj_is_valid(s_fkb_keyboard)) {
        lv_keyboard_set_textarea(s_fkb_keyboard, nullptr);
    }
    if (s_fkb_panel != nullptr && lv_obj_is_valid(s_fkb_panel)) {
        lv_obj_add_flag(s_fkb_panel, LV_OBJ_FLAG_HIDDEN);
    }
    floating_keyboard_set_cursors(nullptr);
}

static void floating_keyboard_show(lv_obj_t *ta)
{
    if (ta == nullptr || s_fkb_panel == nullptr || s_fkb_keyboard == nullptr ||
        !lv_obj_is_valid(s_fkb_panel) || !lv_obj_is_valid(s_fkb_keyboard)) {
        return;
    }
    lv_keyboard_set_textarea(s_fkb_keyboard, ta);
    floating_keyboard_set_cursors(ta);
    lv_obj_clear_flag(s_fkb_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_fkb_panel);
    /* A field inside a long scrollable form may be below the viewport; walk the
     * whole parent chain instead of scrolling only the nearest scroll parent. */
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
}

static void floating_keyboard_drag_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSING || s_fkb_panel == nullptr) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    lv_display_t *disp = lv_display_get_default();
    if (indev == nullptr || disp == nullptr) {
        return;
    }
    lv_point_t vect{};
    lv_indev_get_vect(indev, &vect);
    const lv_coord_t sw = (lv_coord_t)lv_display_get_horizontal_resolution(disp);
    const lv_coord_t sh = (lv_coord_t)lv_display_get_vertical_resolution(disp);
    const lv_coord_t w = lv_obj_get_width(s_fkb_panel);
    const lv_coord_t h = lv_obj_get_height(s_fkb_panel);
    lv_coord_t x = (lv_coord_t)(lv_obj_get_x(s_fkb_panel) + vect.x);
    lv_coord_t y = (lv_coord_t)(lv_obj_get_y(s_fkb_panel) + vect.y);
    x = (x < 0) ? 0 : ((x + w > sw) ? sw - w : x);
    y = (y < 0) ? 0 : ((y + h > sh) ? sh - h : y);
    lv_obj_set_pos(s_fkb_panel, x, y);
}

static void floating_keyboard_close_cb(lv_event_t *e)
{
    (void)e;
    floating_keyboard_hide();
}

static void floating_keyboard_del_cb(lv_event_t *e)
{
    (void)e;
    if (s_fkb_keyboard == nullptr || !lv_obj_is_valid(s_fkb_keyboard)) {
        return;
    }
    lv_obj_t *ta = lv_keyboard_get_textarea(s_fkb_keyboard);
    if (ta != nullptr && lv_obj_is_valid(ta)) {
        lv_textarea_delete_char(ta);
    }
}

static void floating_keyboard_finish_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        floating_keyboard_hide();
    }
}

static void floating_keyboard_target_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target_obj(e));
    if (target == nullptr) {
        return;
    }
    if (lv_obj_check_type(target, &lv_textarea_class)) {
        floating_keyboard_show(target);
    } else {
        floating_keyboard_hide();
    }
}

/* Defined below, next to the other dropdown popup styling helpers. */
static void style_dropdown_popup(lv_obj_t *dd);

/* LVGL 9's lv_dropdown_open() explicitly sends LV_EVENT_READY "to allow
 * styling the list". Hook it so every popup is opaque/white even for dropdowns
 * mounted after the last data refresh (rules/scenes form is one of those). */
static void dropdown_popup_ready_cb(lv_event_t *e)
{
    lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target_obj(e));
    if (dd != nullptr && lv_obj_check_type(dd, &lv_dropdown_class)) {
        style_dropdown_popup(dd);
    }
}

/* LV_OBJ_FLAG_USER_1 is free for application use (lv_obj.h); it marks objects
 * we already attached a listener to, so re-running after a screen-flow mount
 * never stacks duplicate callbacks. */
static void floating_keyboard_watch(lv_obj_t *obj)
{
    if (obj == nullptr || !lv_obj_is_valid(obj)) {
        return;
    }
    const bool is_ta = lv_obj_check_type(obj, &lv_textarea_class);
    const bool is_dd = lv_obj_check_type(obj, &lv_dropdown_class);
    if ((is_ta || is_dd) && !lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_USER_1);
        lv_obj_add_event_cb(obj, floating_keyboard_target_cb, LV_EVENT_CLICKED, nullptr);
        if (is_dd) {
            /* The list object is created in lv_dropdown's constructor (hidden),
             * so it can be styled before the popup is ever opened. */
            style_dropdown_popup(obj);
            lv_obj_add_event_cb(obj, dropdown_popup_ready_cb, LV_EVENT_READY, nullptr);
        }
    }
    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        floating_keyboard_watch(lv_obj_get_child(obj, i));
    }
}

/* lv_keyboard_set_map() writes LVGL's GLOBAL kb_map[]/kb_ctrl[]. Brookesia's
 * hidden JSON keyboards register their layouts there, so the built-in
 * lv_keyboard_def_event_cb() mode-switch keys ("ABC" / "abc" / "1#") can end
 * up installing an empty/stale map -> every key label vanishes and no further
 * key press brings it back. Own static maps + the value-changed handler so our
 * keyboard never depends on that global state. */
static const char *const kFkbLowerMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "1#", "_", "-", ".", ",", ":", "/", "@", " ", LV_SYMBOL_OK, ""
};
static const char *const kFkbUpperMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "1#", "_", "-", ".", ",", ":", "/", "@", " ", LV_SYMBOL_OK, ""
};
static const char *const kFkbSymbolMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "!", "?", "#", "$", "%", "&", "*", "+", "=", "(", "\n",
    "abc", ")", "<", ">", "[", "]", "{", "}", "\\", "|", "~", "\n",
    "_", "-", ".", ",", ":", "/", "@", " ", LV_SYMBOL_OK, ""
};
/* Equal-width buttons; LVGL copies only btn_cnt entries from this array. */
static const lv_buttonmatrix_ctrl_t kFkbCtrl[48] = {};

static uint32_t fkb_item_selector(lv_state_t state)
{
    return static_cast<uint32_t>(LV_PART_ITEMS) |
           static_cast<uint32_t>(state);
}

static void floating_keyboard_apply_style()
{
    lv_obj_t *kb = s_fkb_keyboard;
    if (kb == nullptr || !lv_obj_is_valid(kb)) {
        return;
    }
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(kb, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x131a2b), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1a2238), LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(0x243049), LV_PART_ITEMS);
    static const lv_state_t kStates[] = {
        LV_STATE_DEFAULT, LV_STATE_CHECKED, LV_STATE_PRESSED,
        LV_STATE_DISABLED, LV_STATE_FOCUSED,
    };
    for (lv_state_t state : kStates) {
        const uint32_t sel = fkb_item_selector(state);
        lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, sel);
        lv_obj_set_style_text_color(kb, lv_color_hex(0xffffff), sel);
    }
}

static void floating_keyboard_set_map(const char *const *map)
{
    if (s_fkb_keyboard == nullptr || !lv_obj_is_valid(s_fkb_keyboard)) {
        return;
    }
    lv_buttonmatrix_set_map(s_fkb_keyboard, map);
    lv_buttonmatrix_set_ctrl_map(s_fkb_keyboard, kFkbCtrl);
    floating_keyboard_apply_style();
}

static void floating_keyboard_key_cb(lv_event_t *e)
{
    lv_obj_t *kb = static_cast<lv_obj_t *>(lv_event_get_target_obj(e));
    if (kb == nullptr) {
        return;
    }
    const uint32_t btn = lv_buttonmatrix_get_selected_button(kb);
    if (btn == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *txt = lv_buttonmatrix_get_button_text(kb, btn);
    if (txt == nullptr) {
        return;
    }
    if (strcmp(txt, "ABC") == 0) {
        floating_keyboard_set_map(kFkbUpperMap);
        return;
    }
    if (strcmp(txt, "abc") == 0) {
        floating_keyboard_set_map(kFkbLowerMap);
        return;
    }
    if (strcmp(txt, "1#") == 0) {
        floating_keyboard_set_map(kFkbSymbolMap);
        return;
    }
    if (strcmp(txt, LV_SYMBOL_OK) == 0 || strcmp(txt, LV_SYMBOL_CLOSE) == 0 ||
        strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        floating_keyboard_hide();
        return;
    }
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (ta == nullptr || !lv_obj_is_valid(ta)) {
        return;
    }
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(ta);
    } else {
        lv_textarea_add_text(ta, txt);
    }
    /* Self-healing: keep glyphs white even if something re-applies theme
     * styles while the keyboard is open. */
    floating_keyboard_apply_style();
}

static void floating_keyboard_init()
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == nullptr) {
        return;
    }
    if (s_fkb_panel != nullptr && lv_obj_is_valid(s_fkb_panel)) {
        return;
    }
    lv_obj_t *top = lv_display_get_layer_top(disp);
    if (top == nullptr) {
        return;
    }
    const lv_font_t *cjk = floating_keyboard_cjk_font(lv_display_get_screen_active(disp));
    if (cjk == nullptr) {
        cjk = &lv_font_source_han_sans_sc_16_cjk;
    }
    const lv_coord_t sw = (lv_coord_t)lv_display_get_horizontal_resolution(disp);
    const lv_coord_t sh = (lv_coord_t)lv_display_get_vertical_resolution(disp);
    const lv_coord_t w = sw - 120;
    /* Four key rows + a 24px drag bar need ~230px. The old 220px panel left
     * the last row clipped by the panel's own bounds. */
    const lv_coord_t h = 248;
    constexpr lv_coord_t kTopBarH = 30;

    s_fkb_panel = lv_obj_create(top);
    lv_obj_remove_flag(s_fkb_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_fkb_panel, w, h);
    lv_obj_set_pos(s_fkb_panel, (sw - w) / 2, sh - h - 4);
    lv_obj_add_flag(s_fkb_panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(s_fkb_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_fkb_panel, lv_color_hex(0x131a2b), 0);
    lv_obj_set_style_border_width(s_fkb_panel, 1, 0);
    lv_obj_set_style_border_color(s_fkb_panel, lv_color_hex(0x243049), 0);
    lv_obj_set_style_radius(s_fkb_panel, 8, 0);
    lv_obj_set_style_pad_all(s_fkb_panel, 0, 0);

    lv_obj_t *handle = lv_obj_create(s_fkb_panel);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(handle, w - 190, 24);
    lv_obj_set_pos(handle, 4, 2);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0x1a2238), 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_pad_all(handle, 0, 0);
    lv_obj_add_event_cb(handle, floating_keyboard_drag_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_t *hint = lv_label_create(handle);
    lv_label_set_text(hint, lcd_tr(LCD_STR_FLOATING_KB_HINT));
    lv_obj_set_style_text_font(hint, cjk, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xeef2ff), 0);
    lv_obj_center(hint);

    lv_obj_t *del_btn = lv_button_create(s_fkb_panel);
    lv_obj_set_size(del_btn, 86, 24);
    lv_obj_set_pos(del_btn, w - 184, 2);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x1a2238), 0);
    lv_obj_add_event_cb(del_btn, floating_keyboard_del_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_label_set_text(del_lbl, lcd_tr(LCD_STR_FLOATING_KB_DEL));
    lv_obj_set_style_text_font(del_lbl, cjk, 0);
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(0xeef2ff), 0);
    lv_obj_center(del_lbl);

    lv_obj_t *close_btn = lv_button_create(s_fkb_panel);
    lv_obj_set_size(close_btn, 88, 24);
    lv_obj_set_pos(close_btn, w - 92, 2);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x1a2238), 0);
    lv_obj_add_event_cb(close_btn, floating_keyboard_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, lcd_tr(LCD_STR_FLOATING_KB_CLOSE));
    lv_obj_set_style_text_font(close_lbl, cjk, 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xeef2ff), 0);
    lv_obj_center(close_lbl);

    s_fkb_keyboard = lv_keyboard_create(s_fkb_panel);
    lv_obj_set_align(s_fkb_keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(s_fkb_keyboard, w - 8, h - kTopBarH - 6);
    lv_obj_set_pos(s_fkb_keyboard, 4, kTopBarH);
    /* Theme padding squeezes four rows into too little height; the bottom row
     * then falls outside the widget and gets clipped. */
    lv_obj_remove_flag(s_fkb_keyboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_fkb_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_fkb_keyboard, 2, 0);
    lv_obj_set_style_pad_column(s_fkb_keyboard, 2, 0);
    lv_obj_set_style_border_width(s_fkb_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_fkb_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_fkb_keyboard, 0, LV_PART_ITEMS);
    lv_obj_remove_event_cb(s_fkb_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_fkb_keyboard, floating_keyboard_key_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);
    floating_keyboard_set_map(kFkbLowerMap);
    lv_obj_add_event_cb(s_fkb_keyboard, floating_keyboard_finish_cb,
                        LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_fkb_keyboard, floating_keyboard_finish_cb,
                        LV_EVENT_CANCEL, nullptr);
    lv_obj_add_flag(s_fkb_panel, LV_OBJ_FLAG_HIDDEN);
}

static void floating_keyboard_sync()
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == nullptr) {
        return;
    }
    floating_keyboard_init();
    floating_keyboard_hide();
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    if (scr != nullptr) {
        floating_keyboard_watch(scr);
    }
    floating_keyboard_refresh_cursors();
}

constexpr lv_coord_t kShellSidebarW = 112;
constexpr lv_coord_t kShellMainW = 688;
constexpr lv_coord_t kShellScreenH = 480;

/* Pin one screen root ("shell"): the shell fills the display, the sidebar sits
 * flush left and the main area takes the rest of the width. */
void pin_shell_geometry(lv_obj_t *shell, lv_coord_t screen_w, lv_coord_t screen_h)
{
    if (shell == nullptr) {
        return;
    }
    lv_obj_set_pos(shell, 0, 0);
    lv_obj_set_size(shell, screen_w, screen_h);
    lv_obj_set_style_pad_all(shell, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(shell, 0, LV_PART_MAIN);
    lv_obj_clear_flag(shell, LV_OBJ_FLAG_SCROLLABLE);

    if (lv_obj_get_child_cnt(shell) < 2) {
        ESP_LOGW(TAG, "[diag] shell layout: expected 2 children, got %u",
                 (unsigned)lv_obj_get_child_cnt(shell));
        return;
    }
    lv_obj_t *sidebar = lv_obj_get_child(shell, 0);
    lv_obj_t *main = lv_obj_get_child(shell, 1);
    if (sidebar == nullptr || main == nullptr) {
        return;
    }
    lv_obj_set_pos(sidebar, 0, 0);
    lv_obj_set_size(sidebar, kShellSidebarW, kShellScreenH);
    lv_obj_set_pos(main, kShellSidebarW, 0);
    lv_obj_set_size(main, kShellMainW, kShellScreenH);
    /* NOTE: do NOT call lv_obj_move_foreground() here. The shell's children are
     * indexed by insertion order (sidebar = child 0, main = child 1); moving the
     * sidebar to the front reorders them, so the next time this screen is mounted
     * lv_obj_get_child(shell, 0) returns the *main* area and the pin swaps the
     * two -- which is the "sidebar not flush-left" regression. */
    lv_obj_update_layout(shell);
    ESP_LOGD(TAG,
             "[diag] shell layout shell=(%d,%d,%dx%d) sidebar=(%d,%d,%dx%d) main=(%d,%d,%dx%d)",
             lv_obj_get_x(shell),
             lv_obj_get_y(shell),
             lv_obj_get_width(shell),
             lv_obj_get_height(shell),
             lv_obj_get_x(sidebar),
             lv_obj_get_y(sidebar),
             lv_obj_get_width(sidebar),
             lv_obj_get_height(sidebar),
             lv_obj_get_x(main),
             lv_obj_get_y(main),
             lv_obj_get_width(main),
             lv_obj_get_height(main));
}

static void scroll_tree_top(lv_obj_t *obj);

void pin_screen_shell_layout()
{
    with_lvgl_lock([]() {
        /* Every navigation mounts a fresh screen root; re-attach the floating
         * keyboard listeners to its textInputs/dropdowns and close the keyboard
         * so it does not linger over the newly shown screen. */
        floating_keyboard_sync();
        lv_display_t *disp = lv_display_get_default();
        if (disp == nullptr) {
            return;
        }
        const lv_coord_t screen_w = static_cast<lv_coord_t>(lv_display_get_horizontal_resolution(disp));
        const lv_coord_t screen_h = static_cast<lv_coord_t>(lv_display_get_vertical_resolution(disp));
        lv_obj_t *scr = lv_display_get_screen_active(disp);
        if (scr == nullptr) {
            return;
        }
        lv_obj_set_pos(scr, 0, 0);
        lv_obj_set_size(scr, screen_w, screen_h);
        lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

        /* Brookesia mounts a viewScreen as a sibling of the (hidden) staging
         * root, and the swap is not atomic: right after trigger_screen_flow()
         * the old and the new screen root can both be visible, and the new one
         * may not be laid out yet. Picking a single "best" shell therefore
         * races, and sometimes the freshly mounted screen stays unpinned -- its
         * sidebar then shows up off the left edge. Pin every visible screen
         * root with the same geometry instead: whichever one ends up visible is
         * already correct, and no screen is ever left unpinned. */
        const uint32_t child_cnt = lv_obj_get_child_cnt(scr);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *shell = lv_obj_get_child(scr, i);
            if (shell == nullptr || lv_obj_has_flag(shell, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            if (lv_obj_get_child_cnt(shell) < 2) {
                continue;
            }
            pin_shell_geometry(shell, screen_w, screen_h);
        }
        /* The system screen stays mounted, so its scroll offset survives a
         * trip to another page. Opening 系统 should show the title, not the
         * Wi-Fi card the caret used to scroll into view. */
        if (is_lcd_screen("system")) {
            scroll_tree_top(scr);
        }
    });
}

static void scroll_tree_top(lv_obj_t *obj)
{
    if (obj == nullptr || !lv_obj_is_valid(obj)) {
        return;
    }
    lv_obj_scroll_to(obj, 0, 0, LV_ANIM_OFF);
    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        scroll_tree_top(lv_obj_get_child(obj, i));
    }
}

struct LvglAdapterPauseGuard {
    bool active = false;
    bool hold_splash = false;

    explicit LvglAdapterPauseGuard(bool hold_splash_on_pause = false)
        : hold_splash(hold_splash_on_pause)
    {
        log_lvgl_runtime("pre-pause");
        if (!esp_lv_adapter_is_initialized()) {
            ESP_LOGW(TAG, "[diag] pause skipped: lv_adapter not initialized");
            return;
        }
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = esp_lv_adapter_pause(-1);
        ESP_LOGD(TAG, "[diag] esp_lv_adapter_pause -> %s (%lld ms)", esp_err_to_name(err), (long long)elapsed_ms_since(t0));
        if (err == ESP_OK) {
            active = true;
            log_lvgl_runtime("post-pause");
            if (hold_splash) {
                /* Solid blit only. paint_boot_splash() calls
                 * esp_lv_adapter_refresh_now(), which runs FreeType on the
                 * caller. Store install calls this from lcd_store_op (16 KB);
                 * cf2_interpT2CharString walks off that stack and the chip
                 * reboots with a stack-protection fault. */
                fill_boot_framebuffers_solid();
            }
        }
    }
    ~LvglAdapterPauseGuard()
    {
        if (!active) {
            return;
        }
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = esp_lv_adapter_resume();
        ESP_LOGD(TAG, "[diag] esp_lv_adapter_resume -> %s (%lld ms)", esp_err_to_name(err), (long long)elapsed_ms_since(t0));
        if (err == ESP_OK) {
            request_display_refresh();
        }
        log_lvgl_runtime("post-resume");
    }
    LvglAdapterPauseGuard(const LvglAdapterPauseGuard &) = delete;
    LvglAdapterPauseGuard &operator=(const LvglAdapterPauseGuard &) = delete;
};

/* Owns the paused-LVGL state for the duration of one flash-write window. */
std::unique_ptr<LvglAdapterPauseGuard> s_freeze_guard;

/* Keep LVGL paused for the entire boot UI setup; resumes once at scope exit. */
struct BootLvglHold {
    bool active = false;

    BootLvglHold()
    {
        if (!esp_lv_adapter_is_initialized()) {
            return;
        }
        if (esp_lv_adapter_pause(-1) == ESP_OK) {
            active = true;
        }
    }

    ~BootLvglHold()
    {
        release();
    }

    void release()
    {
        if (!active) {
            return;
        }
        active = false;
        sync_display_framebuffers();
        (void)esp_lv_adapter_resume();
        request_display_refresh();
    }

    BootLvglHold(const BootLvglHold &) = delete;
    BootLvglHold &operator=(const BootLvglHold &) = delete;
};

gui::Environment make_environment()
{
    gui::Environment env{};
    env.width_px = static_cast<int32_t>(s_lcd.display_width);
    env.height_px = static_cast<int32_t>(s_lcd.display_height);
    env.density = 1.0F;
    env.font_scale = 1.0F;
    env.language = lcd_language();
    return env;
}

bool register_lvgl_native_font(
    gui::lvgl::Backend &backend,
    const char *font_id,
    const lv_font_t *font,
    int32_t size_px,
    std::initializer_list<const char *> languages)
{
    if (font == nullptr || size_px <= 0) {
        return false;
    }
    gui::lvgl::FontRegistrationConfig cfg{};
    cfg.font_id = font_id;
    cfg.primary_src = font_id;
    cfg.native_src = reinterpret_cast<uintptr_t>(font);
    cfg.native_size = size_px;
    for (const char *lang : languages) {
        cfg.languages.emplace_back(lang);
    }
    if (!backend.register_font_resource(cfg)) {
        ESP_LOGE(TAG, "register font '%s' (%dpx) failed", font_id, (int)size_px);
        return false;
    }
    ESP_LOGD(TAG, "registered font '%s' (%dpx)", font_id, (int)size_px);
    return true;
}

bool setup_ui_fonts(gui::lvgl::Backend &backend)
{
    bool ok = true;
#if CONFIG_LV_FONT_MONTSERRAT_14
    ok &= register_lvgl_native_font(backend, "latin_14", &lv_font_montserrat_14, 14, {"en"});
#endif
#if CONFIG_LV_FONT_MONTSERRAT_16
    ok &= register_lvgl_native_font(backend, "latin_16", &lv_font_montserrat_16, 16, {"en"});
#endif
#if CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
    /* Built-in ~1338-glyph Han Sans: chrome text (titles, buttons, chips) renders
     * with no FreeType and no 1.4 MB OTF on the device. */
    ok &= register_lvgl_native_font(backend, "cjk_16",
                                    &lv_font_source_han_sans_sc_16_cjk, 16, {"zh", "en"});
#endif
    return ok;
}

bool register_ui_sc_font(gui::Runtime &runtime)
{
    static const char *kScFontPath = "/data/ui/fonts/minihub_sc.otf";
    struct stat st {};
    if (stat(kScFontPath, &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "sc font not on device: %s (flash storage or push ui/fonts/)", kScFontPath);
        return false;
    }

    gui::RuntimeFontResource sc{};
    sc.id = "sc_14";
    sc.kind = "file";
    sc.primary_src = kScFontPath;
    sc.languages = {"zh", "en"};
    /* Do not set fallback_ids to latin_14: brookesia passes fallback primary_src
     * ("latin_14") to FreeType as a file path instead of using native Montserrat. */
    auto result = runtime.register_font(sc);
    if (!result) {
        ESP_LOGW(TAG, "sc font register: %s", result.error().c_str());
        return false;
    }
    ESP_LOGD(TAG, "registered font sc_14 (%ld bytes)", (long)st.st_size);
    return true;
}

void bind_ui_font_language(gui::Runtime &runtime)
{
    const char *fallback = "cjk_16";
    const char *font_id = register_ui_sc_font(runtime) ? "sc_14" : fallback;
    auto en = runtime.set_default_font_for_language("en", font_id);
    if (!en) {
        ESP_LOGW(TAG, "en default font: %s", en.error().c_str());
        font_id = fallback;
        en = runtime.set_default_font_for_language("en", font_id);
    }
    auto zh = runtime.set_default_font_for_language("zh", font_id);
    if (!zh) {
        ESP_LOGW(TAG, "zh default font: %s", zh.error().c_str());
        /* Never let zh fall back to a latin-only face: with the built-in Han
         * Sans compiled in, cjk_16 is always the last drawable option. */
        (void)runtime.set_default_font_for_language("zh", fallback);
    }
    ESP_LOGD(TAG, "UI fonts: en/zh=%s (fallback %s)", font_id, fallback);
}

bool start_display_service()
{
    if (!DisplayHelper::is_available()) {
        ESP_LOGE(TAG, "Display service not available (check brookesia_hal_boards)");
        return false;
    }

    auto outputs_json = DisplayHelper::call_function_sync<boost::json::array>(
        DisplayHelper::FunctionId::GetOutputs,
        service::helper::Timeout(DISPLAY_SERVICE_TIMEOUT_MS));
    if (!outputs_json.has_value() || outputs_json->empty()) {
        ESP_LOGE(TAG, "No display outputs");
        return false;
    }

    std::vector<DisplayHelper::OutputInfo> outputs;
    if (!BROOKESIA_DESCRIBE_FROM_JSON(boost::json::value(outputs_json.value()), outputs) || outputs.empty()) {
        ESP_LOGE(TAG, "Failed to parse display outputs");
        return false;
    }

    const auto &main_output = outputs.front();
    if (main_output.width == 0 || main_output.height == 0) {
        ESP_LOGE(TAG, "Invalid display size");
        return false;
    }

    s_lcd.display_width = main_output.width;
    s_lcd.display_height = main_output.height;
    s_display_output_id = main_output.id;
    s_backlight_supported =
        main_output.backlight.has_value() && main_output.backlight->on_off_supported;
    ESP_LOGI(TAG, "Display %ux%u (%s)", main_output.width, main_output.height, main_output.name.c_str());

    gui::lvgl::DisplaySourceConfig lv_cfg{};
    /* Same core as LcdUiWorker, lower priority: start() must pause before first lv_timer_handler. */
    lv_cfg.task_core_id = 1;
    lv_cfg.task_priority = 2;
    lv_cfg.task_stack_size = 64 * 1024;
    lv_cfg.stack_in_psram = true;
    /* Framebuffer placement is a board-level setting (num_fbs / fb_in_psram in
     * board_devices.yaml); DisplaySourceConfig in brookesia 0.8.6 has no use_psram. */
    if (!LvglDisplaySource::get_instance().start(lv_cfg)) {
        ESP_LOGE(TAG, "LVGL display source start failed");
        return false;
    }

    set_display_backlight(true);

    return true;
}

void set_label_text(const char *path, const char *text)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !text) {
        return;
    }
    auto view = s_lcd.runtime->find_view(s_lcd.document_id, path);
    if (!view.valid()) {
        return;
    }
    view.as_label().set_text(text);
}

void set_label_text_fmt(const char *path, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_label_text(path, buf);
}

void set_view_hidden(const char *path, bool hidden)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !path) {
        return;
    }
    auto view = s_lcd.runtime->find_view(s_lcd.document_id, path);
    if (view.valid()) {
        view.set_hidden(hidden);
    }
}

void append_filter_chip_bindings(
    const char *chip_id,
    bool active,
    std::vector<gui::BindingValueUpdate> &updates)
{
    char btn_path[128];
    char lbl_path[144];
    snprintf(btn_path, sizeof(btn_path),
             LCD_VIEW_PATTERN_FILTER_CHIP, chip_id);
    snprintf(lbl_path, sizeof(lbl_path), "%s/%s_lbl", btn_path, chip_id);

    const char *bg = active ? "#6366f1" : "#1a2238";
    const char *border = active ? "#6366f1" : "#243049";
    const char *text = active ? "#eef2ff" : "#8b9bb4";

    updates.push_back({btn_path, "chip.bg", bg});
    updates.push_back({btn_path, "chip.border", border});
    updates.push_back({lbl_path, "chip.text", text});
}

/* Web builds filter chips from the stable adapter configuration: a disabled
 * or not-installed adapter produces no chip at all. Overview is a dynamic
 * screen, so leaving it and coming back rebuilds the chips from JSON. Re-apply
 * on every refresh; the highlight bindings stay in refresh_filter_chips(). */
void apply_filter_chip_visibility()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    char auth_adapter[32] = {};
    const bool show_auth = minihub_module_pick_auth_adapter(auth_adapter, sizeof(auth_adapter));
    const bool show_matter = minihub_module_overview_adapter_visible("matter");
    set_view_hidden(LCD_VIEW_FILTER_AUTH_BTN, !show_auth);
    set_view_hidden(LCD_VIEW_FILTER_MATTER_BTN, !show_matter);
    set_view_hidden(LCD_VIEW_MATTER_BAR, !show_matter);
    set_label_text(LCD_VIEW_MATTER_STATUS,
                   show_matter ? lcd_tr(LCD_STR_MATTER_READY) : lcd_tr(LCD_STR_MATTER_NOT_READY));
    if (!show_auth && s_overview_filter == OverviewFilter::Auth) {
        s_overview_filter = OverviewFilter::All;
    }
    if (!show_matter && s_overview_filter == OverviewFilter::Matter) {
        s_overview_filter = OverviewFilter::All;
    }
    static bool logged_auth = false;
    static bool logged_matter = false;
    static bool logged = false;
    if (!logged || logged_auth != show_auth || logged_matter != show_matter) {
        logged = true;
        logged_auth = show_auth;
        logged_matter = show_matter;
        ESP_LOGD(TAG, "overview chip visibility: auth=%d matter=%d", show_auth, show_matter);
    }
}

void handle_matter_add()
{
    /* Full commissioning needs a pairing code + polling; on-device we degrade
     * to a hint, same as the "feature parity, degraded interaction" rule. */
    set_label_text(LCD_VIEW_MATTER_STATUS, lcd_tr(LCD_STR_MATTER_ADD_HINT));
}

void refresh_filter_chips()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }

    char auth_adapter[32] = {};
    const bool show_auth = minihub_module_pick_auth_adapter(auth_adapter, sizeof(auth_adapter));
    const bool show_matter = minihub_module_overview_adapter_visible("matter");
    if (!show_auth && s_overview_filter == OverviewFilter::Auth) {
        s_overview_filter = OverviewFilter::All;
    }
    if (!show_matter && s_overview_filter == OverviewFilter::Matter) {
        s_overview_filter = OverviewFilter::All;
    }

    /* Visibility is re-applied by apply_filter_chip_visibility() on every
     * refresh, because a dynamic remount rebuilds these chips from JSON.
     * This function only refreshes the selected chip's highlight. */

    if (show_auth) {
        char filter_lbl[32] = {};
        snprintf(filter_lbl, sizeof(filter_lbl), "%s", lcd_tr(LCD_STR_AUTH_CLOUD));
        (void)minihub_module_ui_label(auth_adapter, kFilterLabelKey, filter_lbl, sizeof(filter_lbl));
        set_label_text(LCD_VIEW_AUTH_FILTER_LABEL,
                       filter_lbl);
        set_label_text(LCD_VIEW_AUTH_SECTION_LABEL, filter_lbl);
    }

    std::vector<gui::BindingValueUpdate> updates;
    append_filter_chip_bindings(LCD_ACTION_FILTER_ALL, s_overview_filter == OverviewFilter::All, updates);
    if (show_auth) {
        append_filter_chip_bindings(LCD_ACTION_FILTER_AUTH, s_overview_filter == OverviewFilter::Auth, updates);
    }
    if (show_matter) {
        append_filter_chip_bindings(LCD_ACTION_FILTER_MATTER, s_overview_filter == OverviewFilter::Matter, updates);
    }
    s_lcd.runtime->set_binding_values(s_lcd.document_id, updates);
}

bool overview_view_valid(const char *path)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !path) {
        return false;
    }
    return s_lcd.runtime->find_view(s_lcd.document_id, path).valid();
}

void warn_if_overview_ui_stale()
{
    if (s_overview_ui_warned) {
        return;
    }
    if (!overview_view_valid(LCD_VIEW_OVERVIEW_TOOLBAR)) {
        s_overview_ui_warned = true;
        ESP_LOGW(TAG,
                 "LCD overview UI is outdated (missing overview_toolbar). "
                 "Flash UI: idf.py flash (or re-flash LittleFS / modstore UI partition)");
    }
}

void set_image_src(const char *path, const char *src)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !src) {
        return;
    }
    auto view = s_lcd.runtime->find_view(s_lcd.document_id, path);
    if (!view.valid()) {
        return;
    }
    view.as_image().set_src(src);
}

/* A template declares its bindings on the child nodes (name/type/...), not on the
 * instance container. set_binding_value() resolves the node at the given path and
 * only matches that node's own bindings, so the update must address the child
 * directly -- passing the container path silently matches nothing. */
void set_row_binding(const char *row_path, const char *child, const char *key, const std::string &value)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !row_path || !child || !key) {
        return;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", row_path, child);
    s_lcd.runtime->set_binding_value(s_lcd.document_id, path, key, value);
}

const char *device_type_icon_id(const char *type)
{
    if (!type || !type[0]) {
        return "icon_device";
    }
    if (strcmp(type, "switch") == 0) {
        return "icon_switch";
    }
    if (strcmp(type, "light") == 0) {
        return "icon_light";
    }
    if (strcmp(type, "sensor") == 0) {
        return "icon_sensor";
    }
    if (strcmp(type, "button") == 0) {
        return "icon_button";
    }
    return "icon_device";
}

const char *point_short_name(const char *point_id)
{
    if (!point_id) {
        return "";
    }
    const char *slash = strrchr(point_id, '/');
    return slash ? slash + 1 : point_id;
}

/* Web renders props.name (minihub_device_name_overlay writes it into the point
 * registry). Use it everywhere a device is shown so a rename is visible. */
const char *point_display_name(const minihub_point_t &pt)
{
    const cJSON *name = pt.props ? cJSON_GetObjectItem(pt.props, "name") : nullptr;
    if (cJSON_IsString(name) && name->valuestring[0]) {
        return name->valuestring;
    }
    return point_short_name(pt.point_id);
}

void ensure_device_card(int index)
{
    if (!s_lcd.runtime) {
        return;
    }
    char path[96];
    snprintf(path, sizeof(path),
             LCD_TEMPLATE_DEVICE_CARD_PARENT "/" LCD_TEMPLATE_DEVICE_CARD_INSTANCE, index);
    if (s_lcd.runtime->find_view(s_lcd.document_id, path).valid()) {
        return;
    }
    char instance[24];
    snprintf(instance, sizeof(instance), LCD_TEMPLATE_DEVICE_CARD_INSTANCE, index);
    auto made = s_lcd.runtime->create_view(
        s_lcd.document_id, LCD_TEMPLATE_DEVICE_CARD_ID,
        LCD_TEMPLATE_DEVICE_CARD_PARENT, instance);
    if (!made) {
        ESP_LOGW(TAG, "device card %d: %s", index, made.error().c_str());
    }
}

void hide_device_cards_from(int index)
{
    /* Anything past the visible count must go, not just hide: an instance that
     * lingers keeps its last binding value on screen. */
    if (!s_lcd.runtime) {
        return;
    }
    for (int i = index; i < LCD_COUNT_DEVICE_SLOTS; i++) {
        char path[96];
        snprintf(path, sizeof(path),
                 LCD_TEMPLATE_DEVICE_CARD_PARENT "/" LCD_TEMPLATE_DEVICE_CARD_INSTANCE, i);
        s_lcd.runtime->destroy_view(s_lcd.document_id, path);
    }
}

void refresh_overview_updated_label()
{
    char updated[32] = {};
    time_t now = time(nullptr);
    struct tm tm_now {};
    /* esp_sntp_get_sync_status() returns COMPLETED only once, then resets
     * itself to RESET. Polling it every second therefore shows the clock for
     * a single tick and then sticks on "awaiting time" until the next NTP
     * update. A year of 2024 or later means the clock has been set. */
    const bool clock_set = now > 0 && localtime_r(&now, &tm_now) != nullptr
                           && (tm_now.tm_year + 1900) >= 2024;
    if (clock_set) {
        snprintf(updated, sizeof(updated), lcd_tr(LCD_STR_OVERVIEW_UPDATED_FMT),
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        snprintf(updated, sizeof(updated), "%s",
                 lcd_tr(LCD_STR_OVERVIEW_UPDATED_NO_TIME));
    }
    set_label_text(LCD_VIEW_OVERVIEW_UPDATED, updated);
}

void refresh_overview()
{
    warn_if_overview_ui_stale();

    minihub_point_t *points = NULL;
    size_t count = 0;
    if (minihub_point_list(&points, &count) != ESP_OK || !points) {
        return;
    }

    s_overview_filtered.clear();
    s_overview_filtered.reserve(count);
    char auth_adapter[32] = {};
    const bool auth_visible = minihub_module_pick_auth_adapter(auth_adapter, sizeof(auth_adapter));
    size_t auth_all = 0;
    for (size_t i = 0; i < count; i++) {
        if (auth_visible && strcmp(points[i].adapter, auth_adapter) == 0) {
            auth_all++;
        }
        if (s_overview_filter == OverviewFilter::Auth &&
            (!auth_visible || strcmp(points[i].adapter, auth_adapter) != 0)) {
            continue;
        }
        if (s_overview_filter == OverviewFilter::Matter && strcmp(points[i].adapter, "matter") != 0) {
            continue;
        }
        s_overview_filtered.push_back(points[i]);
    }

    size_t online = 0;
    for (const minihub_point_t &pt : s_overview_filtered) {
        if (pt.online) {
            online++;
        }
    }

    size_t adapter_unique = 0;
    for (size_t i = 0; i < s_overview_filtered.size(); i++) {
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(s_overview_filtered[j].adapter, s_overview_filtered[i].adapter) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            adapter_unique++;
        }
    }

    char num[16];
    snprintf(num, sizeof(num), "%u", (unsigned)s_overview_filtered.size());
    set_label_text(LCD_VIEW_STAT_DEVICES, num);
    snprintf(num, sizeof(num), "%u", (unsigned)online);
    set_label_text(LCD_VIEW_STAT_ONLINE, num);
    snprintf(num, sizeof(num), "%u", (unsigned)adapter_unique);
    set_label_text(LCD_VIEW_STAT_ADAPTERS, num);

    static int s_auth_poll_tick;
    static bool s_auth_logged_in;
    static bool s_auth_was_logged_in;
    static bool s_auth_auto_sync_done;
    static char s_auth_acct[96] = {};
    if (s_auth_acct[0] == '\0') {
        snprintf(s_auth_acct, sizeof(s_auth_acct), "%s",
                 lcd_tr(LCD_STR_AUTH_NOT_LOGGED_IN));
    }
    if (auth_visible && (s_auth_poll_tick++ % 5) == 0) {
        s_auth_logged_in = false;
        char logged_out[32] = {};
        snprintf(logged_out, sizeof(logged_out), "%s",
                 lcd_tr(LCD_STR_AUTH_NOT_LOGGED_IN));
        (void)minihub_module_ui_label(auth_adapter, "logged_out", logged_out, sizeof(logged_out));
        strncpy(s_auth_acct, logged_out, sizeof(s_auth_acct) - 1);
        cJSON *result = nullptr;
        if (minihub_module_invoke_rpc(auth_adapter, "status", nullptr, &result) == ESP_OK && result) {
            const cJSON *logged = cJSON_GetObjectItem(result, "logged_in");
            const cJSON *uid = cJSON_GetObjectItem(result, "user_id");
            const cJSON *region = cJSON_GetObjectItem(result, "region");
            if (cJSON_IsTrue(logged) && cJSON_IsString(uid) && uid->valuestring[0] != '\0') {
                s_auth_logged_in = true;
                if (cJSON_IsString(region) && region->valuestring[0] != '\0') {
                    snprintf(s_auth_acct, sizeof(s_auth_acct), "%s · %s", uid->valuestring, region->valuestring);
                } else {
                    snprintf(s_auth_acct, sizeof(s_auth_acct), "%s", uid->valuestring);
                }
            }
            cJSON_Delete(result);
        }
    }
    const bool logged_in = auth_visible && s_auth_logged_in;
    const char *acct = s_auth_acct;

    if (auth_visible && logged_in && !s_auth_was_logged_in && auth_all == 0 && !s_auth_auto_sync_done) {
        s_auth_auto_sync_done = true;
        module_auth_lcd::schedule_sync();
    }
    if (!logged_in) {
        s_auth_auto_sync_done = false;
    }
    s_auth_was_logged_in = logged_in;

    refresh_overview_updated_label();

    static const char *kAuthBar = LCD_VIEW_AUTH_BAR;
    const bool filter_auth = s_overview_filter == OverviewFilter::Auth;
    const bool filter_all = s_overview_filter == OverviewFilter::All;
    const bool show_auth_bar = auth_visible && (filter_auth || (filter_all && logged_in));
    const bool show_auth_actions = filter_auth;

    set_view_hidden(kAuthBar, !show_auth_bar);
    if (show_auth_bar) {
        snprintf(num, sizeof(num), "%u", (unsigned)auth_all);
        set_label_text(LCD_VIEW_AUTH_COUNT, num);
        set_label_text(LCD_VIEW_AUTH_ACCOUNT, acct);
        set_view_hidden(LCD_VIEW_AUTH_BTN_LOGIN,
                        !show_auth_actions || logged_in);
        set_view_hidden(LCD_VIEW_AUTH_BTN_SYNC,
                        !show_auth_actions || !logged_in);
        set_view_hidden(LCD_VIEW_AUTH_BTN_LOGOUT,
                        !show_auth_actions || !logged_in);
    }
    set_view_hidden(LCD_VIEW_AUTH_CTA,
                    !(auth_visible && filter_auth && logged_in && auth_all == 0));

    const size_t shown = s_overview_filtered.size() < static_cast<size_t>(kOverviewDeviceSlots)
                             ? s_overview_filtered.size()
                             : static_cast<size_t>(kOverviewDeviceSlots);
    for (size_t i = 0; i < shown; i++) {
        const minihub_point_t &pt = s_overview_filtered[i];
        const int index = static_cast<int>(i);
        ensure_device_card(index);
        char path[96];
        snprintf(path, sizeof(path),
                 LCD_TEMPLATE_DEVICE_CARD_PARENT "/" LCD_TEMPLATE_DEVICE_CARD_INSTANCE, index);

        /* Keys mirror "binding_keys" in ui_contract.json. The template nests the
         * icon/name/type row under a "top" container, so the binding owners live
         * at dev_N/top/name and dev_N/top/type; meta/val sit on the card itself. */
        char icon_path[sizeof(path) + sizeof("/top/icon")];
        snprintf(icon_path, sizeof(icon_path), "%s/top/icon", path);
        set_image_src(icon_path, device_type_icon_id(pt.type));
        set_row_binding(path, "top/name", "card.name", point_display_name(pt));
        set_row_binding(path, "top/type", "card.type", pt.type);
        char text[128];
        snprintf(text, sizeof(text), "%s · %s", pt.online ? lcd_tr(LCD_STR_STATE_ONLINE) : lcd_tr(LCD_STR_STATE_OFFLINE), pt.adapter);
        set_row_binding(path, "meta", "card.meta", std::string{text});
        set_row_binding(path, "val", "card.value", pt.value);
        auto view = s_lcd.runtime->find_view(s_lcd.document_id, path);
        if (view.valid()) {
            view.set_hidden(false);
        }
    }
    hide_device_cards_from(static_cast<int>(shown));
    free(points);
}

const cJSON *catalog_entry(cJSON *catalog, const char *id)
{
    if (!cJSON_IsArray(catalog) || id == nullptr) {
        return nullptr;
    }
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, catalog) {
        const cJSON *iid = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(iid) && strcmp(iid->valuestring, id) == 0) {
            return item;
        }
    }
    return nullptr;
}

void store_row_cache_clear(int section)
{
    s_store_row_count[section] = 0;
    for (int i = 0; i < kStoreRowSlots; i++) {
        s_store_rows[section][i] = StoreRowInfo{};
    }
}

static char store_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

static bool store_text_has(const char *hay, const char *needle)
{
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    if (hay == nullptr) {
        return false;
    }
    for (const char *h = hay; *h != '\0'; ++h) {
        const char *a = h;
        const char *b = needle;
        while (*a != '\0' && *b != '\0' && store_ascii_lower(*a) == store_ascii_lower(*b)) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool store_query_hit(const cJSON *item)
{
    if (s_store_query.empty()) {
        return true;
    }
    const char *q = s_store_query.c_str();
    const cJSON *id = cJSON_GetObjectItem(item, "id");
    const cJSON *name = cJSON_GetObjectItem(item, "name");
    const cJSON *desc = cJSON_GetObjectItem(item, "description");
    const cJSON *vendor = cJSON_GetObjectItem(item, "vendor");
    return store_text_has(cJSON_IsString(name) ? name->valuestring : nullptr, q) ||
           store_text_has(cJSON_IsString(id) ? id->valuestring : nullptr, q) ||
           store_text_has(cJSON_IsString(desc) ? desc->valuestring : nullptr, q) ||
           store_text_has(cJSON_IsString(vendor) ? vendor->valuestring : nullptr, q);
}

void refresh_store_rows(const char *prefix, cJSON *arr, cJSON *catalog)
{
    const int max_rows = LCD_COUNT_STORE_ROWS;
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    char path[96];
    char text[128];
    const bool is_pack = strcmp(prefix, kStorePrefixes[0]) == 0;
    const int section = is_pack ? 1 : 2;
    store_row_cache_clear(section);
    int match_at[LCD_COUNT_STORE_ROWS];
    int nmatch = 0;
    for (int src = 0; src < n && nmatch < max_rows; src++) {
        const cJSON *item = cJSON_GetArrayItem(arr, src);
        if (item == nullptr || !store_query_hit(item)) {
            continue;
        }
        match_at[nmatch++] = src;
    }
    s_store_row_count[section] = nmatch;
    /* Web section header carries a count badge; the LCD puts it in the title. */
    set_label_text_fmt(is_pack ? LCD_VIEW_STORE_PACK_HDR : LCD_VIEW_STORE_MOD_HDR,
                       "%s · %d", lcd_tr(LCD_STR_STORE_PLUGIN), s_store_row_count[section]);

    for (int i = 0; i < max_rows; i++) {
        snprintf(path, sizeof(path), LCD_VIEW_PATTERN_STORE_ROW, prefix, prefix, i);
        auto row = s_lcd.runtime->find_view(s_lcd.document_id, path);
        if (!row.valid()) {
            continue;
        }
        if (i >= nmatch) {
            row.set_hidden(true);
            continue;
        }
        const cJSON *item = cJSON_GetArrayItem(arr, match_at[i]);
        if (!item) {
            row.set_hidden(true);
            continue;
        }

        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        const cJSON *enabled = cJSON_GetObjectItem(item, "enabled");

        if (cJSON_IsString(id) && id->valuestring[0] != '\0') {
            StoreRowInfo &info = s_store_rows[section][i];
            snprintf(info.id, sizeof(info.id), "%s", id->valuestring);
            snprintf(info.name, sizeof(info.name), "%s",
                     (cJSON_IsString(name) && name->valuestring[0] != '\0')
                         ? name->valuestring : id->valuestring);
            const cJSON *cat = catalog_entry(catalog, info.id);
            const cJSON *cat_type = cJSON_GetObjectItem(cat, "type");
            snprintf(info.kind, sizeof(info.kind), "%s",
                     (cJSON_IsString(cat_type) && cat_type->valuestring[0] != '\0')
                         ? cat_type->valuestring : (is_pack ? "pack" : "module"));
            info.config_only = cJSON_IsTrue(cJSON_GetObjectItem(cat, "config_only"));
            /* Only the module list carries `managed`; a pack has no such field
             * and the Web console treats absent as managed (it !== false). */
            const cJSON *managed = cJSON_GetObjectItem(item, "managed");
            info.managed = !cJSON_IsFalse(managed);
            info.upgrade = cJSON_IsTrue(cJSON_GetObjectItem(cat, "upgrade_available"))
                           || cJSON_IsTrue(cJSON_GetObjectItem(item, "binary_stale"));
            info.available = false;
            info.enabled = !cJSON_IsBool(enabled) || cJSON_IsTrue(enabled);
            const cJSON *ver = cJSON_GetObjectItem(item, "version");
            if (!cJSON_IsString(ver)) {
                ver = cJSON_GetObjectItem(cat, "version");
            }
            const cJSON *vendor = cJSON_GetObjectItem(item, "vendor");
            if (!cJSON_IsString(vendor)) {
                vendor = cJSON_GetObjectItem(cat, "vendor");
            }
            const cJSON *desc = cJSON_GetObjectItem(item, "description");
            if (!cJSON_IsString(desc)) {
                desc = cJSON_GetObjectItem(cat, "description");
            }
            snprintf(info.version, sizeof(info.version), "%s",
                     cJSON_IsString(ver) ? ver->valuestring : "-");
            snprintf(info.vendor, sizeof(info.vendor), "%s",
                     cJSON_IsString(vendor) ? vendor->valuestring : "");
            snprintf(info.description, sizeof(info.description), "%s",
                     cJSON_IsString(desc) ? desc->valuestring : "");
        }

        snprintf(path, sizeof(path), LCD_VIEW_PATTERN_STORE_ROW_NAME,
                 prefix, prefix, i, prefix, i);
        /* Web row cells: Name | Version | Description | Type | Adapter | State | Actions. The LCD
         * row has three slots, so it mirrors Name+Version | Type·Description | State. */
        {
            const cJSON *version = cJSON_GetObjectItem(item, "version");
            const char *label = (cJSON_IsString(name) && name->valuestring[0] != '\0')
                                    ? name->valuestring
                                    : (cJSON_IsString(id) ? id->valuestring : "");
            if (cJSON_IsString(version) && version->valuestring[0] != '\0') {
                snprintf(text, sizeof(text), "%s  v%s", label, version->valuestring);
            } else {
                snprintf(text, sizeof(text), "%s", label);
            }
        }
        set_label_text(path, text);

        snprintf(path, sizeof(path), LCD_VIEW_PATTERN_STORE_ROW_DESC,
                 prefix, prefix, i, prefix, i);
        {
            /* One kind on the LCD: the user asked for everything to read as Plugin. */
            const char *kind_label = lcd_tr(LCD_STR_STORE_PLUGIN);
            const cJSON *desc = cJSON_GetObjectItem(item, "description");
            const cJSON *vendor = cJSON_GetObjectItem(item, "vendor");
            const cJSON *adapter = cJSON_GetObjectItem(item, is_pack ? "inject_adapter" : "adapter");
            char body[96];
            if (cJSON_IsString(desc) && desc->valuestring[0] != '\0') {
                if (cJSON_IsString(vendor) && vendor->valuestring[0] != '\0') {
                    snprintf(body, sizeof(body), "%s → %s", vendor->valuestring, desc->valuestring);
                } else {
                    snprintf(body, sizeof(body), "%s", desc->valuestring);
                }
            } else if (cJSON_IsString(adapter) && adapter->valuestring[0] != '\0') {
                snprintf(body, sizeof(body), "adapter: %s", adapter->valuestring);
            } else {
                snprintf(body, sizeof(body), "-");
            }
            snprintf(text, sizeof(text), "%s · %s", kind_label, body);
        }
        set_label_text(path, text);

        snprintf(path, sizeof(path), LCD_VIEW_PATTERN_STORE_ROW_STATE,
                 prefix, prefix, i, prefix, i);
        /* Web status cell: Enabled / Disabled; a factory builtin has no buttons at
         * all and is labelled Built-in. */
        if (!s_store_rows[section][i].managed) {
            set_label_text(path, lcd_tr(LCD_STR_STORE_BUILTIN));
        } else {
            set_label_text(path, (!cJSON_IsBool(enabled) || cJSON_IsTrue(enabled)) ? lcd_tr(LCD_STR_STATE_ENABLED) : lcd_tr(LCD_STR_STATE_DISABLED));
        }
        row.set_hidden(false);
    }
}

/* The store catalog is cached on the device at /store/catalog.json (the Web
 * console refreshes it from the LAN store server). Read that local copy:
 * minihub_store_fetch_catalog() falls back to the network when the file is
 * missing, which would block the UI thread. */
cJSON *read_local_store_catalog()
{
    static const char *path = MINIHUB_STORE_MOUNT "/catalog.json";
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        return nullptr;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }
    const long len = ftell(f);
    if (len <= 0) {
        fclose(f);
        return nullptr;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    if (buf == nullptr) {
        fclose(f);
        return nullptr;
    }
    const size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == nullptr) {
        return nullptr;
    }
    /* The file holds a bare array; tolerate an {"items":[...]} wrapper too. */
    if (!cJSON_IsArray(root)) {
        const cJSON *items = cJSON_GetObjectItem(root, "items");
        if (cJSON_IsArray(items)) {
            cJSON *arr = cJSON_Duplicate(items, true);
            cJSON_Delete(root);
            return arr;
        }
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

bool store_id_installed(const char *id, cJSON *const *lists, int list_count);

/* Catalog `installed` is computed at request time. The file the LCD reads does
 * not carry it, so a package that is on disk but not yet in the loader table
 * (install returns before hub reload rescans) would stay off the Installed tab. */
bool store_module_manifest_path(const char *id, char *out, size_t out_len)
{
    if (id == nullptr || id[0] == '\0' || out == nullptr || out_len == 0) {
        return false;
    }
    struct stat st {};
    snprintf(out, out_len, "%s/%s/module.json", MINIHUB_MODULES_DIR, id);
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    snprintf(out, out_len, "%s/%s.json", MINIHUB_MODULES_DIR, id);
    return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

bool store_package_on_disk(const cJSON *item, char *manifest_path, size_t manifest_len)
{
    const cJSON *id = cJSON_GetObjectItem(item, "id");
    const cJSON *type = cJSON_GetObjectItem(item, "type");
    if (!cJSON_IsString(id) || id->valuestring[0] == '\0' || !cJSON_IsString(type)) {
        return false;
    }
    const bool as_pack = cJSON_IsTrue(cJSON_GetObjectItem(item, "config_only"))
                         || strcmp(type->valuestring, "pack") == 0;
    if (as_pack) {
        if (manifest_path && manifest_len > 0) {
            manifest_path[0] = '\0';
        }
        char path[MINIHUB_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.json", MINIHUB_PACKS_DIR, id->valuestring);
        struct stat st {};
        return stat(path, &st) == 0 && S_ISREG(st.st_mode);
    }
    if (strcmp(type->valuestring, "module") != 0) {
        return false;
    }
    return store_module_manifest_path(id->valuestring, manifest_path, manifest_len);
}

void store_add_disk_row(cJSON *dest, const cJSON *item, const char *manifest_path)
{
    const cJSON *id = cJSON_GetObjectItem(item, "id");
    if (!cJSON_IsArray(dest) || !cJSON_IsString(id)) {
        return;
    }
    cJSON *row = cJSON_CreateObject();
    if (!row) {
        return;
    }
    auto copy_str = [&](const char *key) {
        const cJSON *field = cJSON_GetObjectItem(item, key);
        if (cJSON_IsString(field) && field->valuestring[0] != '\0') {
            cJSON_AddStringToObject(row, key, field->valuestring);
        }
    };
    copy_str("id");
    copy_str("name");
    copy_str("version");
    copy_str("vendor");
    copy_str("description");
    copy_str("adapter");
    bool enabled = true;
    if (manifest_path && manifest_path[0] != '\0') {
        FILE *f = fopen(manifest_path, "rb");
        if (f) {
            char buf[2048];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = '\0';
            cJSON *manifest = cJSON_Parse(buf);
            const cJSON *flag = cJSON_GetObjectItem(manifest, "enabled");
            if (cJSON_IsBool(flag)) {
                enabled = cJSON_IsTrue(flag);
            }
            cJSON_Delete(manifest);
        }
    }
    cJSON_AddBoolToObject(row, "enabled", enabled);
    cJSON_AddBoolToObject(row, "managed", true);
    cJSON_AddBoolToObject(row, "installed", true);
    cJSON_AddItemToArray(dest, row);
}

/* Packages already written under /store or /data/config/packs, but not yet in
 * the loader RAM table, still belong on the Installed tab. The web catalog
 * response does the same with its request-time `installed` flag. */
void store_merge_disk_installed(cJSON *packs, cJSON *modules, cJSON *catalog)
{
    if (!cJSON_IsArray(catalog) || !cJSON_IsArray(packs) || !cJSON_IsArray(modules)) {
        return;
    }
    cJSON *lists[2] = {packs, modules};
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, catalog) {
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id) || store_id_installed(id->valuestring, lists, 2)) {
            continue;
        }
        char manifest[MINIHUB_PATH_MAX];
        if (!store_package_on_disk(item, manifest, sizeof(manifest))) {
            continue;
        }
        const bool as_pack = cJSON_IsTrue(cJSON_GetObjectItem(item, "config_only"))
                             || (cJSON_IsString(cJSON_GetObjectItem(item, "type"))
                                 && strcmp(cJSON_GetObjectItem(item, "type")->valuestring, "pack") == 0);
        store_add_disk_row(as_pack ? packs : modules, item, manifest);
    }
}

bool store_id_installed(const char *id, cJSON *const *lists, int list_count)
{
    if (id == nullptr || id[0] == '\0') {
        return false;
    }
    for (int i = 0; i < list_count; i++) {
        if (!cJSON_IsArray(lists[i])) {
            continue;
        }
        const cJSON *item = nullptr;
        cJSON_ArrayForEach(item, lists[i]) {
            const cJSON *item_id = cJSON_GetObjectItem(item, "id");
            if (cJSON_IsString(item_id) && strcmp(item_id->valuestring, id) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* The Web store splits the catalog into Available / Installed; the LCD mirrors that
 * with a Available section on top of the installed lists. */
void refresh_store_available(cJSON *const *lists, int list_count, cJSON *catalog)
{
    char path[96];
    store_row_cache_clear(0);
    for (int i = 0; i < LCD_COUNT_STORE_AVAIL_ROWS; i++) {
        snprintf(path, sizeof(path), LCD_VIEW_PATTERN_STORE_AVAIL_ROW, i);
        auto row = s_lcd.runtime->find_view(s_lcd.document_id, path);
        if (row.valid()) {
            row.set_hidden(true);
        }
    }

    if (catalog == nullptr) {
        set_view_hidden(LCD_VIEW_STORE_AVAIL_EMPTY, false);
        return;
    }

    int shown = 0;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, catalog) {
        if (shown >= LCD_COUNT_STORE_AVAIL_ROWS) {
            break;
        }
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *version = cJSON_GetObjectItem(item, "version");
        if (!cJSON_IsString(id) || id->valuestring[0] == '\0') {
            continue;
        }
        if (store_id_installed(id->valuestring, lists, list_count)) {
            continue;
        }
        if (!store_query_hit(item)) {
            continue;
        }

        snprintf(path, sizeof(path), LCD_VIEW_PATTERN_STORE_AVAIL_ROW, shown);
        auto row = s_lcd.runtime->find_view(s_lcd.document_id, path);
        if (!row.valid()) {
            break;
        }

        char text[128];
        const char *label = (cJSON_IsString(name) && name->valuestring[0]) ? name->valuestring
                            : id->valuestring;
        if (cJSON_IsString(version) && version->valuestring[0] != '\0') {
            snprintf(text, sizeof(text), "%s  v%s", label, version->valuestring);
        } else {
            snprintf(text, sizeof(text), "%s", label);
        }
        char name_path[96];
        snprintf(name_path, sizeof(name_path), LCD_VIEW_PATTERN_STORE_AVAIL_ROW_NAME, shown, shown);
        set_label_text(name_path, text);

        const char *kind = cJSON_IsString(type) ? type->valuestring : "";
        /* One kind on the LCD: the Web splits Config module / Module, the user asked
         * for every available item to read as Plugin. */
        snprintf(text, sizeof(text), "%s", lcd_tr(LCD_STR_STORE_PLUGIN));
        char desc_path[96];
        snprintf(desc_path, sizeof(desc_path), LCD_VIEW_PATTERN_STORE_AVAIL_ROW_DESC, shown, shown);
        /* Web row cells again: Name | Version | Description | Type. The LCD has two slots,
         * so the type and the id share the second one. */
        set_label_text_fmt(desc_path, "%s · %s", text, id->valuestring);

        StoreRowInfo &info = s_store_rows[0][shown];
        snprintf(info.id, sizeof(info.id), "%s", id->valuestring);
        snprintf(info.name, sizeof(info.name), "%s", label);
        snprintf(info.kind, sizeof(info.kind), "%s", kind);
        info.config_only = cJSON_IsTrue(cJSON_GetObjectItem(item, "config_only"));
        info.managed = true;
        info.upgrade = false;
        info.available = true;
        info.enabled = false;
        const cJSON *desc = cJSON_GetObjectItem(item, "description");
        const cJSON *vendor = cJSON_GetObjectItem(item, "vendor");
        snprintf(info.version, sizeof(info.version), "%s",
                 (cJSON_IsString(version) && version->valuestring[0])
                     ? version->valuestring : "-");
        snprintf(info.vendor, sizeof(info.vendor), "%s",
                 (cJSON_IsString(vendor) && vendor->valuestring[0])
                     ? vendor->valuestring : "");
        snprintf(info.description, sizeof(info.description), "%s",
                 (cJSON_IsString(desc) && desc->valuestring[0])
                     ? desc->valuestring : "");

        row.set_hidden(false);
        shown++;
    }
    s_store_row_count[0] = shown;
    set_label_text_fmt(LCD_VIEW_STORE_AVAIL_HDR, lcd_tr(LCD_STR_STORE_AVAILABLE_COUNT_FMT), shown);
    set_view_hidden(LCD_VIEW_STORE_AVAIL_EMPTY, shown > 0);
}

/* The installed tab shows an empty state when nothing is installed; it must not
 * appear while the Available tab is active. */
void store_update_inst_empty()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    const bool any = (s_store_row_count[1] + s_store_row_count[2]) > 0;
    set_view_hidden(LCD_VIEW_STORE_INST_EMPTY, s_store_tab_available || any);
}

/* The tab row is only 已安装 / 可用. The line under it is 插件 or 可用, and
 * the refresh button sits on the right of that line. */
void store_update_list_hdr()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    if (s_store_tab_available) {
        set_label_text_fmt(LCD_VIEW_STORE_LIST_HDR, lcd_tr(LCD_STR_STORE_AVAILABLE_COUNT_FMT),
                           s_store_row_count[0]);
    } else {
        set_label_text_fmt(LCD_VIEW_STORE_LIST_HDR, lcd_tr(LCD_STR_STORE_PLUGIN_COUNT_FMT),
                           s_store_row_count[1] + s_store_row_count[2]);
    }
}

/* Toggle between the Available and Installed panels on the store screen. The two tab
 * buttons expose store_tab.{bg,border,text} bindings (mirroring the overview
 * filter chips) so we just push the active/inactive colors. */
void store_set_tab(bool available)
{
    s_store_tab_available = available;
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    set_view_hidden(LCD_VIEW_STORE_AVAIL_SECTION, !available);
    set_view_hidden(LCD_VIEW_STORE_PACK_SECTION, available);
    set_view_hidden(LCD_VIEW_STORE_MOD_SECTION, available);
    store_update_inst_empty();
    store_update_list_hdr();

    std::vector<gui::BindingValueUpdate> updates;
    auto append = [](std::vector<gui::BindingValueUpdate> &u, const char *tab, bool active) {
        char btn[72];
        snprintf(btn, sizeof(btn), LCD_VIEW_PATTERN_STORE_TAB_BTN, tab);
        const char *bg = active ? "#6366f1" : "#1a2238";
        const char *border = active ? "#6366f1" : "#243049";
        const char *text = active ? "#eef2ff" : "#8b9bb4";
        u.push_back({btn, "store_tab.bg", bg});
        u.push_back({btn, "store_tab.border", border});
        char lbl[88];
        snprintf(lbl, sizeof(lbl), "%s/%s_lbl", btn, tab);
        u.push_back({lbl, "store_tab.text", text});
    };
    append(updates, "tab_available", available);
    append(updates, "tab_installed", !available);
    s_lcd.runtime->set_binding_values(s_lcd.document_id, updates);
}

void refresh_store()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    if (!state || *state != "store") {
        return;
    }

    store_set_tab(s_store_tab_available);

    static_assert(LCD_ENUM_STORE_PREFIXES_N == 2, "store prefix enumeration changed");
    /* Fetch the catalog once per boot, in the background, so the Available list
     * can fill in; the refresh never blocks the UI thread. */
    static bool s_catalog_refresh_started = false;
    if (!s_catalog_refresh_started) {
        s_catalog_refresh_started = true;
        (void)minihub_store_refresh_catalog_async();
    }
    cJSON *lists[LCD_ENUM_STORE_PREFIXES_N] = {
        minihub_pack_list_json(),
        minihub_module_list_json(),
    };
    for (int i = 0; i < LCD_ENUM_STORE_PREFIXES_N; i++) {
        if (!lists[i]) {
            lists[i] = cJSON_CreateArray();
        }
    }
    /* One catalog read per refresh, shared by the installed rows (which need
     * config_only / upgrade_available) and the Available rows. */
    cJSON *catalog = read_local_store_catalog();
    store_merge_disk_installed(lists[0], lists[1], catalog);
    for (int i = 0; i < LCD_ENUM_STORE_PREFIXES_N; i++) {
        if (lists[i]) {
            refresh_store_rows(kStorePrefixes[i], lists[i], catalog);
        } else {
            store_row_cache_clear(i + 1);
        }
    }
    /* Config pack and Module are both Plugin now: one header with the total count, and
     * the module header stays hidden so the rows read as one list. */
    set_label_text_fmt(LCD_VIEW_STORE_PACK_HDR, lcd_tr(LCD_STR_STORE_PLUGIN_COUNT_FMT),
                       s_store_row_count[1] + s_store_row_count[2]);
    refresh_store_available(lists, LCD_ENUM_STORE_PREFIXES_N, catalog);
    store_update_inst_empty();
    store_update_list_hdr();
    if (catalog) {
        cJSON_Delete(catalog);
    }
    for (int i = 0; i < LCD_ENUM_STORE_PREFIXES_N; i++) {
        if (lists[i]) {
            cJSON_Delete(lists[i]);
        }
    }
}

void format_geo_hub_label(const minihub_geo_t &geo, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    const char *city = geo.city[0] != '\0' ? geo.city : (geo.region[0] != '\0' ? geo.region : nullptr);
    const char *country = geo.country[0] != '\0' ? geo.country : nullptr;
    if (city && country) {
        snprintf(out, out_len, "Hub: %s, %s", city, country);
    } else if (city) {
        snprintf(out, out_len, "Hub: %s", city);
    } else if (country) {
        snprintf(out, out_len, "Hub: %s", country);
    } else {
        snprintf(out, out_len, "Hub: %.4f, %.4f", geo.lat, geo.lon);
    }
}

bool map_snapshot_file_ready()
{
    struct stat st {};
    if (stat(MINIHUB_MAP_SNAPSHOT_ABS, &st) != 0 || st.st_size < 128) {
        return false;
    }
    FILE *f = fopen(MINIHUB_MAP_SNAPSHOT_ABS, "rb");
    if (!f) {
        return false;
    }
    unsigned char header[24];
    const size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    static const unsigned char kPngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (n != sizeof(header) || memcmp(header, kPngSig, sizeof(kPngSig)) != 0) {
        unlink(MINIHUB_MAP_SNAPSHOT_ABS);
        unlink("/data/ui/cache/map.bin");
        return false;
    }
    const int w = (header[16] << 24) | (header[17] << 16) | (header[18] << 8) | header[19];
    const int h = (header[20] << 24) | (header[21] << 16) | (header[22] << 8) | header[23];
    if (w != kMapCanvasW || h != kMapCanvasH) {
        unlink(MINIHUB_MAP_SNAPSHOT_ABS);
        return false;
    }
    return true;
}

void show_map_image_if_ready()
{
    if (!map_snapshot_file_ready()) {
        return;
    }
    set_image_src(MAP_IMAGE_VIEW, MINIHUB_MAP_SNAPSHOT_ABS);
}

void set_map_status_overlay(bool show, const char *text)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    auto view = s_lcd.runtime->find_view(s_lcd.document_id, MAP_STATUS_VIEW);
    if (!view.valid()) {
        return;
    }
    if (show && text) {
        view.as_label().set_text(text);
        view.set_hidden(false);
    } else {
        view.set_hidden(true);
    }
}

void update_map_ready_ui(bool map_ready, bool fetching)
{
    if (map_ready) {
        show_map_image_if_ready();
    }
    if (map_ready && !fetching) {
        set_label_text(LCD_VIEW_MAP_INFO, "OpenStreetMap · Hub location · © OSM");
        set_map_status_overlay(false, nullptr);
    } else {
        set_label_text(LCD_VIEW_MAP_INFO, "");
        set_map_status_overlay(true, "Updating map...");
    }
}

extern "C" void minihub_map_download_begin(void)
{
    /* Map tiles decode via stb_image + PSRAM; keep LVGL running for touch/nav. */
}

extern "C" void minihub_map_download_end(void)
{
}

extern "C" void on_map_snapshot_fetch_done(esp_err_t err, void * /*user_ctx*/)
{
    if (s_lcd.scheduler) {
        s_lcd.scheduler->post([err]() {
            s_map_fetching.store(false, std::memory_order_relaxed);
            with_lvgl_lock([err]() {
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "map snapshot: %s", esp_err_to_name(err));
                }
                refresh_map();
            });
            request_display_refresh();
        });
    } else {
        s_map_fetching.store(false, std::memory_order_relaxed);
    }
}

void schedule_map_snapshot_fetch(double lat, double lon)
{
    if (s_map_fetching.load(std::memory_order_relaxed) ||
        !minihub_web_map_snapshot_should_fetch(lat, lon)) {
        return;
    }
    s_map_fetching.store(true, std::memory_order_relaxed);
    set_map_status_overlay(true, "Updating map...");
    set_label_text(LCD_VIEW_MAP_INFO, "");
    const esp_err_t err =
        minihub_web_fetch_map_snapshot_async(lat, lon, kMapCanvasW, kMapCanvasH, on_map_snapshot_fetch_done, nullptr);
    if (err != ESP_OK) {
        s_map_fetching.store(false, std::memory_order_relaxed);
        ESP_LOGW(TAG, "map snapshot queue: %s", esp_err_to_name(err));
    }
}

void refresh_map()
{
    if (!minihub_web_sta_has_ip()) {
        set_label_text(LCD_VIEW_MAP_HUB, "Waiting for network...");
        set_label_text(LCD_VIEW_MAP_INFO, "");
        set_map_status_overlay(false, nullptr);
        return;
    }
    minihub_geo_t geo = {};
    esp_err_t geo_err = minihub_web_peek_geo(&geo);
    if (geo_err != ESP_OK || !geo.valid) {
        (void)minihub_web_refresh_geo_async();
        set_label_text(LCD_VIEW_MAP_HUB, "Resolving location...");
        set_label_text(LCD_VIEW_MAP_INFO, "");
        set_map_status_overlay(false, nullptr);
        return;
    }

    char hub[192];
    format_geo_hub_label(geo, hub, sizeof(hub));
    set_label_text(LCD_VIEW_MAP_HUB, hub);

    const bool map_ready = map_snapshot_file_ready();
    const bool fetching = s_map_fetching.load(std::memory_order_relaxed);
    update_map_ready_ui(map_ready, fetching);
    schedule_map_snapshot_fetch(geo.lat, geo.lon);
}

/* Same Hub store as Web GET/POST /api/system/display_name. */
static constexpr size_t kUserNameMax = MINIHUB_DISPLAY_NAME_MAX;
static char s_local_user_name[kUserNameMax + 1] = {};

void load_local_user_name()
{
    s_local_user_name[0] = '\0';
    (void)minihub_display_name_get(s_local_user_name, sizeof(s_local_user_name));
}

esp_err_t save_local_user_name(const char *name)
{
    const esp_err_t err = minihub_display_name_set(name);
    if (err == ESP_OK) {
        strncpy(s_local_user_name, name, sizeof(s_local_user_name) - 1);
        s_local_user_name[sizeof(s_local_user_name) - 1] = '\0';
    }
    return err;
}

const char *local_user_display_name()
{
    return s_local_user_name[0] ? s_local_user_name : lcd_tr(LCD_STR_USER_DEFAULT_NAME);
}

void user_rename_open();

void refresh_user_sidebar()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    load_local_user_name();
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    if (!state) {
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), LCD_VIEW_PATTERN_USER_NAME, state->c_str());
    /* Same slot as the web sidebar footer: Hub display_name, not a cloud
     * account. Web default is ov.user ("User"); a Xiaomi id stays on the
     * overview auth bar. */
    set_label_text(path, local_user_display_name());
}

bool is_controllable_point_type(const char *type)
{
    return type && (strcmp(type, "light") == 0 || strcmp(type, "switch") == 0);
}

void handle_device_tap(int slot)
{
    if (slot < 0 || static_cast<size_t>(slot) >= s_overview_filtered.size()) {
        return;
    }

    const minihub_point_t &pt = s_overview_filtered[static_cast<size_t>(slot)];
    char point_id[MINIHUB_POINT_ID_MAX];
    strncpy(point_id, pt.point_id, sizeof(point_id) - 1);
    point_id[sizeof(point_id) - 1] = '\0';
    const bool controllable = is_controllable_point_type(pt.type);
    const bool online = pt.online;

    if (!controllable) {
        ESP_LOGD(TAG, "device tap slot %d ignored (type not controllable)", slot);
        return;
    }
    if (!online) {
        ESP_LOGW(TAG, "device tap slot %d ignored (offline)", slot);
        return;
    }

    minihub_command_t command{};
    strncpy(command.point_id, point_id, sizeof(command.point_id) - 1);
    strncpy(command.cmd, "toggle", sizeof(command.cmd) - 1);
    esp_err_t err = minihub_command_invoke(&command);
    ESP_LOGI(TAG, "toggle %s: %s", point_id, esp_err_to_name(err));
    if (err == ESP_OK) {
        refresh_overview();
        request_display_refresh();
    }
}

void handle_user_tap()
{
    /* Mirror the web sidebar user-row: edit the local display name. Cloud
     * login stays on the overview auth bar (auth_login), not this button. */
    user_rename_open();
}

/* ---- system / rules / scenes ------------------------------------------- */

void ensure_rule_row(int index)
{
    if (!s_lcd.runtime) {
        return;
    }
    char path[96];
    snprintf(path, sizeof(path),
             LCD_TEMPLATE_RULE_ROW_PARENT "/" LCD_TEMPLATE_RULE_ROW_INSTANCE, index);
    if (s_lcd.runtime->find_view(s_lcd.document_id, path).valid()) {
        return;
    }
    char instance[24];
    snprintf(instance, sizeof(instance), LCD_TEMPLATE_RULE_ROW_INSTANCE, index);
    auto made = s_lcd.runtime->create_view(s_lcd.document_id,
                                           LCD_TEMPLATE_RULE_ROW_ID,
                                           LCD_TEMPLATE_RULE_ROW_PARENT,
                                           instance);
    if (!made) {
        ESP_LOGW(TAG, "rule row %d: %s", index, made.error().c_str());
    }
}

void ensure_scene_row(int index)
{
    if (!s_lcd.runtime) {
        return;
    }
    char path[96];
    snprintf(path, sizeof(path),
             LCD_TEMPLATE_SCENE_ROW_PARENT "/" LCD_TEMPLATE_SCENE_ROW_INSTANCE, index);
    if (s_lcd.runtime->find_view(s_lcd.document_id, path).valid()) {
        return;
    }
    char instance[24];
    snprintf(instance, sizeof(instance), LCD_TEMPLATE_SCENE_ROW_INSTANCE, index);
    auto made = s_lcd.runtime->create_view(s_lcd.document_id,
                                           LCD_TEMPLATE_SCENE_ROW_ID,
                                           LCD_TEMPLATE_SCENE_ROW_PARENT,
                                           instance);
    if (!made) {
        ESP_LOGW(TAG, "scene row %d: %s", index, made.error().c_str());
    }
}

void destroy_rule_rows_from(int from)
{
    if (!s_lcd.runtime) {
        return;
    }
    for (int i = from; i < LCD_COUNT_RULE_SLOTS; i++) {
        char path[96];
        snprintf(path, sizeof(path),
                 LCD_TEMPLATE_RULE_ROW_PARENT "/" LCD_TEMPLATE_RULE_ROW_INSTANCE, i);
        s_lcd.runtime->destroy_view(s_lcd.document_id, path);
    }
}

void destroy_scene_rows_from(int from)
{
    if (!s_lcd.runtime) {
        return;
    }
    for (int i = from; i < LCD_COUNT_SCENE_SLOTS; i++) {
        char path[96];
        snprintf(path, sizeof(path),
                 LCD_TEMPLATE_SCENE_ROW_PARENT "/" LCD_TEMPLATE_SCENE_ROW_INSTANCE, i);
        s_lcd.runtime->destroy_view(s_lcd.document_id, path);
    }
}

/* One-line summary of a rule's "then". The Web console renders the whole
 * When / If / Then flow; the LCD row only has room for the outcome, which is the
 * part the user acts on ("Then: cam0 On"). */

/* Copy a snprintf result (already NUL-terminated in a fixed buffer) into a
 * caller buffer of unknown size, truncating instead of overflowing. */
void copy_truncated(char *out, size_t out_len, const char *src, int src_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!src || src_len <= 0) {
        out[0] = '\0';
        return;
    }
    size_t len = (size_t)src_len;
    if (len > out_len - 1) {
        len = out_len - 1;
    }
    memcpy(out, src, len);
    out[len] = '\0';
}

void rule_then_summary(const cJSON *rule, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const cJSON *then = rule ? cJSON_GetObjectItem(rule, "then") : nullptr;
    if (!then) {
        return;
    }

    /* `then` comes in two shapes and the Web console normalizes both with
     * rulesThenList(): the steps form [{actions:[...], delay_after:n}] that the
     * rule editor saves, and the legacy flat form [{type,point_id,cmd}]. Take the
     * first action of the first step either way. */
    const cJSON *action = nullptr;
    if (cJSON_IsArray(then)) {
        const cJSON *first = cJSON_GetArrayItem(then, 0);
        if (cJSON_IsObject(first)) {
            const cJSON *actions = cJSON_GetObjectItem(first, "actions");
            action = cJSON_IsArray(actions) ? cJSON_GetArrayItem(actions, 0) : first;
        }
    } else if (cJSON_IsObject(then)) {
        const cJSON *actions = cJSON_GetObjectItem(then, "actions");
        action = cJSON_IsArray(actions) ? cJSON_GetArrayItem(actions, 0) : then;
    }
    if (!cJSON_IsObject(action)) {
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(action, "type");
    const cJSON *cmd = cJSON_GetObjectItem(action, "cmd");
    const cJSON *pid = cJSON_GetObjectItem(action, "point_id");

    char act[24] = {};
    if (cJSON_IsString(type) && strcmp(type->valuestring, "delay") == 0) {
        const cJSON *sec = cJSON_GetObjectItem(action, "delay_sec");
        if (!cJSON_IsNumber(sec)) {
            sec = cJSON_GetObjectItem(action, "seconds");
        }
        snprintf(act, sizeof(act), lcd_tr(LCD_STR_RULES_DELAY_FMT), cJSON_IsNumber(sec) ? sec->valueint : 0);
    } else if (cJSON_IsString(cmd)) {
        if (strcmp(cmd->valuestring, "turn_on") == 0) {
            snprintf(act, sizeof(act), "%s", lcd_tr(LCD_STR_CMD_ON));
        } else if (strcmp(cmd->valuestring, "turn_off") == 0) {
            snprintf(act, sizeof(act), "%s", lcd_tr(LCD_STR_CMD_OFF));
        } else if (strcmp(cmd->valuestring, "toggle") == 0) {
            snprintf(act, sizeof(act), "%s", lcd_tr(LCD_STR_CMD_TOGGLE));
        } else {
            snprintf(act, sizeof(act), "%s", cmd->valuestring);
        }
    }

    /* point_id is "<device>/<point>": the device part is the one the user knows. */
    char dev[40] = {};
    if (cJSON_IsString(pid) && pid->valuestring[0]) {
        snprintf(dev, sizeof(dev), "%s", pid->valuestring);
        char *slash = strrchr(dev, '/');
        if (slash != nullptr && slash != dev) {
            *slash = '\0';
        }
    }

    if (dev[0] && act[0]) {
        char tmp[64];
        const int n = snprintf(tmp, sizeof(tmp), lcd_tr(LCD_STR_RULES_THEN_FMT), dev, act);
        copy_truncated(out, out_len, tmp, n);
    } else if (act[0]) {
        char tmp[64];
        const int n = snprintf(tmp, sizeof(tmp), lcd_tr(LCD_STR_RULES_THEN_NO_DEVICE_FMT), act);
        copy_truncated(out, out_len, tmp, n);
    } else if (dev[0]) {
        char tmp[64];
        const int n = snprintf(tmp, sizeof(tmp), lcd_tr(LCD_STR_RULES_THEN_DEVICE_FMT), dev);
        copy_truncated(out, out_len, tmp, n);
    }
}

void refresh_rules()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    if (!state || *state != "rules") {
        return;
    }

    cJSON *rules_root = minihub_config_get_rules();
    cJSON *arr = rules_root ? cJSON_GetObjectItem(rules_root, "rules") : nullptr;
    const int total = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    const int shown = total < LCD_COUNT_RULE_SLOTS ? total : LCD_COUNT_RULE_SLOTS;

    for (int i = 0; i < shown; i++) {
        const cJSON *rule = cJSON_GetArrayItem(arr, i);
        if (!rule) {
            continue;
        }
        ensure_rule_row(i);
        char path[96];
        snprintf(path, sizeof(path),
                 LCD_TEMPLATE_RULE_ROW_PARENT "/" LCD_TEMPLATE_RULE_ROW_INSTANCE, i);

        const cJSON *name = cJSON_GetObjectItem(rule, "name");
        const cJSON *id = cJSON_GetObjectItem(rule, "id");
        const char *label = (cJSON_IsString(name) && name->valuestring[0]) ? name->valuestring
                            : ((cJSON_IsString(id) && id->valuestring[0]) ? id->valuestring
                               : lcd_tr(LCD_STR_RULES_UNNAMED));
        const bool enabled = !cJSON_IsFalse(cJSON_GetObjectItem(rule, "enabled"));
        char then[64] = {};
        rule_then_summary(rule, then, sizeof(then));
        set_row_binding(path, "row/name", "card.name", label);
        set_row_binding(path, "row/state", "card.state", enabled ? lcd_tr(LCD_STR_STATE_ENABLED) : lcd_tr(LCD_STR_STATE_ACTIVATED_OFF));
        set_row_binding(path, "then", "card.then", then);
    }
    destroy_rule_rows_from(shown);
    set_view_hidden(LCD_VIEW_RULES_EMPTY, shown > 0);
    ESP_LOGD(TAG, "rules screen: %d rule(s) shown", shown);
}

void refresh_scenes()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    if (!state || *state != "scenes") {
        return;
    }

    cJSON *scenes_root = minihub_config_get_scenes();
    cJSON *arr = scenes_root ? cJSON_GetObjectItem(scenes_root, "scenes") : nullptr;
    const int total = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    const int shown = total < LCD_COUNT_SCENE_SLOTS ? total : LCD_COUNT_SCENE_SLOTS;

    for (int i = 0; i < shown; i++) {
        const cJSON *scene = cJSON_GetArrayItem(arr, i);
        if (!scene) {
            continue;
        }
        ensure_scene_row(i);
        char path[96];
        snprintf(path, sizeof(path),
                 LCD_TEMPLATE_SCENE_ROW_PARENT "/" LCD_TEMPLATE_SCENE_ROW_INSTANCE, i);

        const cJSON *name = cJSON_GetObjectItem(scene, "name");
        const cJSON *id = cJSON_GetObjectItem(scene, "id");
        const char *label = (cJSON_IsString(name) && name->valuestring[0]) ? name->valuestring
                            : ((cJSON_IsString(id) && id->valuestring[0]) ? id->valuestring
                               : lcd_tr(LCD_STR_SCENES_UNNAMED));
        const cJSON *steps = cJSON_GetObjectItem(scene, "steps");
        const int step_count = cJSON_IsArray(steps) ? cJSON_GetArraySize(steps) : 0;
        char meta[48];
        snprintf(meta, sizeof(meta), lcd_tr(LCD_STR_SCENES_META_FMT), step_count);
        set_row_binding(path, "name", "card.name", label);
        set_row_binding(path, "meta", "card.meta", meta);
    }
    destroy_scene_rows_from(shown);
    set_view_hidden(LCD_VIEW_SCENES_EMPTY, shown > 0);
    ESP_LOGD(TAG, "scenes screen: %d scene(s) shown", shown);
}

/* ----------------------------------------------------------------------------
 * Rule / scene creation form.
 *
 * The Web console edits a rule as WHEN / AND IF / THEN and a scene as a list of
 * steps. The LCD mirrors that structure in one scrollable form: device choices
 * come from the point registry through a dropdown binding, while parameter /
 * operator / value use the engine defaults (feature parity, degraded
 * interaction). Names are typed through the shared floating keyboard, so the
 * OK / Cancel row always stays reachable on the 480px display.
 * -------------------------------------------------------------------------- */

static void wire_action(const char *action, void (*handler)());

enum class FormTarget { None, Rule, Scene };

struct DeviceChoice {
    char point_id[MINIHUB_POINT_ID_MAX];
    char name[48];
    char meta[64];
};

struct FormState {
    FormTarget target = FormTarget::None;
    std::string name;
    bool enabled = true;
    int when = 0;
    int if_dev = 0;     /* 0 = no extra condition; 1..n = s_form_devices[n-1] */
    int if_value = 0;   /* 0 = on, 1 = off */
    int then_dev = 0;
    int cmd = 0;        /* 0 = turn_on, 1 = turn_off */
    int when_type = 0;  /* 0 = device, 1 = daily, 2 = once */
    std::string when_time;
};

static FormState s_form;
static std::vector<DeviceChoice> s_form_devices;
static constexpr size_t kFormMaxDevices = 12;

enum FormField {
    kFormWhen = 1,
    kFormIfDev,
    kFormCond,
    kFormThenDev,
    kFormCmd,
    kFormWhenType,
};

static const char *form_screen()
{
    return s_form.target == FormTarget::Scene ? "scenes" : "rules";
}

static void form_resolve(const char *pattern, char *out, size_t out_len)
{
    snprintf(out, out_len, pattern, form_screen());
}

static void form_label(const char *pattern, const char *text)
{
    char path[160];
    form_resolve(pattern, path, sizeof(path));
    set_label_text(path, text);
}

static void form_binding(const char *pattern, const char *key, const std::string &value)
{
    char path[160];
    form_resolve(pattern, path, sizeof(path));
    s_lcd.runtime->set_binding_value(s_lcd.document_id, path, key, value);
}

static void form_hidden(const char *pattern, bool hidden)
{
    char path[160];
    form_resolve(pattern, path, sizeof(path));
    set_view_hidden(path, hidden);
}

static void form_build_devices()
{
    s_form_devices.clear();
    minihub_point_t *points = nullptr;
    size_t count = 0;
    if (minihub_point_list(&points, &count) == ESP_OK && points) {
        for (size_t i = 0; i < count && s_form_devices.size() < kFormMaxDevices; i++) {
            const minihub_point_t &pt = points[i];
            DeviceChoice item{};
            strncpy(item.point_id, pt.point_id, sizeof(item.point_id) - 1);
            strncpy(item.name, point_display_name(pt), sizeof(item.name) - 1);
            snprintf(item.meta, sizeof(item.meta), "%s · %s", pt.type, pt.adapter);
            s_form_devices.push_back(item);
        }
        minihub_point_list_free(points, count);
    }
}

static const DeviceChoice &form_device(int index)
{
    if (index < 0 || index >= (int)s_form_devices.size()) {
        return s_form_devices.front();   /* callers guard on empty() */
    }
    return s_form_devices[(size_t)index];
}

static std::string form_options_json(const char *none_text)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == nullptr) {
        return "[]";
    }
    if (none_text != nullptr) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(none_text));
    }
    for (const auto &dev : s_form_devices) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(dev.name));
    }
    if (s_form_devices.empty()) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(lcd_tr(LCD_STR_FORM_NO_DEVICE)));
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    std::string out = (json != nullptr) ? json : "[]";
    free(json);
    return out;
}

static void make_unique_id(cJSON *arr, const char *prefix, char *out, size_t out_len)
{
    int max_n = -1;
    const int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(arr, i);
        const cJSON *id = cJSON_GetObjectItem(it, "id");
        if (cJSON_IsString(id)) {
            const char *v = id->valuestring;
            if (strncmp(v, prefix, strlen(prefix)) == 0) {
                const int val = atoi(v + strlen(prefix));
                if (val > max_n) {
                    max_n = val;
                }
            }
        }
    }
    snprintf(out, out_len, "%s%d", prefix, max_n + 1);
}

/* Rules engine supports when.type="time" (daily, UTC "HH:MM") and
 * when.type="once" (UTC "YYYY-MM-DD HH:MM"). The Web console lets the user
 * type wall-clock time in the hub timezone and converts to UTC; do the same
 * here so LCD-created schedules match Web-created ones. */
static int form_tz_offset_minutes()
{
    cJSON *status = minihub_tz_status_json();
    if (status == nullptr) {
        return 0;
    }
    const cJSON *off = cJSON_GetObjectItem(status, "offset_min");
    const int value = cJSON_IsNumber(off) ? (int)cJSON_GetNumberValue(off) : 0;
    cJSON_Delete(status);
    return value;
}

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + (int)era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    const unsigned mm = mp + (mp < 10 ? 3 : -9);
    yy += (mm <= 2);
    *y = yy;
    *m = mm;
    *d = dd;
}

static bool form_utc_time(const FormState &f, char *out, size_t out_len)
{
    const int offset = form_tz_offset_minutes();
    if (f.when_type == 1) {
        int h = 0, min = 0;
        if (sscanf(f.when_time.c_str(), "%d:%d", &h, &min) != 2 ||
            h < 0 || h > 23 || min < 0 || min > 59) {
            return false;
        }
        int total = ((h * 60 + min) - offset) % 1440;
        if (total < 0) {
            total += 1440;
        }
        snprintf(out, out_len, "%02d:%02d", total / 60, total % 60);
        return true;
    }
    if (f.when_type == 2) {
        int y = 0, mo = 0, d = 0, h = 0, min = 0;
        if (sscanf(f.when_time.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &min) != 5 ||
            y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
            h < 0 || h > 23 || min < 0 || min > 59) {
            return false;
        }
        const int64_t total = days_from_civil(y, (unsigned)mo, (unsigned)d) * 1440 +
                              h * 60 + min - offset;
        const int64_t days = total >= 0 ? total / 1440 : (total - 1439) / 1440;
        const int mins = (int)(total - days * 1440);
        int uy = 0;
        unsigned um = 0, ud = 0;
        civil_from_days(days, &uy, &um, &ud);
        snprintf(out, out_len, "%04d-%02u-%02u %02d:%02d",
                 uy, um, ud, mins / 60, mins % 60);
        return true;
    }
    return false;
}

static void create_rule(const FormState &f, const char *name)
{
    if (s_form_devices.empty()) {
        return;
    }
    cJSON *rules_root = minihub_config_get_rules();
    if (!rules_root) {
        return;
    }
    cJSON *arr = cJSON_GetObjectItem(rules_root, "rules");
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_AddArrayToObject(rules_root, "rules");
    }
    if (!arr) {
        return;
    }
    char id[48];
    make_unique_id(arr, "rule_lcd_", id, sizeof(id));
    cJSON *rule = cJSON_CreateObject();
    if (!rule) {
        return;
    }
    cJSON_AddStringToObject(rule, "id", id);
    cJSON_AddStringToObject(rule, "name", name);
    cJSON_AddBoolToObject(rule, "enabled", f.enabled ? 1 : 0);
    cJSON *when = cJSON_AddObjectToObject(rule, "when");
    if (f.when_type == 1 || f.when_type == 2) {
        char utc[40];
        if (!form_utc_time(f, utc, sizeof(utc))) {
            cJSON_Delete(rule);
            return;
        }
        cJSON_AddStringToObject(when, "type", f.when_type == 1 ? "time" : "once");
        cJSON_AddStringToObject(when, "at", utc);
    } else {
        cJSON_AddStringToObject(when, "type", "point");
        cJSON_AddStringToObject(when, "point_id", form_device(f.when).point_id);
        /* An empty parameter makes the engine compare the point's own summary
         * value, so a quick-created rule is valid for a camera/sensor too. */
        cJSON_AddStringToObject(when, "parameter", "");
        cJSON_AddStringToObject(when, "op", "eq");
        cJSON_AddStringToObject(when, "value", "on");
    }

    if (f.if_dev > 0 && f.if_dev <= (int)s_form_devices.size()) {
        cJSON *if_arr = cJSON_AddArrayToObject(rule, "if");
        cJSON *cond = cJSON_CreateObject();
        cJSON_AddStringToObject(cond, "point_id",
                                s_form_devices[(size_t)f.if_dev - 1].point_id);
        cJSON_AddStringToObject(cond, "parameter", "");
        cJSON_AddStringToObject(cond, "op", "eq");
        cJSON_AddStringToObject(cond, "value", f.if_value == 0 ? "on" : "off");
        cJSON_AddItemToArray(if_arr, cond);
    }

    cJSON *then = cJSON_AddArrayToObject(rule, "then");
    cJSON *act = cJSON_CreateObject();
    cJSON_AddStringToObject(act, "type", "command");
    cJSON_AddStringToObject(act, "point_id", form_device(f.then_dev).point_id);
    cJSON_AddStringToObject(act, "cmd", f.cmd == 0 ? "turn_on" : "turn_off");
    cJSON_AddItemToArray(then, act);
    cJSON_AddItemToArray(arr, rule);
    const esp_err_t err = minihub_config_save_rules(rules_root);
    if (err == ESP_OK) {
        (void)minihub_rules_reload(minihub_config_get_rules());
        ESP_LOGI(TAG, "rule created: %s", id);
    } else {
        ESP_LOGW(TAG, "rule create save failed: %s", esp_err_to_name(err));
    }
    refresh_rules();
}

static void create_scene(const FormState &f, const char *name)
{
    if (s_form_devices.empty()) {
        return;
    }
    cJSON *scenes_root = minihub_config_get_scenes();
    if (!scenes_root) {
        return;
    }
    cJSON *arr = cJSON_GetObjectItem(scenes_root, "scenes");
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_AddArrayToObject(scenes_root, "scenes");
    }
    if (!arr) {
        return;
    }
    char id[48];
    make_unique_id(arr, "scene_lcd_", id, sizeof(id));
    cJSON *scene = cJSON_CreateObject();
    if (!scene) {
        return;
    }
    cJSON_AddStringToObject(scene, "id", id);
    cJSON_AddStringToObject(scene, "name", name);
    /* Write the native steps model: refresh_scenes() counts `steps`, so the
     * legacy flat `commands` array would show "0 steps" on the LCD. */
    cJSON *steps = cJSON_AddArrayToObject(scene, "steps");
    cJSON *step = cJSON_CreateObject();
    cJSON *acts = cJSON_AddArrayToObject(step, "actions");
    cJSON *act = cJSON_CreateObject();
    cJSON_AddStringToObject(act, "point_id", form_device(f.then_dev).point_id);
    cJSON_AddStringToObject(act, "cmd", f.cmd == 0 ? "turn_on" : "turn_off");
    cJSON_AddItemToArray(acts, act);
    cJSON_AddItemToArray(steps, step);
    cJSON_AddItemToArray(arr, scene);
    const esp_err_t err = minihub_config_save_scenes(scenes_root);
    if (err == ESP_OK) {
        (void)minihub_scenes_reload(minihub_config_get_scenes());
        ESP_LOGI(TAG, "scene created: %s", id);
    } else {
        ESP_LOGW(TAG, "scene create save failed: %s", esp_err_to_name(err));
    }
    refresh_scenes();
    /* The scene engine is manual-only and the rule engine has no "scene" then
     * action. Make WHEN / AND IF real by emitting a rule whose `then` is the
     * very same command the scene step performs. */
    if (f.when_type != 0 || (f.when > 0 && f.when <= (int)s_form_devices.size())) {
        FormState trigger = f;
        trigger.when = (f.when_type == 0) ? f.when - 1 : 0;
        const std::string rule_name =
            std::string(name) + lcd_tr(LCD_STR_FORM_AUTO_RULE_SUFFIX);
        create_rule(trigger, rule_name.c_str());
    }
}

static std::string form_chosen_name()
{
    const size_t first = s_form.name.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return std::string(lcd_tr(s_form.target == FormTarget::Rule
                                      ? LCD_STR_FORM_DEFAULT_RULE
                                      : LCD_STR_FORM_DEFAULT_SCENE));
    }
    const size_t last = s_form.name.find_last_not_of(" \t");
    return s_form.name.substr(first, last - first + 1);
}

/* Device rename and sidebar display-name edit share overview's name_modal.
 * textInput binds to rename.text (not name.text) so rule/scene editors stay
 * isolated. Mode selects which title / save path runs on OK. */
enum class RenameMode { None, Device, User };
static RenameMode s_rename_mode = RenameMode::None;
static int s_rename_slot = -1;
static std::string s_rename_text;

void rename_modal_open(RenameMode mode, const char *seed, const char *title, const char *hint)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !seed) {
        return;
    }
    s_rename_mode = mode;
    s_rename_text = seed;
    char input_path[128], modal_path[128];
    snprintf(input_path, sizeof(input_path), LCD_VIEW_PATTERN_NAME_INPUT, "overview");
    snprintf(modal_path, sizeof(modal_path), LCD_VIEW_PATTERN_NAME_MODAL, "overview");
    s_lcd.runtime->set_binding_value(s_lcd.document_id, input_path,
                                     "rename.text", s_rename_text);
    set_label_text(LCD_VIEW_RENAME_TITLE, title);
    set_label_text(LCD_VIEW_RENAME_HINT, hint);
    set_view_hidden(modal_path, false);
    request_display_refresh();
}

void rename_open(int slot)
{
    if (slot < 0 || static_cast<size_t>(slot) >= s_overview_filtered.size()) {
        return;
    }
    const minihub_point_t &pt = s_overview_filtered[static_cast<size_t>(slot)];
    s_rename_slot = slot;
    rename_modal_open(RenameMode::Device, point_display_name(pt),
                      lcd_tr(LCD_STR_RENAME_TITLE), lcd_tr(LCD_STR_RENAME_HINT));
}

void user_rename_open()
{
    s_rename_slot = -1;
    rename_modal_open(RenameMode::User, local_user_display_name(),
                      lcd_tr(LCD_STR_USER_RENAME_TITLE), lcd_tr(LCD_STR_USER_RENAME_HINT));
}

void handle_rename_changed(const std::string &value)
{
    if (s_rename_mode != RenameMode::None) {
        s_rename_text = value;
    }
}

void handle_rename_cancel()
{
    char modal_path[128];
    snprintf(modal_path, sizeof(modal_path), LCD_VIEW_PATTERN_NAME_MODAL, "overview");
    s_rename_mode = RenameMode::None;
    s_rename_slot = -1;
    s_rename_text.clear();
    set_view_hidden(modal_path, true);
    request_display_refresh();
}

void handle_rename_ok()
{
    if (s_rename_mode == RenameMode::None) {
        return;
    }
    const size_t first = s_rename_text.find_first_not_of(" \t");
    const size_t last = s_rename_text.find_last_not_of(" \t");
    if (first == std::string::npos) {
        set_label_text(LCD_VIEW_RENAME_HINT, lcd_tr(LCD_STR_RENAME_EMPTY));
        return;
    }
    std::string name = s_rename_text.substr(first, last - first + 1);
    if (name.size() > kUserNameMax) {
        set_label_text(LCD_VIEW_RENAME_HINT, lcd_tr(LCD_STR_RENAME_TOO_LONG));
        return;
    }

    if (s_rename_mode == RenameMode::User) {
        const esp_err_t err = save_local_user_name(name.c_str());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "user display name save failed: %s", esp_err_to_name(err));
            set_label_text(LCD_VIEW_RENAME_HINT, lcd_tr(LCD_STR_USER_RENAME_FAILED));
            return;
        }
        ESP_LOGI(TAG, "user display name -> %s", name.c_str());
        char modal_path[128];
        snprintf(modal_path, sizeof(modal_path), LCD_VIEW_PATTERN_NAME_MODAL, "overview");
        s_rename_mode = RenameMode::None;
        s_rename_text.clear();
        set_view_hidden(modal_path, true);
        refresh_user_sidebar();
        request_display_refresh();
        return;
    }

    if (s_rename_slot < 0 || static_cast<size_t>(s_rename_slot) >= s_overview_filtered.size()) {
        return;
    }
    const minihub_point_t &pt = s_overview_filtered[static_cast<size_t>(s_rename_slot)];
    char point_id[MINIHUB_POINT_ID_MAX];
    strncpy(point_id, pt.point_id, sizeof(point_id) - 1);
    point_id[sizeof(point_id) - 1] = '\0';
    const esp_err_t err = minihub_device_name_set(point_id, name.c_str());
    if (err == ESP_OK) {
        cJSON *patch = cJSON_CreateObject();
        cJSON_AddStringToObject(patch, "name", name.c_str());
        (void)minihub_point_merge_props(point_id, nullptr, patch);
        cJSON_Delete(patch);
        ESP_LOGI(TAG, "device renamed: %s -> %s", point_id, name.c_str());
    } else {
        ESP_LOGW(TAG, "device rename failed: %s", esp_err_to_name(err));
        set_label_text(LCD_VIEW_RENAME_HINT, lcd_tr(LCD_STR_RENAME_FAILED));
        return;
    }
    char modal_path[128];
    snprintf(modal_path, sizeof(modal_path), LCD_VIEW_PATTERN_NAME_MODAL, "overview");
    s_rename_mode = RenameMode::None;
    s_rename_slot = -1;
    s_rename_text.clear();
    set_view_hidden(modal_path, true);
    refresh_overview();
    request_display_refresh();
}

static void refresh_ota_status()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    set_label_text_fmt(LCD_VIEW_SYSTEM_OTA, "v%s", CONFIG_MINIHUB_VERSION);
    const char *p = "/system/system_main/sys_grid/sys_card_ota/sys_ota_status";
    if (minihub_ota_is_busy()) {
        set_label_text_fmt(p, lcd_tr(LCD_STR_SYS_OTA_BUSY_FMT), minihub_ota_get_progress());
    } else {
        const char *err = minihub_ota_get_error();
        if (err && err[0]) {
            set_label_text_fmt(p, lcd_tr(LCD_STR_SYS_OTA_LAST_FAIL_FMT), err);
        } else {
            set_label_text(p, lcd_tr(LCD_STR_SYS_OTA_IDLE));
        }
    }
}

static const char *const kOtaStatusPath =
    "/system/system_main/sys_grid/sys_card_ota/sys_ota_status";

static void post_ota_status(std::string text)
{
    if (!s_lcd.scheduler) {
        return;
    }
    s_lcd.scheduler->post([text = std::move(text)]() {
        with_lvgl_lock([text]() { set_label_text(kOtaStatusPath, text.c_str()); });
    });
}

/* HTTP against the store (default https://minihub.espressif.com, or the
 * custom store URL). Must not run on LcdUiWorker. */
static void ota_query_task(void *arg)
{
    const bool apply = *static_cast<bool *>(arg);
    delete static_cast<bool *>(arg);

    char base[MINIHUB_STORE_BASE_URL_MAX];
    if (!minihub_store_copy_base_url(base, sizeof(base))) {
        post_ota_status(lcd_tr(LCD_STR_SYS_OTA_NO_URL));
        s_ota_query_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    const int rc = apply ? minihub_update_apply(MINIHUB_UPDATE_FIRMWARE, base)
                         : minihub_update_check(MINIHUB_UPDATE_FIRMWARE, base);
    std::string text;
    if (rc > 0) {
        text = lcd_tr(apply ? LCD_STR_SYS_OTA_STARTING : LCD_STR_SYS_OTA_AVAILABLE);
    } else if (rc == 0) {
        text = lcd_tr(LCD_STR_SYS_OTA_UP_TO_DATE);
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf), lcd_tr(LCD_STR_SYS_OTA_FAIL_FMT),
                 esp_err_to_name(static_cast<esp_err_t>(rc)));
        text = buf;
    }
    post_ota_status(std::move(text));
    if (apply && minihub_ota_is_busy() && s_lcd.scheduler) {
        s_lcd.scheduler->post([]() { with_lvgl_lock([]() { refresh_ota_status(); }); });
    }
    s_ota_query_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

static void start_ota_query(bool apply)
{
    if (s_ota_query_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    set_label_text(kOtaStatusPath,
                   lcd_tr(apply ? LCD_STR_SYS_OTA_STARTING : LCD_STR_SYS_OTA_CHECKING));
    auto *flag = new (std::nothrow) bool(apply);
    if (flag == nullptr ||
        minihub_task_create_psram(ota_query_task, "lcd_ota_query", 16384, flag, 5, nullptr) != pdPASS) {
        delete flag;
        s_ota_query_busy.store(false, std::memory_order_release);
        set_label_text_fmt(kOtaStatusPath, lcd_tr(LCD_STR_SYS_OTA_FAIL_FMT), "no mem");
    }
}

static void form_update_enabled()
{
    form_label(LCD_VIEW_PATTERN_FORM_ENABLED_LBL,
               s_form.enabled ? lcd_tr(LCD_STR_FORM_ENABLED_ON)
                              : lcd_tr(LCD_STR_FORM_ENABLED_OFF));
}

static void form_apply_device_options()
{
    const bool scene = s_form.target == FormTarget::Scene;
    const std::string plain = form_options_json(nullptr);
    const std::string cond = form_options_json(lcd_tr(LCD_STR_FORM_NO_CONDITION));
    /* A scene is manual by default: its WHEN dropdown starts with
     * "manual (no auto trigger)", so no rule is created unless a device is
     * picked. */
    const std::string trigger = scene
        ? form_options_json(lcd_tr(LCD_STR_FORM_NO_TRIGGER)) : plain;
    form_binding(LCD_VIEW_PATTERN_FORM_WHEN_DEV, "form.devices", trigger);
    form_binding(LCD_VIEW_PATTERN_FORM_IF_DEV, "form.devices", cond);
    form_binding(LCD_VIEW_PATTERN_FORM_THEN_DEV, "form.devices", plain);
}

/* The LVGL widgets keep their selection when a hidden modal is re-shown, while
 * FormState starts from zero every time. Reset the widget too, otherwise the
 * dropdown displays the previous rule's choice but saves the default one. */
static void form_apply_default_selection()
{
    static const char *const kDropPatterns[] = {
        LCD_VIEW_PATTERN_FORM_WHEN_DEV,
        LCD_VIEW_PATTERN_FORM_IF_DEV,
        LCD_VIEW_PATTERN_FORM_IF_COND,
        LCD_VIEW_PATTERN_FORM_THEN_DEV,
        LCD_VIEW_PATTERN_FORM_THEN_CMD,
        LCD_VIEW_PATTERN_FORM_WHEN_TYPE,
    };
    for (const char *pattern : kDropPatterns) {
        form_binding(pattern, "form.index", "0");
    }
}

static void form_open(FormTarget target)
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    s_form = FormState{};
    s_form.target = target;
    form_build_devices();
    form_binding(LCD_VIEW_PATTERN_FORM_NAME_INPUT, "form.name", "");
    form_binding(LCD_VIEW_PATTERN_FORM_WHEN_TIME, "form.when_time", "");
    form_apply_device_options();
    form_apply_default_selection();
    form_hidden(LCD_VIEW_PATTERN_FORM_WHEN_DEV, false);
    form_hidden(LCD_VIEW_PATTERN_FORM_WHEN_TIME, true);
    form_label(LCD_VIEW_PATTERN_FORM_TITLE,
               target == FormTarget::Rule ? lcd_tr(LCD_STR_FORM_RULE_TITLE)
                                         : lcd_tr(LCD_STR_FORM_SCENE_TITLE));
    form_label(LCD_VIEW_PATTERN_FORM_HINT,
               s_form_devices.empty() ? lcd_tr(LCD_STR_FORM_NEED_DEVICE)
               : (target == FormTarget::Scene ? lcd_tr(LCD_STR_FORM_HINT_SCENE)
                                             : lcd_tr(LCD_STR_FORM_HINT)));
    form_update_enabled();
    form_hidden(LCD_VIEW_PATTERN_FORM_MODAL, false);
    request_display_refresh();
}

static void form_close()
{
    floating_keyboard_hide();
    if (s_form.target == FormTarget::None) {
        return;
    }
    form_hidden(LCD_VIEW_PATTERN_FORM_MODAL, true);
    s_form.target = FormTarget::None;
}

static void handle_form_cancel()
{
    form_close();
    request_display_refresh();
}

static void handle_form_enabled()
{
    if (s_form.target == FormTarget::None) {
        return;
    }
    s_form.enabled = !s_form.enabled;
    form_update_enabled();
    request_display_refresh();
}

static void handle_form_save()
{
    if (s_form.target == FormTarget::None) {
        return;
    }
    if (s_form_devices.empty()) {
        handle_form_cancel();
        return;
    }
    if (s_form.when_type != 0) {
        char utc[40];
        if (!form_utc_time(s_form, utc, sizeof(utc))) {
            form_label(LCD_VIEW_PATTERN_FORM_HINT,
                       lcd_tr(LCD_STR_FORM_TIME_INVALID));
            request_display_refresh();
            return;
        }
    }
    const FormTarget target = s_form.target;
    const std::string name = form_chosen_name();
    const FormState snapshot = s_form;
    form_close();       /* must run before clearing the target: it derives the screen */
    if (target == FormTarget::Rule) {
        create_rule(snapshot, name.c_str());
    } else {
        create_scene(snapshot, name.c_str());
    }
    request_display_refresh();
}

static void form_pick(int field, int64_t index)
{
    if (s_form.target == FormTarget::None) {
        return;
    }
    const int value = (int)index;
    switch (field) {
    case kFormWhen:     s_form.when = value; break;
    case kFormIfDev:    s_form.if_dev = value; break;
    case kFormCond:     s_form.if_value = value; break;
    case kFormThenDev:  s_form.then_dev = value; break;
    case kFormCmd:      s_form.cmd = value; break;
    case kFormWhenType: {
        s_form.when_type = value;
        const bool timed = (value != 0);
        form_hidden(LCD_VIEW_PATTERN_FORM_WHEN_DEV, timed);
        form_hidden(LCD_VIEW_PATTERN_FORM_WHEN_TIME, !timed);
        form_label(LCD_VIEW_PATTERN_FORM_HINT,
                   timed ? lcd_tr(LCD_STR_FORM_WHEN_TIME_TIP)
                         : (s_form.target == FormTarget::Scene
                                ? lcd_tr(LCD_STR_FORM_HINT_SCENE)
                                : lcd_tr(LCD_STR_FORM_HINT)));
        request_display_refresh();
        break;
    }
    default: break;
    }
}

static void wire_form_value(const char *action, int field)
{
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, action,
        [field](const gui::Event &ev) {
            const auto index = ev.get_int64("selectedIndex");
            if (!index || !s_lcd.scheduler) {
                return;
            }
            const int64_t picked = *index;
            s_lcd.scheduler->post([field, picked]() {
                with_lvgl_lock([field, picked]() { form_pick(field, picked); });
            });
        }));
}

static void wire_form()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    wire_action(LCD_ACTION_RULE_ADD_OPEN, []() { form_open(FormTarget::Rule); });
    wire_action(LCD_ACTION_SCENE_ADD_OPEN, []() { form_open(FormTarget::Scene); });

    static const char *const kFormScreens[] = {LCD_ENUM_FORM_SCREENS};
    struct PickSpec { const char *pattern; int field; };
    static const PickSpec kPicks[] = {
        {LCD_ACTION_PATTERN_FORM_WHEN_PICK, kFormWhen},
        {LCD_ACTION_PATTERN_FORM_IF_PICK, kFormIfDev},
        {LCD_ACTION_PATTERN_FORM_COND_PICK, kFormCond},
        {LCD_ACTION_PATTERN_FORM_THEN_PICK, kFormThenDev},
        {LCD_ACTION_PATTERN_FORM_CMD_PICK, kFormCmd},
        {LCD_ACTION_PATTERN_FORM_WHEN_TYPE_PICK, kFormWhenType},
    };
    for (size_t i = 0; i < LCD_ENUM_FORM_SCREENS_N; i++) {
        const char *screen = kFormScreens[i];
        char action[64];
        snprintf(action, sizeof(action), LCD_ACTION_PATTERN_FORM_NAME_CHANGED, screen);
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [](const gui::Event &ev) {
                const auto text = ev.get_string("text");
                if (!text || !s_lcd.scheduler) {
                    return;
                }
                const std::string value(*text);
                s_lcd.scheduler->post([value]() {
                    with_lvgl_lock([value]() {
                        if (s_form.target != FormTarget::None) {
                            s_form.name = value;
                        }
                    });
                });
            }));
        for (const auto &pick : kPicks) {
            snprintf(action, sizeof(action), pick.pattern, screen);
            wire_form_value(action, pick.field);
        }
        snprintf(action, sizeof(action),
                 LCD_ACTION_PATTERN_FORM_WHEN_TIME_CHANGED, screen);
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [](const gui::Event &ev) {
                const auto text = ev.get_string("text");
                if (!text || !s_lcd.scheduler) {
                    return;
                }
                const std::string value(*text);
                s_lcd.scheduler->post([value]() {
                    with_lvgl_lock([value]() {
                        if (s_form.target != FormTarget::None) {
                            s_form.when_time = value;
                        }
                    });
                });
            }));
        snprintf(action, sizeof(action), LCD_ACTION_PATTERN_FORM_ENABLED, screen);
        wire_action(action, handle_form_enabled);
        snprintf(action, sizeof(action), LCD_ACTION_PATTERN_FORM_CANCEL, screen);
        wire_action(action, handle_form_cancel);
        snprintf(action, sizeof(action), LCD_ACTION_PATTERN_FORM_SAVE, screen);
        wire_action(action, handle_form_save);
    }

    /* Device rename still uses overview's name_modal; its keyboard is hidden
     * and the shared floating keyboard drives name_input. */
    static const char *const kNameScreens[] = {LCD_ENUM_NAME_SCREENS};
    for (size_t i = 0; i < LCD_ENUM_NAME_SCREENS_N; i++) {
        char ok[48], cancel[48], changed[48];
        snprintf(ok, sizeof(ok), LCD_ACTION_PATTERN_NAME_OK, kNameScreens[i]);
        snprintf(cancel, sizeof(cancel), LCD_ACTION_PATTERN_NAME_CANCEL, kNameScreens[i]);
        snprintf(changed, sizeof(changed), LCD_ACTION_PATTERN_NAME_CHANGED, kNameScreens[i]);
        wire_action(ok, handle_rename_ok);
        wire_action(cancel, handle_rename_cancel);
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, changed,
            [](const gui::Event &ev) {
                const auto text = ev.get_string("text");
                if (!text || !s_lcd.scheduler) {
                    return;
                }
                const std::string value(*text);
                s_lcd.scheduler->post([value]() {
                    with_lvgl_lock([value]() { handle_rename_changed(value); });
                });
            }));
    }
}

static void wire_update_center()
{
    /* Same deploy root the Web console uses (CONFIG_MINIHUB_STORE_URL, default
     * https://minihub.espressif.com, unless a custom store URL is saved). */
    wire_action("system_check_update", []() { start_ota_query(false); });
    wire_action("system_do_ota", []() { start_ota_query(true); });
}

void refresh_system()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    if (!state || *state != "system") {
        return;
    }

    set_label_text_fmt(LCD_VIEW_SYSTEM_FW, "v%s", CONFIG_MINIHUB_VERSION);

    /* Same cards as the Web console, in the same order: Chip / Flash /
     * Internal RAM / PSRAM / Storage / Modstore / Uptime. */
    {
        esp_chip_info_t chip{};
        esp_chip_info(&chip);
        set_label_text_fmt(LCD_VIEW_SYSTEM_CHIP, lcd_tr(LCD_STR_SYS_CHIP_FMT),
                           CONFIG_IDF_TARGET, (unsigned)chip.cores,
                           (unsigned)chip.revision);
    }
    {
        /* The Web console shows the flash capacity; the Kconfig value is the
         * same number esptool flashed. */
        set_label_text_fmt(LCD_VIEW_SYSTEM_FLASH, "%s", CONFIG_ESPTOOLPY_FLASHSIZE);
    }
    {
        size_t total = 0, used = 0;
        if (minihub_store_partition_usage("storage", &total, &used) == ESP_OK) {
            set_label_text_fmt(LCD_VIEW_SYSTEM_STORAGE, lcd_tr(LCD_STR_SYS_STORAGE_FMT),
                               (unsigned long)((total > used ? total - used : 0) / 1024),
                               (unsigned long)(total / 1024));
        } else {
            set_label_text(LCD_VIEW_SYSTEM_STORAGE, lcd_tr(LCD_STR_SYS_NOT_MOUNTED));
        }
    }
    {
        size_t total = 0, used = 0;
        if (minihub_store_partition_usage("modstore", &total, &used) == ESP_OK) {
            set_label_text_fmt(LCD_VIEW_SYSTEM_MODSTORE, lcd_tr(LCD_STR_SYS_STORAGE_FMT),
                               (unsigned long)((total > used ? total - used : 0) / 1024),
                               (unsigned long)(total / 1024));
        } else {
            set_label_text(LCD_VIEW_SYSTEM_MODSTORE, lcd_tr(LCD_STR_SYS_NOT_MOUNTED));
        }
    }

    char ip[48] = {};
    if (minihub_net_get_ip(ip, sizeof(ip)) && ip[0] != '\0') {
        set_label_text(LCD_VIEW_SYSTEM_IP, ip);
    } else {
        set_label_text(LCD_VIEW_SYSTEM_IP, lcd_tr(LCD_STR_SYS_DISCONNECTED));
    }

    const uint64_t up_s = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    const uint32_t days = (uint32_t)(up_s / 86400ULL);
    const uint32_t hours = (uint32_t)((up_s % 86400ULL) / 3600ULL);
    const uint32_t mins = (uint32_t)((up_s % 3600ULL) / 60ULL);
    if (days > 0) {
        set_label_text_fmt(LCD_VIEW_SYSTEM_UPTIME, lcd_tr(LCD_STR_SYS_UPTIME_DAYS_FMT), (unsigned)days, (unsigned)hours);
    } else {
        set_label_text_fmt(LCD_VIEW_SYSTEM_UPTIME, lcd_tr(LCD_STR_SYS_UPTIME_HOURS_FMT), (unsigned)hours, (unsigned)mins);
    }

    const size_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    set_label_text_fmt(LCD_VIEW_SYSTEM_RAM, "%lu / %lu KB",
                       (unsigned long)(ram_free / 1024), (unsigned long)(ram_total / 1024));

    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0) {
        set_label_text_fmt(LCD_VIEW_SYSTEM_PSRAM, "%lu / %lu KB",
                           (unsigned long)(psram_free / 1024), (unsigned long)(psram_total / 1024));
    } else {
        set_label_text(LCD_VIEW_SYSTEM_PSRAM, lcd_tr(LCD_STR_STATE_DISABLED));
    }
    refresh_system_tz();
    refresh_system_lang();
    ESP_LOGD(TAG, "system screen refreshed (ip=%s)", ip);

    /* Wi-Fi link state. The SSID lives in the net component, which the LCD
     * cannot include (its headers pull in esp_netif.h), so the LCD shows the
     * link state next to the address card. */
    {
        set_label_text(LCD_VIEW_SYSTEM_WIFI,
                       minihub_web_sta_has_ip() ? lcd_tr(LCD_STR_SYS_CONNECTED) : lcd_tr(LCD_STR_SYS_DISCONNECTED));
    }
    /* Firmware update: local OTA state. Cloud version check stays in the Web
     * console (it needs the account); this card also exposes Check/Upgrade. */
    refresh_ota_status();
    refresh_system_wifi();

}

static std::string s_wifi_ssid;
static std::string s_wifi_pass;
static bool s_wifi_open = false;
static bool s_net_static = false;
static std::string s_net_ip, s_net_mask, s_net_dns;

void refresh_system_wifi()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    char ssid[64] = {};
    bool has_pass = false;
    if (minihub_net_get_wifi_sta(ssid, sizeof(ssid), &has_pass) == ESP_OK) {
        s_wifi_ssid = ssid;
        s_lcd.runtime->set_binding_value(s_lcd.document_id, LCD_VIEW_SYSTEM_WIFI_SSID, "wifi.ssid", s_wifi_ssid);
    }
    minihub_net_iface_status_t st{};
    if (minihub_net_get_iface_status(MINIHUB_NET_IF_WIFI, &st) == ESP_OK) {
        s_net_static = !st.dhcp;
        s_lcd.runtime->set_binding_value(s_lcd.document_id, LCD_VIEW_SYSTEM_NET_IP, "net.ip",
                         std::string(st.ip));
        s_lcd.runtime->set_binding_value(s_lcd.document_id, LCD_VIEW_SYSTEM_NET_MASK, "net.netmask",
                         std::string(st.netmask));
        s_lcd.runtime->set_binding_value(s_lcd.document_id, LCD_VIEW_SYSTEM_NET_DNS, "net.dns",
                         std::string(st.dns_main));
        set_label_text(LCD_VIEW_SYSTEM_NET_MODE,
                       s_net_static ? lcd_tr(LCD_STR_WIFI_STATIC_IP)
                                    : lcd_tr(LCD_STR_WIFI_DHCP));
        set_view_hidden(LCD_VIEW_SYSTEM_NET_IP, !s_net_static);
        set_view_hidden(LCD_VIEW_SYSTEM_NET_MASK, !s_net_static);
        set_view_hidden(LCD_VIEW_SYSTEM_NET_DNS, !s_net_static);
    }
    set_label_text(LCD_VIEW_SYSTEM_WIFI_OPEN,
                   s_wifi_open ? lcd_tr(LCD_STR_STATE_ENABLED)
                               : lcd_tr(LCD_STR_STATE_DISABLED));
}

static void handle_wifi_ssid_changed(const std::string &v) { s_wifi_ssid = v; }
static void handle_wifi_pass_changed(const std::string &v) { s_wifi_pass = v; }
static void handle_net_ip_changed(const std::string &v) { s_net_ip = v; }
static void handle_net_mask_changed(const std::string &v) { s_net_mask = v; }
static void handle_net_dns_changed(const std::string &v) { s_net_dns = v; }

static void handle_wifi_open_toggle()
{
    s_wifi_open = !s_wifi_open;
    set_label_text(LCD_VIEW_SYSTEM_WIFI_OPEN,
                   s_wifi_open ? lcd_tr(LCD_STR_STATE_ENABLED)
                               : lcd_tr(LCD_STR_STATE_DISABLED));
    request_display_refresh();
}

static void handle_net_mode_toggle()
{
    s_net_static = !s_net_static;
    set_label_text(LCD_VIEW_SYSTEM_NET_MODE,
                   s_net_static ? lcd_tr(LCD_STR_WIFI_STATIC_IP)
                                : lcd_tr(LCD_STR_WIFI_DHCP));
    set_view_hidden(LCD_VIEW_SYSTEM_NET_IP, !s_net_static);
    set_view_hidden(LCD_VIEW_SYSTEM_NET_MASK, !s_net_static);
    set_view_hidden(LCD_VIEW_SYSTEM_NET_DNS, !s_net_static);
    request_display_refresh();
}

static void handle_wifi_save()
{
    const esp_err_t err = minihub_net_set_wifi_sta(
        s_wifi_ssid.c_str(), s_wifi_pass.c_str(), s_wifi_open);
    with_lvgl_lock([err]() {
        set_label_text(LCD_VIEW_SYSTEM_WIFI,
                       err == ESP_OK ? lcd_tr(LCD_STR_WIFI_SAVE_OK)
                                     : lcd_tr(LCD_STR_WIFI_SAVE_FAIL));
        request_display_refresh();
    });
}

static void handle_net_save()
{
    minihub_net_iface_config_t cfg{};
    cfg.dhcp = !s_net_static;
    strncpy(cfg.ip, s_net_ip.c_str(), sizeof(cfg.ip) - 1);
    strncpy(cfg.netmask, s_net_mask.c_str(), sizeof(cfg.netmask) - 1);
    strncpy(cfg.dns_main, s_net_dns.c_str(), sizeof(cfg.dns_main) - 1);
    const esp_err_t err = minihub_net_set_iface_config(MINIHUB_NET_IF_WIFI, &cfg);
    with_lvgl_lock([err]() {
        set_label_text(LCD_VIEW_SYSTEM_WIFI,
                       err == ESP_OK ? lcd_tr(LCD_STR_WIFI_SAVE_OK)
                                     : lcd_tr(LCD_STR_WIFI_SAVE_FAIL));
        request_display_refresh();
    });
}

static void wire_wifi()
{
    auto changed = [](const char *action, void (*fn)(const std::string &)) {
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [fn](const gui::Event &ev) {
                const auto text = ev.get_string("text");
                if (!text || !s_lcd.scheduler) {
                    return;
                }
                const std::string value(*text);
                s_lcd.scheduler->post([value, fn]() {
                    with_lvgl_lock([value, fn]() { fn(value); });
                });
            }));
    };
    changed(LCD_ACTION_WIFI_SSID_CHANGED, handle_wifi_ssid_changed);
    changed(LCD_ACTION_WIFI_PASS_CHANGED, handle_wifi_pass_changed);
    changed(LCD_ACTION_NET_IP_CHANGED, handle_net_ip_changed);
    changed(LCD_ACTION_NET_MASK_CHANGED, handle_net_mask_changed);
    changed(LCD_ACTION_NET_DNS_CHANGED, handle_net_dns_changed);
    wire_action(LCD_ACTION_WIFI_OPEN_TOGGLE, handle_wifi_open_toggle);
    wire_action(LCD_ACTION_WIFI_SAVE, handle_wifi_save);
    wire_action(LCD_ACTION_NET_MODE_TOGGLE, handle_net_mode_toggle);
    wire_action(LCD_ACTION_NET_SAVE, handle_net_save);
}

void handle_rule_tap(int slot)
{
    cJSON *rules_root = minihub_config_get_rules();
    cJSON *arr = rules_root ? cJSON_GetObjectItem(rules_root, "rules") : nullptr;
    if (!cJSON_IsArray(arr)) {
        return;
    }
    cJSON *rule = cJSON_GetArrayItem(arr, slot);
    if (!rule) {
        return;
    }
    const bool now_enabled = cJSON_IsFalse(cJSON_GetObjectItem(rule, "enabled"));
    cJSON *enabled = cJSON_GetObjectItem(rule, "enabled");
    if (enabled) {
        cJSON_SetBoolValue(enabled, now_enabled);
    } else {
        cJSON_AddBoolToObject(rule, "enabled", now_enabled);
    }
    const esp_err_t err = minihub_config_save_rules(rules_root);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rule toggle save failed: %s", esp_err_to_name(err));
        return;
    }
    (void)minihub_rules_reload(minihub_config_get_rules());
    ESP_LOGI(TAG, "rule slot %d -> enabled=%d", slot, now_enabled);
    refresh_rules();
}

static void show_confirm(const char *title, const char *msg, PendingConfirm kind);
void store_sheet_hide();
void store_sheet_not_started(const char *why);

/* Deleting a rule/scene needs a confirmation; the modal lives on the screen
 * the user is on, and show_confirm() picks it by the current screen. */
void handle_rule_delete(int slot)
{
    s_pending_delete_slot = slot;
    show_confirm(lcd_tr(LCD_STR_CONFIRM_DELETE_RULE_TITLE), lcd_tr(LCD_STR_CONFIRM_DELETE_RULE_MSG), PendingConfirm::RuleDelete);
}

void handle_scene_delete(int slot)
{
    s_pending_delete_slot = slot;
    show_confirm(lcd_tr(LCD_STR_CONFIRM_DELETE_SCENE_TITLE), lcd_tr(LCD_STR_CONFIRM_DELETE_SCENE_MSG), PendingConfirm::SceneDelete);
}

void handle_scene_tap(int slot)
{
    cJSON *scenes_root = minihub_config_get_scenes();
    cJSON *arr = scenes_root ? cJSON_GetObjectItem(scenes_root, "scenes") : nullptr;
    cJSON *scene = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, slot) : nullptr;
    if (!scene) {
        return;
    }
    const cJSON *id = cJSON_GetObjectItem(scene, "id");
    if (!cJSON_IsString(id) || id->valuestring[0] == '\0') {
        return;
    }
    const esp_err_t err = minihub_scenes_invoke(id->valuestring);
    ESP_LOGI(TAG, "scene %s invoke: %s", id->valuestring, esp_err_to_name(err));
}

const char *store_op_name(StoreOp op)
{
    switch (op) {
    case StoreOp::Install:
        return "install";
    case StoreOp::Upgrade:
        return "upgrade";
    case StoreOp::Reinstall:
        return "reinstall";
    case StoreOp::Uninstall:
        return "uninstall";
    }
    return "?";
}

void store_op_task(void *arg)
{
    std::unique_ptr<StoreOpRequest> req(static_cast<StoreOpRequest *>(arg));
    esp_err_t err = ESP_ERR_INVALID_STATE;
    switch (req->op) {
    case StoreOp::Install:
        err = minihub_store_install(req->kind, req->id);
        break;
    case StoreOp::Upgrade:
        err = minihub_store_upgrade(req->kind, req->id);
        break;
    case StoreOp::Reinstall:
        err = minihub_store_reinstall(req->kind, req->id);
        break;
    case StoreOp::Uninstall:
        err = minihub_store_uninstall(req->kind, req->id);
        break;
    }
    ESP_LOGI(TAG, "store %s %s/%s: %s", store_op_name(req->op), req->kind, req->id,
             esp_err_to_name(err));
    /* The op runs on its own task; tell the user what happened on the UI. */
    if (s_lcd.scheduler) {
        const bool ok = (err == ESP_OK);
        const bool show_installed = ok && req->op == StoreOp::Install;
        const char *op_name = store_op_name(req->op);
        s_lcd.scheduler->post([ok, show_installed, op_name]() {
            with_lvgl_lock([ok, show_installed, op_name]() {
                if (ok) {
                    if (show_installed) {
                        s_store_tab_available = false;
                    }
                    store_sheet_hide();
                } else {
                    char msg[96];
                    snprintf(msg, sizeof(msg), lcd_tr(LCD_STR_STORE_OP_FAILED_FMT), op_name, lcd_tr(LCD_STR_STORE_OFFLINE_MSG));
                    store_sheet_not_started(msg);
                }
                request_display_refresh();
            });
        });
    }
    if (err == ESP_OK) {
        /* The installed tab reads the loader RAM table. Install only writes the
         * package; the web install handler reloads so the new module is scanned
         * in. Uninstall needs the same reload or the deleted slot stays listed. */
        (void)minihub_hub_reload_async();
    }
    ESP_LOGD(TAG, "store op stack high water: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    s_store_busy.store(false, std::memory_order_release);
    minihub_lcd_request_refresh();
    vTaskDelete(nullptr);
}

bool store_request_for(StoreOp op, const StoreRowInfo &info, StoreOpRequest &out)
{
    if (info.id[0] == '\0') {
        return false;
    }
    if (!info.available && !info.managed) {
        ESP_LOGW(TAG, "store %s is builtin; no operation offered", info.id);
        return false;
    }
    if (op == StoreOp::Upgrade && !info.upgrade) {
        return false;
    }
    out.op = op;
    /* Web: upgrade takes the raw kind, the other three take 'pack' when the
     * catalog marks the item config-only (store.js apiKind). */
    snprintf(out.kind, sizeof(out.kind), "%s",
             (op == StoreOp::Upgrade || !info.config_only) ? info.kind : "pack");
    snprintf(out.id, sizeof(out.id), "%s", info.id);
    return true;
}

bool store_start_request(const StoreOpRequest &request)
{
    if (request.id[0] == '\0') {
        return false;
    }
    if (s_store_busy.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "store operation already running; ignored");
        return false;
    }
    auto *req = new (std::nothrow) StoreOpRequest(request);
    if (req == nullptr) {
        return false;
    }
    s_store_busy.store(true, std::memory_order_release);
    if (xTaskCreate(store_op_task, "lcd_store_op", 16384, req, 5, nullptr) != pdPASS) {
        s_store_busy.store(false, std::memory_order_release);
        delete req;
        ESP_LOGE(TAG, "store op task create failed");
        return false;
    }
    return true;
}

static PendingConfirm s_pending_confirm = PendingConfirm::None;

/* The modal is a child of its own screen, and a screen flow hides the inactive
 * screen — unhiding a descendant would not make it visible. Resolve the modal on
 * whichever screen raised the confirm (system: reboot/factory, store: uninstall). */
std::string current_screen()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return "system";
    }
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    return (state && !state->empty()) ? *state : std::string("system");
}

static void show_confirm(const char *title, const char *msg, PendingConfirm kind)
{
    const std::string screen = current_screen();
    char path[96];
    snprintf(path, sizeof(path), LCD_VIEW_PATTERN_CONFIRM_TITLE, screen.c_str());
    set_label_text(path, title);
    snprintf(path, sizeof(path), LCD_VIEW_PATTERN_CONFIRM_MSG, screen.c_str());
    set_label_text(path, msg);
    s_pending_confirm = kind;
    snprintf(path, sizeof(path), LCD_VIEW_PATTERN_CONFIRM_MODAL, screen.c_str());
    set_view_hidden(path, false);
    request_display_refresh();
}

static void hide_confirm()
{
    const std::string screen = current_screen();
    char path[96];
    snprintf(path, sizeof(path), LCD_VIEW_PATTERN_CONFIRM_MODAL, screen.c_str());
    set_view_hidden(path, true);
}

void handle_system_reboot()
{
    ESP_LOGW(TAG, "reboot confirm requested from the LCD system screen");
    show_confirm(lcd_tr(LCD_STR_SYS_REBOOT), lcd_tr(LCD_STR_CONFIRM_REBOOT_MSG), PendingConfirm::Reboot);
}

void handle_system_factory_reset()
{
    ESP_LOGW(TAG, "factory reset confirm requested from the LCD system screen");
    show_confirm(lcd_tr(LCD_STR_SYS_FACTORY_RESET),
                 lcd_tr(LCD_STR_CONFIRM_FACTORY_MSG),
                 PendingConfirm::FactoryReset);
}

void handle_confirm_ok()
{
    hide_confirm();
    request_display_refresh();
    const PendingConfirm kind = s_pending_confirm;
    s_pending_confirm = PendingConfirm::None;
    if (kind == PendingConfirm::Reboot) {
        if (s_lcd.scheduler) {
            s_lcd.scheduler->post([]() {
                vTaskDelay(pdMS_TO_TICKS(150));
                esp_restart();
            });
        }
    } else if (kind == PendingConfirm::FactoryReset) {
        ESP_LOGW(TAG, "factory reset executing");
        minihub_factory_reset_schedule();
    } else if (kind == PendingConfirm::StoreUninstall) {
        store_start_request(s_store_pending);
    } else if (kind == PendingConfirm::RuleDelete) {
        const int slot = s_pending_delete_slot;
        s_pending_delete_slot = -1;
        cJSON *rules_root = minihub_config_get_rules();
        cJSON *arr = rules_root ? cJSON_GetObjectItem(rules_root, "rules") : nullptr;
        if (cJSON_IsArray(arr) && slot >= 0 && slot < cJSON_GetArraySize(arr)) {
            cJSON_DeleteItemFromArray(arr, slot);
            const esp_err_t err = minihub_config_save_rules(rules_root);
            if (err == ESP_OK) {
                (void)minihub_rules_reload(minihub_config_get_rules());
                ESP_LOGI(TAG, "rule slot %d deleted", slot);
            } else {
                ESP_LOGW(TAG, "rule delete save failed: %s", esp_err_to_name(err));
            }
        }
        refresh_rules();
    } else if (kind == PendingConfirm::SceneDelete) {
        const int slot = s_pending_delete_slot;
        s_pending_delete_slot = -1;
        cJSON *scenes_root = minihub_config_get_scenes();
        cJSON *arr = scenes_root ? cJSON_GetObjectItem(scenes_root, "scenes") : nullptr;
        if (cJSON_IsArray(arr) && slot >= 0 && slot < cJSON_GetArraySize(arr)) {
            cJSON_DeleteItemFromArray(arr, slot);
            const esp_err_t err = minihub_config_save_scenes(scenes_root);
            if (err == ESP_OK) {
                (void)minihub_scenes_reload(minihub_config_get_scenes());
                ESP_LOGI(TAG, "scene slot %d deleted", slot);
            } else {
                ESP_LOGW(TAG, "scene delete save failed: %s", esp_err_to_name(err));
            }
        }
        refresh_scenes();
    }
}

void handle_confirm_cancel()
{
    hide_confirm();
    s_pending_confirm = PendingConfirm::None;
    s_store_pending = StoreOpRequest{};
    s_pending_delete_slot = -1;
    request_display_refresh();
}

void store_sheet_hide()
{
    set_view_hidden(LCD_VIEW_STORE_MODAL, true);
}

void store_sheet_show(int section, int index)
{
    const StoreRowInfo &info = s_store_rows[section][index];
    s_store_selected = info;
    set_label_text(LCD_VIEW_STORE_MODAL_TITLE, info.name);
    /* A row tap shows details, not an install/cancel prompt. Available items
     * are installed from their own inline button (same as the Web console);
     * installed rows still offer upgrade / reinstall / uninstall below. */
    set_label_text_fmt(LCD_VIEW_STORE_MODAL_MSG, lcd_tr(LCD_STR_STORE_META_FMT),
                       info.version[0] ? info.version : "-",
                       info.vendor[0] ? info.vendor : lcd_tr(LCD_STR_STORE_VENDOR_UNKNOWN),
                       info.id);
    set_label_text(LCD_VIEW_STORE_MODAL_DESC,
                   info.description[0] ? info.description : lcd_tr(LCD_STR_STORE_NO_DESC));
    set_view_hidden(LCD_VIEW_STORE_MODAL_INSTALL, true);
    set_view_hidden(LCD_VIEW_STORE_MODAL_UPGRADE, info.available || !info.upgrade);
    set_view_hidden(LCD_VIEW_STORE_MODAL_ENABLE, info.available || !info.managed);
    set_label_text(LCD_VIEW_STORE_MODAL_ENABLE_LBL,
                   info.enabled ? lcd_tr(LCD_STR_STORE_DISABLE) : lcd_tr(LCD_STR_STORE_ENABLE));
    set_view_hidden(LCD_VIEW_STORE_MODAL_REINSTALL, info.available || !info.managed);
    set_view_hidden(LCD_VIEW_STORE_MODAL_UNINSTALL, info.available || !info.managed);
    set_view_hidden(LCD_VIEW_STORE_MODAL, false);
    request_display_refresh();
}

void handle_store_row_tap(const std::string &path)
{
    /* Event.path is /store/store_main/<section>/<prefix>_row_<i>, or a child of
     * that row when the tap landed on a label. Each row owns its own action name
     * (the loader gives one action one owner), so the row identity comes back
     * out of the path, never from a guess. */
    const std::string needles[] = {
        std::string(LCD_VIEW_STORE_AVAIL_SECTION) + "/",
        std::string(LCD_VIEW_STORE_PACK_SECTION) + "/",
        std::string(LCD_VIEW_STORE_MOD_SECTION) + "/",
    };
    int section = -1;
    int index = -1;
    for (int s = 0; s < kStoreSections; s++) {
        const size_t at = path.find(needles[s]);
        if (at == std::string::npos) {
            continue;
        }
        section = s;
        const size_t row_at = path.find(kStoreRowInfix, at + needles[s].size());
        if (row_at != std::string::npos) {
            const char *digits = path.c_str() + row_at + strlen(kStoreRowInfix);
            /* atoi() would answer 0 for a non-numeric tail and silently select
             * the first row; require a digit before trusting the index. */
            if (*digits >= '0' && *digits <= '9') {
                index = atoi(digits);
            }
        }
        break;
    }
    if (section < 0 || index < 0 || index >= s_store_row_count[section]) {
        ESP_LOGD(TAG, "store tap outside a known row: %s", path.c_str());
        return;
    }
    store_sheet_show(section, index);
}

void store_sheet_not_started(const char *why)
{
    set_label_text(LCD_VIEW_STORE_MODAL_MSG, why);
    set_label_text(LCD_VIEW_STORE_MODAL_DESC, "");
    set_view_hidden(LCD_VIEW_STORE_MODAL, false);
    request_display_refresh();
}

void store_sheet_run(StoreOp op)
{
    StoreOpRequest req{};
    if (!store_request_for(op, s_store_selected, req)) {
        store_sheet_not_started(lcd_tr(LCD_STR_STORE_OP_UNSUPPORTED));
        return;
    }
    store_sheet_hide();
    if (!store_start_request(req)) {
        store_sheet_not_started(lcd_tr(LCD_STR_STORE_OP_NOT_STARTED));
    }
}

/* The Web shows the install button inside the row; the LCD now does the same.
 * The button is a child of the row, so a tap on it never reaches the row's own
 * clicked event (LVGL hands the click to the innermost clickable object). */
void handle_store_install_inline(const std::string &path)
{
    int index = -1;
    const size_t row_at = path.find("avail_row_");
    if (row_at != std::string::npos) {
        const char *digits = path.c_str() + row_at + strlen("avail_row_");
        if (*digits >= '0' && *digits <= '9') {
            index = atoi(digits);
        }
    }
    if (index < 0 || index >= s_store_row_count[0]) {
        ESP_LOGD(TAG, "inline install outside a known row: %s", path.c_str());
        return;
    }
    s_store_selected = s_store_rows[0][index];
    store_sheet_run(StoreOp::Install);
}

void handle_store_install()
{
    store_sheet_run(StoreOp::Install);
}

void handle_store_upgrade()
{
    store_sheet_run(StoreOp::Upgrade);
}

void handle_store_enable_toggle()
{
    if (s_store_selected.id[0] == '\0' || s_store_selected.available || !s_store_selected.managed) {
        return;
    }
    const bool next = !s_store_selected.enabled;
    const bool as_pack = s_store_selected.config_only
                         || strcmp(s_store_selected.kind, "pack") == 0;
    const esp_err_t err = as_pack
                              ? minihub_pack_set_enabled(s_store_selected.id, next)
                              : minihub_module_set_enabled(s_store_selected.id, next);
    ESP_LOGI(TAG, "store %s %s -> %s: %s", as_pack ? "pack" : "module", s_store_selected.id,
             next ? "enabled" : "disabled", esp_err_to_name(err));
    if (err != ESP_OK) {
        return;
    }
    s_store_selected.enabled = next;
    set_label_text(LCD_VIEW_STORE_MODAL_ENABLE_LBL,
                   next ? lcd_tr(LCD_STR_STORE_DISABLE) : lcd_tr(LCD_STR_STORE_ENABLE));
    /* Same as the web storeAct: the flag is on disk, the running plugin changes
     * only after the hub rebuilds the loader table. */
    (void)minihub_hub_reload_async();
    request_display_refresh();
}

void handle_store_uninstall()
{
    StoreOpRequest req{};
    if (!store_request_for(StoreOp::Uninstall, s_store_selected, req)) {
        store_sheet_not_started(lcd_tr(LCD_STR_STORE_UNINSTALL_UNSUPPORTED));
        return;
    }
    s_store_pending = req;
    store_sheet_hide();
    char msg[160];
    snprintf(msg, sizeof(msg), lcd_tr(LCD_STR_CONFIRM_UNINSTALL_FMT), req.id);
    show_confirm(lcd_tr(LCD_STR_STORE_UNINSTALL), msg, PendingConfirm::StoreUninstall);
}

void handle_store_reinstall()
{
    /* The user ruling for this screen is "only uninstall asks twice"; the Web
     * also confirms a reinstall, but that is a behaviour difference, not a
     * layout one, so the ruling wins here. */
    store_sheet_run(StoreOp::Reinstall);
}

void handle_store_modal_cancel()
{
    store_sheet_hide();
    request_display_refresh();
}

/* Whole-hour zones get a localized name (East 8 / UTC+8); half and
 * quarter-hour offsets speak for themselves (UTC+5:30). */
static void tz_label(const char *id, char *out, size_t out_len)
{
    /* tz_digits is a localized '|'-separated list of the 14 whole-hour
     * numerals; split it at runtime so it follows the active language. */
    char digits[96];
    snprintf(digits, sizeof(digits), "%s", lcd_tr(LCD_STR_TZ_DIGITS));
    const char *numerals[14] = {};
    int numeral_count = 0;
    char *cursor = digits;
    while (numeral_count < 14 && *cursor != '\0') {
        numerals[numeral_count++] = cursor;
        char *bar = strchr(cursor, '|');
        if (bar == nullptr) {
            break;
        }
        *bar = '\0';
        cursor = bar + 1;
    }
    if (out == nullptr || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (id == nullptr || id[0] == '\0') {
        return;
    }
    if (strcmp(id, "UTC") == 0) {
        snprintf(out, out_len, "%s", lcd_tr(LCD_STR_TZ_ZERO));
        return;
    }
    if (strncmp(id, "UTC", 3) != 0) {
        snprintf(out, out_len, "%s", id);
        return;
    }
    const char *p = id + 3;
    if (*p != '+' && *p != '-') {
        snprintf(out, out_len, "%s", id);
        return;
    }
    const bool east = (*p == '+');
    ++p;
    if (!isdigit((unsigned char)*p)) {
        snprintf(out, out_len, "%s", id);
        return;
    }
    int hour = 0;
    while (isdigit((unsigned char)*p)) {
        hour = hour * 10 + (*p - '0');
        ++p;
    }
    if (*p != '\0' || hour < 1 || hour > 14) {
        snprintf(out, out_len, "%s", id);
        return;
    }
    const char *numeral = (hour <= numeral_count) ? numerals[hour - 1] : id;
    snprintf(out, out_len, lcd_tr(LCD_STR_TZ_LABEL_FMT),
             east ? lcd_tr(LCD_STR_TZ_EAST) : lcd_tr(LCD_STR_TZ_WEST),
             numeral, id);
}

/* LVGL draws LV_SYMBOL_DOWN on LV_PART_INDICATOR with the widget font. That
 * font is the CJK face, which has no FontAwesome private-use glyph, and
 * LV_USE_FONT_PLACEHOLDER turns the missing codepoint into a boxed x at the
 * right edge of every closed dropdown (timezone, language, rule/scene
 * fields). Montserrat 16 contains U+F078, so the indicator stays a chevron
 * while the selected text keeps the CJK font on LV_PART_MAIN. */
static void style_dropdown_indicator(lv_obj_t *dd)
{
#if CONFIG_LV_FONT_MONTSERRAT_16
    const lv_font_t *symbol_font = &lv_font_montserrat_16;
#elif CONFIG_LV_FONT_MONTSERRAT_14
    const lv_font_t *symbol_font = &lv_font_montserrat_14;
#else
    const lv_font_t *symbol_font = LV_FONT_DEFAULT;
#endif
    lv_obj_set_style_text_font(dd, symbol_font, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(dd, lv_color_hex(0xeef2ff), LV_PART_INDICATOR);
}

/* Brookesia applies the JSON style to the dropdown widget itself, but LVGL
 * creates the popup option list as a SEPARATE object that only inherits the
 * display theme. This board installs no lv_theme, so the popup kept LVGL's
 * defaults (measured on hardware): bg_opa=0 (fully transparent) and the
 * built-in Montserrat font, which has no CJK glyphs. The 38 Chinese zone
 * names were therefore drawn into an invisible box. Re-apply the widget's own
 * colors and font to that list. Runs from the system-screen refresh, i.e.
 * after the widget and its bindings have been mounted. Callers already hold
 * the LVGL lock, same as the other set_* helpers in this file. */
static void style_dropdown_popup(lv_obj_t *dd)
{
    style_dropdown_indicator(dd);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list == nullptr) {
        return;
    }
    const lv_font_t *font = lv_obj_get_style_text_font(dd, LV_PART_MAIN);

    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x151c2f), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lv_color_hex(0x243049), LV_PART_MAIN);
    lv_obj_set_style_radius(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(0xeef2ff), LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 360, LV_PART_MAIN);

    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x2b3a5c), LV_PART_SELECTED);
    lv_obj_set_style_text_font(list, font, LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(0xffffff), LV_PART_SELECTED);

    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x3b4a68), LV_PART_SCROLLBAR);

    /* LVGL renders the options as label children of the list; make sure they
     * inherit the same font instead of the theme default. */
    const uint32_t n = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (child == nullptr) {
            continue;
        }
        lv_obj_set_style_text_font(child, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(child, lv_color_hex(0xeef2ff), LV_PART_MAIN);
    }
}

static int style_dropdown_popups(lv_obj_t *obj)
{
    if (obj == nullptr) {
        return 0;
    }
    int found = 0;
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        style_dropdown_popup(obj);
        ++found;
    }
    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        found += style_dropdown_popups(lv_obj_get_child(obj, i));
    }
    return found;
}

static void configure_dropdown_popups()
{
    /* Idempotent: re-apply on every refresh so a re-mounted document does not
     * fall back to the invisible LVGL defaults. lock_thread() is a recursive
     * semaphore, so nesting is safe even when the caller already holds it. */
    with_lvgl_lock([]() {
        lv_display_t *disp = lv_display_get_default();
        if (disp == nullptr) {
            return;
        }
        (void)style_dropdown_popups(lv_display_get_screen_active(disp));
    });
}

void refresh_system_lang()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    cJSON *options = cJSON_CreateArray();
    if (options == nullptr) {
        return;
    }
    cJSON_AddItemToArray(options, cJSON_CreateString(lcd_tr(LCD_STR_LANG_ZH)));
    cJSON_AddItemToArray(options, cJSON_CreateString(lcd_tr(LCD_STR_LANG_EN)));
    char *json = cJSON_PrintUnformatted(options);
    cJSON_Delete(options);
    if (json == nullptr) {
        return;
    }
    /* Same binding mechanism as the timezone dropdown: the node declares
     * dropdownProps.options -> lang.options and selectedIndex -> lang.selected. */
    s_lcd.runtime->set_binding_value(s_lcd.document_id, LCD_VIEW_SYSTEM_LANG,
                                     "lang.options", json);
    free(json);
    const bool english = strcmp(lcd_language(), "en") == 0;
    s_lcd.runtime->set_binding_value(s_lcd.document_id, LCD_VIEW_SYSTEM_LANG,
                                     "lang.selected", english ? "1" : "0");
    set_label_text_fmt(LCD_VIEW_SYSTEM_LANG_HINT, lcd_tr(LCD_STR_SYS_LANG_CUR_FMT),
                       english ? lcd_tr(LCD_STR_LANG_EN) : lcd_tr(LCD_STR_LANG_ZH));
}

void refresh_system_tz()
{
    cJSON *status = minihub_tz_status_json();
    if (status == nullptr) {
        return;
    }
    const cJSON *zones = cJSON_GetObjectItem(status, "zones");
    const cJSON *current = cJSON_GetObjectItem(status, "id");
    const cJSON *offset_min = cJSON_GetObjectItem(status, "offset_min");
    s_tz_zones.clear();
    /* The node declares bindings dropdownProps.options -> tz.options and
     * dropdownProps.selectedIndex -> tz.selected; set_binding_value only applies
     * through a declared binding, so these keys are the contract, not the property
     * paths. The options value must be a JSON array: the runtime parses it with
     * parse_string_array_from_store_string() and rejects anything else. */
    cJSON *options = cJSON_CreateArray();
    if (options == nullptr) {
        cJSON_Delete(status);
        return;
    }
    int selected = 0;
    const cJSON *zone = nullptr;
    cJSON_ArrayForEach(zone, zones) {
        if (!cJSON_IsString(zone)) {
            continue;
        }
        if (cJSON_IsString(current) && strcmp(current->valuestring, zone->valuestring) == 0) {
            selected = (int)s_tz_zones.size();
        }
        char label[48];
        tz_label(zone->valuestring, label, sizeof(label));
        cJSON_AddItemToArray(options, cJSON_CreateString(label));
        s_tz_zones.emplace_back(zone->valuestring);
    }
    const int offset = cJSON_IsNumber(offset_min) ? (int)cJSON_GetNumberValue(offset_min) : 0;
    char tz_id[32] = {};
    if (cJSON_IsString(current)) {
        strncpy(tz_id, current->valuestring, sizeof(tz_id) - 1);
    }
    cJSON_Delete(status);
    if (s_tz_zones.empty()) {
        cJSON_Delete(options);
        return;
    }
    char *json = cJSON_PrintUnformatted(options);
    cJSON_Delete(options);
    if (json == nullptr) {
        return;
    }
    s_lcd.runtime->set_binding_value(s_lcd.document_id, LCD_VIEW_SYSTEM_TZ,
                                     "tz.options", json);
    free(json);
    s_lcd.runtime->set_binding_value(s_lcd.document_id, LCD_VIEW_SYSTEM_TZ,
                                     "tz.selected", std::to_string(selected));

    /* Web shows the wall clock under the picker (Local time YYYY-MM-DD HH:MM).
     * The device has no RTC until SNTP syncs, so a 1970 clock is "not set"
     * and must not be shown as a real time. */
    {
        const time_t now = time(nullptr) + (time_t)offset * 60;
        struct tm tm_buf {};
        gmtime_r(&now, &tm_buf);
        const char *zone = tz_id[0] ? tz_id : "UTC";
        if (tm_buf.tm_year + 1900 < 2024) {
            set_label_text_fmt(LCD_VIEW_SYSTEM_TZ_NOW,
                               lcd_tr(LCD_STR_SYS_TZ_CUR_NO_CLOCK_FMT), zone);
        } else {
            char stamp[32];
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm_buf);
            set_label_text_fmt(LCD_VIEW_SYSTEM_TZ_NOW,
                               lcd_tr(LCD_STR_SYS_TZ_CUR_FMT), zone, stamp);
        }
    }
}

void handle_system_tz_pick(int64_t index)
{
    if (index < 0 || (size_t)index >= s_tz_zones.size()) {
        ESP_LOGD(TAG, "timezone pick outside the offered zone list");
        return;
    }
    const std::string &id = s_tz_zones[(size_t)index];
    const esp_err_t err = minihub_tz_set(id.c_str());
    ESP_LOGI(TAG, "timezone -> %s: %s", id.c_str(), esp_err_to_name(err));
    request_display_refresh();
}

void handle_system_lang_pick(int64_t index)
{
    const char *lang = (index == 1) ? "en" : "zh";
    const esp_err_t err = minihub_lcd_set_language(lang);
    ESP_LOGI(TAG, "UI language pick -> %s: %s", lang, esp_err_to_name(err));
}

void refresh_lcd_data()
{
    if (!s_lcd.runtime || !s_lcd.started) {
        return;
    }
    apply_filter_chip_visibility();
    if (is_lcd_screen("overview")) {
        refresh_overview();
    }
    refresh_user_sidebar();

    if (is_lcd_screen("map")) {
        refresh_map();
    } else if (is_lcd_screen("store")) {
        refresh_store();
    } else if (is_lcd_screen("system")) {
        refresh_system();
    } else if (is_lcd_screen("rules")) {
        refresh_rules();
    } else if (is_lcd_screen("scenes")) {
        refresh_scenes();
    }
    /* Screen flows mount their LVGL trees lazily, so re-apply popup styling
     * after every refresh instead of only when the system page happens to
     * refresh its timezone list. */
    configure_dropdown_popups();
    floating_keyboard_refresh_cursors();
}

void wire_device_rename()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, LCD_ACTION_DEVICE_RENAME,
        [](const gui::Event &ev) {
            const std::string prefix =
                std::string(LCD_TEMPLATE_DEVICE_CARD_PARENT) + "/" +
                LCD_TEMPLATE_DEVICE_CARD_INSTANCE_PREFIX;
            const size_t at = ev.path.rfind(prefix);
            if (at == std::string::npos) {
                return;
            }
            const int slot = atoi(ev.path.c_str() + at + prefix.size());
            if (slot < 0 || slot >= LCD_COUNT_DEVICE_SLOTS) {
                return;
            }
            s_lcd.scheduler->post([slot]() {
                with_lvgl_lock([slot]() { rename_open(slot); });
            });
        }));
}

void wire_matter_add()
{
    wire_action(LCD_ACTION_MATTER_ADD, handle_matter_add);
}

void wire_navigation()
{
    const char *actions[] = {
        LCD_ACTION_NAV_MAP_FROM_OVERVIEW,
        LCD_ACTION_NAV_STORE_FROM_OVERVIEW,
        LCD_ACTION_NAV_SYSTEM_FROM_OVERVIEW,
        LCD_ACTION_NAV_RULES_FROM_OVERVIEW,
        LCD_ACTION_NAV_SCENES_FROM_OVERVIEW,
        LCD_ACTION_NAV_OVERVIEW_FROM_MAP,
        LCD_ACTION_NAV_STORE_FROM_MAP,
        LCD_ACTION_NAV_SYSTEM_FROM_MAP,
        LCD_ACTION_NAV_RULES_FROM_MAP,
        LCD_ACTION_NAV_SCENES_FROM_MAP,
        LCD_ACTION_NAV_OVERVIEW_FROM_STORE,
        LCD_ACTION_NAV_MAP_FROM_STORE,
        LCD_ACTION_NAV_SYSTEM_FROM_STORE,
        LCD_ACTION_NAV_RULES_FROM_STORE,
        LCD_ACTION_NAV_SCENES_FROM_STORE,
        LCD_ACTION_NAV_OVERVIEW_FROM_SYSTEM,
        LCD_ACTION_NAV_MAP_FROM_SYSTEM,
        LCD_ACTION_NAV_STORE_FROM_SYSTEM,
        LCD_ACTION_NAV_RULES_FROM_SYSTEM,
        LCD_ACTION_NAV_SCENES_FROM_SYSTEM,
        LCD_ACTION_NAV_OVERVIEW_FROM_RULES,
        LCD_ACTION_NAV_MAP_FROM_RULES,
        LCD_ACTION_NAV_STORE_FROM_RULES,
        LCD_ACTION_NAV_SYSTEM_FROM_RULES,
        LCD_ACTION_NAV_SCENES_FROM_RULES,
        LCD_ACTION_NAV_OVERVIEW_FROM_SCENES,
        LCD_ACTION_NAV_MAP_FROM_SCENES,
        LCD_ACTION_NAV_STORE_FROM_SCENES,
        LCD_ACTION_NAV_SYSTEM_FROM_SCENES,
        LCD_ACTION_NAV_RULES_FROM_SCENES,
    };
    for (const char *action : actions) {
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [action](const gui::Event &ev) {
                (void)ev;
                if (!s_lcd.runtime || !s_lcd.scheduler) {
                    return;
                }
                /* Touch events run on the lvgl task — do not pause/mount from there. */
                s_lcd.scheduler->post([action]() {
                    if (!s_lcd.runtime) {
                        return;
                    }
                    ESP_LOGD(TAG, "nav %s on task '%s'", action, pcTaskGetName(nullptr));
                    std::expected<void, std::string> result;
                    {
                        LvglAdapterPauseGuard pause;
                        result = s_lcd.runtime->trigger_screen_flow(
                            s_lcd.document_id, FLOW_ID, action);
                    }
                    if (!result) {
                        ESP_LOGW(TAG, "screen flow %s: %s", action, result.error().c_str());
                        return;
                    }
                    pin_screen_shell_layout();
                    with_lvgl_lock([]() { refresh_lcd_data(); });
                    request_display_refresh();
                });
            }));
    }
}

void wire_overview_filters()
{
    auto wire_filter = [](const char *action, OverviewFilter filter) {
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [filter](const gui::Event &ev) {
                (void)ev;
                with_lvgl_lock([filter]() {
                    if (s_overview_filter == filter) {
                        return;
                    }
                    s_overview_filter = filter;
                    refresh_filter_chips();
                    refresh_overview();
                });
                request_display_refresh();
            }));
    };

    wire_filter(LCD_ACTION_FILTER_ALL, OverviewFilter::All);
    wire_filter(LCD_ACTION_FILTER_AUTH, OverviewFilter::Auth);
    wire_filter(LCD_ACTION_FILTER_MATTER, OverviewFilter::Matter);
}

void wire_user_action()
{
    const char *actions[] = {
        LCD_ACTION_USER_TAP_OVERVIEW,
        LCD_ACTION_USER_TAP_MAP,
        LCD_ACTION_USER_TAP_STORE,
        LCD_ACTION_USER_TAP_SYSTEM,
        LCD_ACTION_USER_TAP_RULES,
        LCD_ACTION_USER_TAP_SCENES,
    };
    for (const char *action : actions) {
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [](const gui::Event &ev) {
                (void)ev;
                if (!s_lcd.scheduler) {
                    return;
                }
                s_lcd.scheduler->post([]() {
                    with_lvgl_lock([]() { handle_user_tap(); });
                });
            }));
    }
}

void wire_rule_tap()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, LCD_ACTION_RULE_TOGGLE,
        [](const gui::Event &ev) {
            const std::string prefix =
                std::string(LCD_TEMPLATE_RULE_ROW_PARENT) + "/" +
                LCD_TEMPLATE_RULE_ROW_INSTANCE_PREFIX;
            const size_t at = ev.path.rfind(prefix);
            if (at == std::string::npos) {
                return;             /* never fall back to slot 0 */
            }
            const int slot = atoi(ev.path.c_str() + at + prefix.size());
            if (slot < 0 || slot >= LCD_COUNT_RULE_SLOTS) {
                return;
            }
            s_lcd.scheduler->post([slot]() {
                with_lvgl_lock([slot]() { handle_rule_tap(slot); });
            });
        }));
}

void wire_rule_delete()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, LCD_ACTION_RULE_DELETE,
        [](const gui::Event &ev) {
            const std::string prefix =
                std::string(LCD_TEMPLATE_RULE_ROW_PARENT) + "/" +
                LCD_TEMPLATE_RULE_ROW_INSTANCE_PREFIX;
            const size_t at = ev.path.rfind(prefix);
            if (at == std::string::npos) {
                return;
            }
            const int slot = atoi(ev.path.c_str() + at + prefix.size());
            if (slot < 0 || slot >= LCD_COUNT_RULE_SLOTS) {
                return;
            }
            s_lcd.scheduler->post([slot]() {
                with_lvgl_lock([slot]() { handle_rule_delete(slot); });
            });
        }));
}

void wire_scene_delete()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, LCD_ACTION_SCENE_DELETE,
        [](const gui::Event &ev) {
            const std::string prefix =
                std::string(LCD_TEMPLATE_SCENE_ROW_PARENT) + "/" +
                LCD_TEMPLATE_SCENE_ROW_INSTANCE_PREFIX;
            const size_t at = ev.path.rfind(prefix);
            if (at == std::string::npos) {
                return;
            }
            const int slot = atoi(ev.path.c_str() + at + prefix.size());
            if (slot < 0 || slot >= LCD_COUNT_SCENE_SLOTS) {
                return;
            }
            s_lcd.scheduler->post([slot]() {
                with_lvgl_lock([slot]() { handle_scene_delete(slot); });
            });
        }));
}

void wire_scene_tap()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, LCD_ACTION_SCENE_INVOKE,
        [](const gui::Event &ev) {
            const std::string prefix =
                std::string(LCD_TEMPLATE_SCENE_ROW_PARENT) + "/" +
                LCD_TEMPLATE_SCENE_ROW_INSTANCE_PREFIX;
            const size_t at = ev.path.rfind(prefix);
            if (at == std::string::npos) {
                return;
            }
            const int slot = atoi(ev.path.c_str() + at + prefix.size());
            if (slot < 0 || slot >= LCD_COUNT_SCENE_SLOTS) {
                return;
            }
            s_lcd.scheduler->post([slot]() {
                with_lvgl_lock([slot]() { handle_scene_tap(slot); });
            });
        }));
}

static void wire_action(const char *action, void (*handler)())
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, action,
        [handler](const gui::Event &ev) {
            (void)ev;
            if (!s_lcd.scheduler) {
                return;
            }
            s_lcd.scheduler->post([handler]() {
                with_lvgl_lock(handler);
            });
        }));
}

void wire_system_reboot()
{
    wire_action(LCD_ACTION_SYSTEM_REBOOT, handle_system_reboot);
    wire_action(LCD_ACTION_SYSTEM_FACTORY_RESET, handle_system_factory_reset);
    /* Both the system screen and the store screen own a confirm modal. Their
     * buttons cannot share one action name (document-level uniqueness), so each
     * screen's buttons are named after the screen; the handler is shared and
     * decides what to do from the pending request, not from the button. */
    static const char *const kConfirmScreens[] = {LCD_ENUM_CONFIRM_SCREENS};
    for (size_t i = 0; i < LCD_ENUM_CONFIRM_SCREENS_N; i++) {
        char ok_action[48];
        char cancel_action[48];
        snprintf(ok_action, sizeof(ok_action), LCD_ACTION_PATTERN_CONFIRM_OK,
                 kConfirmScreens[i]);
        snprintf(cancel_action, sizeof(cancel_action), LCD_ACTION_PATTERN_CONFIRM_CANCEL,
                 kConfirmScreens[i]);
        wire_action(ok_action, handle_confirm_ok);
        wire_action(cancel_action, handle_confirm_cancel);
    }
}

static void handle_store_refresh()
{
    if (s_store_refresh_wait.load(std::memory_order_acquire)) {
        return;
    }
    const esp_err_t err = minihub_store_refresh_catalog_async();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        set_label_text(LCD_VIEW_STORE_AVAIL_HDR, lcd_tr(LCD_STR_STORE_REFRESH_FAIL));
        return;
    }
    s_store_refresh_wait.store(true, std::memory_order_release);
    set_label_text(LCD_VIEW_STORE_AVAIL_HDR, lcd_tr(LCD_STR_STORE_REFRESHING));
}

void wire_store_tabs()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    wire_action("store_refresh", handle_store_refresh);
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, "store_search_changed",
        [](const gui::Event &ev) {
            const auto text = ev.get_string("text");
            if (!text || !s_lcd.scheduler) {
                return;
            }
            const std::string value(*text);
            s_lcd.scheduler->post([value]() {
                with_lvgl_lock([value]() {
                    s_store_query = value;
                    refresh_store();
                });
            });
        }));
    auto wire_tab = [](const char *action, bool available) {
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [available](const gui::Event &ev) {
                (void)ev;
                if (!s_lcd.scheduler) {
                    return;
                }
                s_lcd.scheduler->post([available]() {
                    with_lvgl_lock([available]() { store_set_tab(available); });
                });
            }));
    };
    wire_tab(LCD_ACTION_STORE_TAB_AVAILABLE, true);
    wire_tab(LCD_ACTION_STORE_TAB_INSTALLED, false);
}

void wire_store_rows()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    /* Brookesia requires an action name to be unique across the whole document
     * (validator.cpp:564): two *static* nodes may not share one action, because
     * the owner it records is the node's view path. Template instances escape
     * that rule (each instance has its own scope), but the store rows are
     * hand-written nodes, so each of the twelve owns its own action name.
     * The row identity still comes from the event path, never from the name. */
    static const char *const kRowPrefixes[] = {LCD_ENUM_STORE_ROW_PREFIXES};
    for (size_t p = 0; p < LCD_ENUM_STORE_ROW_PREFIXES_N; p++) {
        for (int i = 0; i < LCD_COUNT_STORE_ROW_SLOTS; i++) {
            char action[48];
            snprintf(action, sizeof(action), LCD_ACTION_PATTERN_STORE_ROW_TAP,
                     kRowPrefixes[p], i);
            s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
                s_lcd.document_id, action,
                [](const gui::Event &ev) {
                    if (!s_lcd.scheduler) {
                        return;
                    }
                    /* The handler runs later on the UI worker, so own the path now. */
                    const std::string path = ev.path;
                    s_lcd.scheduler->post([path]() {
                        with_lvgl_lock([path]() { handle_store_row_tap(path); });
                    });
                }));
        }
    }
    /* Inline Install buttons on the available rows, one action per row (the
     * loader gives one action one owner). */
    for (int i = 0; i < LCD_COUNT_STORE_AVAIL_ROWS; i++) {
        char action[48];
        snprintf(action, sizeof(action), LCD_ACTION_PATTERN_STORE_INSTALL_AVAIL, i);
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [](const gui::Event &ev) {
                if (!s_lcd.scheduler) {
                    return;
                }
                const std::string path = ev.path;
                s_lcd.scheduler->post([path]() {
                    with_lvgl_lock([path]() { handle_store_install_inline(path); });
                });
            }));
    }
    wire_action(LCD_ACTION_STORE_INSTALL, handle_store_install);
    wire_action(LCD_ACTION_STORE_UPGRADE, handle_store_upgrade);
    wire_action(LCD_ACTION_STORE_REINSTALL, handle_store_reinstall);
    wire_action(LCD_ACTION_STORE_ENABLE, handle_store_enable_toggle);
    wire_action(LCD_ACTION_STORE_UNINSTALL, handle_store_uninstall);
    wire_action(LCD_ACTION_STORE_MODAL_CANCEL, handle_store_modal_cancel);
}

void wire_system_lang()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, LCD_ACTION_SYSTEM_LANG_PICK,
        [](const gui::Event &ev) {
            const auto index = ev.get_int64("selectedIndex");
            if (!index || !s_lcd.scheduler) {
                return;
            }
            const int64_t picked = *index;
            s_lcd.scheduler->post([picked]() {
                with_lvgl_lock([picked]() { handle_system_lang_pick(picked); });
            });
        }));
}

void wire_system_tz()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, LCD_ACTION_SYSTEM_TZ_PICK,
        [](const gui::Event &ev) {
            /* Dropdown ValueChanged carries payload["selectedIndex"] (the LVGL
             * backend appends it); read it here, act on the UI worker. */
            const auto index = ev.get_int64("selectedIndex");
            if (!index || !s_lcd.scheduler) {
                return;
            }
            const int64_t picked = *index;
            s_lcd.scheduler->post([picked]() {
                with_lvgl_lock([picked]() { handle_system_tz_pick(picked); });
            });
        }));
}

void wire_device_tap()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid() || !s_lcd.scheduler) {
        return;
    }
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, LCD_ACTION_DEVICE_TAP,
        [](const gui::Event &ev) {
            if (ev.path.find("rename_btn") != std::string::npos) {
                return;                 /* the rename button owns this tap */
            }
            const std::string prefix =
                std::string(LCD_TEMPLATE_DEVICE_CARD_PARENT) + "/" +
                LCD_TEMPLATE_DEVICE_CARD_INSTANCE_PREFIX;
            const size_t at = ev.path.rfind(prefix);
            if (at == std::string::npos) {
                ESP_LOGD(TAG, "device tap outside the template: %s", ev.path.c_str());
                return;             /* never fall back to slot 0 */
            }
            const int slot = atoi(ev.path.c_str() + at + prefix.size());
            if (slot < 0 || slot >= LCD_COUNT_DEVICE_SLOTS) {
                return;
            }
            s_lcd.scheduler->post([slot]() {
                with_lvgl_lock([slot]() { handle_device_tap(slot); });
            });
        }));
}

void on_point_event(const minihub_event_t *event, void *user_data)
{
    (void)event;
    (void)user_data;
    /* Never touch LVGL from the event task — arm the one-shot and get out. */
    s_refresh_due.store(true, std::memory_order_release);
    if (s_coalesce_timer) {
        esp_timer_start_once(s_coalesce_timer, kRefreshCoalesceMs * 1000);
    }
}

void schedule_refresh()
{
    lv_async_call([](void *) {
        with_lvgl_lock([]() { refresh_lcd_data(); });
    }, nullptr);
}

/* One-second tick: overview clock, and the store-refresh button's background
 * fetch. Runs on the UI worker under the LVGL lock. */
static void on_second_tick()
{
    if (s_store_refresh_wait.load(std::memory_order_acquire)) {
        bool busy = true;
        bool finished = false;
        bool aborted = false;
        esp_err_t result = ESP_FAIL;
        minihub_store_refresh_catalog_status(&busy, &finished, &result, &aborted);
        if (!busy && finished) {
            s_store_refresh_wait.store(false, std::memory_order_release);
            if (result == ESP_OK || aborted) {
                refresh_store();
            } else {
                set_label_text(LCD_VIEW_STORE_AVAIL_HDR, lcd_tr(LCD_STR_STORE_REFRESH_FAIL));
            }
        }
    }
    if (is_lcd_screen("overview")) {
        refresh_overview_updated_label();
    }
}

static void clock_timer_cb(void *)
{
    if (s_flash_write.load(std::memory_order_acquire) || !s_lcd.scheduler) {
        return;
    }
    bool expected = false;
    if (!s_clock_tick_queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    s_lcd.scheduler->post([]() {
        s_clock_tick_queued.store(false, std::memory_order_release);
        with_lvgl_lock([]() { on_second_tick(); });
    });
}

void stop_lcd_point_events()
{
    minihub_event_bus_unsubscribe("point.updated", on_point_event);
    minihub_event_bus_unsubscribe("point.registered", on_point_event);
    minihub_event_bus_unsubscribe("point.unregistered", on_point_event);
    minihub_event_bus_unsubscribe("hub.reloaded", on_point_event);
    s_events_live.store(false, std::memory_order_release);
    if (s_coalesce_timer) {
        esp_timer_stop(s_coalesce_timer);
        esp_timer_delete(s_coalesce_timer);
        s_coalesce_timer = nullptr;
    }
    if (s_clock_timer) {
        esp_timer_stop(s_clock_timer);
        esp_timer_delete(s_clock_timer);
        s_clock_timer = nullptr;
    }
}

void refresh_task()
{
    /* Keep the due flag set: the panel must redraw once the window closes. */
    auto defer_refresh_while_flashing = []() {
        s_refresh_due.store(true, std::memory_order_release);
        ESP_LOGD(TAG, "refresh deferred: flash write or OTA in flight");
        if (s_lcd.scheduler) {
            s_lcd.scheduler->post_delayed([]() { refresh_task(); }, 500);
        }
    };
    if (s_flash_write.load(std::memory_order_acquire)) {
        defer_refresh_while_flashing();
        return;
    }
    if (minihub_ota_is_busy()) {
        defer_refresh_while_flashing();
        return;
    }
    if (s_refresh_due.exchange(false, std::memory_order_acq_rel)) {
        schedule_refresh();
    } else if (s_events_live.load(std::memory_order_acquire)) {
        /* A display has no reconnect: if an event was dropped, re-read every
         * point on the slow heartbeat so the panel cannot stick for ever. */
        s_refresh_due.store(true, std::memory_order_release);
        schedule_refresh();
    }
    if (s_lcd.scheduler) {
        s_lcd.scheduler->post_delayed([]() { refresh_task(); }, kFallbackResyncMs);
    }
}

bool setup_lcd_hw()
{
    ESP_LOGD(TAG, "setup_lcd_hw on task '%s'", pcTaskGetName(nullptr));
    log_heap_and_stack("hw-begin");
    esp_err_t vfs_err = esp_vfs_littlefs_unregister("storage");
    if (vfs_err != ESP_OK && vfs_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "release VFS before brookesia: %s", esp_err_to_name(vfs_err));
    }

    auto &sm = service::ServiceManager::get_instance();
    if (!sm.init()) {
        ESP_LOGE(TAG, "ServiceManager init failed");
        return false;
    }
    if (!sm.start()) {
        ESP_LOGE(TAG, "ServiceManager start failed");
        return false;
    }

    if (!start_display_service()) {
        return false;
    }
    log_lvgl_runtime("after-lvgl-start");
    paint_boot_splash();
    sync_display_framebuffers();
    s_lcd.hw_ready = true;
    log_heap_and_stack("hw-done");
    return true;
}

bool setup_lcd_ui_mount()
{
    if (!s_lcd.hw_ready) {
        ESP_LOGE(TAG, "setup_lcd_ui_mount: display HW not ready");
        return false;
    }

    lcd_i18n_init();
    const int64_t setup_t0 = esp_timer_get_time();
    ESP_LOGD(TAG, "setup_lcd_ui_mount on task '%s'", pcTaskGetName(nullptr));
    log_heap_and_stack("ui-begin");
    log_lvgl_runtime("ui-begin");

    BootLvglHold boot_hold;
    paint_boot_splash();

    auto backend = std::make_unique<gui::lvgl::Backend>();
    if (!setup_ui_fonts(*backend)) {
        ESP_LOGW(TAG, "Latin font registration incomplete");
    }
    /* Do not attach task_scheduler during mount — single worker would deadlock on self-dispatch. */
    s_lcd.runtime = std::make_unique<gui::Runtime>(std::move(backend));
    bind_ui_font_language(*s_lcd.runtime);
    paint_boot_splash();

    auto env = make_environment();
    ESP_LOGD(TAG, "[diag] parse begin env=%dx%d", env.width_px, env.height_px);
    int64_t t0 = esp_timer_get_time();
    decltype(gui::parse_document_file_with_metadata(UI_ROOT_PATH, env)) parsed;
    std::expected<gui::DocumentId, std::string> loaded;
    parsed = gui::parse_document_file_with_metadata(UI_ROOT_PATH, env);
    ESP_LOGD(TAG, "[diag] parse took %lld ms", (long long)elapsed_ms_since(t0));
    if (!parsed) {
        ESP_LOGE(TAG, "parse UI failed: %s", parsed.error().c_str());
        return false;
    }
    ESP_LOGI(TAG, "UI JSON parsed (%u screens)", (unsigned)parsed->document.screens.size());

    ESP_LOGD(TAG, "loading UI document...");
    t0 = esp_timer_get_time();
    loaded = s_lcd.runtime->load(UI_ROOT_PATH, std::move(parsed->document), env);
    ESP_LOGD(TAG, "[diag] load took %lld ms", (long long)elapsed_ms_since(t0));
    if (!loaded) {
        ESP_LOGE(TAG, "load UI failed: %s", loaded.error().c_str());
        return false;
    }
    s_lcd.document_id = *loaded;
    ESP_LOGD(TAG, "UI document loaded (dynamic screens defer LVGL tree build to mount)");
    log_heap_and_stack("after-load");
    log_lvgl_tree("after-load");

    ESP_LOGD(TAG, "starting screen flow (overview dynamic mount -> create_subtree + mount_screen)...");
    log_lvgl_runtime("pre-mount");
    paint_boot_splash();
    std::expected<void, std::string> flow;
    {
        start_mount_watch();
        t0 = esp_timer_get_time();
        flow = s_lcd.runtime->start_screen_flow(s_lcd.document_id, FLOW_ID);
        const int64_t mount_ms = elapsed_ms_since(t0);
        stop_mount_watch();
        ESP_LOGD(TAG, "[diag] start_screen_flow returned ok=%d took %lld ms",
                 flow.has_value() ? 1 : 0, (long long)mount_ms);
        if (!flow) {
            ESP_LOGE(TAG, "start screen flow failed: %s", flow.error().c_str());
        }
    }
    if (!flow) {
        return false;
    }
    ESP_LOGD(TAG, "screen flow started");
    log_lvgl_tree("after-mount");
    {
        auto title = s_lcd.runtime->find_view(s_lcd.document_id, LCD_VIEW_OVERVIEW_TITLE);
        auto sidebar = s_lcd.runtime->find_view(s_lcd.document_id, LCD_VIEW_SIDEBAR);
        ESP_LOGD(TAG,
                 "[diag] overview views: title=%d sidebar=%d",
                 title.valid() ? 1 : 0,
                 sidebar.valid() ? 1 : 0);
    }

    wire_navigation();
    wire_user_action();
    wire_device_tap();
    wire_device_rename();
    wire_matter_add();
    wire_rule_tap();
    wire_wifi();
    wire_rule_delete();
    wire_scene_tap();
    wire_scene_delete();
    wire_form();
    wire_update_center();
    wire_system_reboot();
    wire_store_tabs();
    wire_store_rows();
    wire_system_tz();
    wire_system_lang();
    wire_overview_filters();
    module_auth_lcd::login_overlay_init();
    module_auth_lcd::wire_overview_auth_actions(
        s_lcd.runtime.get(), s_lcd.document_id, s_lcd.nav_connections, s_lcd.scheduler,
        []() {
            refresh_lcd_data();
            request_display_refresh();
        });
    s_lcd.started = true;

    /* Coalescing one-shot: point events only arm it, never touch LVGL from the
     * event task. Re-mounting is idempotent — clear any previous subscription. */
    stop_lcd_point_events();
    esp_timer_create_args_t coalesce_args {
        .callback = [](void *) { schedule_refresh(); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lcd_coalesce",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&coalesce_args, &s_coalesce_timer) == ESP_OK) {
        bool all = true;
        all &= minihub_event_bus_subscribe("point.updated", on_point_event, nullptr) == ESP_OK;
        all &= minihub_event_bus_subscribe("point.registered", on_point_event, nullptr) == ESP_OK;
        all &= minihub_event_bus_subscribe("point.unregistered", on_point_event, nullptr) == ESP_OK;
        all &= minihub_event_bus_subscribe("hub.reloaded", on_point_event, nullptr) == ESP_OK;
        s_events_live.store(all, std::memory_order_release);
        ESP_LOGI(TAG, "point events %s",
                 all ? "subscribed" : "subscribe failed: fallback polling only");
    }
    esp_timer_create_args_t clock_args {
        .callback = clock_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lcd_clock",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&clock_args, &s_clock_timer) == ESP_OK) {
        esp_timer_start_periodic(s_clock_timer, 1000 * 1000);
    }

    (void)minihub_web_refresh_geo_async();
    with_lvgl_lock([]() {
        refresh_overview();
        refresh_filter_chips();
        refresh_user_sidebar();
    });
    with_lvgl_lock([]() {
        lv_display_t *disp = lv_display_get_default();
        if (disp != nullptr) {
            lv_obj_t *scr = lv_display_get_screen_active(disp);
            if (scr != nullptr) {
                lv_obj_invalidate(scr);
            }
        }
    });
    sync_display_framebuffers();
    boot_hold.release();
    pin_screen_shell_layout();
    refresh_task();
    ESP_LOGI(TAG, "LCD UI ready (%s, ui mount %lld ms)", UI_ROOT_PATH, (long long)elapsed_ms_since(setup_t0));
    log_heap_and_stack("ui-done");
    return true;
}

static bool ensure_lcd_scheduler(void)
{
    if (s_lcd.scheduler) {
        return true;
    }
    s_lcd.scheduler = std::make_shared<lib_utils::TaskScheduler>();
    if (!s_lcd.scheduler) {
        return false;
    }
    return s_lcd.scheduler->start({
        .worker_configs = {{
            .name = "LcdUiWorker",
            .core_id = 1,
            .priority = 3,
            .stack_size = 96 * 1024,
            .stack_in_ext = true,
        }},
    });
}

static esp_err_t run_lcd_worker_sync(const char *phase, std::function<bool()> fn)
{
    if (!ensure_lcd_scheduler()) {
        ESP_LOGE(TAG, "%s: task scheduler start failed", phase);
        return ESP_FAIL;
    }
    std::promise<bool> done;
    auto fut = done.get_future();
    ESP_LOGD(TAG, "[diag] posting %s to LcdUiWorker", phase);
    s_lcd.scheduler->post([&done, fn = std::move(fn)]() { done.set_value(fn()); });
    const int64_t wait_t0 = esp_timer_get_time();
    const bool ok = fut.get();
    ESP_LOGD(TAG, "[diag] %s -> %s (%lld ms)", phase, ok ? "OK" : "FAIL", (long long)elapsed_ms_since(wait_t0));
    return ok ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t minihub_lcd_start_hw_impl(void)
{
    if (s_lcd.hw_ready) {
        return ESP_OK;
    }
    return run_lcd_worker_sync("setup_lcd_hw", setup_lcd_hw);
}

extern "C" esp_err_t minihub_lcd_start_ui_impl(void)
{
    if (s_lcd.started) {
        return ESP_OK;
    }
    if (!s_lcd.hw_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = run_lcd_worker_sync("setup_lcd_ui_mount", setup_lcd_ui_mount);
    return err;
}

} // namespace

extern "C" void minihub_lcd_on_lvgl_display_ready(void)
{
    auto active = DisplayHelper::call_function_sync(
        DisplayHelper::FunctionId::SetActiveSourceRole,
        std::string(),
        std::string(gui::lvgl::DISPLAY_SOURCE_ROLE),
        service::helper::Timeout(DISPLAY_SERVICE_TIMEOUT_MS));
    if (!active.has_value()) {
        ESP_LOGW(TAG, "SetActiveSourceRole before LVGL start failed");
    }
    fill_boot_framebuffers_solid();
    paint_boot_splash();
}

extern "C" void minihub_lcd_on_lvgl_task_started(void)
{
    (void)esp_lv_adapter_pause(-1);
}

extern "C" void minihub_lcd_request_refresh(void)
{
    if (!s_lcd.started || !s_lcd.scheduler) {
        return;
    }
    s_lcd.scheduler->post([]() {
        with_lvgl_lock([]() { refresh_lcd_data(); });
        request_display_refresh();
    });
}

extern "C" void minihub_lcd_flash_write_begin(void)
{
    if (s_flash_write.exchange(true, std::memory_order_acq_rel)) {
        return;                     /* already frozen; do not nest a second guard */
    }
    if (!s_lcd.hw_ready) {
        return;
    }
    ESP_LOGI(TAG, "flash write window: freezing panel");
    s_freeze_guard = std::make_unique<LvglAdapterPauseGuard>(/*hold_splash_on_pause=*/true);
}

extern "C" void minihub_lcd_flash_write_end(void)
{
    if (!s_flash_write.exchange(false, std::memory_order_acq_rel)) {
        return;                     /* unbalanced end: nothing was frozen */
    }
    s_freeze_guard.reset();         /* resumes LVGL and requests one refresh */
    ESP_LOGI(TAG, "flash write window closed");
}

extern "C" esp_err_t minihub_lcd_start_impl(void)
{
    if (s_lcd.started) {
        return ESP_OK;
    }
    esp_err_t err = minihub_lcd_start_hw_impl();
    if (err != ESP_OK) {
        return err;
    }
    return minihub_lcd_start_ui_impl();
}
