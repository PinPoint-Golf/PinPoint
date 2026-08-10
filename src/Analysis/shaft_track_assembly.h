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

#include <QVariantMap>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include <opencv2/core.hpp>

#include "../Core/pp_tuned_constants.h"  // tuned::shaft:: (Stage-A no-return veto + Takeaway defaults)
#include "shaft_tracker_math.h"    // RidgeConfig, BandMatchConfig, BandMatch
#include "swing_analysis.h"        // ShaftTrack2D / ShaftSample2D / ClubLengthEstimate / flags
#include "clubhead_track.h"        // ClubheadConfig (Stage-2 head measurement)
#include "club_length_fusion.h"    // LengthFusionConfig / LengthPriorState / fuseClubLength
#include "shaft_positions.h"       // PositionsConfig / PTime / locatePTimes (Layer B B-time location)
#include "shaft_synthesis.h"       // SynthConfig / synthesizeBetweenAnchors (Layer C synthesis)
#include "shaft_wedge.h"           // WedgeConfig / measureWedge (R8-T1 blur-wedge, S2)
#include "impact_geom.h"           // ImpactGeomConfig / locateImpactGeom (P7 club-at-ball)

// Shaft-tracker v3.0-r1 DECIDING HALF — the physics/statistics that turn
// per-frame evidence into one globally-consistent shaft-angle track. Faithful
// C++ port of tools/shaftlab/club_track_v3.py (constraint-DP over a 1° θ grid +
// ψ-isotonic reconciliation). Pure over plain vectors — no SwingWindow, no
// decode — so every stage is unit-testable against the Python per-stage dumps.
// The window/decode orchestration lives in ShaftTracker (shaft_tracker.h); the
// evidence engines E1/E2 in shaft_tracker_math.h.
//
// The four fundamentals are first-class constraints (design
// docs/design/club_tracking_v3_design.md):
//   C1 butt-termination  · C2 body-overlap veto (GEOMETRIC half-plane, default)
//   C3 phase-signed rot (banded Viterbi)  · C4 reachable cone / ψ-isotonic rail
//
// Porting invariants (docs/design/club_track_v3_exemplar_explained.md §15):
//   emission → DP → tiers evaluation order is fixed; the band negative well is
//   applied LAST; span-bound still emits pred rows for held frames; PAVA is
//   exact O(n) and deterministic.

namespace pinpoint::analysis {

// Hands-only C3 phase labels (order mirrors club_track_v3.py).
enum class SwingPhase : uint8_t {
    Addr, Backswing, Top, Impact, Downswing, Thru, Finish
};

// Layer A line re-registration («snap»), shaft_position_first design §2 Layer A.
// After PASS 2 places each sample, refine the drawn line's (⊥offset, Δθ) against
// the local ridge line-integral so it lands ON the club, and record lineConf.
// enabled=false ⇒ the pass is skipped and every emitted byte is identical to the
// pre-snap tracker (soak contract). "shaft.snap.*" keys via fromOverrides.
struct SnapConfig {
    bool   enabled        = false;  // master gate — dark until the A2 corpus gate flips it
    double maxOffsetPx    = 15.0;   // perpendicular search half-range (px)
    double maxDeltaDeg    = 3.0;    // angular search half-range (deg)
    double minLineConf    = 0.25;   // accept-snap floor on the ridge support under the line
    int    corridorHalfPx = 2;      // lateral half-width integrated along the candidate line (px)
};

// Hand-axis θ prior (WB4, wholebody_pose_design.md §2.2). The grip's hand axis
// (wrist-root → middle-MCP, averaged over confident hands) approximates the
// shaft direction at the grip — a per-frame, band-independent DP prior. Adds a
// cost to states whose θ deviates > maxDeg from the hand axis, scaled by weight ×
// per-frame hand confidence, ONLY on frames where the hand-axis endpoints clear
// confMin. enabled=false ⇒ the whole term is skipped (not a zero weight) so the
// emission cost tables are BIT-IDENTICAL to a pre-WB4 run. "shaft.handAxisPrior.*"
// keys via fromOverrides.
struct HandAxisPriorConfig {
    bool   enabled = false;   // master gate — dark until its own corpus gate flips it
    double weight  = 6.0;     // cost per off-axis state (× hand conf), DP cost units
    double confMin = 0.30;    // per-endpoint conf gate (codebase-wide convention)
    double maxDeg  = 35.0;    // states beyond this from the hand axis are penalised
};

// The full v3.0-r1 parameter set. Every field defaults to the validated Python
// constant; fromOverrides() applies "shaft.<name>" keys from a tuning map so
// SwingLab sweeps iterate at binary speed. Geometric-C2 and span-bounding are
// the shipping defaults (rasterC2=false, spanBound=true) — commits 8e2b382 +
// dfa3170. The --raster-c2 / --no-span-bound Python oracles map to rasterC2 /
// !spanBound and exist only for parity cross-checks.
struct ShaftV3Config {
    // grid / DP
    double  grid          = 1.0;   // θ grid step (deg); NS = round(360/grid)
    double  wmaxAddr      = 3.0;   // per-phase |Δθ| ceiling (deg/frame)
    double  wmaxBackswing = 9.0;
    double  wmaxTop       = 5.0;
    double  wmaxDownswing = 16.0;
    double  wmaxImpact    = 24.0;
    double  wmaxThru      = 16.0;
    double  wmaxFinish    = 11.0;
    // cost weights (DP minimises)
    double  wE2      = 10.0;   // emission span: 0 (best) .. wE2 (none)
    double  wBand    = 8.0;    // band-lock negative-emission well
    double  wArm     = 16.0;   // C4 into-forearm veto
    double  wC1      = 10.0;   // C1 off-arm reverse-support
    double  wC2      = 13.0;   // C2 body-overlap veto
    double  wCone    = 4.0;    // C4 wide-cone soft penalty
    double  kSmooth  = 0.03;   // transition θ-smoothness (per deg²)
    double  coneHalf = 150.0;  // C4 wide half-cone about φ (deg)
    double  c1Tol    = 0.45;   // reverse-ridge strength above which C1 bites
    double  rayEvMin = 0.45;   // normalised E2 evidence to call a frame 'ray'
    // Evidence honesty (S1). normScores() rescales EVERY frame's ridge row into
    // [0,1] against its own p50/p97, so a blur-flat frame with no line in it
    // still mints a full-strength "winner" — the percentile normalisation cannot
    // tell noise from a shaft. evAbsFloor is the absolute, pre-normalisation
    // statement that a channel saw a line at all: a frame channel whose RAW
    // ridge p97 misses the floor contributes zero normalised scores AND zero
    // support for that frame (it can still be carried by the other channel).
    // <= 0 disables the whole test (dark), collapsing to the pre-S1 expressions.
    // FLIPPED ON 2026-08-10 (S2 gate, brief §"Re-validation": 11/11 truth P6
    // ≤5 ms, dark path proven byte-identical): 100 is the deep-blur/low-light
    // safety net from the S1 calibration — inert on the blessed corpus (real
    // frames' raw p97 sits 210+), it only drowns a channel that saw nothing.
    double  evAbsFloor = 100.0;   // raw-p97 floor per channel (score units); <=0 = off
    double  evAbsFloorDif = -1.0; // dif-channel override; < 0 ⇒ use evAbsFloor
    // Absolute RidgeResult.support (fraction of supported samples along the ray)
    // required at the DP's θ before a frame may claim the RAY tier. Independent
    // of evAbsFloor: the floor asks whether the channel saw anything, this asks
    // whether the WINNING direction is actually a continuous line. <= 0 = off.
    // FLIPPED ON 2026-08-10 at the S1 arm-A value (0.5 demoted 2 more tracks
    // invalid and silenced a P2-truth swing — rejected).
    double  raySupportMin = 0.4;
    double  bandTol  = 6.0;    // |θ* − θ_band| (deg) to claim the band tier
    double  armVetoDeg = 12.0; // ARM_VETO_DEG: no lock within this of grip→arm
    // static-hold demotion
    double  stillSpeed = 0.8;  // grip px/frame below which a frame is 'still'
    int     stillMin   = 25;   // min static-run length (frames)
    int     bandNear   = 5;    // a static ray is admissible within N frames of a band
    // swing-span bounding
    int64_t spanCollarUs = 100000;   // settling padding on the FINISH side (fin0+collar) (µs)
    // Address-side collar: a symmetric measurement pad in front of the true
    // takeaway (bs0), mirroring the finish-side spanCollarUs. Was 400000
    // (2026-07-08) to paper over the grip-speed bs0 lag — the club rotates
    // about the wrist while the grip is still, so the high-threshold detector
    // landed mid-takeaway and the collar reached back to recover it. The Stage
    // A onset walk-back (swLow/phiOnset) now fixes that lag AT SOURCE, so bs0
    // is the real takeaway and the collar is back to a small settling pad.
    int64_t addressCollarUs = 100000;
    bool    spanBound    = true;     // DEFAULT ON (dfa3170); false = --no-span-bound oracle
    // Stage A true-onset segmentation (swing_span_bounding_plan.md §4). The
    // high-swSpd run detection still FINDS the swing; onset then walks bs0 back
    // to the real takeaway. swLow <= 0 disables A1+A2+A3 entirely and
    // reproduces the pre-Stage-A behaviour bit-for-bit (the Python v3.0-r1
    // oracle predates onset v2 — its parity fixture pins swLow=0).
    double  swLow             = 1.5;      // A1 hysteresis low threshold (px/frame walk-back stop)
    double  phiOnsetDegPerFrame = 0.25;   // A2 φ-witness sustained-motion threshold (0 = φ witness off)
    int64_t bsMinBeforeImpactUs = 550000; // A3 onset clamp, near edge (impact − this)
    int64_t bsMaxBeforeImpactUs = 1600000;// A3 onset clamp, far edge (impact − this)
    // Stage A "no-return" veto (fidget-proofing, defaults in pp_tuned_constants.h
    // shaft::). On real capture the lerped-pose grip keeps a 2–4 px/f smoothed
    // speed floor through every fidget settle, so A1 walks the onset back through
    // the WHOLE fidget to the deep pre-fidget stillness (and A2's φ witness walks
    // further still) — 0.5–1.5 s early against the truth annotations. The veto
    // runs AFTER A1/A2 and BEFORE the A3 clamp and can only move the onset
    // LATER, via a DEPARTURE-REFERENCED revisit scan (no address anchor, no
    // absolute rest gate — both premises were unsatisfiable on real capture: the
    // golfer settles into an address DISPLACED from the pre-fidget stance, and
    // true grip rest never happens):
    //   revisit(r) = min dist(gS[f], gS[r]) over f ∈ [r + onsetReturnGapFrames,
    //   bs0], gS = gauss(median5(grip)); the veto takes the LAST r in
    //   [onset, bs0 − gap] with revisit(r) < onsetReturnBoxPx — the last instant
    //   the track ever comes back to. Every fidget settle (and waggle burst) is
    //   revisited by the next excursion's return; the takeaway departs for good,
    //   so the boundary lands at the final settle. The scan horizon is bs0 — the
    //   selected (post-bridging) run start, i.e. the takeaway-run candidate — so
    //   the revisit test never sees the top dwell (which revisits itself);
    //   onsetRunBridgeFrames is what makes bs0 the true takeaway run on
    //   fragmented backswings, and the A3 clamp remains the backstop for an
    //   unbridged mis-pick (where the walked-back onset also sits near bs0, so
    //   the gap guard usually empties the scan anyway).
    // FROZEN ON 2026-07-17 (box 7 / gap 15; 17-swing truth: Address-error median
    // 0.564 → 0.060 s); onsetReturnBoxPx <= 0 still disables the whole block
    // (the swLow<=0 dark idiom) — byte-identical to the pre-veto onset. Box/gap
    // coupling: a slow one-piece creep advances creepSpeed×gap px per exclusion
    // window, so the box must stay below that to not clip it (box 7 / gap 15
    // tolerates creep ≥ ~0.6 px/f).
    // Side effect (gated): estimateSwingSpanUs shares segmentPhases, so the veto
    // also tightens the pose/shaft span bound on fidget swings.
    double  onsetReturnBoxPx     = tuned::shaft::kOnsetReturnBoxPx;     // revisit radius (px); 0 = veto off
    int     onsetReturnGapFrames = tuned::shaft::kOnsetReturnGapFrames; // forward exclusion before a revisit counts
    // Run bridging (C): merge >swSpd runs separated by fewer than this many
    // quiet frames BEFORE the min-length filter and the two-longest ranking. A
    // slow real backswing fragments into short bursts on the lerped-pose speed
    // profile and loses the two-longest race to follow-through fragments — bs0
    // then lands at the top/downswing. FROZEN ON 2026-07-17 at 10; 0 = off
    // (the legacy ranking, byte-identical). Separate key so its effect stays
    // separable from the veto in A/Bs.
    int     onsetRunBridgeFrames = tuned::shaft::kOnsetRunBridgeFrames;
    // m3gate (FROZEN ON 2026-07-18 at 0.2; 0 = off, the legacy ranking): a
    // bridged run assembled from >= 3 raw runs enters the two-longest ranking
    // only if its smoothed net displacement >= this fraction of its raw path
    // length. Freeze evidence: 17-swing truth — s0002 Takeaway +0.100, s0001
    // Address +0.042, other 15 zero-movement; corpus 19 corrective moves, 0
    // score changes. Kills the
    // s0002-class mis-pick: grip-anchor pose FLAPPING during a presentation
    // move produced seven 7–8-frame >swSpd oscillation runs (gaps 4–6) that
    // bridged into a 79-frame chain going nowhere (net/path 0.013), tied the
    // downswing for two-longest, and parked bs0 in the flap (A3 far-edge pin
    // at impact − bsMax, Takeaway −0.52 s). The >= 3-chain qualifier is a
    // FIXED structural rule, deliberately not a knob: m = 2 merges are the
    // frozen w2s4 evidence — the fragmented-backswing rescue AND the
    // downswing+follow-through reversal merge (legitimately low net, ratio
    // 0.08) — and are permanently exempt. Separation: flap 0.013 vs >= 0.34
    // for every legitimate chain (25×); degenerate fallback re-ranks the
    // ungated list if the gate would empty it. NOTE (recorded decision,
    // 2026-07-18): the gate fixes s0002's TAKEAWAY (+0.100 s); its ADDRESS
    // residual (−0.58 s, hold-end reverts to the deep pre-flap hold because
    // the flap corrupts every stillness window) is ACCEPTED pending the
    // upstream grip-anchor flapping fix — once the pose anchor is stable the
    // flap runs disappear and the frozen revisit/floor machinery handles the
    // swing natively.
    double  onsetBridgeMinNetFrac = tuned::shaft::kOnsetBridgeMinNetFrac;
    // Additive vision Takeaway event (phasesToSegmentation, W2). FROZEN ON
    // 2026-07-17: the ladder is {Address,Takeaway,Top,Impact,Finish}; false
    // drops the Takeaway event (the pre-freeze four-event ladder). Separate key
    // from the veto above (disjoint failure modes: onset PLACEMENT vs event-SET
    // change).
    bool    emitTakeaway         = tuned::shaft::kEmitTakeaway;
    // Run-candidacy clamp (same supplied-impact guard as A3): a motion run that
    // STARTS later than impact + this cannot be a swing phase, so it must not
    // compete with a fragmented backswing for the two-longest-runs slots (post-
    // finish fidgeting at the window tail was seen winning fin0, collapsing the
    // coverage span — 0709_swing_0004 escalation). 0 disables; inert when no
    // impact frame is supplied (hands-derived impf must not filter its inputs).
    int64_t runMaxStartAfterImpactUs = 1000000;
    // C2 body ROI
    double  bodyMargin = 34.0;       // px inflation of the body polygon
    double  bodyRLo    = 45.0;       // ray-sample radii for the inside-fraction test
    double  bodyRHi    = 470.0;
    double  bodyRStep  = 14.0;
    bool    rasterC2   = false;      // DEFAULT geometric (8e2b382); true = --raster-c2 oracle
    // ψ-isotonic reconciliation (v3.0-r1)
    bool    psiRail      = true;     // false = --no-psi-rail (v3.0 wide-cone only)
    double  armOutlierDeg = 20.0;    // smooth_phi Hampel gate (deg/frame)
    double  wIsoBand     = 8.0;      // isotonic fit weight by tier
    double  wIsoRay      = 2.0;
    double  wIsoPred     = 0.3;
    double  isoHuber     = 8.0;      // Huber knee (deg)
    int     isoIters     = 3;        // IRLS reweight iterations
    double  reconTol     = 6.0;      // θ move beyond this ⇒ retier 'recon'
    int     psiWinBack   = 3;        // ψ-reversal free window before top
    int     psiWinFwd    = 12;       // and after (release lag)
    // hands-only segmentation
    double  swSpd     = 8.0;         // grip speed (px/f) threshold for the swing
    int     impHalf   = 12;          // impact-zone half-width (frames)
    // validity gate
    double  coverageMin = 0.60;      // meas fraction over the span ⇒ track.valid
    // length-ladder pose-scale rung (A2, clubhead_length plan). When neither the
    // ball nor a retro-band gives a measured club length, the still shoulder-mid→
    // ankle-mid pixel extent is a stature surrogate: px/m = extent / (0.83 ×
    // lenStatureM); projected length = px/m × (clubLengthM − lenGripDownM). The
    // untaped default — no measured pixel scale exists otherwise.
    double  lenStatureM  = 1.70;     // assumed golfer stature (m)
    double  lenGripDownM = 0.13;     // grip-down-from-butt so the club starts at the hands, not the butt
    // evidence engine sub-configs
    RidgeConfig     ridge;
    BandMatchConfig band;
    // Stage-2 measured-clubhead config (Phase B, clubhead_track.h). Defaults to
    // enabled=false (dark at merge). NOTE: ShaftV3Config::fromOverrides (in
    // shaft_track_assembly.cpp) does NOT populate this — the B3 wiring agent adds
    // `head = ClubheadConfig::fromOverrides(ov);` there so "shaft.head.*" keys
    // apply. Until then head keeps its validated-constant defaults.
    ClubheadConfig  head;
    // Multi-estimator club-length fusion (club_length_fusion.h): "fusion.*" keys.
    // fromOverrides populates it (`fusion = LengthFusionConfig::fromOverrides(ov)`).
    // enabled=true by default (feeds the length ladder + Stage-2 search); the
    // no-regression gate runs with "fusion.enabled" = 0, which reproduces today's
    // ladder byte-for-byte.
    LengthFusionConfig fusion;
    // Layer A line re-registration («snap») — "shaft.snap.*" keys. enabled=false
    // by default (dark at merge); fromOverrides populates it.
    SnapConfig      snap;
    // Layer B P-position extraction (shaft_position_first §2 Layer B) —
    // "positions.*" keys. enabled=false by default (dark); when on, decideTrack
    // locates P1–P8 and fills ShaftTrack2D.positions from the emitted track
    // (report-only in B1). fromOverrides populates it.
    PositionsConfig positions;
    // Layer C synthesis between anchors (shaft_position_first §2 Layer C) —
    // "synth.*" keys. enabled=false by default (dark); when on, decideTrack fills
    // ShaftTrack2D.synth with the VISUALIZATION-tier interpolated series between the
    // located P-anchors. fromOverrides populates it.
    SynthConfig     synth;
    // Hand-axis θ prior (WB4) — "shaft.handAxisPrior.*" keys. enabled=false by
    // default (dark); when on, the per-frame hand-axis direction penalises the DP
    // states far from it near the grip. fromOverrides populates it.
    HandAxisPriorConfig handAxisPrior;
    // R8-T1 blur-wedge (S2, shaft_wedge.h) — "shaft.wedge.*" keys. enabled=false
    // by default (dark); when on, frames whose R6-predicted club rate clears
    // wedge.omegaMinDegS get a proximal fan sweep inside the kinematic envelope,
    // an emission well at the measured centroid (pre-DP, clamped at −wBand), the
    // WEDGE tier, and (wedge.kinCone) an off-envelope penalty. fromOverrides
    // populates it. The wedge threshold scales from evAbsFloor (S1) — with the
    // floor unset the wedge measures nothing (never fabricate).
    WedgeConfig     wedge;
    // P7 impact from club-at-ball geometry (impact_geom.h) — "shaft.impactGeom.*"
    // keys. enabled=false by default (dark); when on, the theta==theta_ball
    // crossing arbitrates the acoustic anchor's frame mapping (a coverage gap
    // around impact lands the nearest-frame mapping 234-362 ms late on 3 truth
    // swings and poisons the P6 window), and (impactGeom.retime) retimes the
    // corroborated P7/Impact instant sub-frame. fromOverrides populates it.
    // Gated on the A1 address-ball cluster resolving — data, never sessionType.
    ImpactGeomConfig impactGeom;

    static ShaftV3Config fromOverrides(const QVariantMap& ov);
};

// ── hands-only phase model (C3) ──────────────────────────────────────────────
struct PhaseModel {
    std::vector<SwingPhase> phase;      // per frame
    int  bs0    = 0;   // takeaway start
    int  top    = 0;   // transition
    int  impact = 0;   // impact frame
    int  fin0   = 0;   // finish begins
    // No-return boundary from the onset veto (-1 = veto dark / never fired).
    // The revisit scan's boundary IS the last address settle, so downstream
    // walk-backs (addressHoldEndFrame) must never walk below it — the settles
    // floor at 2–4 px/f on real capture, so an absolute-stillness walk-back
    // otherwise skips straight through the fidget to the deep pre-fidget hold.
    // A3 interaction: floor = min(boundary, final bs0) — when the A3 clamp
    // pushes the onset EARLIER than the boundary, the floor follows the onset
    // (a floor above the walk-back start would be unreachable); when A3 pushes
    // it later, the boundary (the settle) still stands as the floor.
    int  onsetFloor = -1;
    std::vector<double> spdSmoothed;    // smoothed grip speed (px/frame)
};

// Segment the swing from the hands alone. impactFrame < 0 ⇒ derive it (first
// post-top grip-return-to-address-height frame). Degenerate (no swing) ⇒ whole
// clip 'addr', bs0=0, fin0=nf-1 (safe: full processing).
//
// Stage A true-onset (swing_span_bounding_plan.md §4): after the high-swSpd run
// picks the swing, bs0 walks back to the real takeaway — to the first frame
// below cfg.swLow (A1) and, when phiSmoothed is supplied (may be null), the
// first sustained |Δφ|/f above cfg.phiOnsetDegPerFrame (A2); onset = min of the
// two. When impactFrame >= 0 the onset is clamped into [impact − bsMax, impact
// − bsMin] frames (A3). cfg.swLow <= 0 disables A1+A2+A3 (legacy behaviour).
PhaseModel segmentPhases(const std::vector<double>& gx, const std::vector<double>& gy,
                         int nf, double fps, int impactFrame, const ShaftV3Config& cfg,
                         const std::vector<double>* phiSmoothed = nullptr);

// Robust unit-vector smoothing of the lead-arm direction (deg), wrap-aware.
// Hampel-rejects physically-impossible jumps (> armOutlierDeg) then median +
// Gaussian. Idempotent on clean φ.
std::vector<double> smoothPhi(const std::vector<double>& phiDeg, const ShaftV3Config& cfg);

// Temporal median(5)+Gaussian(2) smoothing of per-frame joints, per joint.
// rawJoints[f] = the frame's joints (px); all frames must share the joint count.
std::vector<std::vector<cv::Point2d>>
smoothJoints(const std::vector<std::vector<cv::Point2d>>& rawJoints);

// C2 body ROI as per-frame convex-hull half-planes (outward normals + offsets).
// A point p is inside the body inflated by bodyMargin iff
// max_i(n_i·p − d_i) ≤ bodyMargin. Empty input ⇒ empty result (C2 disabled).
struct BodyPoly {
    std::vector<cv::Vec2d> n;   // outward unit normals, one per hull edge
    std::vector<double>    d;   // offsets d_i = n_i · vertex_i
};
std::vector<BodyPoly> bodyPolys(const std::vector<std::vector<cv::Point2d>>& smoothedJoints);

// C2 body ROI as the ORIGINAL rasterised+dilated mask (uint8 HxW per frame) —
// the --raster-c2 byte-oracle. Empty input ⇒ empty result.
std::vector<cv::Mat> bodyMasks(const std::vector<std::vector<cv::Point2d>>& smoothedJoints,
                               int W, int H, const ShaftV3Config& cfg);

// Sustained low-speed runs → static[] (runs ≥ stillMin below stillSpeed).
std::vector<char> staticRuns(const std::vector<double>& spdSmoothed, const ShaftV3Config& cfg);

// ── per-frame DP emission (C1/C2/C4 + band well) ─────────────────────────────
// Build one frame's emission cost row (length NS) and its C2 inside-fraction
// row. evMax/rawNorm are the pre-band normalised E2 evidence (length NS).
// `poly` may be null (geometric C2 skipped); `mask` non-empty selects the
// raster form. Applies, in order: raise ev at the band bin, em = wE2·(1−ev),
// C4 arm-veto, C4 wide cone (off addr/finish/top), C1 reverse-ridge, C2 veto
// (mid-swing), the WB4 hand-axis prior, then the band negative well em[bi] =
// −wBand LAST. handAxisDeg/handAxisConf are this frame's grip hand-axis direction
// (deg, image atan2 convention) + confidence; the prior term is guarded ENTIRELY
// on cfg.handAxisPrior.enabled (NaN handAxisDeg / conf 0 also skip it), so with
// the default-off config the cost row is bit-identical to a pre-WB4 run.
void frameEmission(std::vector<float>& emOut, std::vector<float>& insideOut,
                   const std::vector<float>& evMax, const std::vector<float>& rawNorm,
                   const BandMatch& band, double phiSDeg, SwingPhase phase, int chir,
                   double gx, double gy, const BodyPoly* poly, const cv::Mat& mask,
                   const std::vector<float>& gridRad, const std::vector<float>& gridDeg,
                   const ShaftV3Config& cfg,
                   double handAxisDeg = std::numeric_limits<double>::quiet_NaN(),
                   double handAxisConf = 0.0);

// WB4 hand-axis θ prior — add `weight × handAxisConf` to every state deviating
// more than maxDeg from handAxisDeg. Pure (deg wrap via the file-local circWrap);
// a no-op when cfg.enabled is false, handAxisConf < cfg.confMin, or handAxisDeg
// is NaN — in every such case emOut is left BIT-IDENTICAL. Applied BEFORE the
// band negative well so the well still dominates. Unit-tested standalone.
void addHandAxisPrior(std::vector<float>& emOut, const std::vector<float>& gridDeg,
                      double handAxisDeg, double handAxisConf, const HandAxisPriorConfig& cfg);

// ── global banded Viterbi DP over the θ grid (C3) ────────────────────────────
// emis: nf rows × NS cols. Returns the per-frame grid index (thstar) and its
// θ in degrees. Banded, sign-restricted transitions per phase; quadratic
// θ-smoothness; wrap via modular shift.
struct DPResult {
    std::vector<int>    thstar;   // per-frame grid index
    std::vector<double> thetaDeg; // per-frame θ (deg)
};
DPResult viterbiDP(const std::vector<std::vector<float>>& emis,
                   const std::vector<SwingPhase>& phase, const ShaftV3Config& cfg);

// ── ψ-isotonic reconciliation ────────────────────────────────────────────────
// Exact O(n) weighted Pool-Adjacent-Violators; deterministic.
std::vector<double> pava(const std::vector<double>& y, const std::vector<double>& w,
                         bool increasing);
// Huber-IRLS isotonic fit (down-weights residuals beyond isoHuber).
std::vector<double> robustIsotonic(const std::vector<double>& y, const std::vector<double>& w,
                                   bool increasing, const ShaftV3Config& cfg);

// Treat monotone ψ = θ − φ as truth and fit it per block (backswing increasing,
// downswing+impact+thru decreasing), excluding the top window. In RECON_PHASES
// (impact) non-band θ is reconstructed as ψ_iso + φ (arm witness); elsewhere θ
// is kept and the residual recorded. tierHint(f) ∈ {band,ray,pred} weights the
// fit. Returns thetaOut / psiResid (NaN where unfit) / recon flags.
struct ReconResult {
    std::vector<double> thetaOut;
    std::vector<double> psiResid;
    std::vector<char>   recon;
};
// bandOk[f]: whether frame f has a band lock. tierBand/tierRay/tierPred weights
// chosen per frame from evAt vs rayEvMin (see the .cpp). top is the transition
// frame; nan-θ frames are skipped.
ReconResult reconcilePsi(const std::vector<double>& thetaDeg,
                         const std::vector<double>& phiSmoothed,
                         const std::vector<SwingPhase>& phase,
                         const std::vector<char>& bandOk,
                         const std::vector<double>& evAt, int top, int nf,
                         const ShaftV3Config& cfg);

// ── SwingWindow-free decide core (shared by the live tracker + the parity
//    harness) ────────────────────────────────────────────────────────────────
// Diagnostics filled when a sink is passed to decideTrack (SwingLab only).
struct ShaftDecideTrace {
    PhaseModel          phases;
    std::vector<double> phiSmoothed;
    int                 chir = 1;
    int                 spanLo = 0, spanHi = 0, heavyFrames = 0;
    DPResult            dp;
    ReconResult         recon;
    std::vector<int>    frameIdx;   // one entry per emitted (anchored) frame
    std::vector<int>    tier;       // 0 pred / 1 ray / 2 band / 3 recon / 4 wedge
    std::vector<double> thetaDeg;   // reconciled θ (deg)
    std::vector<float>  conf;
    // Vision-only phase landmarks (hands-only C3 model → app Segmentation),
    // conf 0 when no swing was detected. Lets a camera-only (no-IMU) analysis
    // surface Address/Top/Impact/Finish markers the tracker already computes.
    Segmentation        segmentation;
    // v3.4 ball anchor (design §9.2): the frame the clubhead's theta first
    // departs theta_ball beyond a threshold — computed and logged ONLY, never
    // consumed by the DP/phase model above (that relabel is deferred to the
    // v3.0-r1 corpus re-gate). -1 = no ball data / not computed.
    int                 ballTk0Frame = -1;
    // A2 length ladder (clubhead_length plan): which rung set the projected
    // grip→head length for coasted/predicted heads, and its final clamped px
    // value. rung 1 ball-measured, 2 band-corrected, 3 pose-scale, 4 frame-
    // height fallback; 0 = not computed. Head/length diagnostics only — never
    // fed back into the θ path.
    int                 projLenRung = 0;
    double              projLenPx   = 0.0;
    // A2b club-length fusion (club_length_fusion.h) — SwingLab triage of the
    // fused length + confidence. fuseFusedPx/Conf include the persistent prior
    // (the RECORDED length); fuseInstantPx/Conf are prior-free (drive the prior
    // update); fuseHeadPx = the E-head p95 that entered (−1 = none); fuseNEst =
    // surviving instantaneous estimators; fusePriorN = prior support; fuseSpread
    // = relative survivor spread. Head/length diagnostics only — never θ.
    double              fuseFusedPx     = -1.0;
    double              fuseFusedConf   = 0.0;
    double              fuseInstantPx   = -1.0;
    double              fuseInstantConf = 0.0;
    double              fuseHeadPx      = -1.0;
    int                 fuseNEst        = 0;
    int                 fusePriorN      = 0;
    double              fuseSpread      = 0.0;
    // A1 golf-prior plausibility gate on the ball length measurement (gateA-
    // 0704: consistent driver-head lock above the ankle line shorted rung 1).
    // 0 accepted / not gated, 1 rejected at/above the ankle line, 2 rejected
    // outside the between-the-feet corridor.
    int                 lPxRejected = 0;
    // Stage-2 measured-clubhead diagnostics (Phase B), per frame [0,nf). headTier
    // = HeadTier (0 off / 1 pred / 2 meas); headR = the temporal-model radial
    // estimate (px, NaN = none); headZ = the raw per-frame measured radius (px,
    // NaN = no terminus). Empty unless cfg.head.enabled && sceneMed non-empty.
    // headMs = head-pass wall-clock (ms, 0 = not run). SwingLab triage only.
    std::vector<int>    headTier;
    std::vector<double> headR, headZ;
    double              headMs = 0.0;
    // Layer A snap pass (shaft_position_first): SwingLab triage of the line
    // re-registration. snapAppliedN = accepted snaps; medianSnapOffsetPx = median
    // |⊥ offset| applied over accepted snaps; medianLineConf = median lineConf over
    // all vision-tier samples measured (accepted or not). All −1 unless a trace
    // sink is present AND cfg.snap.enabled (the pass ran).
    int                 snapAppliedN      = -1;
    double              medianSnapOffsetPx = -1;
    double              medianLineConf     = -1;
    // S1 calibration channels, per frame [0,nf) — the raw (PRE-floor, pre-
    // normalisation) ridge p97 of each evidence channel and the ridge support at
    // the DP's chosen θ. These are what evAbsFloor/raySupportMin get set from:
    // histogram genuine-RAY backswing frames against mid-downswing blur frames
    // and read the separation off. −1 = the channel/frame did not run. Empty
    // unless a trace sink is present; diagnostics only, never read back.
    std::vector<double> rawP97, difP97, supAtDp;
    // S2 wedge diagnostics, per frame [0,nf): the R6-predicted club rate, and
    // the measured fan centroid/width (NaN = frame not triggered / no
    // candidate). wedgeTExpS = the calibrated exposure estimate (s; −1 = wedge
    // dark). Empty unless a trace sink is present AND cfg.wedge.enabled.
    std::vector<double> wedgeOmegaDegS, wedgeCentroidDeg, wedgeWidthDeg;
    double              wedgeTExpS = -1.0;
    // P7 impact-geometry diagnostics (shaft.impactGeom.*): the located
    // theta==theta_ball crossing (-1 = dark / no address ball / not found),
    // its at-or-before frame, and what the decision did (impact_geom.h codes:
    // 0 kept-anchor/abstain, 1 override, 2 no-anchor adopt, 3 sub-frame
    // retime). Diagnostics only, never read back.
    int64_t             impactGeomTUs     = -1;
    int                 impactGeomFrame   = -1;
    int                 impactGeomApplied = 0;
};

// Map the hands-only phase model to an app Segmentation with real timestamps:
// Address/Top(top)/Impact(impact)/Finish(fin0) events + swing bounds, all at the
// given (vision-grade) confidence. conf 0 ⇒ "bounds are just the window".
// addressFrame >= 0 relocates the Address EVENT to the located hold end
// (camera-first P1 fix); < 0 keeps the legacy bs0 (takeaway start). Swing
// bounds always derive from bs0 (motion onset is what the span bound wants).
// emitTakeaway = true additionally emits a Takeaway event at bs0 (the motion
// onset, always ≥ the Address hold end); false ⇒ the {Address,Top,Impact,Finish}
// ladder is byte-identical to the pre-W2 tracker (W2, cfg.emitTakeaway).
Segmentation phasesToSegmentation(const PhaseModel& pm, const std::vector<int64_t>& tUs, float conf,
                                  int addressFrame = -1, bool emitTakeaway = false);

// Swing span [onset t, fin0 t] in µs for the Stage B two-pass pose bound
// (swing_span_bounding_plan.md §5). Runs segmentPhases over the coarse grip
// track (impactFrame = nearest frame to impactUs, or -1 when impactUs < 0) and
// returns the true-takeaway → finish window. ok = false on the degenerate
// no-run path (whole-clip address: bs0 == 0 && fin0 == nf-1), where the
// caller must fall back to the full window. Pure over plain vectors so Stage B
// can call it before any dense pose pass exists.
struct SwingSpanEstimate {
    int64_t startUs = 0;
    int64_t endUs   = 0;
    bool    ok      = false;
};
SwingSpanEstimate estimateSwingSpanUs(const std::vector<double>& gx, const std::vector<double>& gy,
                                      const std::vector<int64_t>& tUs, double fps,
                                      int64_t impactUs, const ShaftV3Config& cfg);

// Returns CV_8UC1 grey for coverage-frame index i (0..nf-1), or empty when the
// frame is undecodable. Called at stride 8 for the scene-background model and
// once per swing-span frame for evidence.
using FrameSource = std::function<cv::Mat(int)>;

// A2 length-ladder resolver — the projected grip→head length (px) for coasted/
// predicted heads, picked from the first available scale source then clamped to
// plausibility. Pure so the precedence + clamps are unit-testable without a
// frame source. rung ∈ {1 ball-measured L_px, 2 band-corrected sTypical·(clubLenMm
// −r0Med), 3 pose-scale surrogate, 4 0.45·frameH fallback}; the first with a
// positive input wins. Clamps: floor = armFloorPx (1.05× still shoulder→grip —
// a club is always longer than the lead arm, when armFloorPx>0); ceiling =
// 1.1·measuredClubLenPx on rung 1 else 0.62·frameH (keeps a bad scale on-frame).
// measuredClubLenPx ≤ 0 / sTypical ≤ 0 / poseExtentPx ≤ 0 mean "that source
// absent". Never touches θ.
double projectedClubLenPx(double measuredClubLenPx, double sTypical, double r0Med,
                          double poseExtentPx, double armFloorPx,
                          double clubLenMm, double frameH, const ShaftV3Config& cfg,
                          int& rung);

// The whole club_track_v3 decide pipeline over pre-derived per-frame inputs:
// interpolate/smooth φ → hands-only phase model → C2 geometry → span-bounded
// E1+E2 evidence → emission → banded Viterbi → ψ-reconcile → tiering. Vision-
// only. gx/gy/phiRaw/rawJoints/tUs are one entry per coverage frame (phiRaw may
// hold NaN; it is gap-filled here). impactFrame < 0 ⇒ derive it hands-only.
// bandsMm empty ⇒ E1 disabled (ray-only). Sets out.samples/valid/coverage/
// frameWidth/Height; the caller sets out.camera.
//
// `ball` (may be null; null == no ball data) is read ONLY to measure
// out.measuredClubLenPx over the address window before head placement (A1) and
// to seed rung 1 of the length ladder — it never enters segmentPhases/emission/
// DP/reconcile, so θ is identical with or without it (the head/length half is
// strictly decoupled from the corpus-validated θ path).
//
// `lengthPrior` (may be null; null == no persistent prior) joins E-prior into the
// club-length fusion when cfg.fusion.enabled && its n ≥ 2. Read-only here — the
// prior UPDATE (from out.lengths.fusedInstant*) happens in the caller after the
// swing is analysed. Defaulted so the parity/decide harnesses + swinglab_run
// compile unchanged. With cfg.fusion.enabled == false the fusion is skipped
// entirely and the length ladder is byte-identical to the pre-fusion tracker.
// `handAxisDeg`/`handAxisConf` (WB4, may be empty) are the per-coverage-frame
// grip hand-axis direction (deg, image atan2) + confidence, fed to the DP
// hand-axis prior (cfg.handAxisPrior). Empty ⇒ the prior contributes nothing;
// with cfg.handAxisPrior.enabled false the term is skipped regardless, so the
// θ path is bit-identical to a pre-WB4 run either way.
ShaftTrack2D decideTrack(const FrameSource& frameAt,
                         const std::vector<int64_t>& tUs,
                         const std::vector<double>& gx, const std::vector<double>& gy,
                         const std::vector<double>& phiRaw,
                         const std::vector<std::vector<cv::Point2d>>& rawJoints,
                         int frameW, int frameH, double fps,
                         const std::vector<double>& bandsMm, double clubLenMm,
                         int impactFrame, const ShaftV3Config& cfg,
                         ShaftDecideTrace* trace = nullptr,
                         const BallTrack2D* ball = nullptr,
                         const LengthPriorState* lengthPrior = nullptr,
                         const std::vector<double>& handAxisDeg = {},
                         const std::vector<double>& handAxisConf = {});

} // namespace pinpoint::analysis
