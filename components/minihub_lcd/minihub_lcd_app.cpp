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

#include <sys/stat.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
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
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label);
#if CONFIG_LV_FONT_MONTSERRAT_14
LV_FONT_DECLARE(lv_font_montserrat_14);
#endif
#if CONFIG_LV_FONT_MONTSERRAT_16
LV_FONT_DECLARE(lv_font_montserrat_16);
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
constexpr int kOverviewDeviceSlots = 12;
constexpr int kMapCanvasW = 664;
constexpr int kMapCanvasH = 316;
constexpr const char *MAP_IMAGE_VIEW = "/map/map_main/map_canvas_wrap/map_image";
constexpr const char *MAP_STATUS_VIEW = "/map/map_main/map_canvas_wrap/map_status";

bool map_snapshot_file_ready();

void refresh_map();

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
        ESP_LOGI(TAG, "[diag] %s task '%s' not found", phase, task_name);
        return;
    }
    ESP_LOGI(TAG, "[diag] %s task '%s' state=%s", phase, task_name, task_state_name(eTaskGetState(handle)));
}

void log_heap_and_stack(const char *phase)
{
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
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
    ESP_LOGI(TAG,
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
            ESP_LOGI(TAG, "[diag] %s no default lv_display", phase);
            return;
        }
        lv_obj_t *scr = lv_display_get_screen_active(disp);
        if (scr == nullptr) {
            ESP_LOGI(TAG, "[diag] %s no active screen", phase);
            return;
        }
        uint32_t child_cnt = lv_obj_get_child_cnt(scr);
        ESP_LOGI(TAG,
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
    ESP_LOGI(TAG, "[diag] mount-watch elapsed=%lld ms (still inside start_screen_flow/create_subtree?)", (long long)elapsed_ms);
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
    ESP_LOGI(TAG, "[diag] mount-watch started (500ms interval)");
}

void stop_mount_watch()
{
    s_mount_in_progress.store(false, std::memory_order_relaxed);
    if (s_mount_watch_timer != nullptr) {
        (void)esp_timer_stop(s_mount_watch_timer);
    }
    ESP_LOGI(TAG, "[diag] mount-watch stopped");
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

constexpr lv_coord_t kShellSidebarW = 112;
constexpr lv_coord_t kShellMainW = 688;
constexpr lv_coord_t kShellScreenH = 480;

lv_obj_t *find_screen_shell_root(lv_obj_t *scr)
{
    if (scr == nullptr) {
        return nullptr;
    }
    const uint32_t child_cnt = lv_obj_get_child_cnt(scr);
    if (child_cnt == 0) {
        return scr;
    }
    if (child_cnt == 1) {
        return lv_obj_get_child(scr, 0);
    }

    /* Brookesia keeps a hidden staging_root on active_screen; mounted viewScreens are siblings. */
    lv_obj_t *best = nullptr;
    lv_coord_t best_w = 0;
    lv_coord_t best_h = 0;
    for (uint32_t i = 0; i < child_cnt; ++i) {
        lv_obj_t *child = lv_obj_get_child(scr, i);
        if (child == nullptr || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        const lv_coord_t w = lv_obj_get_width(child);
        const lv_coord_t h = lv_obj_get_height(child);
        if (w <= 0 && h <= 0) {
            continue;
        }
        if (w > best_w || (w == best_w && h > best_h)) {
            best = child;
            best_w = w;
            best_h = h;
        }
    }
    return best != nullptr ? best : scr;
}

void pin_screen_shell_layout()
{
    with_lvgl_lock([]() {
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

        lv_obj_t *shell = find_screen_shell_root(scr);
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
        lv_obj_move_foreground(sidebar);
        lv_obj_update_layout(shell);
        ESP_LOGI(TAG,
                 "[diag] shell layout scr_children=%u shell=%p shell=(%d,%d,%dx%d) "
                 "sidebar=(%d,%d,%dx%d) main=(%d,%d,%dx%d)",
                 (unsigned)lv_obj_get_child_cnt(scr),
                 static_cast<void *>(shell),
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
    });
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
        ESP_LOGI(TAG, "[diag] esp_lv_adapter_pause -> %s (%lld ms)", esp_err_to_name(err), (long long)elapsed_ms_since(t0));
        if (err == ESP_OK) {
            active = true;
            log_lvgl_runtime("post-pause");
            if (hold_splash) {
                paint_boot_splash();
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
        ESP_LOGI(TAG, "[diag] esp_lv_adapter_resume -> %s (%lld ms)", esp_err_to_name(err), (long long)elapsed_ms_since(t0));
        if (err == ESP_OK) {
            request_display_refresh();
        }
        log_lvgl_runtime("post-resume");
    }
    LvglAdapterPauseGuard(const LvglAdapterPauseGuard &) = delete;
    LvglAdapterPauseGuard &operator=(const LvglAdapterPauseGuard &) = delete;
};

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
    env.language = "zh";
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
    ESP_LOGI(TAG, "registered font '%s' (%dpx)", font_id, (int)size_px);
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
    ESP_LOGI(TAG, "registered font sc_14 (%ld bytes)", (long)st.st_size);
    return true;
}

void bind_ui_font_language(gui::Runtime &runtime)
{
    const char *font_id = "latin_14";
    if (register_ui_sc_font(runtime)) {
        font_id = "sc_14";
    }
    auto en = runtime.set_default_font_for_language("en", font_id);
    if (!en) {
        ESP_LOGW(TAG, "en default font: %s", en.error().c_str());
    }
    auto zh = runtime.set_default_font_for_language("zh", font_id);
    if (!zh) {
        ESP_LOGW(TAG, "zh default font: %s", zh.error().c_str());
    }
    ESP_LOGI(TAG, "UI fonts: en/zh=%s", font_id);
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
    lv_cfg.use_psram = true;
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
             "/overview/overview_main/overview_toolbar/filter_row/%s", chip_id);
    snprintf(lbl_path, sizeof(lbl_path), "%s/%s_lbl", btn_path, chip_id);

    const char *bg = active ? "#6366f1" : "#1a2238";
    const char *border = active ? "#6366f1" : "#243049";
    const char *text = active ? "#eef2ff" : "#8b9bb4";

    updates.push_back({btn_path, "chip.bg", bg});
    updates.push_back({btn_path, "chip.border", border});
    updates.push_back({lbl_path, "chip.text", text});
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

    set_view_hidden("/overview/overview_main/overview_toolbar/filter_row/filter_auth", !show_auth);
    set_view_hidden("/overview/overview_main/overview_toolbar/filter_row/filter_matter", !show_matter);

    if (show_auth) {
        char filter_lbl[32] = "云账号";
        (void)minihub_module_ui_label(auth_adapter, "filter_label", filter_lbl, sizeof(filter_lbl));
        set_label_text("/overview/overview_main/overview_toolbar/filter_row/filter_auth/filter_auth_lbl",
                       filter_lbl);
        set_label_text("/overview/overview_main/overview_toolbar/auth_bar/auth_section_lbl", filter_lbl);
    }

    std::vector<gui::BindingValueUpdate> updates;
    append_filter_chip_bindings("filter_all", s_overview_filter == OverviewFilter::All, updates);
    if (show_auth) {
        append_filter_chip_bindings("filter_auth", s_overview_filter == OverviewFilter::Auth, updates);
    }
    if (show_matter) {
        append_filter_chip_bindings("filter_matter", s_overview_filter == OverviewFilter::Matter, updates);
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
    if (!overview_view_valid("/overview/overview_main/overview_toolbar")) {
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

void hide_device_slots_from(size_t shown)
{
    for (int i = static_cast<int>(shown); i < kOverviewDeviceSlots; i++) {
        char path[80];
        snprintf(path, sizeof(path), "/overview/overview_main/device_list/dev_%d", i);
        auto view = s_lcd.runtime->find_view(s_lcd.document_id, path);
        if (view.valid()) {
            view.set_hidden(true);
        }
    }
}

void refresh_overview_updated_label()
{
    char updated[32] = "更新 --:--:--";
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        time_t now = time(nullptr);
        struct tm tm_now {};
        if (now > 0 && localtime_r(&now, &tm_now) != nullptr) {
            snprintf(updated, sizeof(updated), "更新 %02d:%02d:%02d",
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        }
    } else {
        snprintf(updated, sizeof(updated), "更新 待授时");
    }
    set_label_text("/overview/overview_main/header_row/overview_updated", updated);
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
    set_label_text("/overview/overview_main/stats_row/stat_devices/stat_devices_val", num);
    snprintf(num, sizeof(num), "%u", (unsigned)online);
    set_label_text("/overview/overview_main/stats_row/stat_online/stat_online_val", num);
    snprintf(num, sizeof(num), "%u", (unsigned)adapter_unique);
    set_label_text("/overview/overview_main/stats_row/stat_adapters/stat_adapters_val", num);

    static int s_auth_poll_tick;
    static bool s_auth_logged_in;
    static bool s_auth_was_logged_in;
    static bool s_auth_auto_sync_done;
    static char s_auth_acct[96] = "未登录";
    if (auth_visible && (s_auth_poll_tick++ % 5) == 0) {
        s_auth_logged_in = false;
        char logged_out[32] = "未登录";
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

    static const char *kAuthBar = "/overview/overview_main/overview_toolbar/auth_bar";
    const bool filter_auth = s_overview_filter == OverviewFilter::Auth;
    const bool filter_all = s_overview_filter == OverviewFilter::All;
    const bool show_auth_bar = auth_visible && (filter_auth || (filter_all && logged_in));
    const bool show_auth_actions = filter_auth;

    set_view_hidden(kAuthBar, !show_auth_bar);
    if (show_auth_bar) {
        snprintf(num, sizeof(num), "%u", (unsigned)auth_all);
        set_label_text("/overview/overview_main/overview_toolbar/auth_bar/auth_count", num);
        set_label_text("/overview/overview_main/overview_toolbar/auth_bar/auth_acct", acct);
        set_view_hidden("/overview/overview_main/overview_toolbar/auth_bar/auth_btn_login",
                        !show_auth_actions || logged_in);
        set_view_hidden("/overview/overview_main/overview_toolbar/auth_bar/auth_btn_sync",
                        !show_auth_actions || !logged_in);
        set_view_hidden("/overview/overview_main/overview_toolbar/auth_bar/auth_btn_logout",
                        !show_auth_actions || !logged_in);
    }
    set_view_hidden("/overview/overview_main/auth_cta",
                    !(auth_visible && filter_auth && logged_in && auth_all == 0));

    const size_t shown = s_overview_filtered.size() < static_cast<size_t>(kOverviewDeviceSlots)
                             ? s_overview_filtered.size()
                             : static_cast<size_t>(kOverviewDeviceSlots);
    for (size_t i = 0; i < shown; i++) {
        const minihub_point_t &pt = s_overview_filtered[i];
        char base[64];
        snprintf(base, sizeof(base), "/overview/overview_main/device_list/dev_%u", (unsigned)i);
        char path[96];
        snprintf(path, sizeof(path), "%s/dev_%u_icon", base, (unsigned)i);
        set_image_src(path, device_type_icon_id(pt.type));
        snprintf(path, sizeof(path), "%s/dev_%u_name", base, (unsigned)i);
        set_label_text(path, point_short_name(pt.point_id));
        snprintf(path, sizeof(path), "%s/dev_%u_type", base, (unsigned)i);
        set_label_text(path, pt.type);
        snprintf(path, sizeof(path), "%s/dev_%u_meta", base, (unsigned)i);
        set_label_text_fmt(path, "%s · %s", pt.online ? "在线" : "离线", pt.adapter);
        snprintf(path, sizeof(path), "%s/dev_%u_val", base, (unsigned)i);
        set_label_text(path, pt.value);
        auto view = s_lcd.runtime->find_view(s_lcd.document_id, base);
        if (view.valid()) {
            view.set_hidden(false);
        }
    }
    hide_device_slots_from(shown);
    free(points);
}

void refresh_store_rows(const char *prefix, cJSON *arr)
{
    const int max_rows = 4;
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    char path[96];
    char text[128];
    const bool is_pack = strcmp(prefix, "pack") == 0;

    for (int i = 0; i < max_rows; i++) {
        snprintf(path, sizeof(path), "/store/store_main/%s_section/%s_row_%d", prefix, prefix, i);
        auto row = s_lcd.runtime->find_view(s_lcd.document_id, path);
        if (!row.valid()) {
            continue;
        }
        if (i >= n) {
            row.set_hidden(true);
            continue;
        }
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) {
            row.set_hidden(true);
            continue;
        }

        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        const cJSON *enabled = cJSON_GetObjectItem(item, "enabled");

        snprintf(path, sizeof(path), "/store/store_main/%s_section/%s_row_%d/%s_name_%d",
                 prefix, prefix, i, prefix, i);
        if (is_pack) {
            const cJSON *version = cJSON_GetObjectItem(item, "version");
            if (cJSON_IsString(name) && name->valuestring[0] != '\0') {
                if (cJSON_IsString(version) && version->valuestring[0] != '\0') {
                    snprintf(text, sizeof(text), "%s  v%s", name->valuestring, version->valuestring);
                } else {
                    snprintf(text, sizeof(text), "%s", name->valuestring);
                }
            } else {
                snprintf(text, sizeof(text), "%s",
                         cJSON_IsString(id) ? id->valuestring : "");
            }
        } else if (cJSON_IsString(name) && name->valuestring[0] != '\0') {
            snprintf(text, sizeof(text), "%s", name->valuestring);
        } else {
            snprintf(text, sizeof(text), "%s",
                     cJSON_IsString(id) ? id->valuestring : "");
        }
        set_label_text(path, text);

        snprintf(path, sizeof(path), "/store/store_main/%s_section/%s_row_%d/%s_desc_%d",
                 prefix, prefix, i, prefix, i);
        if (is_pack) {
            const cJSON *vendor = cJSON_GetObjectItem(item, "vendor");
            const cJSON *adapter = cJSON_GetObjectItem(item, "inject_adapter");
            snprintf(text, sizeof(text), "%s -> %s",
                     cJSON_IsString(vendor) ? vendor->valuestring : "-",
                     cJSON_IsString(adapter) ? adapter->valuestring : "-");
        } else {
            const cJSON *adapter = cJSON_GetObjectItem(item, "adapter");
            snprintf(text, sizeof(text), "adapter: %s",
                     cJSON_IsString(adapter) ? adapter->valuestring : "-");
        }
        set_label_text(path, text);

        snprintf(path, sizeof(path), "/store/store_main/%s_section/%s_row_%d/%s_state_%d",
                 prefix, prefix, i, prefix, i);
        set_label_text(path, (!cJSON_IsBool(enabled) || cJSON_IsTrue(enabled)) ? "On" : "Off");
        row.set_hidden(false);
    }
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

    cJSON *packs = minihub_pack_list_json();
    if (packs) {
        refresh_store_rows("pack", packs);
        cJSON_Delete(packs);
    }
    cJSON *mods = minihub_module_list_json();
    if (mods) {
        refresh_store_rows("mod", mods);
        cJSON_Delete(mods);
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
        set_label_text("/map/map_main/map_info", "OpenStreetMap · Hub location · © OSM");
        set_map_status_overlay(false, nullptr);
    } else {
        set_label_text("/map/map_main/map_info", "");
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
    set_label_text("/map/map_main/map_info", "");
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
        set_label_text("/map/map_main/map_hub", "Waiting for network...");
        set_label_text("/map/map_main/map_info", "");
        set_map_status_overlay(false, nullptr);
        return;
    }
    minihub_geo_t geo = {};
    esp_err_t geo_err = minihub_web_peek_geo(&geo);
    if (geo_err != ESP_OK || !geo.valid) {
        (void)minihub_web_refresh_geo_async();
        set_label_text("/map/map_main/map_hub", "Resolving location...");
        set_label_text("/map/map_main/map_info", "");
        set_map_status_overlay(false, nullptr);
        return;
    }

    char hub[192];
    format_geo_hub_label(geo, hub, sizeof(hub));
    set_label_text("/map/map_main/map_hub", hub);

    const bool map_ready = map_snapshot_file_ready();
    const bool fetching = s_map_fetching.load(std::memory_order_relaxed);
    update_map_ready_ui(map_ready, fetching);
    schedule_map_snapshot_fetch(geo.lat, geo.lon);
}

void refresh_user_sidebar()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    if (!state) {
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/%s/sidebar/user_name", state->c_str());

    cJSON *result = nullptr;
    char auth_adapter[32] = {};
    if (!minihub_module_pick_auth_adapter(auth_adapter, sizeof(auth_adapter)) ||
        minihub_module_invoke_rpc(auth_adapter, "status", nullptr, &result) != ESP_OK || !result) {
        return;
    }
    const cJSON *logged = cJSON_GetObjectItem(result, "logged_in");
    const cJSON *uid = cJSON_GetObjectItem(result, "user_id");
    if (cJSON_IsTrue(logged) && cJSON_IsString(uid) && uid->valuestring[0] != '\0') {
        set_label_text(path, uid->valuestring);
    } else {
        set_label_text(path, "用户");
    }
    cJSON_Delete(result);
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
        ESP_LOGI(TAG, "device tap slot %d ignored (type not controllable)", slot);
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
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    const auto state = s_lcd.runtime->get_screen_flow_state(s_lcd.document_id, FLOW_ID);
    if (!state) {
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/%s/sidebar/user_name", state->c_str());

    cJSON *result = nullptr;
    char auth_adapter[32] = {};
    if (!minihub_module_pick_auth_adapter(auth_adapter, sizeof(auth_adapter)) ||
        minihub_module_invoke_rpc(auth_adapter, "status", nullptr, &result) != ESP_OK || !result) {
        return;
    }
    const cJSON *logged = cJSON_GetObjectItem(result, "logged_in");
    const cJSON *uid = cJSON_GetObjectItem(result, "user_id");
    const cJSON *region = cJSON_GetObjectItem(result, "region");
    if (cJSON_IsTrue(logged) && cJSON_IsString(uid)) {
        ESP_LOGI(TAG,
                 "Cloud signed in (%s): user_id=%s region=%s",
                 auth_adapter,
                 uid->valuestring,
                 cJSON_IsString(region) ? region->valuestring : "-");
        set_label_text(path, uid->valuestring);
    } else {
        ESP_LOGI(TAG, "Account: open cloud login on overview (%s)", auth_adapter);
        set_label_text(path, "用户");
        module_auth_lcd::login_overlay_show_browser_hint();
    }
    cJSON_Delete(result);
    request_display_refresh();
}

void refresh_lcd_data()
{
    if (!s_lcd.runtime || !s_lcd.started) {
        return;
    }
    if (is_lcd_screen("overview")) {
        refresh_overview();
    }
    refresh_user_sidebar();

    if (is_lcd_screen("map")) {
        refresh_map();
    } else if (is_lcd_screen("store")) {
        refresh_store();
    }
}

void wire_navigation()
{
    const char *actions[] = {"nav_overview", "nav_map", "nav_store"};
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
                    ESP_LOGI(TAG, "nav %s on task '%s'", action, pcTaskGetName(nullptr));
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

    wire_filter("filter_all", OverviewFilter::All);
    wire_filter("filter_auth", OverviewFilter::Auth);
    wire_filter("filter_matter", OverviewFilter::Matter);
}

void wire_user_action()
{
    s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
        s_lcd.document_id, "user_tap",
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

void wire_device_tap()
{
    if (!s_lcd.runtime || !s_lcd.document_id.is_valid()) {
        return;
    }
    for (int i = 0; i < kOverviewDeviceSlots; i++) {
        char action[24];
        snprintf(action, sizeof(action), "device_tap_%d", i);
        const int slot = i;
        s_lcd.nav_connections.push_back(s_lcd.runtime->subscribe_event_action(
            s_lcd.document_id, action,
            [slot](const gui::Event &ev) {
                (void)ev;
                if (!s_lcd.scheduler) {
                    return;
                }
                s_lcd.scheduler->post([slot]() {
                    with_lvgl_lock([slot]() { handle_device_tap(slot); });
                });
            }));
    }
}

void refresh_task()
{
    lv_async_call(
        [](void *) {
            with_lvgl_lock([]() { refresh_lcd_data(); });
        },
        nullptr);
    if (s_lcd.scheduler) {
        s_lcd.scheduler->post_delayed([]() { refresh_task(); }, CONFIG_MINIHUB_LCD_UI_REFRESH_MS);
    }
}

bool setup_lcd_hw()
{
    ESP_LOGI(TAG, "setup_lcd_hw on task '%s'", pcTaskGetName(nullptr));
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

    const int64_t setup_t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "setup_lcd_ui_mount on task '%s'", pcTaskGetName(nullptr));
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
    ESP_LOGI(TAG, "[diag] parse begin env=%dx%d", env.width_px, env.height_px);
    int64_t t0 = esp_timer_get_time();
    decltype(gui::parse_document_file_with_metadata(UI_ROOT_PATH, env)) parsed;
    std::expected<gui::DocumentId, std::string> loaded;
    parsed = gui::parse_document_file_with_metadata(UI_ROOT_PATH, env);
    ESP_LOGI(TAG, "[diag] parse took %lld ms", (long long)elapsed_ms_since(t0));
    if (!parsed) {
        ESP_LOGE(TAG, "parse UI failed: %s", parsed.error().c_str());
        return false;
    }
    ESP_LOGI(TAG, "UI JSON parsed (%u screens)", (unsigned)parsed->document.screens.size());

    ESP_LOGI(TAG, "loading UI document...");
    t0 = esp_timer_get_time();
    loaded = s_lcd.runtime->load(UI_ROOT_PATH, std::move(parsed->document), env);
    ESP_LOGI(TAG, "[diag] load took %lld ms", (long long)elapsed_ms_since(t0));
    if (!loaded) {
        ESP_LOGE(TAG, "load UI failed: %s", loaded.error().c_str());
        return false;
    }
    s_lcd.document_id = *loaded;
    ESP_LOGI(TAG, "UI document loaded (dynamic screens defer LVGL tree build to mount)");
    log_heap_and_stack("after-load");
    log_lvgl_tree("after-load");

    ESP_LOGI(TAG, "starting screen flow (overview dynamic mount -> create_subtree + mount_screen)...");
    log_lvgl_runtime("pre-mount");
    paint_boot_splash();
    std::expected<void, std::string> flow;
    {
        start_mount_watch();
        t0 = esp_timer_get_time();
        flow = s_lcd.runtime->start_screen_flow(s_lcd.document_id, FLOW_ID);
        const int64_t mount_ms = elapsed_ms_since(t0);
        stop_mount_watch();
        ESP_LOGI(TAG, "[diag] start_screen_flow returned ok=%d took %lld ms",
                 flow.has_value() ? 1 : 0, (long long)mount_ms);
        if (!flow) {
            ESP_LOGE(TAG, "start screen flow failed: %s", flow.error().c_str());
        }
    }
    if (!flow) {
        return false;
    }
    ESP_LOGI(TAG, "screen flow started");
    log_lvgl_tree("after-mount");
    {
        auto title = s_lcd.runtime->find_view(s_lcd.document_id, "/overview/overview_main/overview_title");
        auto sidebar = s_lcd.runtime->find_view(s_lcd.document_id, "/overview/sidebar");
        ESP_LOGI(TAG,
                 "[diag] overview views: title=%d sidebar=%d",
                 title.valid() ? 1 : 0,
                 sidebar.valid() ? 1 : 0);
    }

    wire_navigation();
    wire_user_action();
    wire_device_tap();
    wire_overview_filters();
    module_auth_lcd::login_overlay_init();
    module_auth_lcd::wire_overview_auth_actions(
        s_lcd.runtime.get(), s_lcd.document_id, s_lcd.nav_connections, s_lcd.scheduler,
        []() {
            refresh_lcd_data();
            request_display_refresh();
        });
    s_lcd.started = true;
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
    ESP_LOGI(TAG, "[diag] posting %s to LcdUiWorker", phase);
    s_lcd.scheduler->post([&done, fn = std::move(fn)]() { done.set_value(fn()); });
    const int64_t wait_t0 = esp_timer_get_time();
    const bool ok = fut.get();
    ESP_LOGI(TAG, "[diag] %s -> %s (%lld ms)", phase, ok ? "OK" : "FAIL", (long long)elapsed_ms_since(wait_t0));
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
