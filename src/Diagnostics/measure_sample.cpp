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

namespace pinpoint::analysis {

namespace {

// Median of a non-empty vector, O(n) and order-independent. Lifted rather than shared with
// WristAngleSampler's detail::medianOf because that header carries the whole wrist assessment
// vocabulary (PpJointDof, handedness, gimbal proxies) and this module needs none of it — the
// convention is what matters, and it is four lines.
double medianOf(std::vector<double> v)
{
    const std::size_t n   = v.size();
    const std::size_t mid = n / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    const double hi = v[mid];
    if (n % 2 == 1)
        return hi;
    std::nth_element(v.begin(), v.begin() + (mid - 1), v.end());
    return 0.5 * (v[mid - 1] + hi);
}

} // namespace

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
    std::sort(ladder.begin(), ladder.end(),
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
        // ABSENT MEANS EVERY SAMPLE COUNTS, which is what makes this change invisible to an
        // existing swing — hence no kPhaseGridSchemaVersion bump. A mask SHORTER than the curve is
        // a malformed document rather than a partial statement, and is treated as no mask at all:
        // guessing which end it was truncated from would invent validity we were never told about.
        const QJsonArray validArr = mo.value(QStringLiteral("valid")).toArray();
        const bool       haveMask = n > 0 && validArr.size() >= n;
        const auto sampleValid = [&](int i) { return !haveMask || validArr.at(i).toInt() != 0; };

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
        for (const PhaseAt &e : candidates) {
            std::vector<double> win;
            for (int i = 0; i < n; ++i) {
                const int64_t t = tArr.at(i).toVariant().toLongLong();
                if (std::llabs(t - e.tUs) > cfg.windowHalfUs)
                    continue;
                if (!sampleValid(i))
                    continue;      // bridged, not measured — it may not pull the median
                win.push_back(vArr.at(i).toDouble());
            }
            if (static_cast<int>(win.size()) >= cfg.minValidSamples) {
                mg.values.push_back(PhaseGridValue{ e.phase, e.tUs, medianOf(std::move(win)) });
                continue;
            }
            const auto it = std::find_if(labelled.begin(), labelled.end(),
                                         [&](const auto &pr) { return pr.first == e.phase; });
            if (it != labelled.end())
                mg.values.push_back(PhaseGridValue{ e.phase, it->second.tUs, it->second.value });
        }

        std::sort(mg.values.begin(), mg.values.end(),
                  [](const PhaseGridValue &a, const PhaseGridValue &b) { return a.tUs < b.tUs; });

        // Spans between consecutive PRESENT values. Half-open at the start so aggregating
        // consecutive spans double-counts nothing, and so a span carries what happened BETWEEN two
        // phases rather than restating either endpoint.
        for (std::size_t i = 1; i < mg.values.size(); ++i) {
            const int64_t lo = mg.values[i - 1].tUs;
            const int64_t hi = mg.values[i].tUs;

            PhaseGridSpan sp;
            sp.from = mg.values[i - 1].phase;
            sp.to   = mg.values[i].phase;

            bool any = false;
            for (int k = 0; k < n; ++k) {
                const int64_t t = tArr.at(k).toVariant().toLongLong();
                if (t <= lo || t > hi)
                    continue;
                if (!sampleValid(k))
                    continue;      // a bridged sample cannot BE the peak; see the mask note above
                const double v = vArr.at(k).toDouble();
                if (!any) { sp.min = sp.max = v; any = true; }
                else      { sp.min = std::min(sp.min, v); sp.max = std::max(sp.max, v); }
            }
            // No samples strictly inside: fall back to the two endpoint medians, so an Extremum
            // over a sparse window still answers with the best the curve supports rather than
            // reporting the whole measure unavailable.
            if (!any) {
                sp.min = std::min(mg.values[i - 1].value, mg.values[i].value);
                sp.max = std::max(mg.values[i - 1].value, mg.values[i].value);
            }
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

        // The endpoints themselves are part of the search — a peak sitting exactly on a phase is
        // still the peak.
        double best = (r.sense == ExtremumSense::Min) ? std::min(lo->value, hi->value)
                                                      : std::max(lo->value, hi->value);
        for (const PhaseGridSpan &sp : mg->spans) {
            const PhaseGridValue *f = mg->at(sp.from);
            const PhaseGridValue *t = mg->at(sp.to);
            if (!f || !t)
                continue;
            if (f->tUs < lo->tUs || t->tUs > hi->tUs)
                continue;
            best = (r.sense == ExtremumSense::Min) ? std::min(best, sp.min)
                                                   : std::max(best, sp.max);
        }

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

    QJsonObject src;
    src.insert(QStringLiteral("size"),     sourceSize);
    src.insert(QStringLiteral("mtime_ms"), sourceMtimeMs);
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
                             bool *guardOk)
{
    if (guardOk) *guardOk = false;
    SwingPhaseGrid grid;

    if (root.value(QStringLiteral("schema")).toInt() != kPhaseGridSchemaVersion)
        return grid;

    const QJsonObject src = root.value(QStringLiteral("source")).toObject();
    if (src.value(QStringLiteral("size")).toVariant().toLongLong() != sourceSize
        || src.value(QStringLiteral("mtime_ms")).toVariant().toLongLong() != sourceMtimeMs)
        return grid;                                   // stale: rebuild, never trust

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
            SwingPhaseGrid cached =
                loadPhaseGrid(QJsonDocument::fromJson(sf.readAll()).object(), size, mtimeMs, &ok);
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
