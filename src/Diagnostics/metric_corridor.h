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

#include "characteristic_pack.h"
#include "norm_provider.h"

#include <optional>

// The (metric key, phase) → corridor resolution every metric surface renders.
//
// This is the second half of `measureForMetricAtPhase()`. That answers *which measure reads this
// metric at this phase*; this carries the answer on to the norm that grades it and to the four
// numbers a corridor is drawn from. Together they are what replaced `MetricCatalogue::corridor()`
// at stage 9: a metric descriptor states what a metric IS and no longer judges it, so a corridor
// comes from the norm set, resolved in the shot's own context, and from nowhere else.
//
// It is header-only and free-standing on purpose. Its callers are a QML façade
// (`Gui/review/metric_catalog.cpp`, which feeds MetricDetail, PpBandRail and the dashboard zones)
// and its test — and a rule that exists only inside a façade is a rule nothing can test.

namespace pinpoint::analysis {

// One phase's corridor, ready to render. The four band edges keep the names the surfaces already
// read (`greenLo/greenHi/amberLo/amberHi`), so what changed at stage 9 is where the numbers come
// from, not the shape anything draws.
struct MetricCorridor {
    Phase  phase   = Phase::Impact;
    double greenLo = 0.0, greenHi = 0.0;   // the Ideal band — the norm's own claim, policy-free
    double amberLo = 0.0, amberHi = 0.0;   // out to where Action begins

    // True when the measure behind it is a change from address rather than an absolute reading.
    // Two corridors on ONE metric can now differ here — the Δ-from-address cell measure at the top
    // of the swing and the absolute reading at impact are different measures of one series — so a
    // surface drawing both has to say which is which.
    bool deltaFromAddress = false;

    // Provenance, for a surface that wants to say where the corridor came from rather than just
    // draw it. `contextId` is where the norm was actually authored, which is not necessarily the
    // context asked for — that is what the tree is for.
    QString measureId;
    QString contextId;
    bool    inherited  = false;
    bool    overridden = false;          // a non-core layer supplied it: the user's own corridor
};

// Resolve the corridor for `metricKey` at `phase`, in `contextId`.
//
// It walks the measures that read that metric at that phase in PREFERENCE order (absolute reading
// first — that is what a phase-keyed corridor means — then a change observed at that phase) and
// takes the first one a norm resolves for. It is not enough to take the preferred measure and stop:
// `m_leadWristAtTop` wins at P4 and carries no norm, so stopping there would draw nothing at the top
// of the swing while the Δ-from-address measure beside it has carried a corridor since v1. Which one
// answered is reported — `measureId`, and `deltaFromAddress` — because the two are different
// quantities and a surface drawing both must be able to say which is which.
//
// Returns nothing — never a zeroed corridor — when no measure reads that metric at that phase, or
// when none of them has a norm anywhere on the context chain. That is a different fact from "the
// corridor is 0 to 0", and a caller drawing the latter would invent a band the pack does not hold.
inline std::optional<MetricCorridor> corridorForMetricAtPhase(const CharacteristicPack &pack,
                                                              const INormProvider      &norms,
                                                              const QString            &metricKey,
                                                              Phase                     phase,
                                                              const QString            &contextId,
                                                              const GradePolicy        &policy = {})
{
    for (const Measure *m : measuresForMetricAtPhase(pack, metricKey, phase)) {
        const NormResolution res = norms.resolve(m->id, contextId);
        if (!res.found())
            continue;

        const NormBandEdges e = bandEdgesOf(*res.norm, policy);

        MetricCorridor c;
        c.phase            = phase;
        c.greenLo          = e.idealLo;
        c.greenHi          = e.idealHi;
        c.amberLo          = e.watchLo;
        c.amberHi          = e.watchHi;
        c.deltaFromAddress = measureIsDeltaFromAddress(*m);
        c.measureId        = m->id;
        c.contextId        = res.contextId;
        c.inherited        = res.inherited;
        c.overridden       = res.overridden;
        return c;
    }
    return std::nullopt;
}

} // namespace pinpoint::analysis
