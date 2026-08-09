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

#include "swing_scorer.h"

#include "analysis_tuning.h"
#include "../Core/pp_tuned_constants.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace pinpoint::analysis {
namespace {

// One reference band. oneSidedDir: 0 = two-sided; +1 = penalise values BELOW mu
// (good side is above, e.g. lead-wrist flexion — cupping/extension is the fault);
// -1 = penalise values ABOVE mu (e.g. a bent lead arm).
struct ScoreBand {
    const char *key;
    Phase       phase;
    double      mu, sigma;
    int         oneSidedDir;
    double      weight;
    const char *region;
};

// PROVISIONAL Wrist (session type 1) bands — see docs/reference/wristmetrics.md for the evidence.
// flex/ext (bow/cup) is weighted HIGHEST: it drives clubhead speed more than the other
// axes (Sweeney forward-kinematic model). radial/ulnar (hinge) is down-weighted — it is
// the weakest IMU axis and a secondary speed contributor (Buchanan/Zhang handicap study:
// high-HCP golfers carry ~7 deg more deviation at impact). Centres/signs are confirmed on
// the wizard "check your sensors" page; the math/aggregation are correct regardless.
namespace tb = pinpoint::tuned::scoring::bands;
constexpr ScoreBand kWristBands[] = {
    { "leadWristFlexExt", Phase::Impact, tb::kFlexExtMu, tb::kFlexExtSigma, tb::kFlexExtOneSided, tb::kFlexExtWeight, "wrist"   },  // bow/cup — speed driver; cupping penalised
    { "leadWristRadUln",  Phase::Impact, tb::kRadUlnMu,  tb::kRadUlnSigma,  tb::kRadUlnOneSided,  tb::kRadUlnWeight,  "wrist"   },  // hinge — weak IMU axis, low weight
    { "forearmPronation", Phase::Impact, tb::kPronationMu, tb::kPronationSigma, tb::kPronationOneSided, tb::kPronationWeight, "forearm" },  // roll — no published benchmark
    { "leadArmFlexion",   Phase::Impact, tb::kArmFlexionMu, tb::kArmFlexionSigma, tb::kArmFlexionOneSided, tb::kArmFlexionWeight, "arm" },  // near-straight lead arm
};

// Deadband shape (design §B.1) — defaults frozen in pp_tuned_constants.h, overridable.
struct ScoreDeadband {
    double zIn  = pinpoint::tuned::scoring::kZIn;
    double zOut = pinpoint::tuned::scoring::kZOut;
    double p    = pinpoint::tuned::scoring::kFalloffPow;
};

// Per-session bands with SwingLab `score.*` overrides applied onto a mutable copy of the
// frozen defaults. Empty map ⇒ byte-identical to the constexpr table (apply is a no-op on miss).
std::vector<ScoreBand> scoreBandsFor(int sessionType, const QVariantMap &ov)
{
    if (sessionType != 1)   // only Wrist Motion is scored in v1
        return {};
    std::vector<ScoreBand> bands(std::begin(kWristBands), std::end(kWristBands));
    if (ov.isEmpty())
        return bands;
    namespace tn = pinpoint::analysis::tuning;
    for (ScoreBand &b : bands) {
        const QByteArray pfx = QByteArray("score.") + b.key + '.';
        tn::apply(ov, (pfx + "mu").constData(),          b.mu);
        tn::apply(ov, (pfx + "sigma").constData(),       b.sigma);
        tn::apply(ov, (pfx + "weight").constData(),      b.weight);
        tn::apply(ov, (pfx + "oneSidedDir").constData(), b.oneSidedDir);
    }
    return bands;
}

ScoreDeadband deadbandFor(const QVariantMap &ov)
{
    ScoreDeadband d;
    if (!ov.isEmpty()) {
        namespace tn = pinpoint::analysis::tuning;
        tn::apply(ov, "score.zIn",  d.zIn);
        tn::apply(ov, "score.zOut", d.zOut);
        tn::apply(ov, "score.p",    d.p);
    }
    return d;
}

const MetricSeries *findSeries(const std::vector<MetricSeries> &v, const QString &key)
{
    for (const MetricSeries &m : v)
        if (m.key == key) return &m;
    return nullptr;
}

double phaseValue(const MetricSeries &m, Phase p)
{
    for (const PhaseSample &s : m.phaseSamples)
        if (s.phase == p) return s.value;
    return m.value.empty() ? 0.0 : m.value.back();
}

QString phaseName(Phase p)
{
    static const char *kN[8] = { "address", "takeaway", "top", "transition",
                                 "downswing", "impact", "release", "finish" };
    // The Phase enum runs past this v1 table (MidBackswing = 8 upward); a band
    // authored at one of those must not index off the end.
    const int i = int(p);
    if (i < 0 || i >= 8) return QStringLiteral("phase");
    return QString::fromLatin1(kN[i]);
}

// Deadband + bounded falloff (design §B.1). |z|<=zIn → 100; ramps to ~0 at zOut; then 1.
// One-sided: the "good" side is clamped to 100. Sets *band to green/yellow/red.
double bandSubScore(double value, const ScoreBand &b, const ScoreDeadband &dz, QString *band)
{
    const double zIn = dz.zIn, zOut = dz.zOut, p = dz.p;
    double z = (value - b.mu) / b.sigma;
    if (b.oneSidedDir > 0 && z > 0) z = 0.0;   // good side (above mu) — no penalty
    if (b.oneSidedDir < 0 && z < 0) z = 0.0;   // good side (below mu) — no penalty
    z = std::abs(z);

    if (band) *band = (z <= zIn) ? QStringLiteral("green")
                     : (z <= zOut) ? QStringLiteral("yellow")
                                   : QStringLiteral("red");

    if (z <= zIn)  return 100.0;
    if (z >= zOut) return 1.0;
    const double t = (z - zIn) / (zOut - zIn);
    return std::max(1.0, 100.0 * (1.0 - std::pow(t, p)));
}

// Weighted geometric mean of (subScore, weight) pairs, floored at 1.
int weightedGeoMean(const std::vector<std::pair<double, double>> &sw)
{
    double wsum = 0.0, lnsum = 0.0;
    for (const auto &[s, w] : sw) { wsum += w; lnsum += w * std::log(std::max(s, 1.0)); }
    return wsum > 0.0 ? int(std::lround(std::exp(lnsum / wsum))) : 0;
}

} // namespace

ScoreBreakdown SwingScorer::score(const std::vector<MetricSeries> &series, int sessionType,
                                  const QVariantMap &overrides)
{
    ScoreBreakdown sb;
    const std::vector<ScoreBand> bands = scoreBandsFor(sessionType, overrides);
    const ScoreDeadband          deadband = deadbandFor(overrides);

    std::vector<std::pair<double, double>> overall;            // (subScore, weight)
    QHash<QString, std::vector<std::pair<double, double>>> byRegion, byPhase;

    for (const ScoreBand &b : bands) {
        const MetricSeries *m = findSeries(series, QString::fromLatin1(b.key));
        if (!m)
            continue;
        const double value = phaseValue(*m, b.phase);

        ScoredMetric sm;
        sm.key      = m->key;
        sm.value    = value;
        sm.mu       = b.mu;
        sm.sigma    = b.sigma;
        sm.oneSided = (b.oneSidedDir != 0);
        sm.phase    = b.phase;
        sm.weight   = b.weight;
        sm.subScore = bandSubScore(value, b, deadband, &sm.band);
        sb.metrics.push_back(sm);

        overall.push_back({ sm.subScore, b.weight });
        byRegion[QString::fromLatin1(b.region)].push_back({ sm.subScore, b.weight });
        byPhase[phaseName(b.phase)].push_back({ sm.subScore, b.weight });
    }

    sb.overall = weightedGeoMean(overall);
    for (auto it = byRegion.constBegin(); it != byRegion.constEnd(); ++it)
        sb.perRegion.insert(it.key(), weightedGeoMean(it.value()));
    for (auto it = byPhase.constBegin(); it != byPhase.constEnd(); ++it)
        sb.perPhase.insert(it.key(), weightedGeoMean(it.value()));
    return sb;
}

} // namespace pinpoint::analysis
