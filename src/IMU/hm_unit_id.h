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

#include <QString>

// ---------------------------------------------------------------------------
// The wG3 per-unit identifier — "<deviceId>#lowerArm" / "<deviceId>#palm"
// ---------------------------------------------------------------------------
//
// ⚠ THE SPELLING LIVES HERE AND NOWHERE ELSE, and that has always been the rule —
// this header only moves it somewhere everything that needs it can reach. The
// string is PERSISTED three times over: as the EventBuffer
// SourceDescriptor::identifier (Phase B), as the AppSettings::imuPlacement key
// (Phase C), and as `source.serial` in every exported swing.json. Two spellings
// would not fail; they would silently orphan a device's placement and its recorded
// lanes with it.
//
// It used to live on HmUnit (src/Gui/imu/hm_instance.h), which put it out of reach
// of everything below the GUI. The offline re-analyzer has to go the OTHER way —
// from a recorded `source.serial` back to which arm segment that lane measured —
// because a wG3 capture carries no per-user calibration to rebuild a binding from.
// HmUnit::unitIdFor() now delegates here, so there is still exactly one spelling.
//
// ⚠ DELIBERATELY FREE OF THE HACKMOTION SDK. The unit is an int, not an `hm_unit`,
// so this header is includable from the analysis layer, which does not link the
// vendor library. The values match the enum and are asserted against it at the one
// site that has both (hm_instance.cpp).

namespace pinpoint::hm_unit_id {

// Matching hackmotion/sample.h's hm_unit: HM_UNIT_LOWER_ARM = 0, HM_UNIT_PALM = 1.
inline constexpr int kLowerArm = 0;
inline constexpr int kPalm     = 1;
inline constexpr int kCount    = 2;

inline QString suffix(int unit)
{
    return unit == kPalm ? QStringLiteral("#palm") : QStringLiteral("#lowerArm");
}

inline QString unitIdFor(const QString &deviceId, int unit)
{
    return deviceId + suffix(unit);
}

// Split a unit id (or placement key) into device id + unit. False for a bare device
// id — a Witmotion, or a wG3 whose interim Phase A entry has not been migrated.
//
// ⚠ Regenerates and compares rather than matching the suffix text, so a suffix this
// build does not recognise stays UNRESOLVED instead of being guessed at.
inline bool parse(const QString &key, QString *deviceId, int *unit)
{
    const int sep = key.lastIndexOf(QLatin1Char('#'));
    if (sep <= 0) return false;
    const QString id = key.left(sep);
    for (int u = 0; u < kCount; ++u) {
        if (key == unitIdFor(id, u)) {
            if (deviceId) *deviceId = id;
            if (unit)     *unit     = u;
            return true;
        }
    }
    return false;
}

} // namespace pinpoint::hm_unit_id
