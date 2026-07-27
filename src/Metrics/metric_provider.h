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

#include "reference_bands.h"   // BandContext
#include "swing_analysis.h"    // SegmentRole, ReconstructionTier

#include <QString>

#include <vector>

// The provider seam (design §5). A producer declares which descriptor keys it can satisfy and, per
// shot, at what quality — generalising swing_data_source's lane resolution to every metric. It does
// NOT compute the metric: production stays in the analysis stages. The catalogue is assembled from a
// fixed provider set on demand (makeMetricCatalogue), mirroring makeReferenceBandProvider — there is
// deliberately no startup registration / self-registering static (analysis_stage.h anti-goals).

namespace pinpoint::analysis {

// The per-shot facts the resolver needs (design §3.5). Built once per shot from swing.json (tier,
// analysis.bindings[].role, pose/shaft/ball stream presence, session type) — the same fields
// swing_data_source already reads — or from the studio's configured capability when the directory
// is browsed with no shot loaded ("available with my setup").
struct ShotContext {
    ReconstructionTier       tier         = ReconstructionTier::Angles2D;
    std::vector<SegmentRole> imuRoles;      // roles actually bound & calibrated this shot
    bool                     hasFaceOn    = false;
    bool                     hasClubTrack = false;   // ShaftTrack2D valid
    bool                     hasBallTrack = false;   // BallTrack2D present
    bool                     hasLaunchMonitor = false;   // a launch monitor supplied readings for this shot
    int                      sessionType  = -1;      // SessionController::Type; -1 = none
    BandContext              band;                   // archetype/club/shape for normative resolution

    bool hasRole(SegmentRole r) const
    {
        for (SegmentRole x : imuRoles)
            if (x == r) return true;
        return false;
    }
};

// The result of resolving one metric against one shot (design §3.5). Bridged = produced but at
// reduced fidelity / a lower tier than ideal (graceful degradation), distinct from a hard miss.
struct MetricAvailability {
    enum State { Measured, Bridged, Unavailable };
    State              state  = Unavailable;
    QString            reason;                             // "needs face-on camera", "partial · needs body IMUs"
    ReconstructionTier tier   = ReconstructionTier::Angles2D;
};

// Best-state ordering used by the resolver: Measured > Bridged > Unavailable.
inline int metricStateRank(MetricAvailability::State s)
{
    switch (s) {
    case MetricAvailability::Measured:    return 2;
    case MetricAvailability::Bridged:     return 1;
    case MetricAvailability::Unavailable: return 0;
    }
    return 0;
}

// A producer's capability declaration. Stateless; one shared const instance per producer lives for
// the process lifetime (makeMetricCatalogue holds them as function-local statics), so the catalogue
// keeps only non-owning pointers.
class IMetricProvider {
public:
    virtual ~IMetricProvider() = default;

    // The descriptor keys this producer can satisfy (context-independent).
    virtual std::vector<QString> provides() const = 0;

    // Per-shot quality for one of its keys. Called only for keys in provides().
    virtual MetricAvailability availability(const QString &key, const ShotContext &ctx) const = 0;

    // Lower value wins when two providers claim the same key at equal state. Default 0.
    virtual int priority() const { return 0; }
};

} // namespace pinpoint::analysis
