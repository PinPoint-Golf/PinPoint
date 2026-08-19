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

#include "wrist_resemblance.h"

#include "analysis_tuning.h"
#include "../Core/pp_tuned_constants.h"

#include <algorithm>
#include <cmath>

// WristResemblanceScorer computes the Archetype Similarity Index (descriptive resemblance
// to bowed, neutral, and cupped templates based on flexion-extension at Top and Impact).
// Because a static two-frame FE slice is biomechanically blind to secondary axes (RUD/PS)
// and release rate dynamics, this index functions strictly as a style classification, while
// the headline quality score is driven by the penalty-based assessment engine.

namespace pinpoint::analysis {
namespace {

namespace tr = pinpoint::tuned::scoring::resemblance;

// Instrument ladder: a HackMotion swing publishes `hm.<key>` and never the bare key.
// See swing_analysis.h's findSeriesByLadder for why this is not four separate loops.
const MetricSeries *findSeries(const std::vector<MetricSeries> &v, const QString &key)
{
    return findSeriesByLadder(v, key);
}

// FE value at a scored phase: the labelled phaseSample if present, else the last curve sample.
double phaseValue(const MetricSeries &m, Phase p)
{
    for (const PhaseSample &s : m.phaseSamples)
        if (s.phase == p) return s.value;
    return m.value.empty() ? 0.0 : m.value.back();
}

} // namespace

std::array<ResemblancePattern, 3> WristResemblanceScorer::patterns(const QVariantMap &ov)
{
    std::array<ResemblancePattern, 3> pats = {{
        { QStringLiteral("bowed"),   tr::kBowedMuTop,   tr::kBowedMuImpact,   tr::kBowedSigma   },
        { QStringLiteral("neutral"), tr::kNeutralMuTop, tr::kNeutralMuImpact, tr::kNeutralSigma },
        { QStringLiteral("cupped"),  tr::kCuppedMuTop,  tr::kCuppedMuImpact,  tr::kCuppedSigma  },
    }};
    // Discrimination literals are exposed as dotted keys but FROZEN — empty map ⇒ defaults
    // (validation A.5 #15; the SwingLab sweep refuses score.* until labels exist).
    if (!ov.isEmpty()) {
        namespace tn = pinpoint::analysis::tuning;
        for (ResemblancePattern &p : pats) {
            const QByteArray pfx = QByteArray("score.resemblance.") + p.label.toLatin1() + '.';
            tn::apply(ov, (pfx + "muTop").constData(),    p.muTop);
            tn::apply(ov, (pfx + "muImpact").constData(), p.muImpact);
            tn::apply(ov, (pfx + "sigma").constData(),    p.sigma);
        }
    }
    return pats;
}

double WristResemblanceScorer::blendedDeltaPts(const QVariantMap &ov)
{
    double d = tr::kBlendedDeltaPts;
    if (!ov.isEmpty())
        pinpoint::analysis::tuning::apply(ov, "score.resemblance.blendedDeltaPts", d);
    return d;
}

ScoreBreakdown WristResemblanceScorer::score(const std::vector<MetricSeries> &series,
                                             const QVariantMap &ov)
{
    ScoreBreakdown sb;
    sb.kind = ScoreKind::Resemblance;

    const std::array<ResemblancePattern, 3> pats = patterns(ov);
    const double blendedDelta = blendedDeltaPts(ov);

    const MetricSeries *fe = findSeries(series, QStringLiteral("leadWristFlexExt"));
    if (!fe) {                       // no FE channel ⇒ resemblance is undefined
        sb.overall      = 0;
        sb.patternLabel = QStringLiteral("unknown");
        return sb;
    }
    const double xTop    = phaseValue(*fe, Phase::Top);
    const double xImpact = phaseValue(*fe, Phase::Impact);

    int best = 0, bestR = -1, secondR = -1;
    for (int i = 0; i < 3; ++i) {
        const ResemblancePattern &p = pats[size_t(i)];
        const double dTop = (xTop    - p.muTop)    / p.sigma;
        const double dImp = (xImpact - p.muImpact) / p.sigma;
        const double d2   = dTop * dTop + dImp * dImp;
        const int R = int(std::lround(100.0 * std::exp(-0.5 * d2)));
        sb.resemblance.insert(p.label, R);
        if (R > bestR)      { secondR = bestR; bestR = R; best = i; }
        else if (R > secondR) { secondR = R; }
    }

    sb.overall      = std::max(bestR, 0);
    sb.patternLabel = pats[size_t(best)].label;
    sb.blended      = (secondR >= 0) && (bestR - secondR) <= int(std::lround(blendedDelta));
    return sb;
}

} // namespace pinpoint::analysis
