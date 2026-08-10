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

#include <cmath>
#include <cstddef>
#include <vector>

// R8-T1 blur-wedge measurement (shaft_detection_skeleton_design.md §R8). At
// delivery the club rotates at up to ~1400°/s, so within one exposure the
// shaft images not as a line but as a FAN about the grip: a plateau in the
// per-angle ridge response S(θ) whose energy-weighted centroid is the
// mid-exposure angle and whose angular width measures ω·t_exp. The v3 tracker
// hunts a thin 90 px straight ridge and sees nothing here; this module reads
// the plateau instead — verification by integration over the R6-predicted
// envelope, not per-pixel peaks.
//
// Honesty contract (§R8 rule 4): the threshold is ABSOLUTE (threshScale ×
// the S1 evAbsFloor reference — deliberately below the frame floor, safe only
// because the search is confined to the tight kinematic envelope). Both
// channels below threshold ⇒ NO candidate — the wedge widens the band where
// vision still contributes; it never pretends to see a shaft that isn't there.
//
// Pure header: no Qt, no OpenCV — operates on the per-θ score rows a proximal
// ridgeSweep already produced. Standalone-testable.

namespace pinpoint::analysis {

// "shaft.wedge.*" keys via ShaftV3Config::fromOverrides. enabled=false ⇒ the
// tracker never triggers, sweeps, injects, or tiers — byte-identical output.
struct WedgeConfig {
    // FLIPPED ON 2026-08-10 (S2 corpus gate + re-validation, shaft_wedge_p6_impl.md:
    // P6 emissions 19→46/61, every truth-labelled P6 within 5 ms, track.valid
    // 58 ≥ baseline 55, P2/P8 unchanged).
    bool   enabled          = true;    // master gate; false = the pre-S2 tracker byte-for-byte
    double omegaMinDegS     = 720.0;   // |ω̂| trigger: below this the thin-line machinery is trusted
    double tExpBootstrapS   = 0.003;   // exposure bootstrap when no calibration frame exists (s)
    double calOmegaLoDegS   = 286.0;   // |ω̂| floor (≈5 rad/s) for a frame to enter the t_exp median
    double kSigma           = 3.0;     // envelope half-width in σ_β units
    double threshScale      = 0.5;     // plateau threshold = threshScale × evAbsFloor
    double minSpanDeg       = 2.0;     // narrower plateaus are line-like noise, not a fan
    double proximalRHiFrac  = 0.35;    // proximal sweep ceiling as a fraction of rmax
    double proximalMinLenPx = 40.0;    // R5 blur relaxation of RidgeConfig.minLenPx (proximal only)
    double wWell            = 6.0;     // emission well depth at the centroid (DP cost units)
    double conf             = 0.45;    // emitted sample confidence for the WEDGE tier
    double dpTolDeg         = 4.0;     // DP θ within σ_θ + this of the centroid ⇒ WEDGE tier
    // FLIPPED ON 2026-08-10 with enabled (the gate's deciding arm: kinCone
    // converted 3 further truth swings by defunding structure-backed short
    // paths — the S1 finding's lever — at the cost of 1 track.valid, 59→58).
    bool   kinCone          = true;    // off-envelope penalty on triggered frames
    double wKinCone         = 4.0;     // its weight (cf. cfg.wCone) — prior-as-constraint, never a tier
};

// t_exp plausibility clamp (s): global-shutter golf capture sits within
// [1/1000, 1/125]; the median estimate is clamped here, and the bootstrap
// (3 ms) is used when no frame qualifies for calibration.
inline constexpr double kTExpLoS = 0.001;
inline constexpr double kTExpHiS = 0.008;

struct WedgeCandidate {
    bool   ok          = false;
    double centroidDeg = 0.0;   // energy-weighted circular centroid = mid-exposure angle
    double widthDeg    = 0.0;   // plateau angular extent ≈ ω·t_exp
    double energy      = 0.0;   // summed above-threshold response over the plateau
};

// Plateau finder over the envelope's per-θ score rows. rawScore/difScore are
// the proximal ridgeSweep responses at binDeg[j] (deg), in CONTIGUOUS arc
// order (the caller walks the envelope centre-out, so index adjacency == θ
// adjacency); difScore may be empty (no scene-median channel). absFloorRef is
// the S1 evAbsFloor (score units) — the absolute anchor the threshold scales
// from; <= 0 ⇒ no absolute reference exists ⇒ no candidate (never fabricate).
// phiSDeg/armVetoDeg: a centroid within armVetoDeg of φ+180 is the trail-arm
// smear, not the club — rejected.
inline WedgeCandidate measureWedge(const std::vector<float>& rawScore,
                                   const std::vector<float>& difScore,
                                   const std::vector<float>& binDeg,
                                   double phiSDeg, double armVetoDeg,
                                   double absFloorRef, const WedgeConfig& cfg)
{
    WedgeCandidate out;
    const size_t n = binDeg.size();
    if (n < 2 || rawScore.size() != n || (!difScore.empty() && difScore.size() != n))
        return out;
    if (absFloorRef <= 0.0) return out;
    const float thresh = float(cfg.threshScale * absFloorRef);

    // Per-bin response = best surviving channel; above = plateau membership.
    // Bin step from the arc (uniform by construction).
    auto wrapDeg = [](double d) {
        d = std::fmod(d + 180.0, 360.0);
        if (d < 0) d += 360.0;
        return d - 180.0;
    };
    const double stepDeg = std::abs(wrapDeg(double(binDeg[1]) - double(binDeg[0])));
    if (!(stepDeg > 0.0)) return out;

    // Best contiguous run of above-threshold bins by summed energy.
    size_t runStart = 0;
    bool   inRun = false;
    double bestEnergy = 0.0;
    size_t bestA = 0, bestB = 0;   // inclusive
    double runEnergy = 0.0;
    for (size_t j = 0; j <= n; ++j) {
        const bool above = j < n && (rawScore[j] >= thresh
                                     || (!difScore.empty() && difScore[j] >= thresh));
        if (above) {
            const float e = std::max(rawScore[j], difScore.empty() ? 0.f : difScore[j]);
            if (!inRun) { inRun = true; runStart = j; runEnergy = 0.0; }
            runEnergy += double(e);
        } else if (inRun) {
            inRun = false;
            const double spanDeg = double(j - 1 - runStart) * stepDeg;
            if (spanDeg >= cfg.minSpanDeg && runEnergy > bestEnergy) {
                bestEnergy = runEnergy; bestA = runStart; bestB = j - 1;
            }
        }
    }
    if (bestEnergy <= 0.0) return out;

    // Energy-weighted circular centroid over the winning run, computed as an
    // offset from the run's first bin so the arc's own wrap never bites.
    const double ref = double(binDeg[bestA]);
    double wSum = 0.0, oSum = 0.0;
    for (size_t j = bestA; j <= bestB; ++j) {
        const double e = double(std::max(rawScore[j], difScore.empty() ? 0.f : difScore[j]));
        if (e < thresh) continue;   // interior bins are above by construction; keep exact
        wSum += e;
        oSum += e * wrapDeg(double(binDeg[j]) - ref);
    }
    if (!(wSum > 0.0)) return out;
    double cen = std::fmod(ref + oSum / wSum, 360.0);
    if (cen < 0) cen += 360.0;

    // Trail-arm smear veto: the forearm sweeps with the club and produces its
    // own fan at φ+180 — never promote it to a club measurement.
    const double armDeg = std::fmod(phiSDeg + 180.0, 360.0);
    if (std::abs(wrapDeg(cen - armDeg)) < armVetoDeg) return out;

    out.ok          = true;
    out.centroidDeg = cen;
    out.widthDeg    = double(bestB - bestA) * stepDeg;
    out.energy      = bestEnergy;
    return out;
}

} // namespace pinpoint::analysis
