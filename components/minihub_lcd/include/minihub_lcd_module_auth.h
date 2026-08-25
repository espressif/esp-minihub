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

using AuthDoneFn = std::function<void()>;

void login_overlay_init();
void login_overlay_show_browser_hint();

void wire_overview_auth_actions(
    esp_brookesia::gui::Runtime *runtime,
    esp_brookesia::gui::DocumentId document_id,
    std::vector<esp_brookesia::gui::ScopedConnection> &connections,
    const std::shared_ptr<esp_brookesia::lib_utils::TaskScheduler> &scheduler,
    const AuthDoneFn &on_auth_done);

void schedule_sync();

}  // namespace module_auth_lcd
