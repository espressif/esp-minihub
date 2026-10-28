/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Column ceiling shared by the CSV loader and the model validator, so a catalog that one
 * of them accepts cannot be silently truncated by the other. The flat registry itself only
 * maps COL_N of these by name; anything beyond that is ignored on purpose (a legacy
 * `protocol` or `visible` column stays loadable), but "ignored" has to mean the same number
 * of columns in both files. */
#define DW_CSV_MAX_COLS 24
