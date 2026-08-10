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

#include "ball_anchor.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "shot_analyzer.h"   // ShotAnalysisJob
#include "analysis_tuning.h" // tuning::apply (ball.tk0AddressOverride A/B key)
#include "../Core/pp_debug.h"
#include "../Core/pp_tuned_constants.h"   // tuned::ball::kTk0AddressOverride

namespace pinpoint::analysis {
namespace {

constexpr double kPi = 3.14159265358979323846;

// First-cut thresholds (build + self-verify per plan; corpus-tuning owed —
// see docs/implementation/shaft_detection_v3_impl.md v3.4, "gates deferred
// to the post-release validation pass"). Sweepable later via job.tuningOverrides
// under the ball.* namespace if corpus data says these need moving.
constexpr double kTk0DepartDeg   = 25.0;   // theta-vs-theta_ball departure => tk0
constexpr double kAgreeTolDeg    = 15.0;   // agreement tolerance vs an existing Measured sample
constexpr double kMaxJumpNormPx  = 6.0;    // mis-lock rejection radius: chain gate (θ loop) / cluster gate (length)
constexpr float  kAnchorConf     = 0.5f;   // soft-anchor confidence (uncorrected — no delta lean-bias table yet)
constexpr int    kMinLenSamples  = 5;      // accepted samples below which the length measurement abstains (-1)
// Golf-prior plausibility gate on the length measurement (face-on: ball always
// between the feet + below the ankle line). Margins are frame-relative so they
// track resolution; plain constexpr until corpus data says they need sweeping.
constexpr double kBallBelowAnkleMarginFrac = 0.02;   // ·frameH slack above the ankle line
constexpr double kBallFeetMarginFrac       = 0.10;   // ·frameW widening per side of the ankle x-extent

double angDiffDeg(double aRad, double bRad)
{
    double d = (aRad - bRad) * 180.0 / kPi;
    while (d > 180.0)  d -= 360.0;
    while (d < -180.0) d += 360.0;
    return std::abs(d);
}

// Nearest ball sample to t_us (linear scan — nf is a few hundred to ~1000
// frames, this runs once per coverage frame in a single post-hoc pass, not
// in any hot loop). Returns nullptr when ball has no frames or the nearest
// sample is stale (> ~2 frame periods away).
const BallSample2D *nearestBallSample(const BallTrack2D &ball, int64_t tUs, int64_t toleranceUs)
{
    const BallSample2D *best = nullptr;
    int64_t bestDt = std::numeric_limits<int64_t>::max();
    for (const auto &s : ball.frames) {
        const int64_t dt = std::llabs(s.t_us - tUs);
        if (dt < bestDt) { bestDt = dt; best = &s; }
    }
    if (best && bestDt <= toleranceUs) return best;
    return nullptr;
}

// Frame↔ball matching tolerance: half the coarser of the two cadences, with
// margin. Offline (BallRunner) emits one sample per frame, so the 2×frame-
// interval term dominates and behavior is unchanged. The LIVE accumulator is
// throttle-gated (~35–45 Hz against ~150 fps frames), where 2×frame-interval
// sits exactly at the worst-case distance to the nearest sample and cadence
// jitter drops matches; ¾ of the ball track's own median spacing keeps every
// frame matchable with 50% headroom.
int64_t ballMatchToleranceUs(const BallTrack2D &ball, const std::vector<int64_t> &tUs)
{
    const int nf = int(tUs.size());
    const int64_t frameTol = nf > 1
        ? std::max<int64_t>(1, (tUs.back() - tUs.front()) / int64_t(nf - 1)) * 2
        : 200000;
    if (ball.frames.size() < 2) return frameTol;
    std::vector<int64_t> dts;
    dts.reserve(ball.frames.size() - 1);
    for (size_t i = 1; i < ball.frames.size(); ++i)
        dts.push_back(ball.frames[i].t_us - ball.frames[i - 1].t_us);
    std::nth_element(dts.begin(), dts.begin() + dts.size() / 2, dts.end());
    const int64_t ballDt = std::max<int64_t>(1, dts[dts.size() / 2]);
    return std::max(frameTol, (3 * ballDt) / 4);
}

} // namespace

ThetaBallSeries buildThetaBallSeries(const BallTrack2D &ball,
                                     const std::vector<double> &gx, const std::vector<double> &gy,
                                     const std::vector<int64_t> &tUs, int frameW, int frameH)
{
    const int nf = int(tUs.size());
    ThetaBallSeries out;
    out.haveBall.assign(size_t(nf), false);
    out.thetaBall.assign(size_t(nf), 0.0);
    out.ballPx.assign(size_t(nf), QPointF());
    if (ball.frames.empty() || nf == 0 || gx.size() != size_t(nf) || gy.size() != size_t(nf))
        return out;   // no ball data / shapes don't line up — all-false series

    const int64_t frameToleranceUs = ballMatchToleranceUs(ball, tUs);

    // Per-frame theta_ball(f) = atan2(B - G) wherever a stable ball sample is
    // available near this coverage frame (design §9.1). A "found" sample that
    // jumped a long way from the last accepted one is treated as a mis-lock,
    // not a moved ball (the ball is stationary by construction at this stage
    // — design §9.7's "constant plus one step"). The θ loop deliberately keeps
    // the sequential jump gate (per-frame anchoring — no single robust statistic
    // to gate against, unlike the length measurement's two-pass cluster gate).
    QPointF lastAccepted;
    bool haveLast = false;
    for (int i = 0; i < nf; ++i) {
        const BallSample2D *s = nearestBallSample(ball, tUs[size_t(i)], frameToleranceUs);
        if (!s || !s->found) continue;
        const QPointF centerPx(s->center.x() * frameW, s->center.y() * frameH);
        if (haveLast) {
            const double jump = std::hypot(centerPx.x() - lastAccepted.x(),
                                           centerPx.y() - lastAccepted.y());
            if (jump > kMaxJumpNormPx) continue;   // mis-lock — skip this sample
        }
        lastAccepted = centerPx;
        haveLast = true;
        out.ballPx[size_t(i)]   = centerPx;
        out.thetaBall[size_t(i)] = std::atan2(centerPx.y() - gy[size_t(i)], centerPx.x() - gx[size_t(i)]);
        out.haveBall[size_t(i)]  = true;
    }
    return out;
}

double medianGripBallLenPx(const BallTrack2D &ball,
                           const std::vector<double> &gx, const std::vector<double> &gy,
                           const std::vector<int64_t> &tUs, int frameW, int frameH,
                           int bs0, int collar, const std::vector<char> *still,
                           const std::vector<AnklePx> *ankles, int *rejectReason,
                           QPointF *clusterBallPx)
{
    if (rejectReason) *rejectReason = 0;
    const int nf = int(tUs.size());
    if (ball.frames.empty() || nf == 0 || gx.size() != size_t(nf) || gy.size() != size_t(nf))
        return -1.0;   // no ball data / shapes don't line up — additive-only
    const int64_t frameToleranceUs = ballMatchToleranceUs(ball, tUs);
    bs0 = std::clamp(bs0, 0, nf);
    collar = std::max(1, collar);

    // Window = the address hold PROPER, not the whole pre-takeaway period.
    // Teeing/setup earlier in the clip is also quasi-still but with the hands
    // AT the ball (tiny |B−G|) and pulled the whole-window median 107–178 px
    // short on the 2026-07-04 corpus. Seed on the last still frame within
    // `collar` of bs0 (speed smoothing bleeds the run end a few frames early),
    // take that frame's whole contiguous run, clip to [0, bs0+collar). No
    // stillness info / no hold near bs0 ⇒ the trailing collar frames before
    // takeaway (pre-bs0, so the grip is at its address position regardless).
    int lo = -1, hi = -1;
    if (still && int(still->size()) == nf) {
        int seed = -1;
        for (int j = std::min(bs0, nf - 1); j >= std::max(0, bs0 - collar) && seed < 0; --j)
            if ((*still)[size_t(j)]) seed = j;
        if (seed >= 0) {
            int rLo = seed, rHi = seed;
            while (rLo > 0 && (*still)[size_t(rLo - 1)]) --rLo;
            while (rHi + 1 < nf && (*still)[size_t(rHi + 1)]) ++rHi;
            lo = rLo;
            hi = std::min({rHi + 1, bs0 + collar, nf});
        }
    }
    if (lo < 0) { lo = std::max(0, bs0 - collar); hi = std::min(bs0, nf); }
    if (hi <= lo) return -1.0;

    // Pass 1 — component-wise median ball position over ALL found samples in
    // the window. The ball is stationary here by construction, so the true
    // samples form one tight cluster and the median lands inside it however
    // many detector warm-up mis-locks precede them. (A chained first-accepted
    // gate is order-DEPENDENT: one early mis-lock rejects every later good
    // sample — observed on the 2026-07-04 corpus.)
    std::vector<double> bxs, bys;
    for (int i = lo; i < hi; ++i) {
        const BallSample2D *s = nearestBallSample(ball, tUs[size_t(i)], frameToleranceUs);
        if (!s || !s->found) continue;
        bxs.push_back(s->center.x() * frameW);
        bys.push_back(s->center.y() * frameH);
    }
    if (int(bxs.size()) < kMinLenSamples) return -1.0;
    const auto median = [](std::vector<double> &v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    const double medX = median(bxs), medY = median(bys);

    // Golf-prior plausibility gate — the cluster gate keeps any CONSISTENT
    // lock, so a detector parked on the driver head at address sails through
    // it (gateA-0704: locked y 130–175 px above the truth ball, rung 1 shorted
    // −58..−164 px). Face-on the real ball is always between the feet and
    // BELOW the ankle line; a lock that isn't cannot be the ball, so abstain
    // (-1 → the ladder degrades honestly to the pose-scale rung). Gated only
    // when the window has kMinLenSamples usable ankle frames — pose-free
    // callers are accepted exactly as before.
    if (ankles && int(ankles->size()) == nf) {
        std::vector<double> ay, alx, arx;
        for (int i = lo; i < hi; ++i) {
            const AnklePx &a = (*ankles)[size_t(i)];
            if (!a.ok) continue;
            ay.push_back(a.ly); ay.push_back(a.ry);
            alx.push_back(a.lx); arx.push_back(a.rx);
        }
        if (int(alx.size()) >= kMinLenSamples) {
            const double ankleY = median(ay);
            if (medY <= ankleY - kBallBelowAnkleMarginFrac * frameH) {
                if (rejectReason) *rejectReason = 1;   // at/above the ankle line — not the ball
                return -1.0;
            }
            const double lX = median(alx), rX = median(arx);
            const double margin = kBallFeetMarginFrac * frameW;
            if (medX < std::min(lX, rX) - margin || medX > std::max(lX, rX) + margin) {
                if (rejectReason) *rejectReason = 2;   // outside the between-the-feet corridor
                return -1.0;
            }
        }
    }

    // Pass 2 — accept only samples inside the cluster; median |B−G| over them.
    std::vector<double> lenSamples;
    for (int i = lo; i < hi; ++i) {
        const BallSample2D *s = nearestBallSample(ball, tUs[size_t(i)], frameToleranceUs);
        if (!s || !s->found) continue;
        const double bx = s->center.x() * frameW, by = s->center.y() * frameH;
        if (std::hypot(bx - medX, by - medY) > kMaxJumpNormPx) continue;   // off-cluster — mis-lock
        lenSamples.push_back(std::hypot(bx - gx[size_t(i)], by - gy[size_t(i)]));
    }
    if (int(lenSamples.size()) < kMinLenSamples) return -1.0;
    // Accept path only: the gated cluster centre doubles as the pre-swing ball
    // anchor for the P7 impact geometry (see header contract).
    if (clusterBallPx) *clusterBallPx = QPointF(medX, medY);
    return median(lenSamples);
}

void applyBallAnchor(ShaftTrack2D &out, const BallTrack2D &ball,
                     const std::vector<double> &gx, const std::vector<double> &gy,
                     const std::vector<int64_t> &tUs, int frameW, int frameH,
                     int impf, const ShotAnalysisJob &job, ShaftDecideTrace *trace)
{
    if (ball.frames.empty() || out.samples.empty() || out.samples.size() != tUs.size())
        return;   // no ball data, or shapes don't line up — additive-only, no-op

    const int nf = int(out.samples.size());

    // Per-frame theta_ball(f) = atan2(B - G) wherever a stable ball sample is
    // available near this coverage frame (design §9.1) — the mis-lock jump gate
    // is inside the shared builder now (ball_anchor.h buildThetaBallSeries), so
    // this pass and the EventRefine Tier-A at-ball test agree by construction.
    //
    // Known asymmetry vs medianGripBallLenPx: the length measurement moved to
    // an order-independent two-pass cluster gate (a warm-up mis-lock there can
    // chain-reject every later good sample), but the θ series deliberately keeps
    // the sequential gate for now — θ anchoring is per-frame (no single robust
    // statistic to gate against) and its departure/abstain logic downstream is
    // corpus-validated as-is. Revisit if warm-up locks show up in tk0.
    //
    // TODO(gateA-0704): a CONSISTENTLY mis-locked ball (driver-head lock) also
    // feeds this θ-anchor path unchecked — the length measurement now rejects
    // it via the ankle-line/feet golf priors (medianGripBallLenPx), but the
    // per-frame θ anchors here have no equivalent plausibility gate yet.
    // Separate audit owed before the anchors are trusted on teed clubs.
    const ThetaBallSeries tb = buildThetaBallSeries(ball, gx, gy, tUs, frameW, frameH);
    const std::vector<bool>    &haveBall  = tb.haveBall;
    const std::vector<double>  &thetaBall = tb.thetaBall;
    const std::vector<QPointF> &ballPx    = tb.ballPx;

    // ── Exploit 1 (design §9.2, refined 2026-07-08) — tk0: first departure
    // of the club's own tracked theta from theta_ball, scanned forward from
    // the first ball sample, capped at bs0 (the current default — no earlier
    // boundary to search for past it). The departure threshold is ADAPTIVE:
    // a frame still counts as "pointing at the ball" if it's within the
    // fixed floor OR no worse than bs0's own departure — bs0 is itself
    // already inside the grip-speed heuristic's lagging boundary, so its own
    // alignment is a permissive, self-calibrating reference (Mark).
    const int bs0 = out.addressPhaseFrame;
    double effectiveDepartDeg = kTk0DepartDeg;
    if (bs0 >= 0 && bs0 < nf && haveBall[size_t(bs0)]) {
        const double refDist = angDiffDeg(out.samples[size_t(bs0)].thetaRad, thetaBall[size_t(bs0)]);
        effectiveDepartDeg = std::max(effectiveDepartDeg, refDist);
    }
    int tk0 = -1;
    const int searchEnd = (bs0 >= 0 && bs0 < nf) ? bs0 : nf;
    for (int i = 0; i < searchEnd && tk0 < 0; ++i) {
        if (!haveBall[size_t(i)]) continue;
        if (angDiffDeg(out.samples[size_t(i)].thetaRad, thetaBall[size_t(i)]) > effectiveDepartDeg) {
            tk0 = i;
        }
    }
    // No earlier departure found within [0,bs0) => bs0 itself stands (no
    // override), but exploit 4 below still has an address-hold bound to work
    // with rather than skipping entirely.
    if (tk0 < 0 && bs0 >= 0) tk0 = bs0;
    if (trace) trace->ballTk0Frame = tk0;

    // Override the reported address boundary when the ball found a genuinely
    // earlier one. Only meaningful when a trace sink is present — that's the
    // vision Segmentation adopted for camera-only swings (wrist_analyzer.cpp's
    // !hasImu fallback); fused swings' reported Address still comes from the
    // IMU today, untouched here (a separate, later question — Mark).
    // tk0 → Takeaway re-scope (LANDED): the ball's departure-from-θ_ball evidence
    // now refines Takeaway (and leaves Address on the addressHoldEndFrame path) in
    // the EventRefine slot (src/Analysis/event_refine.h — the LAST-departure/no-
    // return search, which supersedes this FIRST-departure tk0 that fired on the
    // first fidget). This in-DP tk0 stays report-only (trace->ballTk0Frame); the
    // W4 A/B override below is the frozen-off legacy overwrite path.
    // W4 A/B: ball.tk0AddressOverride — FROZEN OFF 2026-07-17 (the 17-swing
    // truth evaluation showed the first-departure tk0 fires on the first FIDGET
    // departure and overwrote a good hold-end Address; w2s4 −0.134 s → −1.533 s).
    // true restores the pre-freeze overwrite for A/B comparison.
    bool tk0AddressOverride = tuned::ball::kTk0AddressOverride;
    tuning::apply(job.tuningOverrides, "ball.tk0AddressOverride", tk0AddressOverride);
    if (tk0AddressOverride && trace && tk0 >= 0 && bs0 >= 0 && tk0 < bs0) {
        for (PhaseEvent &ev : trace->segmentation.events)
            if (ev.phase == Phase::Address) ev.t_us = tUs[size_t(tk0)];
        trace->segmentation.swingStartUs = tUs[size_t(tk0)];
    }

    // ── Exploit 4 (design §9.4/§9.5) — address discrimination + scale floor.
    // Publish ball-anchored theta across [firstBallFrame, tk0) wherever the DP
    // did NOT already produce a real measurement there (spanBound leaves the
    // address hold as a flat/coasted bridge by design — plan §2 is what makes
    // these frames reachable at all). Gated on agreement wherever a real
    // measurement DOES already exist; on disagreement, abstain (touch
    // nothing) rather than override it.
    // out.measuredClubLenPx (design §9.4) is now measured inside decideTrack
    // BEFORE head placement (A1 — medianGripBallLenPx over the address HOLD,
    // the last still run at bs0), so it can drive the length ladder; this
    // pass no longer recomputes it.
    const int addressEnd = tk0 >= 0 ? tk0 : nf;   // tk0 defaults to bs0 above; -1 only when bs0 itself is unset
    if (tk0 >= 0) {
        for (int i = 0; i < addressEnd; ++i) {
            if (!haveBall[size_t(i)]) continue;
            ShaftSample2D &sm = out.samples[size_t(i)];
            const bool measured = (sm.flags & ShaftMeasured) != 0;
            if (measured) {
                // Corroborate only — never overwrite a real measurement (leave
                // its head untouched). Disagreement (> tolerance) is an honesty
                // signal: abstain, i.e. do nothing further to this sample.
                if (angDiffDeg(sm.thetaRad, thetaBall[size_t(i)]) <= kAgreeTolDeg)
                    sm.flags |= ShaftBallAnchored;
                continue;
            }
            // A3 — at address the head IS the ball (a measurement, not a
            // projection): move headPx onto it, record the real visible length,
            // and clear ShaftHeadProjected so the overlay stops drawing the
            // stale projected terminus on the old θ.
            sm.thetaRad = thetaBall[size_t(i)];
            sm.gripPx   = QPointF(gx[size_t(i)], gy[size_t(i)]);
            sm.headPx   = ballPx[size_t(i)];
            sm.visibleLenPx = std::hypot(ballPx[size_t(i)].x() - gx[size_t(i)],
                                         ballPx[size_t(i)].y() - gy[size_t(i)]);
            sm.conf     = std::max(sm.conf, kAnchorConf);
            sm.flags    = uint16_t((sm.flags & ~ShaftHeadProjected) | ShaftBallAnchored);
        }
    }

    // ── Exploit 3 (design §9.3) — impact anchor. Last pre-launch frame: if
    // the DP didn't measure it directly (Coasted/ImuBridged/KinematicPredicted
    // — the §8.1 arm-witness bridge), soft-anchor from grip->ball. Ships
    // uncorrected (no delta lean-bias subtraction — that table is deferred).
    if (ball.launchTUs >= 0) {
        int fImp = -1;
        for (int i = 0; i < nf; ++i) {
            if (tUs[size_t(i)] <= ball.launchTUs) fImp = i; else break;
        }
        if (fImp >= 0) {
            ShaftSample2D &sm = out.samples[size_t(fImp)];
            if ((sm.flags & ShaftMeasured) == 0) {
                const double bx = ball.launchCenter.x() * frameW, by = ball.launchCenter.y() * frameH;
                sm.thetaRad = std::atan2(by - gy[size_t(fImp)], bx - gx[size_t(fImp)]);
                sm.gripPx   = QPointF(gx[size_t(fImp)], gy[size_t(fImp)]);
                // A3 — head is at the ball at impact too; same measured-head
                // treatment as the address block above.
                sm.headPx   = QPointF(bx, by);
                sm.visibleLenPx = std::hypot(bx - gx[size_t(fImp)], by - gy[size_t(fImp)]);
                sm.conf     = std::max(sm.conf, kAnchorConf);
                sm.flags    = uint16_t((sm.flags & ~ShaftHeadProjected) | ShaftBallAnchored);
            }
        }
    }
    Q_UNUSED(impf)
}

} // namespace pinpoint::analysis
