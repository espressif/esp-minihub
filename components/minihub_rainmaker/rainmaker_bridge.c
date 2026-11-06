/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rainmaker_bridge.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "cJSON.h"

#include "esp_rmaker_core.h"
#include "esp_rmaker_standard_types.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_devices.h"

/* The device-widget catalog is the source of truth for how a parameter should
 * render (widget + bounds), shared with the Web console and rules editor. The
 * generic pass-through consults it so a speaker's volume becomes a slider and
 * its play/mute buttons become push buttons instead of free-text fields. */
#include "minihub_device_widget.h"

#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"

static const char *TAG = "rmaker_bridge";

#define BRIDGE_MAX_DEVICES MINIHUB_MAX_POINTS

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char device_name[MINIHUB_POINT_ID_MAX];
    char *priv_point_id; /* owned; passed to RainMaker device priv_data */
    esp_rmaker_device_t *device;
    int kind;            /* rmaker_kind_t, set at add time */
    bool has_brightness;
    bool has_cct;
    bool has_humidity;
    bool has_recording;
} bridge_entry_t;

/* ---- MiniHub device_widget catalog mapping (see docs/en/type_catalog.rst) ----
 * The bridge previously hardcoded type strings ("sensor") and param keys
 * ("temperature" for color temperature) that no longer match the new model
 * (components/minihub_device_widget/model/<name>.csv). We now build RainMaker
 * devices/params from a table synced with parameters.csv / devices.csv so the
 * bridge tracks the catalog. Keep RM_*_PARAMS in sync with the CSVs.
 *
 * `speaker` is a dedicated kind (K_SPEAKER). Its param *names* follow the official
 * ESP RainMaker **Entertainment** standard (media-activity-control / media-playback-state
 * / media-activity-state / volume / mute), but the RM *types* are deliberately custom
 * (esp.param.rm_spk_*) instead of the official esp.param.media-* ones: the phone app
 * special-cases esp.param.media-* and renders an empty control because this app build
 * lacks that enum. Custom types fall back to the generic dropdown driven by our
 * valid_strs. Those RM specifics do NOT live in the shared
 * device catalog (parameters.csv / devices.csv), which stays generic (the hub
 * point's real vocabulary: play_prev/play_pause/play_resume/play_next/mute/
 * unmute/volume/playing/muted, also used by the web console + rules editor).
 * The translation lives entirely in this conversion layer: create_speaker()
 * registers the official RM params, speaker_write() maps RM writes -> hub
 * commands, report_speaker() maps hub state -> RM params. Instance-only status
 * params like `playing`/`muted` stay in the hub point (the web reads them as
 * media-toggle state) and are never surfaced as raw RM controls.
 *
 * All other catalog types with no dedicated mapping fall through to K_GENERIC:
 * create_generic() builds an `esp.device.<type>` device and adds one RainMaker
 * param per entry in props.params. Each param's UI type + bounds come from the
 * device-widget catalog (minihub_device_widget_find): a slider gets bounds, a
 * `ui.button` becomes a push button, a toggle stays a switch, etc. Only when the
 * catalog has no entry does it fall back to the instance value type
 * (bool->TOGGLE, number->SLIDER, string->TEXT). Cloud writes are forwarded back
 * via the point's generic "set"
 * command. The vendored esp-rainmaker only ships fan/lightbulb/switch/temp_sensor
 * helpers and its standard params lack volume/media/mute, but the raw
 * esp_rmaker_device_create / esp_rmaker_param_create APIs accept arbitrary type
 * strings, so we bypass the helpers for everything else.
 */
typedef enum { VT_INT, VT_FLOAT, VT_BOOL, VT_STR } rm_vt_t;
typedef enum { K_SWITCH, K_LIGHT, K_SENSOR, K_CAMERA, K_SPEAKER, K_GENERIC } rmaker_kind_t;

typedef struct {
    const char *key;     /* props.params.<name> of the device point (parameters.csv) */
    const char *rm_name; /* RainMaker param display name */
    const char *rm_type; /* RainMaker param type string */
    rm_vt_t vt;
    bool rw;             /* Read/Write vs Read-only (from parameters.csv) */
    const char *ui;      /* ESP_RMAKER_UI_* or NULL */
    bool always;         /* add even if not present in props (power/brightness) */
} rm_param_t;

static const rm_param_t RM_LIGHT_PARAMS[] = {
    {"brightness", ESP_RMAKER_DEF_BRIGHTNESS_NAME, "esp.param.brightness", VT_INT,  true,  ESP_RMAKER_UI_SLIDER, true},
    {"cct",        ESP_RMAKER_DEF_CCT_NAME,        "esp.param.cct",        VT_INT,  true,  ESP_RMAKER_UI_SLIDER, false},
    {"hue",        ESP_RMAKER_DEF_HUE_NAME,        "esp.param.hue",        VT_INT,  true,  ESP_RMAKER_UI_SLIDER, false},
    {"saturation", ESP_RMAKER_DEF_SATURATION_NAME, "esp.param.saturation", VT_INT,  true,  ESP_RMAKER_UI_SLIDER, false},
    {"intensity",  ESP_RMAKER_DEF_INTENSITY_NAME,  "esp.param.intensity",  VT_INT,  true,  ESP_RMAKER_UI_SLIDER, false},
};

static const rm_param_t RM_SWITCH_PARAMS[] = {
    {"voltage_v",   "Voltage",      "esp.param.voltage_v",   VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"current_a",   "Current",      "esp.param.current_a",   VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"power_w",     "Active Power", "esp.param.power_w",     VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"energy_wh",   "Energy",       "esp.param.energy_wh",   VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"temperature", ESP_RMAKER_DEF_TEMPERATURE_NAME, "esp.param.temperature", VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"fault",       "Fault",        "esp.param.fault",       VT_INT,   false, ESP_RMAKER_UI_TEXT, false},
    {"indicator",   "Indicator",    "esp.param.indicator",   VT_BOOL,  true,  ESP_RMAKER_UI_TOGGLE, false},
};

/* Sensor params except temperature (the temp-sensor device base already adds it). */
static const rm_param_t RM_SENSOR_PARAMS[] = {
    {"humidity",    "Ambient Humidity", "esp.param.humidity",    VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"co2",         "CO2",              "esp.param.co2",         VT_INT,   false, ESP_RMAKER_UI_TEXT, false},
    {"pm25",        "PM2.5",            "esp.param.pm25",        VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"pressure_hpa","Pressure",         "esp.param.pressure_hpa",VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"illuminance", "Illuminance",      "esp.param.illuminance", VT_FLOAT, false, ESP_RMAKER_UI_TEXT, false},
    {"contact",     "Contact",          "esp.param.contact",     VT_BOOL,  false, ESP_RMAKER_UI_TEXT, false},
    {"motion",      "Motion",           "esp.param.motion",      VT_BOOL,  false, ESP_RMAKER_UI_TEXT, false},
    {"occupancy",   "Occupancy",        "esp.param.occupancy",   VT_BOOL,  false, ESP_RMAKER_UI_TEXT, false},
    {"smoke",       "Smoke",            "esp.param.smoke",       VT_BOOL,  false, ESP_RMAKER_UI_TEXT, false},
    {"tamper",      "Tamper",           "esp.param.tamper",      VT_BOOL,  false, ESP_RMAKER_UI_TEXT, false},
    {"leak",        "Water Leak",       "esp.param.leak",        VT_BOOL,  false, ESP_RMAKER_UI_TEXT, false},
    {"battery",     "Battery",          "esp.param.battery",     VT_INT,   false, ESP_RMAKER_UI_TEXT, false},
};

/* Map a RainMaker param display name back to the model parameter row, so a
 * cloud write to any listed parameter can be routed as the model's
 * "set" + {parameter, value} command instead of being dropped. */
static const rm_param_t *rm_param_by_rm_name(const char *rm_name)
{
    static const rm_param_t *const lists[] = { RM_LIGHT_PARAMS, RM_SWITCH_PARAMS, RM_SENSOR_PARAMS };
    static const size_t counts[] = {
        sizeof(RM_LIGHT_PARAMS) / sizeof(RM_LIGHT_PARAMS[0]),
        sizeof(RM_SWITCH_PARAMS) / sizeof(RM_SWITCH_PARAMS[0]),
        sizeof(RM_SENSOR_PARAMS) / sizeof(RM_SENSOR_PARAMS[0]),
    };
    for (size_t l = 0; l < sizeof(lists) / sizeof(lists[0]); l++) {
        for (size_t i = 0; i < counts[l]; i++) {
            if (strcmp(lists[l][i].rm_name, rm_name) == 0) {
                return &lists[l][i];
            }
        }
    }
    return NULL;
}

static bool value_is_on(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }
    if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        return true;
    }
    return false;
}

/* Live parameters live at props.params.<name>, each entry an object holding
 * "value" plus optional per-instance access / min / max / step. Producers write
 * value as Bool, Number or String, so read it through one tolerant accessor. */
static const cJSON *rm_param_entry(const cJSON *props, const char *key)
{
    const cJSON *params = props ? cJSON_GetObjectItem(props, "params") : NULL;
    const cJSON *ent = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    return cJSON_IsObject(ent) ? ent : NULL;
}

static const cJSON *rm_param_value(const cJSON *entry)
{
    return entry ? cJSON_GetObjectItemCaseSensitive(entry, "value") : NULL;
}

static bool rm_entry_bool(const cJSON *entry, bool def)
{
    const cJSON *v = rm_param_value(entry);
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v);
    }
    if (cJSON_IsNumber(v)) {
        return v->valuedouble != 0.0;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        return value_is_on(v->valuestring);
    }
    return def;
}

static double rm_entry_num(const cJSON *entry, double def)
{
    const cJSON *v = rm_param_value(entry);
    if (cJSON_IsNumber(v)) {
        return v->valuedouble;
    }
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v) ? 1.0 : 0.0;
    }
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        return atof(v->valuestring);
    }
    return def;
}

/* Instance access wins; a missing key means writable (same rule as the console). */
static bool rm_entry_readonly(const cJSON *entry)
{
    const cJSON *a = entry ? cJSON_GetObjectItem(entry, "access") : NULL;
    return cJSON_IsString(a) && strcmp(a->valuestring, "R") == 0;
}

static bool rm_entry_num_opt(const cJSON *entry, const char *key, double *out)
{
    const cJSON *v = entry ? cJSON_GetObjectItem(entry, key) : NULL;
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}

/* Instance-level bounds (min/max/step) when the catalog does not supply them.
 * The generic pass-through also consults minihub_device_widget for the widget /
 * bounds, so this is only the per-instance fallback. */
static bool rm_entry_bounds(const cJSON *entry, double *min, double *max, double *step)
{
    if (!entry) {
        return false;
    }
    const bool has_min = rm_entry_num_opt(entry, "min", min);
    const bool has_max = rm_entry_num_opt(entry, "max", max);
    const bool has_step = rm_entry_num_opt(entry, "step", step);
    if (!has_min) {
        *min = 0;
    }
    if (!has_max) {
        *max = 0;
    }
    if (!has_step) {
        *step = 0;
    }
    return has_min || has_max || has_step;
}

static esp_rmaker_param_val_t rm_initial(const rm_param_t *p, const cJSON *entry, const char *value)
{
    switch (p->vt) {
    case VT_BOOL:
        if (entry) {
            return esp_rmaker_bool(rm_entry_bool(entry, false));
        }
        return esp_rmaker_bool(value && strcmp(p->key, "power") == 0 ? value_is_on(value) : false);
    case VT_INT: {
        int v = (int)rm_entry_num(entry, 0);
        if (strcmp(p->key, "brightness") == 0) {
            if (v <= 0) {
                v = 50;
            }
            if (v > 100) {
                v = 100;
            }
        }
        return esp_rmaker_int(v);
    }
    case VT_FLOAT: {
        float v = (float)rm_entry_num(entry, 0.0);
        if (!entry && value && strcmp(p->key, "temperature") == 0) {
            v = (float)atof(value);
        }
        return esp_rmaker_float(v);
    }
    case VT_STR:
    default:
        return esp_rmaker_str(value ? value : "");
    }
}

/* Temperature sensors historically keep the reading in the point summary. */
static float rm_temperature(const minihub_point_t *point)
{
    const cJSON *entry = rm_param_entry(point->props, "temperature");
    if (entry) {
        return (float)rm_entry_num(entry, 0.0);
    }
    return point->value[0] ? (float)atof(point->value) : 0.0f;
}

/* Every ESP_RMAKER_UI_SLIDER must carry min/max/step bounds or the RainMaker
 * app renders it wrong. Derive effective bounds: prefer catalog bounds (valid
 * range, or step-only), otherwise fall back to a safe default (0..max with
 * step = range/100) so the current value stays inside the range. */
static void rm_effective_slider_bounds(bool has, double mn, double mx, double st,
                                       double cur, double *emin, double *emax, double *estep)
{
    *emin = 0;
    if (has && mx > mn) {
        *emax = mx;
        *estep = (st > 0.0) ? st : (mx - mn) / 100.0;
    } else if (has && st > 0.0) {
        *emax = (cur > 0) ? cur : (st * 100.0);
        *estep = st;
    } else {
        *emax = (cur > 0) ? cur : 100.0;
        *estep = (*emax - *emin) / 100.0;
    }
    if (*emax <= *emin) {
        *emax = *emin + 1.0;
        *estep = (*emax - *emin) / 100.0;
    }
}

static void rm_add_params(bridge_entry_t *e, const minihub_point_t *point,
                          const rm_param_t *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const rm_param_t *p = &list[i];
        const cJSON *entry = rm_param_entry(point->props, p->key);
        if (!p->always && !entry) {
            continue;
        }
        const bool writable = p->rw && !rm_entry_readonly(entry);
        esp_rmaker_param_t *param = esp_rmaker_param_create(
            p->rm_name, p->rm_type, rm_initial(p, entry, point->value),
            writable ? (PROP_FLAG_READ | PROP_FLAG_WRITE) : PROP_FLAG_READ);
        if (!param) {
            continue;
        }
        if (p->ui) {
            esp_rmaker_param_add_ui_type(param, p->ui);
        }
        if (p->vt == VT_INT || p->vt == VT_FLOAT) {
            /* RainMaker requires bounds on a slider. */
            double bmin = 0, bmax = 0, bstep = 0;
            bool got = rm_entry_bounds(entry, &bmin, &bmax, &bstep);
            double emin, emax, estep;
            rm_effective_slider_bounds(got, bmin, bmax, bstep,
                                       rm_entry_num(entry, 0), &emin, &emax, &estep);
            const esp_rmaker_param_val_t vmin = (p->vt == VT_INT) ? esp_rmaker_int((int)emin) : esp_rmaker_float((float)emin);
            const esp_rmaker_param_val_t vmax = (p->vt == VT_INT) ? esp_rmaker_int((int)emax) : esp_rmaker_float((float)emax);
            const esp_rmaker_param_val_t vstep = (p->vt == VT_INT) ? esp_rmaker_int((int)estep) : esp_rmaker_float((float)estep);
            esp_rmaker_param_add_bounds(param, vmin, vmax, vstep);
        }
        esp_rmaker_device_add_param(e->device, param);
    }
}

static void rm_report_params(bridge_entry_t *e, const minihub_point_t *point,
                             const rm_param_t *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const rm_param_t *p = &list[i];
        esp_rmaker_param_t *param = esp_rmaker_device_get_param_by_name(e->device, p->rm_name);
        if (!param) {
            continue;
        }
        const cJSON *entry = rm_param_entry(point->props, p->key);
        bool have = entry != NULL;
        if (!have && strcmp(p->key, "temperature") == 0 && point->value[0]) {
            have = true;
        } else if (!have && strcmp(p->key, "power") == 0) {
            have = true;
        }
        if (!have) {
            continue;
        }
        esp_rmaker_param_update_and_report(param, rm_initial(p, entry, point->value));
    }
}

static rmaker_kind_t rmaker_kind_for_type(const char *type)
{
    if (!strcmp(type, "switch")) {
        return K_SWITCH;
    }
    if (!strcmp(type, "light") || !strcmp(type, "light_strip") || !strcmp(type, "fan_light")) {
        return K_LIGHT;
    }
    if (!strcmp(type, "camera")) {
        return K_CAMERA;
    }
    if (!strncmp(type, "sensor", 6)) { /* sensor / sensor_* */
        return K_SENSOR;
    }
    if (!strcmp(type, "speaker")) {
        return K_SPEAKER;
    }
    return K_GENERIC;
}

static esp_rmaker_node_t *s_node;
EXT_RAM_BSS_ATTR static bridge_entry_t s_entries[BRIDGE_MAX_DEVICES];
static size_t s_entry_count;
static bool s_updating_from_cloud;
static bool s_suppress_config_report;
static esp_rmaker_device_t *s_hub_device;
static esp_timer_handle_t s_hub_timer;

static void maybe_report_node_config(void)
{
    if (!s_suppress_config_report) {
        (void)esp_rmaker_report_node_details();
    }
}

static void point_id_to_device_name(const char *point_id, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; point_id[i] != '\0' && j + 1 < out_len; i++) {
        char c = point_id[i];
        out[j++] = (c == '/') ? '_' : c;
    }
    out[j] = '\0';
}

static bridge_entry_t *find_entry(const char *point_id)
{
    for (size_t i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].point_id, point_id) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static esp_err_t invoke_cmd(const char *point_id, const char *cmd, cJSON *params)
{
    minihub_command_t c = {0};
    strncpy(c.point_id, point_id, sizeof(c.point_id) - 1);
    strncpy(c.cmd, cmd, sizeof(c.cmd) - 1);
    c.params = params;
    return minihub_command_invoke(&c);
}

/* Translate an official RainMaker Entertainment param write into the hub speaker
 * point's command vocabulary. Returns ESP_OK on success. */
static esp_err_t speaker_write(const char *point_id, const char *param_name,
                              const esp_rmaker_param_val_t *val)
{
    cJSON *p = NULL;
    const char *cmd = NULL;

    if (strcmp(param_name, "media-activity-control") == 0) {
        const char *a = (val->type == RMAKER_VAL_TYPE_STRING && val->val.s) ? val->val.s : "";
        if (!strcmp(a, "play") || !strcmp(a, "resume")) {
            cmd = "play_resume";
        } else if (!strcmp(a, "pause") || !strcmp(a, "stop")) {
            cmd = "play_pause";
        } else if (!strcmp(a, "next")) {
            cmd = "play_next";
        } else if (!strcmp(a, "previous") || !strcmp(a, "prev")) {
            cmd = "play_prev";
        } else {
            ESP_LOGW(TAG, "speaker: unsupported media-activity-control '%s'", a);
            return ESP_ERR_NOT_SUPPORTED;
        }
        p = cJSON_CreateObject();
        if (p) {
            cJSON_AddStringToObject(p, "parameter", cmd);
            cJSON_AddBoolToObject(p, "value", true);
        }
    } else if (strcmp(param_name, "volume") == 0) {
        int hub_vol = 0;
        if (val->type == RMAKER_VAL_TYPE_FLOAT) {
            hub_vol = (int)(val->val.f * 5.0f + 0.5f);
        } else if (val->type == RMAKER_VAL_TYPE_INTEGER) {
            hub_vol = (int)(val->val.i * 5);
        }
        if (hub_vol < 0) {
            hub_vol = 0;
        }
        if (hub_vol > 100) {
            hub_vol = 100;
        }
        p = cJSON_CreateObject();
        if (p) {
            cJSON_AddStringToObject(p, "parameter", "volume");
            cJSON_AddNumberToObject(p, "value", hub_vol);
        }
    } else if (strcmp(param_name, "mute") == 0) {
        cmd = val->val.b ? "mute" : "unmute";
        p = cJSON_CreateObject();
        if (p) {
            cJSON_AddStringToObject(p, "parameter", cmd);
            cJSON_AddBoolToObject(p, "value", true);
        }
    } else if (strcmp(param_name, "power") == 0) {
        return invoke_cmd(point_id, val->val.b ? "turn_on" : "turn_off", NULL);
    } else {
        /* media-playback-state / media-activity-state are read-only status. */
        return ESP_OK;
    }

    if (!p) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = invoke_cmd(point_id, "set", p);
    cJSON_Delete(p);
    return err;
}

static esp_err_t write_cb(const esp_rmaker_device_t *device, const esp_rmaker_param_t *param,
                          const esp_rmaker_param_val_t val, void *priv_data,
                          esp_rmaker_write_ctx_t *ctx)
{
    (void)ctx;
    const char *point_id = (const char *)priv_data;
    if (!point_id || point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *param_name = esp_rmaker_param_get_name(param);
    if (!param_name) {
        return ESP_ERR_INVALID_ARG;
    }

    s_updating_from_cloud = true;
    esp_err_t err = ESP_OK;

    bridge_entry_t *e = find_entry(point_id);
    if (e && e->kind == (int)K_SPEAKER) {
        /* Official RainMaker Entertainment params -> hub speaker commands. */
        bool report_back = (strcmp(param_name, "media-activity-control") != 0);
        err = speaker_write(point_id, param_name, &val);
        if (err == ESP_OK && report_back) {
            esp_rmaker_param_update_and_report(param, val);
        }
    } else if (e && e->kind == (int)K_GENERIC) {
        /* Generic pass-through device (unsupported catalog type): forward the
         * cloud write as the point's generic "set" command. */
        cJSON *params = cJSON_CreateObject();
        if (!params) {
            err = ESP_ERR_NO_MEM;
        } else {
            cJSON_AddStringToObject(params, "parameter", param_name);
            switch (val.type) {
            case RMAKER_VAL_TYPE_BOOLEAN:
                cJSON_AddBoolToObject(params, "value", val.val.b);
                break;
            case RMAKER_VAL_TYPE_INTEGER:
                cJSON_AddNumberToObject(params, "value", val.val.i);
                break;
            case RMAKER_VAL_TYPE_FLOAT:
                cJSON_AddNumberToObject(params, "value", val.val.f);
                break;
            case RMAKER_VAL_TYPE_STRING:
                cJSON_AddStringToObject(params, "value", val.val.s ? val.val.s : "");
                break;
            default:
                cJSON_Delete(params);
                ESP_LOGW(TAG, "unsupported generic write type for %s/%s", point_id, param_name);
                s_updating_from_cloud = false;
                return ESP_ERR_NOT_SUPPORTED;
            }
            err = invoke_cmd(point_id, "set", params);
            if (err == ESP_OK) {
                esp_rmaker_param_update_and_report(param, val);
            }
            cJSON_Delete(params);
        }
    } else if (strcmp(param_name, ESP_RMAKER_DEF_POWER_NAME) == 0) {
        err = invoke_cmd(point_id, val.val.b ? "turn_on" : "turn_off", NULL);
        if (err == ESP_OK) {
            esp_rmaker_param_update_and_report(param, val);
        }
    } else if (strcmp(param_name, ESP_RMAKER_DEF_BRIGHTNESS_NAME) == 0) {
        int bright = val.val.i;
        if (bright < 0) {
            bright = 0;
        }
        if (bright > 100) {
            bright = 100;
        }
        cJSON *params = cJSON_CreateObject();
        if (params) {
            cJSON_AddNumberToObject(params, "brightness", bright);
        }
        err = invoke_cmd(point_id, "set_brightness", params);
        if (params) {
            cJSON_Delete(params);
        }
        if (err == ESP_OK) {
            esp_rmaker_param_update_and_report(param, esp_rmaker_int(bright));
        }
    } else if (strcmp(param_name, ESP_RMAKER_DEF_CCT_NAME) == 0) {
        cJSON *params = cJSON_CreateObject();
        if (params) {
            cJSON_AddNumberToObject(params, "cct", val.val.i);
        }
        err = invoke_cmd(point_id, "set_color_temp", params);
        if (params) {
            cJSON_Delete(params);
        }
        if (err == ESP_OK) {
            esp_rmaker_param_update_and_report(param, val);
        }
    } else if (strcmp(param_name, ESP_RMAKER_DEF_NAME_PARAM) == 0) {
        esp_rmaker_param_update_and_report(param, val);
    } else {
        const rm_param_t *p = rm_param_by_rm_name(param_name);
        if (!p || !p->rw) {
            ESP_LOGD(TAG, "ignore write %s on %s", param_name, point_id);
        } else {
            minihub_point_t *pt = minihub_point_get(point_id);
            if (rm_entry_readonly(rm_param_entry(pt ? pt->props : NULL, p->key))) {
                err = ESP_ERR_NOT_SUPPORTED;
                ESP_LOGW(TAG, "%s.%s is read-only on this device", point_id, p->key);
            } else {
                cJSON *params = cJSON_CreateObject();
                if (!params) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    cJSON_AddStringToObject(params, "parameter", p->key);
                    switch (p->vt) {
                    case VT_BOOL:  cJSON_AddBoolToObject(params, "value", val.val.b); break;
                    case VT_INT:   cJSON_AddNumberToObject(params, "value", val.val.i); break;
                    case VT_FLOAT: cJSON_AddNumberToObject(params, "value", val.val.f); break;
                    default:       cJSON_AddStringToObject(params, "value", val.val.s ? val.val.s : ""); break;
                    }
                    err = invoke_cmd(point_id, "set", params);
                    if (err == ESP_OK) {
                        esp_rmaker_param_update_and_report(param, val);
                    }
                    cJSON_Delete(params);
                }
            }
        }
    }

    s_updating_from_cloud = false;
    /* media-activity-control (and any speaker param the bridge does not echo back)
     * relies on the plugin's point.set to push the new playback state up. That
     * point.set fired above while s_updating_from_cloud was still set, so its
     * report was suppressed. Re-fire it now so the cloud reflects play/pause
     * promptly instead of waiting for the next poll cycle. */
    if (err == ESP_OK && e && e->kind == (int)K_SPEAKER &&
        strcmp(param_name, "media-activity-control") == 0) {
        uint16_t idx = minihub_point_index_of(point_id);
        if (idx != UINT16_MAX) {
            rainmaker_bridge_on_point_updated(idx);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cloud write %s/%s failed: %s", point_id, param_name, esp_err_to_name(err));
    }
    return err;
}

static void report_switch_or_light(bridge_entry_t *e, const minihub_point_t *point)
{
    esp_rmaker_param_t *power = esp_rmaker_device_get_param_by_name(e->device, ESP_RMAKER_DEF_POWER_NAME);
    if (power) {
        esp_rmaker_param_update_and_report(power, esp_rmaker_bool(value_is_on(point->value)));
    }
    if ((rmaker_kind_t)e->kind == K_LIGHT) {
        rm_report_params(e, point, RM_LIGHT_PARAMS, sizeof(RM_LIGHT_PARAMS) / sizeof(RM_LIGHT_PARAMS[0]));
    } else {
        rm_report_params(e, point, RM_SWITCH_PARAMS, sizeof(RM_SWITCH_PARAMS) / sizeof(RM_SWITCH_PARAMS[0]));
    }
}

static void report_sensor(bridge_entry_t *e, const minihub_point_t *point)
{
    esp_rmaker_param_t *temp = esp_rmaker_device_get_param_by_name(e->device, ESP_RMAKER_DEF_TEMPERATURE_NAME);
    if (temp) {
        const float t = rm_temperature(point);
        esp_rmaker_param_update_and_report(temp, esp_rmaker_float(t));
    }
    rm_report_params(e, point, RM_SENSOR_PARAMS, sizeof(RM_SENSOR_PARAMS) / sizeof(RM_SENSOR_PARAMS[0]));
}

static void report_camera(bridge_entry_t *e, const minihub_point_t *point)
{
    esp_rmaker_param_t *power = esp_rmaker_device_get_param_by_name(e->device, ESP_RMAKER_DEF_POWER_NAME);
    if (power) {
        esp_rmaker_param_update_and_report(power, esp_rmaker_bool(value_is_on(point->value)));
    }
    if (e->has_recording) {
        esp_rmaker_param_t *rec = esp_rmaker_device_get_param_by_name(e->device, "Recording");
        if (rec) {
            bool recording = false;
            if (point->props) {
                const cJSON *v = cJSON_GetObjectItem(point->props, "recording");
                recording = cJSON_IsTrue(v);
            }
            esp_rmaker_param_update_and_report(rec, esp_rmaker_bool(recording));
        }
    }
}

static esp_err_t alloc_priv(bridge_entry_t *e)
{
    free(e->priv_point_id);
    e->priv_point_id = strdup(e->point_id);
    return e->priv_point_id ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t create_switch(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    e->device = esp_rmaker_switch_device_create(e->device_name, e->priv_point_id, value_is_on(point->value));
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);
    rm_add_params(e, point, RM_SWITCH_PARAMS, sizeof(RM_SWITCH_PARAMS) / sizeof(RM_SWITCH_PARAMS[0]));
    return esp_rmaker_node_add_device(s_node, e->device);
}

static esp_err_t create_light(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    e->device = esp_rmaker_lightbulb_device_create(e->device_name, e->priv_point_id, value_is_on(point->value));
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);
    rm_add_params(e, point, RM_LIGHT_PARAMS, sizeof(RM_LIGHT_PARAMS) / sizeof(RM_LIGHT_PARAMS[0]));
    return esp_rmaker_node_add_device(s_node, e->device);
}

static esp_err_t create_sensor(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    const float t = rm_temperature(point);
    if (!strcmp(point->type, "sensor_temp_humidity")) {
        e->device = esp_rmaker_temp_sensor_device_create(e->device_name, e->priv_point_id, t);
    } else {
        e->device = esp_rmaker_device_create(e->device_name, "esp.device.sensor", e->priv_point_id);
    }
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);
    rm_add_params(e, point, RM_SENSOR_PARAMS, sizeof(RM_SENSOR_PARAMS) / sizeof(RM_SENSOR_PARAMS[0]));
    return esp_rmaker_node_add_device(s_node, e->device);
}

static esp_err_t create_camera(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    e->device = esp_rmaker_device_create(e->device_name, "esp.device.camera", e->priv_point_id);
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);

    esp_rmaker_param_t *name = esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM, e->device_name);
    if (name) {
        esp_rmaker_device_add_param(e->device, name);
    }

    esp_rmaker_param_t *power = esp_rmaker_param_create(ESP_RMAKER_DEF_POWER_NAME, ESP_RMAKER_PARAM_POWER,
                                                        esp_rmaker_bool(value_is_on(point->value)),
                                                        PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (power) {
        esp_rmaker_param_add_ui_type(power, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(e->device, power);
    }

    bool recording = false;
    if (point->props) {
        const cJSON *v = cJSON_GetObjectItem(point->props, "recording");
        recording = cJSON_IsTrue(v);
    }
    esp_rmaker_param_t *rec = esp_rmaker_param_create("Recording", "esp.param.recording",
                                                      esp_rmaker_bool(recording), PROP_FLAG_READ);
    if (rec) {
        esp_rmaker_param_add_ui_type(rec, ESP_RMAKER_UI_TEXT);
        esp_rmaker_device_add_param(e->device, rec);
        e->has_recording = true;
    }

    return esp_rmaker_node_add_device(s_node, e->device);
}

/* Generic pass-through for catalog types with no dedicated mapping. Builds one
 * RainMaker param per live entry in props.params, preserving the original
 * parameter name so a custom App can branch on it. */
static void report_generic(bridge_entry_t *e, const minihub_point_t *point)
{
    const cJSON *params = point->props ? cJSON_GetObjectItem(point->props, "params") : NULL;
    if (!params || !cJSON_IsObject(params)) {
        return;
    }
    for (cJSON *it = params->child; it; it = it->next) {
        const char *key = it->string;
        if (!key) {
            continue;
        }
        esp_rmaker_param_t *param = esp_rmaker_device_get_param_by_name(e->device, key);
        if (!param) {
            continue;
        }
        const cJSON *entry = cJSON_IsObject(it) ? it : NULL;
        const cJSON *val = rm_param_value(entry);

        /* Catalog widget drives the value type we report, so a numeric slider
         * stays numeric even when the instance carries a stringified value. */
        const char *widget = NULL;
        const minihub_device_widget_entry_t *dw = minihub_device_widget_find(point->type, key);
        cJSON *style = (dw && dw->style_json) ? cJSON_Parse(dw->style_json) : NULL;
        if (style) {
            cJSON *jw = cJSON_GetObjectItemCaseSensitive(style, "widget");
            if (cJSON_IsString(jw) && jw->valuestring[0]) {
                widget = jw->valuestring;
                if (strncmp(widget, "ui.", 3) == 0) {
                    widget += 3; /* catalog ids are "ui.<name>" */
                }
            }
        }
        if (widget && strcmp(widget, "slider") == 0) {
            esp_rmaker_param_update_and_report(param, esp_rmaker_float((float)rm_entry_num(entry, 0)));
        } else if (widget && strcmp(widget, "button") == 0) {
            /* value-less action: nothing to report */
        } else if (widget && strcmp(widget, "toggle") == 0) {
            esp_rmaker_param_update_and_report(param, esp_rmaker_bool(rm_entry_bool(entry, false)));
        } else if (widget && (strcmp(widget, "dropdown") == 0 || strcmp(widget, "hidden") == 0)) {
            const char *s = (val && cJSON_IsString(val)) ? val->valuestring : "";
            esp_rmaker_param_update_and_report(param, esp_rmaker_str(s));
        } else if (widget && strcmp(widget, "text") == 0) {
            /* Readout: report by the param's value type (matches create). */
            if (cJSON_IsBool(val)) {
                esp_rmaker_param_update_and_report(param, esp_rmaker_bool(rm_entry_bool(entry, false)));
            } else if (cJSON_IsNumber(val)) {
                esp_rmaker_param_update_and_report(param, esp_rmaker_float((float)rm_entry_num(entry, 0)));
            } else {
                const char *s = (val && cJSON_IsString(val)) ? val->valuestring : "";
                esp_rmaker_param_update_and_report(param, esp_rmaker_str(s));
            }
        } else if (cJSON_IsBool(val)) {
            esp_rmaker_param_update_and_report(param, esp_rmaker_bool(rm_entry_bool(entry, false)));
        } else if (cJSON_IsNumber(val)) {
            esp_rmaker_param_update_and_report(param, esp_rmaker_float((float)rm_entry_num(entry, 0)));
        } else if (cJSON_IsString(val)) {
            esp_rmaker_param_update_and_report(param, esp_rmaker_str(val->valuestring ? val->valuestring : ""));
        }
        if (style) {
            cJSON_Delete(style);
        }
    }
}

/* Build one RainMaker param for a generic pass-through entry. The catalog
 * (minihub_device_widget) is consulted first so the control matches the console:
 * a slider gets bounds, a media button becomes a push button, etc. Falls back to
 * the instance value type when the catalog has no entry (e.g. a vendor param). */
static void create_generic_param(bridge_entry_t *e, const minihub_point_t *point,
                                 const char *key, const cJSON *entry, const cJSON *val)
{
    bool readonly = rm_entry_readonly(entry);
    uint8_t flags = readonly ? PROP_FLAG_READ : (PROP_FLAG_READ | PROP_FLAG_WRITE);
    char ptype[64];
    snprintf(ptype, sizeof(ptype), "esp.param.%s", key);

    const char *widget = NULL;
    double mn = 0, mx = 100, st = 1;
    bool has_bounds = false;
    const minihub_device_widget_entry_t *dw = minihub_device_widget_find(point->type, key);
    cJSON *style = (dw && dw->style_json) ? cJSON_Parse(dw->style_json) : NULL;
    if (style) {
        cJSON *jw = cJSON_GetObjectItemCaseSensitive(style, "widget");
        if (cJSON_IsString(jw) && jw->valuestring[0]) {
            widget = jw->valuestring; /* valid until cJSON_Delete(style) below */
            /* Catalog ui_element values are "ui.<name>" (ui.button, ui.slider,
             * ui.toggle, ui.text); strip the prefix so the bare name matches. */
            if (strncmp(widget, "ui.", 3) == 0) {
                widget += 3;
            }
        }
        cJSON *v;
        if ((v = cJSON_GetObjectItemCaseSensitive(style, "min")) && cJSON_IsNumber(v)) {
            mn = v->valuedouble;
            has_bounds = true;
        }
        if ((v = cJSON_GetObjectItemCaseSensitive(style, "max")) && cJSON_IsNumber(v)) {
            mx = v->valuedouble;
            has_bounds = true;
        }
        if ((v = cJSON_GetObjectItemCaseSensitive(style, "step")) && cJSON_IsNumber(v)) {
            st = v->valuedouble;
        }
    }
    if (!has_bounds && rm_entry_bounds(entry, &mn, &mx, &st)) {
        has_bounds = true;
    }

    esp_rmaker_param_t *param = NULL;
    if (widget && strcmp(widget, "slider") == 0) {
        const double v = rm_entry_num(entry, has_bounds ? mn : 0);
        param = esp_rmaker_param_create(key, ptype, esp_rmaker_float((float)v), flags);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_SLIDER);
            /* RainMaker requires every slider to carry min/max/step bounds. */
            double emin, emax, estep;
            rm_effective_slider_bounds(has_bounds, mn, mx, st, v, &emin, &emax, &estep);
            esp_rmaker_param_add_bounds(param,
                esp_rmaker_float((float)emin), esp_rmaker_float((float)emax),
                esp_rmaker_float((float)estep));
        }
    } else if (widget && strcmp(widget, "button") == 0) {
        /* Momentary action (e.g. media play/mute). Expose as a push button; the
         * App tap forwards a generic "set" command (see write_cb). */
        param = esp_rmaker_param_create(key, ptype, esp_rmaker_bool(false), flags);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_PUSHBUTTON);
        }
    } else if (widget && strcmp(widget, "toggle") == 0) {
        param = esp_rmaker_param_create(key, ptype,
            esp_rmaker_bool(rm_entry_bool(entry, false)), flags);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_TOGGLE);
        }
    } else if (widget && strcmp(widget, "dropdown") == 0) {
        const char *s = (val && cJSON_IsString(val)) ? val->valuestring : "";
        param = esp_rmaker_param_create(key, ptype, esp_rmaker_str(s), flags);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_DROPDOWN);
        }
    } else if (widget && strcmp(widget, "hidden") == 0) {
        const char *s = (val && cJSON_IsString(val)) ? val->valuestring : "";
        param = esp_rmaker_param_create(key, ptype, esp_rmaker_str(s), flags);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_HIDDEN);
        }
    } else if (widget && strcmp(widget, "text") == 0) {
        /* Readout / status label: always a plain text control, never an ON/OFF
         * toggle or a slider, even when the instance value is a bool (e.g. a
         * contact sensor) or a number. Keep the value type matching the data so
         * report_generic() can push it back unchanged. */
        if (cJSON_IsBool(val)) {
            param = esp_rmaker_param_create(key, ptype,
                esp_rmaker_bool(rm_entry_bool(entry, false)), flags);
        } else if (cJSON_IsNumber(val)) {
            param = esp_rmaker_param_create(key, ptype,
                esp_rmaker_float((float)rm_entry_num(entry, 0)), flags);
        } else {
            const char *s = (val && cJSON_IsString(val)) ? val->valuestring : "";
            param = esp_rmaker_param_create(key, ptype, esp_rmaker_str(s), flags);
        }
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_TEXT);
        }
    } else if (cJSON_IsBool(val)) {
        param = esp_rmaker_param_create(key, ptype,
            esp_rmaker_bool(rm_entry_bool(entry, false)), flags);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_TOGGLE);
        }
    } else if (cJSON_IsNumber(val)) {
        const double v = rm_entry_num(entry, 0);
        param = esp_rmaker_param_create(key, ptype, esp_rmaker_float((float)v), flags);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_SLIDER);
            /* RainMaker requires every slider to carry min/max/step bounds. */
            double emin, emax, estep;
            rm_effective_slider_bounds(has_bounds, mn, mx, st, v, &emin, &emax, &estep);
            esp_rmaker_param_add_bounds(param,
                esp_rmaker_float((float)emin), esp_rmaker_float((float)emax),
                esp_rmaker_float((float)estep));
        }
    } else if (cJSON_IsString(val)) {
        param = esp_rmaker_param_create(key, ptype,
            esp_rmaker_str(val->valuestring ? val->valuestring : ""), flags);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_TEXT);
        }
    } else if (!val && !readonly) {
        /* Value-less command (e.g. a media button): expose as a writable push
         * button; the write is forwarded as a generic "set". */
        param = esp_rmaker_param_create(key, ptype,
            esp_rmaker_bool(false), PROP_FLAG_READ | PROP_FLAG_WRITE);
        if (param) {
            esp_rmaker_param_add_ui_type(param, ESP_RMAKER_UI_PUSHBUTTON);
        }
    }
    if (style) {
        cJSON_Delete(style);
    }
    if (param) {
        esp_rmaker_device_add_param(e->device, param);
    }
}

static esp_err_t create_generic(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    char dev_type[64];
    snprintf(dev_type, sizeof(dev_type), "esp.device.%s", point->type);
    e->device = esp_rmaker_device_create(e->device_name, dev_type, e->priv_point_id);
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);

    /* One RainMaker param per live parameter in props.params. */
    const cJSON *params = point->props ? cJSON_GetObjectItem(point->props, "params") : NULL;
    if (params && cJSON_IsObject(params)) {
        for (cJSON *it = params->child; it; it = it->next) {
            const char *key = it->string;
            if (!key) {
                continue;
            }
            const cJSON *entry = cJSON_IsObject(it) ? it : NULL;
            const cJSON *val = rm_param_value(entry);
            create_generic_param(e, point, key, entry, val);
        }
    }

    return esp_rmaker_node_add_device(s_node, e->device);
}

/* Speaker: the RainMaker-facing schema is the OFFICIAL Entertainment standard
 * (esp.param.media-activity-control / media-playback-state / media-activity-state
 * / volume / mute). The hub point speaks its own command vocabulary
 * (play_prev / play_pause / play_resume / play_next / mute / unmute / volume /
 * playing / muted). This conversion layer owns the mapping, so the shared device
 * catalog (parameters.csv / devices.csv) stays generic and is NOT touched.
 * playing / muted are hub-point status only and are never surfaced as raw
 * controls here. */
static esp_err_t create_speaker(bridge_entry_t *e, const minihub_point_t *point)
{
    if (alloc_priv(e) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    char dev_type[64];
    snprintf(dev_type, sizeof(dev_type), "esp.device.%s", point->type);
    e->device = esp_rmaker_device_create(e->device_name, dev_type, e->priv_point_id);
    if (!e->device) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(e->device, write_cb, NULL);

    /* Dropdown choice lists must outlive the param (esp_rmaker stores the
     * pointer, no copy). Static storage satisfies that for free. */
    static const char *activity_actions[] = {"play", "pause", "next", "previous"};
    static const char *playback_states[]  = {"play", "pause", "stop"};
    static const char *activity_states[]  = {"active", "inactive"};

    /* Seed params from the point's current state so the app shows real values on
     * first connect (params otherwise stay at create-time defaults: power off,
     * volume 0, empty state strings) until the first on_point_updated. The app
     * shows an empty valid-string param as the "Select option" placeholder. */
    bool playing = rm_entry_bool(rm_param_entry(point->props, "playing"), false);
    float hub_volume = (float)rm_entry_num(rm_param_entry(point->props, "volume"), 0);
    bool muted = rm_entry_bool(rm_param_entry(point->props, "muted"), false);
    bool powered = rm_entry_bool(rm_param_entry(point->props, "power"), false);
    float rm_volume = hub_volume / 5.0f;   /* hub 0..100 -> RainMaker 0..20 */
    if (rm_volume < 0.0f) {
        rm_volume = 0.0f;
    }
    if (rm_volume > 20.0f) {
        rm_volume = 20.0f;
    }

    esp_rmaker_param_t *p;
    esp_rmaker_param_t *power_param = NULL;

    /* power (Bool toggle, R/W, primary) */
    p = esp_rmaker_param_create("power", "esp.param.power",
                                esp_rmaker_bool(powered), PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(e->device, p);
        power_param = p;
    }
    /* volume (Float slider 0-20 step 1, R/W) */
    p = esp_rmaker_param_create("volume", "esp.param.volume",
                                esp_rmaker_float(rm_volume), PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_SLIDER);
        esp_rmaker_param_add_bounds(p, esp_rmaker_float(0.0f), esp_rmaker_float(20.0f), esp_rmaker_float(1.0f));
        esp_rmaker_device_add_param(e->device, p);
    }
    /* mute (Bool toggle, R/W) */
    p = esp_rmaker_param_create("mute", "esp.param.mute",
                                esp_rmaker_bool(muted), PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_TOGGLE);
        esp_rmaker_device_add_param(e->device, p);
    }
    /* media-activity-control (String dropdown, R/W; command-only, no initial) */
    p = esp_rmaker_param_create("media-activity-control", "esp.param.rm_spk_ctl",
                                esp_rmaker_str(""), PROP_FLAG_READ | PROP_FLAG_WRITE);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_DROPDOWN);
        esp_rmaker_param_add_valid_str_list(p, activity_actions, 4);
        esp_rmaker_device_add_param(e->device, p);
    }
    /* media-playback-state (String dropdown, READ-ONLY status: device -> RM) */
    p = esp_rmaker_param_create("media-playback-state", "esp.param.rm_spk_pb",
                                esp_rmaker_str(playing ? "play" : "pause"), PROP_FLAG_READ);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_DROPDOWN);
        esp_rmaker_param_add_valid_str_list(p, playback_states, 3);
        esp_rmaker_device_add_param(e->device, p);
    }
    /* media-activity-state (String dropdown, READ-ONLY status: device -> RM) */
    p = esp_rmaker_param_create("media-activity-state", "esp.param.rm_spk_act",
                                esp_rmaker_str(playing ? "active" : "inactive"), PROP_FLAG_READ);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_DROPDOWN);
        esp_rmaker_param_add_valid_str_list(p, activity_states, 2);
        esp_rmaker_device_add_param(e->device, p);
    }

    if (power_param) {
        esp_rmaker_device_assign_primary_param(e->device, power_param);
    }

    return esp_rmaker_node_add_device(s_node, e->device);
}

esp_err_t rainmaker_bridge_add_point(const minihub_point_t *point)
{
    if (!s_node || !point || point->point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    rmaker_kind_t kind = rmaker_kind_for_type(point->type);
    if (find_entry(point->point_id)) {
        return ESP_OK;
    }
    if (s_entry_count >= BRIDGE_MAX_DEVICES) {
        return ESP_ERR_NO_MEM;
    }

    bridge_entry_t *e = &s_entries[s_entry_count];
    memset(e, 0, sizeof(*e));
    e->kind = (int)kind;
    strncpy(e->point_id, point->point_id, sizeof(e->point_id) - 1);
    point_id_to_device_name(point->point_id, e->device_name, sizeof(e->device_name));

    esp_err_t err;
    switch (kind) {
    case K_SWITCH:
        err = create_switch(e, point);
        break;
    case K_LIGHT:
        err = create_light(e, point);
        break;
    case K_CAMERA:
        err = create_camera(e, point);
        break;
    case K_SPEAKER:
        err = create_speaker(e, point);
        break;
    case K_GENERIC:
        err = create_generic(e, point);
        break;
    case K_SENSOR:
    default:
        err = create_sensor(e, point);
        break;
    }

    if (err != ESP_OK || !e->device) {
        ESP_LOGW(TAG, "add device for %s failed: %s", point->point_id, esp_err_to_name(err));
        free(e->priv_point_id);
        memset(e, 0, sizeof(*e));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    s_entry_count++;
    ESP_LOGI(TAG, "mapped %s (%s) -> %s", point->point_id, point->type, e->device_name);
    maybe_report_node_config();
    return ESP_OK;
}

esp_err_t rainmaker_bridge_remove_point(const char *point_id)
{
    if (!s_node || !point_id) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].point_id, point_id) != 0) {
            continue;
        }
        if (s_entries[i].device) {
            esp_rmaker_node_remove_device(s_node, s_entries[i].device);
            esp_rmaker_device_delete(s_entries[i].device);
        }
        free(s_entries[i].priv_point_id);
        for (size_t j = i; j + 1 < s_entry_count; j++) {
            s_entries[j] = s_entries[j + 1];
        }
        memset(&s_entries[s_entry_count - 1], 0, sizeof(bridge_entry_t));
        s_entry_count--;
        ESP_LOGI(TAG, "unmapped %s", point_id);
        maybe_report_node_config();
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* Map hub speaker point state into the official RainMaker Entertainment params.
 * Hub vocabulary: volume (0-100), muted (bool), playing (bool), power (bool). */
static void report_speaker(bridge_entry_t *e, const minihub_point_t *point)
{
    esp_rmaker_param_t *p;
    const cJSON *entry;

    entry = rm_param_entry(point->props, "volume");
    p = esp_rmaker_device_get_param_by_name(e->device, "volume");
    if (entry && p) {
        float hub = (float)rm_entry_num(entry, 0);
        esp_rmaker_param_update_and_report(p, esp_rmaker_float(hub / 5.0f));
    }

    entry = rm_param_entry(point->props, "muted");
    p = esp_rmaker_device_get_param_by_name(e->device, "mute");
    if (entry && p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_bool(rm_entry_bool(entry, false)));
    }

    entry = rm_param_entry(point->props, "playing");
    if (entry) {
        bool on = rm_entry_bool(entry, false);
        p = esp_rmaker_device_get_param_by_name(e->device, "media-playback-state");
        if (p) {
            esp_rmaker_param_update_and_report(p, esp_rmaker_str(on ? "play" : "pause"));
        }
        p = esp_rmaker_device_get_param_by_name(e->device, "media-activity-state");
        if (p) {
            esp_rmaker_param_update_and_report(p, esp_rmaker_str(on ? "active" : "inactive"));
        }
    }

    entry = rm_param_entry(point->props, "power");
    p = esp_rmaker_device_get_param_by_name(e->device, "power");
    if (entry && p) {
        esp_rmaker_param_update_and_report(p, esp_rmaker_bool(rm_entry_bool(entry, false)));
    }
}

void rainmaker_bridge_on_point_updated(uint16_t point_index)
{
    if (s_updating_from_cloud || !s_node) {
        return;
    }
    minihub_point_t *point = minihub_point_get_by_index(point_index);
    if (!point) {
        return;
    }
    bridge_entry_t *e = find_entry(point->point_id);
    if (!e || !e->device) {
        return;
    }
    rmaker_kind_t kind = rmaker_kind_for_type(point->type);
    if (kind == K_SENSOR) {
        report_sensor(e, point);
    } else if (kind == K_CAMERA) {
        report_camera(e, point);
    } else if (kind == K_SPEAKER) {
        report_speaker(e, point);
    } else if (kind == K_GENERIC) {
        report_generic(e, point);
    } else {
        report_switch_or_light(e, point);
    }
}

esp_err_t rainmaker_bridge_sync_all(void)
{
    if (!s_node) {
        return ESP_ERR_INVALID_STATE;
    }
    minihub_point_t *points = NULL;
    size_t count = 0;
    esp_err_t err = minihub_point_list(&points, &count);
    if (err != ESP_OK) {
        return err;
    }

    s_suppress_config_report = true;

    /* Drop RainMaker devices whose points no longer exist (soft reload / sync). */
    for (size_t i = 0; i < s_entry_count; ) {
        bool still_present = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(s_entries[i].point_id, points[j].point_id) == 0) {
                still_present = true;
                break;
            }
        }
        if (!still_present) {
            (void)rainmaker_bridge_remove_point(s_entries[i].point_id);
            continue;
        }
        i++;
    }

    for (size_t i = 0; i < count; i++) {
        rainmaker_bridge_add_point(&points[i]);
        if (points[i].props) {
            cJSON_Delete(points[i].props);
            points[i].props = NULL;
        }
    }
    free(points);

    s_suppress_config_report = false;
    (void)esp_rmaker_report_node_details();
    return ESP_OK;
}

static void on_point_updated(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event) {
        return;
    }
    rainmaker_bridge_on_point_updated(event->point_index);
}

static void on_point_registered(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event || !event->data) {
        return;
    }
    const cJSON *id = cJSON_GetObjectItem(event->data, "point_id");
    if (!cJSON_IsString(id) || !id->valuestring) {
        return;
    }
    minihub_point_t *p = minihub_point_get(id->valuestring);
    if (p) {
        rainmaker_bridge_add_point(p);
    }
}

static void on_point_unregistered(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event || !event->data) {
        return;
    }
    const cJSON *id = cJSON_GetObjectItem(event->data, "point_id");
    if (cJSON_IsString(id) && id->valuestring) {
        rainmaker_bridge_remove_point(id->valuestring);
    }
}

/* ---- MiniHub self device (the gateway itself, not a bridged Point) ---- */
static esp_err_t hub_write_cb(const esp_rmaker_device_t *device,
                              const esp_rmaker_param_t *param,
                              const esp_rmaker_param_val_t val, void *priv_data,
                              esp_rmaker_write_ctx_t *ctx)
{
    (void)device; (void)priv_data; (void)ctx;
    const char *name = esp_rmaker_param_get_name(param);
    if (name && strcmp(name, "Reboot") == 0 && val.val.b) {
        ESP_LOGI(TAG, "hub reboot requested via RainMaker");
        esp_restart();
    }
    return ESP_OK;
}

static void hub_timer_cb(void *arg)
{
    (void)arg;
    rainmaker_bridge_update_hub_status();
}

esp_err_t rainmaker_bridge_add_hub_device(esp_rmaker_node_t *node)
{
    if (!node) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_rmaker_device_t *dev = esp_rmaker_device_create("MiniHub Gateway", "esp.device.gateway", NULL);
    if (!dev) {
        return ESP_FAIL;
    }
    esp_rmaker_device_add_cb(dev, hub_write_cb, NULL);

    esp_rmaker_param_t *name = esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM, "MiniHub Gateway");
    if (name) {
        esp_rmaker_device_add_param(dev, name);
    }

    esp_rmaker_param_t *net = esp_rmaker_param_create("Network", "esp.param.network",
                                                     esp_rmaker_str(""), PROP_FLAG_READ);
    esp_rmaker_param_add_ui_type(net, ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(dev, net);

    esp_rmaker_param_t *ip = esp_rmaker_param_create("IP", "esp.param.ip",
                                                    esp_rmaker_str(""), PROP_FLAG_READ);
    esp_rmaker_param_add_ui_type(ip, ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(dev, ip);

    esp_rmaker_param_t *uptime = esp_rmaker_param_create("Uptime (min)", "esp.param.uptime",
                                                         esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_param_add_ui_type(uptime, ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(dev, uptime);

    esp_rmaker_param_t *heap = esp_rmaker_param_create("Free Heap (KB)", "esp.param.freeheap",
                                                       esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_param_add_ui_type(heap, ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(dev, heap);

    esp_rmaker_param_t *psram = esp_rmaker_param_create("PSRAM Free (KB)", "esp.param.psramfree",
                                                        esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_param_add_ui_type(psram, ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(dev, psram);

    esp_rmaker_param_t *count = esp_rmaker_param_create("Connected Devices", "esp.param.devices",
                                                        esp_rmaker_int(0), PROP_FLAG_READ);
    esp_rmaker_param_add_ui_type(count, ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(dev, count);

    const esp_app_desc_t *app = esp_app_get_description();
    const char *mh_ver = (app && app->version[0]) ? app->version : "unknown";
    const char *idf_ver = esp_get_idf_version();
    char fw_buf[128];
    snprintf(fw_buf, sizeof(fw_buf), "MiniHub: %s(%s)\nIDF: %s", CONFIG_MINIHUB_VERSION, mh_ver, idf_ver);
    esp_rmaker_param_t *firmware = esp_rmaker_param_create("Firmware", "esp.param.firmware",
        esp_rmaker_str(fw_buf), PROP_FLAG_READ);
    esp_rmaker_param_add_ui_type(firmware, ESP_RMAKER_UI_TEXT);
    esp_rmaker_device_add_param(dev, firmware);

    esp_rmaker_param_t *reboot = esp_rmaker_param_create("Reboot", "esp.param.reboot",
                                                         esp_rmaker_bool(false),
                                                         PROP_FLAG_WRITE);
    esp_rmaker_param_add_ui_type(reboot, ESP_RMAKER_UI_TRIGGER);
    esp_rmaker_device_add_param(dev, reboot);

    esp_err_t err = esp_rmaker_node_add_device(node, dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add hub device failed: %s", esp_err_to_name(err));
        return err;
    }
    s_hub_device = dev;
    ESP_LOGI(TAG, "mapped MiniHub (gateway) -> hub device");
    return ESP_OK;
}

esp_err_t rainmaker_bridge_update_hub_status(void)
{
    if (!s_hub_device) {
        return ESP_ERR_INVALID_STATE;
    }
    int up_min = (int)(esp_timer_get_time() / 1000000 / 60);
    int heap_kb = (int)(esp_get_free_heap_size() / 1024);
    int psram_kb = (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    int devs = (int)s_entry_count;

    char net_buf[32] = "down";
    char ip_buf[48] = "";
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
            const char *desc = esp_netif_get_desc(netif);
            snprintf(net_buf, sizeof(net_buf), "%s", desc ? desc : "up");
            snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&info.ip));
        }
    }

    esp_rmaker_param_t *p;
    p = esp_rmaker_device_get_param_by_name(s_hub_device, "Network");
    if (p) esp_rmaker_param_update_and_report(p, esp_rmaker_str(net_buf));
    p = esp_rmaker_device_get_param_by_name(s_hub_device, "IP");
    if (p) esp_rmaker_param_update_and_report(p, esp_rmaker_str(ip_buf));
    p = esp_rmaker_device_get_param_by_name(s_hub_device, "Uptime (min)");
    if (p) esp_rmaker_param_update_and_report(p, esp_rmaker_int(up_min));
    p = esp_rmaker_device_get_param_by_name(s_hub_device, "Free Heap (KB)");
    if (p) esp_rmaker_param_update_and_report(p, esp_rmaker_int(heap_kb));
    p = esp_rmaker_device_get_param_by_name(s_hub_device, "PSRAM Free (KB)");
    if (p) esp_rmaker_param_update_and_report(p, esp_rmaker_int(psram_kb));
    p = esp_rmaker_device_get_param_by_name(s_hub_device, "Connected Devices");
    if (p) esp_rmaker_param_update_and_report(p, esp_rmaker_int(devs));
    return ESP_OK;
}

esp_err_t rainmaker_bridge_init(esp_rmaker_node_t *node)
{
    if (!node) {
        return ESP_ERR_INVALID_ARG;
    }
    s_node = node;
    s_entry_count = 0;
    memset(s_entries, 0, sizeof(s_entries));

    esp_err_t err = minihub_event_bus_subscribe("point.updated", on_point_updated, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_event_bus_subscribe("point.registered", on_point_registered, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_event_bus_subscribe("point.unregistered", on_point_unregistered, NULL);
    if (err != ESP_OK) {
        return err;
    }

    esp_timer_create_args_t t = { .callback = hub_timer_cb, .name = "hub_status" };
    if (esp_timer_create(&t, &s_hub_timer) == ESP_OK) {
        esp_timer_start_periodic(s_hub_timer, 60000000); /* refresh hub telemetry every 60s */
    }

    return rainmaker_bridge_sync_all();
}
