/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "brookesia/gui_interface.hpp"
#include "brookesia/lib_utils.hpp"

namespace module_auth_lcd {

/**
 * @brief Called on the LVGL lock after an auth sync or logout RPC finishes.
 */
using AuthDoneFn = std::function<void()>;

/**
 * @brief Create the hidden login overlay once.
 *
 * A later call returns without building another overlay.
 */
void login_overlay_init();

/**
 * @brief Show the browser-login overlay.
 *
 * Builds the overlay first when it does not exist yet. The title is the
 * current auth adapter's `login_title`. The message names the STA address
 * when the station has an IPv4 address, and otherwise says the network is
 * not connected.
 */
void login_overlay_show_browser_hint();

/**
 * @brief Subscribe the overview document to the auth actions.
 *
 * Subscribes `auth_login`, `auth_sync`, `auth_sync_cta`, and `auth_logout`.
 * Login shows the browser hint. Sync and logout run the matching module RPC.
 * When `scheduler` is empty the action runs on the document callback.
 *
 * @param runtime GUI runtime that owns the document.
 * @param document_id Document that emits the auth actions.
 * @param connections Receives one scoped subscription per action.
 * @param scheduler Posts the sync and logout workers. Empty runs them inline.
 * @param on_auth_done Invoked after a sync or logout RPC. May be empty.
 */
void wire_overview_auth_actions(
    esp_brookesia::gui::Runtime *runtime,
    esp_brookesia::gui::DocumentId document_id,
    std::vector<esp_brookesia::gui::ScopedConnection> &connections,
    const std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> &scheduler,
    const AuthDoneFn &on_auth_done);

/**
 * @brief Queue a sync RPC for the current auth adapter.
 *
 * Does nothing when no auth adapter is selected. Without a scheduler stored
 * by `wire_overview_auth_actions()`, the sync runs on this call. Otherwise
 * it is posted after 3000 ms.
 */
void schedule_sync();

}  // namespace module_auth_lcd
