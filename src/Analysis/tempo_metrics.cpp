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

#include "tempo_metrics.h"

#include <algorithm>
#include <cmath>

namespace pinpoint::analysis {

namespace {

// Per-event 1σ, seconds. Confidence widens it and nothing else — the
// score_uncertainty.cpp:110-114 rule ("a poorly-segmented swing reads as LESS
// CERTAIN, it never changes the central value"). conf is clamped because the
// vision ladder's flat 0.5 and the IMU gate-miss 0.25 are both in range but
// EventRefine writes ≥ 0.8 on a refined Address, and Impact is a hard 1.0.
double eventSigmaS(float conf, const TempoConfig &cfg)
{
    const double c = std::clamp(double(conf), 0.0, 1.0);
    return cfg.baseSigmaS * (1.0 + (1.0 - c) * cfg.confInflate);
}

} // namespace

TempoResult measureTempo(const Segmentation &seg, const TempoConfig &cfg)
{
    TempoResult r;
    if (!cfg.enabled)
        return r;

    // conf == 0 is the segmenter's own "bounds are just the window" signal — on
    // the IMU clampFallback path that means Address is the window edge and there
    // is no Top. Refuse the whole ladder rather than trust part of it.
    if (double(seg.conf) <= cfg.minConf)
        return r;

    const PhaseEvent *addr = seg.eventFor(Phase::Address);
    const PhaseEvent *top  = seg.eventFor(Phase::Top);
    const PhaseEvent *imp  = seg.eventFor(Phase::Impact);
    if (!addr || !top || !imp)
        return r;

    const double B = double(top->t_us - addr->t_us) / 1e6;   // backswing, s
    const double D = double(imp->t_us - top->t_us)  / 1e6;   // downswing, s
    if (B <= 0.0 || D <= 0.0)
        return r;   // non-monotone ladder — refuse (the segmenter's monotone pass
                    // can drop events, so this is reachable, not defensive noise)

    const double sA = eventSigmaS(addr->conf, cfg);
    const double sT = eventSigmaS(top->conf,  cfg);
    const double sI = eventSigmaS(imp->conf,  cfg);

    r.valid           = true;
    r.backswingS      = B;
    r.downswingS      = D;
    r.ratio           = B / D;
    r.impactUs        = imp->t_us;
    r.backswingSigmaS = std::hypot(sA, sT);

    // Correlated propagation of R = B/D = (T−A)/(I−T). Top appears in BOTH
    // terms, so it must be differentiated ONCE against the whole expression —
    // treating B and D as independent would count T twice with the wrong sign
    // and UNDERSTATE the dominant error term.
    //
    //   ∂R/∂A = −1/D        ∂R/∂T = (B + D)/D²        ∂R/∂I = −B/D²
    //
    const double dA = -1.0 / D;
    const double dT = (B + D) / (D * D);
    const double dI = -B / (D * D);
    r.ratioSigma = std::sqrt(dA * dA * sA * sA + dT * dT * sT * sT + dI * dI * sI * sI);

    return r;
}

std::vector<MetricSeries> buildTempoSeries(const Segmentation &seg, const TempoConfig &cfg)
{
    std::vector<MetricSeries> out;
    const TempoResult t = measureTempo(seg, cfg);
    if (!t.valid)
        return out;

    // Summary scalars use the same degenerate-MetricSeries shape as the
    // foot_metrics setup scalars (empty curve, one phaseSample) — see the
    // rationale in foot_metrics.h. The phase is IMPACT, not Address: these
    // describe the whole swing, and readers sample tempoRatio at phase 5.
    const auto push = [&](const QString &key, const QString &label, const QString &unit,
                          double value, double sigma) {
        MetricSeries m;
        m.key   = key;
        m.label = label;
        m.unit  = unit;
        m.sigma = sigma;
        m.phaseSamples.push_back({ Phase::Impact, t.impactUs, value, QString() });
        out.push_back(std::move(m));
    };

    push(QStringLiteral("tempoBackswing"), QStringLiteral("Backswing tempo"),
         QStringLiteral("s"), t.backswingS, t.backswingSigmaS);
    push(QStringLiteral("tempoRatio"), QStringLiteral("Tempo ratio"),
         QStringLiteral(":1"), t.ratio, t.ratioSigma);

    return out;
}

} // namespace pinpoint::analysis
