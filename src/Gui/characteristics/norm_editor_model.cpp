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

bool NormEditorModel::begin(const QString &measureId, const QString &contextId)
{
    const Measure *m = m_pack->pack().measure(measureId);
    if (!m || contextId.isEmpty() || !m_norms->contexts().node(contextId))
        return false;

    m_open       = true;
    m_measureId  = measureId;
    m_contextId  = contextId;
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

    // Seed from what resolves TODAY — an own row if there is one, otherwise the inherited corridor,
    // so "override for this context" starts from the thing being overridden rather than from zero.
    const NormResolution res = m_norms->resolve(measureId, contextId);
    m_hadOwnRow = res.found() && !res.inherited;

    m_draft = Norm{};
    m_draft.measureId = measureId;
    m_draft.contextId = contextId;
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
    if (hi < lo) std::swap(lo, hi);

    // A zero-width band admits only its own centre — norm.h calls that degenerate but well-defined,
    // and the validator warns about it separately. It is not this control's job to refuse it, but
    // it IS this control's job not to produce a negative tolerance from a crossed drag.
    m_draft.mu      = 0.5 * (lo + hi);
    m_draft.sigmaLo = m_draft.mu - lo;
    m_draft.sigmaHi = hi - m_draft.mu;

    // Hand-editing makes this an authored figure again. Leaving `Seated · n = 42` attached to
    // numbers a hand has since moved would be a provenance claim the norm no longer supports.
    if (m_draft.source == NormSource::Seated || m_draft.source == NormSource::Imported) {
        m_draft.source = NormSource::Heuristic;
        m_draft.n      = 0;
    }
    m_dirty = true;
    emit draftChanged();
    emit sampleChanged();
}

void NormEditorModel::nudgeClaimLo(double to) { setClaimBand(to, m_draft.claimHi()); }
void NormEditorModel::nudgeClaimHi(double to) { setClaimBand(m_draft.claimLo(), to); }

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
        const NormBandEdges ce = bandEdgesOf(*n, policy());
        r.insert(QStringLiteral("idealLo"),      ce.idealLo);
        r.insert(QStringLiteral("idealHi"),      ce.idealHi);
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
    if (const Norm *shipped = m_norms ? m_norms->shippedNorm(m_measureId, m_contextId) : nullptr) {
        NormBasis basis;
        basis.mu        = shipped->mu;
        basis.sigmaLo   = shipped->sigmaLo;
        basis.sigmaHi   = shipped->sigmaHi;
        basis.monitorLo = shipped->monitorLo;
        basis.monitorHi = shipped->monitorHi;
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
    const bool hadShipped = m_norms->shippedNorm(m_measureId, m_contextId) != nullptr;

    NormPack pack = readUserPack();
    if (!pack.remove(m_measureId, m_contextId)) {
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

    out.insert(QStringLiteral("measureId"),    m_measureId);
    out.insert(QStringLiteral("measureLabel"), m ? (m->label.isEmpty() ? m->id : m->label) : m_measureId);
    out.insert(QStringLiteral("unit"),         unitOf());
    out.insert(QStringLiteral("highMeans"),    m ? m->highMeans : QString());
    out.insert(QStringLiteral("contextId"),    m_contextId);
    out.insert(QStringLiteral("contextLabel"), contextLabel(m_contextId));
    out.insert(QStringLiteral("route"),        m_route);

    // The Watch edge through bandEdgesOf(), so the editor draws the edge that GRADES. It matters for
    // a migrated row: the draft is a copy of the resolved norm, monitor bounds and all, and those
    // bounds dominate the z-derived edge inside grade(). Deriving the number here from sigma alone
    // showed the wrong edge on all 56 migrated corridors — a corridor the app does not use, in the
    // one screen whose entire job is to show what a corridor does.
    const NormBandEdges e = bandEdgesOf(m_draft, p);

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
    out.insert(QStringLiteral("goodLo"),  m_draft.mu - p.goodMaxZ * m_draft.sigmaLo);
    out.insert(QStringLiteral("goodHi"),  m_draft.mu + p.goodMaxZ * m_draft.sigmaHi);
    out.insert(QStringLiteral("watchLo"), e.watchLo);
    out.insert(QStringLiteral("watchHi"), e.watchHi);
    out.insert(QStringLiteral("explicitMonitor"), m_draft.hasExplicitMonitor());

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
        parentId.isEmpty() ? NormResolution{} : m_norms->resolve(m_measureId, parentId);
    out.insert(QStringLiteral("inherited"),     !m_hadOwnRow);
    out.insert(QStringLiteral("inheritedFrom"), par.found() ? contextLabel(par.contextId) : QString());
    // A DIFF against the parent row, so both sides are CLAIMS. "You are 37 wider than the full
    // swing" compares two assertions; running it through the grade policy would scale both by the
    // same factor and change the sentence for no reason the reader could act on.
    out.insert(QStringLiteral("parentClaimLo"), par.found() ? par.norm->claimLo() : 0.0);
    out.insert(QStringLiteral("parentClaimHi"), par.found() ? par.norm->claimHi() : 0.0);
    out.insert(QStringLiteral("hasParent"),     par.found());

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
    const bool  overridden = m_norms->isOverridden(m_measureId, m_contextId);
    const Norm *shipped    = m_norms->shippedNorm(m_measureId, m_contextId);

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
        editedNote = tr("You changed this. PinPoint ships %1 to %2 %3.")
                         .arg(QString::number(shipped->claimLo(), 'f', 1),
                              QString::number(shipped->claimHi(), 'f', 1), unitOf());
    else if (overridden)
        editedNote = tr("You added this. PinPoint ships no corridor for %1 — without yours it "
                        "would inherit.").arg(contextLabel(m_contextId));
    out.insert(QStringLiteral("editedNote"), editedNote);
    return out;
}

QVariantList NormEditorModel::samples() const
{
    const GradePolicy p = policy();

    QVariantList out;
    for (const Sample *s : visible()) {
        const Grade g = grade(s->value, m_draft, p);

        QVariantMap r;
        r.insert(QStringLiteral("swingDir"),   s->swingDir);
        r.insert(QStringLiteral("session"),    s->session);
        r.insert(QStringLiteral("club"),       s->club);
        r.insert(QStringLiteral("ordinal"),    s->ordinal);
        r.insert(QStringLiteral("label"),      tr("Swing %1").arg(s->ordinal));
        r.insert(QStringLiteral("value"),      s->value);
        r.insert(QStringLiteral("included"),   s->included);
        r.insert(QStringLiteral("grade"),      gradeName(g));
        r.insert(QStringLiteral("gradeLabel"), gradeLabel(g));
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
        const Grade  g  = grade(lo + 0.5 * w, m_draft, p);

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
    int ideal = 0, good = 0, watch = 0, action = 0;

    for (const Sample *s : visible()) {
        if (!s->included) continue;
        switch (grade(s->value, m_draft, p)) {
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
    out.insert(QStringLiteral("action"), action);
    out.insert(QStringLiteral("total"),  ideal + good + watch + action);
    return out;
}
