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

#include "norm_editor_model.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace pinpoint::analysis;

namespace {

// The grade policy presets, by name. Duplicated from norm_model.cpp deliberately rather than
// exported: both are QML façades over the same rule, and sharing a table between them would mean
// one owning the other's lifetime for three numbers. The NAMES are the contract, and norm_model_test
// pins them.
GradePolicy policyFor(const QString &name)
{
    if (name == QLatin1String("lenient")) return GradePolicy{ 1.5, 2.5, 3.5 };
    if (name == QLatin1String("strict"))  return GradePolicy{ 0.75, 1.5, 2.25 };
    return GradePolicy{ 1.0, 2.0, 3.0 };
}

// Forwards to the words that live with the enum (norm.h). This was the THIRD copy of those four
// strings — the measures view and the metric detail page render them too.
QString sourceLabel(NormSource s) { return normSourceLabel(s); }

// Enough decimals to be FAITHFUL, and now a forward to the one in norm.h rather than a second
// copy of it. It arrived here in A4c because this screen was where the rounding did damage; six
// surfaces render a corridor as a sentence and every one of them needs the same rule, so it moved
// to sit with the vocabulary it formats.
QString fmtNum(double v) { return normNumber(v); }

// How many swings one scan will look at. A cap, not a sample size — and it is REPORTED when it
// bites (sampleSummary.truncated), because a silent cap reads as "that is the whole library".
constexpr int kMaxScan = 2000;

// Bins for the histogram. Enough to show a shape, few enough that a 6-swing sample does not render
// as six lonely spikes across a wide axis.
constexpr int kBins = 24;

// Below this the fitted tolerance is arithmetic, not a population. Seating is refused rather than
// producing a corridor whose narrowness is an artefact of having three swings.
constexpr int kMinSeatSamples = 3;

// The user's own norm set, read straight from its path rather than through makeFileNormProvider():
// that provider adopts the FIRST readable *.norms.json in the directory, which is the right rule
// for assembling a library and the wrong one for deciding which file a write lands in.
NormPack readUserPack()
{
    NormPack pack;
    pack.id      = QStringLiteral("user");
    pack.version = QStringLiteral("1");

    const QString path = userNormPath();
    if (path.isEmpty())
        return pack;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return pack;

    NormPackLoadResult res = loadNormPack(f.readAll(), path);
    if (res.parsed && !res.pack.id.isEmpty())
        return res.pack;
    return pack;
}

} // namespace

// See the header for why this is a median and a percentile rather than a mean and a deviation,
// and why it lives out here rather than inside seatFromSample().
OneSidedFit fitOneSided(std::vector<double> values, Shape shape)
{
    OneSidedFit fit;
    if (values.empty())
        return fit;

    std::sort(values.begin(), values.end());
    const auto pct = [&values](double p) {
        const double idx = p * double(values.size() - 1);
        const auto   i   = std::size_t(std::floor(idx));
        const auto   j   = std::min(i + 1, values.size() - 1);
        return values[i] + (idx - double(i)) * (values[j] - values[i]);   // linear interpolation
    };

    fit.mu        = pct(0.50);
    fit.tolerance = (shape == Shape::Ceiling) ? (pct(0.84) - fit.mu) : (fit.mu - pct(0.16));
    // Order statistics cannot invert, so this only ever clamps floating-point noise off a
    // degenerate sample — but a negative tolerance would author a corridor whose edges are the
    // wrong way round, and that is not a thing to leave to arithmetic luck.
    fit.tolerance = std::max(0.0, fit.tolerance);
    return fit;
}

NormEditorModel::NormEditorModel(QObject *parent)
    : QObject(parent),
      m_pack(makeCharacteristicPackProvider()),
      m_norms(sharedNormProvider()),
      m_cat(makeMetricCatalogue()),
      m_policyName(QStringLiteral("standard")),
      m_route(QStringLiteral("hand")),
      m_drawFrom(QStringLiteral("all"))
{
    m_watcher = new QFutureWatcher<QVariantList>(this);
    connect(m_watcher, &QFutureWatcher<QVariantList>::finished,
            this, &NormEditorModel::onScanFinished);
}

NormEditorModel::~NormEditorModel()
{
    // The worker holds nothing of ours by reference, but it does hold the future's result buffer.
    // Waiting is the honest teardown: a detached scan writing into a destroyed watcher is a crash
    // that only shows up when someone closes the panel mid-scan.
    if (m_watcher && m_watcher->isRunning())
        m_watcher->waitForFinished();
}

GradePolicy NormEditorModel::policy() const { return policyFor(m_policyName); }

void NormEditorModel::setGradePolicy(const QString &name)
{
    const QString resolved = (name == QLatin1String("lenient") || name == QLatin1String("strict"))
                                 ? name : QStringLiteral("standard");
    if (resolved == m_policyName) return;
    m_policyName = resolved;
    emit draftChanged();
    emit sampleChanged();          // the band edges moved, so every bar's colour did too
}

void NormEditorModel::reload()
{
    resetSharedNormProvider();
    m_norms = sharedNormProvider();
}

Shape NormEditorModel::shape() const
{
    const Measure *m = measure();
    return m ? m->shape : Shape::Target;
}

// A corridor's own claim as a sentence fragment. Takes the Norm rather than a pair, because on a
// one-sided row the headline is `mu` — the aspiration — and a pair does not carry it: claimHi on a
// floor is mu + a tolerance nothing grades, which is a number that should never reach a reader.
QString NormEditorModel::claimPhrase(const Norm &n) const
{
    return rangePhrase(n.claimLo(), n.claimHi(), n.mu, shape());
}

const Measure *NormEditorModel::measure() const
{
    return m_measureId.isEmpty() ? nullptr : m_pack->pack().measure(m_measureId);
}

QString NormEditorModel::unitOf() const
{
    const Measure *m = measure();
    if (!m) return QString();
    if (!m->unit.isEmpty()) return m->unit;
    if (!m->metricKey.isEmpty())
        if (const MetricDescriptor *d = m_cat.descriptor(m->metricKey))
            return d->unit;
    return QString();
}

QString NormEditorModel::contextLabel(const QString &id) const
{
    const ContextNode *n = m_norms->contexts().node(id);
    return n ? n->label : id;
}

// ── Opening a draft ─────────────────────────────────────────────────────────

bool NormEditorModel::begin(const QString &measureId, const QString &contextId,
                            const QVariantMap &cohort)
{
    const Measure *m = m_pack->pack().measure(measureId);
    if (!m || contextId.isEmpty() || !m_norms->contexts().node(contextId))
        return false;

    // Refused, not coerced. A cohort this build cannot read would fall back to unqualified, and the
    // author would then be editing the corridor for EVERYONE on a screen they opened to edit one
    // segment — a save that looks routine and rewrites the wrong row.
    Cohort who;
    if (!cohortFromMap(cohort, who))
        return false;

    m_open       = true;
    m_measureId  = measureId;
    m_contextId  = contextId;
    m_cohort     = who;
    m_route      = QStringLiteral("hand");
    m_dirty      = false;
    m_axisLocked = false;      // a drag interrupted by re-opening must not leave the axis latched
    m_refused.clear();

    // A capture gap is refused, not edited. A corridor on a measure no sensor can produce cannot
    // grade anything — it can only sit in the library looking like coverage. The editor still
    // OPENS, so the reason is readable; it just cannot be saved.
    if (m->status == MeasureStatus::NotCapturable) {
        m_refused = m->gapReason.isEmpty()
                        ? tr("No sensor this product has can resolve this measure, so a corridor "
                             "on it could never grade anything.")
                        : m->gapReason;
    }

    // Seed from what resolves TODAY for THIS COHORT — its own row if there is one, otherwise
    // whatever grades that population here, so "override for this context" starts from the thing
    // being overridden rather than from zero. Resolving with the row's own cohort as the athlete
    // always finds its own row first when one exists (probe 1), which is what makes one call serve
    // both the edit and the create case.
    const NormResolution res = m_norms->resolve(measureId, contextId, who);
    // An OWN row is the row at this exact key — same context AND same cohort. A broader cohort's row
    // at the same context is being inherited from just as surely as an ancestor context's is, and
    // calling it "own" would offer a reset that drops a row this editor never opened.
    m_hadOwnRow = res.found() && !res.inherited && res.cohort() == who;

    m_draft = Norm{};
    m_draft.measureId = measureId;
    m_draft.contextId = contextId;
    m_draft.cohort    = who;
    m_draft.unit      = unitOf();
    if (res.found()) {
        const Norm &n   = *res.norm;
        m_draft.mu      = n.mu;
        m_draft.sigmaLo = n.sigmaLo;
        m_draft.sigmaHi = n.sigmaHi;
        m_draft.n       = m_hadOwnRow ? n.n : 0;
        m_draft.source  = m_hadOwnRow ? n.source : NormSource::Heuristic;
        m_draft.author  = m_hadOwnRow ? n.author : QString();
        m_draft.citation = m_hadOwnRow ? n.citation : QString();
        m_draft.setOn   = m_hadOwnRow ? n.setOn : QDate();
        // The plausibility bounds ARE carried in, and the contrast with the monitor bounds below is
        // the whole reason: a bound the editor SHOWS must survive a round trip through it, or
        // opening a capped row and saving it would silently drop the cap — turning readings the
        // norm had stopped believing back into confident diagnoses. Inherited too, on purpose: a
        // cap is a statement about what the instrument can produce, and the override starts from
        // the thing it is overriding.
        m_draft.plausibleLo = n.plausibleLo;
        m_draft.plausibleHi = n.plausibleHi;
        // The explicit monitor bounds are NEVER carried into a draft, even from a migrated row the
        // draft is overriding. The editor does not expose them (norm.h), and silently preserving a
        // bound the user cannot see would make the Action edge disagree with the one drawn.
    } else {
        // Nothing to inherit: centre on something the axis can render rather than a degenerate
        // zero-width band at the origin.
        m_draft.mu      = 0.0;
        m_draft.sigmaLo = 1.0;
        m_draft.sigmaHi = 1.0;
    }
    m_original = m_draft;

    emit draftChanged();
    startScan();
    return true;
}

void NormEditorModel::cancel()
{
    m_open       = false;
    m_axisLocked = false;
    m_measureId.clear();
    m_contextId.clear();
    m_samples.clear();
    m_dirty = false;
    m_refused.clear();
    emit draftChanged();
    emit sampleChanged();
}

void NormEditorModel::setRoute(const QString &r)
{
    const QString resolved = (r == QLatin1String("seat") || r == QLatin1String("import"))
                                 ? r : QStringLiteral("hand");
    if (resolved == m_route) return;
    m_route = resolved;
    emit draftChanged();
}

// ── Set by hand ─────────────────────────────────────────────────────────────

void NormEditorModel::setClaimBand(double lo, double hi)
{
    // REFUSED on a one-sided norm, rather than left to the caller's discretion. Taking the midpoint
    // of two edges would move mu — the aspiration, the headline — as a side effect of setting a
    // tolerance, and splitting the width would leave sigmaLo != sigmaHi, which validateNormsAgainst
    // refuses on a one-sided row. A legal-looking gesture would author an invalid pack. The
    // one-sided operations are setAspiration and setTolerance, and nudgeClaimLo/Hi route to them.
    if (shapeIsOneSided(shape())) return;

    if (hi < lo) std::swap(lo, hi);

    // A zero-width band admits only its own centre — norm.h calls that degenerate but well-defined,
    // and the validator warns about it separately. It is not this control's job to refuse it, but
    // it IS this control's job not to produce a negative tolerance from a crossed drag.
    m_draft.mu      = 0.5 * (lo + hi);
    m_draft.sigmaLo = m_draft.mu - lo;
    m_draft.sigmaHi = hi - m_draft.mu;

    // Hand-editing makes this an authored figure again. Leaving `Seated · n = 42` attached to
    // numbers a hand has since moved would be a provenance claim the norm no longer supports.
    dropDerivedProvenance();
}

// Hand-editing makes this an authored figure again, on every route into the numbers. Factored out
// of setClaimBand so the one-sided mutators cannot quietly keep a `Seated · n = 42` label attached
// to a mu a hand has since dragged.
void NormEditorModel::dropDerivedProvenance()
{
    if (m_draft.source == NormSource::Seated || m_draft.source == NormSource::Imported) {
        m_draft.source = NormSource::Heuristic;
        m_draft.n      = 0;
    }
    m_dirty = true;
    emit draftChanged();
    emit sampleChanged();
}

void NormEditorModel::setAspiration(double mu)
{
    if (!shapeIsOneSided(shape())) return;      // a target norm's centre is a consequence, not a control
    if (!std::isfinite(mu) || qFuzzyCompare(1.0 + mu, 1.0 + m_draft.mu)) return;
    m_draft.mu = mu;
    dropDerivedProvenance();
}

void NormEditorModel::setTolerance(double tolerance)
{
    if (!shapeIsOneSided(shape())) return;
    if (!std::isfinite(tolerance)) return;
    // Magnitude, because the graded side is below mu on a floor and above it on a ceiling, and a
    // user typing -0.05 into a field labelled TOLERANCE means the same thing either way.
    const double t = std::fabs(tolerance);
    if (qFuzzyCompare(1.0 + t, 1.0 + m_draft.sigmaLo) && qFuzzyCompare(1.0 + t, 1.0 + m_draft.sigmaHi))
        return;
    // BOTH sides, always equal. validateNormsAgainst refuses a one-sided row whose sigmas differ,
    // and the ungraded one is a number nothing reads — letting it drift would author an invalid
    // pack out of a gesture that looked entirely legal.
    m_draft.sigmaLo = t;
    m_draft.sigmaHi = t;
    dropDerivedProvenance();
}

void NormEditorModel::setPlausibleLo(double v)
{
    if (!std::isfinite(v)) return;
    m_draft.plausibleLo = v;
    dropDerivedProvenance();
}

void NormEditorModel::setPlausibleHi(double v)
{
    if (!std::isfinite(v)) return;
    m_draft.plausibleHi = v;
    dropDerivedProvenance();
}

void NormEditorModel::clearPlausibleLo()
{
    if (!m_draft.plausibleLo.has_value()) return;
    m_draft.plausibleLo.reset();
    dropDerivedProvenance();
}

void NormEditorModel::clearPlausibleHi()
{
    if (!m_draft.plausibleHi.has_value()) return;
    m_draft.plausibleHi.reset();
    dropDerivedProvenance();
}

// The two rules, mirroring validateNormPack's `plausibleOrder` and validateNormsAgainst's
// `plausibleInsideCorridor` exactly. Duplicated in the sense that the words differ — these are
// addressed to an author mid-gesture, not to a reader of a load report — but never in the sense
// that the arithmetic differs, which is why both read the same lenient edge.
QString NormEditorModel::plausibleLoProblem() const
{
    if (!m_draft.plausibleLo.has_value()) return QString();

    if (m_draft.plausibleHi.has_value() && *m_draft.plausibleLo > *m_draft.plausibleHi)
        return tr("The lower bound is above the upper one.");

    const NormBandEdges e =
        bandEdgesOf(m_draft, gradePolicyByName(QStringLiteral("lenient")), -1.0, shape());
    if (!e.lowOpen && *m_draft.plausibleLo > e.watchLo)
        return tr("The corridor grades down to %1, so a reading there would be called a fault and "
                  "disbelieved at once. Move this to %1 or below.").arg(fmtNum(e.watchLo));
    return QString();
}

QString NormEditorModel::plausibleHiProblem() const
{
    if (!m_draft.plausibleHi.has_value()) return QString();

    if (m_draft.plausibleLo.has_value() && *m_draft.plausibleLo > *m_draft.plausibleHi)
        return tr("The upper bound is below the lower one.");

    const NormBandEdges e =
        bandEdgesOf(m_draft, gradePolicyByName(QStringLiteral("lenient")), -1.0, shape());
    if (!e.highOpen && *m_draft.plausibleHi < e.watchHi)
        return tr("The corridor grades up to %1, so a reading there would be called a fault and "
                  "disbelieved at once. Move this to %1 or above.").arg(fmtNum(e.watchHi));
    return QString();
}

void NormEditorModel::nudgeGradedEdge(double edgeValue)
{
    // Clamped at zero rather than taken as a magnitude, and the difference is what the drag feels
    // like. setTolerance() takes |t| because a user TYPING -0.05 into a field labelled TOLERANCE
    // means 0.05; a user DRAGGING the edge past the centre means "smaller, and smaller again", and
    // reflecting it out the far side would make the handle leap away from the pointer. Clamping
    // parks it on the centre — a zero tolerance, degenerate but well-defined (norm.h) — which is
    // also why the two-sided swap-follow has no counterpart here: nothing can cross anything.
    switch (shape()) {
    case Shape::Floor:   setTolerance(std::max(0.0, m_draft.mu - edgeValue)); return;
    case Shape::Ceiling: setTolerance(std::max(0.0, edgeValue - m_draft.mu)); return;
    case Shape::Target:  return;
    }
}

// Shape-aware routers, so a caller holding "the low field" keeps working on all three shapes and
// no surface has to know which of the two interactions it is driving.
void NormEditorModel::nudgeClaimLo(double to)
{
    switch (shape()) {
    case Shape::Floor:   nudgeGradedEdge(to); return;   // on a floor the LOW edge is the graded one
    case Shape::Ceiling: return;                        // …and here the low side is open: no edge to move
    case Shape::Target:  break;
    }
    setClaimBand(to, m_draft.claimHi());
}

void NormEditorModel::nudgeClaimHi(double to)
{
    switch (shape()) {
    case Shape::Ceiling: nudgeGradedEdge(to); return;
    case Shape::Floor:   return;
    case Shape::Target:  break;
    }
    setClaimBand(m_draft.claimLo(), to);
}

void NormEditorModel::beginHandleDrag()
{
    if (m_axisLocked)
        return;
    // Latch whatever the axis is right now, BEFORE the first nudge moves the corridor.
    const QVariantMap now = sampleSummary();
    m_lockedAxisLo = now.value(QStringLiteral("axisLo")).toDouble();
    m_lockedAxisHi = now.value(QStringLiteral("axisHi")).toDouble();
    m_axisLocked   = true;
}

void NormEditorModel::endHandleDrag()
{
    if (!m_axisLocked)
        return;
    m_axisLocked = false;
    // Re-fit now the gesture is over: a corridor dragged past the old edge should come back into
    // frame, just not while it is being dragged.
    emit sampleChanged();
}

// ── The drawn sample ────────────────────────────────────────────────────────

void NormEditorModel::setLibraryRoot(const QString &path)
{
    if (path == m_libraryRoot) return;
    m_libraryRoot = path;
    if (m_open) startScan();
    else        emit sampleChanged();
}

void NormEditorModel::setDrawFrom(const QString &d)
{
    const QString resolved = (d == QLatin1String("athlete") || d == QLatin1String("session"))
                                 ? d : QStringLiteral("all");
    if (resolved == m_drawFrom) return;
    m_drawFrom = resolved;
    // The scan is scope-independent — it reads the whole library once and the filter selects from
    // it. Re-scanning per scope would re-parse the library three times to answer one question.
    emit sampleChanged();
}

void NormEditorModel::rescan() { startScan(); }

void NormEditorModel::startScan()
{
    const Measure *m = measure();
    if (!m || m_libraryRoot.isEmpty()) {
        m_samples.clear();
        m_scanned     = 0;
        m_scanning    = false;
        m_everScanned = !m_libraryRoot.isEmpty();
        emit sampleChanged();
        return;
    }
    if (m_watcher->isRunning())
        m_watcher->waitForFinished();

    const QString root    = m_libraryRoot;
    const Measure target  = *m;                 // by value: the worker must not read the pack

    m_scanning = true;
    emit sampleChanged();

    m_watcher->setFuture(QtConcurrent::run([root, target]() -> QVariantList {
        QVariantList out;

        // <root>/<athlete>/<session>/swing_*/ — the layout swing_paths.h documents. Walked here
        // rather than through SwingDocReader because that lives in Export, which drags the whole
        // exporter (and FFmpeg) in for a directory listing.
        const QDir rootDir(root);
        const QStringList athletes = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &athlete : athletes) {
            const QDir aDir(rootDir.filePath(athlete));
            const QStringList sessions = aDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &session : sessions) {
                const QDir sDir(aDir.filePath(session));
                const QStringList swings =
                    sDir.entryList({ QStringLiteral("swing_*") }, QDir::Dirs, QDir::Name);
                for (const QString &swing : swings) {
                    if (out.size() >= kMaxScan) {
                        QVariantMap capped;
                        capped.insert(QStringLiteral("truncated"), true);
                        out.append(capped);
                        return out;
                    }
                    const QString swingDir = sDir.filePath(swing);
                    const SwingPhaseGrid grid = readPhaseGrid(swingDir, /*writeSidecar*/ true);
                    const std::optional<double> v = reduceOverGrid(grid, target);

                    QVariantMap r;
                    r.insert(QStringLiteral("swingDir"),    swingDir);
                    r.insert(QStringLiteral("athlete"),     athlete);
                    r.insert(QStringLiteral("session"),     session);
                    r.insert(QStringLiteral("club"),        grid.club);
                    r.insert(QStringLiteral("ordinal"),     grid.ordinal);
                    r.insert(QStringLiteral("wallclockMs"), grid.wallclockMs);
                    r.insert(QStringLiteral("produced"),    v.has_value());
                    r.insert(QStringLiteral("value"),       v.value_or(0.0));
                    out.append(r);
                }
            }
        }
        return out;
    }));
}

void NormEditorModel::onScanFinished()
{
    m_scanning    = false;
    m_everScanned = true;

    // The draft closed while this was in flight. Landing an old sample under whatever is open now
    // would put one measure's swings beneath another measure's corridor — the histogram would look
    // perfectly plausible and be about the wrong thing entirely.
    if (!m_open) {
        m_samples.clear();
        emit sampleChanged();
        return;
    }

    const QVariantList rows = m_watcher->result();

    m_samples.clear();
    m_scanned   = 0;
    m_truncated = false;

    // The most recent session on disk, by directory mtime — the same recency basis
    // SwingDocReader::latestSessionDir uses, because folder names embed the naming pattern and a
    // name sort does not track recency. It is what "this session" and "this athlete" mean.
    QDateTime newest;
    m_athleteFolder.clear();
    m_sessionFolder.clear();

    for (const QVariant &rv : rows) {
        const QVariantMap r = rv.toMap();
        if (r.value(QStringLiteral("truncated")).toBool()) { m_truncated = true; continue; }
        ++m_scanned;

        const QString swingDir = r.value(QStringLiteral("swingDir")).toString();
        const QDateTime mt = QFileInfo(QFileInfo(swingDir).absolutePath()).lastModified();
        if (!newest.isValid() || mt > newest) {
            newest          = mt;
            m_sessionFolder = r.value(QStringLiteral("session")).toString();
            m_athleteFolder = r.value(QStringLiteral("athlete")).toString();
        }

        if (!r.value(QStringLiteral("produced")).toBool())
            continue;

        Sample s;
        s.swingDir    = swingDir;
        s.athlete     = r.value(QStringLiteral("athlete")).toString();
        s.session     = r.value(QStringLiteral("session")).toString();
        s.club        = r.value(QStringLiteral("club")).toString();
        s.ordinal     = r.value(QStringLiteral("ordinal")).toInt();
        s.wallclockMs = r.value(QStringLiteral("wallclockMs")).toLongLong();
        s.value       = r.value(QStringLiteral("value")).toDouble();
        s.included    = true;
        m_samples.push_back(std::move(s));
    }

    // Stable, readable order: oldest first, so the list reads like the library does.
    std::sort(m_samples.begin(), m_samples.end(), [](const Sample &a, const Sample &b) {
        if (a.session != b.session) return a.session < b.session;
        return a.ordinal < b.ordinal;
    });

    emit sampleChanged();
}

// "This athlete" and "this session" are the athlete and session folders of the MOST RECENTLY
// MODIFIED session on disk — the same recency basis SwingDocReader::latestSessionDir uses, and the
// only definition available to a model that is deliberately free of the session controller.
bool NormEditorModel::passesScope(const Sample &s, const QString &scope) const
{
    if (scope == QLatin1String("session")) return s.session == m_sessionFolder;
    if (scope == QLatin1String("athlete")) return s.athlete == m_athleteFolder;
    return true;
}

std::vector<const NormEditorModel::Sample *> NormEditorModel::visible() const
{
    std::vector<const Sample *> out;
    for (const Sample &s : m_samples)
        if (passesScope(s, m_drawFrom)) out.push_back(&s);
    return out;
}

QVariantList NormEditorModel::drawFromOptions() const
{
    QVariantList out;
    const auto add = [&](const char *name, const QString &label, bool enabled) {
        int n = 0;
        for (const Sample &s : m_samples)
            if (passesScope(s, QString::fromLatin1(name))) ++n;

        QVariantMap r;
        r.insert(QStringLiteral("name"),    QString::fromLatin1(name));
        r.insert(QStringLiteral("label"),   label);
        r.insert(QStringLiteral("count"),   n);
        r.insert(QStringLiteral("enabled"), enabled);
        out.append(r);
    };
    add("all",     tr("All swings"),   true);
    add("athlete", tr("This athlete"), !m_athleteFolder.isEmpty());
    add("session", tr("This session"), !m_sessionFolder.isEmpty());
    return out;
}

void NormEditorModel::setSampleIncluded(const QString &swingDir, bool on)
{
    for (Sample &s : m_samples)
        if (s.swingDir == swingDir) { s.included = on; emit sampleChanged(); return; }
}

void NormEditorModel::setAllIncluded(bool on)
{
    for (Sample &s : m_samples)
        if (passesScope(s, m_drawFrom)) s.included = on;
    emit sampleChanged();
}

// ── Seating ─────────────────────────────────────────────────────────────────

QVariantMap NormEditorModel::seatFromSample()
{
    QVariantMap out;

    std::vector<double> vals;
    for (const Sample *s : visible())
        if (s->included) vals.push_back(s->value);

    if (static_cast<int>(vals.size()) < kMinSeatSamples) {
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("message"),
                   tr("%n marked swing(s) is not a population. Mark at least %1 before fitting.",
                      "", int(vals.size())).arg(kMinSeatSamples));
        return out;
    }

    if (shapeIsOneSided(shape())) {
        const OneSidedFit fit = fitOneSided(vals, shape());
        // ONE tolerance, on both sides, and there is no borrow fallback. The two-sided one below
        // exists because splitting a sample about its mean can legitimately leave a side empty,
        // which is an artefact of the split. Here both order statistics come from the same
        // one-sided distribution, so a zero spread means the sample really is a point mass — a
        // degenerate but well-defined corridor (norm.h) — and saying so beats inventing a width.
        m_draft.mu      = fit.mu;
        m_draft.sigmaLo = fit.tolerance;
        m_draft.sigmaHi = fit.tolerance;
        m_draft.n       = int(vals.size());
        m_draft.source  = NormSource::Seated;
        m_draft.setOn   = QDate::currentDate();
        m_dirty         = true;

        emit draftChanged();
        emit sampleChanged();

        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("message"),
                   tr("Fitted to %n swing(s).", "", int(vals.size())));
        return out;
    }

    // Mean and a PER-SIDE deviation about it, so an asymmetric sample produces an asymmetric
    // corridor — the same asymmetry the two handles express by hand, and the reason Norm carries
    // sigmaLo and sigmaHi separately rather than one tolerance (norm.h).
    const double mean =
        std::accumulate(vals.begin(), vals.end(), 0.0) / double(vals.size());

    double sumLo = 0.0, sumHi = 0.0;
    int    nLo = 0, nHi = 0;
    for (double v : vals) {
        const double d = v - mean;
        if (d < 0.0) { sumLo += d * d; ++nLo; }
        else         { sumHi += d * d; ++nHi; }
    }
    // A side with no samples borrows the other's spread rather than collapsing to zero: a corridor
    // with a zero tolerance on one side grades everything past the centre as Action, which is a
    // far stronger claim than "the sample happened to fall on one side".
    const double sdLo = nLo > 0 ? std::sqrt(sumLo / double(nLo)) : 0.0;
    const double sdHi = nHi > 0 ? std::sqrt(sumHi / double(nHi)) : 0.0;

    m_draft.mu      = mean;
    m_draft.sigmaLo = sdLo > 0.0 ? sdLo : sdHi;
    m_draft.sigmaHi = sdHi > 0.0 ? sdHi : sdLo;
    m_draft.n       = int(vals.size());
    m_draft.source  = NormSource::Seated;
    m_draft.setOn   = QDate::currentDate();
    m_dirty         = true;

    emit draftChanged();
    emit sampleChanged();

    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("message"),
               tr("Fitted to %n swing(s).", "", int(vals.size())));
    return out;
}

// ── Import ──────────────────────────────────────────────────────────────────

QVariantList NormEditorModel::importCandidates() const
{
    QVariantList out;
    if (!m_open)
        return out;

    const Shape sh = shape();

    // Every context that carries its OWN row for this measure, excluding the one being edited.
    // "Adopt a row from another norm pack" reduces to this today — layers are merged into one
    // assembled set by (measureId, contextId), so a user row and a shipped row for the same key
    // are not two candidates, they are one resolved value.
    for (const QString &cid : m_norms->overriddenContextsFor(m_measureId)) {
        if (cid == m_contextId) continue;
        const Norm *n = m_norms->norms().find(m_measureId, cid);
        if (!n) continue;

        QVariantMap r;
        r.insert(QStringLiteral("contextId"),    cid);
        r.insert(QStringLiteral("contextLabel"), contextLabel(cid));
        r.insert(QStringLiteral("mu"),           n->mu);
        // A preview of what adopting this row would GRADE as Ideal, so it reads on the same scale
        // as the plot beside it rather than on the row's bare claim.
        const NormBandEdges ce = bandEdgesOf(*n, policy(), -1.0, sh);
        r.insert(QStringLiteral("idealLo"),      ce.idealLo);
        r.insert(QStringLiteral("idealHi"),      ce.idealHi);
        // The row's own claim as a fragment, so the list reads "at least 1.4" on a one-sided
        // measure rather than "1.4 to 1.5", whose second number is not a bound of anything. Every
        // candidate is a row for the SAME measure at another context, so they all share this
        // measure's shape — that is the whole reason shape sits on the measure and not the norm.
        r.insert(QStringLiteral("rangeText"),    claimPhrase(*n));
        r.insert(QStringLiteral("sourceLabel"),  sourceLabel(n->source));
        r.insert(QStringLiteral("n"),            n->n);
        out.append(r);
    }
    return out;
}

void NormEditorModel::adoptFrom(const QString &contextId)
{
    const Norm *n = m_norms->norms().find(m_measureId, contextId);
    if (!n) return;

    m_draft.mu      = n->mu;
    m_draft.sigmaLo = n->sigmaLo;
    m_draft.sigmaHi = n->sigmaHi;
    m_draft.n       = n->n;
    m_draft.source  = NormSource::Imported;
    m_draft.setOn   = QDate::currentDate();
    m_draft.citation = tr("Adopted from %1.").arg(contextLabel(contextId));
    m_dirty = true;

    emit draftChanged();
    emit sampleChanged();
}

// ── Commit ──────────────────────────────────────────────────────────────────

QVariantMap NormEditorModel::save()
{
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);

    if (!m_open) {
        out.insert(QStringLiteral("message"), tr("Nothing is open to save."));
        return out;
    }
    if (!m_refused.isEmpty()) {
        out.insert(QStringLiteral("message"), m_refused);
        return out;
    }
    if (!(m_draft.sigmaLo >= 0.0) || !(m_draft.sigmaHi >= 0.0)) {
        out.insert(QStringLiteral("message"), tr("A corridor cannot have a negative tolerance."));
        return out;
    }
    // Refused HERE and not left to the pack validation below, because that call is
    // validateNormPack — the standalone one, which cannot see the measure and therefore carries
    // `plausibleOrder` but NOT `plausibleInsideCorridor`. Without this the editor would happily
    // write a row that the referential validator flags at the next load: a rule that exists, is
    // tested, and never runs where the mistake is made.
    for (const QString &why : { plausibleLoProblem(), plausibleHiProblem() }) {
        if (why.isEmpty()) continue;
        out.insert(QStringLiteral("message"), why);
        return out;
    }

    // The unit is re-read at save time, not trusted from the draft: norm_pack's referential
    // validator refuses a norm whose unit is not its measure's, and a draft opened before a pack
    // edit could otherwise carry a stale one into the file and fail to load next launch.
    m_draft.unit = unitOf();
    if (!m_draft.setOn.isValid())
        m_draft.setOn = QDate::currentDate();

    // Record the SHIPPED numbers this override is being made against, if core carries a row at this
    // exact key. It is the base of a three-way comparison, and it is what lets the health list say
    // later that the shipped corridor has been revised since — a claim a two-way comparison cannot
    // make, because "yours differs from theirs" is also just what an override is. Re-stamped on every
    // save, so re-editing an override re-bases it against what core says today; that is right,
    // because the author has just looked at the shipped band beside their own.
    if (const Norm *shipped = m_norms ? m_norms->shippedNorm(m_measureId, m_contextId, m_cohort)
                                     : nullptr) {
        NormBasis basis;
        basis.mu        = shipped->mu;
        basis.sigmaLo   = shipped->sigmaLo;
        basis.sigmaHi   = shipped->sigmaHi;
        basis.monitorLo = shipped->monitorLo;
        basis.monitorHi = shipped->monitorHi;
        // A2 put these on NormBasis and taught the pack to persist them, but nothing ever WROTE
        // them here — so the base recorded against a capped shipped row was silently uncapped, and
        // the "core has been revised" notice could never fire on a cap. A field complete on both
        // sides, reaching nothing.
        basis.plausibleLo = shipped->plausibleLo;
        basis.plausibleHi = shipped->plausibleHi;
        m_draft.basedOn = basis;
    } else {
        m_draft.basedOn.reset();      // nothing shipped here: there is no base to record
    }

    NormPack pack = readUserPack();
    pack.upsert(m_draft);

    const ValidationReport rep = validateNormPack(pack);
    if (!rep.ok()) {
        QString why;
        for (const ValidationIssue &i : rep.issues)
            if (i.severity == IssueSeverity::Error) { why = i.message; break; }
        out.insert(QStringLiteral("message"),
                   why.isEmpty() ? tr("The norm set would not be valid.") : why);
        return out;
    }

    QString whyNot;
    if (!saveUserNormPack(pack, &whyNot)) {
        out.insert(QStringLiteral("message"),
                   whyNot.isEmpty() ? tr("Could not write the norm set.") : whyNot);
        return out;
    }

    // Ledger C4: without this the write is on disk but invisible — every reader holds the cached
    // provider until the next launch, so the editor would appear to do nothing.
    reload();
    m_hadOwnRow = true;
    m_original  = m_draft;
    m_dirty     = false;

    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("message"),
               tr("Saved to your norm set. This is the population norm for everyone using it."));
    emit draftChanged();
    emit sampleChanged();
    emit normsChanged();
    return out;
}

QVariantMap NormEditorModel::discardChanges()
{
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);
    if (!m_open) {
        out.insert(QStringLiteral("message"), tr("Nothing is open."));
        return out;
    }
    if (!m_dirty) {
        out.insert(QStringLiteral("message"), tr("Nothing has been changed."));
        return out;
    }

    // Re-seed from the ORIGINAL captured at begin(), not from a fresh resolve: if the draft was
    // saved and then dragged again, the user means "back to how I found it this time", and a
    // resolve would hand back the saved edit instead.
    m_draft = m_original;
    m_dirty = false;
    emit draftChanged();
    emit sampleChanged();

    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("message"), tr("Changes discarded. Nothing was saved."));
    return out;
}

QVariantMap NormEditorModel::resetToDefault()
{
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);
    if (!m_open) {
        out.insert(QStringLiteral("message"), tr("Nothing is open to reset."));
        return out;
    }

    // Read BEFORE the drop: afterwards there is no override left to describe, and the message has
    // to say which of the two outcomes actually happened.
    const bool hadShipped = m_norms->shippedNorm(m_measureId, m_contextId, m_cohort) != nullptr;

    NormPack pack = readUserPack();
    if (!pack.remove(m_measureId, m_contextId, m_cohort)) {
        // Nothing of the user's to remove. The shipped row is NOT deleted — the core set is
        // read-only by design, and resetting means dropping your override, never editing what
        // shipped.
        out.insert(QStringLiteral("message"),
                   tr("This corridor is the shipped one — there is nothing of yours to drop."));
        return out;
    }

    QString whyNot;
    if (!saveUserNormPack(pack, &whyNot)) {
        out.insert(QStringLiteral("message"),
                   whyNot.isEmpty() ? tr("Could not write the norm set.") : whyNot);
        return out;
    }

    reload();
    const QString measureId = m_measureId, contextId = m_contextId;
    begin(measureId, contextId);                   // re-seed from whatever now resolves

    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("message"),
               hadShipped ? tr("Reset to the shipped corridor.")
                          : tr("Your override is gone. This context inherits again."));
    emit normsChanged();
    return out;
}

// ── Rendering shapes ────────────────────────────────────────────────────────

QVariantMap NormEditorModel::draft() const
{
    QVariantMap out;
    if (!m_open)
        return out;

    const Measure *m   = measure();
    const GradePolicy p = policy();
    const Shape       sh = shape();

    out.insert(QStringLiteral("measureId"),    m_measureId);
    out.insert(QStringLiteral("measureLabel"), m ? (m->label.isEmpty() ? m->id : m->label) : m_measureId);
    out.insert(QStringLiteral("unit"),         unitOf());
    out.insert(QStringLiteral("highMeans"),    m ? m->highMeans : QString());
    out.insert(QStringLiteral("contextId"),    m_contextId);
    out.insert(QStringLiteral("contextLabel"), contextLabel(m_contextId));
    // WHICH POPULATION this draft is for. It has to be on screen: a segmented corridor and the
    // universal one are edited on an identical panel, and an author who cannot see which they have
    // open will eventually save one believing it was the other. `cohortNote` is the whole sentence
    // because that decision — segment or everyone — is not a formatting choice.
    out.insert(QStringLiteral("cohort"),       cohortToMap(m_cohort));
    out.insert(QStringLiteral("cohortLabel"),  cohortLabel(m_cohort));
    out.insert(QStringLiteral("cohortNote"),
               m_cohort.isUnqualified()
                   ? QString()
                   : tr("This corridor grades %1 only. Everyone else is graded by the corridor "
                        "beside it.").arg(cohortLabel(m_cohort)));
    out.insert(QStringLiteral("route"),        m_route);

    // The Watch edge through bandEdgesOf(), so the editor draws the edge that GRADES. It matters for
    // a migrated row: the draft is a copy of the resolved norm, monitor bounds and all, and those
    // bounds dominate the z-derived edge inside grade(). Deriving the number here from sigma alone
    // showed the wrong edge on all 56 migrated corridors — a corridor the app does not use, in the
    // one screen whose entire job is to show what a corridor does.
    const NormBandEdges e = bandEdgesOf(m_draft, p, -1.0, sh);

    out.insert(QStringLiteral("mu"),      m_draft.mu);

    // TWO different pairs, and the editor is the one screen where the difference is visible.
    //
    //   claimLo/claimHi  — mu +/- sigma, what THIS NORM asserts. The handles bind here, the
    //                      numeric fields edit here, and neither moves when the grade policy
    //                      changes: a sensitivity setting must not edit an assertion.
    //   idealLo/idealHi  — mu +/- idealMaxZ * sigma, what the ACTIVE POLICY grades as Ideal. The
    //                      green band on the plot is drawn from these.
    //
    // Under `standard` they are the same numbers, which is why one pair sufficed for nine stages.
    // Under `strict` the handles now sit OUTSIDE the drawn green core, and that is the point: it is
    // the policy visibly making more of the corridor than Ideal, stated rather than hidden.
    out.insert(QStringLiteral("claimLo"), m_draft.claimLo());
    out.insert(QStringLiteral("claimHi"), m_draft.claimHi());
    out.insert(QStringLiteral("idealLo"), e.idealLo);
    out.insert(QStringLiteral("idealHi"), e.idealHi);
    // The Good edges are the one pair computed HERE rather than by bandEdgesOf, which is why they
    // need the shape collapse spelled out: without it a floor would draw its Good band running two
    // sigma ABOVE the aspiration, over the top of the Ideal band that owns that whole side. Same
    // rule, same place in the sequence — see bandEdgesOf().
    const double goodLo = e.lowOpen  ? m_draft.mu : (m_draft.mu - p.goodMaxZ * m_draft.sigmaLo);
    const double goodHi = e.highOpen ? m_draft.mu : (m_draft.mu + p.goodMaxZ * m_draft.sigmaHi);
    out.insert(QStringLiteral("goodLo"),  goodLo);
    out.insert(QStringLiteral("goodHi"),  goodHi);
    out.insert(QStringLiteral("watchLo"), e.watchLo);
    out.insert(QStringLiteral("watchHi"), e.watchHi);
    out.insert(QStringLiteral("explicitMonitor"), m_draft.hasExplicitMonitor(sh));

    // ── Shape ───────────────────────────────────────────────────────────────
    //
    // Written explicitly on every draft including two-sided ones, both flags and all: QML reads a
    // missing key as `undefined` and `undefined === true` is false, so an omitted flag looks
    // exactly like a considered one and the place a bug could hide is the place nobody looks.
    out.insert(QStringLiteral("shape"),      shapeName(sh));
    out.insert(QStringLiteral("shapeLabel"), shapeLabel(sh));
    out.insert(QStringLiteral("oneSided"),   shapeIsOneSided(sh));
    out.insert(QStringLiteral("lowOpen"),    e.lowOpen);
    out.insert(QStringLiteral("highOpen"),   e.highOpen);

    // The two numbers a one-sided corridor actually has. `tolerance` is the graded slack and
    // `gradedEdge` is where it lands — the same quantity in the field's coordinates and the plot's,
    // so QML converts nothing. On a target norm they are the low half, which nothing reads.
    const double tol = (sh == Shape::Ceiling) ? m_draft.sigmaHi : m_draft.sigmaLo;
    out.insert(QStringLiteral("tolerance"),  tol);
    out.insert(QStringLiteral("gradedEdge"), (sh == Shape::Ceiling) ? (m_draft.mu + tol)
                                                                   : (m_draft.mu - tol));

    // The end-cap the open side terminates in. It must never be a hard edge: a band that stops at
    // the edge of a plot reads as a bound, and the whole claim of a one-sided norm is that there
    // isn't one on that side.
    out.insert(QStringLiteral("openEndLabel"),
               sh == Shape::Floor   ? tr("no upper limit")
             : sh == Shape::Ceiling ? tr("no lower limit")
                                    : QString());

    // Shape as a sentence, with the measure's own highMeans folded in where it has one — so the
    // author reads "Higher is better: more of the clubhead's speed reaching the ball" rather than
    // a bare enum word they have to look up.
    const QString hm = m ? m->highMeans : QString();
    out.insert(QStringLiteral("shapeNote"),
               !shapeIsOneSided(sh)  ? QString()
             : hm.isEmpty()          ? shapeLabel(sh)
                                     : tr("%1: %2").arg(shapeLabel(sh), hm));

    // What this corridor claims, as a fragment: "1.4 to 1.5" / "at least 1.5" / "no more than 12.0".
    out.insert(QStringLiteral("claimPhrase"), claimPhrase(m_draft));

    // ── Plausibility ────────────────────────────────────────────────────────
    //
    // `has*` separately from the value, because absent and zero are different answers and a
    // QVariantMap double cannot tell them apart — `.toDouble()` on a missing key yields 0.0, which
    // would read as "stops believing readings below zero" on every uncapped row in the pack.
    out.insert(QStringLiteral("hasPlausibleLo"), m_draft.plausibleLo.has_value());
    out.insert(QStringLiteral("hasPlausibleHi"), m_draft.plausibleHi.has_value());
    out.insert(QStringLiteral("plausibleLo"),    m_draft.plausibleLo.value_or(0.0));
    out.insert(QStringLiteral("plausibleHi"),    m_draft.plausibleHi.value_or(0.0));
    out.insert(QStringLiteral("plausibleLoError"), plausibleLoProblem());
    out.insert(QStringLiteral("plausibleHiError"), plausibleHiProblem());

    // What the ACTIVE POLICY makes of it. Read-only, and quoted with the Ideal edge alongside Good
    // and Watch because under any preset but `standard` Ideal is not where the handles are — an
    // author who saw Good and Watch move with the policy but not Ideal would reasonably conclude
    // the green band was fixed. One-sided, only the graded tail has edges to quote, and saying
    // "action beyond 1.3 – 1.5" on a floor would name a fault on the side that grades Ideal.
    const auto &f1 = fmtNum;
    QString policyNote;
    if (sh == Shape::Floor)
        policyNote = tr("Ideal from %1 · good from %2 · action below %3")
                         .arg(f1(e.idealLo), f1(goodLo), f1(e.watchLo));
    else if (sh == Shape::Ceiling)
        policyNote = tr("Ideal to %1 · good to %2 · action above %3")
                         .arg(f1(e.idealHi), f1(goodHi), f1(e.watchHi));
    else
        policyNote = tr("Ideal %1 – %2 · good to %3 – %4 · action beyond %5 – %6")
                         .arg(f1(e.idealLo), f1(e.idealHi), f1(goodLo), f1(goodHi),
                              f1(e.watchLo), f1(e.watchHi));
    out.insert(QStringLiteral("policyNote"), policyNote);

    out.insert(QStringLiteral("n"),           m_draft.n);
    out.insert(QStringLiteral("source"),      normSourceName(m_draft.source));
    out.insert(QStringLiteral("sourceLabel"), sourceLabel(m_draft.source));
    out.insert(QStringLiteral("author"),      m_draft.author);
    out.insert(QStringLiteral("citation"),    m_draft.citation);
    out.insert(QStringLiteral("setOn"),
               m_draft.setOn.isValid() ? m_draft.setOn.toString(Qt::ISODate) : QString());
    out.insert(QStringLiteral("weak"),        normIsWeak(m_draft));
    out.insert(QStringLiteral("weakReason"),  normWeakReason(m_draft));

    // The inheritance line: what this context resolves to WITHOUT the draft, so an author can see
    // what they are overriding and by how much.
    out.insert(QStringLiteral("own"), m_hadOwnRow);
    const ContextNode *node = m_norms->contexts().node(m_contextId);
    const QString parentId  = node ? node->parentId : QString();
    const NormResolution par =
        parentId.isEmpty() ? NormResolution{} : m_norms->resolve(m_measureId, parentId, m_cohort);
    out.insert(QStringLiteral("inherited"),     !m_hadOwnRow);
    out.insert(QStringLiteral("inheritedFrom"), par.found() ? contextLabel(par.contextId) : QString());
    // A DIFF against the parent row, so both sides are CLAIMS. "You are 37 wider than the full
    // swing" compares two assertions; running it through the grade policy would scale both by the
    // same factor and change the sentence for no reason the reader could act on.
    out.insert(QStringLiteral("parentClaimLo"), par.found() ? par.norm->claimLo() : 0.0);
    out.insert(QStringLiteral("parentClaimHi"), par.found() ? par.norm->claimHi() : 0.0);
    out.insert(QStringLiteral("hasParent"),     par.found());

    // The inheritance line, whole, because it is one sentence with a decision inside it. "You are
    // 37 wider than the full swing" is what tells an author whether the override is worth having.
    //
    // ONE-SIDED, "wider" is still exactly the right word — it compares the two graded tolerances,
    // which is the only width either corridor has. What changes is the first half: a floor's parent
    // "sets at least 1.40", not "sets 1.32 to 1.48", and the second number in that pair was never
    // real.
    QString parentNote;
    if (par.found()) {
        const double mine   = shapeIsOneSided(sh) ? tol
                                                  : (m_draft.claimHi() - m_draft.claimLo());
        const double theirs = shapeIsOneSided(sh)
                                  ? ((sh == Shape::Ceiling) ? par.norm->sigmaHi : par.norm->sigmaLo)
                                  : (par.norm->claimHi() - par.norm->claimLo());
        parentNote = tr("%1 sets %2. This corridor is %3 %4 wide against its %5.")
                         .arg(contextLabel(par.contextId), claimPhrase(*par.norm),
                              f1(mine), unitOf(), f1(theirs));
    }
    out.insert(QStringLiteral("parentNote"), parentNote);

    out.insert(QStringLiteral("dirty"),          m_dirty);
    out.insert(QStringLiteral("refused"),        !m_refused.isEmpty());
    out.insert(QStringLiteral("refusedReason"),  m_refused);
    out.insert(QStringLiteral("canSave"),        m_refused.isEmpty() && m_dirty);
    out.insert(QStringLiteral("canDiscard"),     m_dirty);

    // ── Shipped vs yours ────────────────────────────────────────────────────
    //
    // Only the USER's own override can be reset. `own`/m_hadOwnRow is true for a SHIPPED row too,
    // so keying off that would offer an action that can only fail. And what the reset PROMISES
    // depends on whether core carries a row at this exact key — see resetToDefault().
    const bool  overridden = m_norms->isOverridden(m_measureId, m_contextId, m_cohort);
    const Norm *shipped    = m_norms->shippedNorm(m_measureId, m_contextId, m_cohort);

    out.insert(QStringLiteral("overridden"), overridden);
    out.insert(QStringLiteral("canReset"),   overridden);
    out.insert(QStringLiteral("hasShipped"), shipped != nullptr);
    // Also a diff, also both sides claims — see parentClaimLo above.
    out.insert(QStringLiteral("shippedClaimLo"), shipped ? shipped->claimLo() : 0.0);
    out.insert(QStringLiteral("shippedClaimHi"), shipped ? shipped->claimHi() : 0.0);
    out.insert(QStringLiteral("resetLabel"),
               shipped ? tr("Reset to shipped") : tr("Remove your override"));

    // The sentence the provenance block shows when this corridor is not what PinPoint ships. Built
    // here rather than assembled in a delegate: which of the three cases applies is a statement
    // about the norm stack, not a formatting choice.
    QString editedNote;
    if (overridden && shipped)
        editedNote = tr("You changed this. PinPoint ships %1 %2.")
                         .arg(claimPhrase(*shipped), unitOf());
    else if (overridden)
        editedNote = tr("You added this. PinPoint ships no corridor for %1 — without yours it "
                        "would inherit.").arg(contextLabel(m_contextId));
    out.insert(QStringLiteral("editedNote"), editedNote);
    return out;
}

QVariantList NormEditorModel::samples() const
{
    const GradePolicy p = policy();
    const Shape       sh = shape();

    QVariantList out;
    for (const Sample *s : visible()) {
        const Grade g = grade(s->value, m_draft, p, sh);
        // NotMeasured has TWO causes here and they are not the same answer. A swing the corridor
        // simply did not reach is one thing; a reading the norm does not believe is another, and
        // rendering both as "Not measured" makes a mis-tracked ball look like an absence. This is
        // the first LIVE surface for the distinction — the engine that carries the flag is still
        // dormant, but this list grades real swings today.
        const bool implausible = m_draft.isImplausible(s->value);

        QVariantMap r;
        r.insert(QStringLiteral("swingDir"),   s->swingDir);
        r.insert(QStringLiteral("session"),    s->session);
        r.insert(QStringLiteral("club"),       s->club);
        r.insert(QStringLiteral("ordinal"),    s->ordinal);
        r.insert(QStringLiteral("label"),      tr("Swing %1").arg(s->ordinal));
        r.insert(QStringLiteral("value"),      s->value);
        r.insert(QStringLiteral("included"),   s->included);
        r.insert(QStringLiteral("grade"),      gradeName(g));
        r.insert(QStringLiteral("gradeLabel"), implausible ? implausibleLabel() : gradeLabel(g));
        r.insert(QStringLiteral("implausible"), implausible);
        r.insert(QStringLiteral("implausibleNote"),
                 implausible ? implausibleNote(s->value, unitOf()) : QString());
        out.append(r);
    }
    return out;
}

QVariantMap NormEditorModel::sampleSummary() const
{
    const std::vector<const Sample *> vis = visible();

    std::vector<double> inc;
    for (const Sample *s : vis)
        if (s->included) inc.push_back(s->value);
    std::sort(inc.begin(), inc.end());

    QVariantMap out;
    out.insert(QStringLiteral("scanned"),     m_scanned);
    out.insert(QStringLiteral("produced"),    int(vis.size()));
    out.insert(QStringLiteral("included"),    int(inc.size()));
    out.insert(QStringLiteral("unit"),        unitOf());
    out.insert(QStringLiteral("scanning"),    m_scanning);
    out.insert(QStringLiteral("everScanned"), m_everScanned);
    out.insert(QStringLiteral("hasLibrary"),  !m_libraryRoot.isEmpty());
    // A cap that bit is REPORTED. Silent truncation reads as "that is the whole library", which is
    // the one thing a sample summary must never imply.
    out.insert(QStringLiteral("truncated"),   m_truncated);
    out.insert(QStringLiteral("scanLimit"),   kMaxScan);

    if (!inc.empty()) {
        out.insert(QStringLiteral("min"),    inc.front());
        out.insert(QStringLiteral("max"),    inc.back());
        out.insert(QStringLiteral("median"), inc.size() % 2
                                                 ? inc[inc.size() / 2]
                                                 : 0.5 * (inc[inc.size() / 2 - 1] + inc[inc.size() / 2]));
    } else {
        out.insert(QStringLiteral("min"),    0.0);
        out.insert(QStringLiteral("max"),    0.0);
        out.insert(QStringLiteral("median"), 0.0);
    }

    // A drag is in progress: hand back the latched axis unchanged. Recomputing here is what fed
    // the runaway — see beginHandleDrag().
    if (m_axisLocked) {
        out.insert(QStringLiteral("axisLo"), m_lockedAxisLo);
        out.insert(QStringLiteral("axisHi"), m_lockedAxisHi);
        return out;
    }

    // The axis spans the corridor AND the data, so neither can slide off the edge: a corridor with
    // no swings near it is exactly the case worth seeing, and an axis fitted to the data alone
    // would hide it by cropping the band out of frame.
    double lo = m_draft.mu - policy().watchMaxZ * std::max(m_draft.sigmaLo, 1e-9);
    double hi = m_draft.mu + policy().watchMaxZ * std::max(m_draft.sigmaHi, 1e-9);
    if (!inc.empty()) {
        lo = std::min(lo, inc.front());
        hi = std::max(hi, inc.back());
    }
    const double pad = std::max((hi - lo) * 0.08, 1e-6);
    out.insert(QStringLiteral("axisLo"), lo - pad);
    out.insert(QStringLiteral("axisHi"), hi + pad);
    return out;
}

QVariantList NormEditorModel::histogram() const
{
    const QVariantMap sum   = sampleSummary();
    const double      axisLo = sum.value(QStringLiteral("axisLo")).toDouble();
    const double      axisHi = sum.value(QStringLiteral("axisHi")).toDouble();
    const GradePolicy p      = policy();
    const Shape       sh     = shape();

    QVariantList out;
    if (!(axisHi > axisLo))
        return out;

    std::vector<int> counts(kBins, 0);
    for (const Sample *s : visible()) {
        if (!s->included) continue;
        int b = int((s->value - axisLo) / (axisHi - axisLo) * kBins);
        b = std::clamp(b, 0, kBins - 1);
        ++counts[std::size_t(b)];
    }

    const double w = (axisHi - axisLo) / kBins;
    for (int i = 0; i < kBins; ++i) {
        const double lo = axisLo + i * w;
        const Grade  g  = grade(lo + 0.5 * w, m_draft, p, sh);

        QVariantMap r;
        r.insert(QStringLiteral("lo"),    lo);
        r.insert(QStringLiteral("hi"),    lo + w);
        r.insert(QStringLiteral("count"), counts[std::size_t(i)]);
        r.insert(QStringLiteral("grade"), gradeName(g));
        out.append(r);
    }
    return out;
}

QVariantMap NormEditorModel::gradeCounts() const
{
    const GradePolicy p = policy();
    const Shape       sh = shape();
    int ideal = 0, good = 0, watch = 0, action = 0, implausible = 0;

    for (const Sample *s : visible()) {
        if (!s->included) continue;
        // Counted SEPARATELY, and this is the reason the count exists at all: an implausible
        // reading grades NotMeasured, and NotMeasured falls through the switch below without
        // incrementing anything. A capped corridor would therefore quietly drop swings out of the
        // running line — "31 Ideal · 8 Watch · 3 Action" over 45 marked swings, with three
        // unaccounted for and nothing saying so. The line is the safety mechanism; a safety
        // mechanism that silently under-reports is worse than none.
        if (m_draft.isImplausible(s->value)) { ++implausible; continue; }
        switch (grade(s->value, m_draft, p, sh)) {
        case Grade::Ideal:  ++ideal;  break;
        case Grade::Good:   ++good;   break;
        case Grade::Watch:  ++watch;  break;
        case Grade::Action: ++action; break;
        case Grade::NotMeasured:      break;
        }
    }

    QVariantMap out;
    out.insert(QStringLiteral("ideal"),  ideal);
    out.insert(QStringLiteral("good"),   good);
    out.insert(QStringLiteral("watch"),  watch);
    out.insert(QStringLiteral("action"),      action);
    out.insert(QStringLiteral("implausible"), implausible);
    // `total` is every marked swing the line accounts for, implausible ones included — it is what
    // "45 swings drawn" has to add up to, and leaving them out is exactly the silent shortfall
    // this count was added to prevent.
    out.insert(QStringLiteral("total"),  ideal + good + watch + action + implausible);
    return out;
}
