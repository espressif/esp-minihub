/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host tests for the BLE Mesh adapter: the real ble_mesh_adapter.c against the
 * real capability map and the real device-widget catalog, with the Hub APIs it
 * calls replaced by recording seams (stubs/hub_stubs.c). */

#include "minihub_adapter.h"
#include "minihub_device_widget.h"
#include "hub_stubs.h"
#include "test_framework.h"

/* ELF section auto-registration has no Mach-O equivalent; the tests reach the
 * adapter through the exported struct instead. */
#undef REGISTER_ADAPTER
#define REGISTER_ADAPTER(symbol)
#include "ble_mesh_adapter.c"

#define LIGHT_UUID "00112233445566778899aabbccddeeff"

static cJSON *s_cfg;

/* Restart the adapter on a fresh config, as the Host does on a config change. */
static esp_err_t start_adapter(const char *json)
{
    hub_stub_reset();
    cJSON *cfg = json ? cJSON_Parse(json) : NULL;
    cJSON *prev = s_cfg;
    s_cfg = cfg;
    esp_err_t err = ble_mesh_adapter.init(cfg);
    if (err == ESP_OK) {
        err = ble_mesh_adapter.start();
    }
    cJSON_Delete(prev);
    return err;
}

static esp_err_t command(const char *point_id, const char *cmd, const char *params_json)
{
    minihub_command_t c = {0};
    strncpy(c.point_id, point_id, sizeof(c.point_id) - 1);
    strncpy(c.cmd, cmd, sizeof(c.cmd) - 1);
    c.params = params_json ? cJSON_Parse(params_json) : NULL;
    esp_err_t err = ble_mesh_adapter.handle_command(&c);
    cJSON_Delete(c.params);
    return err;
}

static const minihub_ble_mesh_device_t *config_light(void)
{
    static const char *json =
        "{\"devices\":[{\"point_id\":\"ble_mesh.bm_0001\",\"type\":\"light\",\"unicast\":1,"
        "\"uuid\":\"" LIGHT_UUID "\",\"models\":[\"0x1000\",\"0x1300\"]}]}";
    if (start_adapter(json) != ESP_OK) {
        return NULL;
    }
    return minihub_ble_mesh_find_by_point("ble_mesh.bm_0001");
}

/* --------------------------------------------------------------- baseline */

static void test_config_light_registers_one_point(void)
{
    const minihub_ble_mesh_device_t *dev = config_light();
    CHECK(dev != NULL);
    if (!dev) {
        return;
    }
    CHECK(dev->caps == (MINIHUB_BM_CAP_ONOFF | MINIHUB_BM_CAP_LIGHTNESS));
    CHECK(dev->elem_num == 1);

    const hub_point_t *p = hub_point_find("ble_mesh.bm_0001");
    CHECK(p != NULL);
    if (!p) {
        return;
    }
    CHECK_STR(p->point.type, "light");
    CHECK_STR(p->point.adapter, "ble_mesh");
    CHECK(p->point.online);
    CHECK_STR(p->point.value, "off");
    CHECK_STR(cJSON_GetStringValue(hub_point_param("ble_mesh.bm_0001", "power")), "off");
    CHECK_DBL(cJSON_GetNumberValue(hub_point_param("ble_mesh.bm_0001", "brightness")), 100.0);
    /* caps carry no CTL / HSL, so those catalog params are not exposed. */
    CHECK(hub_point_param("ble_mesh.bm_0001", "cct") == NULL);
    CHECK(hub_point_param("ble_mesh.bm_0001", "hue") == NULL);
}

static void test_turn_on_sends_onoff_and_echoes_the_summary(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_0001", "turn_on", NULL) == ESP_OK);
    const hub_mesh_cmd_t *c = hub_last_mesh_cmd();
    CHECK(c != NULL);
    if (c) {
        CHECK(c->kind == HUB_MESH_ONOFF);
        CHECK(c->unicast == 1);
        CHECK(c->onoff == MINIHUB_BM_ONOFF_ON);
    }
    const hub_point_t *p = hub_point_find("ble_mesh.bm_0001");
    CHECK(p && strcmp(p->point.value, "on") == 0);
}

/* The legacy OnOff commands and the status report publish one value: the point
 * summary AND the parameter the console's toggle reads. */

static void test_turn_on_updates_the_power_param(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_0001", "turn_on", NULL) == ESP_OK);
    CHECK_STR(cJSON_GetStringValue(hub_point_param("ble_mesh.bm_0001", "power")), "on");
}

static void test_turn_off_updates_the_power_param(void)
{
    CHECK(config_light() != NULL);
    /* Seed through the parameter path so "off" is a change, not the init value. */
    CHECK(command("ble_mesh.bm_0001", "set", "{\"parameter\":\"power\",\"value\":\"on\"}") == ESP_OK);
    CHECK(command("ble_mesh.bm_0001", "turn_off", NULL) == ESP_OK);
    CHECK_STR(cJSON_GetStringValue(hub_point_param("ble_mesh.bm_0001", "power")), "off");
}

static void test_toggle_flips_the_power_param(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_0001", "set", "{\"parameter\":\"power\",\"value\":\"on\"}") == ESP_OK);
    CHECK(command("ble_mesh.bm_0001", "toggle", NULL) == ESP_OK);
    const hub_point_t *p = hub_point_find("ble_mesh.bm_0001");
    CHECK_STR(p ? p->point.value : "x", "off");
    CHECK_STR(cJSON_GetStringValue(hub_point_param("ble_mesh.bm_0001", "power")), "off");
}

static void test_brightness_param_writes_lightness(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_0001", "set", "{\"parameter\":\"brightness\",\"value\":50}") == ESP_OK);
    const hub_mesh_cmd_t *c = hub_last_mesh_cmd();
    CHECK(c != NULL);
    if (c) {
        CHECK(c->kind == HUB_MESH_LIGHTNESS);
        /* 50% with half-up rounding: (50*65535/100)+0.5 → 32768 */
        CHECK(c->lightness == 32768);
    }
    CHECK_DBL(cJSON_GetNumberValue(hub_point_param("ble_mesh.bm_0001", "brightness")), 50.0);
}

static void test_brightness_above_the_catalog_max_is_clamped(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_0001", "set", "{\"parameter\":\"brightness\",\"value\":150}") == ESP_OK);
    const hub_mesh_cmd_t *c = hub_last_mesh_cmd();
    CHECK(c && c->kind == HUB_MESH_LIGHTNESS && c->lightness == 65535);
    CHECK_DBL(cJSON_GetNumberValue(hub_point_param("ble_mesh.bm_0001", "brightness")), 100.0);
}

static void test_unknown_command_is_not_supported(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_0001", "no_such_command", NULL) == ESP_ERR_NOT_SUPPORTED);
    CHECK(hub_mesh_cmd_count == 0);
}

static void test_unknown_point_is_not_found(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_ffff", "turn_on", NULL) == ESP_ERR_NOT_FOUND);
}

static void test_published_model_alias_still_resolves(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0002\",\"type\":\"switch\","
                        "\"unicast\":2,\"models\":[\"gen_onoff_srv\"]}]}") == ESP_OK);
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point("ble_mesh.bm_0002");
    CHECK(dev != NULL);
    CHECK(dev && dev->caps == MINIHUB_BM_CAP_ONOFF);
}

static void test_type_only_switch_gets_the_onoff_cap(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0003\",\"type\":\"switch\","
                        "\"unicast\":3}]}") == ESP_OK);
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point("ble_mesh.bm_0003");
    CHECK(dev != NULL);
    CHECK(dev && dev->caps == MINIHUB_BM_CAP_ONOFF);
    CHECK(hub_point_find("ble_mesh.bm_0003") != NULL);
}

static void test_set_color_temp_default_is_the_table_init(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0004\",\"type\":\"light\","
                        "\"unicast\":4,\"models\":[\"0x1000\",\"0x1303\"]}]}") == ESP_OK);
    CHECK(command("ble_mesh.bm_0004", "set_color_temp", NULL) == ESP_OK);
    const hub_mesh_cmd_t *c = hub_last_mesh_cmd();
    CHECK(c != NULL);
    if (c) {
        CHECK(c->kind == HUB_MESH_CTL);
        CHECK(c->temperature == 4000);
    }
}

static void test_onoff_report_updates_the_point_and_product_params(void)
{
    CHECK(config_light() != NULL);
    CHECK(hub_on_attr != NULL);
    minihub_ble_mesh_attr_evt_t evt = {0};
    evt.unicast = 1;
    evt.kind = MINIHUB_BM_ATTR_ONOFF;
    evt.u.onoff = 1;
    hub_on_attr(&evt, hub_cb_ctx);
    const hub_point_t *p = hub_point_find("ble_mesh.bm_0001");
    CHECK(p && strcmp(p->point.value, "on") == 0);
    CHECK_STR(cJSON_GetStringValue(hub_point_param("ble_mesh.bm_0001", "power")), "on");
    CHECK(hub_pp_apply_count == 1);
    if (hub_pp_apply_count == 1) {
        CHECK(hub_pp_applies[0].model_id == 0x1000);
        CHECK_STR(hub_pp_applies[0].field, "onoff");
        CHECK_DBL(hub_pp_applies[0].raw, 1.0);
    }
}

/* ------------------------------------------------- config model vocabulary */

/* Every name the table publishes must resolve through the config parser: a new
 * models[] row is a table edit, not a C edit. */
static void test_config_accepts_every_published_model_name(void)
{
    size_t n = minihub_bm_model_count();
    CHECK(n > 0);
    for (size_t i = 0; i < n; i++) {
        const minihub_bm_map_model_t *m = minihub_bm_model_at(i);
        CHECK(m != NULL);
        if (!m) {
            continue;
        }
        const char *names[2] = {m->name, m->alias};
        for (size_t k = 0; k < 2; k++) {
            if (!names[k][0]) {
                continue;
            }
            cJSON *item = cJSON_CreateString(names[k]);
            uint16_t id = 0;
            bool ok = parse_model_id(item, &id);
            if (!ok || id != m->model_id) {
                printf("FAIL %s: model name \"%s\" resolved=%d id=0x%04x, want 0x%04x\n", s_test,
                       names[k], (int)ok, (unsigned)id, (unsigned)m->model_id);
                s_failures++;
            }
            s_checks++;
            cJSON_Delete(item);
        }
    }
}

static void test_config_rejects_a_name_the_table_does_not_have(void)
{
    cJSON *item = cJSON_CreateString("no_such_model");
    uint16_t id = 0x1234;
    CHECK(!parse_model_id(item, &id));
    CHECK(id == 0x1234);
    cJSON_Delete(item);
}

static void test_config_still_accepts_hex_and_decimal_ids(void)
{
    cJSON *hex = cJSON_CreateString("0x1307");
    uint16_t id = 0;
    CHECK(parse_model_id(hex, &id));
    CHECK(id == 0x1307);
    cJSON_Delete(hex);

    cJSON *dec = cJSON_CreateNumber(4352);
    CHECK(parse_model_id(dec, &id));
    CHECK(id == 0x1100);
    cJSON_Delete(dec);
}

/* ------------------------------------------------- config-declared devices */

static void test_config_sensor_device_is_registered(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0005\","
                        "\"type\":\"sensor_temp_humidity\",\"unicast\":5}]}") == ESP_OK);
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point("ble_mesh.bm_0005");
    CHECK(dev != NULL);
    CHECK(dev && (dev->caps & MINIHUB_BM_CAP_SENSOR) != 0);
    const hub_point_t *p = hub_point_find("ble_mesh.bm_0005");
    CHECK(p != NULL);
    if (!p) {
        return;
    }
    CHECK_STR(p->point.type, "sensor_temp_humidity");
    /* No report yet: the summary stays empty rather than inventing a reading. */
    CHECK_STR(p->point.value, "");
    const cJSON *t = hub_point_param("ble_mesh.bm_0005", "temperature");
    CHECK(t != NULL);
    const cJSON *h = hub_point_param("ble_mesh.bm_0005", "humidity");
    CHECK(h != NULL);
    const cJSON *params = cJSON_GetObjectItem(p->point.props, "params");
    const cJSON *tent = cJSON_GetObjectItem(params, "temperature");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(tent, "access")), "R");
}

static void test_config_sensor_model_declaration_is_registered(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0006\","
                        "\"type\":\"sensor_illuminance\",\"unicast\":6,"
                        "\"models\":[\"sensor\"]}]}") == ESP_OK);
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point("ble_mesh.bm_0006");
    CHECK(dev != NULL);
    CHECK(dev && dev->caps == MINIHUB_BM_CAP_SENSOR);
    CHECK(hub_point_find("ble_mesh.bm_0006") != NULL);
    CHECK(hub_point_param("ble_mesh.bm_0006", "illuminance") != NULL);
}

static void test_type_only_light_caps_cover_its_catalog_params(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0007\",\"type\":\"light\","
                        "\"unicast\":7}]}") == ESP_OK);
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point("ble_mesh.bm_0007");
    CHECK(dev != NULL);
    if (!dev) {
        return;
    }
    CHECK(dev->caps == (MINIHUB_BM_CAP_ONOFF | MINIHUB_BM_CAP_LIGHTNESS | MINIHUB_BM_CAP_CTL |
                        MINIHUB_BM_CAP_HSL));
    CHECK(hub_point_param("ble_mesh.bm_0007", "power") != NULL);
    CHECK(hub_point_param("ble_mesh.bm_0007", "brightness") != NULL);
    CHECK(hub_point_param("ble_mesh.bm_0007", "cct") != NULL);
    CHECK(hub_point_param("ble_mesh.bm_0007", "hue") != NULL);
    CHECK(hub_point_param("ble_mesh.bm_0007", "saturation") != NULL);
    /* The catalog lists intensity, but no mesh model drives it. */
    CHECK(hub_point_param("ble_mesh.bm_0007", "intensity") == NULL);
}

static void test_unknown_type_is_not_registered(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0008\",\"type\":\"no_such_type\","
                        "\"unicast\":8}]}") == ESP_OK);
    CHECK(minihub_ble_mesh_find_by_point("ble_mesh.bm_0008") == NULL);
    CHECK(hub_point_find("ble_mesh.bm_0008") == NULL);
}

/* ------------------------------------------------------- table-owned values */

static void test_set_brightness_default_comes_from_the_table(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_0001", "set_brightness", NULL) == ESP_OK);
    const hub_mesh_cmd_t *c = hub_last_mesh_cmd();
    CHECK(c != NULL);
    if (c) {
        CHECK(c->kind == HUB_MESH_LIGHTNESS);
        /* params[].init owns the brightness default (100), not the command. */
        CHECK(c->lightness == 65535);
    }
}

static void test_set_brightness_below_the_catalog_min_clamps_to_min(void)
{
    CHECK(config_light() != NULL);
    CHECK(command("ble_mesh.bm_0001", "set_brightness", "{\"brightness\":-5}") == ESP_OK);
    const hub_mesh_cmd_t *c = hub_last_mesh_cmd();
    /* The catalog's brightness min is 1; only the catalog clamps (D2). */
    CHECK(c && c->kind == HUB_MESH_LIGHTNESS && c->lightness == 655);
    CHECK_DBL(cJSON_GetNumberValue(hub_point_param("ble_mesh.bm_0001", "brightness")), 1.0);
}

static void test_written_zero_still_means_off(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0009\",\"type\":\"light\","
                        "\"unicast\":9,\"models\":[\"0x1000\",\"0x1303\"]}]}") == ESP_OK);
    CHECK(command("ble_mesh.bm_0009", "set", "{\"parameter\":\"brightness\",\"value\":0}") == ESP_OK);
    CHECK(command("ble_mesh.bm_0009", "set_color_temp", "{\"cct\":3000}") == ESP_OK);
    const hub_mesh_cmd_t *c = hub_last_mesh_cmd();
    CHECK(c != NULL);
    if (c) {
        CHECK(c->kind == HUB_MESH_CTL);
        CHECK(c->temperature == 3000);
        /* A dark light stays dark: the CTL brightness component is a percentage
         * scale, so 0 is legal even though the catalog's display min is 1. */
        CHECK(c->lightness == 0);
    }
}

static void test_ctl_brightness_default_comes_from_the_table(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_000a\",\"type\":\"light\","
                        "\"unicast\":10,\"models\":[\"0x1000\",\"0x1303\"]}]}") == ESP_OK);
    CHECK(command("ble_mesh.bm_000a", "set_color_temp", "{\"cct\":3000}") == ESP_OK);
    const hub_mesh_cmd_t *c = hub_last_mesh_cmd();
    CHECK(c && c->kind == HUB_MESH_CTL && c->lightness == 65535);
}

static void test_summary_default_comes_from_the_primary_param_init(void)
{
    CHECK_STR(default_value_for_type("light"), "off");
    CHECK_STR(default_value_for_type("switch"), "off");
    /* No report yet, and the table gives sensor params no init. */
    CHECK_STR(default_value_for_type("sensor_temp_humidity"), "");
    CHECK_STR(default_value_for_type("sensor_illuminance"), "");
    /* Types the catalog does not describe get no invented summary. */
    CHECK_STR(default_value_for_type("sensor"), "");
    CHECK_STR(default_value_for_type("no_such_type"), "");
    CHECK_STR(default_value_for_type(NULL), "");
}

static void test_onoff_needs_the_onoff_cap_not_the_type_name(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_000b\",\"type\":\"light\","
                        "\"unicast\":11,\"models\":[\"0x1300\"]}]}") == ESP_OK);
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point("ble_mesh.bm_000b");
    CHECK(dev && dev->caps == MINIHUB_BM_CAP_LIGHTNESS);
    /* The node has no OnOff model: sending one would be silently dropped. */
    CHECK(command("ble_mesh.bm_000b", "turn_on", NULL) == ESP_ERR_NOT_SUPPORTED);
    CHECK(hub_mesh_cmd_count == 0);
}

/* ------------------------------------------------------------- allocation */

static long s_json_balance;

static void *json_counted_malloc(size_t n)
{
    s_json_balance++;
    return malloc(n);
}

static void json_counted_free(void *p)
{
    s_json_balance--;
    free(p);
}

static void test_command_frees_every_json_it_allocates(void)
{
    CHECK(config_light() != NULL);
    s_json_balance = 0;
    CHECK(command("ble_mesh.bm_0001", "set_brightness", "{\"brightness\":40}") == ESP_OK);
    CHECK(s_json_balance == 0);
    s_json_balance = 0;
    CHECK(command("ble_mesh.bm_0001", "toggle", NULL) == ESP_OK);
    CHECK(s_json_balance == 0);

    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_000c\",\"type\":\"light\","
                        "\"unicast\":12,\"models\":[\"0x1000\",\"0x1303\"]}]}") == ESP_OK);
    s_json_balance = 0;
    CHECK(command("ble_mesh.bm_000c", "set_color_temp", "{\"cct\":3000}") == ESP_OK);
    CHECK(s_json_balance == 0);
}

static void test_sensor_report_fills_the_summary(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_000d\","
                        "\"type\":\"sensor_temp_humidity\",\"unicast\":13}]}") == ESP_OK);
    CHECK(hub_on_attr != NULL);
    minihub_ble_mesh_attr_evt_t evt = {0};
    evt.unicast = 13;
    evt.kind = MINIHUB_BM_ATTR_SENSOR;
    evt.u.sensor.property_id = 0x004F;
    evt.u.sensor.value = 21.5;
    hub_on_attr(&evt, hub_cb_ctx);
    const hub_point_t *p = hub_point_find("ble_mesh.bm_000d");
    CHECK(p != NULL);
    CHECK_DBL(cJSON_GetNumberValue(hub_point_param("ble_mesh.bm_000d", "temperature")), 21.5);
    /* temperature is devices.csv's primary_param for this type, so its report
     * is the overview-card summary. */
    CHECK_STR(p ? p->point.value : "", "21.5");
}

/* The catalog owns which param summarises a type; entries are sorted by name,
 * so position cannot stand in for it. */
static void test_catalog_exposes_the_primary_param(void)
{
    static const struct {
        const char *type;
        const char *primary;
    } cases[] = {
        {"light", "power"},
        {"switch", "power"},
        {"sensor_temp_humidity", "temperature"},
        {"sensor_illuminance", "illuminance"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const minihub_device_widget_device_t *d = minihub_device_widget_get(cases[i].type);
        CHECK(d != NULL);
        if (d) {
            CHECK_STR(d->primary_param, cases[i].primary);
        }
    }
}

/* ---------------------------------------------------- telemetry to quirks */

static bool pp_saw(uint16_t model_id, const char *field, double raw)
{
    for (size_t i = 0; i < hub_pp_apply_count; i++) {
        const hub_pp_apply_t *a = &hub_pp_applies[i];
        if (a->model_id == model_id && strcmp(a->field, field) == 0 && a->raw == raw) {
            return true;
        }
    }
    return false;
}

static void test_hsl_report_reaches_product_params(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_000e\",\"type\":\"light\","
                        "\"unicast\":14,\"uuid\":\"" LIGHT_UUID "\","
                        "\"models\":[\"0x1000\",\"0x1307\"]}]}") == ESP_OK);
    CHECK(hub_on_attr != NULL);
    minihub_ble_mesh_attr_evt_t evt = {0};
    evt.unicast = 14;
    evt.kind = MINIHUB_BM_ATTR_HSL;
    evt.u.hsl.hue = 21845;
    evt.u.hsl.saturation = 32768;
    evt.u.hsl.lightness = 65535;
    hub_on_attr(&evt, hub_cb_ctx);
    /* The node's HSL state is published in degrees / percent … */
    CHECK_DBL(cJSON_GetNumberValue(hub_point_param("ble_mesh.bm_000e", "hue")), 120.0);
    CHECK_DBL(cJSON_GetNumberValue(hub_point_param("ble_mesh.bm_000e", "saturation")), 50.0);
    CHECK_DBL(cJSON_GetNumberValue(hub_point_param("ble_mesh.bm_000e", "brightness")), 100.0);
    /* … and the raw mesh state reaches the per-product quirk layer. */
    CHECK(hub_pp_apply_count == 3);
    CHECK(pp_saw(0x1307, "hue", 21845.0));
    CHECK(pp_saw(0x1307, "saturation", 32768.0));
    CHECK(pp_saw(0x1307, "lightness", 65535.0));
}

static void test_sensor_report_reaches_product_params(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_000f\","
                        "\"type\":\"sensor_temp_humidity\",\"unicast\":15,"
                        "\"uuid\":\"" LIGHT_UUID "\"}]}") == ESP_OK);
    CHECK(hub_on_attr != NULL);
    minihub_ble_mesh_attr_evt_t evt = {0};
    evt.unicast = 15;
    evt.kind = MINIHUB_BM_ATTR_SENSOR;
    evt.u.sensor.property_id = 0x004F;
    evt.u.sensor.value = -0.5;
    hub_on_attr(&evt, hub_cb_ctx);
    CHECK(hub_pp_apply_count == 1);
    CHECK(pp_saw(0x1100, "temperature", -0.5));
}

static void test_unknown_sensor_property_reaches_nobody(void)
{
    CHECK(start_adapter("{\"devices\":[{\"point_id\":\"ble_mesh.bm_0010\","
                        "\"type\":\"sensor_temp_humidity\",\"unicast\":16,"
                        "\"uuid\":\"" LIGHT_UUID "\"}]}") == ESP_OK);
    minihub_ble_mesh_attr_evt_t evt = {0};
    evt.unicast = 16;
    evt.kind = MINIHUB_BM_ATTR_SENSOR;
    evt.u.sensor.property_id = 0x00FF;
    evt.u.sensor.value = 1.0;
    hub_on_attr(&evt, hub_cb_ctx);
    CHECK(hub_pp_apply_count == 0);
    /* The param is seeded at registration, but an unknown property fills nothing. */
    CHECK(!cJSON_IsNumber(hub_point_param("ble_mesh.bm_0010", "temperature")));
    const hub_point_t *p = hub_point_find("ble_mesh.bm_0010");
    CHECK_STR(p ? p->point.value : "x", "");
}

/* ------------------------------------------- the catalog's second producer */

/* The flat registry has two producers: the build-time generator and the
 * firmware flattener that runs on an uploaded model pair. Both have to carry
 * primary_param, or a remote catalog update silently drops what the adapter
 * reads. These tests stage a model pair under MINIHUB_DW_MODEL_DIR and flatten
 * it, so they run last: the output path is the catalog file itself. */

static bool copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

/* The real parameters.csv, plus either the real devices.csv (NULL) or text. */
static bool stage_model(const char *devices_csv)
{
    char src[512];
    char dst[512];
    snprintf(src, sizeof(src), "%s/parameters.csv", MINIHUB_DW_TEST_MODEL_SRC);
    snprintf(dst, sizeof(dst), "%s/parameters.csv", MINIHUB_DW_MODEL_DIR);
    if (!copy_file(src, dst)) {
        return false;
    }
    snprintf(dst, sizeof(dst), "%s/devices.csv", MINIHUB_DW_MODEL_DIR);
    if (!devices_csv) {
        snprintf(src, sizeof(src), "%s/devices.csv", MINIHUB_DW_TEST_MODEL_SRC);
        return copy_file(src, dst);
    }
    FILE *f = fopen(dst, "wb");
    if (!f) {
        return false;
    }
    bool ok = fputs(devices_csv, f) >= 0;
    return fclose(f) == 0 && ok;
}

static void test_flattened_model_keeps_the_primary_param(void)
{
    CHECK(stage_model(NULL));
    char err[192] = {0};
    CHECK(minihub_device_widget_flatten_stored_model(err, sizeof(err)) == ESP_OK);
    CHECK_STR(err, "");
    CHECK(minihub_device_widget_load(MINIHUB_DW_CSV_PATH) == ESP_OK);
    const minihub_device_widget_device_t *d = minihub_device_widget_get("light");
    CHECK(d != NULL);
    CHECK_STR(d ? d->primary_param : "", "power");
    d = minihub_device_widget_get("sensor_temp_humidity");
    CHECK(d != NULL);
    CHECK_STR(d ? d->primary_param : "", "temperature");
    CHECK(minihub_device_widget_load(NULL) == ESP_OK);
}

static void test_flatten_rejects_a_primary_param_the_type_does_not_list(void)
{
    CHECK(stage_model("device_id,parameters,primary_param,mandatory_params,image\n"
                      "light,\"power, brightness\",cct,power,\n"));
    char err[192] = {0};
    CHECK(minihub_device_widget_flatten_stored_model(err, sizeof(err)) != ESP_OK);
    CHECK(strstr(err, "primary_param") != NULL);
    CHECK(minihub_device_widget_load(NULL) == ESP_OK);
}

int main(void)
{
    /* Publish the compiled-in catalog without touching the filesystem. */
    if (minihub_device_widget_load(NULL) != ESP_OK) {
        fprintf(stderr, "cannot load the embedded device-widget catalog\n");
        return 2;
    }
    cJSON_Hooks hooks = {json_counted_malloc, json_counted_free};
    cJSON_InitHooks(&hooks);

    RUN(test_config_light_registers_one_point);
    RUN(test_turn_on_sends_onoff_and_echoes_the_summary);
    RUN(test_turn_on_updates_the_power_param);
    RUN(test_turn_off_updates_the_power_param);
    RUN(test_toggle_flips_the_power_param);
    RUN(test_brightness_param_writes_lightness);
    RUN(test_brightness_above_the_catalog_max_is_clamped);
    RUN(test_unknown_command_is_not_supported);
    RUN(test_unknown_point_is_not_found);
    RUN(test_published_model_alias_still_resolves);
    RUN(test_type_only_switch_gets_the_onoff_cap);
    RUN(test_set_color_temp_default_is_the_table_init);
    RUN(test_onoff_report_updates_the_point_and_product_params);
    RUN(test_config_accepts_every_published_model_name);
    RUN(test_config_rejects_a_name_the_table_does_not_have);
    RUN(test_config_still_accepts_hex_and_decimal_ids);
    RUN(test_config_sensor_device_is_registered);
    RUN(test_config_sensor_model_declaration_is_registered);
    RUN(test_type_only_light_caps_cover_its_catalog_params);
    RUN(test_unknown_type_is_not_registered);
    RUN(test_set_brightness_default_comes_from_the_table);
    RUN(test_set_brightness_below_the_catalog_min_clamps_to_min);
    RUN(test_written_zero_still_means_off);
    RUN(test_ctl_brightness_default_comes_from_the_table);
    RUN(test_summary_default_comes_from_the_primary_param_init);
    RUN(test_onoff_needs_the_onoff_cap_not_the_type_name);
    RUN(test_command_frees_every_json_it_allocates);
    RUN(test_sensor_report_fills_the_summary);
    RUN(test_catalog_exposes_the_primary_param);
    RUN(test_hsl_report_reaches_product_params);
    RUN(test_sensor_report_reaches_product_params);
    RUN(test_unknown_sensor_property_reaches_nobody);
    RUN(test_flattened_model_keeps_the_primary_param);
    RUN(test_flatten_rejects_a_primary_param_the_type_does_not_list);

    cJSON_Delete(s_cfg);
    hub_stub_reset();
    return test_summary();
}
