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

#include "../Analysis/dashboard_reductions.h"   // barDomain
#include "timeline_labels.h"                    // the one phase-tag vocabulary (hasPositionTag)

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
    //
    // ⚠ P-POSITIONS ONLY. A chip is labelled "P1→P2" from the two phases it spans, so a
    // phase with no P-position cannot be an endpoint — it would read "→P2". Skipping those
    // events also makes the chips the coaching windows a golfer already talks in (P1→P2,
    // P4→P5) instead of segments bounded by Takeaway and Max speed. The events themselves
    // are untouched: they still tick on the plot, they just do not bound a chip.
    const TimelineLabels tags;
    QVector<QPair<qint64, int>> ev;   // (t_us, phase)
    ev.reserve(phases.size());
    for (const QVariant &pv : phases) {
        const QVariantMap p = pv.toMap();
        const int ph = p.value(QStringLiteral("phase")).toInt();
        if (!tags.hasPositionTag(ph)) continue;
        ev.append({ p.value(QStringLiteral("t_us")).toLongLong(), ph });
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
    // ⚠ P-POSITIONS ONLY, for the same reason segments() skips them: the single caller
    // composes the free-drag window's name as "<near start>→<near end>", so an untagged
    // nearest phase would render "→P4" under the chart. Naming the window by the nearest
    // P-position instead is also the more useful answer — "the window is about P4→P6" is
    // what a reader wants, not that its edge happened to land beside Max speed.
    const TimelineLabels tags;
    int    best = -1;
    qint64 bestD = std::numeric_limits<qint64>::max();
    for (const QVariant &pv : phases) {
        const QVariantMap m = pv.toMap();
        const int ph = m.value(QStringLiteral("phase")).toInt();
        if (!tags.hasPositionTag(ph)) continue;
        const qint64 d = qAbs(m.value(QStringLiteral("t_us")).toLongLong() - us);
        if (d < bestD) { bestD = d; best = ph; }
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

QString ChartMetrics::shortLabel(const QString &key) const
{
    // Compact names for the tight gutters/cards/tooltips, read from the MANIFEST rather than
    // from a table here. This was a seven-entry QHash whose every value was character-for-
    // character the descriptor's own `shortLabel`, and a duplicate of authored data only
    // stays correct while nobody adds a metric: `forearmRotation` and the three `hm.*` rungs
    // landed in the catalogue with short names already written and drew their full labels in
    // the split-mode gutter, because the copy here had never heard of them.
    //
    // A key the catalogue does not know, or one whose descriptor leaves shortLabel empty,
    // returns "" so the caller falls back to the series' full label — the previous contract,
    // unchanged.
    const pinpoint::analysis::MetricDescriptor *d = m_catalogue.descriptor(key);
    return d ? d->shortLabel : QString();
}

// ── Corridor-bar backing — marshalling only; the maths is dashboard_reductions.h ─

QVariantMap ChartMetrics::barDomain(double greenLo, double greenHi,
                                    double amberLo, double amberHi,
                                    bool lowOpen, bool highOpen,
                                    double value, bool hasValue) const
{
    const pinpoint::analysis::BarDomain d =
        pinpoint::analysis::barDomain(greenLo, greenHi, amberLo, amberHi,
                                      lowOpen, highOpen, value, hasValue);
    return QVariantMap{ { QStringLiteral("lo"),    d.lo },
                        { QStringLiteral("hi"),    d.hi },
                        { QStringLiteral("valid"), d.valid } };
}

// ── Units ───────────────────────────────────────────────────────────────────────

QString ChartMetrics::shortUnit(const QString &unit) const
{
    // Percent of a body dimension. The denominator is what the phrase carries, and on this panel
    // the METRIC'S OWN NAME already carries it — "Sway" is a percentage of stance width because
    // that is what pelvis sway is measured in, and a reader who wants the denominator spelled out
    // has the Metric Library. Four units, one token.
    if (unit.startsWith(QLatin1Char('%')))
        return QStringLiteral("%");
    return unit;
}

QString ChartMetrics::formatBare(double v, const QString &unit) const
{
    const QString u = shortUnit(unit.isEmpty() ? QStringLiteral("°") : unit);
    const long long r = std::llround(v);
    // The leading "+" is a DEGREES-ONLY convention and is deliberately not generalised: these are
    // signed deviations from a reference posture, where the sign is the reading. A "+75 mph" or a
    // "+2 in" would be decoration on a quantity whose sign nobody is asking about.
    return ((r > 0 && u == QStringLiteral("°")) ? QStringLiteral("+") : QString())
           + QString::number(r);
}

QString ChartMetrics::formatValue(double v, const QString &unit) const
{
    const QString u = shortUnit(unit.isEmpty() ? QStringLiteral("°") : unit);
    // Degrees close up against the number, everything else takes a space. "12°" is one token to a
    // reader and "12mph" is a typo.
    return formatBare(v, unit) + (u == QStringLiteral("°") ? QString() : QStringLiteral(" ")) + u;
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
    const QSet<QString> plottable = plotted;   // the group walk below consumes `plotted`

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

    // ── Cross-cutting presets ───────────────────────────────────────────────────────────────
    //
    // A metric has one `group`, and the groups above are the presets derived from it. A coaching
    // read that deliberately spans groups needs a second, additive mechanism — MetricDescriptor's
    // `presets` — or the metric has to be taken out of the group it is properly filed under. The
    // plumb bob is the case: the hip centre over the stance and the tilt of the hip line are read
    // and graded together, while hip tilt's home stays with pelvis sway and lift.
    //
    // Manifest order again, both for the presets themselves and for the keys inside each, so this
    // list and the Metric Library sequence the same metrics the same way.
    //
    // ⚠ AT LEAST TWO MEMBERS, or the preset is not offered. A preset exists to put several curves
    // on screen together; a one-curve preset duplicates a legend chip and pads the combo with an
    // entry that says nothing the group does not. It also means the preset disappears honestly on
    // a swing that produced only one of its members — a plumb bob with no plumb-bob curve is not a
    // plumb bob.
    {
        std::vector<std::pair<QString, QStringList>> presets;
        QHash<QString, int> presetIndexOf;
        for (const pinpoint::analysis::MetricDescriptor *d : m_catalogue.all()) {
            if (!plottable.contains(d->key))
                continue;
            for (const QString &name : d->presets) {
                auto it = presetIndexOf.constFind(name);
                if (it == presetIndexOf.constEnd()) {
                    presetIndexOf.insert(name, int(presets.size()));
                    presets.push_back({ name, QStringList{ d->key } });
                } else {
                    presets[it.value()].second.append(d->key);
                }
            }
        }
        for (auto &pr : presets)
            if (pr.second.size() >= 2)
                groups.push_back(std::move(pr));
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
