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

#include "metric_type.h"
#include "swing_analysis.h"          // Phase, SegmentRole, ReconstructionTier
// wrist_assessment_types.h is no longer needed HERE (PpJointDof left with MetricNormative at stage 9)
// but several includers of this header have always got PpJointDof / PpSwingPosition through it.
#include "wrist_assessment_types.h"

#include <QString>
#include <QStringList>

#include <vector>

// MetricDescriptor — the stable identity + metadata of a metric (design §3.4). Constant for every
// shot; never names a producer (design principle 1). Per-shot availability is resolved separately
// (metric_provider.h). All header-only value types, Qt-only, no Qt-GUI.

namespace pinpoint::analysis {

// What a metric needs before any producer can satisfy it (design §3.2). The descriptor states
// requirements; it never names a provider. When no provider claims a key, the resolver renders the
// unmet requirement into a human-readable availability reason ("needs face-on camera").
struct MetricRequirement {
    bool                     faceOnCamera = false;                     // pose / shaft / ball products
    std::vector<SegmentRole> imuRoles;                                 // required anatomical IMU roles
    bool                     clubTrack   = false;                      // ShaftTrack2D present
    bool                     ballTrack   = false;                      // BallTrack2D present
    // A connected launch monitor. Face angle at impact, spin and strike location are not optically
    // resolvable at our frame rates, and an integration is intended rather than a producer of our
    // own. Stating it HERE rather than as a separate status is what makes the absence graceful: a
    // user without one gets "needs a launch monitor" through the same path a missing face-on camera
    // takes, and the day a connector lands the same metric resolves Measured with no content change.
    bool                     launchMonitor = false;
    ReconstructionTier       minTier     = ReconstructionTier::Angles2D;
};

// A metric descriptor carries NO normative values.
//
// Until stage 9 of the diagnostics-norms work it did: `MetricNormative` held a DOF to delegate to
// the compiled band table, or a per-phase inline corridor, plus a hand-written note naming the
// context those numbers assumed. All three are gone. Corridors are now content in the norm set,
// keyed on a MEASURE (post-reducer, so "Δ from address at the top" and "absolute at impact" cannot
// be confused for one another) and resolved in the shot's own context through the tree — see
// `Diagnostics/metric_corridor.h`, which is what every metric surface calls.
//
// The descriptor states what a metric IS. It no longer judges it.

// The metric descriptor. `key` equals the existing MetricSeries.key / ScoredMetric.key.
struct MetricDescriptor {
    QString    key;                        // == MetricSeries.key (stable identity)
    MetricType type = MetricType::TimeSeries;
    QString    label;                      // "Lead wrist — bow / cup"
    QString    shortLabel;                 // "Bow/cup" (mirrors ChartMetrics::shortLabel)
    QString    unit;                       // "°", "mph", "×frame", …
    QString    group;                      // "Wrist & forearm" | "Club & speed" | "Setup" | …

    QString    description;                // what it means (consolidated from docs/)
    QString    howToRead;                  // sign convention, when to read, what good looks like
    bool       flexPositive = true;        // sign polarity, mirrors MetricSeries.flexPositive

    std::vector<Phase> phases;             // phases sampled (PointInTime) / where peak matters (TimeSeries)
    bool               scored = false;     // has a band and contributes to a score
    // Roadmap placeholder: in the design catalogue but no producer yet (always resolves Unavailable).
    // Surfaced so the directory can badge it "Planned" — the requirement then reads as "will need …".
    bool               planned = false;

    // NB: named `requirement`, NOT `requires` — `requires` is a C++20 keyword and cannot be an identifier.
    MetricRequirement  requirement;

    // Reverse index of consumers — static, hand-authored in the manifest (design §13.2 decision).
    // Powers the detail page's "Where it's used". e.g. {"dashboard:motion","score:wrist","fault:cuppedAtTop"}.
    QStringList        usedBy;
};

} // namespace pinpoint::analysis
