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
#include <vector>

namespace {

// The VALID samples of a series, lifted out of the QVariantLists once, with their original
// indices kept. Every reducer below reads this rather than the raw arrays, which is what makes
// "an invalid sample is not a measurement" true by construction instead of by five separate
// `if` statements that could each be forgotten.
//
// `src` is what the adjacency test needs: two entries whose original indices differ by more
// than one have invalid samples BETWEEN them, so an interpolation across that bracket reached
// over something unmeasured and the summary has to admit it (`partial`).
struct ValidSamples {
    std::vector<qint64> t;
    std::vector<double> v;
    std::vector<int>    src;       // index into the caller's arrays
    int size() const { return int(t.size()); }
};

// Collect them. `valid` is the C4 mask (swing.json `metrics[].valid`): EMPTY means every
// sample is valid — the state of every series written before the field existed, so the empty
// case must cost nothing and behave identically to the old code. A mask entry of 0 marks a
// sample the grid bridged across a gated or absent run.
//
// ⚠ THE SHORT-MASK RULE, and it is deliberately the same one measure_sample.cpp's buildPhaseGrid
// applies: a mask is honoured only when it covers the whole curve (`size >= n`); a SHORTER one is
// a malformed document rather than a partial statement and is treated as NO MASK AT ALL. Guessing
// which end it was truncated from would invent validity nobody told us about, and having the chart
// and the diagnostics grid disagree about a malformed file is worse than either answer. Everything
// that consults validity in this file — collectValid and measuredAt — goes through haveMask().
bool haveMask(int n, const QVariantList &valid)
{
    return n > 0 && valid.size() >= n;
}

ValidSamples collectValid(const QVariantList &tUs, const QVariantList &value,
                          const QVariantList &valid)
{
    ValidSamples s;
    const int n = qMin(tUs.size(), value.size());
    const bool masked = haveMask(n, valid);
    s.t.reserve(size_t(n)); s.v.reserve(size_t(n)); s.src.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        if (masked && valid.at(i).toInt() == 0) continue;
        s.t.push_back(tUs.at(i).toLongLong());
        s.v.push_back(value.at(i).toDouble());
        s.src.push_back(i);
    }
    return s;
}

// Linear interpolation at `pos` over the ascending valid samples. Clamps to the end values
// outside the range. (Distinct from TimelineLabels::valueAtNearest, which snaps to the nearest
// sample — window edges want a true interpolated value.)
//
// `bracketSkipped` reports whether the two samples it read from are non-adjacent in the
// ORIGINAL series, i.e. whether this edge value came from across a bridged run. A clamped edge
// (outside the valid extent) never sets it: the invalid samples it was clamped past are inside
// the window and set `partial` on the window-scan rule instead.
double interpValid(const ValidSamples &s, qint64 pos, bool *bracketSkipped = nullptr)
{
    if (bracketSkipped) *bracketSkipped = false;
    const int n = s.size();
    if (n == 0) return 0.0;
    if (pos <= s.t.front()) return s.v.front();
    if (pos >= s.t.back())  return s.v.back();

    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (s.t[size_t(mid)] < pos) lo = mid + 1;
        else                        hi = mid;
    }
    // ⚠ `pos < t[lo]` matters: the search leaves t[lo-1] < pos <= t[lo], and when pos lands
    // EXACTLY on a valid sample the value is that sample's, whatever is on the other side of it.
    // Without this a window edge sitting on the first good sample after a bridged run reported
    // itself interpolated, and every such card wore a PARTIAL chip it had not earned.
    if (bracketSkipped)
        *bracketSkipped = pos < s.t[size_t(lo)]
                          && (s.src[size_t(lo)] - s.src[size_t(lo - 1)]) > 1;

    const qint64 tHi = s.t[size_t(lo)];
    const qint64 tLo = s.t[size_t(lo - 1)];
    const double vLo = s.v[size_t(lo - 1)];
    const double vHi = s.v[size_t(lo)];
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
    // An empty mask IS "every sample is valid" (C4), so this is not a shortcut around the
    // masked path — it is the same computation with nothing marked, and one implementation.
    return summaryMasked(tUs, value, QVariantList{}, startUs, endUs);
}

QVariantMap ChartMetrics::summaryMasked(const QVariantList &tUs, const QVariantList &value,
                                        const QVariantList &valid,
                                        qint64 startUs, qint64 endUs) const
{
    QVariantMap r;
    const int n = qMin(tUs.size(), value.size());

    if (startUs > endUs) std::swap(startUs, endUs);

    const ValidSamples s = collectValid(tUs, value, valid);

    // Window edges, linearly interpolated — between the nearest VALID samples, which is the
    // only place this reducer is allowed to cross a bridged run at all, and it reports it.
    bool skipStart = false, skipEnd = false;
    const double start = interpValid(s, startUs, &skipStart);
    const double end   = interpValid(s, endUs,   &skipEnd);

    // Extremes over the interpolated edges plus every VALID sample strictly inside the window.
    double mn = qMin(start, end), mx = qMax(start, end);
    qint64 tMin = (start <= end) ? startUs : endUs;
    qint64 tMax = (start >= end) ? startUs : endUs;

    double rate = 0.0;                  // max |Δvalue/Δt|, deg per 100 ms
    bool   havePrev = false;
    double prevV = 0.0;
    qint64 prevT = 0;

    for (int i = 0; i < s.size(); ++i) {
        const qint64 t = s.t[size_t(i)];
        if (t < startUs || t > endUs) continue;
        const double v = s.v[size_t(i)];
        if (v < mn) { mn = v; tMin = t; }
        if (v > mx) { mx = v; tMax = t; }
        // "Consecutive" now means consecutive VALID samples, so a slope is never taken with a
        // bridged value at either end. Where that spans a gap the Δt is the real elapsed time,
        // which makes the rate SMALLER — the safe direction: a gap must not be able to invent
        // the steepest slope in the window.
        if (havePrev && t != prevT) {
            const double dv = qAbs(v - prevV) / (double(t - prevT) / 1.0e5);
            if (dv > rate) rate = dv;
        }
        prevV = v; prevT = t; havePrev = true;
    }

    // ── partial: this window's numbers do not rest on a continuous measurement ──────────────
    //
    // Rule 1 — an invalid sample lies inside the window. Whatever the reducers returned, part of
    // the span they cover was never measured, so a reader comparing this card against a
    // neighbour is comparing different amounts of evidence.
    //
    // Rule 2 — an edge value was interpolated across invalid samples (see interpValid). This is
    // the case rule 1 misses: a window that sits entirely INSIDE a bridged run contains no valid
    // sample to scan, and every number in it came from the two measurements bracketing it.
    bool partial = skipStart || skipEnd;
    if (!partial && haveMask(n, valid)) {
        for (int i = 0; i < n; ++i) {
            if (valid.at(i).toInt() != 0) continue;
            const qint64 t = tUs.at(i).toLongLong();
            if (t >= startUs && t <= endUs) { partial = true; break; }
        }
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
    r.insert(QStringLiteral("partial"), partial);
    return r;
}

QVariantMap ChartMetrics::domainFor(const QString &key) const
{
    // The DEFAULT-CONSTRUCTED domain is the whole swing (Address..Finish), and using it here
    // rather than writing 0 and 7 out means the fallback for an unknown key is authored in
    // exactly one place — metric_descriptor.h — alongside the value every descriptor that does
    // not override it already carries.
    const pinpoint::analysis::MetricDescriptor *d = m_catalogue.descriptor(key);
    const pinpoint::analysis::PhaseDomain dom = d ? d->domain
                                                  : pinpoint::analysis::PhaseDomain{};

    // Narrowing is measured against the DEFAULT, not against the swing, and per side. See the
    // header: the axis is the padded swing, so "clip to Address..Finish" is not a no-op — it is a
    // 250 ms bite out of both ends of every metric the manifest never narrowed. A side the
    // manifest left alone must not be clipped at all.
    const pinpoint::analysis::PhaseDomain whole{};
    const bool firstNarrowed = dom.first != whole.first;
    const bool lastNarrowed  = dom.last  != whole.last;

    return QVariantMap{ { QStringLiteral("firstPhase"),    int(dom.first) },
                        { QStringLiteral("lastPhase"),     int(dom.last)  },
                        { QStringLiteral("firstNarrowed"), firstNarrowed  },
                        { QStringLiteral("lastNarrowed"),  lastNarrowed   },
                        { QStringLiteral("narrowed"),      firstNarrowed || lastNarrowed } };
}

bool ChartMetrics::measuredAt(const QVariantList &tUs, const QVariantList &valid,
                             qint64 us, qint64 fromUs, qint64 toUs) const
{
    // Outside the domain first: no amount of validity makes a reading of a foreshortened body
    // line mean something, so the domain test does not depend on there being samples at all.
    if (toUs > fromUs && (us < fromUs || us > toUs)) return false;

    // ONE short-mask rule for the whole file — see haveMask(). `n` is the CURVE's length, so a
    // mask shorter than it is discarded wholesale rather than bounding the search: bounding it at
    // qMin(sizes) silently answered a different question than collectValid did about the same
    // series, which is how two views of one curve start disagreeing.
    const int n = tUs.size();
    if (!haveMask(n, valid)) return true;          // nothing marked ⇒ every sample is a measurement

    // Nearest sample. Linear, because the ONE remaining caller asks this once per phase dot
    // (≤10 a series) at data-change time — every per-frame caller answers it in JS off an index
    // instead (PpChartPlot._measured), since marshalling a whole series per frame is not free.
    int    best = -1;
    qint64 bestD = std::numeric_limits<qint64>::max();
    for (int i = 0; i < n; ++i) {
        const qint64 d = qAbs(tUs.at(i).toLongLong() - us);
        if (d < bestD) { bestD = d; best = i; }
    }
    // No sample at all is not the same as an invalid one: there is nothing here to call bridged,
    // and the caller's own "is there a curve" test already gated it.
    return best < 0 || valid.at(best).toInt() != 0;
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
    // ⚠ A DISTANCE TOLERANCE, and no default. "Nearest" over a whole swing means the Address
    // sample is 900 ms away and still nearest, so an unconditional nearest-band answer tinted a
    // card by a verdict passed at a completely different instant — and an empty list fell through
    // to a bare "good", which is a pass grade invented from no data at all. Both cases now return
    // "" and the caller tints neutrally. kBandNearUs is one generous frame (20 ms at 50 fps): a
    // phaseSample IS stamped at an instant, so anything further away is a different reading.
    static constexpr qint64 kBandNearUs = 20000;
    if (bestD > kBandNearUs) return QString();
    return best;
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
