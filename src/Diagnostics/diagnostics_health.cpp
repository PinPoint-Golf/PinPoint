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

#include "diagnostics_health.h"

#include "drill_pack.h"
#include "screen_pack.h"

#include <QObject>
#include <QRegularExpression>
#include <QSet>

namespace pinpoint::analysis {

namespace {

ValidationIssue warn(const QString &code, const QString &subject, const QString &message)
{
    return { IssueSeverity::Warning, code, subject, message };
}

// There WAS a local `normKey()` here, spelling the health row's subject with no spaces around the
// '@' while norm_pack spelled the same key with them — and CharacteristicLibraryModel split whatever
// arrived on the first '@' to build the view's deep-link, so half the links carried a leading space
// into the context id. It now uses norm_pack's `normKeyLabel` / `splitNormKey`: a subject string
// that is both rendered to a user and parsed back by a view needs one pair of functions.

// Does this metric's own prose say the number depends on the club?
//
// Read from `howToRead` rather than a flag, because that is where the claim actually is and a flag
// would be a second place to state it — one that could disagree with the sentence a coach reads.
// Both shipped phrasings are covered ("the numbers are club-dependent", "the right number is club-
// and player-dependent").
bool saysClubDependent(const QString &howToRead)
{
    static const QRegularExpression re(
        QStringLiteral("club-\\s*(and\\s+player-\\s*)?dependent|depends\\s+on\\s+the\\s+club|per\\s+club"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(howToRead).hasMatch();
}

// True when every norm this measure carries sits at the default context or above it — i.e. nothing
// distinguishes one club from another.
bool onlyGeneralContexts(const NormPack &norms, const ContextTree &tree, const QString &measureId)
{
    const QStringList chain = tree.chain(kDefaultContextId());   // full_swing, any, …
    for (const QString &cid : norms.contextsFor(measureId))
        if (!chain.contains(cid))
            return false;
    return true;
}

bool sameNumber(double a, double b) { return qFuzzyCompare(a + 1.0, b + 1.0); }

bool sameOptional(const std::optional<double> &a, const std::optional<double> &b)
{
    if (a.has_value() != b.has_value()) return false;
    return !a.has_value() || sameNumber(*a, *b);
}

// The measure's shape, or Target when the pack no longer carries it. A norm row can outlive its
// measure — that is what `unknownNormMeasure` reports — and a health notice must still be able to
// name the corridor rather than crashing on the way to saying so.
Shape shapeOf(const CharacteristicPack &pack, const QString &measureId)
{
    const Measure *m = pack.measure(measureId);
    return m ? m->shape : Shape::Target;
}

// A plausibility pair as a verb phrase — "believes anything", "stops believing above 1.56",
// "believes between 0.8 and 1.56". Written out rather than rendered as "%1 to %2" because either
// bound may be absent and a missing one is not a zero: "0 to 1.56" on a row that caps only above
// states a floor at the origin that nobody authored.
QString believeClause(const std::optional<double> &lo, const std::optional<double> &hi)
{
    const auto f = [](double v) { return QString::number(v, 'g', 3); };
    if (lo.has_value() && hi.has_value())
        return QObject::tr("believes readings between %1 and %2").arg(f(*lo), f(*hi));
    if (hi.has_value())
        return QObject::tr("stops believing readings above %1").arg(f(*hi));
    if (lo.has_value())
        return QObject::tr("stops believing readings below %1").arg(f(*lo));
    return QObject::tr("believes any reading");
}

} // namespace

std::vector<ValidationIssue> diagnosticsHealth(const CharacteristicPack &pack,
                                               const INormProvider      &norms,
                                               const MetricCatalogue    &catalogue)
{
    std::vector<ValidationIssue> out;

    const NormPack    &set  = norms.norms();
    const ContextTree &tree = norms.contexts();

    // ── The referential norm checks that existed and nothing ran ────────────
    //
    // validateNormsAgainst() has owned unknownNormMeasure / unknownNormContext / normUnitMismatch /
    // normNotCapturable since stage 1, and until now its only caller was its own test: the providers
    // validate each pack STANDALONE, because an overlay legitimately references measures it does not
    // itself contain, and nobody had ever run the referential half over the assembled library. A
    // check that never runs is indistinguishable from a check that passes.
    for (const ValidationIssue &i : validateNormsAgainst(set, pack, tree).issues)
        out.push_back(i);

    // ── A corridor signal that cannot fire ──────────────────────────────────
    //
    // Scoped to LIVE measures. A measure with no producer is already reported by the roadmap, and
    // listing it here as "no norm" would be a second row about one gap, ranked as though authoring a
    // corridor would fix it — it would not; nothing can produce a value to grade.
    QSet<QString> measuresWithNorms;
    for (const Norm &n : set.norms)
        measuresWithNorms.insert(n.measureId);

    for (const Signal &s : pack.signalDefs) {
        if (s.test != SignalTest::OutsideCorridor) continue;
        for (const QString &mid : s.measures) {
            const Measure *m = pack.measure(mid);
            if (m == nullptr || m->status != MeasureStatus::Live) continue;
            if (measuresWithNorms.contains(mid)) continue;
            out.push_back(warn(QStringLiteral("signalNoNorm"), s.id,
                               QObject::tr("'%1' compares %2 against a corridor, and that measure "
                                           "has no norm in any context. The signal cannot fire — "
                                           "author a norm for it, or accept that whatever it "
                                           "detects is undetectable.")
                                   .arg(s.id, m->label.isEmpty() ? m->id : m->label)));
        }
    }

    // ── A signal watching a tail the norm never grades ──────────────────────
    //
    // Reported wherever it is authored, at any measure status, and NOT scoped to Live the way
    // signalNoNorm above is. The two mean opposite things: a signal with no norm is waiting for
    // work somebody could do, and this one is an author having misunderstood the measure. A floor
    // has no upper fault, so a High signal on it can never fire whatever gets built — dressing that
    // up as a producer backlog would put it in the wrong queue forever.
    //
    // The engine refuses it at runtime as well (characteristic_engine.cpp). Both, deliberately:
    // the runtime guard makes the answer right, and this makes the mistake visible.
    //
    // THRESHOLD, not ratio, is the second test in this set. A threshold signal reads the same
    // norm-joined reading a corridor signal does and the engine refuses its open tail identically,
    // so the mistake is the same mistake and belongs in the same list. A RATIO's tails are not a
    // measure's at all: the engine grades its quotient against an authored number, and the shape of
    // the measure above the line says which tail of THAT quantity faults, not which tail of the
    // quotient does — a small enough denominator sends the quotient high off a floor's good side.
    // Reporting a ratio here would accuse an author of a mistake they had not made.
    for (const Signal &s : pack.signalDefs) {
        if (s.test != SignalTest::OutsideCorridor && s.test != SignalTest::Threshold) continue;
        const Measure *m = pack.measure(s.measures.value(0));
        if (m == nullptr || !shapeIsOneSided(m->shape)) continue;

        const Direction d      = s.direction.value_or(Direction::High);
        const bool      atOpen = (m->shape == Shape::Floor) ? (d == Direction::High)
                                                            : (d == Direction::Low);
        if (!atOpen) continue;

        out.push_back(warn(QStringLiteral("signalOnOpenTail"), s.id,
                           QObject::tr("'%1' watches the %2 tail of %3, and that measure is '%4' — "
                                       "so that tail never grades and the signal can never fire. "
                                       "Point it at the other tail, or the condition behind it is "
                                       "undetectable by design.")
                               .arg(s.id,
                                    d == Direction::High ? QObject::tr("high") : QObject::tr("low"),
                                    m->label.isEmpty() ? m->id : m->label,
                                    shapeLabel(m->shape))));
    }

    // ── A tail that grades with nothing behind it ───────────────────────────
    //
    // The exact mirror of signalOnOpenTail above: that one is a signal watching a tail that cannot
    // grade, this one is a tail that grades with no signal watching it. Here rather than in
    // validatePack() because only the assembled library knows the tail grades at all — a pack
    // carries no numbers, and a measure with no norm anywhere grades on NEITHER tail, so the same
    // authoring gap is a roadmap item there and a live defect here.
    //
    // Scoped to Target, because that is the shape that grades both tails: sigmaHi defaults to
    // sigmaLo (norm_pack.cpp), so a corridor authored once is symmetric whether or not its author
    // thought about the far side. An empty direction set is left to `unusedMeasure` — a measure
    // nothing watches at all is a different row about a different omission, and reporting both would
    // rank one gap twice.
    for (const Measure &m : pack.measures) {
        if (m.shape != Shape::Target) continue;
        if (!measuresWithNorms.contains(m.id)) continue;

        QSet<int> dirs;
        for (const Signal &s : pack.signalDefs) {
            if (s.test != SignalTest::OutsideCorridor || !s.direction.has_value()) continue;
            if (s.measures.contains(m.id)) dirs.insert(static_cast<int>(*s.direction));
        }
        if (dirs.size() != 1) continue;

        const Direction watched  = static_cast<Direction>(*dirs.cbegin());
        const Direction ungraded = (watched == Direction::High) ? Direction::Low : Direction::High;

        // Authored as deliberate, with a reason validatePack() insists on. Silences THIS tail only.
        if (m.unwatchedTail.has_value() && *m.unwatchedTail == ungraded) continue;

        out.push_back(warn(QStringLiteral("ungradedTail"), m.id,
                           QObject::tr("The %2 tail of '%1' grades but nothing explains it: a "
                                       "reading out there shows a colour with no fault behind it. "
                                       "Author the condition for that tail, set the measure's shape "
                                       "so the tail stops grading, or record why it is deliberately "
                                       "unwatched.")
                               .arg(m.label.isEmpty() ? m.id : m.label,
                                    ungraded == Direction::High ? QObject::tr("high")
                                                                : QObject::tr("low"))));
    }

    // ── A screen or drill reference pointing at nothing ─────────────────────
    //
    // Both joins are exact string matches into a separate registry, so a typo or a renamed row
    // fails SILENTLY: the condition still loads, still validates, still renders — and the panel
    // that was meant to tell a coach how to run the test shows nothing at all, with no error
    // anywhere to say why. That is the same shape as a corridor keyed on the wrong phase, which
    // cost two stages before it was found.
    {
        const ScreenSet &screens = sharedScreenSet();
        const DrillSet  &drills  = sharedDrillSet();

        for (const Condition &c : pack.conditions) {
            if (!c.screenRef.isEmpty() && !screens.screen(c.screenRef))
                out.push_back(warn(QStringLiteral("unknownScreenRef"), c.id,
                                   QObject::tr("'%1' is settled by the screen '%2', and no such "
                                               "screen is authored. The recommendation would name a "
                                               "test nobody can look up.")
                                       .arg(c.label.isEmpty() ? c.id : c.label, c.screenRef)));

            for (const QString &d : c.drills)
                if (!drills.drill(d))
                    out.push_back(warn(QStringLiteral("unknownDrillRef"), c.id,
                                       QObject::tr("'%1' names the drill '%2', which is not in the "
                                                   "drill set.")
                                           .arg(c.label.isEmpty() ? c.id : c.label, d)));
        }

        // The other direction is not an error and is not reported: a screen or drill nothing points
        // at yet is a library being written, not a library that is wrong.
        for (const ValidationIssue &i : validateScreenSet(screens).issues) out.push_back(i);
        for (const ValidationIssue &i : validateDrillSet(drills).issues)   out.push_back(i);
    }

    // ── A reference nothing cites and nothing explains ──────────────────────
    // The one place the other direction IS reported, and the reason is the flag: a bibliography can
    // say out loud that a record is here for its own sake, so a record that says neither is a record
    // nobody has accounted for. See referenceHealth() for the argument.
    for (const ValidationIssue &i : referenceHealth(pack, sharedReferenceSet())) out.push_back(i);

    // ── Your own corridors, seated on nothing ───────────────────────────────
    //
    // `isOverridden` is per KEY and tracked at merge time, which is what scopes this to the personal
    // layer without inventing a field. Comparing values would not do: a user row holding exactly the
    // shipped numbers is still the user's.
    for (const Norm &n : set.norms) {
        if (n.n > 0) continue;
        if (!norms.isOverridden(n.measureId, n.contextId, n.cohort)) continue;   // not shipped rows
        if (n.source == NormSource::Literature) continue;              // noProvenance covers that
        out.push_back(warn(QStringLiteral("personalNormNoSample"), normKeyLabel(n),
                           QObject::tr("Your corridor for %1 was typed rather than seated — it "
                                       "records no swings behind it. Fine as a starting figure; "
                                       "seat it from the library when you have a sample.")
                               .arg(n.measureId)));
    }

    // ── The measure's unit against the catalogue's ──────────────────────────
    //
    // The one join nothing else makes. `normUnitMismatch` compares the norm against the measure and
    // the loader refuses a mismatch there, so both ends of the CONTENT agree by construction — and
    // that is exactly why this was invisible: the producer is the third party, and it was never in
    // the conversation. Three live measures shipped declaring cm over producers emitting ×frame or
    // mm, and each failed silently in whichever direction its scale factor pointed.
    //
    // Scoped to Provided measures, because a Composed one has no metricKey and nothing to compare
    // against. NOT scoped to Live: a planned measure with the wrong unit is a corridor that will be
    // wrong the day its producer lands, and finding it then is finding it late.
    for (const Measure &m : pack.measures) {
        if (m.kind != MeasureKind::Provided || m.metricKey.isEmpty()) continue;
        if (m.reducer.kind == ReducerKind::Rate) continue;   // per-second by design, not a mismatch
        const MetricDescriptor *d = catalogue.descriptor(m.metricKey);
        if (d == nullptr || d->unit.isEmpty() || m.unit.isEmpty()) continue;
        if (d->unit == m.unit) continue;
        out.push_back(warn(QStringLiteral("measureUnitMismatch"), m.id,
                           QObject::tr("%1 is graded in '%2', and '%3' produces '%4'. Nothing "
                                       "converts between them, so the reading is compared against a "
                                       "corridor in a different scale — which grades confidently and "
                                       "wrongly rather than reporting nothing.")
                               .arg(m.label.isEmpty() ? m.id : m.label, m.unit, m.metricKey,
                                    d->unit)));
    }

    // ── Club-dependent by the metric's own account, with one corridor ───────
    for (const Measure &m : pack.measures) {
        if (m.metricKey.isEmpty()) continue;
        if (!measuresWithNorms.contains(m.id)) continue;    // no corridor at all is a different row
        const MetricDescriptor *d = catalogue.descriptor(m.metricKey);
        if (d == nullptr || !saysClubDependent(d->howToRead)) continue;
        if (!onlyGeneralContexts(set, tree, m.id)) continue;
        out.push_back(warn(QStringLiteral("clubDependentNoContext"), m.id,
                           QObject::tr("%1 says this number is club-dependent, but every corridor "
                                       "for it sits at full swing or above — a driver and a wedge "
                                       "are being graded against the same band. Add per-club rows.")
                               .arg(d->label.isEmpty() ? d->key : d->label)));
    }

    // ── Cohort coverage ─────────────────────────────────────────────────────
    //
    // Two nudges about the SHAPE of a norm set's cohort rows, both silent on every unqualified set —
    // which is all of them today. Neither is a load error: an incomplete cohort grid grades
    // correctly, it just grades some golfers against a broader row than the author probably meant.
    {
        // Grouped by (measure, context), because that is the node the probe order runs at. A vector
        // with a linear find rather than a map: norm sets are ~150 rows, and this keeps the health
        // list in pack order instead of in hash order.
        struct Group {
            QString                   measureId;
            QString                   contextId;
            std::vector<const Norm *> rows;
        };
        std::vector<Group> groups;
        for (const Norm &n : set.norms) {
            Group *g = nullptr;
            for (Group &e : groups)
                if (e.measureId == n.measureId && e.contextId == n.contextId) { g = &e; break; }
            if (g == nullptr) {
                groups.push_back(Group{ n.measureId, n.contextId, {} });
                g = &groups.back();
            }
            g->rows.push_back(&n);
        }

        const auto has = [](const Group &g, const Cohort &c) {
            for (const Norm *n : g.rows)
                if (n->cohort == c) return true;
            return false;
        };
        const auto cohortOf = [](std::optional<Sex> s, std::optional<AgeBand> a) {
            Cohort c;
            c.sex = s;
            c.age = a;
            return c;
        };

        for (const Group &g : groups) {
            // A sex-only row beside a band-only row, with the combination unauthored. The probe
            // order puts age ahead of sex at equal specificity, so a woman of 60 resolves the 55–64
            // row and the `female` row never applies to her at all — which is a defensible default
            // and is very unlikely to be what the author had in mind.
            QStringList sexesOnly;
            QStringList bandsOnly;
            bool        anyCombined = false;
            for (const Norm *n : g.rows) {
                if (n->cohort.sex.has_value() && n->cohort.age.has_value()) anyCombined = true;
                else if (n->cohort.sex.has_value()) sexesOnly << sexLabel(*n->cohort.sex);
                else if (n->cohort.age.has_value()) bandsOnly << ageBandLabel(*n->cohort.age);
            }
            if (!anyCombined && !sexesOnly.isEmpty() && !bandsOnly.isEmpty())
                out.push_back(warn(QStringLiteral("cohortGap"),
                                   normKeyLabel(g.measureId, g.contextId),
                                   QObject::tr("%1 has corridors for %2 and for %3, and none for a "
                                               "combination of the two. Age is probed ahead of sex, "
                                               "so a golfer who is both resolves the age row and the "
                                               "sex row never applies to them. Author the combined "
                                               "rows, or drop one axis.")
                                       .arg(g.measureId, sexesOnly.join(QStringLiteral(" and ")),
                                            bandsOnly.join(QStringLiteral(" and ")))));

            // An `adult` row under a complete set of its own sub-bands can never resolve. `adult` is
            // the PARENT band and is reached only when the exact band misses, so filling in all
            // three beneath it retires it — and a corridor sitting in the pack looking authoritative
            // while grading nobody is the same mistake as a monitor bound on an open tail. (No date
            // of birth resolves to the parent band; see cohortProbeOrder.)
            const std::optional<Sex> sexes[] = { std::nullopt, Sex::Male, Sex::Female };
            for (const std::optional<Sex> &s : sexes) {
                if (!has(g, cohortOf(s, AgeBand::Adult))) continue;
                if (!has(g, cohortOf(s, AgeBand::Adult18_54))
                    || !has(g, cohortOf(s, AgeBand::Adult55_64))
                    || !has(g, cohortOf(s, AgeBand::Adult65Plus)))
                    continue;
                out.push_back(warn(QStringLiteral("shadowedCohort"),
                                   normKeyLabel(g.measureId, g.contextId, cohortOf(s, AgeBand::Adult)),
                                   QObject::tr("The %1 corridor for %2 can never resolve: every age "
                                               "band beneath it is authored, and the exact band is "
                                               "always tried first. Remove it, or remove one of the "
                                               "bands it was meant to cover.")
                                       .arg(cohortLabel(cohortOf(s, AgeBand::Adult)), g.measureId)));
            }
        }
    }

    // ── Contexts nothing resolves for ───────────────────────────────────────
    //
    // TWO findings, and the difference between them matters more than either. Resolution walks UP the
    // chain, so a context with nothing of its own is still graded by its ancestors — that is the whole
    // point of the tree, and reporting it as broken would argue against the design. What IS broken is
    // a context with nothing anywhere on its chain: a shot there is graded by NOTHING. Not a wider
    // corridor, none — every reading NotMeasured, every corridor signal silent, the whole pack inert
    // for that kind of shot. Those two cannot share a message.
    for (const ContextNode &node : tree.nodes()) {
        bool beneath = false;                 // a row at or below this node
        for (const Norm &n : set.norms) {
            if (n.contextId == node.id || tree.isDescendantOf(n.contextId, node.id)) { beneath = true; break; }
        }
        if (beneath) continue;

        const QStringList chain = tree.chain(node.id);
        bool above = false;                   // a row anywhere up the chain
        for (const Norm &n : set.norms) {
            if (chain.contains(n.contextId)) { above = true; break; }
        }

        const QString label = node.label.isEmpty() ? node.id : node.label;
        if (above) {
            out.push_back(warn(QStringLiteral("emptyContext"), node.id,
                               QObject::tr("'%1' carries no corridors of its own, so choosing it "
                                           "grades exactly as its parent does. Harmless — but it is "
                                           "a control with no effect until something is authored "
                                           "under it.").arg(label)));
        } else {
            out.push_back(warn(QStringLiteral("ungradedContext"), node.id,
                               QObject::tr("Nothing resolves for '%1' anywhere up its chain, so a "
                                           "shot in this context is graded by NOTHING — every "
                                           "reading comes back not measured and no corridor signal "
                                           "can fire. Author corridors under it, or move it beneath "
                                           "a context that has them.").arg(label)));
        }
    }

    // ── An override made against numbers that have since been revised ───────
    //
    // This needs a BASE and cannot be faked without one: "your row differs from the shipped row" is
    // also just what an override IS, so it would fire on every edit forever. A user row records the
    // shipped values it was made against (`basedOn`), and this fires only when the shipped row has
    // moved away from that base. Rows saved before `basedOn` existed carry none and are silent —
    // which is correct, because for those we genuinely do not know.
    for (const Norm &mine : set.norms) {
        if (!mine.basedOn.has_value()) continue;
        if (!norms.isOverridden(mine.measureId, mine.contextId, mine.cohort)) continue;
        const Norm *theirs = norms.shippedNorm(mine.measureId, mine.contextId, mine.cohort);
        if (theirs == nullptr) continue;                 // core no longer carries a row here

        const NormBasis &base = *mine.basedOn;
        const bool corridorMoved = !sameNumber(base.mu, theirs->mu)
                                   || !sameNumber(base.sigmaLo, theirs->sigmaLo)
                                   || !sameNumber(base.sigmaHi, theirs->sigmaHi)
                                   || !sameOptional(base.monitorLo, theirs->monitorLo)
                                   || !sameOptional(base.monitorHi, theirs->monitorHi);
        // The plausibility pair is part of the base, and a change to it is worth reporting on its
        // own: gaining a cap turns readings from Action into NotMeasured, which is a large change
        // to stay silent about. But it is NOT a corridor revision, and it must not be described as
        // one — the corridor numbers can be identical either side of it, and a notice reading
        // "was 1.4 to 1.6, has since been revised to 1.4 to 1.6" is a notice nobody can act on.
        const bool capsMoved = !sameOptional(base.plausibleLo, theirs->plausibleLo)
                               || !sameOptional(base.plausibleHi, theirs->plausibleHi);
        if (!corridorMoved && !capsMoved) continue;

        out.push_back(warn(QStringLiteral("overrideCoreChanged"),
                           normKeyLabel(mine),
                           corridorMoved
                               ? QObject::tr("You overrode %1 when the shipped corridor was %2. It "
                                             "has since been revised to %3. Yours is still what "
                                             "grades — keep it, or take theirs.")
                                     .arg(mine.measureId,
                                          // Claims on both sides — this compares two assertions
                                          // about the population, and the grade policy is not part
                                          // of either. Shape-aware, so a floor reads "was at least
                                          // 1.4" rather than naming an upper bound core never set.
                                          rangePhrase(base.mu - base.sigmaLo, base.mu + base.sigmaHi,
                                                      base.mu, shapeOf(pack, mine.measureId)),
                                          rangePhrase(theirs->claimLo(), theirs->claimHi(),
                                                      theirs->mu, shapeOf(pack, mine.measureId)))
                               : QObject::tr("The shipped corridor for %1 is unchanged, but what it "
                                             "will BELIEVE is not: it now %2, where it %3 when you "
                                             "overrode it. Yours is still what grades — keep it, "
                                             "or take theirs.")
                                     .arg(mine.measureId,
                                          believeClause(theirs->plausibleLo, theirs->plausibleHi),
                                          believeClause(base.plausibleLo, base.plausibleHi))));
    }

    return out;
}

std::vector<ValidationIssue> referenceHealth(const CharacteristicPack &pack,
                                             const ReferenceSet       &refs)
{
    std::vector<ValidationIssue> out;

    // Every identifier the pack actually leans on. Collected once rather than searched per record:
    // the join is an exact string match either way, and the pack is the larger of the two.
    QSet<QString> cited;
    for (const Condition &c : pack.conditions)
        if (!c.provenance.citation.isEmpty()) cited.insert(c.provenance.citation);
    for (const Edge &e : pack.edges)
        if (!e.provenance.citation.isEmpty()) cited.insert(e.provenance.citation);

    for (const Reference &r : refs.references) {
        // The flag is the record saying why it is here. Nothing more is required of it — and nothing
        // less is accepted, which is the whole point: it must be a deliberate act, not a default.
        if (r.generalReading) continue;

        // Any of the three identifiers will do. Matching on the DOI alone would report every
        // PMID-only and ISBN-only record as an orphan — a check that fires on correct content
        // teaches people to ignore it.
        if ((!r.doi.isEmpty()  && cited.contains(r.doi))
            || (!r.pmid.isEmpty() && cited.contains(r.pmid))
            || (!r.isbn.isEmpty() && cited.contains(r.isbn)))
            continue;

        out.push_back(warn(QStringLiteral("referenceOrphan"), r.id,
                           QObject::tr("'%1' is cited by nothing and is not marked as general "
                                       "reading, so the bibliography does not say why it is here. "
                                       "Cite it, flag it, or remove it.")
                               .arg(r.title.isEmpty() ? r.id : r.title)));
    }

    return out;
}

std::vector<ValidationIssue> corpusShareHealth(const std::vector<CorpusGradeCounts> &counts)
{
    std::vector<ValidationIssue> out;

    for (const CorpusGradeCounts &c : counts) {
        const int total = c.total();
        if (total < kMinCorpusForShare) continue;        // too few readings to mean anything

        struct Band { int n; const char *word; };
        const Band bands[] = { { c.ideal, "Ideal" }, { c.good, "Good" },
                               { c.watch, "Watch" }, { c.action, "Action" } };

        for (const Band &b : bands) {
            const double share = double(b.n) / double(total);
            if (share < kOneBandShare) continue;

            // Which way it is wrong matters, because the two have opposite fixes: almost everything
            // outside means the corridor is in the wrong place or the wrong unit; almost everything
            // Ideal means it is so wide it can never say anything.
            const QString hint = (qstrcmp(b.word, "Ideal") == 0)
                ? QObject::tr("so wide that nothing in the library falls outside it — it cannot "
                              "report a deviation")
                : QObject::tr("grading almost the whole library into one band — check the corridor's "
                              "centre and its unit before trusting it");

            out.push_back({ IssueSeverity::Warning, QStringLiteral("oneBandCorpus"),
                            normKeyLabel(c.measureId, c.contextId),
                            QObject::tr("%1 grades %2 of %3 drawn swings as %4 — %5.")
                                .arg(c.measureId)
                                .arg(b.n)
                                .arg(total)
                                .arg(QString::fromLatin1(b.word))
                                .arg(hint) });
            break;                                       // one row per corridor, not four
        }
    }
    return out;
}

} // namespace pinpoint::analysis
