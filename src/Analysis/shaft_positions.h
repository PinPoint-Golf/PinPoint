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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../Core/pp_tuned_constants.h"   // tuned::positions:: (P1 club-quiet σ; Qt/OpenCV-free)

// Position-first shaft measurement — Layer B "B-time location"
// (docs/design/shaft_position_first_design.md §2 Layer B). Header-only, standalone
// (no Qt / no OpenCV), unit-tested per the detector-math convention
// (impact_detector.h / onset_detector.h). Pure 1-D crossing detection on the
// already-smoothed per-frame θ(t) and lead-arm φ(t) signals the shaft tracker
// produces — far more stable than any per-frame measurement — returning the
// located coaching P-times P1–P8.
//
// The coaching P-system (design §1):
//   P1 Address · P2 Shaft parallel (backswing) · P3 Lead arm parallel (backswing)
//   P4 Top     · P5 Lead arm parallel (downswing) · P6 Shaft parallel (downswing)
//   P7 Impact  · P8 Shaft parallel (follow-through)
// P1/P4/P7 are anchored to the tracker's segmentation landmarks (address/top/
// impact frames). P2/P6/P8 are image-plane shaft-parallel crossings of θ(t);
// P3/P5 are lead-arm-parallel crossings of φ(t).
//
// PARALLEL PREDICATE (image plane, atan2 convention).
//   θ is the shaft direction grip→head as an image angle: dir = (cos θ, sin θ)
//   with image y DOWN (shaft_track_assembly.cpp: headPx = grip + L·(cos θ, sin θ)).
//   φ is the lead-arm direction in the same convention. The shaft/arm is
//   "parallel to the ground" when its direction is HORIZONTAL, i.e. its vertical
//   component sin(·) = 0. We fold the two horizontal directions (pointing left vs
//   right) together with an ELEVATION angle
//        elevationDeg(a) = atan2(sin a, |cos a|)   (deg)
//   which is 0 at horizontal, +90 pointing straight down (head below grip), −90
//   straight up — continuous through vertical, and monotone across each phase
//   window (down→up in the backswing, up→down in the downswing, down→up in the
//   follow-through). A parallel event is a zero-crossing of elevationDeg.
//   "Image-plane parallel", not true 3-D parallel — accepted coaching practice on
//   face-on video (design §1, "Accepted caveat").
//
// HYSTERESIS. A crossing is confirmed only after the signal has travelled beyond
// ±hysteresisDeg on BOTH sides of horizontal, so a sub-band wiggle around
// horizontal (noise) can neither arm nor double-fire; the reported time is the
// steepest zero-straddling frame pair between the two armed frames, interpolated
// sub-frame. Missing crossings (abbreviated swings, NaN φ) ⇒ that P is simply
// absent from the result — the caller reports per-P coverage n/8 honestly.

namespace pinpoint::analysis {

// B2 milestone-fit tuning (shaft_position_first §2 Layer B "B-fit"). Pure POD (no
// Qt / no OpenCV) so it lives here beside the B-time config; consumed by
// fitPosition (shaft_position_fit.h) which owns the OpenCV shift-and-stack. Keys
// are dotted "positions.*" via ShaftV3Config::fromOverrides. fitEnabled=false ⇒
// the fit never runs ⇒ ShaftTrack2D.positions stays the B1 TrackSample track
// byte-for-byte (soak contract). The empirical P-shape (B1 baseline gate) drives
// the two sector widths: at P1–P4 the DP track is already good (local polish);
// at P5–P8 it is 100+ px off with the direction sometimes FLIPPED ~180°, so the
// fit searches GLOBALLY over the arm-plausibility-admitted sector around the
// (reliable) grip anchor — a local refinement cannot recover there.
struct PositionFitConfig {
    bool   fitEnabled       = true;   // ON since B4 (b2-fit2 gate: never-degrade guards hold, P7 flip fixed)
    int    halfWindowFrames = 4;      // ±k frames stacked (shift-and-stack registration)
    double minFitConf       = 0.35;   // accept floor on ridge support under the fitted line
    double wideSectorDeg    = 170.0;  // θ search WIDTH at P5–P8 (global; centred on arm/ball)
    double narrowSectorDeg  = 15.0;   // θ search WIDTH at P1–P4 (local polish about the track θ)
    double gripSearchPx     = 6.0;    // grip perturbation half-range (px)
    // search resolution + plausible radial-length band (design §2 Layer B step 2)
    double thetaStepDeg     = 0.5;    // coarse θ grid step (deg)
    double lenStepPx        = 3.0;    // radial-length search step (px)
    double lenMinFracH      = 0.06;   // Lmin = max(armFloor, lenMinFracH·frameH)
    double lenMaxFracH      = 0.62;   // Lmax = lenMaxFracH·frameH (matches decideTrack's rmax)
    double gripStepPx       = 2.0;    // grip perturbation grid step (px)
    int    corridorHalfPx   = 2;      // lateral corridor half-width for the ridge integral (px)
    double ballBonus        = 0.6;    // P7/P1 head-near-ball multiplicative reward (0 = off)
    double ballSigmaPx      = 22.0;   // Gaussian falloff of the ball reward (px)
    double plateauFrac      = 0.9;    // σ = half-width of the ≥plateauFrac·max score plateau
    // Never-degrade guards (B2 corpus gate 2026-07-11: fits at measured-tier
    // P2/P4 made them WORSE — 8.4→11.6 px / 2.9→7.9 px — while the ridge-terminus
    // L was weak everywhere; the fit must only rescue positions the track has
    // actually lost, and the drawn extent should come from the fused club length).
    double skipMeasuredConf = 0.5;    // skip the fit when the nearest emitted sample is
                                      // vision-measured with conf ≥ this (0 = never skip)
    bool   useFusedLen      = true;   // accepted fits draw at the fused club length
                                      // (σL from the fusion posterior), not the ridge L
};

// "positions.*" tuning namespace (design §4). Nested in ShaftV3Config as
// `positions`; enabled=false keeps position extraction dark (a positions-off run
// leaves ShaftTrack2D.positions empty ⇒ swing.json byte-identical).
struct PositionsConfig {
    bool   enabled       = true;    // ON since B4 (report-only extraction, additive positions[])
    double hysteresisDeg = 8.0;     // deadband each side of horizontal (arm + anti-double-fire)
    // P1 = end of the ADDRESS HOLD, not bs0 (Mark's 2026-07-11 markup: bs0 is the
    // takeaway-start frame and fired ~150 ms late on a slow driver takeaway — P1
    // landed 164 ms after the labelled address, 29° into the sweep, while a 1.8 s
    // ball-anchored still-hold sat unused). Camera-first principle: the hold is
    // located from grip stillness on the face-on track, corroborated by the
    // BallAnchored flag span when present.
    // Still = small NET drift over the trailing window, not small per-frame
    // speed: takeaway CREEP (~0.25 px/frame, directional) and pose-keypoint
    // JITTER (~1 px, zero-mean) overlap per-frame — a speed threshold lands
    // either 90 ms late (2.0, inside the creep) or arbitrarily early (0.75,
    // below the noise floor), both measured on Mark's marked driver swing.
    // Net-over-window separates them: creep nets ~2.5 px/10 frames, jitter <1.
    double p1StillNetPx   = 2.0;    // net grip displacement across the window ⇒ motion at/above
    double p1StillSpeedPx = 2.5;    // per-frame spike cap — a violent waggle is not "still"
                                    // even when it nets out (oscillation returns to the ball)
    int    p1StillWindow  = 10;     // trailing window (frames; ~67 ms at 150 fps)
    // W3 club-quiet gate (addressHoldEndFrame's optional clubQuiet mask): a frame
    // counts as club-quiet when its ball-corridor activity (BallSample2D.clubActivity,
    // ball.clubActivity) is below this many robustNoise σ. Only consulted when the
    // ball track carries activity — a club bob about a FROZEN grip is invisible to
    // the grip-only stillness test above, and this is what catches it. The same
    // activity σ is the seed for the EventRefine Tier-B at-ball gate (refine.
    // activityQuietSigma, event_refine.h) — the two-consumer contract widened when
    // EventRefine landed (swing_analysis.h BallSample2D.clubActivity).
    double p1ClubQuietSigma = tuned::positions::kP1ClubQuietSigma;
    // P6 = the LAST horizontal crossing in (P4, P7), not the first. The
    // elevation fold treats θ=0 and θ=180 as the same "parallel", and on swings
    // whose top-of-swing shaft sits just above the fold (θ(top) ≈ 20°) the
    // shaft dips through θ≈0 a dozen frames after P4 — the first crossing —
    // while the true delivery parallel (θ=180, ~4 frames before impact) is the
    // last. "Delivery" is definitionally the final parallel before impact, so
    // last-crossing is the physical reading; a single-crossing window is
    // unchanged (last == first). Dark until the S2 default-flip commit
    // (2026-08-10 gate: the wedge-corrected long path exposed the first-pick
    // on 0703 swings 0008/0009, −140 ms; p2/p8 windows keep first-crossing —
    // correct for their geometry).
    bool   p6LastCrossing = false;
    PositionFitConfig fit;          // B2 milestone fit (dark until fit.fitEnabled flips)
};

// One located P-time. `p` is the coaching P-index 1..8, or 10 (Finish) — P9 and a
// fuller follow-through model are reserved (NOT a Phase enum value); `tUs` is the
// sub-frame-interpolated event time in the input tUs domain.
struct PTime {
    int     p   = 0;
    int64_t tUs = 0;
};

namespace positions_detail {

constexpr double kPi = 3.14159265358979323846;

// Elevation of an image-plane direction above/below horizontal (deg), folding the
// two horizontal directions together — 0 at horizontal, ±90 at vertical.
inline double elevationDeg(double angleDeg)
{
    const double r = angleDeg * kPi / 180.0;
    return std::atan2(std::sin(r), std::abs(std::cos(r))) * 180.0 / kPi;
}

struct Crossing {
    bool    found = false;
    int64_t tUs   = 0;
};

// The single hysteresis-confirmed horizontal crossing of `angleDeg` over the
// frame window [i0, i1] (inclusive). NaN frames are skipped. Returns not-found
// when the signal never arms on both sides (no genuine transit through
// horizontal). See the header preamble for the predicate + hysteresis contract.
inline Crossing findHorizontalCrossing(const std::vector<int64_t>& tUs,
                                       const std::vector<double>& angleDeg,
                                       int i0, int i1, double hystDeg)
{
    Crossing out;
    const int n = int(angleDeg.size());
    i0 = std::max(0, i0);
    i1 = std::min(n - 1, i1);
    if (i1 <= i0) return out;

    // Valid (non-NaN) frame indices in the window — crossings interpolate between
    // consecutive VALID frames so a NaN gap never fabricates a straddle.
    std::vector<int> vi;
    for (int i = i0; i <= i1; ++i)
        if (!std::isnan(angleDeg[i])) vi.push_back(i);
    if (int(vi.size()) < 2) return out;

    // Hysteresis state machine: find the first armed +→− or −→+ transition.
    // armedSign ∈ {−1,0,+1}; armA = valid-array index of the last armed frame.
    int armedSign = 0, armA = -1, fromA = -1, toA = -1;
    for (int a = 0; a < int(vi.size()); ++a) {
        const double e = elevationDeg(angleDeg[vi[a]]);
        if (e > hystDeg) {
            if (armedSign < 0) { fromA = armA; toA = a; break; }
            armedSign = 1; armA = a;
        } else if (e < -hystDeg) {
            if (armedSign > 0) { fromA = armA; toA = a; break; }
            armedSign = -1; armA = a;
        }
    }
    if (fromA < 0) return out;   // never armed both sides ⇒ no genuine crossing

    // Steepest zero-straddling consecutive-valid pair within [fromA, toA] — the
    // genuine transit through horizontal (noise crossings are shallower), then
    // interpolate the zero sub-frame.
    double  bestSlope = -1.0;
    int64_t bestT     = 0;
    bool    have      = false;
    for (int a = fromA; a < toA; ++a) {
        const double ea = elevationDeg(angleDeg[vi[a]]);
        const double eb = elevationDeg(angleDeg[vi[a + 1]]);
        const bool straddle = (ea >= 0.0 && eb < 0.0) || (ea <= 0.0 && eb > 0.0);
        if (!straddle) continue;
        const double slope = std::abs(ea - eb);
        if (slope <= bestSlope) continue;
        bestSlope = slope;
        const double frac = (ea == eb) ? 0.0 : ea / (ea - eb);   // ∈ [0,1]
        const int64_t ta = tUs[vi[a]], tb = tUs[vi[a + 1]];
        bestT = ta + int64_t(std::llround(frac * double(tb - ta)));
        have = true;
    }
    if (!have) { out.found = true; out.tUs = tUs[vi[toA]]; return out; }   // numerical fallback
    out.found = true;
    out.tUs   = bestT;
    return out;
}

// The LAST hysteresis-confirmed horizontal crossing over [i0, i1] — the same
// state machine as findHorizontalCrossing, but it re-arms and keeps scanning
// after each transition instead of stopping at the first, then refines the
// final transition's span. On a single-crossing window the two functions
// return the identical instant. Self-contained sibling (the first-crossing
// body above is frozen under its unit pins — deliberately not refactored).
inline Crossing findLastHorizontalCrossing(const std::vector<int64_t>& tUs,
                                           const std::vector<double>& angleDeg,
                                           int i0, int i1, double hystDeg)
{
    Crossing out;
    const int n = int(angleDeg.size());
    i0 = std::max(0, i0);
    i1 = std::min(n - 1, i1);
    if (i1 <= i0) return out;

    std::vector<int> vi;
    for (int i = i0; i <= i1; ++i)
        if (!std::isnan(angleDeg[i])) vi.push_back(i);
    if (int(vi.size()) < 2) return out;

    // Hysteresis state machine, run to the END of the window: every armed
    // +→− / −→+ transition overwrites (fromA, toA), so the survivor is the
    // last genuine transit through horizontal.
    int armedSign = 0, armA = -1, fromA = -1, toA = -1;
    for (int a = 0; a < int(vi.size()); ++a) {
        const double e = elevationDeg(angleDeg[vi[a]]);
        if (e > hystDeg) {
            if (armedSign < 0) { fromA = armA; toA = a; }
            armedSign = 1; armA = a;
        } else if (e < -hystDeg) {
            if (armedSign > 0) { fromA = armA; toA = a; }
            armedSign = -1; armA = a;
        }
    }
    if (fromA < 0) return out;   // never armed both sides ⇒ no genuine crossing

    // Steepest zero-straddling pair within the final transition, interpolated
    // sub-frame — identical refinement to the first-crossing sibling.
    double  bestSlope = -1.0;
    int64_t bestT     = 0;
    bool    have      = false;
    for (int a = fromA; a < toA; ++a) {
        const double ea = elevationDeg(angleDeg[vi[a]]);
        const double eb = elevationDeg(angleDeg[vi[a + 1]]);
        const bool straddle = (ea >= 0.0 && eb < 0.0) || (ea <= 0.0 && eb > 0.0);
        if (!straddle) continue;
        const double slope = std::abs(ea - eb);
        if (slope <= bestSlope) continue;
        bestSlope = slope;
        const double frac = (ea == eb) ? 0.0 : ea / (ea - eb);   // ∈ [0,1]
        const int64_t ta = tUs[vi[a]], tb = tUs[vi[a + 1]];
        bestT = ta + int64_t(std::llround(frac * double(tb - ta)));
        have = true;
    }
    if (!have) { out.found = true; out.tUs = tUs[vi[toA]]; return out; }   // numerical fallback
    out.found = true;
    out.tUs   = bestT;
    return out;
}

} // namespace positions_detail

// P1 instant = the END of the address hold: the last frame at/before bs0 whose
// trailing p1StillWindow frames all have grip speed < p1StillSpeedPx. bs0 is the
// phase model's TAKEAWAY-start frame — definitionally motion, and it can fire
// well after true onset on a slow takeaway — so walking back to sustained
// stillness recovers the coaching P1 (the still setup). When any frame in
// [0, bs0] carries ball-anchored evidence (baByFrame non-empty), the chosen
// frame must itself be ball-anchored — the camera-first corroboration that the
// club is still AT THE BALL, not paused mid-waggle; if no such frame exists the
// stillness-only frame stands. No sustained stillness at all ⇒ bs0 (unchanged
// legacy behaviour, e.g. a swing that enters the window already moving).
//
// SECOND CALLER (reuse, don't re-derive): the late-pipeline EventRefine slot
// (event_refine.h) re-invokes this with the SAME signature but a bs0 set to its
// refined Takeaway frame (the last-departure/no-return L) and a clubQuiet mask
// built from refine.activityQuietSigma — so the refined Address is the hold end
// walked back from the refined takeaway, on identical machinery.
//
// W3 optional clubQuiet mask (default nullptr ⇒ EVERY existing caller/test is
// byte-identical): a per-frame flag (1 = the CLUB is quiet near the ball, from
// BallSample2D.clubActivity < p1ClubQuietSigma). The grip-stillness test above is
// blind to a club bob about a FROZEN grip (the club rotates while the wrist is
// still), so when the mask is present a frame additionally counts only if its
// whole trailing window is club-quiet. Two-tier fallback mirroring baByFrame: if
// no frame passes still+quiet, the plain grip-still answer (the value returned
// with clubQuiet == nullptr) stands — the mask can only move the hold-end to a
// BETTER (also-quiet) frame, NEVER degrade below today's result.
//
// Optional floorFrame (default -1 ⇒ EVERY existing caller/test is byte-
// identical): the no-return boundary from segmentPhases' onset veto
// (PhaseModel::onsetFloor) — the LAST address settle. On real capture the
// settles floor at 2–4 px/f, so the absolute stillAt thresholds pass NOWHERE
// near the true settle and the walk-back skips straight through the fidget to
// the deep pre-fidget hold (0.5–1.5 s early on 12/17 truth swings even with a
// corrected bs0). With a floor: no frame below floorFrame is ever considered
// (in either tier), and when NOTHING in [floorFrame, bs0] passes, the answer is
// floorFrame itself — the settle the veto proved the track never left for good
// — instead of the legacy deep-hold/bs0 fallback. The floor is only ever set
// when the veto fired (needs shaft.onsetReturnBoxPx > 0), so the dark path is
// untouched.
inline int addressHoldEndFrame(const std::vector<double>& gx, const std::vector<double>& gy,
                               const std::vector<char>& baByFrame, int bs0,
                               const PositionsConfig& cfg,
                               const std::vector<char>* clubQuiet = nullptr,
                               int floorFrame = -1)
{
    const int nf = int(gx.size());
    if (nf < 2 || int(gy.size()) != nf || bs0 <= 0) return bs0;
    const int    last = std::min(bs0, nf - 1);
    const int    w    = std::max(1, cfg.p1StillWindow);
    const double thr  = cfg.p1StillSpeedPx;
    // Walk-back floor (see the contract note above). Safety-clamped to the walk
    // range; loF is the lowest candidate frame either tier may consider.
    if (floorFrame >= 0) floorFrame = std::min(floorFrame, last);
    const int loF = floorFrame >= 0 ? std::max(w, floorFrame) : w;

    // The raw grip is pose-derived (60 Hz hand keypoints lerped to camera rate)
    // with a ~1–2 px noise floor — comparable to the net-drift threshold, which
    // let noise masquerade as motion through a genuinely still hold. Median(5)
    // then box(5) knocks the noise down ~√5 while the directional takeaway
    // creep (the signal) survives untouched.
    auto smooth = [nf](const std::vector<double>& v) {
        std::vector<double> m(v.size());
        for (int i = 0; i < nf; ++i) {
            double win[5];
            int    n = 0;
            for (int j = std::max(0, i - 2); j <= std::min(nf - 1, i + 2); ++j) win[n++] = v[size_t(j)];
            std::sort(win, win + n);
            m[size_t(i)] = win[n / 2];
        }
        std::vector<double> out(v.size());
        for (int i = 0; i < nf; ++i) {
            double s = 0.0;
            int    n = 0;
            for (int j = std::max(0, i - 2); j <= std::min(nf - 1, i + 2); ++j) { s += m[size_t(j)]; ++n; }
            out[size_t(i)] = s / double(n);
        }
        return out;
    };
    const std::vector<double> sx = smooth(gx), sy = smooth(gy);

    auto stillAt = [&](int f) {
        if (f < w) return false;                       // need a full trailing window
        // Net drift over the window (creep is directional; jitter nets out)…
        if (std::hypot(sx[size_t(f)] - sx[size_t(f - w)],
                       sy[size_t(f)] - sy[size_t(f - w)]) >= cfg.p1StillNetPx)
            return false;
        // …plus a per-frame spike cap: an oscillating waggle nets ~0 but is not still.
        for (int i = f - w + 1; i <= f; ++i) {
            const double sp = std::hypot(sx[size_t(i)] - sx[size_t(i - 1)],
                                         sy[size_t(i)] - sy[size_t(i - 1)]);
            if (sp >= thr) return false;
        }
        return true;
    };
    const bool haveBa = !baByFrame.empty() && int(baByFrame.size()) == nf;
    bool anyBa = false;
    if (haveBa)
        for (int f = 0; f <= last && !anyBa; ++f) anyBa = baByFrame[size_t(f)] != 0;

    // The grip-still (+ BA corroboration) answer — the NEVER-DEGRADE fallback.
    // Candidates never drop below loF; when a floor is set and nothing passes,
    // the floor (the last settle) is the answer, not the legacy bs0 fallback.
    auto stillAnswer = [&]() -> int {
        int stillOnly = -1;
        for (int f = last; f >= loF; --f) {
            if (!stillAt(f)) continue;
            if (stillOnly < 0) stillOnly = f;
            if (!anyBa || baByFrame[size_t(f)] != 0) return f;   // corroborated (or no BA to demand)
        }
        if (stillOnly >= 0) return stillOnly;
        return floorFrame >= 0 ? floorFrame : bs0;
    };
    const int legacy = stillAnswer();

    // W3 club-quiet mask (present ⇒ nf-sized). Prefer the last frame that is still
    // AND whose whole trailing window is club-quiet; two-tier like anyBa (prefer a
    // BA-corroborated one). Nothing passing still+quiet ⇒ fall back to `legacy`.
    const bool haveQuiet = clubQuiet && !clubQuiet->empty() && int(clubQuiet->size()) == nf;
    if (!haveQuiet) return legacy;

    auto quietWindow = [&](int f) {
        for (int i = f - w + 1; i <= f; ++i)
            if ((*clubQuiet)[size_t(i)] == 0) return false;   // one non-quiet frame breaks the hold
        return true;
    };
    int stillQuietOnly = -1;
    for (int f = last; f >= loF; --f) {     // same floor as the grip-still tier
        if (!stillAt(f) || !quietWindow(f)) continue;
        if (stillQuietOnly < 0) stillQuietOnly = f;
        if (!anyBa || baByFrame[size_t(f)] != 0) return f;   // still + quiet + (BA if demanded)
    }
    return stillQuietOnly >= 0 ? stillQuietOnly : legacy;    // legacy is already floored
}

// Locate the coaching P-times P1–P8 (plus P10 = Finish) from the per-frame
// reconciled θ(t) and lead-arm φ(t) (both DEGREES, image atan2 convention, one
// entry per frame; NaN permitted, e.g. φ where the pose lead-arm is absent). tUs[]
// is the per-frame time domain. addressFrame/topFrame/impactFrame/finishFrame are
// the tracker's P1/P4/P7/P10 landmark frame indices (segmentPhases bs0/top/impact/
// fin0); each < 0 or out of range simply omits that landmark. P9 and a fuller
// follow-through P-model are DEFERRED — for now P10 is simply the Finish milestone
// so the review timeline carries a marker there (aligned with the Finish station by
// construction, as Finish is never refined). Windows: P2 ∈ (P1,P4), P6 ∈ (P4,P7),
// P8 after P7, P3 ∈ (P2,P4), P5 ∈ (P4,P6); a window whose bounding crossing is
// absent omits its dependent P (P3 needs P2, P5 needs P6). Returns the found P-times
// ORDERED by p (1..8, then 10) and STRICTLY ascending in t_us — P10 (Finish) is
// emitted only when it is genuinely last in time (see the P10 note below), so both
// orderings hold together (the Layer C synthesis t_us-ascending contract depends on it).
inline std::vector<PTime> locatePTimes(const std::vector<int64_t>& tUs,
                                       const std::vector<double>& thetaDeg,
                                       const std::vector<double>& phiDeg,
                                       int addressFrame, int topFrame, int impactFrame,
                                       int finishFrame,
                                       const PositionsConfig& cfg)
{
    using namespace positions_detail;
    std::vector<PTime> out;
    const int nf = int(tUs.size());
    if (nf < 2 || int(thetaDeg.size()) != nf) return out;
    const double hyst = cfg.hysteresisDeg;
    const bool   phiOk = int(phiDeg.size()) == nf;

    auto inRange       = [&](int f) { return f >= 0 && f < nf; };
    auto frameAtOrAfter  = [&](int64_t t) { int i = 0;      while (i < nf && tUs[i] < t) ++i; return i; };
    auto frameAtOrBefore = [&](int64_t t) { int i = nf - 1; while (i > 0  && tUs[i] > t) --i; return i; };

    // P1 — address landmark.
    if (inRange(addressFrame)) out.push_back({1, tUs[addressFrame]});

    // P2 — shaft parallel in (P1, P4).
    Crossing p2;
    if (inRange(addressFrame) && inRange(topFrame) && topFrame > addressFrame)
        p2 = findHorizontalCrossing(tUs, thetaDeg, addressFrame, topFrame, hyst);
    if (p2.found) out.push_back({2, p2.tUs});

    // P3 — lead arm parallel in (P2, P4). Needs P2 and a φ signal.
    if (p2.found && phiOk && inRange(topFrame)) {
        const Crossing p3 = findHorizontalCrossing(tUs, phiDeg, frameAtOrAfter(p2.tUs), topFrame, hyst);
        if (p3.found) out.push_back({3, p3.tUs});
    }

    // P4 — top landmark.
    if (inRange(topFrame)) out.push_back({4, tUs[topFrame]});

    // P6 — shaft parallel in (P4, P7). Located before P5 (bounds its window).
    // p6LastCrossing picks the LAST transit (delivery = the final parallel
    // before impact; the fold's θ≈0 dip just after a shallow top is the first).
    Crossing p6;
    if (inRange(topFrame) && inRange(impactFrame) && impactFrame > topFrame)
        p6 = cfg.p6LastCrossing
                 ? findLastHorizontalCrossing(tUs, thetaDeg, topFrame, impactFrame, hyst)
                 : findHorizontalCrossing(tUs, thetaDeg, topFrame, impactFrame, hyst);

    // P5 — lead arm parallel in (P4, P6). Needs P6 and a φ signal. Pushed BEFORE
    // P6 so the result stays ordered by p.
    if (p6.found && phiOk && inRange(topFrame)) {
        const Crossing p5 = findHorizontalCrossing(tUs, phiDeg, topFrame, frameAtOrBefore(p6.tUs), hyst);
        if (p5.found) out.push_back({5, p5.tUs});
    }
    if (p6.found) out.push_back({6, p6.tUs});

    // P7 — impact landmark.
    if (inRange(impactFrame)) out.push_back({7, tUs[impactFrame]});

    // P8 — shaft parallel after P7.
    if (inRange(impactFrame) && impactFrame < nf - 1) {
        const Crossing p8 = findHorizontalCrossing(tUs, thetaDeg, impactFrame, nf - 1, hyst);
        if (p8.found) out.push_back({8, p8.tUs});
    }

    // P10 — finish landmark. P9 and a full follow-through P-model are deferred; for
    // now P10 is simply the Finish milestone so the timeline shows a marker there.
    // Only emitted when it is genuinely LAST in time: in a short finish the P8
    // follow-through crossing can fall after fin0, and appending an out-of-order P10
    // would break the strictly-t_us-ascending contract Layer C synthesis relies on
    // (shaft_synthesis.h). Omitting it there is safe — P10 would be temporally tangled
    // with P8 anyway.
    if (inRange(finishFrame) && (out.empty() || tUs[finishFrame] > out.back().tUs))
        out.push_back({10, tUs[finishFrame]});
    return out;
}

} // namespace pinpoint::analysis
