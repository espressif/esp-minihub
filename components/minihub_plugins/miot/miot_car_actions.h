/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <string.h>

typedef struct {
    const char *model_prefix;
    int ac_on_siid;
    int ac_on_aiid;
    int ac_off_siid;
    int ac_off_aiid;
} miot_car_ac_t;

/** Prefix match; siid/aiid of 0 means unsupported for that model/action. */
static inline bool miot_car_lookup_ac(const char *model, bool on, int *siid, int *aiid)
{
    if (!model || !siid || !aiid) {
        return false;
    }

    static const miot_car_ac_t k_table[] = {
        /* Fill from Spec/lab; zeros until verified */
        { "micar.car.ms11", 0, 0, 0, 0 },
    };

    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); i++) {
        const miot_car_ac_t *row = &k_table[i];
        if (strncmp(model, row->model_prefix, strlen(row->model_prefix)) != 0) {
            continue;
        }
        int s = on ? row->ac_on_siid : row->ac_off_siid;
        int a = on ? row->ac_on_aiid : row->ac_off_aiid;
        if (s <= 0 || a <= 0) {
            return false;
        }
        *siid = s;
        *aiid = a;
        return true;
    }
    return false;
}
