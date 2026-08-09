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

// Tempo metrics — the two "free" segmentation-only summaries (shot_analyzer_design
// §A.6): how long the backswing takes, and the ratio between the two halves of the
// swing. Pure over a resolved Segmentation: no window, no context, no pose, no
// club. Deliberately the event_refine.{h,cpp} shape — engine here, ~20 lines of
// stage glue in wrist_analyzer.cpp.
//
// BASIS — Address→Top, NOT Takeaway→Top:
//
//     B (backswing) = Top    − Address
//     D (downswing) = Impact − Top
//     R (ratio)     = B / D
//
// This matches the metric catalogue's own descriptions of tempoBackswing /
// tempoRatio. Be aware it DIFFERS from the Takeaway-based definition used by the
// golf literature the reference band comes from (Tour Tempo / TPI: backswing
// 0.847 ± 0.111 s, ratio ~3:1, good players ~2.2–3.0:1), so this basis reads
// slightly HIGH against those figures by the Address→Takeaway gap. That gap is
// structurally small — Address ≤ Takeaway by construction on both ladders (the
// vision Address is the address-hold END and Takeaway is motion onset at bs0;
// the IMU Address is the end of the stillness run) — but it has never been
// measured, so tempoRatio's corridor is provisional until the corpus supplies
// the distribution.
//
// REFUSAL, NOT APPROXIMATION. The IMU segmenter has a documented degenerate path
// (phase_segmenter.cpp clampFallback) that emits ONLY {Address, Impact, Finish}
// with conf 0 and Address pinned to the window edge — no Top at all. A conf gate
// plus a hard "all three events present" check means an untrustworthy ladder
// produces NO series rather than a plausible-looking wrong number. Same contract
// as foot_metrics/head_track: no output beats garbage output.
//
// UNCERTAINTY. Top sits in BOTH the numerator and the denominator with opposite
// sign, so its timing error is doubly leveraged: a 30 ms Top error — exactly the
// validation plan's ≤30 ms target — moves the ratio ~15 %, over half the width of
// the whole 2.2–3.0:1 band. Real-capture Top error has never been characterised
// (the target is documented; no measured result exists). So every emitted series
// carries a propagated 1σ in MetricSeries::sigma, following score_uncertainty.h's
// rule that low confidence WIDENS the interval and never moves the central value.

#include <QString>
#include <QVariantMap>
#include <vector>

#include "swing_analysis.h"        // Segmentation, PhaseEvent, MetricSeries, Phase
#include "analysis_tuning.h"       // tuning::apply
#include "../Core/pp_tuned_constants.h"   // tuned::tempo::

namespace pinpoint::analysis {

// Tempo knobs. Defaults track the frozen constants (pp_tuned_constants.h tempo::);
// SwingLab sweeps them via "tempo.*" dotted keys. enabled=false is the OFF-parity
// path — buildTempoSeries returns empty, which IS byte-identical to not running.
struct TempoConfig {
    bool   enabled     = tuned::tempo::kEnabled;       // tempo.enabled
    double minConf     = tuned::tempo::kMinConf;       // tempo.minConf — refuse at/below this seg.conf
    double baseSigmaS  = tuned::tempo::kBaseSigmaS;    // tempo.baseSigmaS — 1σ event-timing floor (s)
    double confInflate = tuned::tempo::kConfInflate;   // tempo.confInflate

    static TempoConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        TempoConfig c;
        apply(ov, "tempo.enabled",     c.enabled);
        apply(ov, "tempo.minConf",     c.minConf);
        apply(ov, "tempo.baseSigmaS",  c.baseSigmaS);
        apply(ov, "tempo.confInflate", c.confInflate);
        return c;
    }
};

// The measured tempo of one swing. `valid` false ⇒ the ladder was refused and
// NOTHING should be emitted (see the refusal contract above); every other field
// is meaningless in that case.
struct TempoResult {
    bool    valid        = false;
    double  backswingS   = 0.0;    // Top − Address, seconds
    double  downswingS   = 0.0;    // Impact − Top, seconds
    double  ratio        = 0.0;    // backswingS / downswingS
    double  backswingSigmaS = 0.0; // 1σ on backswingS
    double  ratioSigma      = 0.0; // 1σ on ratio (correlated propagation — Top counted once)
    int64_t impactUs     = 0;      // where the summary phase-sample is anchored
};

// Measure tempo from a resolved event ladder. Returns valid=false (and leaves
// everything else zero) on: disabled, seg.conf <= minConf, any of
// Address/Top/Impact missing, or a non-positive backswing/downswing duration.
TempoResult measureTempo(const Segmentation &seg, const TempoConfig &cfg = {});

// Emit {tempoBackswing, tempoRatio} as Summary series — an EMPTY t_us/value curve
// plus a single Impact phaseSample carrying the value, and MetricSeries::sigma
// carrying the propagated 1σ. (The scalar-as-degenerate-MetricSeries convention
// is documented at length in foot_metrics.h; the Impact anchor is where readers
// sample tempoRatio — phase 5.)
// UNSCORED — these go to detail->series, never the scored local series. Empty
// when measureTempo refuses.
std::vector<MetricSeries> buildTempoSeries(const Segmentation &seg, const TempoConfig &cfg = {});

} // namespace pinpoint::analysis
