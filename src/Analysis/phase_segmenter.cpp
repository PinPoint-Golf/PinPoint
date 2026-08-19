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

#include "phase_segmenter.h"

#include <algorithm>
#include <cmath>

#include "phase_signals.h"

namespace pinpoint::analysis {
namespace ps = phase_signals;
namespace {

// A detector candidate: a refined instant + shaped confidence + how the instant
// was OBTAINED. `cls` says in data what the capped conf constants below have
// always said in comments — 0.35 "hand-orientation PROXY", 0.30 "continuous
// waggle" fallback, 0.20 "window-edge clamp, visibly so" — so timeline_fusion.h
// can arbitrate on it instead of guessing from conf magnitudes. Measured is the
// default because most detectors here genuinely measure their quantity.
struct Cand {
    bool        ok   = false;
    int64_t     tUs  = 0;
    float       conf = 0.f;
    TimingClass cls  = TimingClass::Measured;
};

int nearestIndex(const std::vector<int64_t> &grid, int64_t t)
{
    if (t <= grid.front()) return 0;
    if (t >= grid.back())  return int(grid.size()) - 1;
    const auto it = std::lower_bound(grid.begin(), grid.end(), t);
    const int hi = int(it - grid.begin());
    const int lo = hi - 1;
    return (t - grid[size_t(lo)] <= grid[size_t(hi)] - t) ? lo : hi;
}

int idxAtOrBelow(const std::vector<int64_t> &grid, int64_t t)
{
    const auto it = std::upper_bound(grid.begin(), grid.end(), t);
    return std::max(0, int(it - grid.begin()) - 1);
}

// Duration-prior confidence shaping (A.5): 1.0 at the gate centre falling to
// 0.6 at the edges; a hard gate miss keeps the event but flags it clearly.
float gateConf(int64_t x, int64_t lo, int64_t hi)
{
    if (x < lo || x > hi)
        return 0.25f;
    const double mid  = 0.5 * double(lo + hi);
    const double norm = (hi > lo) ? 2.0 * std::abs(double(x) - mid) / double(hi - lo) : 0.0;
    return float(1.0 - 0.4 * norm * norm);
}

// Linear sub-grid refinement of `y` crossing `level` between samples i and
// i+1 (caller guarantees the straddle).
int64_t refineCrossingUs(const std::vector<int64_t> &grid,
                         const std::vector<double> &y, int i, double level)
{
    const double a = y[size_t(i)] - level, b = y[size_t(i) + 1] - level;
    const double f = (a == b) ? 0.0 : std::clamp(a / (a - b), 0.0, 1.0);
    return grid[size_t(i)]
         + int64_t(std::llround(f * double(grid[size_t(i) + 1] - grid[size_t(i)])));
}

bool usable(const SegmentStream *s, size_t n)
{
    return s && s->qAnat.size() == n && s->gyroDps.size() == n && s->accelG.size() == n;
}

// Per-segment Top detection (A.5.1): find the downswing run — the maximal
// interval ending at impact where the signed swing-plane rate is positive —
// and place Top at the zero-crossing that starts it.
struct TopDetect {
    Cand cand;
    std::vector<double> env;   // the segment's energy envelope (reused downstream)
};

TopDetect detectTop(const SegmentStream &s, const std::vector<int64_t> &grid,
                    double fsHz, int idxImpact, int64_t impactUs,
                    const SegmentationConfig &cfg)
{
    TopDetect out;
    out.env = ps::energyEnvelope(s, fsHz, cfg.fcEnvelopeHz);

    const std::vector<QVector3D> gw = ps::worldGyro(s);
    const QVector3D n = ps::principalRotationAxis(
        gw, grid, impactUs - 300000, impactUs - 30000);
    if (n.isNull())
        return out;
    const std::vector<double> rate =
        ps::lowpassZeroPhase(ps::signedRate(gw, n), fsHz, cfg.fcEnvelopeHz);

    int i = std::min(idxImpact, int(rate.size()) - 1);
    // The rate can sit at/below zero exactly at impact (the reversal happens
    // at the ball) — step back a small slack to land inside the downswing run.
    while (i > 1 && rate[size_t(i)] <= 0.0
           && grid[size_t(std::min(idxImpact, int(grid.size()) - 1))] - grid[size_t(i)]
                  <= cfg.topImpactSlackUs)
        --i;
    if (i < 1 || rate[size_t(i)] <= 0.0)
        return out;   // no positive run reaching impact — nothing to anchor on
    const int64_t oldest = impactUs - cfg.topMaxBeforeImpactUs;
    while (i > 0 && grid[size_t(i) - 1] >= oldest && rate[size_t(i) - 1] > 0.0)
        --i;
    if (i == 0 || rate[size_t(i) - 1] > 0.0)
        return out;   // gate exhausted without a reversal — refuse, don't guess

    const int64_t t = refineCrossingUs(grid, rate, i - 1, 0.0);
    out.cand.ok   = true;
    out.cand.tUs  = t;
    out.cand.conf = 0.9f * gateConf(impactUs - t, cfg.topMinBeforeImpactUs,
                                    cfg.topMaxBeforeImpactUs);
    return out;
}

// Hand/forearm voting (A.5 multi-segment rule): agreement within the window
// averages instants and boosts confidence; disagreement prefers the first
// (hand) and lowers it.
// A vote is no better than its worse witness: averaging a measurement with a
// fallback produces a time that is still part placeholder, so the class of the
// combined candidate is the LOWER-ranked of the two.
Cand vote(const Cand &hand, const Cand &fore, int64_t agreeUs)
{
    if (hand.ok && fore.ok) {
        const TimingClass cls =
            timingRank(hand.cls) <= timingRank(fore.cls) ? hand.cls : fore.cls;
        if (std::llabs(hand.tUs - fore.tUs) <= agreeUs)
            return { true, (hand.tUs + fore.tUs) / 2,
                     std::min(0.98f, std::max(hand.conf, fore.conf) + 0.05f), cls };
        return { true, hand.tUs, hand.conf * 0.7f, hand.cls };
    }
    return hand.ok ? hand : fore;
}

// Takeaway (A.5.3): back-chain from Top while the envelope stays above the
// hysteresis floor; the chain ends at the first dip below it sustained
// ≥ quietUs, and Takeaway is the rise out of that dip. Because the walk
// starts inside the backswing, waggles (separated from the swing by a quiet
// beat) cannot capture it.
// TODO (future, IMU-ladder no-return analog): the vision path grew a "no-return"
// veto (shaft_track_assembly.cpp segmentPhases) that rejects a club bob which
// DEPARTS the address point and RETURNS, so the Takeaway lands at the last
// settle before the one-piece motion. A fidget with a quiet beat is already
// rejected here by the quiet-gap rule, but a bob with NO quiet beat (continuous
// low-envelope wobble) is not — the analog fix would anchor Address at the last
// still return-to-address of the hand/forearm envelope. Out of scope until a
// calibrated-IMU fidget corpus exists to gate it (plan §"Out of scope").
Cand detectTakeaway(const std::vector<double> &env, const std::vector<int64_t> &grid,
                    int idxTop, int idxImpact, int64_t topUs,
                    const SegmentationConfig &cfg)
{
    Cand out;
    if (env.empty() || idxTop < 1)
        return out;

    double peak = 0.0;
    for (int i = idxTop; i <= idxImpact && i < int(env.size()); ++i)
        peak = std::max(peak, env[size_t(i)]);
    const double thLow = std::max(cfg.takeawayFracOfPeak * peak, cfg.takeawayMinDps);

    // Skip the transition pause: the hands slow to ~zero AT the top, and that
    // sub-floor region is adjacent to Top, not a dip separating waggle from
    // swing — start the dip hunt inside the backswing's energy.
    int start = std::min(idxTop, int(env.size()) - 1);
    while (start > 0 && env[size_t(start)] < thLow)
        --start;

    int dipEnd = -1;   // highest index of the current below-floor dip
    for (int i = start; i >= 0; --i) {
        if (env[size_t(i)] >= thLow) {
            dipEnd = -1;
            continue;
        }
        if (dipEnd < 0)
            dipEnd = i;
        if (grid[size_t(dipEnd)] - grid[size_t(i)] >= cfg.takeawayQuietUs) {
            // Sustained quiet [i..dipEnd]; the swing rises out of it at dipEnd.
            // The takeaway INSTANT is the onset-floor crossing inside the dip
            // (θ_low is hysteresis for dip detection — with fast hands it can
            // sit 50+ °/s up the rising edge and would date the onset late).
            int k = dipEnd;
            while (k > i && env[size_t(k)] >= cfg.takeawayMinDps)
                --k;
            out.ok  = true;
            if (env[size_t(k)] < cfg.takeawayMinDps && k + 1 < int(env.size()))
                out.tUs = refineCrossingUs(grid, env, k, cfg.takeawayMinDps);
            else if (dipEnd + 1 < int(env.size()))
                out.tUs = refineCrossingUs(grid, env, dipEnd, thLow);
            else
                out.tUs = grid[size_t(dipEnd)];
            out.conf = 0.85f * gateConf(topUs - out.tUs,
                                        cfg.backswingMinUs, cfg.backswingMaxUs);
            return out;
        }
    }
    // Never went quiet inside the window — continuous pre-shot motion. The
    // window edge is a placeholder, not an estimate of the takeaway.
    out.ok   = true;
    out.tUs  = grid.front();
    out.conf = 0.3f;
    out.cls  = TimingClass::Fallback;
    return out;
}

} // namespace

Segmentation PhaseSegmenter::segment(const FusedStreams &streams, int64_t impactUs,
                                     const SegmentationConfig &cfg)
{
    Segmentation out;
    const std::vector<int64_t> &grid = streams.timeGrid;
    const size_t N = grid.size();
    if (N < 3)
        return out;

    out.swingStartUs = grid.front();
    out.swingEndUs   = grid.back();

    const int64_t clampedImpact = std::clamp(impactUs, grid.front(), grid.back());
    const int     idxImpact     = nearestIndex(grid, clampedImpact);
    const double  fsHz = 1.0e6 / double(std::max<int64_t>(grid[1] - grid[0], 1));

    const SegmentStream *hand = streams.streamFor(SegmentRole::LeadHand);
    const SegmentStream *fore = streams.streamFor(SegmentRole::LeadForearm);
    if (!usable(hand, N)) hand = nullptr;
    if (!usable(fore, N)) fore = nullptr;

    // The v1-equivalent clamp fallback: emitted when there is no usable motion
    // stream OR the downswing reversal never shows inside its gate. conf 0 =
    // "bounds are just the window" (A.2.6: refuse the ladder, never guess it).
    const auto clampFallback = [&] {
        out.events.clear();
        out.events.push_back({ Phase::Address, grid.front(),  0.3f, SegmentRole::Unknown,
                               TimingClass::Fallback });
        out.events.push_back({ Phase::Impact,  clampedImpact, 1.0f, SegmentRole::Unknown,
                               TimingClass::Anchor });
        out.events.push_back({ Phase::Finish,  grid.back(),   0.2f, SegmentRole::Unknown,
                               TimingClass::Fallback });
        out.conf = 0.0f;
        return out;
    };
    if (!hand && !fore)
        return clampFallback();

    // ── 1. Top — backward from impact, voted across hand + forearm ──────────
    TopDetect handTop, foreTop;
    if (hand) handTop = detectTop(*hand, grid, fsHz, idxImpact, clampedImpact, cfg);
    if (fore) foreTop = detectTop(*fore, grid, fsHz, idxImpact, clampedImpact, cfg);
    Cand top = vote(hand ? handTop.cand : Cand{}, fore ? foreTop.cand : Cand{},
                    cfg.voteAgreeUs);
    if (!top.ok)
        return clampFallback();

    // Thorax cross-check (A.5 voting): reversal within ±60 ms boosts Top.
    if (const SegmentStream *thx = streams.streamFor(SegmentRole::Thorax);
        usable(thx, N)) {
        const TopDetect t = detectTop(*thx, grid, fsHz, idxImpact, clampedImpact, cfg);
        if (t.cand.ok)
            top.conf = (std::llabs(t.cand.tUs - top.tUs) <= cfg.thoraxAgreeUs)
                           ? std::min(0.98f, top.conf + 0.05f)
                           : top.conf * 0.85f;
    }

    // The reference chain for energy-based searches: hand first (most distal).
    const SegmentRole refRole = hand ? SegmentRole::LeadHand : SegmentRole::LeadForearm;
    const std::vector<double> &refEnv = hand ? handTop.env : foreTop.env;
    const int idxTop = idxAtOrBelow(grid, top.tUs);

    // ── 2. Transition — pelvis axial reversal shortly before Top ────────────
    Cand transition;
    if (const SegmentStream *pel = streams.streamFor(SegmentRole::Pelvis);
        usable(pel, N)) {
        const std::vector<double> axial = ps::lowpassZeroPhase(
            ps::axialRate(*pel, QVector3D(0, 1, 0)), fsHz, cfg.fcEnvelopeHz);
        double mean = 0.0;
        int cnt = 0;
        for (int i = idxTop; i <= idxImpact; ++i, ++cnt)
            mean += axial[size_t(i)];
        mean = cnt ? mean / cnt : 0.0;
        if (std::abs(mean) >= cfg.transMinMeanDps) {
            // The downswing direction is whatever sign the pelvis actually
            // turned with through Top→Impact; find the crossing INTO it.
            const double sgn = mean > 0 ? 1.0 : -1.0;
            const int lo = idxAtOrBelow(grid, top.tUs - cfg.transBeforeTopUs);
            const int hi = std::min(int(N) - 2,
                                    idxAtOrBelow(grid, top.tUs + cfg.transAfterTopUs));
            int best = -1;
            for (int i = lo; i <= hi; ++i)
                if (sgn * axial[size_t(i)] <= 0.0 && sgn * axial[size_t(i) + 1] > 0.0)
                    if (best < 0 || std::llabs(grid[size_t(i)] - top.tUs)
                                        < std::llabs(grid[size_t(best)] - top.tUs))
                        best = i;
            if (best >= 0) {
                transition.ok   = true;
                transition.tUs  = refineCrossingUs(grid, axial, best, 0.0);
                transition.conf = 0.7f * float(std::clamp(std::abs(mean) / 30.0, 0.3, 1.0));
            }
        }
    }

    // ── 3. Takeaway — back-chain from Top, voted across hand + forearm ──────
    const Cand takeaway = vote(
        hand ? detectTakeaway(handTop.env, grid, idxTop, idxImpact, top.tUs, cfg) : Cand{},
        fore ? detectTakeaway(foreTop.env, grid, idxTop, idxImpact, top.tUs, cfg) : Cand{},
        cfg.voteAgreeUs);

    // ── 4. Address — end of the last sustained stillness before Takeaway ────
    // Fallback: continuous waggle — Address co-timed to Takeaway because no
    // sustained stillness was ever found. This is the branch that held Address on
    // 11/11 wG3 swings, ~97 ms from the club's own P1 (timeline-fusion.md §2).
    Cand address{ true, takeaway.tUs, 0.3f, TimingClass::Fallback };
    {
        const std::vector<uint8_t> still =
            ps::stillMask(streams, cfg.stillGyroDps, cfg.stillAccelTolG);
        const int idxTake = idxAtOrBelow(grid, takeaway.tUs);
        int b = -1;
        for (int i = idxTake; i >= 0; --i) {
            if (!still[size_t(i)]) {
                b = -1;
                continue;
            }
            if (b < 0)
                b = i;
            if (grid[size_t(b)] - grid[size_t(i)] >= cfg.addressStillMinUs) {
                // END of the stillness — a real measurement of address.
                address = { true, grid[size_t(b)], 0.9f, TimingClass::Measured };
                break;
            }
        }
        // The stillness end (raw gyro) can land a hair after the takeaway
        // instant (zero-phase-filtered envelope spreads the onset backward);
        // Address ≤ Takeaway by definition, so clamp instead of letting the
        // monotone chain drop the takeaway.
        address.tUs = std::min(address.tUs, takeaway.tUs);
    }

    // ── 5. Geometric checkpoints — forearm/hand inclination crossings ───────
    Cand midBack, armParDown, shaftParThrough, followThrough, delivery;
    if (fore) {
        const std::vector<double> incl =
            ps::lowpassZeroPhase(ps::inclination(*fore), fsHz, cfg.fcEnvelopeHz);
        const int idxTake = idxAtOrBelow(grid, takeaway.tUs);
        for (int i = idxTake; i < idxTop && i + 1 < int(N); ++i)             // P3: rising
            if (incl[size_t(i)] < 0.0 && incl[size_t(i) + 1] >= 0.0) {
                midBack = { true, refineCrossingUs(grid, incl, i, 0.0), 0.7f };
                break;
            }
        // P5 (ArmParallelDown) IS this crossing — lead arm parallel, downswing.
        for (int i = idxTop; i < idxImpact && i + 1 < int(N); ++i)           // P5: falling
            if (incl[size_t(i)] > 0.0 && incl[size_t(i) + 1] <= 0.0) {
                armParDown = { true, refineCrossingUs(grid, incl, i, 0.0), 0.7f };
                break;
            }
        // P8 (ShaftParallelThrough) is defined shaft-parallel: the first rising
        // crossing is a forearm PROXY, so it carries a capped confidence like
        // the Delivery (P6) hand proxy. P9 (arm parallel) is exact — 0.65.
        int post = 0;                                                        // P8 / P9: rising
        for (int i = idxImpact; i + 1 < int(N); ++i)
            if (incl[size_t(i)] < 0.0 && incl[size_t(i) + 1] >= 0.0) {
                const int64_t tUs = refineCrossingUs(grid, incl, i, 0.0);
                if (++post == 1) shaftParThrough = { true, tUs, 0.35f, TimingClass::Proxy };
                else { followThrough = { true, tUs, 0.65f, TimingClass::Measured }; break; }
            }
    }
    if (hand) {
        // Delivery (P6) hand-orientation PROXY until the shaft refinement —
        // labelled by its capped confidence (A.5.5, ≤ 0.4).
        const std::vector<double> incl =
            ps::lowpassZeroPhase(ps::inclination(*hand), fsHz, cfg.fcEnvelopeHz);
        const int from = armParDown.ok ? idxAtOrBelow(grid, armParDown.tUs) : idxTop;
        for (int i = from; i < idxImpact && i + 1 < int(N); ++i)
            if (incl[size_t(i)] > 0.0 && incl[size_t(i) + 1] <= 0.0) {
                delivery = { true, refineCrossingUs(grid, incl, i, 0.0), 0.35f,
                             TimingClass::Proxy };
                break;
            }
    }

    // ── 6. MaxSpeed — peak of the reference envelope in [Top, Impact+50ms] ──
    Cand maxSpeed;
    {
        const int hi = std::min(int(N) - 1,
                                idxAtOrBelow(grid, clampedImpact + cfg.maxSpeedPostImpactUs));
        int p = idxTop;
        for (int i = idxTop; i <= hi; ++i)
            if (refEnv[size_t(i)] > refEnv[size_t(p)]) p = i;
        if (p > idxTop && refEnv[size_t(p)] > cfg.takeawayMinDps)
            maxSpeed = { true, ps::fracIndexToUs(grid, ps::refineExtremum(refEnv, p)), 0.85f };
    }

    // ── 7. Finish — relaxed quiet sustained after impact ────────────────────
    // Window-edge clamp, visibly so — and now sayably so: Fallback is what stops
    // it wearing the P10 label over the club's measured finish.
    Cand finish{ true, grid.back(), 0.2f, TimingClass::Fallback };
    {
        const std::vector<uint8_t> quiet =
            ps::stillMask(streams, cfg.finishGyroDps, 2.0 * cfg.stillAccelTolG);
        const int from = std::min(int(N) - 1,
                                  idxAtOrBelow(grid, clampedImpact + cfg.finishMinAfterImpactUs) + 1);
        int runStart = -1;
        for (int i = from; i < int(N); ++i) {
            if (!quiet[size_t(i)]) {
                runStart = -1;
                continue;
            }
            if (runStart < 0)
                runStart = i;
            if (grid[size_t(i)] - grid[size_t(runStart)] >= cfg.finishSustainUs) {
                finish = { true, grid[size_t(runStart)],
                           0.8f * gateConf(grid[size_t(runStart)] - clampedImpact,
                                           cfg.finishMinUs, cfg.finishMaxUs),
                           TimingClass::Measured };   // the quiet-run detector found a real finish
                break;
            }
        }
    }

    // ── Assemble the ladder in canonical order, then enforce monotonicity ───
    const SegmentRole foreRole = fore ? SegmentRole::LeadForearm : refRole;
    const auto addEvent = [&out](Phase ph, const Cand &c, SegmentRole prov) {
        if (c.ok)
            out.events.push_back({ ph, c.tUs, c.conf, prov, c.cls });
    };
    // P5/P8 go out under the names the diagnostics vocabulary uses —
    // ArmParallelDown/ShaftParallelThrough, not the v1 Downswing/Release. They
    // are the same instants, so only one name of each pair is ever emitted.
    addEvent(Phase::Address,              address,         refRole);
    addEvent(Phase::Takeaway,             takeaway,        refRole);
    addEvent(Phase::MidBackswing,         midBack,         foreRole);
    addEvent(Phase::Transition,           transition,      SegmentRole::Pelvis);
    addEvent(Phase::Top,                  top,             refRole);
    addEvent(Phase::ArmParallelDown,      armParDown,      foreRole);
    addEvent(Phase::Delivery,             delivery,        SegmentRole::LeadHand);
    addEvent(Phase::MaxSpeed,             maxSpeed,        refRole);
    addEvent(Phase::Impact,               { true, clampedImpact, 1.0f, TimingClass::Anchor },
             SegmentRole::Unknown);
    addEvent(Phase::ShaftParallelThrough, shaftParThrough, foreRole);
    addEvent(Phase::FollowThrough,        followThrough,   foreRole);
    addEvent(Phase::Finish,               finish,          refRole);

    // Monotone chain (A.2.5): a violating event is DROPPED — lower confidence
    // loses, Impact never loses — never reordered.
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t k = 1; k < out.events.size(); ++k) {
            if (out.events[k].t_us >= out.events[k - 1].t_us)
                continue;
            const bool dropPrev =
                out.events[k].phase == Phase::Impact
                || (out.events[k - 1].phase != Phase::Impact
                    && out.events[k - 1].conf < out.events[k].conf);
            out.events.erase(out.events.begin() + long(dropPrev ? k - 1 : k));
            changed = true;
            break;
        }
    }

    // ── Bounds + overall confidence (A.6) ────────────────────────────────────
    const PhaseEvent *evAddress = out.eventFor(Phase::Address);
    const PhaseEvent *evTop     = out.eventFor(Phase::Top);
    const PhaseEvent *evFinish  = out.eventFor(Phase::Finish);
    if (evAddress)
        out.swingStartUs = std::max(grid.front(), evAddress->t_us - cfg.boundPadUs);
    if (evFinish)
        out.swingEndUs = std::min(grid.back(), evFinish->t_us + cfg.boundPadUs);
    float c = 1.0f;
    for (const PhaseEvent *e : { evAddress, evTop, out.eventFor(Phase::Impact), evFinish })
        c = std::min(c, e ? e->conf : 0.0f);
    out.conf = c;
    return out;
}

} // namespace pinpoint::analysis
