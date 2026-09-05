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

// The sparse-channel → MetricSeries plumbing every face-on producer repeats.
//
// head_track.cpp, foot_metrics.cpp and lower_body_metrics.cpp each carry their own private copy of
// medianOf / interpChannel / phaseTime / nearestIndex, written three times to the same contract
// ("linear interp, hold at the ends, bridge gaps, NEVER NaN"). A fourth, fifth, sixth and seventh
// copy landed with the face-on producer batch, so the contract is promoted here instead — one
// spelling, one set of semantics, every consumer.
//
// The three older producers were deliberately NOT migrated in the same change: they are corpus-gated
// and byte-compared, and a refactor that touches them would have to re-gate them to prove a
// no-op. They should move here the next time one of them is opened for a real reason.
//
// lower_body_metrics.cpp reached that next time (the validity mask below — it has to call
// channelValidityMask, and an unqualified call to its own private interpChannel / phaseTimeOpt /
// nearestIndex becomes AMBIGUOUS the moment this header is included). Its three copies were deleted
// rather than renamed: they were character-for-character the same functions, save for the empty-grid
// guard nearestIndex() here adds and that one cannot fire (the caller checks the grid first).
// head_track.cpp and foot_metrics.cpp still carry theirs.
//
// Pure, header-only, Qt-only (no OpenCV / no Qt-GUI). Everything is deterministic and testable
// without video.

#include "swing_analysis.h"          // MetricSeries, PhaseEvent, Phase
#include "../Core/pp_tuned_constants.h"   // tuned::channel:: (the bridge allowance)

#include <QtGlobal>                 // Q_ASSERT — see medianSigmaOverValid on why not ppWarn

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace pinpoint::analysis {

// One sparse channel: the valid subset of frames, ascending in t_us. A frame whose geometry could
// not be resolved is simply ABSENT — never a sentinel and never a zero, because a channel that
// substitutes zero for "not measured" grades as a confident reading of nothing.
struct MetricChannel {
    std::vector<int64_t> t_us;
    std::vector<double>  value;

    void push(int64_t t, double v) { t_us.push_back(t); value.push_back(v); }
    bool empty() const { return t_us.empty(); }
    size_t size() const { return t_us.size(); }
};

// Median of a copy. Order-independent by construction, which is what makes it safe against a
// detector warm-up mis-lock on the opening frames (see ball_position.h's pass-1 note).
inline double medianOfCopy(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1u) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Linear interpolation of an ascending sparse channel at t: hold at the ends, bridge gaps, never
// NaN. A low-confidence run coasts across rather than dropping the metric out.
inline double interpChannel(const std::vector<int64_t> &xs, const std::vector<double> &ys, int64_t x)
{
    if (xs.empty()) return 0.0;                     // guarded by the caller (channel non-empty)
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back())  return ys.back();
    const auto it = std::lower_bound(xs.begin(), xs.end(), x);
    const size_t hi = size_t(it - xs.begin());
    const size_t lo = hi - 1;
    const int64_t span = xs[hi] - xs[lo];
    if (span <= 0) return ys[lo];                   // coincident samples (defensive)
    const double f = double(x - xs[lo]) / double(span);
    return ys[lo] + (ys[hi] - ys[lo]) * f;
}

// Which resampled samples are a MEASUREMENT and which are a bridge over nothing.
//
// interpChannel() above never returns NaN: it holds at the ends and coasts across gaps, which is
// the right thing for a renderer and the wrong thing for a reducer. A one-frame confidence dropout
// bridged over 8 ms is a measurement to any useful precision; a run of frames the geometry gate
// refused (upper_body_metrics / lower_body_metrics: a body line turned out of the image plane) is
// not a measurement at all, and PEAK / PK RATE / a phase sample taken there is a confident reading
// of a straight line we drew ourselves.
//
// ⚠ TWO DIFFERENT REASONS A CHANNEL LACKS A FRAME, and they get opposite answers. This is the
// distinction the first version of this function missed, and the corpus gate caught it: on
// 2026-08-18 Wrist_01 swing_0001 a 10-frame GATED run in hipLineTilt at 7 ms spacing came back
// flagged VALID, because every frame of it sat within 60 ms of the last measurement — so the bridge
// (−28…−13°) was drawn and graded as a reading, which is the exact fabrication the design forbids.
// On swing_0003 that emitted a P4 shoulder-plane sample of 25.8° taken from thin air.
//
//   * THE KEYPOINTS WERE UNCONFIDENT — the pose detector dropped out. The geometry was there; we
//     just did not see it. Holding across it is a HOLD, and the budget below decides how long a
//     hold stays honest.
//   * THE PRODUCER GATED THE FRAME — the geometry was seen and REFUSED (a body line turned out of
//     the image plane, see lower_body_metrics / upper_body_metrics). There is nothing to hold: the
//     quantity did not exist at that instant. NO budget can excuse bridging it, so `gatedT` forces
//     0 whatever the spacing and whatever maxBridgeUs says.
//
// So: 1 where grid[i] lies within the bridge allowance of a real channel sample (and inside the
// channel's time extent) AND is not a gated instant, else 0. Returns an EMPTY vector when every
// sample is valid — that is the common case, it is what every series that predates this field
// carries, and MetricSeries::valid's contract makes empty mean "all valid" precisely so those series
// serialise byte-identically.
//
// THE ALLOWANCE IS NOT A CONSTANT: max(maxBridgeUs, spacingFactor × the LOCAL grid spacing).
//
// ⚠ THE GRID IS NOT UNIFORMLY SAMPLED, and a fixed budget gets the sparse end of it wrong.
// PoseRunner poses every frame only inside the dense zone; the address region is sampled at
// addressStride 15 (≈100 ms at 150 fps) or coarseStride 12 (≈80 ms) — pose_runner.h. One dropped
// sample there is 80–100 ms from its neighbours, so a fixed 60 ms would mark it, and that is the
// wrong answer: across one missing sample of a still, sparsely posed address, holding the previous
// value is a HOLD, not a fabrication — there is nothing happening in between to misrepresent.
// Mid-swing the spacing is ≈8 ms, so the fixed floor dominates and a genuinely gated run of a tenth
// of a second is still marked. Local spacing is the LARGER of the two neighbour gaps, so the
// allowance does not collapse on the sparse side of a stride change.
//
// OUTSIDE THE EXTENT IS INVALID whatever the allowance says, because the value there is not a bridge
// between two measurements at all: it is a constant hold past the last one. That is the honest
// reading of a channel that started late (the ball ruler resolved mid-swing, the elbows were only
// confident after address) and it is why the gate is not simply a distance.
// `gatedT` is the ASCENDING list of instants the producer refused on geometry — not every instant the
// channel lacks. A frame missing for want of confidence must NOT appear in it, or a one-frame
// detector dropout stops bridging and every producer regresses to a hole.
inline std::vector<uint8_t> channelValidityMask(const std::vector<int64_t> &grid,
                                                const std::vector<int64_t> &channelT,
                                                int64_t maxBridgeUs,
                                                double spacingFactor
                                                    = tuned::channel::kBridgeSpacingFactor,
                                                const std::vector<int64_t> &gatedT = {})
{
    std::vector<uint8_t> mask;
    if (grid.empty())
        return mask;                                // nothing to mark
    if (channelT.empty())
        return std::vector<uint8_t>(grid.size(), 0u);   // nothing was measured anywhere

    mask.assign(grid.size(), 1u);
    bool anyInvalid = false;
    for (size_t i = 0; i < grid.size(); ++i) {
        const int64_t t = grid[i];
        // Gated first, and unconditionally: the geometry was seen and refused, so there is no
        // measurement anywhere near this instant to bridge FROM in the sense the budget assumes.
        bool ok = !std::binary_search(gatedT.begin(), gatedT.end(), t);
        ok = ok && (t >= channelT.front() && t <= channelT.back());
        if (ok) {
            // lower_bound cannot return end() here: t <= back().
            const auto it = std::lower_bound(channelT.begin(), channelT.end(), t);
            int64_t d = *it - t;                    // >= 0
            if (it != channelT.begin())
                d = std::min(d, t - *(it - 1));

            int64_t localSpacing = 0;
            if (i > 0)
                localSpacing = std::max(localSpacing, grid[i] - grid[i - 1]);
            if (i + 1 < grid.size())
                localSpacing = std::max(localSpacing, grid[i + 1] - grid[i]);
            const int64_t allowance =
                std::max(maxBridgeUs, int64_t(spacingFactor * double(localSpacing)));

            ok = (d <= allowance);
        }
        if (!ok) {
            mask[i] = 0u;
            anyInvalid = true;
        }
    }
    if (!anyInvalid)
        mask.clear();                               // all valid ⇒ EMPTY, never an all-ones array
    return mask;
}

// The instant of a phase in the ladder, or nullopt when the segmenter never found it.
//
// ⚠ USE THIS, NOT phaseTime(), WHEN EMITTING A phaseSample. phaseTime() takes a `fallback` and
// every sample-emitting caller passed the grid front, so an UNSEGMENTED phase emitted a sample
// taken at the FIRST FRAME wearing that phase's label. It was masked while every producer asked
// only for Address / Top / Impact, which a successful segmentation always has. It stops being
// masked the moment anything samples P2/P3/P5/P6 — those are located by the P-position bridge and
// are genuinely missing on real swings — and the lie would not stay cosmetic: measure_sample.cpp
// falls back to the LABELLED sample where the curve has nothing, so a frame-0 reading labelled P6
// would be graded against a P6 corridor.
//
// An absent phase must produce NO sample. "We could not measure this" and "this is the value" are
// different statements, and merging them turns a segmentation gap into a confident number.
//
// The `fallback` form below survives for the OTHER use: picking a reference instant (the address
// anchor, the impact anchor) where coasting to the first or last frame is the intended behaviour
// and no phase label is attached to the result.
inline std::optional<int64_t> phaseTimeOpt(const std::vector<PhaseEvent> &phases, Phase p)
{
    for (const PhaseEvent &e : phases)
        if (e.phase == p) return e.t_us;
    return std::nullopt;
}

// The instant of a phase in the ladder, or `fallback` when the segmenter never found it. For
// reference instants only — see the warning on phaseTimeOpt() before using it to emit a sample.
inline int64_t phaseTime(const std::vector<PhaseEvent> &phases, Phase p, int64_t fallback)
{
    return phaseTimeOpt(phases, p).value_or(fallback);
}

// Index of the grid sample nearest t. The grid is the full per-frame timeline, so this is the frame
// a phase instant lands on.
inline int nearestIndex(const std::vector<int64_t> &grid, int64_t t)
{
    if (grid.empty()) return 0;
    if (t <= grid.front()) return 0;
    if (t >= grid.back())  return int(grid.size()) - 1;
    const auto it = std::lower_bound(grid.begin(), grid.end(), t);
    const int hi = int(it - grid.begin());
    const int lo = hi - 1;
    return (t - grid[lo] <= grid[hi] - t) ? lo : hi;
}

// ── σ PROPAGATION PRIMITIVES ────────────────────────────────────────────────────────────────
//
// Shared by lower_body_metrics.cpp and upper_body_metrics.cpp, which measure the same four body
// lines between them (hips, ankles, shoulders, elbows) under one sign convention — so they get one
// uncertainty convention too, from one place.
//
// THE ONE FACT EVERYTHING BELOW RESTS ON: `PoseKpAux::sigma` is a TRUE PER-AXIS σ, not a radial one
// and not an average of two different numbers. pose_smoother.cpp filters a keypoint's x and y with
// two Kalman/RTS passes that share q, dt, R AND the accept flag — a keypoint is rejected on both axes
// or neither — so their posterior variances are equal bit-for-bit and sqrt(0.5·(var_x + var_y)) is
// just that common σ written the long way (the invariant is pinned by a comment at the site). Every
// step below that treats the noise as ISOTROPIC is therefore exact rather than approximate: a formula
// reading only a keypoint's y, only its x, or a projection onto an arbitrary direction all use the
// same scalar unchanged.

// sqrt(a² + b²) — the first-order combination of two INDEPENDENT σ — and 0 when EITHER input is
// missing. Half an error budget is not a smaller one, it is an unknown one, so a quantity with one
// uncharacterised input reports no σ rather than an optimistic one. 0 in, 0 out, all the way up.
inline double quad2(double a, double b)
{
    return (a > 0.0 && b > 0.0) ? std::sqrt(a * a + b * b) : 0.0;
}

// σ (deg) of a body-line tilt — `atan2(Δy, |Δx|)`, the one convention lower_body_metrics' hip and
// ankle lines and upper_body_metrics' shoulder and elbow lines all share.
//
// EXACT to first order rather than a small-angle stand-in, and that is why it divides by the line's
// EUCLIDEAN length where the tilt itself divides by |Δx|. With Δ a difference of two independent
// keypoints, var(Δx) = var(Δy) = σ_a² + σ_b² (per-axis σ, see above), and the partials
//     ∂θ/∂Δy = |Δx| / (Δx² + Δy²)        ∂θ/∂Δx = −Δy / (Δx² + Δy²)
// combine in quadrature to
//     σ_θ = sqrt(σ_a² + σ_b²) · sqrt(Δx² + Δy²) / (Δx² + Δy²) = sqrt(σ_a² + σ_b²) / L
// with L the Euclidean length — a keypoint σ divided by the LEVER ARM it acts through. Radians, so
// ×180/π to report degrees. Same shape as body_rotation.cpp's foreshortening σ.
//
// ⚠ THE ∂θ/∂Δx TERM IS THE WHOLE DIFFERENCE BETWEEN L AND |Δx|. Dropping it — reading the formula as
// "the y noise over the span" — is right only while the line is LEVEL, and understates σ by
// L/|Δx| = 1/cos(tilt) as the line steepens: 15 % at 30° of tilt, 41 % at 45°. Keeping it costs one
// multiply and is the honest answer at every tilt, so there is no version of this that omits it.
inline double lineTiltSigmaDeg(const QPointF &lead, const QPointF &trail,
                               double sigLead, double sigTrail)
{
    constexpr double kRadToDeg = 57.29577951308232;
    const double q = quad2(sigLead, sigTrail);
    if (q <= 0.0) return 0.0;                       // one end uncharacterised ⇒ no σ for the line
    const double dx = trail.x() - lead.x(), dy = trail.y() - lead.y();
    const double L  = std::sqrt(dx * dx + dy * dy);
    if (L <= 1e-9) return 0.0;                      // coincident ends: no lever arm, no σ
    return q / L * kRadToDeg;
}

// THE SERIES' σ: the median of a channel's per-sample σ over the samples the FINAL validity mask
// still calls measurements, or nullopt when none of them did. Lives here beside channelValidityMask
// because it is that mask's reader — the two are one rule seen from either end.
//
// TAKES THE BUILT SERIES, not a grid and a mask as two loose vectors. The first cut of this took
// (grid, channelT, channelSigma, valid) and the first two were both `vector<int64_t>` sitting next to
// each other — swap them at one of two call sites and every σ silently becomes the median over
// whichever frames happened to line up, with no symptom a test would notice. `m` carries both the grid
// (`m.t_us`) and the mask (`m.valid`) and cannot be transposed with `channelT`, whose meaning is
// different and whose length is not the grid's.
//
// `channelSigma` is PARALLEL to `channelT`: one propagated σ per pushed value, in the channel's own
// unit, 0 meaning "no σ for this sample". 0 is PoseKpAux::sigma's own sentinel and it stays one — the
// smoother produced no posterior for at least one joint the value was built from, and a partial error
// budget is UNKNOWN rather than small.
//
// ⚠ nullptr MEANS "THIS CHANNEL PROPAGATES NO σ BY DESIGN" and is a normal, silent answer — the
// upper-body `leadArmToTorso` is the case, and there will be others. A NON-null vector of the WRONG
// LENGTH is a different thing entirely: a producer that pushed a value without pushing its σ, so every
// σ after that point describes the wrong sample. That is a programming error, it is asserted, and the
// release build degrades to "no σ" rather than to a plausible wrong one. Passing an empty vector to
// mean "none" is exactly the ambiguity this signature exists to remove; pass nullptr.
//
// Q_ASSERT rather than ppWarn deliberately: this header is included by targets that link NO log at all
// (pp_log_stream.cpp's own note — six suites, including the two face-on producer suites, satisfy the
// linker with a stub or nothing), so a log line here would be a link-time dependency on every consumer,
// and reaching for qWarning() instead would put the one log on a second channel. A desync is a bug in
// the producer, not a condition in the data, and an assert is the instrument for that.
//
// ⚠ CALL THIS AFTER EVERY MASK, NOT DURING THE CHANNEL PASS. A gated run (refused geometry), a bridge
// longer than the budget and the post-Impact phase-domain tail are all frames no reducer may read —
// and they are also the frames whose geometry is most degenerate and whose propagated σ is largest, so
// averaging them in would bias the number the chart rounds by in the one direction that matters.
// `m.valid` EMPTY means every sample is valid, per MetricSeries::valid's contract, not "no samples".
//
// The MEDIAN, not the mean, for the reason body_rotation.cpp takes one: a few frames where the
// smoother had just re-acquired a keypoint carry a σ an order of magnitude above the rest, and a mean
// would let them set the display step for the whole swing.
//
// nullopt, never 0: MetricSeries::sigma absent means "not characterised" and 0 would mean "measured
// perfectly", which is never true. Callers write the field only when this returns a value.
inline std::optional<double> medianSigmaOverValid(const MetricSeries &m,
                                                  const std::vector<int64_t> &channelT,
                                                  const std::vector<double> *channelSigma)
{
    if (!channelSigma || channelT.empty())
        return std::nullopt;                // no σ track by design, or nothing was measured
    Q_ASSERT(channelSigma->size() == channelT.size());   // a desync is a producer bug — see above
    if (channelSigma->size() != channelT.size())
        return std::nullopt;                // …and in release it withholds σ rather than guessing
    std::vector<double> keep;
    keep.reserve(channelT.size());
    for (size_t i = 0; i < channelT.size(); ++i) {
        if (!((*channelSigma)[i] > 0.0))
            continue;                   // no smoothed σ for some joint this sample was built from
        if (!m.valid.empty() && m.valid[size_t(nearestIndex(m.t_us, channelT[i]))] == 0u)
            continue;                   // gated, over-bridged or out of domain — not a reading
        keep.push_back((*channelSigma)[i]);
    }
    if (keep.empty())
        return std::nullopt;
    return medianOfCopy(keep);
}

// The phases every face-on curve carries as `phaseSamples`.
//
// The REDUCTION does not depend on this list — measure_sample.h samples the curve itself at each
// segmented phase, so a measure reading P5 works whether or not P5 appears here. phaseSamples exist
// for the chart layer and for the one class of metric that has no curve at all (the setup scalars).
// Four is the useful set: the three the older producers already emit, plus the finish, which the
// balance and thorax-rotation measures read and which nothing sampled before.
inline const std::vector<Phase> &defaultPhaseSamples()
{
    static const std::vector<Phase> kPhases{ Phase::Address, Phase::Top, Phase::Impact,
                                            Phase::Finish };
    return kPhases;
}

// Resample one sparse channel onto the full frame grid and wrap it as a MetricSeries.
//
// UNSCORED, always: the corridor that judges this number lives in the diagnostics norm set and is
// resolved in the shot's own context (Diagnostics/metric_corridor.h). A producer that also carried
// a band would be a second, unversioned opinion about what is normal.
//
// Returns a series with an empty key when the channel is empty, which the caller drops. That is the
// refuse-don't-fabricate contract: no samples means no metric, not a metric that is zero.
//
// `gatedT` (ascending) is the instants the producer REFUSED on geometry, as distinct from the ones it
// simply lacks: they are 0 in the mask whatever the budget. See channelValidityMask.
//
// `maxBridgeUs` < 0 (the default) means DO NOT MASK: the resample bridges silently, exactly as it
// always has, and `valid` stays empty. Every producer that predates the mask keeps that behaviour
// and keeps serialising byte-identically; a producer that gates frames on geometry passes its
// configured budget (channel.maxBridgeUs) and gets the mask. 0 is a legitimate, strictest setting —
// only a sample sitting exactly on a measurement is valid — which is why the sentinel is negative.
inline MetricSeries buildChannelSeries(const std::vector<int64_t> &grid, const MetricChannel &ch,
                                       const QString &key, const QString &label, const QString &unit,
                                       const std::vector<PhaseEvent> &phases,
                                       const std::vector<Phase> &sampleAt = defaultPhaseSamples(),
                                       int64_t maxBridgeUs = -1,
                                       double spacingFactor = tuned::channel::kBridgeSpacingFactor,
                                       const std::vector<int64_t> &gatedT = {})
{
    MetricSeries m;
    if (ch.empty() || grid.empty())
        return m;

    m.key   = key;
    m.label = label;
    m.unit  = unit;
    m.t_us  = grid;
    m.value.resize(grid.size());
    for (size_t i = 0; i < grid.size(); ++i)
        m.value[i] = interpChannel(ch.t_us, ch.value, grid[i]);

    // The value stays filled either way — the renderer wants a continuous curve — and the mask says
    // which of it we actually measured. Empty when all of it was.
    if (maxBridgeUs >= 0)
        m.valid = channelValidityMask(grid, ch.t_us, maxBridgeUs, spacingFactor, gatedT);

    for (const Phase p : sampleAt) {
        const std::optional<int64_t> t = phaseTimeOpt(phases, p);
        if (!t) continue;                           // unsegmented — no sample, never a frame-0 one
        const int idx = nearestIndex(grid, *t);
        // AN INVALID INSTANT PRODUCES NO SAMPLE, for the same reason an unsegmented phase produces
        // none: measure_sample.cpp falls back to the LABELLED sample where the curve has nothing, so
        // a bridged value wearing a phase label would be graded against that phase's corridor.
        if (!m.valid.empty() && m.valid[size_t(idx)] == 0u)
            continue;
        m.phaseSamples.push_back({ p, grid[idx], m.value[size_t(idx)], QString() });
    }
    return m;
}

// A sample OUTSIDE THE METRIC'S PHASE DOMAIN is not a measurement of that quantity — mark it.
//
// Design docs/design/metric_presentation_honesty.md §5.1's domain table: `pelvisSway`, `pelvisLift`,
// `leadKneeDrift`, `plumbBobDistance`, `hipLineTilt`, `secondaryAxisTilt`, `spineSideBend`,
// `thoraxLateralDrift`, `shoulderPlaneAngle` and `elbowAlignment` are Address→Impact quantities.
// Past impact the pelvis and thorax have turned toward the target, so the frontal-plane projection
// of a LATERAL quantity is measuring rotation — `MetricDescriptor::stereoGain()`'s own comment says
// it: "turning the pelvis moves the APPARENT hip centre sideways with no sway at all". The +35 %
// sway step after impact in design §2's screenshot is that, and it is not noise: no amount of
// smoothing or windowing makes it a sway reading, because the quantity did not exist there.
//
// ⚠ THIS IS THE ONLY PLACE THE DOMAIN LEAK CAN BE CLOSED ONCE. The reducers cannot close it: the
// diagnostics engine caches an extremum per (lo, hi] span while the review card reduces a whole
// window, and those two agree only while a sample's windowed mean is the same number whoever asked
// — so a reducer may not clip its own support to its query (series_reduce.h says why, with W2's
// 20-of-514 measurement). Clip the support and out-of-domain samples still leak into every
// whole-window card; mark them INVALID here and no reduction at any query can draw on them, because
// they have stopped being measurements. Same mechanism as a gated frame, same reason.
//
// The VALUES are untouched: the curve is still continuous and still drawn (dashed, outside the
// domain, PpChartPlot.qml), the tooltip still prints it. What changes is what may be REDUCED, which
// is the whole shape of this design.
//
// ⚠ AND ONLY THE TAIL IS MARKED — see the paragraph above the function. The pre-Address head stays
// valid on purpose.
//
// ⚠ THE HEAD IS OPEN BY DEFAULT, AND ONLY THE POST-IMPACT TAIL IS MARKED. `first` is nullopt unless
// a caller asks for a bound, and no producer asks. Two reasons, both found by the 5-swing gate that
// followed the first cut, where every clamped summary card came back `partial: true`:
//
//   * THE CHART DOES NOT CLIP THE START SIDE. A domain whose first phase is Address IS the default
//     first phase, so `ChartMetrics::domainFor` reports `firstNarrowed = false` and the card's window
//     still begins at the series' first sample. Marking the pre-Address head invalid then puts the
//     card's own start edge inside a masked run, reduceAt finds nothing within ±15 ms, the edge falls
//     back to interpolation and the card says `partial` — on every swing, for a head nobody was
//     reducing over in the first place.
//   * THE PRE-ADDRESS SAMPLES ARE HONEST. The golfer is standing still, referenced to the address
//     frames themselves, so a pelvis or body-line reading there is a reading of address posture and
//     nothing is turned out of the image plane yet. The design's own still-address gate window is
//     [Address − 300 ms, Address] (§7 item 2, and the probe's STILL ADDRESS row) — it is measured
//     ENTIRELY on those samples, and marking them invalid would withdraw the evidence for the number
//     the phase is judged by.
//
// The TAIL is the whole of the problem this closes: past impact the projection stops being the
// quantity. So the domain here is half-open in practice — unbounded before, Impact-bounded after —
// and the two-argument form stays for a caller that genuinely has a bounded first phase.
//
// Each end is inclusive AT THE NEAREST GRID SAMPLE (nearestIndex — the same snap the chart's phase
// dots use, so the boundary sample the user sees on the domain edge is the boundary sample the
// reducers keep). An UNSEGMENTED or UNREQUESTED end is UNBOUNDED on that side: if the segmenter never
// found Impact there is no instant to mark a tail from, and marking one from a guess would withdraw
// real measurements. Neither end bounded ⇒ the series is left exactly as it was.
inline void applyPhaseDomainMask(MetricSeries &m, const std::vector<PhaseEvent> &phases,
                                 std::optional<Phase> first = std::nullopt,
                                 std::optional<Phase> last  = Phase::Impact)
{
    if (m.t_us.empty())
        return;
    const std::optional<int64_t> a = first ? phaseTimeOpt(phases, *first) : std::nullopt;
    const std::optional<int64_t> b = last  ? phaseTimeOpt(phases, *last)  : std::nullopt;
    if (!a && !b)
        return;                                     // no domain to apply — leave the series alone

    const int lo = a ? nearestIndex(m.t_us, *a) : 0;
    const int hi = b ? nearestIndex(m.t_us, *b) : int(m.t_us.size()) - 1;
    if (lo > hi)
        return;                                     // a ladder with Impact before Address: refuse

    if (m.valid.empty())
        m.valid.assign(m.t_us.size(), 1u);
    for (int i = 0; i < int(m.t_us.size()); ++i)
        if (i < lo || i > hi)
            m.valid[size_t(i)] = 0u;

    // A LABELLED reading outside the domain has to go with them. measure_sample.cpp falls back to
    // phaseSamples where the curve has nothing to say, so a Finish sample left behind here would be
    // graded against a corridor by the very path this mask exists to starve.
    m.phaseSamples.erase(std::remove_if(m.phaseSamples.begin(), m.phaseSamples.end(),
                                        [&](const PhaseSample &ps) {
                                            return ps.t_us < m.t_us[size_t(lo)]
                                                   || ps.t_us > m.t_us[size_t(hi)];
                                        }),
                         m.phaseSamples.end());

    // EMPTY MEANS EVERY SAMPLE VALID — never an all-ones array, the same discipline `sigma` and
    // channelValidityMask follow, so a swing whose grid stops at impact serialises as it always did.
    if (std::find(m.valid.begin(), m.valid.end(), uint8_t(0)) == m.valid.end())
        m.valid.clear();
}

// Append `s` to `out` unless it refused (empty key). Keeps every producer's emit block to one line
// per series and makes the refusal path impossible to forget.
inline void appendIfProduced(std::vector<MetricSeries> &out, MetricSeries &&s)
{
    if (!s.key.isEmpty())
        out.push_back(std::move(s));
}

} // namespace pinpoint::analysis
