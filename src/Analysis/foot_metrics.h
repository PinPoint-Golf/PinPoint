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

// Setup + footwork metrics (WB3, wholebody_pose_design.md §2.1/§5). Structure
// mirrors head_track.h deliberately (same shape: a per-frame state pass, a
// robust Address reference, Address-referenced sparse channels, a MetricSeries
// builder) — pure, deterministic, Qt-only (no OpenCV/ORT); unit-tested
// standalone.
//
// It reads only the COCO-WholeBody foot keypoints: L-bigtoe(17)/L-heel(19),
// R-bigtoe(20)/R-heel(22) — heel + bigtoe of a foot must BOTH clear the
// confidence gate for that foot to contribute anything this frame (every
// derived quantity below — stance width, flare, toe-line, heel-lift — needs
// both points of at least one foot). Legacy tracks analysed before WB0 (17 kp,
// conf[17..] defaulted to 0) therefore never clear the gate ⇒ trackFeet()
// leaves `valid` false and buildFootSeries() emits nothing (no output, never
// garbage) — the same contract head_track.h has for an all-low-confidence
// track.
//
// "Lead" foot follows the SAME handedness convention already established for
// the lead arm (pose_runner.cpp: `leftLeads = (handedness != 2)`,
// metric_extractor.cpp: `leftArm = (handedness != 2)`) — the caller resolves
// `leadIsLeft` once and passes it in; this module does not read handedness
// itself.
//
// SCALAR-METRIC REPRESENTATION (design decision — see wrist_analyzer.cpp
// integration + docs/reference/swing_json_schema.md 2026-07-13 note): stance
// width / per-foot flare / toe-line angle are ONE measurement each, taken at
// address — not a per-frame curve. No dedicated "scalar metric" shape exists
// anywhere in the codebase (ScoreBreakdown::metrics / ScoredMetric is scorer-
// internal audit trail and is never serialized to swing.json at all — see
// swing_doc.cpp's serializeScore(), which has no analogue of `metrics`).
// MetricSeries, by contrast, is already the one persisted/consumed shape
// (swing_doc.cpp, disk_replay_source.cpp, shot_processor.cpp, PpMetricChart),
// and every one of those readers/writers loops over `t_us`/`value`/
// `phaseSamples` generically with no non-empty-curve assumption. So the setup
// scalars use the representation that contract ALREADY tolerates cleanly: a
// MetricSeries with an EMPTY `t_us`/`value` curve and a single Address
// `phaseSamples` entry carrying the one measurement. This needs no
// reader/writer change anywhere (swing_doc.cpp is explicitly off-limits for
// this packet) and keeps each scalar's own key/label/unit — cramming multiple
// unrelated quantities into one series' phaseSamples (or inventing a bespoke
// "setup" block) was rejected as it would corrupt the existing convention that
// a series' phaseSamples are the SAME quantity sampled at different phases.
// `leadHeelLift` is a genuine per-frame curve and uses the normal shape,
// identical to head_track's sway/lift.
//
// ISOTROPY: like head_track.h, all geometry (distances, angles) is computed in
// PIXELS via (frameW, frameH) separately, then any px DISTANCE is re-normalized
// by the SINGLE reference dimension frameW (matching head_track's sway/lift
// convention) so `stanceWidth`/`leadHeelLift` are isotropic "×frame" units.
// Angles are dimensionless degrees and need no re-normalization.

#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <vector>

#include "swing_analysis.h"          // PoseTrack2D, PoseFrame2D, MetricSeries, PhaseEvent
#include "analysis_tuning.h"         // tuning::apply
#include "../Core/pp_tuned_constants.h"   // tuned::foot::

namespace pinpoint::analysis {

// Foot-tracking knobs. Defaults track the frozen constants (pp_tuned_constants.h
// foot::); SwingLab sweeps them via "foot.*" dotted keys.
struct FootMetricsConfig {
    double  confMin       = tuned::foot::kConfMin;        // foot.confMin
    int     addrMinFrames = tuned::foot::kAddrMinFrames;   // foot.addrMinFrames
    int64_t addrWindowUs  = tuned::foot::kAddrWindowUs;    // foot.addrWindowUs

    static FootMetricsConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        FootMetricsConfig c;
        apply(ov, "foot.confMin",       c.confMin);
        apply(ov, "foot.addrMinFrames", c.addrMinFrames);
        apply(ov, "foot.addrWindowUs",  c.addrWindowUs);
        return c;
    }
};

// Per-frame foot state. `leadValid`/`trailValid` require BOTH that foot's heel
// AND bigtoe to clear confMin this frame — the single validity bit every
// derived quantity below keys off. A frame with neither foot valid leaves both
// false (the channel skips it — resampling bridges the gap, never NaN).
struct FootState {
    int64_t t_us       = 0;
    bool    leadValid  = false;
    bool    trailValid = false;
    QPointF leadHeelPx,  leadToePx;    // px (frameW/H de-normalized); valid iff leadValid
    QPointF trailHeelPx, trailToePx;   // px; valid iff trailValid
    float   leadConf  = 0.f;           // mean of the two contributing confidences
    float   trailConf = 0.f;
};

// Address-measured setup geometry. Each quantity has its own `*Valid` flag —
// stanceWidth/toeLine need BOTH feet valid on a reference frame; flare needs
// only its own foot.
struct FootSetup {
    bool   stanceWidthValid = false;
    double stanceWidthXFrame = 0.0;    // heel-to-heel, isotropic px / frameW ("×frame")
    // Shoulder-to-shoulder distance in px, over the SAME address reference frames the stance is
    // measured on. This is the denominator that turns stance width into a body-relative reading:
    // a norm in millimetres is a norm on the golfer's height, and "a shoulder-width stance" is
    // both scale-free and how the stance is actually described.
    bool   shoulderWidthValid = false;
    double shoulderWidthPx = 0.0;
    // The address heel pair itself (px), component-wise median over the SAME
    // reference frames stanceWidth is measured from — so anything built on it
    // shares stanceWidth's denominator by construction rather than by luck.
    // Exported for ball_position.h (ball-along-the-stance needs the line, not
    // just its length); `heelsValid` carries the same both-feet-valid condition
    // as stanceWidthValid and is a separate flag purely for readability.
    bool    heelsValid = false;
    QPointF leadHeelPxAddr, trailHeelPxAddr;
    bool   leadFlareValid = false;
    double leadFlareDeg = 0.0;         // lead heel→bigtoe vs image +x axis, degrees
    bool   trailFlareValid = false;
    double trailFlareDeg = 0.0;        // trail heel→bigtoe vs image +x axis, degrees
    bool   toeLineValid = false;
    double toeLineDeg = 0.0;           // lead-bigtoe→trail-bigtoe vs image +x axis, degrees
};

struct FootMetricsResult {
    std::vector<FootState> states;    // one per input frame (time order)
    FootSetup setup;
    // Address-referenced lead-heel-lift sparse channel (valid-subset t_us,
    // ascending), already isotropic ×frame units. elevDiff(t) = leadToeY(t) −
    // leadHeelY(t) in px (image y-down, so elevDiff grows as the heel rises
    // above the toe); liftValue = elevDiff(t) − elevDiff(address), so it is
    // zero at address and POSITIVE when the heel lifts relative to it.
    std::vector<int64_t> liftTUs;
    std::vector<double>  liftValue;
    int  frameW = 0, frameH = 0;
    bool valid  = false;   // address reference resolved (≥1 lead-valid frame)
};

// Per-frame foot state + Address-measured setup geometry + the lead-heel-lift
// trace, from the (smoothed, else raw) pose track. `leadIsLeft` selects which
// physical foot (COCO L/R) is "lead" — same handedness convention as the lead
// arm (caller resolves it, e.g. `handedness != 2`). addressUs = the Address
// phase instant for the robust reference; < 0 ⇒ fall back to the first
// cfg.addrMinFrames lead-valid frames.
FootMetricsResult trackFeet(const PoseTrack2D &pose, int frameW, int frameH, bool leadIsLeft,
                            int64_t addressUs = -1, const FootMetricsConfig &cfg = {});

// Resample the lead-heel-lift channel onto the full per-frame grid (linear
// interp, hold at ends, gaps bridged — NEVER NaN) and emit `leadHeelLift`
// (curve, Address/Top/Impact phase samples) plus the address-only setup
// scalars `stanceWidth` / `leadFootFlare` / `trailFootFlare` / `toeLineAngle`
// (empty curve, single Address phaseSample — see the header note above).
// UNSCORED (no reference bands until a corpus exists). Empty when the address
// reference is unresolved (no lead foot anywhere, e.g. a legacy 17-kp track).
//
// `mmPerPx` is the ball-diameter ruler (ball_position.h) at the ball's
// ground-plane depth, which face-on is essentially the feet's depth — so it is
// the right ruler for stance geometry. When it resolves (> 0), stanceWidth is
// emitted in REAL mm; when it does not, it falls back to the frame-relative
// "×frame" with an honest unit string. Same two-unit convention head_track uses
// for sway/lift via the inter-ear ruler, and `mmPerPx <= 0` is byte-identical to
// the pre-ruler behaviour.
//
// NB `leadHeelLift` stays "×frame" even when the ruler resolves. It shares the
// ruler and the depth plane, so converting it would be MORE internally
// consistent — but it is a separately-shipped live metric and changing its unit
// is its own gate, not a side effect of this one.
std::vector<MetricSeries> buildFootSeries(const FootMetricsResult &res,
                                          const std::vector<PhaseEvent> &phases,
                                          double mmPerPx = -1.0);

} // namespace pinpoint::analysis
