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

#include "chart_metrics.h"

#include "../Analysis/kinematic_sequence.h"     // kinematicSequenceNodes (pure reduction)
#include "../Analysis/dashboard_reductions.h"   // railCheckpoints / railRange / interpolateAtUs

#include <QHash>
#include <QSet>
#include <QtGlobal>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Linear interpolation of `value` at `pos` over the ascending `tUs` samples. Clamps to the
// end values outside the range. (Distinct from TimelineLabels::valueAtNearest, which snaps
// to the nearest sample — window edges want a true interpolated value.)
double interpAt(const QVariantList &tUs, const QVariantList &value, qint64 pos)
{
    const int n = qMin(tUs.size(), value.size());
    if (n == 0) return 0.0;
    if (pos <= tUs.at(0).toLongLong())     return value.at(0).toDouble();
    if (pos >= tUs.at(n - 1).toLongLong()) return value.at(n - 1).toDouble();

    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (tUs.at(mid).toLongLong() < pos) lo = mid + 1;
        else                                hi = mid;
    }
    const qint64 tHi = tUs.at(lo).toLongLong();
    const qint64 tLo = tUs.at(lo - 1).toLongLong();
    const double vLo = value.at(lo - 1).toDouble();
    const double vHi = value.at(lo).toDouble();
    if (tHi == tLo) return vLo;
    const double f = double(pos - tLo) / double(tHi - tLo);
    return vLo + (vHi - vLo) * f;
}

} // namespace

QVariantList ChartMetrics::segments(const QVariantList &phases, qint64 spanUs) const
{
    QVariantList out;

    // [0] = Full swing. Label is "Full" in QML; phaseA/phaseB = -1 mark it as the whole span.
    {
        QVariantMap full;
        full.insert(QStringLiteral("startUs"), qint64(0));
        full.insert(QStringLiteral("endUs"),   qMax<qint64>(1, spanUs));
        full.insert(QStringLiteral("phaseA"),  -1);
        full.insert(QStringLiteral("phaseB"),  -1);
        out.append(full);
    }

    // Adjacent phase pairs, ordered by time — mirrors swing_data_source.cpp.
    QVector<QPair<qint64, int>> ev;   // (t_us, phase)
    ev.reserve(phases.size());
    for (const QVariant &pv : phases) {
        const QVariantMap p = pv.toMap();
        ev.append({ p.value(QStringLiteral("t_us")).toLongLong(),
                    p.value(QStringLiteral("phase")).toInt() });
    }
    std::sort(ev.begin(), ev.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    for (int i = 0; i + 1 < ev.size(); ++i) {
        QVariantMap seg;
        seg.insert(QStringLiteral("startUs"), ev[i].first);
        seg.insert(QStringLiteral("endUs"),   ev[i + 1].first);
        seg.insert(QStringLiteral("phaseA"),  ev[i].second);
        seg.insert(QStringLiteral("phaseB"),  ev[i + 1].second);
        out.append(seg);
    }
    return out;
}

QVariantMap ChartMetrics::summary(const QVariantList &tUs, const QVariantList &value,
                                  qint64 startUs, qint64 endUs) const
{
    QVariantMap r;
    const int n = qMin(tUs.size(), value.size());

    if (startUs > endUs) std::swap(startUs, endUs);

    // Window edges, linearly interpolated.
    const double start = interpAt(tUs, value, startUs);
    const double end   = interpAt(tUs, value, endUs);

    // Extremes over the interpolated edges plus every sample strictly inside the window.
    double mn = qMin(start, end), mx = qMax(start, end);
    qint64 tMin = (start <= end) ? startUs : endUs;
    qint64 tMax = (start >= end) ? startUs : endUs;

    double rate = 0.0;                  // max |Δvalue/Δt|, deg per 100 ms
    bool   havePrev = false;
    double prevV = 0.0;
    qint64 prevT = 0;

    for (int i = 0; i < n; ++i) {
        const qint64 t = tUs.at(i).toLongLong();
        if (t < startUs || t > endUs) continue;
        const double v = value.at(i).toDouble();
        if (v < mn) { mn = v; tMin = t; }
        if (v > mx) { mx = v; tMax = t; }
        if (havePrev && t != prevT) {
            const double dv = qAbs(v - prevV) / (double(t - prevT) / 1.0e5);
            if (dv > rate) rate = dv;
        }
        prevV = v; prevT = t; havePrev = true;
    }

    const bool maxWins = qAbs(mx) >= qAbs(mn);
    const double peak  = maxWins ? mx : mn;
    const qint64 tPeak = maxWins ? tMax : tMin;

    r.insert(QStringLiteral("start"),   start);
    r.insert(QStringLiteral("end"),     end);
    r.insert(QStringLiteral("min"),     mn);
    r.insert(QStringLiteral("max"),     mx);
    r.insert(QStringLiteral("peak"),    peak);
    r.insert(QStringLiteral("range"),   mx - mn);
    r.insert(QStringLiteral("delta"),   end - start);
    r.insert(QStringLiteral("rate"),    rate);
    r.insert(QStringLiteral("tPeakUs"), tPeak);
    return r;
}

QVariantList ChartMetrics::niceTicks(double lo, double hi, int maxTicks) const
{
    QVariantList out;
    if (!(hi > lo) || maxTicks < 1) return out;

    const double span = hi - lo;
    const double raw  = span / maxTicks;
    const double mag  = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    const double step = (norm < 1.5 ? 1.0 : norm < 3.0 ? 2.0 : norm < 7.0 ? 5.0 : 10.0) * mag;
    if (!(step > 0.0)) return out;

    // Guard against a pathological range producing a huge tick count.
    for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-6; v += step) {
        if (out.size() > 64) break;
        out.append(double(qRound64(v * 1.0e4)) / 1.0e4);   // tidy float noise
    }
    return out;
}

QVariantList ChartMetrics::timeTicksMs(qint64 domStartUs, qint64 domEndUs,
                                       qint64 impactUs) const
{
    QVariantList out;
    if (domEndUs <= domStartUs) return out;

    const double spanMs = double(domEndUs - domStartUs) / 1000.0;
    const int step = spanMs > 900 ? 200 : spanMs > 400 ? 100 : spanMs > 180 ? 50 : 20;

    const double startMsFromImpact = double(domStartUs - impactUs) / 1000.0;
    for (int ms = int(std::ceil(startMsFromImpact / step)) * step;
         impactUs + qint64(ms) * 1000 <= domEndUs; ms += step) {
        if (impactUs + qint64(ms) * 1000 >= domStartUs) out.append(ms);
        if (out.size() > 64) break;
    }
    return out;
}

int ChartMetrics::nearestPhase(const QVariantList &phases, qint64 us) const
{
    int    best = -1;
    qint64 bestD = std::numeric_limits<qint64>::max();
    for (const QVariant &pv : phases) {
        const QVariantMap m = pv.toMap();
        const qint64 d = qAbs(m.value(QStringLiteral("t_us")).toLongLong() - us);
        if (d < bestD) { bestD = d; best = m.value(QStringLiteral("phase")).toInt(); }
    }
    return best;
}

QString ChartMetrics::bandAtNearest(const QVariantList &phaseSamples, qint64 us) const
{
    QString best;
    qint64  bestD = std::numeric_limits<qint64>::max();
    for (const QVariant &pv : phaseSamples) {
        const QVariantMap m = pv.toMap();
        const qint64 d = qAbs(m.value(QStringLiteral("t_us")).toLongLong() - us);
        if (d < bestD) { bestD = d; best = m.value(QStringLiteral("band")).toString(); }
    }
    return best.isEmpty() ? QStringLiteral("good") : best;
}

QVariantList ChartMetrics::sequenceNodes(const QVariantList &series,
                                         const QStringList &keys) const
{
    using pinpoint::analysis::SeqSeries;
    using pinpoint::analysis::SeqNode;

    // Index the incoming series by key so we can honour the caller's key ordering
    // and skip keys that are absent from this shot.
    QHash<QString, QVariantMap> byKey;
    byKey.reserve(series.size());
    for (const QVariant &sv : series) {
        const QVariantMap m = sv.toMap();
        byKey.insert(m.value(QStringLiteral("key")).toString(), m);
    }

    std::vector<SeqSeries> inputs;
    inputs.reserve(keys.size());
    for (const QString &key : keys) {
        const auto it = byKey.constFind(key);
        if (it == byKey.constEnd()) continue;          // series not present → dropped
        const QVariantList tUs = it->value(QStringLiteral("t_us")).toList();
        const QVariantList val = it->value(QStringLiteral("value")).toList();
        const int n = int(qMin(tUs.size(), val.size()));
        if (n == 0) continue;                          // no curve → dropped

        SeqSeries s;
        s.key = key;
        s.tUs.reserve(n);
        s.value.reserve(n);
        for (int i = 0; i < n; ++i) {
            s.tUs.push_back(tUs.at(i).toLongLong());
            s.value.push_back(val.at(i).toDouble());
        }
        inputs.push_back(std::move(s));
    }

    QVariantList out;
    for (const SeqNode &nd : pinpoint::analysis::kinematicSequenceNodes(inputs)) {
        out.append(QVariantMap{
            { QStringLiteral("key"),     nd.key },
            { QStringLiteral("tPeakUs"), qlonglong(nd.tPeakUs) },
            { QStringLiteral("peak"),    nd.peak },
            { QStringLiteral("gapMs"),   nd.gapMs },
            { QStringLiteral("order"),   nd.order } });
    }
    return out;
}

QString ChartMetrics::shortLabel(const QString &key) const
{
    // Compact names for the metric keys that need them in tight gutters/cards/tooltips.
    // Unknown keys return "" so the caller falls back to the series' full label.
    static const QHash<QString, QString> kShort = {
        { QStringLiteral("leadWristFlexExt"), QStringLiteral("Bow/cup") },
        { QStringLiteral("leadWristRadUln"),  QStringLiteral("Hinge")   },
        { QStringLiteral("forearmPronation"), QStringLiteral("Roll")    },
        { QStringLiteral("leadArmFlexion"),   QStringLiteral("Elbow")   },
        { QStringLiteral("clubheadSpeed"),    QStringLiteral("Club speed")},
        { QStringLiteral("handSpeed"),        QStringLiteral("Hand speed")},
        { QStringLiteral("lagAngle"),         QStringLiteral("Lag")     },
    };
    return kShort.value(key);
}

// ── PpBandRail backing — marshalling only; the maths is dashboard_reductions.h ───

QVariantList ChartMetrics::railCheckpoints(const QVariantList &phaseSamples,
                                           const QVariantList &corridors) const
{
    using namespace pinpoint::analysis;

    std::vector<RailSample> samples;
    samples.reserve(phaseSamples.size());
    for (const QVariant &v : phaseSamples) {
        const QVariantMap m = v.toMap();
        RailSample s;
        s.phase = m.value(QStringLiteral("phase"), -1).toInt();
        s.tUs   = m.value(QStringLiteral("t_us")).toLongLong();
        s.value = m.value(QStringLiteral("value")).toDouble();
        s.band  = m.value(QStringLiteral("band")).toString();
        samples.push_back(std::move(s));
    }

    std::vector<RailCorridor> cors;
    cors.reserve(corridors.size());
    for (const QVariant &v : corridors) {
        const QVariantMap m = v.toMap();
        RailCorridor c;
        c.phase   = m.value(QStringLiteral("phase"), -1).toInt();
        c.greenLo = m.value(QStringLiteral("greenLo")).toDouble();
        c.greenHi = m.value(QStringLiteral("greenHi")).toDouble();
        c.amberLo = m.value(QStringLiteral("amberLo")).toDouble();
        c.amberHi = m.value(QStringLiteral("amberHi")).toDouble();
        // Explicit booleans, never an absent key: `.toDouble()`/`.toBool()` on a missing
        // QVariantMap entry yields 0/false silently, so openness has to be written at every hop
        // or a floor quietly becomes a two-sided corridor somewhere along the chain.
        c.lowOpen  = m.value(QStringLiteral("lowOpen")).toBool();
        c.highOpen = m.value(QStringLiteral("highOpen")).toBool();
        cors.push_back(std::move(c));
    }

    QVariantList out;
    for (const RailPoint &p : pinpoint::analysis::railCheckpoints(samples, cors)) {
        out.append(QVariantMap{
            { QStringLiteral("phase"),       p.phase },
            { QStringLiteral("tUs"),         qlonglong(p.tUs) },
            { QStringLiteral("value"),       p.value },
            { QStringLiteral("band"),        p.band },
            { QStringLiteral("hasCorridor"), p.hasCorridor },
            { QStringLiteral("greenLo"),     p.greenLo },
            { QStringLiteral("greenHi"),     p.greenHi },
            { QStringLiteral("amberLo"),     p.amberLo },
            { QStringLiteral("amberHi"),     p.amberHi },
            { QStringLiteral("lowOpen"),     p.lowOpen },
            { QStringLiteral("highOpen"),    p.highOpen } });
    }
    return out;
}

QVariantMap ChartMetrics::railRange(const QVariantList &points) const
{
    using namespace pinpoint::analysis;

    // Round-trip the checkpoints back through the value type. Cheap (a handful of
    // phases) and keeps ONE domain rule — QML can hand back exactly what
    // railCheckpoints() gave it without a second, divergent shape.
    std::vector<RailPoint> pts;
    pts.reserve(points.size());
    for (const QVariant &v : points) {
        const QVariantMap m = v.toMap();
        RailPoint p;
        p.phase       = m.value(QStringLiteral("phase"), -1).toInt();
        p.tUs         = m.value(QStringLiteral("tUs")).toLongLong();
        p.value       = m.value(QStringLiteral("value")).toDouble();
        p.band        = m.value(QStringLiteral("band")).toString();
        p.hasCorridor = m.value(QStringLiteral("hasCorridor")).toBool();
        p.greenLo     = m.value(QStringLiteral("greenLo")).toDouble();
        p.greenHi     = m.value(QStringLiteral("greenHi")).toDouble();
        p.amberLo     = m.value(QStringLiteral("amberLo")).toDouble();
        p.amberHi     = m.value(QStringLiteral("amberHi")).toDouble();
        p.lowOpen     = m.value(QStringLiteral("lowOpen")).toBool();
        p.highOpen    = m.value(QStringLiteral("highOpen")).toBool();
        pts.push_back(std::move(p));
    }

    const RailRange r = pinpoint::analysis::railRange(pts);
    return QVariantMap{ { QStringLiteral("lo"),    r.lo },
                        { QStringLiteral("hi"),    r.hi },
                        { QStringLiteral("valid"), r.valid } };
}

double ChartMetrics::valueAtUs(const QVariantList &tUs, const QVariantList &value,
                               qint64 us) const
{
    const int n = int(qMin(tUs.size(), value.size()));
    std::vector<int64_t> t;
    std::vector<double>  v;
    t.reserve(n);
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        t.push_back(tUs.at(i).toLongLong());
        v.push_back(value.at(i).toDouble());
    }
    return pinpoint::analysis::interpolateAtUs(t, v, us);
}

QVariantMap ChartMetrics::barDomain(double greenLo, double greenHi,
                                    double amberLo, double amberHi,
                                    bool lowOpen, bool highOpen,
                                    double value, bool hasValue) const
{
    // Flat scalars rather than a corridor map: PpRangeBar carries its bounds as four
    // separate properties (it is fed by a caller that assembles them, not by a norm
    // set), so a map-taking signature would force one of the two bars to build a map
    // purely to hand it straight back.
    const pinpoint::analysis::BarDomain d =
        pinpoint::analysis::barDomain(greenLo, greenHi, amberLo, amberHi,
                                      lowOpen, highOpen, value, hasValue);
    return QVariantMap{ { QStringLiteral("lo"),    d.lo },
                        { QStringLiteral("hi"),    d.hi },
                        { QStringLiteral("valid"), d.valid } };
}

QVariantList ChartMetrics::scoreSegments(const QVariantMap &buckets) const
{
    std::vector<pinpoint::analysis::ScoreSegment> segs;
    segs.reserve(buckets.size());
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it)
        segs.push_back({ it.key(), it.value().toInt() });

    segs = pinpoint::analysis::orderScoreSegments(std::move(segs));

    QVariantList out;
    for (const pinpoint::analysis::ScoreSegment &s : segs)
        out.append(QVariantMap{ { QStringLiteral("label"), s.label },
                                { QStringLiteral("value"), s.value } });
    return out;
}

QString ChartMetrics::orientationLabel(double value, double greenLo, double greenHi) const
{
    return pinpoint::analysis::orientationLabel(value, greenLo, greenHi);
}

// ── Chart metric presets ────────────────────────────────────────────────────────

QVariantList ChartMetrics::seriesGroups(const QVariantList &seriesList) const
{
    // The keys this swing can actually DRAW. `> 1` (not `> 0`) matches PpMetricChart's own
    // _visible test: a single sample is a point, not a trace, and the plot skips it.
    QSet<QString> plotted;
    for (const QVariant &v : seriesList) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("t_us")).toList().size() > 1)
            plotted.insert(m.value(QStringLiteral("key")).toString());
    }
    if (plotted.isEmpty())
        return {};

    // Manifest order, preserved by walking all() once and appending a group the first time it
    // is seen. A QHash keyed on the group name would lose exactly the ordering the Metric
    // Library already presents these in, and two surfaces disagreeing about the order of the
    // same vocabulary is the kind of small wrongness that reads as a bug.
    std::vector<std::pair<QString, QStringList>> groups;
    QHash<QString, int> indexOf;
    for (const pinpoint::analysis::MetricDescriptor *d : m_catalogue.all()) {
        if (!plotted.remove(d->key))      // remove ⇒ whatever survives is uncatalogued
            continue;
        auto it = indexOf.constFind(d->group);
        if (it == indexOf.constEnd()) {
            indexOf.insert(d->group, int(groups.size()));
            groups.push_back({ d->group, QStringList{ d->key } });
        } else {
            groups[it.value()].second.append(d->key);
        }
    }

    // Whatever the manifest did not claim. Sorted: `plotted` is a QSet, whose iteration order is
    // unspecified and would otherwise make this group's contents vary between runs.
    if (!plotted.isEmpty()) {
        QStringList rest(plotted.cbegin(), plotted.cend());
        rest.sort();
        groups.push_back({ QStringLiteral("Other"), std::move(rest) });
    }

    QVariantList out;
    out.reserve(int(groups.size()));
    for (const auto &g : groups)
        out.append(QVariantMap{ { QStringLiteral("group"), g.first },
                                { QStringLiteral("keys"),  g.second } });
    return out;
}
