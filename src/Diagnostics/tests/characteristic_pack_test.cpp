// Standalone tests for the pack schema, loader and validator (src/Diagnostics/characteristic_pack.*).
//
// Every rejection path is exercised. A validator whose negative cases are untested is a validator
// that passes everything, and this one is the pack's only real defence — a characteristic library
// degrades by duplicate measures proliferating and by its causal graph quietly becoming wrong,
// neither of which is visible by reading the JSON.
//
//   cmake --build build/analyzer-tests --target characteristic_pack_test
//   ctest --test-dir build/analyzer-tests -R characteristic_pack --output-on-failure

#include "../characteristic_pack.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static bool hasCode(const ValidationReport &r, const char *code, IssueSeverity sev)
{
    for (const ValidationIssue &i : r.issues)
        if (i.code == QLatin1String(code) && i.severity == sev) return true;
    return false;
}
static bool hasError(const ValidationReport &r, const char *code)   { return hasCode(r, code, IssueSeverity::Error); }
static bool hasWarning(const ValidationReport &r, const char *code) { return hasCode(r, code, IssueSeverity::Warning); }

// ── Fixture: a minimal but WELL-FORMED pack ─────────────────────────────────
// Two tails of one axis, one latent screened cause explaining both, a citation so nothing trips the
// proposed-tier warning. Each negative test perturbs exactly one thing.
static CharacteristicPack goodPack()
{
    CharacteristicPack p;
    p.id            = QStringLiteral("test");
    p.version       = QStringLiteral("1.0.0");
    p.schemaVersion = kPackSchemaVersion;

    Measure m;
    m.id              = QStringLiteral("stanceWidth");
    m.kind            = MeasureKind::Composed;
    m.series          = Series{ AnatomyRole::LeadAnkle, Quantity::Distance, AnatomyRole::TrailAnkle };
    m.reducer.kind    = ReducerKind::At;
    m.reducer.anchor  = Phase::Address;
    p.measures.push_back(m);

    Signal wide;
    wide.id        = QStringLiteral("sigWide");
    wide.test      = SignalTest::OutsideCorridor;
    wide.measures  = { QStringLiteral("stanceWidth") };
    wide.direction = Direction::High;
    p.signalDefs.push_back(wide);

    Signal narrow;
    narrow.id        = QStringLiteral("sigNarrow");
    narrow.test      = SignalTest::OutsideCorridor;
    narrow.measures  = { QStringLiteral("stanceWidth") };
    narrow.direction = Direction::Low;
    p.signalDefs.push_back(narrow);

    auto mkCondition = [](const QString &id, const QString &axis, const QString &sig) {
        Condition c;
        c.id                 = id;
        c.label              = id;
        c.axis               = axis;
        c.group              = ConditionGroup::Setup;
        c.observability      = Observability::Observable;
        c.confirmedBy        = ConfirmedBy::Measured;
        c.state              = ConditionState::Active;
        c.provenance.citation = QStringLiteral("10.0000/example");
        c.provenance.tier     = ProvenanceTier::Supported;
        if (!sig.isEmpty()) c.detectedBy = { sig };
        c.consequence.byLocale.insert(QStringLiteral("en"), QStringLiteral("It costs distance."));
        return c;
    };

    p.conditions.push_back(mkCondition(QStringLiteral("stanceWide"), QStringLiteral("stanceWidth"),
                                       QStringLiteral("sigWide")));
    p.conditions.push_back(mkCondition(QStringLiteral("stanceNarrow"), QStringLiteral("stanceWidth"),
                                       QStringLiteral("sigNarrow")));

    Condition screened;
    screened.id                  = QStringLiteral("limitedHipIr");
    screened.label               = QStringLiteral("limited hip internal rotation");
    screened.group               = ConditionGroup::Setup;
    screened.observability       = Observability::Latent;
    screened.confirmedBy         = ConfirmedBy::Screened;
    screened.screenRef           = QStringLiteral("screen.hipInternalRotation");
    screened.state               = ConditionState::Active;
    screened.provenance.citation = QStringLiteral("10.0000/example");
    screened.provenance.tier     = ProvenanceTier::Supported;
    p.conditions.push_back(screened);

    for (const QString &to : { QStringLiteral("stanceWide"), QStringLiteral("stanceNarrow") }) {
        Edge e;
        e.from     = QStringLiteral("limitedHipIr");   // cause
        e.to       = to;                                // effect
        e.type     = EdgeType::Causes;
        e.strength = Strength::Moderate;
        p.edges.push_back(e);
    }
    return p;
}

int main()
{
    std::printf("characteristic_pack_test\n");

    // ── The fixture itself must be clean ────────────────────────────────────────
    {
        const ValidationReport r = validatePack(goodPack());
        if (r.errorCount() != 0)
            for (const ValidationIssue &i : r.withSeverity(IssueSeverity::Error))
                std::printf("        unexpected error: %s\n", qPrintable(i.message));
        check(r.ok(), "the well-formed fixture validates with no errors");
    }

    // ── Round-trip ──────────────────────────────────────────────────────────────
    {
        const CharacteristicPack orig = goodPack();
        const PackLoadResult     back = loadPack(savePack(orig), QStringLiteral("round-trip"));

        check(back.loaded, "a saved pack loads again");
        check(back.pack.measures.size() == orig.measures.size(), "measure count survives");
        check(back.pack.signalDefs.size() == orig.signalDefs.size(), "signal count survives");
        check(back.pack.conditions.size() == orig.conditions.size(), "condition count survives");
        check(back.pack.edges.size() == orig.edges.size(), "edge count survives");

        const Measure *m = back.pack.measure(QStringLiteral("stanceWidth"));
        check(m && m->series == orig.measures.front().series, "the series tuple survives");
        check(m && m->reducer.kind == ReducerKind::At && m->reducer.anchor == Phase::Address,
              "the reducer survives");

        // Edge orientation MUST survive: `from` is the cause. A round-trip that transposed them
        // would invert the entire graph, and no coverage count could detect it.
        check(!back.pack.edges.empty() && back.pack.edges.front().from == QStringLiteral("limitedHipIr"),
              "edge orientation survives the round-trip (from = cause)");

        const Condition *c = back.pack.condition(QStringLiteral("stanceWide"));
        check(c && c->consequence.text() == QStringLiteral("It costs distance."),
              "localised narrative survives");
        check(c && c->axis == QStringLiteral("stanceWidth"), "axis survives");
    }

    // ── Localised text: a bare string is accepted as English ────────────────────
    {
        const QByteArray json = R"({
          "id": "loc", "version": "1", "schemaVersion": 1,
          "conditions": [ { "id": "x", "label": "x", "observability": "latent",
                            "consequence": "plain string" } ]
        })";
        const PackLoadResult res = loadPack(json, QStringLiteral("loc"));
        const Condition     *c   = res.pack.condition(QStringLiteral("x"));
        check(c && c->consequence.text() == QStringLiteral("plain string"),
              "a bare narrative string is read as English");
    }

    // ── Referential integrity ───────────────────────────────────────────────────
    {
        CharacteristicPack p = goodPack();
        p.signalDefs.front().measures = { QStringLiteral("noSuchMeasure") };
        check(hasError(validatePack(p), "unknownMeasure"), "a signal on an unknown measure is rejected");
    }
    {
        CharacteristicPack p = goodPack();
        p.conditions.front().detectedBy = { QStringLiteral("noSuchSignal") };
        check(hasError(validatePack(p), "unknownSignal"), "a condition on an unknown signal is rejected");
    }
    {
        CharacteristicPack p = goodPack();
        p.edges.front().from = QStringLiteral("ghost");
        check(hasError(validatePack(p), "unknownCondition"), "an edge to an unknown condition is rejected");
    }

    // ── Id collisions ───────────────────────────────────────────────────────────
    {
        CharacteristicPack p = goodPack();
        p.conditions.push_back(p.conditions.front());
        check(hasError(validatePack(p), "duplicateId"), "a duplicate condition id is rejected");
    }
    {
        CharacteristicPack p = goodPack();
        p.measures.push_back(p.measures.front());
        check(hasError(validatePack(p), "duplicateId"), "a duplicate measure id is rejected");
    }

    // ── The graph must be a DAG ─────────────────────────────────────────────────
    {
        CharacteristicPack p = goodPack();
        Edge back;
        back.from = QStringLiteral("stanceWide");
        back.to   = QStringLiteral("limitedHipIr");
        back.type = EdgeType::Causes;
        p.edges.push_back(back);
        check(hasError(validatePack(p), "cycle"), "a causal cycle is rejected");
    }
    {
        CharacteristicPack p = goodPack();
        Edge self;
        self.from = QStringLiteral("stanceWide");
        self.to   = QStringLiteral("stanceWide");
        p.edges.push_back(self);
        check(hasError(validatePack(p), "selfEdge"), "a self edge is rejected");
    }

    // ── Corroborates must not shadow a causal claim ─────────────────────────────
    // A pair that both causes and corroborates would count twice in the confidence ranking.
    {
        CharacteristicPack p = goodPack();
        Edge corr;
        corr.from = QStringLiteral("limitedHipIr");
        corr.to   = QStringLiteral("stanceWide");
        corr.type = EdgeType::Corroborates;
        p.edges.push_back(corr);
        check(hasError(validatePack(p), "corroboratesCausal"),
              "Corroborates between causally-linked conditions is rejected");

        // The same edge between unrelated conditions is fine.
        CharacteristicPack q = goodPack();
        Edge ok;
        ok.from = QStringLiteral("stanceWide");
        ok.to   = QStringLiteral("stanceNarrow");
        ok.type = EdgeType::Corroborates;
        q.edges.push_back(ok);
        check(!hasError(validatePack(q), "corroboratesCausal"),
              "Corroborates between unrelated conditions is allowed");
    }

    // ── Signal shape ────────────────────────────────────────────────────────────
    {
        CharacteristicPack p = goodPack();
        p.signalDefs.front().direction.reset();
        check(hasError(validatePack(p), "signalDirection"),
              "a corridor signal with no direction is rejected (it cannot identify a tail)");
    }
    {
        CharacteristicPack p = goodPack();
        p.signalDefs.front().threshold = 12.0;
        check(hasError(validatePack(p), "signalThreshold"),
              "a corridor signal carrying an authored number is rejected");
    }
    {
        CharacteristicPack p = goodPack();
        p.signalDefs.front().test = SignalTest::Threshold;
        check(hasError(validatePack(p), "signalThreshold"), "a threshold test with no number is rejected");
    }
    {
        CharacteristicPack p = goodPack();
        p.signalDefs.front().test = SignalTest::Ratio;
        check(hasError(validatePack(p), "signalArity"), "a ratio test needs two measures");
    }

    // ── Axis pairing ────────────────────────────────────────────────────────────
    // Two tails must sit on the SAME series, or they are not tails of one corridor.
    {
        CharacteristicPack p = goodPack();
        Measure other        = p.measures.front();
        other.id             = QStringLiteral("otherMeasure");
        other.series         = Series{ AnatomyRole::Spine, Quantity::Angle, AnatomyRole::Ground };
        p.measures.push_back(other);
        p.signalDefs.back().measures = { QStringLiteral("otherMeasure") };
        check(hasError(validatePack(p), "axisMismatch"),
              "two tails on different series are rejected");
    }
    {
        CharacteristicPack p = goodPack();
        p.conditions.erase(p.conditions.begin() + 1);   // drop the narrow tail
        p.edges.erase(p.edges.begin() + 1);
        check(hasWarning(validatePack(p), "singleTailAxis"),
              "an axis with one authored tail warns (deliberate, or an oversight?)");
    }

    // ── Facet and reducer validity reach the pack level ─────────────────────────
    {
        CharacteristicPack p = goodPack();
        p.measures.front().series.what = AnatomyRole::LeadKnee;   // a point with an Angle below
        p.measures.front().series.quantity = Quantity::Angle;
        check(hasError(validatePack(p), "badFacets"), "an invalid series is rejected at pack level");
    }
    {
        CharacteristicPack p = goodPack();
        p.measures.front().reducer.anchor.reset();
        check(hasError(validatePack(p), "badReducer"), "a malformed reducer is rejected");
    }

    // ── Schema version ──────────────────────────────────────────────────────────
    {
        QJsonObject root = savePack(goodPack());
        root.insert(QStringLiteral("schemaVersion"), kPackSchemaVersion + 1);
        const PackLoadResult res = loadPack(root, QStringLiteral("future"));
        check(!res.loaded && hasError(res.report, "schemaVersion"),
              "a newer schema is refused, not partially read");
    }
    {
        const PackLoadResult res = loadPack(QByteArray("{ not json"), QStringLiteral("broken"));
        check(!res.loaded && hasError(res.report, "parse"), "unparseable JSON is reported, not thrown");
    }

    // ── Spinal roles are classified by the loader, not trusted to the author ────
    // They cannot come from the pose SKELETON (no keypoint between the shoulders and the hips), but
    // the back contour of a down-the-line silhouette shows them — so the loader corrects an
    // OVER-claim down to NoProducer and leaves a weaker authored status alone.
    {
        CharacteristicPack p = goodPack();
        p.measures.front().series = Series{ AnatomyRole::ThoracicSegment, Quantity::Angle, AnatomyRole::Ground };
        p.measures.front().status = MeasureStatus::Live;          // author over-claimed

        const PackLoadResult res = loadPack(savePack(p), QStringLiteral("gap"));
        const Measure       *m   = res.pack.measure(QStringLiteral("stanceWidth"));
        check(m && m->status == MeasureStatus::NoProducer,
              "an over-claimed spinal series is corrected to a roadmap item, not a capture gap");
        check(m && !m->gapReason.isEmpty(), "and it carries the reason for the UI");

        // A weaker authored status is respected: NotCapturable is a judgement the author may
        // legitimately make about a measure the loader knows nothing else about.
        CharacteristicPack q = goodPack();
        q.measures.front().series = Series{ AnatomyRole::LumbarSegment, Quantity::Angle, AnatomyRole::Ground };
        q.measures.front().status = MeasureStatus::NotCapturable;
        const PackLoadResult qres = loadPack(savePack(q), QStringLiteral("gap2"));
        const Measure       *qm   = qres.pack.measure(QStringLiteral("stanceWidth"));
        check(qm && qm->status == MeasureStatus::NotCapturable,
              "an author's weaker status is not overridden upward");
    }

    // ── ExternalDevice: roadmap work gated on hardware, not a capture gap ───────
    //
    // The three statuses answer three different questions and the UI renders each differently, so
    // the round-trip matters as much as the check: a status that saved as one thing and loaded as
    // another would silently reclassify a launch-monitor row as pipeline work.
    {
        CharacteristicPack p = goodPack();
        p.measures.front().status    = MeasureStatus::ExternalDevice;
        p.measures.front().gapReason = QStringLiteral("Requires launch monitor: spin is not "
                                                      "measurable over a short indoor flight.");
        const PackLoadResult res = loadPack(savePack(p), QStringLiteral("lm"));
        const Measure       *m   = res.pack.measure(QStringLiteral("stanceWidth"));
        check(m && m->status == MeasureStatus::ExternalDevice,
              "ExternalDevice survives a save/load round-trip");
        check(!hasWarning(validatePack(p), "externalDeviceNoReason"),
              "…and a named device passes the reason check");

        p.measures.front().gapReason.clear();
        check(hasWarning(validatePack(p), "externalDeviceNoReason"),
              "an external-device measure that does not say WHICH device warns");
    }

    // ── One coach term, one condition ───────────────────────────────────────────
    //
    // Not tidiness: a search for "flip" resolves to whichever condition came first in the file, so
    // a shared alias sends the reader to the wrong page with no sign anything went wrong.
    {
        CharacteristicPack p = goodPack();
        p.conditions.at(0).aliases = { QStringLiteral("wide base") };
        p.conditions.at(1).aliases = { QStringLiteral("Wide Base") };   // same term, different case
        check(hasWarning(validatePack(p), "duplicateAlias"),
              "two conditions claiming one term warns, case-insensitively");

        p.conditions.at(1).aliases = { QStringLiteral("narrow base") };
        check(!hasWarning(validatePack(p), "duplicateAlias"), "…and distinct terms do not");

        // A LABEL is a claim on a term too — an alias that shadows another condition's name is the
        // same defect wearing different clothes.
        p.conditions.at(1).aliases = { p.conditions.at(0).label };
        check(hasWarning(validatePack(p), "duplicateAlias"),
              "an alias that shadows another condition's label warns");
    }
    {
        CharacteristicPack p = goodPack();
        p.conditions.front().aliases = { QStringLiteral("wide base"), QStringLiteral("too wide") };
        const PackLoadResult res = loadPack(savePack(p), QStringLiteral("aliases"));
        const Condition     *c   = res.pack.condition(p.conditions.front().id);
        check(c && c->aliases.size() == 2 && c->aliases.contains(QStringLiteral("too wide")),
              "condition aliases survive a save/load round-trip");
    }

    // ── The new condition groups ────────────────────────────────────────────────
    // Impact, Finish and BallFlight were added with the outcome layer. A group whose token does not
    // round-trip silently lands every condition in it back in Setup, which is the default.
    {
        CharacteristicPack p = goodPack();
        p.conditions.front().group = ConditionGroup::BallFlight;
        const PackLoadResult res = loadPack(savePack(p), QStringLiteral("groups"));
        const Condition     *c   = res.pack.condition(p.conditions.front().id);
        check(c && c->group == ConditionGroup::BallFlight, "ballFlight round-trips as itself");

        ConditionGroup g{};
        check(conditionGroupFromName(QStringLiteral("impact"), g) && g == ConditionGroup::Impact,
              "impact parses");
        check(conditionGroupFromName(QStringLiteral("finish"), g) && g == ConditionGroup::Finish,
              "finish parses");
        check(allConditionGroups().size() == 9, "all nine groups are enumerated in one place");
    }

    // ── Edge provenance ─────────────────────────────────────────────────────────
    //
    // An edge is a CLAIM, and for a long while it was the only claim in the pack that could not say
    // how well founded it was — a bare citation string, no tier, so an uncited edge and a cited one
    // drew identically while `strength` ranked which cause a coach saw first.
    //
    // The two tiers that carry no citation are the point of the exercise. Proposed means nobody has
    // looked; NoSourceFound means somebody looked and the literature is silent; Practice means the
    // field agrees and has never measured it. Those call for three different actions, and the old
    // schema could express only the first.
    {
        CharacteristicPack p = goodPack();
        p.edges.front().provenance.tier        = ProvenanceTier::Supported;
        p.edges.front().provenance.citation    = QStringLiteral("10.1177/0363546514555698");
        p.edges.front().provenance.searchedOn  = QDate(2026, 7, 27);
        p.edges.front().provenance.searchTerms = QStringLiteral("hip internal rotation golf");

        const PackLoadResult back = loadPack(savePack(p), QStringLiteral("edge-prov"));
        check(back.loaded, "a pack with edge provenance loads");
        const Edge &e = back.pack.edges.front();
        check(e.provenance.tier == ProvenanceTier::Supported, "the tier round-trips");
        check(e.provenance.citation == QStringLiteral("10.1177/0363546514555698"),
              "the citation round-trips");
        check(e.provenance.searchedOn == QDate(2026, 7, 27), "the search date round-trips");
        check(e.provenance.searchTerms == QStringLiteral("hip internal rotation golf"),
              "the search terms round-trip, so a re-run starts from what was already tried");
    }
    {
        // A cited tier with no citation is a claim about the literature that names none of it.
        CharacteristicPack p = goodPack();
        p.edges.front().provenance.tier = ProvenanceTier::Indirect;
        const PackLoadResult back = loadPack(savePack(p), QStringLiteral("edge-tier"));
        check(hasWarning(back.report, "edgeTierNoCitation"),
              "an edge claiming a cited tier with no citation is demoted and warns");
        check(back.pack.edges.front().provenance.tier == ProvenanceTier::Proposed,
              "and it is demoted to proposed, not left claiming what it cannot show");
    }
    {
        // The demotion must NOT reach the two uncited tiers, or recording a null destroys it.
        for (const ProvenanceTier t : { ProvenanceTier::NoSourceFound, ProvenanceTier::Practice }) {
            CharacteristicPack p = goodPack();
            p.edges.front().provenance.tier       = t;
            p.edges.front().provenance.searchedOn = QDate(2026, 7, 27);
            const PackLoadResult back = loadPack(savePack(p), QStringLiteral("edge-null"));
            check(back.pack.edges.front().provenance.tier == t,
                  QByteArray(provenanceTierName(t).toLatin1())
                      .append(" survives the load — it is a finding, not a missing citation")
                      .constData());
        }
    }
    {
        // NoSourceFound is a claim about the outcome of a SEARCH, so it is meaningless undated:
        // it cannot be told from an unasked question, or re-opened when the field publishes.
        CharacteristicPack p = goodPack();
        p.edges.front().provenance.tier = ProvenanceTier::NoSourceFound;
        const PackLoadResult undated = loadPack(savePack(p), QStringLiteral("edge-undated"));
        check(hasWarning(undated.report, "searchNoDate"),
              "an undated noSourceFound warns and falls back to proposed");
        check(undated.pack.edges.front().provenance.tier == ProvenanceTier::Proposed,
              "an undated search outcome reverts to the open question it actually is");
    }
    {
        // Practice is NOT a claim about a search, and demanding a date of it was the bug that
        // made this model incoherent: it forced every edge authored from coaching knowledge to
        // record as `proposed`, which says nobody has any basis for it. They plainly had one --
        // it is where the whole library came from. The tier says what the EVIDENCE is; searchedOn
        // says whether anybody has LOOKED; those are independent.
        CharacteristicPack p = goodPack();
        p.edges.front().provenance.tier = ProvenanceTier::Practice;
        const PackLoadResult back = loadPack(savePack(p), QStringLiteral("edge-practice"));
        check(!hasWarning(back.report, "searchNoDate"),
              "an undated practice edge does not warn — it is orthodoxy, not a search result");
        check(back.pack.edges.front().provenance.tier == ProvenanceTier::Practice,
              "and it keeps the tier, so the claim's real basis is on the record");
        check(needsLiteratureSearch(back.pack.edges.front().provenance),
              "yet it stays in the work queue: attributed is not the same as checked");

        // Once searched and still unmatched, it keeps the tier and gains the date.
        p.edges.front().provenance.searchedOn = QDate(2026, 7, 27);
        const PackLoadResult dated = loadPack(savePack(p), QStringLiteral("edge-practice-dated"));
        check(!needsLiteratureSearch(dated.pack.edges.front().provenance),
              "a dated practice edge leaves the queue — somebody has now been to the literature");
    }
    {
        // Back-compat: the old schema's bare citation string on an edge. No shipped edge ever had
        // one, but a community pack written against it might, and silently dropping the field
        // would lose the only thing that schema could say.
        QJsonObject root = savePack(goodPack());
        QJsonArray  es   = root.value(QStringLiteral("edges")).toArray();
        QJsonObject e0   = es.at(0).toObject();
        e0.insert(QStringLiteral("citation"), QStringLiteral("10.0000/legacy"));
        es.replace(0, e0);
        root.insert(QStringLiteral("edges"), es);

        const PackLoadResult back = loadPack(root, QStringLiteral("legacy-edge"));
        const Edge          &e    = back.pack.edges.front();
        check(e.provenance.citation == QStringLiteral("10.0000/legacy"),
              "a legacy bare edge citation is preserved, not dropped");
        check(e.provenance.tier == ProvenanceTier::Supported,
              "and reads as Supported — a citation somebody wrote and did not grade");
    }

    // ── Health warnings (these ARE the health list) ─────────────────────────────
    //
    // Every check below is gated in BOTH directions. Half the value is the negative case: a check
    // that cannot stay silent is a check that reports the design rather than a defect, and a health
    // list carrying rows nobody can act on is one people stop reading. Four of these had no test at
    // all in either direction, which is how `observableNoSignal` came to accuse seven deliberately
    // signal-less conditions without anything noticing.
    {
        CharacteristicPack p = goodPack();
        p.conditions.front().detectedBy.clear();
        check(hasWarning(validatePack(p), "observableNoSignal"), "an undetectable Observable warns");

        // ...but only when it claimed to be MEASURED. Observability answers "can it be seen";
        // ConfirmedBy answers "how is it established". A thin shot is plainly visible and equally
        // plainly not measurable from our pixels — Observable + Asserted with no signal is the
        // truthful encoding, not an omission.
        p.conditions.front().confirmedBy = ConfirmedBy::Asserted;
        check(!hasWarning(validatePack(p), "observableNoSignal"),
              "an Observable + Asserted condition is signal-less by design and stays silent");

        p.conditions.front().confirmedBy = ConfirmedBy::Screened;
        p.conditions.front().screenRef   = QStringLiteral("screen.hipInternalRotation");
        check(!hasWarning(validatePack(p), "observableNoSignal"),
              "an Observable + Screened condition stays silent too");
    }
    {
        // inconsistentReach — the inverse of the pair above, and the reason that one is scoped:
        // a condition outside capture reach may not ALSO claim to be detected.
        check(!hasWarning(validatePack(goodPack()), "inconsistentReach"),
              "a Measured condition with a signal does not warn about reach");

        CharacteristicPack p = goodPack();
        p.conditions.front().confirmedBy = ConfirmedBy::Asserted;   // keeps its signal
        check(hasWarning(validatePack(p), "inconsistentReach"),
              "an Asserted condition that also claims a detecting signal warns");
    }
    {
        // needsRevalidation — a state, not a shape. It must survive the round-trip to be reported
        // at all, so the check and the enum spelling are gated together.
        check(!hasWarning(validatePack(goodPack()), "needsRevalidation"),
              "an Active condition is not flagged for revalidation");

        CharacteristicPack p = goodPack();
        p.conditions.front().state = ConditionState::NeedsRevalidation;
        check(hasWarning(validatePack(p), "needsRevalidation"),
              "a condition flagged for revalidation warns");
        const PackLoadResult back = loadPack(savePack(p), QStringLiteral("revalidation"));
        check(hasWarning(back.report, "needsRevalidation"),
              "and the flag survives a save/load, or the warning would vanish on reload");
    }
    {
        // duplicateMeasure — two Provided measures reading one metric with an IDENTICAL reduction.
        // They are one number described twice, and a norm on each can grade the same value two
        // different ways. Only the reduction distinguishes them, so the check keys on all of it.
        check(!hasWarning(validatePack(goodPack()), "duplicateMeasure"),
              "distinct measures do not warn about duplication");

        auto provided = [](const QString &id, Phase anchor) {
            Measure m;
            m.id             = id;
            m.kind           = MeasureKind::Provided;
            m.metricKey      = QStringLiteral("stanceWidth");
            m.reducer.kind   = ReducerKind::At;
            m.reducer.anchor = anchor;
            return m;
        };

        CharacteristicPack p = goodPack();
        p.measures.push_back(provided(QStringLiteral("widthA"), Phase::Address));
        p.measures.push_back(provided(QStringLiteral("widthB"), Phase::Impact));
        check(!hasWarning(validatePack(p), "duplicateMeasure"),
              "one metric read at two different phases is two measures, not a duplicate");

        p.measures.back().reducer.anchor = Phase::Address;          // now byte-identical
        check(hasWarning(validatePack(p), "duplicateMeasure"),
              "one metric read twice with the same reduction warns");
    }
    {
        // bothTailsOneCondition — one condition holding both tails of one corridor fires whichever
        // way the reading goes, so it cannot tell too much from too little. The fixture is built for
        // this: two signals, one measure, opposite directions, correctly split across two conditions.
        check(!hasWarning(validatePack(goodPack()), "bothTailsOneCondition"),
              "the two tails split across two conditions do not warn");

        CharacteristicPack p = goodPack();
        p.conditions.front().detectedBy = { QStringLiteral("sigWide"), QStringLiteral("sigNarrow") };
        check(hasWarning(validatePack(p), "bothTailsOneCondition"),
              "one condition holding both tails of one measure warns");
    }
    {
        CharacteristicPack p = goodPack();
        p.edges.clear();
        const ValidationReport r = validatePack(p);
        check(hasWarning(r, "noCause"), "a characteristic with no cause warns");
        check(hasWarning(r, "orphanCause"), "a latent cause that explains nothing warns");
    }
    {
        CharacteristicPack p = goodPack();
        p.measures.push_back(Measure{});
        p.measures.back().id             = QStringLiteral("unused");
        p.measures.back().series         = Series{ AnatomyRole::Spine, Quantity::Angle, AnatomyRole::Ground };
        p.measures.back().reducer.anchor = Phase::Address;
        check(hasWarning(validatePack(p), "unusedMeasure"), "a measure nothing uses warns");
    }
    {
        CharacteristicPack p = goodPack();
        p.conditions.front().provenance.citation.clear();
        p.conditions.front().provenance.tier = ProvenanceTier::Proposed;
        check(hasWarning(validatePack(p), "proposedTier"), "an uncited condition warns so the UI can badge it");
    }
    {
        // A tier claim with no citation is demoted at load, not believed.
        QJsonObject root = savePack(goodPack());
        QJsonArray  cs   = root.value(QStringLiteral("conditions")).toArray();
        QJsonObject c0   = cs.at(0).toObject();
        QJsonObject prov;
        prov.insert(QStringLiteral("tier"), QStringLiteral("established"));
        c0.insert(QStringLiteral("provenance"), prov);
        cs.replace(0, c0);
        root.insert(QStringLiteral("conditions"), cs);

        const PackLoadResult res = loadPack(root, QStringLiteral("tier"));
        const Condition     *c   = res.pack.condition(QStringLiteral("stanceWide"));
        check(c && c->provenance.tier == ProvenanceTier::Proposed,
              "a tier claim without a citation is demoted to proposed");
    }
    {
        // Every cause Asserted => offerable, never concludable. Legitimate, but deliberate.
        CharacteristicPack p = goodPack();
        p.conditions.back().confirmedBy = ConfirmedBy::Asserted;
        check(hasWarning(validatePack(p), "noResolvableCause"),
              "a characteristic explained only by Behavioural causes warns");
    }

    // ── Edge orientation, structurally ──────────────────────────────────────────
    // The seed tables read effect-first while Edge is cause-first, so every row flips on
    // transcription. Coverage counts are IDENTICAL under edge reversal and cannot catch it. A
    // physical screen result explains things; nothing in a swing explains it.
    {
        CharacteristicPack p = goodPack();
        for (Edge &e : p.edges) std::swap(e.from, e.to);   // invert the whole graph
        const ValidationReport r = validatePack(p);
        check(hasWarning(r, "screenedHasCause"), "an inverted graph is caught by the orientation check");

        // And confirm the trap is real: coverage totals are unchanged by the inversion, so the
        // concentration assertion alone would have passed on this wrong graph.
        const CharacteristicPack good = goodPack();
        int coverageGood = 0, coverageBad = 0;
        for (const Condition &c : good.conditions) coverageGood += coverageOf(good, c.id);
        for (const Condition &c : p.conditions)    coverageBad  += coverageOf(p, c.id);
        check(coverageGood == coverageBad,
              "total coverage is identical under inversion — which is why the structural check exists");
    }

    // ── Graph helpers ───────────────────────────────────────────────────────────
    {
        const CharacteristicPack p = goodPack();
        check(coverageOf(p, QStringLiteral("limitedHipIr")) == 2, "coverage counts what a cause explains");
        check(causesOf(p, QStringLiteral("stanceWide")) == QStringList{ QStringLiteral("limitedHipIr") },
              "causesOf walks upstream");
        check(effectsOf(p, QStringLiteral("limitedHipIr")).size() == 2, "effectsOf walks downstream");
        check(hasCausalPath(p, QStringLiteral("limitedHipIr"), QStringLiteral("stanceWide")),
              "a direct causal path is found");
        check(!hasCausalPath(p, QStringLiteral("stanceWide"), QStringLiteral("stanceNarrow")),
              "unrelated conditions have no causal path");
        check(tailsOfAxis(p, QStringLiteral("stanceWidth")).size() == 2, "both tails of an axis are found");
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
