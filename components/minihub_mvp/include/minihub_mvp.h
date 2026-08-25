/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/** Ensures MVP MiniHub components stay linked when main does not call Core yet. */
void minihub_mvp_link_anchor(void);
