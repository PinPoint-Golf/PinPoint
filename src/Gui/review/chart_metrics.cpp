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

#include "../../Analysis/series_reduce.h"   // C8 — reduceAt / reduceExtremum / reduceRate
#include "../Analysis/dashboard_reductions.h"   // barDomain
#include "timeline_labels.h"                    // the one phase-tag vocabulary (hasPositionTag)

#include <QHash>
#include <QSet>
#include <QtGlobal>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

namespace pa = pinpoint::analysis;

// ⚠ THE SHORT-MASK RULE, and it is deliberately the same one measure_sample.cpp's buildPhaseGrid
// applies: a mask is honoured only when it covers the whole curve (`size >= n`); a SHORTER one is
// a malformed document rather than a partial statement and is treated as NO MASK AT ALL. Guessing
// which end it was truncated from would invent validity nobody told us about, and having the chart
// and the diagnostics grid disagree about a malformed file is worse than either answer. Everything
// that consults validity in this file — lift() and measuredAt() — goes through haveMask().
//
// C8 asks the CALLER to apply this rule (SeriesView takes a `valid` pointer that may be null), for
// the same reason: the shared reducers must not have to carry a second opinion about a malformed
// document.
bool haveMask(int n, const QVariantList &valid)
{
    return n > 0 && valid.size() >= n;
}

// The VALID samples of a series, in order — the shape the one remaining piece of arithmetic in
// this file needs (interpValid, the fallback window edge). The original indices are NOT kept any
// more: they existed for the `bracketSkipped` adjacency test, and a fallback edge is a `partial`
// window whether or not the two samples it read from were neighbours.
struct ValidSamples {
    std::vector<int64_t> t;
    std::vector<double>  v;
    int size() const { return int(t.size()); }
};

// The series lifted out of the QVariantLists ONCE, in the two shapes its two readers want:
//
//  • `t` / `v` / `valid` — the WHOLE curve, unfiltered, which is what a C8 SeriesView borrows.
//    The shared reducers apply the valid rule themselves and they have to: a centred-window mean
//    and a least-squares slope both need to know a sample was DROPPED rather than have the gap
//    quietly closed up by pre-filtering. Closing it up is precisely what the old
//    adjacent-difference rate did, and why a 2 ms bridge could read as 100 units per 100 ms.
//  • `good` — the valid samples alone, for interpValid below.
//
// ⚠ int64_t, not qint64, and here they are not interchangeable: SeriesView borrows a
// `const int64_t *`, and while the two are the same type on macOS and Windows they are `long` vs
// `long long` on Linux, where handing over a vector<qint64>'s data() would not compile at all.
struct Lifted {
    std::vector<int64_t> t;
    std::vector<double>  v;
    std::vector<uint8_t> valid;      // empty unless a mask is honoured — see haveMask()
    ValidSamples         good;
    bool                 masked = false;
};

// Collect them, in one pass over the QVariantLists (they are the expensive part: every entry is a
// QVariant conversion, and this is called per card per window change).
//
// `valid` is the C4 mask (swing.json `metrics[].valid`): EMPTY means every sample is valid — the
// state of every series written before the field existed, so the empty case must cost nothing and
// behave identically to the unmasked path. A mask entry of 0 marks a sample the grid bridged
// across a gated or absent run.
Lifted lift(const QVariantList &tUs, const QVariantList &value, const QVariantList &valid)
{
    Lifted s;
    const int n = qMin(tUs.size(), value.size());
    s.masked = haveMask(n, valid);
    s.t.reserve(size_t(n)); s.v.reserve(size_t(n));
    if (s.masked) s.valid.reserve(size_t(n));
    s.good.t.reserve(size_t(n)); s.good.v.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const int64_t t = int64_t(tUs.at(i).toLongLong());
        const double  v = value.at(i).toDouble();
        const bool    ok = !s.masked || valid.at(i).toInt() != 0;
        s.t.push_back(t);
        s.v.push_back(v);
        if (s.masked) s.valid.push_back(ok ? uint8_t(1) : uint8_t(0));
        if (ok) { s.good.t.push_back(t); s.good.v.push_back(v); }
    }
    return s;
}

// A borrowed C8 view of the whole curve. Named `chartView` rather than reusing C8's own viewOf()
// (which lives in series_reduce_metric.h, the one file over there that knows the analysis types
// exist) because that one takes a MetricSeries and the chart never has one — it has the QML
// bridge's parallel QVariantLists. series_reduce.h itself is std-only, which is why including it
// from the Gui costs nothing.
pa::SeriesView chartView(const Lifted &s)
{
    pa::SeriesView view;
    view.t     = s.t.empty() ? nullptr : s.t.data();
    view.v     = s.v.empty() ? nullptr : s.v.data();
    view.valid = (s.masked && !s.valid.empty()) ? s.valid.data() : nullptr;
    view.n     = s.t.size();
    return view;
}

// Linear interpolation at `pos` over the ascending VALID samples, clamped to the end values
// outside their range.
//
// ⚠ THE ONLY REDUCTION ARITHMETIC LEFT IN THIS FILE, and it survives for exactly one job: the
// window edge that reduceAt cannot answer, because no valid sample lies within ±15 ms of it (the
// edge sits inside a bridged run wider than the median window, or outside the data entirely).
// Something has to be printed there, and the value between the nearest measurements is the
// honest something — but a caller that reaches this has already lost its claim to a measured
// edge, so summaryMasked sets `partial` whenever it does.
//
// That is also why the old `bracketSkipped` out-parameter is gone: it existed to ask whether the
// two samples read from were non-adjacent in the original series, i.e. whether this edge had
// crossed a bridged run. Every call now IS such a case by construction, so the answer is always
// yes and the question no longer earns its complexity.
double interpValid(const ValidSamples &s, qint64 pos)
{
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
    const int64_t tHi = s.t[size_t(lo)];
    const int64_t tLo = s.t[size_t(lo - 1)];
    const double  vLo = s.v[size_t(lo - 1)];
    const double  vHi = s.v[size_t(lo)];
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

    if (startUs > endUs) std::swap(startUs, endUs);

    const Lifted s = lift(tUs, value, valid);
    const pa::SeriesView view = chartView(s);

    // The three windows arrive as ReduceConfig's OWN defaults (tuned::sampler::kWindowHalfUs,
    // tuned::reduce::kExtremumWindowUs, tuned::reduce::kRateWindowUs), so ±15 ms, 40 ms and 50 ms
    // are authored in exactly one place. The card literally cannot be reading a different 40 ms
    // than buildPhaseGrid is, which is the property design §5.2 exists to buy and §7 item 5 is
    // measured on.
    const pa::ReduceConfig cfg{};

    // ── Window edges — the ±15 ms windowed MEDIAN, the reducer the diagnostics grid already uses ─
    //
    // Design §5.2: a median of the valid samples AROUND the instant, not a linear interpolation
    // between the two straddling it. One bad frame on the edge used to set the endpoint a reader
    // compares PEAK against, and Δ SEGMENT with it.
    //
    // FALLBACK, and it is reported rather than hidden: reduceAt is not ok when there is no valid
    // sample within ±15 ms of the edge — the edge sits inside a bridged run wider than the median
    // window, or off the end of the data. interpValid still gives the honest number (the value
    // between the nearest measurements, clamped at the ends), but nothing was measured near there,
    // so the window is `partial`.
    bool fellBack = false;
    const auto edgeAt = [&](qint64 us) {
        const pa::Reduced a = pa::reduceAt(view, us, cfg);
        if (a.ok) return a.value;
        fellBack = true;
        return interpValid(s.good, us);
    };
    const double start = edgeAt(startUs);
    const double end   = edgeAt(endUs);

    // ── edgeOk: is there ANY measurement in this series to read an edge from? ────────────────
    //
    // ⚠ THE SAME PRINCIPLE AS rateOk, APPLIED TO THE OTHER SIX NUMBERS. A series whose every
    // sample is bridged — or an empty curve — has no valid sample at all, so interpValid has
    // nothing to interpolate BETWEEN and returns 0.0. Every number built on that (start, end,
    // min, max, peak, range, delta) is then a confident 0 drawn from nothing: PEAK 0, Δ 0,
    // RANGE 0, wearing a PARTIAL chip that reads as "mostly fine". `partial` is not strong enough
    // for that case and never was — it qualifies numbers, and these are not numbers.
    //
    // So the card gates those tiles on `edgeOk` exactly as it gates PK RATE on `rateOk`. The
    // values are still returned (a caller mid-migration gets today's zeros rather than a crash),
    // but a display that prints them has been told not to.
    const bool edgeOk = s.good.size() > 0;

    // ── min / max — the extremum of the CENTRED-WINDOW MEAN, no longer a raw argmax ────────────
    //
    // ⚠ THIS IS A DEFINITION CHANGE and it is the point of the phase: a one-sample outlier can no
    // longer be the peak, because under the 40 ms window a peak has to have been there for 40 ms.
    // A 99 among 4s at 8 ms sampling reports (4·4 + 99)/5 ≈ 23, not 99. It also means the
    // interpolated window EDGES no longer take part in min/max the way they did when this scanned
    // the raw samples and seeded itself from them: an extremum is now a statement about
    // measurements inside the window, and an edge is a statement about an instant.
    const pa::Reduced loR = pa::reduceExtremum(view, startUs, endUs, false, cfg);
    const pa::Reduced hiR = pa::reduceExtremum(view, startUs, endUs, true,  cfg);

    double mn = 0.0, mx = 0.0, sigMn = 0.0, sigMx = 0.0;
    qint64 tMin = startUs, tMax = startUs;
    if (loR.ok && hiR.ok) {
        mn = loR.value; tMin = loR.atUs; sigMn = loR.sigma;
        mx = hiR.value; tMax = hiR.atUs; sigMx = hiR.sigma;
    } else {
        // No valid sample lies inside the window at all — a window narrower than the sample
        // spacing, or one wholly inside a bridged run. The edges are then the only readings there
        // are, which is what this did with them before C8 as well. It is not by itself a `partial`
        // case: a window narrower than the spacing on a fully valid series is a reader's choice,
        // not a gap in the measurement.
        mn = qMin(start, end); mx = qMax(start, end);
        tMin = (start <= end) ? startUs : endUs;
        tMax = (start >= end) ? startUs : endUs;
    }

    // ── rate — the steepest least-squares slope over a ≥50 ms window, and it may not exist ─────
    //
    // ⚠ THE SECOND DEFINITION CHANGE. This was max |Δvalue/Δt| between consecutive samples, i.e.
    // frame noise divided by the frame interval: on a still address it read 39 and 291 units per
    // 100 ms on the corpus (design §7 item 2). A fit over ≥50 ms with ≥3 valid samples cannot be
    // driven by one frame.
    //
    // SIGNED now, because a least-squares slope has a direction and throwing it away here would
    // leave no consumer able to recover it. `rateOk` is the honest half: a window with fewer than
    // three valid samples or a span under 50 ms produces NO rate, and this reports 0 with
    // rateOk false rather than a fabricated number — the card prints "—". A reader must gate on
    // rateOk before touching `rate`, `rateSigma` or `tRateUs`.
    const pa::Reduced rateR = pa::reduceRate(view, startUs, endUs, cfg);

    // ── partial: this window's numbers do not rest on a continuous measurement ──────────────
    //
    // Rule 1 — an edge fell back to interpolation (above) ON A MASKED SERIES: no valid sample
    // within ±15 ms of it, so that endpoint, Δ and possibly min/max came from across ground the
    // producer bridged. This is the case rule 2 misses, including the window that sits ENTIRELY
    // inside a bridged run and so contains no sample to scan at all.
    //
    // ⚠ AND `s.masked` IS WHY IT IS NOT JUST `fellBack`. A fallback also happens with NO MASK at
    // all, whenever a window edge lands more than 15 ms from any sample — which on a real timeline
    // is common, not exotic: 21 % of a rich_7iron series' span is more than 15 ms from a sample and
    // its largest gap is 83 ms, so a brush dragged there would have put a PARTIAL chip on a swing
    // with nothing whatever to declare. Pre-Phase-2 `partial` was unreachable without a mask, and
    // it must stay that way: the chip's claim is "the producer bridged part of this window", and
    // the mask is the only thing that ever says so. A COARSE series is not an incomplete one — it
    // was measured everywhere it claims to have been — and the case where there is nothing to read
    // at all is `edgeOk`, not this.
    //
    // Rule 2 — an invalid sample lies inside the window. Whatever the reducers returned, part of
    // the span they cover was never measured, so a reader comparing this card against a neighbour
    // is comparing different amounts of evidence.
    bool partial = fellBack && s.masked;
    if (!partial && s.masked) {
        for (size_t i = 0; i < s.valid.size(); ++i) {
            if (s.valid[i] != 0) continue;
            if (s.t[i] >= startUs && s.t[i] <= endUs) { partial = true; break; }
        }
    }

    // peak = the extremum of larger MAGNITUDE (today's rule, unchanged) — and its σ travels with
    // it, so PEAK can carry "± σ" (design §5.3) without a second reduction that could disagree
    // about which extremum won.
    const bool   maxWins   = qAbs(mx) >= qAbs(mn);
    const double peak      = maxWins ? mx : mn;
    const qint64 tPeak     = maxWins ? tMax : tMin;
    const double peakSigma = maxWins ? sigMx : sigMn;

    r.insert(QStringLiteral("start"),     start);
    r.insert(QStringLiteral("end"),       end);
    r.insert(QStringLiteral("min"),       mn);
    r.insert(QStringLiteral("max"),       mx);
    r.insert(QStringLiteral("peak"),      peak);
    // ⚠ range AND delta REST ON DIFFERENT EVIDENCE and can therefore contradict each other: range
    // is a span of 40 ms windowed MEANS anchored at the samples inside the window, delta a
    // difference of ±15 ms MEDIANS taken at its two edges (neither support is clamped to the
    // window; what differs is what each is anchored on). On a window
    // narrower than the sample spacing the medians can move while there is nothing inside to have
    // a span, so a card can read RANGE 0.2 beside Δ 4. Left inconsistent deliberately: see the
    // header — forcing range ≥ |delta| would blend two reducers and hide the only signal a reader
    // gets that the window is too narrow for the curve in it.
    r.insert(QStringLiteral("range"),     mx - mn);
    r.insert(QStringLiteral("delta"),     end - start);
    r.insert(QStringLiteral("rate"),      rateR.ok ? rateR.value : 0.0);
    r.insert(QStringLiteral("tPeakUs"),   tPeak);
    r.insert(QStringLiteral("partial"),   partial);
    // NEW IN PHASE 2. peakSigma / rateSigma are the σ the card prints beside the two tiles a
    // reader's trust is decided on; rateOk gates the rate tile; tRateUs is the centre of the
    // winning slope window, so a caller can mark WHERE the steepest change was, exactly as
    // tPeakUs does for the peak. Both are in the CALLER'S timebase, whatever that is — for the
    // review chart, the clip µs of analysisDetail.series[i].t_us — so tRateUs is 0 when no rate
    // was fitted AND 0 is a perfectly ordinary instant in it: rateOk, never tRateUs, is the thing
    // to test. (tPeakUs has no such state: with no valid sample in the window the extremes come
    // from the edges, and it names the edge they came from.)
    r.insert(QStringLiteral("peakSigma"), peakSigma);
    r.insert(QStringLiteral("rateSigma"), rateR.ok ? rateR.sigma : 0.0);
    r.insert(QStringLiteral("edgeOk"),    edgeOk);
    r.insert(QStringLiteral("rateOk"),    rateR.ok);
    r.insert(QStringLiteral("tRateUs"),   rateR.ok ? qint64(rateR.atUs) : qint64(0));
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
    // qMin(sizes) silently answered a different question than lift() did about the same
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
