/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"

/** Persist a display name. Does not update the live point. */
esp_err_t minihub_device_name_set(const char *point_id, const char *name);

/**
 * If a saved name exists, write it onto props as "name".
 * Creates the object when props is NULL and a name is stored.
 */
void minihub_device_name_overlay(const char *point_id, cJSON **props);
