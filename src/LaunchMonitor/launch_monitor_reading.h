/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include <QMetaType>
#include <QString>

#include <optional>
#include <vector>

namespace pinpoint::lm {

// One shot as a launch monitor reported it, already converted into the metric
// catalogue's units. Every field is optional because a device need not report
// every quantity, and a connector must never invent one it did not receive.
//
// WHY EVERY KEY IS `lm.`-PREFIXED. The monitor's first purpose here is to be a
// REFERENCE INSTRUMENT we check our own optical estimates against, not a better
// producer of them. The route ladder resolves exactly one winner per metric key,
// so a device rung placed above the camera rung on `clubheadSpeed` would not sit
// beside our estimate — it would replace it, and the comparison that justifies
// owning the device at all would be gone. So the measured value is a metric of
// its own (`lm.clubheadSpeed`) and the estimate keeps its bare key untouched.
// The prefix is applied uniformly, including to quantities we will never
// estimate (`lm.faceAngle`), so a reader never has to remember which is which.
struct LaunchMonitorReading {
    // ── Provenance ──────────────────────────────────────────────────────────
    // The device's own shot counter. NOT a PinPoint shot id and never treated as
    // one — it is how we tell a fresh row from the same row restated, nothing more.
    QString deviceShotId;
    // The club the monitor believed was in use ("Irn", "Drv", …). Recorded only.
    // PinPoint's own club selection is what resolves a normative corridor: these
    // codes are coarser than our per-club contexts, so preferring them would make
    // grading less specific rather than more.
    QString deviceClub;
    // Which connector produced this — the same token AppSettings stores, set by the
    // connector itself. Recorded rather than assumed at the point of writing, so a
    // second connector does not silently label its readings as the first one's.
    QString deviceKind;
    QString sourcePath;         // the file this came from
    qint64  readAtMs = 0;       // wall clock when we read it (epoch ms)

    // ── Measured, where we also estimate ────────────────────────────────────
    // Each of these has a bare-keyed counterpart produced by our own pipeline
    // (live for the first two, planned optical for the rest). The pair IS the
    // validation surface — see the note above.
    std::optional<double> clubheadSpeed;    // mph  ← bare `clubheadSpeed`, camera
    std::optional<double> attackAngle;      // °    ← bare `attackAngle`, camera
    std::optional<double> ballSpeed;        // mph  ← bare `ballSpeed`, planned
    std::optional<double> launchAngle;      // °    ← bare `launchAngle`, planned
    std::optional<double> launchDirection;  // °    ← bare `launchDirection`, planned
    std::optional<double> clubPath;         // °    ← bare `clubPath`, planned

    // ── Measured, and ours alone will never be ──────────────────────────────
    std::optional<double> faceAngle;        // °
    std::optional<double> faceToPath;       // °
    std::optional<double> dynamicLoft;      // °
    std::optional<double> spinLoft;         // °   (derived: dynamicLoft − attackAngle)
    std::optional<double> lieAngle;         // °
    std::optional<double> closureRate;      // °/s
    std::optional<double> spinRate;         // rpm (the device's total spin)
    std::optional<double> backSpin;         // rpm
    std::optional<double> sideSpin;         // rpm
    std::optional<double> spinAxis;         // °   (derived: atan2(sideSpin, backSpin))
    std::optional<double> smashFactor;      // ratio (derived: ballSpeed / clubheadSpeed)
    std::optional<double> strikeLocation;   // mm  heel(−) ← → toe(+)
    std::optional<double> strikeHeight;     // mm  low(−)  ← → high(+)

    // ── Flight-model outputs ────────────────────────────────────────────────
    std::optional<double> carryDistance;    // yd
    std::optional<double> totalDistance;    // yd
    std::optional<double> offline;          // yd  left(−) ← → right(+)
    std::optional<double> peakHeight;       // ft
    std::optional<double> descentAngle;     // °
    std::optional<double> distanceToPin;    // yd

    // True once any measured field is present. A row that parsed but carried no
    // numbers is not a reading and must not be attributed to a shot.
    bool hasAnyValue() const;
};

// The catalogue identity of one reading field. This table is the SINGLE place the
// 25 launch-monitor metrics are enumerated: the swing.json writer, the MetricSeries
// emitter and the manifest cross-check test all walk it, so a field added to the
// struct without a descriptor (or vice versa) fails a test rather than going quietly
// missing from a shot.
struct FieldDef {
    const char *key;        // "lm.clubheadSpeed" — matches MetricDescriptor::key exactly
    const char *rawName;    // key within the swing.json `launchMonitor` block
    const char *label;      // matches MetricDescriptor::label exactly
    const char *unit;       // matches MetricDescriptor::unit exactly
    // Which band of the session board this field sits in — one of fieldGroups(),
    // cross-checked. NOT MetricDescriptor::group: that taxonomy spans all 86 metrics
    // and puts thirteen of these in one "Ball flight" bucket that the board splits
    // three ways, so it cannot be mapped without hand-writing this column anyway.
    const char *group;
    // The board abbreviation ("CLUB SPEED", "DYN. LOFT"). Deliberately NOT
    // MetricDescriptor::shortLabel: that carries the "(LM)" qualifier which exists to
    // disambiguate against our own estimate, and a panel showing only measured values
    // has nothing to disambiguate against. The moment a bare-key estimate appears
    // beside one of these, the qualifier has to come back.
    const char *abbrev;
    std::optional<double> LaunchMonitorReading::*member;
};

// The board's bands, in DISPLAY order — which is not derivable from fieldDefs()
// (first appearance there gives Club, Launch, Strike, …). Every FieldDef::group must
// name one of these and every band must own at least one field; both are asserted by
// the catalogue cross-check, so a typo or an orphaned band fails a test rather than
// producing a silently empty row on the panel.
const std::vector<const char *> &fieldGroups();

// Declaration order is display order wherever the table drives a list.
//
// `label` and `unit` are repeated from the descriptor rather than looked up, so that writing a
// swing.json does not drag the metric catalogue into src/Export. The duplication is guarded:
// metric_catalogue_test walks this table and fails if any key is missing a descriptor or if a
// label or unit has drifted from it.
const std::vector<FieldDef> &fieldDefs();

} // namespace pinpoint::lm

Q_DECLARE_METATYPE(pinpoint::lm::LaunchMonitorReading)
