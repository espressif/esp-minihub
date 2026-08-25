/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MIOT_SPEC_URN_MAX     192
#define MIOT_SPEC_DEVICE_MAX  32
#define MIOT_SPEC_MODEL_MAX   64
#define MIOT_SPEC_EXTRAS_MAX  12

/** Optional HA-like control harvested from Spec (capped). */
typedef struct {
    char key[24];      /* indicator, motion_detection, tts, volume, ... */
    char kind[12];     /* switch, number, select, action */
    int siid;
    int piid;          /* 0 if action */
    int aiid;          /* 0 if property */
} miot_spec_extra_t;

/** Slim Spec profile cached under /data/config/miot_spec/models/<model>.json */
typedef struct {
    char model[MIOT_SPEC_MODEL_MAX];
    char urn[MIOT_SPEC_URN_MAX];
    char device[MIOT_SPEC_DEVICE_MAX]; /* Spec device type: light, camera, … */
    int siid;
    int piid_on;
    int piid_bright;
    int bright_max;          /* Spec value-range max; 100 or 65535 */
    int piid_ct;
    int piid_color;
    bool supports_brightness;
    bool supports_color_temp;
    bool supports_color;
    int siid_fan;
    int piid_fan;
    int siid_env;
    int piid_temp;           /* ambient */
    int piid_mode;
    int piid_target_temp;
    int piid_position;       /* current-position */
    int piid_target_position;
    int siid_motor;
    int piid_motor;
    uint8_t motor_open;
    uint8_t motor_close;
    uint8_t motor_pause;
    /** Primary "on" is sleep-mode (true=asleep/off); invert for Hub on/off. */
    bool invert_on;
    int piid_volume;         /* speaker volume when no power on */
    int piid_door;           /* lock door-state when no power on */
    int piid_illum;          /* magnet / motion illumination */
    int piid_no_motion;      /* motion: no-motion-duration */
    int siid_battery;
    int piid_battery;        /* battery-level */
    int siid_stream;         /* p2p-stream service */
    int piid_video_codec;
    int piid_audio_codec;
    int siid_power;          /* power-consumption service */
    int piid_energy;         /* kWh */
    int piid_current;        /* A */
    int piid_power;          /* W */
    int piid_voltage;        /* V */
    int piid_fault;
    miot_spec_extra_t extras[MIOT_SPEC_EXTRAS_MAX];
    size_t extras_n;
    int profile_rev;
    bool valid;
} miot_spec_profile_t;

/**
 * Ensure Spec slim profiles exist for each model (stream instances index if needed,
 * fetch instance JSON, write local cache). Safe to call from miot HTTPS worker.
 *
 * models: array of model strings; n_models count. NULL entries skipped.
 */
esp_err_t miot_spec_ensure_models(const char *const *models, size_t n_models);

/** Load slim profile from local cache (no network). ESP_ERR_NOT_FOUND if missing. */
esp_err_t miot_spec_load_profile(const char *model, miot_spec_profile_t *out);

/**
 * Map Spec device type → MiniHub point type (≤15 chars).
 * e.g. intelligent-speaker → speaker; curtain → cover.
 */
const char *miot_spec_hub_type(const char *spec_device);

/** True if Spec device should never use miIO LAN. */
bool miot_spec_device_cloud_only(const char *spec_device);
