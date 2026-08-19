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

#include "event_refine.h"

#include <QPointF>
#include <algorithm>
#include <cmath>
#include <limits>

#include "ball_anchor.h"       // buildThetaBallSeries (SHARED θ_ball — one implementation)
#include "shaft_positions.h"   // addressHoldEndFrame (REUSED for the refined Address)

namespace pinpoint::analysis {
namespace {

constexpr double kPiD = 3.14159265358979323846;

// First-cut Tier-C + confidence constants (corpus-tuning owed, same posture as
// ball_anchor.cpp's thresholds — sweepable later under "refine.*" if the corpus
// says they need moving). Tier C = "grip within a small radius of the P1 fitted
// grip"; always available, lowest authority.
constexpr double kGripTierRadiusPx = 30.0;   // px around the P1 fitted grip counted "at address"
// Tier base confidences (A measured-θ / B club-activity / C grip-radius) — highest
// available tier decides the event; the base is its authority.
constexpr float  kConfTierA = 0.80f;
constexpr float  kConfTierB = 0.65f;
constexpr float  kConfTierC = 0.45f;
// Corroboration adjustments applied per NON-deciding tier that also has data at L:
// agreement raises the confidence, disagreement lowers it.
constexpr float  kAgreeBonus      = 0.10f;
constexpr float  kDisagreePenalty = 0.15f;

double angDiffDeg(double aRad, double bRad)
{
    double d = (aRad - bRad) * 180.0 / kPiD;
    while (d > 180.0)  d -= 360.0;
    while (d < -180.0) d += 360.0;
    return std::abs(d);
}

int nearestFrame(const std::vector<int64_t> &tUs, int64_t t)
{
    int best = 0;
    int64_t bd = std::numeric_limits<int64_t>::max();
    for (int i = 0; i < int(tUs.size()); ++i) {
        const int64_t d = std::llabs(tUs[size_t(i)] - t);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

} // namespace

EventRefineResult refineEvents(Segmentation &seg, ShaftTrack2D &shaft,
                               const BallTrack2D &ball, int64_t impactUs,
                               const EventRefineConfig &cfg)
{
    EventRefineResult res;

    // P6 residual-first telemetry (log-only): launch − impact when both exist.
    // Computed regardless of whether any event is refined (and even before the
    // enabled gate) — Impact itself is NEVER refined (marker contract).
    if (cfg.impactResidual && ball.launchTUs >= 0 && impactUs >= 0) {
        res.impactResidualValid = true;
        res.impactResidualUs    = ball.launchTUs - impactUs;
    }
    if (!cfg.enabled)
        return res;   // master gate (belt-and-braces — the stage's canRun already gates it)

    const int nf = int(shaft.samples.size());
    const PhaseEvent *topEv = seg.eventFor(Phase::Top);
    if (nf < 2 || !topEv)
        return res;   // no Top landmark ⇒ abstain (contract)

    // Per-frame grip + time from the shaft samples (the θ_ball builder needs both).
    std::vector<double>  gx(size_t(nf), 0.0), gy(size_t(nf), 0.0);
    std::vector<int64_t> tUs(size_t(nf), 0);
    for (int i = 0; i < nf; ++i) {
        gx[size_t(i)]  = shaft.samples[size_t(i)].gripPx.x();
        gy[size_t(i)]  = shaft.samples[size_t(i)].gripPx.y();
        tUs[size_t(i)] = shaft.samples[size_t(i)].t_us;
    }

    // θ_ball via the SHARED builder — one implementation with applyBallAnchor.
    const ThetaBallSeries tb = buildThetaBallSeries(ball, gx, gy, tUs,
                                                    shaft.frameWidth, shaft.frameHeight);

    // Per-frame club-corridor activity (Tier B): the nearest ball sample's
    // clubActivity, present only when >= 0 (dark ball.clubActivity ⇒ all absent).
    std::vector<float> activity(size_t(nf), -1.f);
    bool anyActivity = false;
    if (!ball.frames.empty()) {
        for (int i = 0; i < nf; ++i) {
            const BallSample2D *best = nullptr;
            int64_t bd = std::numeric_limits<int64_t>::max();
            for (const BallSample2D &f : ball.frames) {
                const int64_t d = std::llabs(f.t_us - tUs[size_t(i)]);
                if (d < bd) { bd = d; best = &f; }
            }
            if (best && best->clubActivity >= 0.f) {
                activity[size_t(i)] = best->clubActivity;
                anyActivity = true;
            }
        }
    }

    // Tier-C reference: the P1 fitted grip (shaft.positions p == 1, IMAGE px).
    bool haveP1grip = false;
    QPointF p1grip;
    for (const ShaftPosition &p : shaft.positions)
        if (p.p == 1) { p1grip = p.gripPx; haveP1grip = true; break; }

    // Adaptive at-ball threshold (like tk0, ball_anchor.cpp): floor departThetaDeg
    // to the club's own departure at the address reference (bs0) frame — bs0 is
    // already inside the grip-speed heuristic's lagging boundary, so its own
    // alignment is a permissive, self-calibrating reference.
    double effDepart = cfg.departThetaDeg;
    const int bs0 = shaft.addressPhaseFrame;
    if (bs0 >= 0 && bs0 < nf && !tb.haveBall.empty() && tb.haveBall[size_t(bs0)]
        && (shaft.samples[size_t(bs0)].flags & ShaftMeasured)
        && !(shaft.samples[size_t(bs0)].flags & ShaftBallAnchored)) {
        effDepart = std::max(effDepart, angDiffDeg(shaft.samples[size_t(bs0)].thetaRad,
                                                   tb.thetaBall[size_t(bs0)]));
    }

    // ── Per-frame at-ball evidence, three tiers ──────────────────────────────
    // Highest AVAILABLE tier decides atBall[i]; the deciding tier is recorded for
    // the base confidence. A ShaftBallAnchored frame asserts at-ball DIRECTLY (its
    // θ = θ_ball by construction), with Tier-A authority but excluded from the
    // distance test.
    enum Tier { TierNone = 0, TierC = 1, TierB = 2, TierA = 3 };
    auto anchored  = [&](int i) { return (shaft.samples[size_t(i)].flags & ShaftBallAnchored) != 0; };
    auto aEligible = [&](int i) {
        return !tb.haveBall.empty() && tb.haveBall[size_t(i)]
            && (shaft.samples[size_t(i)].flags & ShaftMeasured) && !anchored(i);
    };
    auto aAtBall = [&](int i) {
        return angDiffDeg(shaft.samples[size_t(i)].thetaRad, tb.thetaBall[size_t(i)]) <= effDepart;
    };
    auto bAtBall = [&](int i) {
        return activity[size_t(i)] >= 0.f && double(activity[size_t(i)]) < cfg.activityQuietSigma;
    };
    auto cAtBall = [&](int i) {
        return haveP1grip
            && std::hypot(gx[size_t(i)] - p1grip.x(), gy[size_t(i)] - p1grip.y()) <= kGripTierRadiusPx;
    };

    std::vector<char> atBall(size_t(nf), 0), tierAt(size_t(nf), TierNone);
    for (int i = 0; i < nf; ++i) {
        if (anchored(i))                     { atBall[size_t(i)] = 1;         tierAt[size_t(i)] = TierA; }
        else if (aEligible(i))               { atBall[size_t(i)] = aAtBall(i); tierAt[size_t(i)] = TierA; }
        else if (activity[size_t(i)] >= 0.f) { atBall[size_t(i)] = bAtBall(i); tierAt[size_t(i)] = TierB; }
        else if (haveP1grip)                 { atBall[size_t(i)] = cAtBall(i); tierAt[size_t(i)] = TierC; }
        // else TierNone / atBall stays 0 (no evidence this frame)
    }

    // ── Refined Takeaway = LAST-departure / no-return ────────────────────────
    // Scan [searchStart, topFrame): L = the end of the LAST genuine at-ball run
    // (run duration >= returnHoldMs; shorter runs are flicker, debounced out). A
    // genuine run after L would move L forward — so the final L has NO genuine
    // at-ball run in (L, Top]. That last run is the address hold; its departure is
    // the takeaway (the last time the club leaves the ball for good).
    int firstBall = -1;
    for (int i = 0; i < nf; ++i)
        if (!tb.haveBall.empty() && tb.haveBall[size_t(i)]) { firstBall = i; break; }
    const int searchStart = firstBall >= 0 ? firstBall : 0;
    const int topFrame    = std::clamp(nearestFrame(tUs, topEv->t_us), 0, nf - 1);
    const int64_t holdUs  = int64_t(std::max(0, cfg.returnHoldMs)) * 1000;

    int L = -1;
    Tier lTier = TierNone;
    for (int i = searchStart; i < topFrame; ++i) {
        if (!atBall[size_t(i)]) continue;
        int a = i; while (a > searchStart && atBall[size_t(a - 1)]) --a;
        int b = i; while (b + 1 < topFrame && atBall[size_t(b + 1)]) ++b;
        if (tUs[size_t(b)] - tUs[size_t(a)] >= holdUs) {   // genuine run — remember its end
            L = b;
            lTier = Tier(tierAt[size_t(b)]);
        }
        i = b;   // skip past the whole run (genuine or flicker)
    }
    if (L < 0)
        return res;   // no genuine at-ball run before Top ⇒ abstain
    res.departFrame = L;
    res.tier        = int(lTier);

    // Fused confidence: the deciding tier's base + corroboration from every OTHER
    // tier that has data at L (agreement raises, disagreement lowers).
    float conf = (lTier == TierA) ? kConfTierA : (lTier == TierB) ? kConfTierB : kConfTierC;
    auto corrob = [&](bool agrees) { conf += agrees ? kAgreeBonus : -kDisagreePenalty; };
    if (lTier != TierA) {
        if (anchored(L))       corrob(true);           // anchored asserts at-ball
        else if (aEligible(L)) corrob(aAtBall(L));
    }
    if (lTier != TierB && activity[size_t(L)] >= 0.f) corrob(bAtBall(L));
    if (lTier != TierC && haveP1grip)                 corrob(cAtBall(L));
    conf = std::clamp(conf, 0.f, 1.f);
    res.conf = conf;

    // Refined Takeaway time = the L→departure crossing. Sub-frame interpolate the
    // Tier-A angDiff crossing effDepart when both L and L+1 are measured; else the
    // frame midpoint (tUs[L], tUs[L+1]).
    int64_t takeUs;
    const int Lr = std::min(L + 1, nf - 1);
    if (Lr > L && lTier == TierA && aEligible(L) && aEligible(Lr)) {
        const double dL = angDiffDeg(shaft.samples[size_t(L)].thetaRad,  tb.thetaBall[size_t(L)]);
        const double dR = angDiffDeg(shaft.samples[size_t(Lr)].thetaRad, tb.thetaBall[size_t(Lr)]);
        double frac = 0.5;
        if (dR > dL) frac = std::clamp((effDepart - dL) / (dR - dL), 0.0, 1.0);
        takeUs = tUs[size_t(L)] + int64_t(std::llround(frac * double(tUs[size_t(Lr)] - tUs[size_t(L)])));
    } else if (Lr > L) {
        takeUs = (tUs[size_t(L)] + tUs[size_t(Lr)]) / 2;
    } else {
        takeUs = tUs[size_t(L)];
    }

    // ── Refined Address = the hold end walked back from the refined takeaway ──
    // Re-call addressHoldEndFrame (REUSE, don't re-derive) with horizon bs0 = L,
    // baByFrame from the BallAnchored flags, clubQuiet from Tier B.
    std::vector<char> baByFrame(size_t(nf), 0), clubQuiet(size_t(nf), 0);
    for (int i = 0; i < nf; ++i) {
        if (anchored(i)) baByFrame[size_t(i)] = 1;
        if (activity[size_t(i)] >= 0.f && double(activity[size_t(i)]) < cfg.activityQuietSigma)
            clubQuiet[size_t(i)] = 1;
    }
    const std::vector<char> *clubQuietPtr = anyActivity ? &clubQuiet : nullptr;

    // Floor the walk-back at the onset-veto floor (the LAST settle the veto proved
    // the track never left for good). When the veto is DARK (onsetFloorFrame < 0)
    // fall back to the takeaway-onset frame bs0 (addressPhaseFrame) — the Stage-A
    // walk-back-corrected motion onset, the trusted Address boundary. On a fidget
    // swing the shaft still POINTS at the ball through the whole waggle (Tier A
    // can't separate fidget from settle) AND the grip-stillness test fails through
    // the settle (pose flap), so with no floor addressHoldEndFrame reverts to the
    // deep PRE-fidget hold (the documented s0002 −0.58 s Address failure); the bs0
    // fallback stops that over-walk at the settle onset instead. When the veto
    // floor IS set it wins (it is ≤ bs0 by the PhaseModel contract) — those swings
    // are unchanged.
    const int floorFrame = shaft.onsetFloorFrame >= 0 ? shaft.onsetFloorFrame
                                                      : shaft.addressPhaseFrame;
    PositionsConfig pcfg;   // grip-still walk-back knobs (defaults); mask + floor drive the rest
    const int addrFrame = addressHoldEndFrame(gx, gy, baByFrame, L, pcfg, clubQuietPtr, floorFrame);
    res.addressFrame = addrFrame;
    const int64_t addrUs = (addrFrame >= 0 && addrFrame < nf) ? tUs[size_t(addrFrame)] : -1;

    // ── Apply — gates + monotone ladder + provenance = Club ──────────────────
    PhaseEvent *addrEv = nullptr, *takeEv = nullptr;
    for (PhaseEvent &e : seg.events) {
        if (e.phase == Phase::Address)  addrEv = &e;
        if (e.phase == Phase::Takeaway) takeEv = &e;
    }

    // Takeaway first (its result bounds Address's upper limit). Apply only when
    // conf >= minConf, |shift| <= maxShiftS, and Address(current) <= takeUs <= Top.
    if (cfg.takeaway && takeEv && conf >= float(cfg.minConf)) {
        const double shiftS = std::abs(double(takeUs - takeEv->t_us)) * 1e-6;
        const int64_t addrLo = addrEv ? addrEv->t_us : std::numeric_limits<int64_t>::min();
        if (shiftS <= cfg.maxShiftS && takeUs >= addrLo && takeUs <= topEv->t_us) {
            takeEv->t_us      = takeUs;
            takeEv->conf      = conf;
            takeEv->provenance = SegmentRole::Club;
            takeEv->timing     = TimingClass::Measured;   // relocated on the club's own departure

            res.takeawayRefined = true;
            res.takeawayUs      = takeUs;
        }
    }

    // Address. Invariant refinedAddress <= refinedTakeaway (else abstain), plus the
    // conf/shift gates. swingStartUs is kept CONSISTENT with the refined Address:
    // the span start must not sit AFTER the address (the address has to lie within
    // [swingStart, swingEnd]). On the vision path swingStartUs is bs0/onset-derived
    // (NOT Address−pad), so a blind delta-shift would push it past the event — clamp
    // to <= the refined Address instead (never moved later). swingEndUs untouched;
    // persisted-only coupling (no compute feedback).
    if (cfg.address && addrEv && addrUs >= 0 && conf >= float(cfg.minConf)) {
        const double shiftS = std::abs(double(addrUs - addrEv->t_us)) * 1e-6;
        int64_t addrHi = topEv->t_us;
        if (res.takeawayRefined)  addrHi = res.takeawayUs;
        else if (takeEv)          addrHi = takeEv->t_us;
        if (shiftS <= cfg.maxShiftS && addrUs <= addrHi) {
            addrEv->t_us       = addrUs;
            addrEv->conf       = conf;
            addrEv->provenance = SegmentRole::Club;
            addrEv->timing     = TimingClass::Measured;   // the grip-stillness walk-back measured it
            seg.swingStartUs   = std::min(seg.swingStartUs, addrUs);
            res.addressRefined = true;
            res.addressUs      = addrUs;

            // Keep the P1 coaching position in lock-step with its Address twin:
            // both were born from addressHoldEndFrame at shaft-assembly time, so a
            // refined Address must drag P1 with it or the two blocks (phases[] vs
            // club.positions[]) diverge on the timeline. addrFrame is an exact
            // sample index, so re-derive P1 straight off that frame's track sample
            // (grip→head length + drawn direction, matching sampleTrackAt in
            // shaft_track_assembly.cpp) and downgrade it to a plain TrackSample —
            // any prior milestone fit no longer applies at the new anchor.
            for (ShaftPosition &p : shaft.positions) {
                if (p.p != 1) continue;
                const ShaftSample2D &s = shaft.samples[size_t(addrFrame)];
                p.t_us         = addrUs;
                p.gripPx       = s.gripPx;
                p.thetaRad     = s.thetaRad;
                p.conf         = s.conf;
                p.lenPx        = std::hypot(s.headPx.x() - s.gripPx.x(),
                                            s.headPx.y() - s.gripPx.y());
                p.headPx       = QPointF(s.gripPx.x() + p.lenPx * std::cos(p.thetaRad),
                                         s.gripPx.y() + p.lenPx * std::sin(p.thetaRad));
                p.source       = uint8_t(PositionSource::TrackSample);
                p.sigmaThetaDeg = -1.f;
                p.sigmaLenPx    = -1.f;
                p.stackN        = 0;
                res.p1Synced   = true;
                break;
            }
        }
    }

    res.refined = res.takeawayRefined || res.addressRefined;
    if (res.refined)
        seg.version = 3;   // "shaft refinement ran" (Segmentation::version schema)
    return res;
}

} // namespace pinpoint::analysis
