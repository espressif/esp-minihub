/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host tests for the BLE Mesh capability map and the sensor decoder.
 * Build and run with run_host_tests.py in this directory. */

#include "ble_mesh_map.h"
#include "ble_mesh_sensor.h"
#include "minihub_ble_mesh_types.h"
#include "test_framework.h"

/* ---------------------------------------------------------------- map table */

static void test_map_init_loads_embedded_table(void)
{
    CHECK(minihub_bm_map_init() == ESP_OK);
    CHECK(minihub_bm_model_count() == 6);
    CHECK(minihub_bm_control_count() == 5);
    CHECK(minihub_bm_map_init() == ESP_OK); /* idempotent */
}

/* The publication period has no rounding: a value with no exact encoding must be
 * refused, not silently written to a node as a nearby period. */
static void test_pub_period_encoding_is_exact(void)
{
    uint8_t o = 0xFF;
    CHECK(minihub_bm_encode_pub_period(0, &o) && o == 0x00);
    CHECK(minihub_bm_encode_pub_period(7, &o) && o == 0x47);
    CHECK(minihub_bm_encode_pub_period(30, &o) && o == 0x83);
    CHECK(minihub_bm_encode_pub_period(600, &o) && o == 0xC1);
    CHECK(minihub_bm_encode_pub_period(630, &o) && o == 0xBF);
    CHECK(!minihub_bm_encode_pub_period(64, &o));
    CHECK(!minihub_bm_encode_pub_period(640, &o));
    CHECK(!minihub_bm_encode_pub_period(0, NULL));
}

static void test_shipped_map_stays_get_only(void)
{
    /* Without a `report` block the Hub writes no publication to any node. */
    const minihub_bm_report_t *r = minihub_bm_report();
    CHECK(r != NULL);
    CHECK(!r->enabled);
    CHECK(r->group == 0);
}

/* The harness points MINIHUB_BM_MAP_DATA_PATH at its own build tree. */
static bool write_report_overlay(const char *json)
{
    FILE *f = fopen(MINIHUB_BM_MAP_DATA_PATH, "w");
    if (!f) {
        return false;
    }
    fputs(json, f);
    fclose(f);
    return true;
}

/* A node drops a Publication Set whose TTL is neither 0x00-0x7F nor 0xFF, and it
 * drops it without a Status — so the map has to refuse that TTL up front. */
static void test_report_block_rejects_what_a_node_would_drop(void)
{
    CHECK(write_report_overlay("{\"report\":{\"group\":\"0xC001\",\"period_s\":30,\"ttl\":200}}"));
    CHECK(minihub_bm_map_reload() == ESP_OK);
    CHECK(!minihub_bm_report()->enabled);

    CHECK(write_report_overlay("{\"report\":{\"group\":\"0xC001\",\"period_s\":30,\"ttl\":127}}"));
    CHECK(minihub_bm_map_reload() == ESP_OK);
    CHECK(minihub_bm_report()->enabled);
    CHECK(minihub_bm_report()->ttl == 127);
    CHECK(minihub_bm_report()->period_octet == 0x83);

    CHECK(write_report_overlay("{\"report\":{\"group\":\"0xC001\",\"ttl\":255}}"));
    CHECK(minihub_bm_map_reload() == ESP_OK);
    CHECK(minihub_bm_report()->enabled);
    CHECK(minihub_bm_report()->ttl == 255);

    CHECK(write_report_overlay("{\"report\":{\"group\":\"0xC001\",\"period_s\":64}}"));
    CHECK(minihub_bm_map_reload() == ESP_OK);
    CHECK(!minihub_bm_report()->enabled);

    CHECK(write_report_overlay("{\"report\":{\"group\":\"0xC001\",\"retransmit_count\":8}}"));
    CHECK(minihub_bm_map_reload() == ESP_OK);
    CHECK(!minihub_bm_report()->enabled);

    CHECK(write_report_overlay("{\"report\":{\"period_s\":30}}"));
    CHECK(minihub_bm_map_reload() == ESP_OK);
    CHECK(!minihub_bm_report()->enabled);

    remove(MINIHUB_BM_MAP_DATA_PATH);
    CHECK(minihub_bm_map_reload() == ESP_OK);
    CHECK(!minihub_bm_report()->enabled);
}

static void test_model_cap_ctl_also_implies_lightness(void)
{
    uint16_t caps = 0;
    uint16_t client = 0;
    CHECK(minihub_bm_model_cap(0x1303, &caps, &client));
    CHECK(caps == (MINIHUB_BM_CAP_CTL | MINIHUB_BM_CAP_LIGHTNESS));
    CHECK(client == 0x1305);
}

static void test_model_cap_unknown_model_returns_false(void)
{
    uint16_t caps = 0xFFFF;
    uint16_t client = 0xFFFF;
    CHECK(!minihub_bm_model_cap(0xFF01, &caps, &client));
}

static void test_classify_light_by_lightness_cap(void)
{
    CHECK_STR(minihub_bm_classify(MINIHUB_BM_CAP_LIGHTNESS, NULL, 0), "light");
}

static void test_classify_switch_by_onoff_cap(void)
{
    CHECK_STR(minihub_bm_classify(MINIHUB_BM_CAP_ONOFF, NULL, 0), "switch");
}

static void test_classify_unknown_node_returns_null(void)
{
    CHECK(minihub_bm_classify(0, NULL, 0) == NULL);
}

static void test_classify_temp_humidity_requires_humidity_property(void)
{
    const uint16_t temp_only[] = { 0x004F };
    CHECK(minihub_bm_classify(MINIHUB_BM_CAP_SENSOR, temp_only, 1) == NULL);
    const uint16_t both[] = { 0x004F, 0x0076 };
    CHECK_STR(minihub_bm_classify(MINIHUB_BM_CAP_SENSOR, both, 2), "sensor_temp_humidity");
}

static void test_classify_priority_prefers_temp_humidity(void)
{
    const uint16_t props[] = { 0x0076, 0x004F, 0x0055 };
    CHECK_STR(minihub_bm_classify(MINIHUB_BM_CAP_SENSOR, props, 3), "sensor_temp_humidity");
}

static void test_classify_rows_are_readable(void)
{
    size_t n = minihub_bm_classify_count();
    CHECK(n == 7);
    CHECK(minihub_bm_classify_at(n) == NULL);
    bool saw_sensor = false, saw_light = false;
    for (size_t i = 0; i < n; i++) {
        const minihub_bm_map_classify_t *r = minihub_bm_classify_at(i);
        CHECK(r != NULL);
        if (!r) {
            continue;
        }
        if (strcmp(r->type, "sensor_temp_humidity") == 0) {
            saw_sensor = true;
            CHECK(r->all_prop_count == 1);
            CHECK(r->all_props[0] == 0x0076);
            CHECK(r->any_prop_count == 2);
            CHECK(r->any_caps_mask == 0);
        }
        if (strcmp(r->type, "light") == 0) {
            saw_light = true;
            CHECK(r->all_prop_count == 0);
            CHECK(r->any_prop_count == 0);
            CHECK(r->any_caps_mask == (MINIHUB_BM_CAP_LIGHTNESS | MINIHUB_BM_CAP_CTL |
                                       MINIHUB_BM_CAP_HSL | MINIHUB_BM_CAP_LEVEL));
        }
    }
    CHECK(saw_sensor);
    CHECK(saw_light);
}

static void test_params_for_caps_dedupes_brightness(void)
{
    const char *names[MINIHUB_BM_MAP_MAX_PARAMS];
    size_t n = minihub_bm_params_for(MINIHUB_BM_CAP_ONOFF | MINIHUB_BM_CAP_LIGHTNESS, NULL, 0,
                                     names, MINIHUB_BM_MAP_MAX_PARAMS);
    CHECK(n == 2);
    if (n == 2) {
        CHECK_STR(names[0], "power");
        CHECK_STR(names[1], "brightness");
    }
}

static void test_params_for_sensor_property(void)
{
    const uint16_t props[] = { 0x0076 };
    const char *names[MINIHUB_BM_MAP_MAX_PARAMS];
    size_t n = minihub_bm_params_for(MINIHUB_BM_CAP_SENSOR, props, 1, names,
                                     MINIHUB_BM_MAP_MAX_PARAMS);
    CHECK(n == 1);
    if (n == 1) {
        CHECK_STR(names[0], "humidity");
    }
}

static void test_param_init_comes_from_table(void)
{
    const minihub_bm_map_param_t *cct = minihub_bm_param_by_name("cct");
    CHECK(cct != NULL);
    if (cct) {
        CHECK_STR(cct->init, "4000");
        CHECK(cct->init_is_number);
    }
    const minihub_bm_map_param_t *power = minihub_bm_param_by_name("power");
    CHECK(power != NULL);
    if (power) {
        CHECK_STR(power->init, "off");
        CHECK(!power->init_is_number);
    }
    const minihub_bm_map_param_t *hum = minihub_bm_param_by_name("humidity");
    CHECK(hum != NULL);
    if (hum) {
        CHECK_STR(hum->access, "R");
    }
    CHECK(minihub_bm_param_by_name("no_such_param") == NULL);
}

static void test_control_brightness_falls_back_to_level(void)
{
    const minihub_bm_map_control_t *c = minihub_bm_control_for_param("brightness");
    CHECK(c != NULL);
    if (c) {
        CHECK(c->kind == MINIHUB_BM_CTRL_LIGHTNESS);
        CHECK(c->model_id == 0x1300);
        CHECK(c->fallback == MINIHUB_BM_CTRL_LEVEL);
        CHECK_STR(c->field, "lightness");
    }
    CHECK(minihub_bm_control_for_param("humidity") == NULL);
}

static void test_model_name_lookup_canonical(void)
{
    uint16_t id = 0;
    CHECK(minihub_bm_model_id_by_name("onoff", &id));
    CHECK(id == 0x1000);
    CHECK(minihub_bm_model_id_by_name("lightness", &id));
    CHECK(id == 0x1300);
    CHECK(minihub_bm_model_id_by_name("sensor", &id));
    CHECK(id == 0x1100);
}

static void test_model_name_lookup_published_aliases(void)
{
    uint16_t id = 0;
    CHECK(minihub_bm_model_id_by_name("gen_onoff_srv", &id));
    CHECK(id == 0x1000);
    CHECK(minihub_bm_model_id_by_name("gen_level_srv", &id));
    CHECK(id == 0x1002);
    CHECK(minihub_bm_model_id_by_name("light_lightness_srv", &id));
    CHECK(id == 0x1300);
    CHECK(minihub_bm_model_id_by_name("light_ctl_srv", &id));
    CHECK(id == 0x1303);
    CHECK(minihub_bm_model_id_by_name("light_hsl_srv", &id));
    CHECK(id == 0x1307);
}

static void test_model_name_lookup_rejects_unknown(void)
{
    uint16_t id = 0x1234;
    CHECK(!minihub_bm_model_id_by_name("no_such_model", &id));
    CHECK(!minihub_bm_model_id_by_name("", &id));
    CHECK(!minihub_bm_model_id_by_name(NULL, &id));
    CHECK(id == 0x1234); /* untouched */
}

static void test_model_rows_carry_their_name(void)
{
    const minihub_bm_map_model_t *m = minihub_bm_model_at(0);
    CHECK(m != NULL);
    if (m) {
        CHECK_STR(m->name, "onoff");
        CHECK_STR(m->alias, "gen_onoff_srv");
    }
}

static void test_control_models_declare_their_get(void)
{
    static const struct {
        uint16_t id;
        uint16_t get;
    } rows[] = {
        { 0x1000, 0x8201 }, /* Gen OnOff */
        { 0x1002, 0x8205 }, /* Gen Level */
        { 0x1300, 0x824B }, /* Light Lightness */
        { 0x1303, 0x825D }, /* Light CTL */
        { 0x1307, 0x826D }, /* Light HSL */
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const minihub_bm_map_model_t *m = minihub_bm_model_by_id(rows[i].id);
        CHECK(m != NULL);
        if (m) {
            CHECK(m->get_opcode == rows[i].get);
        }
    }
    /* A sensor reads back through Descriptor + Value, not one Get. */
    const minihub_bm_map_model_t *sensor = minihub_bm_model_by_id(0x1100);
    CHECK(sensor != NULL);
    if (sensor) {
        CHECK(sensor->get_opcode == 0);
    }
    CHECK(minihub_bm_model_by_id(0x9999) == NULL);
}

static void test_sensor_row_temperature8_matches_sig(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x004F);
    CHECK(s != NULL);
    if (s) {
        CHECK(s->size == 1);
        CHECK(s->is_signed);
        CHECK_DBL(s->scale, 0.5);
        CHECK(s->has_unknown);
        CHECK(s->unknown == 0x7F);
        CHECK(s->has_min);
        CHECK_DBL(s->min, -40.0);
        CHECK(s->has_max);
        CHECK_DBL(s->max, 125.0);
    }
    CHECK(minihub_bm_sensor_by_property(0x0001) == NULL);
}

/* ------------------------------------------------------------- sensor decode */

static void test_sensor_value_temperature8_negative(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x004F);
    const uint8_t raw[] = { 0xFF };
    double v = 0;
    CHECK(minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
    CHECK_DBL(v, -0.5);
}

static void test_sensor_value_temperature8_unknown_sentinel(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x004F);
    const uint8_t raw[] = { 0x7F };
    double v = 1234;
    CHECK(!minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
    CHECK_DBL(v, 1234); /* untouched */
}

static void test_sensor_value_temperature16_signed(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x0075);
    const uint8_t raw[] = { 0x9C, 0xFF }; /* -100 */
    double v = 0;
    CHECK(minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
    CHECK_DBL(v, -1.0);
}

static void test_sensor_value_humidity(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x0076);
    const uint8_t raw[] = { 0x88, 0x13 }; /* 5000 */
    double v = 0;
    CHECK(minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
    CHECK_DBL(v, 50.0);
}

static void test_sensor_value_humidity_unknown_sentinel(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x0076);
    const uint8_t raw[] = { 0xFF, 0xFF };
    double v = 0;
    CHECK(!minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
}

static void test_sensor_value_illuminance_three_octets(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x0055);
    const uint8_t raw[] = { 0x10, 0x27, 0x00 }; /* 10000 */
    double v = 0;
    CHECK(minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
    CHECK_DBL(v, 100.0);
}

static void test_sensor_value_pressure_four_octets(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x00A9);
    const uint8_t raw[] = { 0x40, 0x42, 0x0F, 0x00 }; /* 1000000 Pa */
    double v = 0;
    CHECK(minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
    CHECK_DBL(v, 1000.0);
}

static void test_sensor_value_motion_bool_threshold(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x0042);
    double v = 0;
    const uint8_t set[] = { 0x02 };
    CHECK(minihub_bm_sensor_value(s, set, sizeof(set), &v));
    CHECK_DBL(v, 1.0);
    const uint8_t clear[] = { 0x00 };
    CHECK(minihub_bm_sensor_value(s, clear, sizeof(clear), &v));
    CHECK_DBL(v, 0.0);
}

static void test_sensor_value_clamps_to_catalog_bounds(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x0077);
    const uint8_t raw[] = { 0x89, 0x13 }; /* 5001 ppm, above the 5000 catalog max */
    double v = 0;
    CHECK(minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
    CHECK_DBL(v, 5000.0);
}

static void test_sensor_value_rejects_short_buffer(void)
{
    const minihub_bm_sensor_t *s = minihub_bm_sensor_by_property(0x0075);
    const uint8_t raw[] = { 0x9C };
    double v = 0;
    CHECK(!minihub_bm_sensor_value(s, raw, sizeof(raw), &v));
}

/* --------------------------------------------------------------- MPID parser */

static void test_mpid_format_a_uses_table_size(void)
{
    /* Temperature 8 (0x004F), Length field 1 = the raw length IDF writes. */
    const uint8_t buf[] = { 0xE2, 0x09, 0xFF };
    size_t off = 0;
    minihub_bm_mpid_t m = {0};
    CHECK(minihub_bm_mpid_next(buf, sizeof(buf), &off, &m));
    CHECK(m.property_id == 0x004F);
    CHECK(m.len == 1);
    CHECK(m.format == 0);
    CHECK(m.length_field == 1);
    CHECK(off == 3);
    CHECK(m.data == buf + 2);
}

static void test_mpid_format_a_accepts_one_based_length(void)
{
    /* Same property, Length field 0 = the 1-based length Mesh Model 1.1 gives. */
    const uint8_t buf[] = { 0xE0, 0x09, 0xFF };
    size_t off = 0;
    minihub_bm_mpid_t m = {0};
    CHECK(minihub_bm_mpid_next(buf, sizeof(buf), &off, &m));
    CHECK(m.property_id == 0x004F);
    CHECK(m.len == 1);
    CHECK(off == 3);
}

static void test_mpid_format_b(void)
{
    /* Present Indoor Humidity (0x0076), Length field 2 = raw length. */
    const uint8_t buf[] = { 0x05, 0x76, 0x00, 0x88, 0x13 };
    size_t off = 0;
    minihub_bm_mpid_t m = {0};
    CHECK(minihub_bm_mpid_next(buf, sizeof(buf), &off, &m));
    CHECK(m.property_id == 0x0076);
    CHECK(m.len == 2);
    CHECK(m.format == 1);
    CHECK(off == 5);
}

static void test_mpid_unknown_property_skipped_by_length_field(void)
{
    /* 0x0001 is not in sensors[]: Length field 2 → 3 octets (1-based). */
    const uint8_t buf[] = { 0x05, 0x01, 0x00, 0xAA, 0xBB, 0xCC };
    size_t off = 0;
    minihub_bm_mpid_t m = {0};
    CHECK(minihub_bm_mpid_next(buf, sizeof(buf), &off, &m));
    CHECK(m.property_id == 0x0001);
    CHECK(m.len == 3);
    CHECK(off == 6);
}

static void test_mpid_rejects_truncated_value(void)
{
    const uint8_t buf[] = { 0xE2, 0x09 };
    size_t off = 0;
    minihub_bm_mpid_t m = {0};
    CHECK(!minihub_bm_mpid_next(buf, sizeof(buf), &off, &m));
}

static void test_mpid_walks_two_records(void)
{
    const uint8_t buf[] = { 0xE2, 0x09, 0xFF, 0x05, 0x76, 0x00, 0x88, 0x13 };
    size_t off = 0;
    minihub_bm_mpid_t m = {0};
    CHECK(minihub_bm_mpid_next(buf, sizeof(buf), &off, &m));
    CHECK(m.property_id == 0x004F);
    CHECK(minihub_bm_mpid_next(buf, sizeof(buf), &off, &m));
    CHECK(m.property_id == 0x0076);
    CHECK(off == sizeof(buf));
    CHECK(!minihub_bm_mpid_next(buf, sizeof(buf), &off, &m));
}

static void test_descriptor_props_reads_eight_octet_records(void)
{
    const uint8_t buf[] = {
        0x4F, 0x00, 0, 0, 0, 0, 0, 0,
        0x76, 0x00, 0, 0, 0, 0, 0, 0,
    };
    uint16_t ids[4] = {0};
    CHECK(minihub_bm_descriptor_props(buf, sizeof(buf), ids, 4) == 2);
    CHECK(ids[0] == 0x004F);
    CHECK(ids[1] == 0x0076);
}

/* ------------------------------------------------------------ id / uuid glue */

static void test_format_point_id(void)
{
    char id[MINIHUB_POINT_ID_MAX];
    CHECK(minihub_bm_format_point_id(id, sizeof(id), 0x0042) == 0);
    CHECK_STR(id, "ble_mesh.bm_0042");
}

static void test_normalize_legacy_slash_id(void)
{
    char id[MINIHUB_POINT_ID_MAX] = "light/bm_0042";
    CHECK(minihub_bm_normalize_point_id(id, sizeof(id)));
    CHECK_STR(id, "ble_mesh.bm_0042");
    CHECK(!minihub_bm_normalize_point_id(id, sizeof(id)));
}

static void test_stable_suffix(void)
{
    CHECK_STR(minihub_bm_stable_suffix("ble_mesh.bm_0042"), "bm_0042");
    CHECK_STR(minihub_bm_stable_suffix("light/bm_0042"), "bm_0042");
    CHECK_STR(minihub_bm_stable_suffix("bm_0042"), "bm_0042");
    CHECK(minihub_bm_stable_suffix("ble_mesh.other") == NULL);
}

static void test_uuid_hex_round_trip(void)
{
    const uint8_t uuid[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    char hex[MINIHUB_BM_UUID_HEX_LEN];
    CHECK(minihub_bm_uuid_to_hex(uuid, hex, sizeof(hex)) == 0);
    CHECK_STR(hex, "00112233445566778899aabbccddeeff");
    uint8_t back[16] = {0};
    CHECK(minihub_bm_hex_to_uuid(hex, back) == 0);
    CHECK(memcmp(uuid, back, sizeof(uuid)) == 0);
    CHECK(minihub_bm_hex_to_uuid("0011", back) == -1);
}

int main(void)
{
    RUN(test_map_init_loads_embedded_table);
    RUN(test_pub_period_encoding_is_exact);
    RUN(test_shipped_map_stays_get_only);
    RUN(test_report_block_rejects_what_a_node_would_drop);
    RUN(test_model_cap_ctl_also_implies_lightness);
    RUN(test_model_cap_unknown_model_returns_false);
    RUN(test_classify_light_by_lightness_cap);
    RUN(test_classify_switch_by_onoff_cap);
    RUN(test_classify_unknown_node_returns_null);
    RUN(test_classify_temp_humidity_requires_humidity_property);
    RUN(test_classify_priority_prefers_temp_humidity);
    RUN(test_classify_rows_are_readable);
    RUN(test_params_for_caps_dedupes_brightness);
    RUN(test_params_for_sensor_property);
    RUN(test_param_init_comes_from_table);
    RUN(test_control_brightness_falls_back_to_level);
    RUN(test_model_name_lookup_canonical);
    RUN(test_model_name_lookup_published_aliases);
    RUN(test_model_name_lookup_rejects_unknown);
    RUN(test_model_rows_carry_their_name);
    RUN(test_control_models_declare_their_get);
    RUN(test_sensor_row_temperature8_matches_sig);
    RUN(test_sensor_value_temperature8_negative);
    RUN(test_sensor_value_temperature8_unknown_sentinel);
    RUN(test_sensor_value_temperature16_signed);
    RUN(test_sensor_value_humidity);
    RUN(test_sensor_value_humidity_unknown_sentinel);
    RUN(test_sensor_value_illuminance_three_octets);
    RUN(test_sensor_value_pressure_four_octets);
    RUN(test_sensor_value_motion_bool_threshold);
    RUN(test_sensor_value_clamps_to_catalog_bounds);
    RUN(test_sensor_value_rejects_short_buffer);
    RUN(test_mpid_format_a_uses_table_size);
    RUN(test_mpid_format_a_accepts_one_based_length);
    RUN(test_mpid_format_b);
    RUN(test_mpid_unknown_property_skipped_by_length_field);
    RUN(test_mpid_rejects_truncated_value);
    RUN(test_mpid_walks_two_records);
    RUN(test_descriptor_props_reads_eight_octet_records);
    RUN(test_format_point_id);
    RUN(test_normalize_legacy_slash_id);
    RUN(test_stable_suffix);
    RUN(test_uuid_hex_round_trip);

    return test_summary();
}
