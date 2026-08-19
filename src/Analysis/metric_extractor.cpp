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

#include "metric_extractor.h"

#include <QString>
#include <algorithm>

#include "wrist_angles.h"
#include "../Core/pp_profiler.h"

namespace pinpoint::analysis {
namespace {

int nearestIndex(const std::vector<int64_t> &grid, int64_t t)
{
    if (t <= grid.front()) return 0;
    if (t >= grid.back())  return static_cast<int>(grid.size()) - 1;
    const auto it = std::lower_bound(grid.begin(), grid.end(), t);
    const int hi = static_cast<int>(it - grid.begin());
    const int lo = hi - 1;
    return (t - grid[lo] <= grid[hi] - t) ? lo : hi;
}

int64_t phaseTime(const std::vector<PhaseEvent> &phases, Phase p, int64_t fallback)
{
    for (const PhaseEvent &e : phases)
        if (e.phase == p) return e.t_us;
    return fallback;
}

MetricSeries buildSeries(const QString &key, const QString &label, const QString &unit,
                         const std::vector<int64_t> &grid, std::vector<double> value,
                         const std::vector<PhaseEvent> &phases)
{
    MetricSeries m;
    m.key   = key;
    m.label = label;
    m.unit  = unit;
    m.t_us  = grid;
    m.value = std::move(value);
    for (const Phase p : { Phase::Address, Phase::Top, Phase::Impact }) {
        const int idx = nearestIndex(grid, phaseTime(phases, p, grid.front()));
        m.phaseSamples.push_back({ p, grid[idx], m.value[idx], QString() });
    }
    return m;
}

// WHICH INSTRUMENT MEASURED IT, IN THE KEY. A wG3's wrist angles publish as
// `hm.leadWristFlexExt`; a Witmotion's as the bare `leadWristFlexExt`. New keys,
// never overwritten ones — the bare key therefore always means OUR estimate, and
// which instrument graded a swing stays recoverable after the fact. That is the
// entire point of the split, and it matters more rather than less now that no swing
// can ever carry both: an across-session comparison is the only comparison there is.
//
// ⚠ THE ARITHMETIC EITHER SIDE OF THIS CHOICE IS IDENTICAL AND MUST STAY SO. With
// one instrument per swing, "both instruments are measured by the same maths" is a
// property of THIS CODE PATH rather than something a dual-worn capture could
// demonstrate. So the prefix decides a name and nothing else; a parallel producer
// for the HackMotion lane would quietly make every instrument difference
// uninterpretable.
//
// ⚠ Mirrors the `lm.` convention (metric_catalogue_manifest.cpp:2213) — a prefix
// means "this device said it", and the ladder in Measure::preferKeys is where a
// measure states which rung it would rather have.
QString instrumentKey(const QString &bare, bool hackMotion)
{
    return hackMotion ? QStringLiteral("hm.") + bare : bare;
}

} // namespace

std::vector<MetricSeries> MetricExtractor::extract(const FusedStreams &s,
                                                   const std::vector<PhaseEvent> &phases,
                                                   int handedness)
{
    std::vector<MetricSeries> out;
    const std::vector<int64_t> &grid = s.timeGrid;
    const int N = static_cast<int>(grid.size());
    if (N < 2)
        return out;

    // [seam] rough proxy for the derived-series working set during extraction
    // (≈ up to 8 N-length double series produced below).
    PP_PROFILE_MEM_SCOPE("Analysis.Series", int64_t(N) * 8 * int64_t(sizeof(double)));

    // Lead arm = left unless the golfer is left-handed (provisional sign convention,
    // confirmed on the wizard "check your sensors" page — see wrist_angles.h).
    const bool leftArm = (handedness != 2);

    const SegmentStream *fore  = s.streamFor(SegmentRole::LeadForearm);
    const SegmentStream *hand  = s.streamFor(SegmentRole::LeadHand);
    const SegmentStream *upper = s.streamFor(SegmentRole::LeadUpperArm);

    // Series are NEUTRAL-relative (absolute joint posture vs the calibration neutral) —
    // the same reference as the live check-sensors readout, and what the scorer's absolute
    // bands expect. Δ-from-address is derived in the UI from the Address phase-sample
    // (docs/design/shot_analyzer_viz.md).

    // --- wrist flex/ext + radial/ulnar (forearm + hand) ---
    if (fore && hand && static_cast<int>(fore->qAnat.size()) == N
                     && static_cast<int>(hand->qAnat.size()) == N) {
        // Both units of a wrist joint come from one peripheral, so the two agree by
        // construction; requiring both says so rather than trusting it.
        const bool hmWrist = fore->hackMotion && hand->hackMotion;
        std::vector<double> fe(N), rud(N);
        for (int i = 0; i < N; ++i) {
            const QQuaternion rel = (fore->qAnat[i].conjugated() * hand->qAnat[i]).normalized();
            const WristAngles wa = wristFlexExtDeviation(rel, leftArm);
            fe[i]  = radToDeg(wa.feRad);
            rud[i] = radToDeg(wa.rudRad);
        }
        out.push_back(buildSeries(instrumentKey(QStringLiteral("leadWristFlexExt"), hmWrist),
                                  QStringLiteral("Lead wrist (bow/cup)"), QStringLiteral("°"),
                                  grid, std::move(fe), phases));
        out.push_back(buildSeries(instrumentKey(QStringLiteral("leadWristRadUln"), hmWrist),
                                  QStringLiteral("Lead wrist hinge"), QStringLiteral("°"),
                                  grid, std::move(rud), phases));
    }

    // --- forearm rotation: ONE SEGMENT, address-referenced (forearm alone) ---
    //
    // ⚠ THIS IS NOT forearmPronation AND MUST NOT BE READ AS ONE. That is the ISB
    // radioulnar angle, defined against the HUMERUS, and it is emitted below only
    // when an upper-arm unit exists. This asks the different and simpler question a
    // lone forearm sensor CAN answer — how far has the forearm turned since Address
    // — and the address-to-impact travel is the direct indicator of flipping.
    //
    // ⚠ SO IT IS PRODUCED WHENEVER THE FOREARM IS BOUND, INDEPENDENTLY OF THE UPPER
    // ARM. A Witmotion rig wearing A+B+C delivers BOTH: forearmPronation, which is
    // the joint angle, and this, which is the segment's own travel. Gating it on the
    // upper arm being absent would make the same name mean one thing on a two-sensor
    // rig and nothing on a three-sensor one, and would leave a P1→P7 comparison
    // between two of this golfer's own sessions undefined.
    //
    // ⚠ ONE DEFINITION ACROSS BOTH VENDORS — slot A alone, identical maths, which is
    // why it is computed here rather than per-instrument. Defining a Witmotion's
    // rotation against the upper arm while a wG3's is forearm-alone would publish two
    // different quantities under one name, and any cross-instrument comparison would
    // read the shoulder's contribution as sensor error.
    //
    // Rule 0 does not govern a single segment's axial rotation (ISB defines rotations
    // BETWEEN two segment triads), so Rule 1 does: positive is the pronation
    // direction, AGREEING with the vendor exactly where leadWristFlexExt deliberately
    // disagrees with them. That asymmetry is correct and is written down in
    // docs/design/pinpoint_sign_conventions.md.
    if (fore && static_cast<int>(fore->qAnat.size()) == N) {
        const int addrIdx = nearestIndex(grid, phaseTime(phases, Phase::Address, grid.front()));
        const QQuaternion qAddr = fore->qAnat[addrIdx];
        std::vector<double> rot(N);
        for (int i = 0; i < N; ++i) {
            const ForearmElbow ef = forearmPronElbowFlex(forearmRel(fore->qAnat[i], qAddr),
                                                         leftArm);
            rot[i] = radToDeg(ef.pronRad);
        }
        out.push_back(buildSeries(instrumentKey(QStringLiteral("forearmRotation"),
                                                fore->hackMotion),
                                  QStringLiteral("Lead forearm rotation"), QStringLiteral("°"),
                                  grid, std::move(rot), phases));
    }

    // --- forearm pronation + elbow flexion (upper arm + forearm) ---
    if (upper && fore && static_cast<int>(upper->qAnat.size()) == N
                      && static_cast<int>(fore->qAnat.size()) == N) {
        std::vector<double> pron(N), elbow(N);
        for (int i = 0; i < N; ++i) {
            const QQuaternion rel = (upper->qAnat[i].conjugated() * fore->qAnat[i]).normalized();
            const ForearmElbow ef = forearmPronElbowFlex(rel, leftArm);
            pron[i]  = radToDeg(ef.pronRad);
            elbow[i] = radToDeg(ef.flexRad);
        }
        out.push_back(buildSeries(QStringLiteral("forearmPronation"),
                                  QStringLiteral("Lead forearm roll"), QStringLiteral("°"),
                                  grid, std::move(pron), phases));
        out.push_back(buildSeries(QStringLiteral("leadArmFlexion"),
                                  QStringLiteral("Lead arm (elbow)"), QStringLiteral("°"),
                                  grid, std::move(elbow), phases));
    }

    return out;
}

} // namespace pinpoint::analysis
