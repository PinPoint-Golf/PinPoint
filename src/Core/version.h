/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

// ── Release version ───────────────────────────────────────────────────────────
// Bump MAJOR/MINOR for releases; set POSTFIX to "" for clean releases,
// e.g. "-beta1", "-alpha1", "-rc2" for pre-releases.
#define PINPOINT_VERSION_MAJOR   0
#define PINPOINT_VERSION_MINOR   1
#define PINPOINT_VERSION_POSTFIX "-alpha10"

// ── Monotonic build number ───────────────────────────────────────────────────
// A single strictly-increasing integer used as the *comparison* key by the
// Windows updater (WinSparkle's `sparkle:version` / win_sparkle_set_app_build_version;
// see docs/design/windows_update.md §7). The human display version above stays the
// source of truth for what the user sees; this is purely for ordering, because
// string-comparing "-alpha2" vs a clean release is unreliable.
//
// MUST increase on every release. Recommended formula (keep in lockstep by hand):
//   MAJOR*1_000_000 + MINOR*10_000 + PATCH*100 + prerelease
// where prerelease ∈ [0,98] encodes alpha(0+N) < beta(25+N) < rc(50+N) < release(99).
// v0.1-alpha5 → 0 + 10000 + 0 + 5 = 10005.
// v0.1-alpha6 → 0 + 10000 + 0 + 6 = 10006.
// v0.1-alpha7 → 0 + 10000 + 0 + 7 = 10007.
// v0.1-alpha8 → 0 + 10000 + 0 + 8 = 10008.
// v0.1-alpha9 → 0 + 10000 + 0 + 9 = 10009.
// v0.1-alpha10 → 0 + 10000 + 0 + 10 = 10010.
#define PINPOINT_VERSION_BUILD   10010

// ── Assembled string ─────────────────────────────────────────────────────────
#define PP_STR_IMPL(x) #x
#define PP_STR(x)      PP_STR_IMPL(x)

#define PINPOINT_VERSION_STRING \
    "v" PP_STR(PINPOINT_VERSION_MAJOR) "." PP_STR(PINPOINT_VERSION_MINOR) PINPOINT_VERSION_POSTFIX

// Build number as a string literal (for win_sparkle_set_app_build_version()).
#define PINPOINT_VERSION_BUILD_STRING PP_STR(PINPOINT_VERSION_BUILD)
