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

#include "measure_sample.h"

#include "../Analysis/series_reduce.h"
#include "../Core/club_vocabulary.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pinpoint::analysis {

// ── Lookups ─────────────────────────────────────────────────────────────────

const PhaseGridValue *MetricPhaseGrid::at(Phase p) const
{
    for (const PhaseGridValue &v : values)
        if (v.phase == p) return &v;
    return nullptr;
}

const MetricPhaseGrid *SwingPhaseGrid::metric(const QString &key) const
{
    if (key.isEmpty())
        return nullptr;
    for (const MetricPhaseGrid &m : metrics)
        if (m.key == key) return &m;
    return nullptr;
}

// ── Building ────────────────────────────────────────────────────────────────

SwingPhaseGrid buildPhaseGrid(const QJsonObject &analysis, const PhaseGridConfig &cfg)
{
    SwingPhaseGrid grid;
    grid.config = cfg;      // the sidecar guard compares these windows; see savePhaseGrid

    // The segmented ladder, deduplicated and time-ordered. A doc can legitimately carry the same
    // phase twice (a re-segmentation appended rather than replaced); the FIRST wins, matching
    // Segmentation::eventFor(), so this module and the analyzer read the same instant.
    struct PhaseAt { Phase phase; int64_t tUs; };
    std::vector<PhaseAt> ladder;
    for (const QJsonValue &pv : analysis.value(QStringLiteral("phases")).toArray()) {
        const QJsonObject po = pv.toObject();
        if (!po.contains(QStringLiteral("phase")))
            continue;
        const Phase p = static_cast<Phase>(po.value(QStringLiteral("phase")).toInt());

        // Round-trip through the token vocabulary rather than range-checking: Phase's enumerators
        // are not contiguous, so a stray int must be rejected, never cast into a neighbour.
        Phase back{};
        if (!phaseFromToken(phaseToken(p), back) || back != p)
            continue;
        if (std::any_of(ladder.begin(), ladder.end(),
                        [&](const PhaseAt &e) { return e.phase == p; }))
            continue;
        ladder.push_back({ p, po.value(QStringLiteral("t_us")).toVariant().toLongLong() });
    }
    // STABLE, because phases can share an instant (rich_7iron segments Address and Takeaway at the
    // same t_us) and an unstable sort would order those two by whatever the implementation felt like.
    // Document order is the only defensible tie-break: it is what the analyzer wrote.
    std::stable_sort(ladder.begin(), ladder.end(),
                     [](const PhaseAt &a, const PhaseAt &b) { return a.tUs < b.tUs; });

    if (ladder.empty())
        return grid;   // unsegmented: no phase to read anything at, so nothing is producible

    for (const QJsonValue &mv : analysis.value(QStringLiteral("metrics")).toArray()) {
        const QJsonObject mo  = mv.toObject();
        const QString     key = mo.value(QStringLiteral("key")).toString();
        if (key.isEmpty())
            continue;

        const QJsonArray tArr = mo.value(QStringLiteral("t_us")).toArray();
        const QJsonArray vArr = mo.value(QStringLiteral("value")).toArray();
        const int        n    = std::min(tArr.size(), vArr.size());

        // ── The validity mask ───────────────────────────────────────────────────────────────────
        //
        // `valid` is an int 0/1 array parallel to `t_us`, and it is PRESENT ONLY WHEN AT LEAST ONE
        // SAMPLE IS INVALID — the same discipline `sigma` follows, so an all-valid series is never
        // written and every swing.json that predates the field carries nothing. A 0 marks a sample
        // whose value was BRIDGED across a gated or absent run (metric_channel.h): the curve stays
        // continuous for the renderer, but the number there is interpolation, not measurement.
        //
        // So a bridged sample enters neither a phase's windowed median nor a span's extremes. That
        // is the same rule as "a phase the segmenter never found yields nothing": reducing over a
        // fabricated value is a confident wrong answer, and this sidecar would cache it. Design
        // §5.1; the mask itself is MetricSeries::valid.
        //
        // ABSENT MEANS EVERY SAMPLE COUNTS, so a swing that carries no mask grids exactly as one
        // carrying an all-ones mask does — that equivalence is what makes the field additive, and it
        // is what the mask tests compare. A mask SHORTER than the curve is a malformed document
        // rather than a partial statement, and is treated as no mask at all: guessing which end it
        // was truncated from would invent validity we were never told about.
        const QJsonArray validArr = mo.value(QStringLiteral("valid")).toArray();
        const bool       haveMask = n > 0 && validArr.size() >= n;

        // ── One pass out of JSON, then arithmetic on plain arrays ────────────────────────────────
        //
        // QJsonArray::at() + toVariant() per sample per phase per span was the old shape, and with
        // the windowed-mean extremum it would be a nested walk over a boxed container. Convert once
        // and hand the reducers a borrowed view: series_reduce.h is std-only precisely so that this
        // module and the review chart can share it without either dragging the other's types along.
        //
        // The SHORT-MASK RULE is applied HERE and nowhere below — a `valid` array that does not
        // reach the end of `t_us` becomes a null mask, which is what `haveMask` already decided. The
        // reducers never see a partial mask, so they cannot have an opinion about one.
        std::vector<int64_t> tv;
        std::vector<double>  vv;
        std::vector<uint8_t> validv;
        tv.reserve(std::size_t(n));
        vv.reserve(std::size_t(n));
        if (haveMask)
            validv.reserve(std::size_t(n));
        for (int i = 0; i < n; ++i) {
            tv.push_back(tArr.at(i).toVariant().toLongLong());
            vv.push_back(vArr.at(i).toDouble());
            if (haveMask)
                validv.push_back(validArr.at(i).toInt() != 0 ? uint8_t(1) : uint8_t(0));
        }

        SeriesView view;
        view.t     = tv.empty() ? nullptr : tv.data();
        view.v     = vv.empty() ? nullptr : vv.data();
        view.valid = validv.empty() ? nullptr : validv.data();
        view.n     = tv.size();

        // The grid's own knobs win over the reducer defaults — same numbers today, but the two must
        // not be able to drift apart silently if either is ever swept.
        ReduceConfig rc;
        rc.atHalfWindowUs   = cfg.windowHalfUs;
        rc.minAtSamples     = cfg.minValidSamples;
        rc.extremumWindowUs = cfg.extremumWindowUs;

        // The metric's own labelled readings at key phases.
        //
        // NOT an optimisation — for a large class of metrics it is the ONLY data there is. Every
        // setup metric in a real swing.json (stanceWidth, ballPosition, tempoRatio, the foot-flare
        // and toe-line angles) ships with an EMPTY curve and nothing but phaseSamples: they are
        // read once at a position, so there is no curve to sample. A builder that read only `value[]`
        // silently produced nothing for all of them, and the failure looks exactly like "no swing
        // carries this measure".
        struct Labelled { int64_t tUs; double value; };
        std::vector<std::pair<Phase, Labelled>> labelled;
        for (const QJsonValue &sv : mo.value(QStringLiteral("phaseSamples")).toArray()) {
            const QJsonObject so = sv.toObject();
            if (!so.contains(QStringLiteral("phase")))
                continue;
            const Phase p = static_cast<Phase>(so.value(QStringLiteral("phase")).toInt());
            Phase back{};
            if (!phaseFromToken(phaseToken(p), back) || back != p)
                continue;
            labelled.emplace_back(p, Labelled{ so.value(QStringLiteral("t_us")).toVariant().toLongLong(),
                                               so.value(QStringLiteral("value")).toDouble() });
        }

        if (n == 0 && labelled.empty())
            continue;

        MetricPhaseGrid mg;
        mg.key  = key;
        mg.unit = mo.value(QStringLiteral("unit")).toString();

        // The phases this METRIC can answer at: the swing's ladder, plus any phase its own
        // phaseSamples name. A producer that labelled P6 knows something the ladder did not record,
        // and dropping it would leave a measure unavailable when its number is right there.
        std::vector<PhaseAt> candidates = ladder;
        for (const auto &[p, l] : labelled)
            if (!std::any_of(candidates.begin(), candidates.end(),
                             [&](const PhaseAt &e) { return e.phase == p; }))
                candidates.push_back({ p, l.tUs });

        // Values: a short windowed median about each phase instant, falling back to the metric's
        // own labelled reading where the curve has nothing to say. A phase with neither gets NO
        // entry — inventing a nearest-sample value is exactly the fabrication across a gap the
        // median convention exists to avoid.
        //
        // A phase whose whole window is masked invalid takes that SAME path, deliberately: "the
        // curve has nothing to say here" is exactly what a fully bridged window means, so it needs
        // no branch of its own, and the labelled phaseSamples fallback still applies to it —
        // a producer that stamped a reading at this phase measured something the curve had lost.
        //
        // reduceAt is the SAME arithmetic the local median was: valid samples with |t − tUs| ≤
        // windowHalfUs (inclusive at the bound), median as the mean of the two middles when the
        // count is even. Both halves of that are load-bearing and both are pinned by the endpoint
        // -truncation fixture in the test, which reads 11.925 only if the bound includes its 16th
        // sample AND the even case averages rather than taking the lower middle. `minValidSamples`
        // rides along as ReduceConfig::minAtSamples, which exists for exactly this delegation.
        for (const PhaseAt &e : candidates) {
            const Reduced r = reduceAt(view, e.tUs, rc);
            if (r.ok) {
                mg.values.push_back(PhaseGridValue{ e.phase, e.tUs, r.value });
                continue;
            }
            const auto it = std::find_if(labelled.begin(), labelled.end(),
                                         [&](const auto &pr) { return pr.first == e.phase; });
            if (it != labelled.end())
                mg.values.push_back(PhaseGridValue{ e.phase, it->second.tUs, it->second.value });
        }

        std::stable_sort(mg.values.begin(), mg.values.end(),
                         [](const PhaseGridValue &a, const PhaseGridValue &b) {
                             return a.tUs < b.tUs;
                         });   // stable for the same reason the ladder is

        // Spans between consecutive PRESENT values, CLOSED AT BOTH ENDS — [lo, hi], not (lo, hi].
        //
        // The engine's whole job here is to cache what the REVIEW CHART would say, and the chart's
        // candidate set over a phase window includes both instants. Excluding the opening one made
        // the two disagree by up to 9.4° on a real swing, and it bought nothing: min and max are
        // idempotent, so a sample shared by two adjacent spans cannot skew either aggregate.
        //
        // The extreme is the extreme of the CENTRED-WINDOW MEAN, not of the raw samples (design
        // §5.2): each candidate is scored over its own ±extremumWindowUs/2 neighbourhood, so the
        // single wild sample that used to BE the peak — and be cached here, and be graded against a
        // corridor — has to hold its value for the width of the window to win. A bridged sample is
        // neither a candidate nor a member of anyone's window, the same rule the median follows.
        for (std::size_t i = 1; i < mg.values.size(); ++i) {
            const int64_t lo = mg.values[i - 1].tUs;
            const int64_t hi = mg.values[i].tUs;

            // A ZERO-WIDTH SPAN IS NOT A SEARCH WINDOW. Two phases legitimately share an instant
            // (rich_7iron: Address and Takeaway), and [t, t] holds one candidate scored over one
            // near-raw reading. Storing that would drop a different smoothing scale into an
            // aggregate that is otherwise all 40 ms windowed means — which is precisely how the
            // 9.4° disagreement arose. Nothing is lost by omitting it: the interval is empty, and
            // both endpoints are already candidates in the neighbouring spans.
            if (hi <= lo)
                continue;

            const Reduced mn = reduceExtremum(view, lo, hi, /*wantMax=*/false, rc);
            const Reduced mx = reduceExtremum(view, lo, hi, /*wantMax=*/true,  rc);

            // Nothing valid in the interval: store NO span. The endpoint-median fallback that used
            // to sit here is now one whole-aggregation fallback in reduceOverGrid — per span it fired
            // once per empty interval and injected medians into a windowed-mean min/max even when
            // other spans had real answers, which is a fabricated extreme wearing a cached number's
            // clothes. Hoisted, it fires only when the entire window had nothing to say.
            if (!mn.ok || !mx.ok)
                continue;

            PhaseGridSpan sp;
            sp.from = mg.values[i - 1].phase;
            sp.to   = mg.values[i].phase;
            sp.min  = mn.value;
            sp.max  = mx.value;
            mg.spans.push_back(sp);
        }

        if (!mg.values.empty())
            grid.metrics.push_back(std::move(mg));
    }

    return grid;
}

// ── Reduction ───────────────────────────────────────────────────────────────

std::optional<double> reduceOverGrid(const SwingPhaseGrid &grid, const QString &metricKey,
                                     const Reducer &r)
{
    const MetricPhaseGrid *mg = grid.metric(metricKey);
    if (!mg)
        return std::nullopt;

    switch (r.kind) {
    case ReducerKind::At: {
        if (!r.anchor.has_value())
            return std::nullopt;                       // validateReducer rejects this; be safe anyway
        const PhaseGridValue *v = mg->at(*r.anchor);
        return v ? std::optional<double>(v->value) : std::nullopt;
    }

    case ReducerKind::Delta:
    case ReducerKind::Rate: {
        // "Delta and Rate run from the anchor to the window's end" — measure_facets.cpp:278.
        if (!r.anchor.has_value())
            return std::nullopt;
        const PhaseGridValue *a = mg->at(*r.anchor);
        const PhaseGridValue *b = mg->at(r.window.second);
        if (!a || !b)
            return std::nullopt;
        const double d = b->value - a->value;
        if (r.kind == ReducerKind::Delta)
            return d;

        const double secs = double(b->tUs - a->tUs) / 1'000'000.0;
        if (!(std::fabs(secs) > 0.0))
            return std::nullopt;                       // two phases at one instant: no rate exists
        return d / secs;
    }

    case ReducerKind::Extremum: {
        // Aggregate the spans covering [window.first, window.second]. Both bounds must be present
        // phases: a window whose end was never segmented is a window nothing can be searched over,
        // and guessing the nearest phase would silently answer a different question.
        const PhaseGridValue *lo = mg->at(r.window.first);
        const PhaseGridValue *hi = mg->at(r.window.second);
        if (!lo || !hi || lo->tUs > hi->tUs)
            return std::nullopt;

        // THE SPANS AND NOTHING ELSE. There is deliberately no endpoint-median seed: the spans are
        // closed, so both endpoints are already candidates inside them, and seeding a windowed-mean
        // min/max with a ±15 ms MEDIAN mixed two smoothing scales in one aggregate — the engine then
        // reported extremes the review chart could not reproduce (36 of 170 cases on one corpus
        // swing, worst 9.4°). See the header note on this function.
        bool   any  = false;
        double best = 0.0;
        for (const PhaseGridSpan &sp : mg->spans) {
            const PhaseGridValue *f = mg->at(sp.from);
            const PhaseGridValue *t = mg->at(sp.to);
            if (!f || !t)
                continue;
            if (f->tUs < lo->tUs || t->tUs > hi->tUs)
                continue;
            const double v = (r.sense == ExtremumSense::Min) ? sp.min : sp.max;
            if (!any) { best = v; any = true; }
            else      { best = (r.sense == ExtremumSense::Min) ? std::min(best, v)
                                                              : std::max(best, v); }
        }

        // LAST RESORT, once per query rather than once per span. No span in this window answered —
        // every interval was zero-width or wholly bridged — so the only readings the curve supports
        // here are the two endpoint medians. Reporting the better of them beats darkening a measure
        // whose value is sitting right there, and it is structurally what the chart does when its own
        // window reduces to its edges.
        if (!any)
            best = (r.sense == ExtremumSense::Min) ? std::min(lo->value, hi->value)
                                                   : std::max(lo->value, hi->value);

        if (!r.anchor.has_value())
            return best;

        // SIGNED deviation from the anchor — see the header. min/max of (value - anchor) is
        // (min/max of value) - anchor, because the anchor is a constant across the window.
        const PhaseGridValue *a = mg->at(*r.anchor);
        if (!a)
            return std::nullopt;
        return best - a->value;
    }
    }
    return std::nullopt;
}

std::optional<double> reduceOverGrid(const SwingPhaseGrid &grid, const Measure &m)
{
    // A Composed measure names facets, not a catalogue key: nothing in a swing.json produces it, so
    // it is unavailable here by construction rather than by a lookup failure. Saying so explicitly
    // keeps the two reasons distinguishable to anyone reading a stack trace.
    if (m.metricKey.isEmpty() && m.preferKeys.isEmpty())
        return std::nullopt;

    // BEST INSTRUMENT FIRST, and the first rung that ANSWERS wins — not the first that exists. A
    // launch monitor can report a shot while omitting one column (the GCQuad fixture carries no low
    // point at all), and a device reading that is present-but-empty must fall through to our own
    // estimate rather than dark the measure. That is the whole difference between a ladder and a
    // preference, and it is why this asks reduceOverGrid rather than grid.metric().
    for (const QString &key : measureKeyLadder(m))
        if (const std::optional<double> v = reduceOverGrid(grid, key, m.reducer))
            return v;
    return std::nullopt;
}

// ── Sidecar ─────────────────────────────────────────────────────────────────

QString phaseGridPath(const QString &swingDir)
{
    return QDir(swingDir).filePath(QStringLiteral("swing_phasegrid.json"));
}

QJsonObject savePhaseGrid(const SwingPhaseGrid &grid, qint64 sourceSize, qint64 sourceMtimeMs)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), kPhaseGridSchemaVersion);

    // The guard. size+mtime say "this swing.json has not changed"; the reduction parameters say "and
    // it was reduced the way you are about to ask for". Without the second half a swept window leaves
    // every sidecar in the library matching its source byte-for-byte while serving the OLD window's
    // numbers, and nothing anywhere would say so.
    QJsonObject src;
    src.insert(QStringLiteral("size"),               sourceSize);
    src.insert(QStringLiteral("mtime_ms"),           sourceMtimeMs);
    src.insert(QStringLiteral("window_half_us"),     qint64(grid.config.windowHalfUs));
    src.insert(QStringLiteral("extremum_window_us"), qint64(grid.config.extremumWindowUs));
    src.insert(QStringLiteral("min_valid_samples"),  grid.config.minValidSamples);
    root.insert(QStringLiteral("source"), src);

    root.insert(QStringLiteral("session"),  grid.sessionId);
    root.insert(QStringLiteral("club"),     grid.club);
    root.insert(QStringLiteral("ordinal"),  grid.ordinal);
    root.insert(QStringLiteral("wallclock_ms"), grid.wallclockMs);

    QJsonArray metrics;
    for (const MetricPhaseGrid &m : grid.metrics) {
        QJsonObject mo;
        mo.insert(QStringLiteral("key"),  m.key);
        mo.insert(QStringLiteral("unit"), m.unit);

        QJsonArray vals;
        for (const PhaseGridValue &v : m.values) {
            QJsonObject vo;
            vo.insert(QStringLiteral("phase"), int(v.phase));
            vo.insert(QStringLiteral("t_us"),  qint64(v.tUs));
            vo.insert(QStringLiteral("value"), v.value);
            vals.append(vo);
        }
        mo.insert(QStringLiteral("values"), vals);

        QJsonArray spans;
        for (const PhaseGridSpan &s : m.spans) {
            QJsonObject so;
            so.insert(QStringLiteral("from"), int(s.from));
            so.insert(QStringLiteral("to"),   int(s.to));
            so.insert(QStringLiteral("min"),  s.min);
            so.insert(QStringLiteral("max"),  s.max);
            spans.append(so);
        }
        mo.insert(QStringLiteral("spans"), spans);

        metrics.append(mo);
    }
    root.insert(QStringLiteral("metrics"), metrics);
    return root;
}

SwingPhaseGrid loadPhaseGrid(const QJsonObject &root, qint64 sourceSize, qint64 sourceMtimeMs,
                             bool *guardOk, const PhaseGridConfig &cfg)
{
    if (guardOk) *guardOk = false;
    SwingPhaseGrid grid;
    grid.config = cfg;

    if (root.value(QStringLiteral("schema")).toInt() != kPhaseGridSchemaVersion)
        return grid;

    const QJsonObject src = root.value(QStringLiteral("source")).toObject();
    if (src.value(QStringLiteral("size")).toVariant().toLongLong() != sourceSize
        || src.value(QStringLiteral("mtime_ms")).toVariant().toLongLong() != sourceMtimeMs)
        return grid;                                   // stale: rebuild, never trust

    // Built under a different reduction: the numbers are internally consistent and answer a question
    // this run is not asking. Rebuild rather than reconcile — there is no way to rescale a cached
    // extreme to a wider window, and the guard is here precisely so nobody has to try.
    PhaseGridConfig had;
    had.windowHalfUs     = src.value(QStringLiteral("window_half_us")).toVariant().toLongLong();
    had.extremumWindowUs = src.value(QStringLiteral("extremum_window_us")).toVariant().toLongLong();
    had.minValidSamples  = src.value(QStringLiteral("min_valid_samples")).toInt();
    if (!sameGridReduction(had, cfg))
        return grid;

    grid.sessionId   = root.value(QStringLiteral("session")).toString();
    grid.club        = root.value(QStringLiteral("club")).toString();
    grid.ordinal     = root.value(QStringLiteral("ordinal")).toInt();
    grid.wallclockMs = root.value(QStringLiteral("wallclock_ms")).toVariant().toLongLong();

    for (const QJsonValue &mv : root.value(QStringLiteral("metrics")).toArray()) {
        const QJsonObject mo = mv.toObject();
        MetricPhaseGrid   mg;
        mg.key  = mo.value(QStringLiteral("key")).toString();
        mg.unit = mo.value(QStringLiteral("unit")).toString();
        if (mg.key.isEmpty())
            continue;

        for (const QJsonValue &vv : mo.value(QStringLiteral("values")).toArray()) {
            const QJsonObject vo = vv.toObject();
            mg.values.push_back(PhaseGridValue{
                static_cast<Phase>(vo.value(QStringLiteral("phase")).toInt()),
                vo.value(QStringLiteral("t_us")).toVariant().toLongLong(),
                vo.value(QStringLiteral("value")).toDouble() });
        }
        for (const QJsonValue &sv : mo.value(QStringLiteral("spans")).toArray()) {
            const QJsonObject so = sv.toObject();
            mg.spans.push_back(PhaseGridSpan{
                static_cast<Phase>(so.value(QStringLiteral("from")).toInt()),
                static_cast<Phase>(so.value(QStringLiteral("to")).toInt()),
                so.value(QStringLiteral("min")).toDouble(),
                so.value(QStringLiteral("max")).toDouble() });
        }
        if (!mg.values.empty())
            grid.metrics.push_back(std::move(mg));
    }

    if (guardOk) *guardOk = true;
    return grid;
}

SwingPhaseGrid readPhaseGrid(const QString &swingDir, bool writeSidecar, const PhaseGridConfig &cfg)
{
    SwingPhaseGrid grid;

    const QString   docPath = QDir(swingDir).filePath(QStringLiteral("swing.json"));
    const QFileInfo docInfo(docPath);
    if (!docInfo.exists())
        return grid;

    const qint64 size    = docInfo.size();
    const qint64 mtimeMs = docInfo.lastModified().toMSecsSinceEpoch();

    // The cheap path.
    const QString sidePath = phaseGridPath(swingDir);
    if (QFile::exists(sidePath)) {
        QFile sf(sidePath);
        if (sf.open(QIODevice::ReadOnly)) {
            bool ok = false;
            SwingPhaseGrid cached = loadPhaseGrid(QJsonDocument::fromJson(sf.readAll()).object(),
                                                 size, mtimeMs, &ok, cfg);
            if (ok) {
                cached.swingDir = swingDir;
                return cached;
            }
        }
    }

    // The caller asked never to fat-parse. Report nothing rather than stall — an unindexed swing
    // renders as "not yet read", which is honest and recoverable, where a one-second freeze per
    // swing is neither.
    if (!writeSidecar)
        return grid;

    QFile df(docPath);
    if (!df.open(QIODevice::ReadOnly))
        return grid;

    QJsonParseError    pe{};
    const QJsonObject  root = QJsonDocument::fromJson(df.readAll(), &pe).object();
    if (pe.error != QJsonParseError::NoError || root.isEmpty())
        return grid;

    grid = buildPhaseGrid(root.value(QStringLiteral("analysis")).toObject(), cfg);
    grid.swingDir  = swingDir;
    grid.sessionId = QFileInfo(QFileInfo(swingDir).absolutePath()).fileName();

    // Identity fields, spelled exactly as swing_doc.cpp's summaryFromRoot() spells them — that
    // function is the authority and this must not drift from it, or the draw-from filter would
    // disagree with the carousel about which swing is which. Read here rather than borrowed from
    // SwingDocReader because Diagnostics must not depend on Export: this module compiles into the
    // standalone analyzer test binary, which has no FFmpeg.
    grid.ordinal = root.value(QStringLiteral("swing")).toObject()
                       .value(QStringLiteral("index")).toInt();

    const QDateTime wc = QDateTime::fromString(
        root.value(QStringLiteral("clock")).toObject().value(QStringLiteral("wallclock")).toString(),
        Qt::ISODateWithMs);
    grid.wallclockMs = wc.isValid() ? wc.toMSecsSinceEpoch() : 0;

    // THE shared resolver the summary uses (review.club, else capture.club.name, else the stub),
    // so a per-club draw-from filter buckets the two paths identically instead of splitting one
    // club into two.
    grid.club = swingDocClub(root);

    // Best-effort: a library on read-only media still browses, it just re-parses each time.
    QSaveFile out(sidePath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(savePhaseGrid(grid, size, mtimeMs)).toJson(QJsonDocument::Compact));
        out.commit();
    }
    return grid;
}

} // namespace pinpoint::analysis
