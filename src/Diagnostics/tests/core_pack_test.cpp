// Acceptance tests for the SHIPPED seed pack (src/Resources/diagnostics/core.json).
//
// The pack is not done when its characteristics load — it is done when the causal graph resolves in
// the right direction, the dominant causes concentrate, and no brand name has leaked into the
// content. Each of those is a way the pack can be wrong while looking entirely correct.
//
//   cmake --build build/analyzer-tests --target core_pack_test
//   ctest --test-dir build/analyzer-tests -R core_pack --output-on-failure

#include "../characteristic_pack.h"
#include "../context_tree.h"
#include "../norm_pack.h"
#include "../pack_provider.h"
#include "../reference_pack.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <map>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

int main()
{
    std::printf("core_pack_test\n");

    QFile f(QStringLiteral(PP_CORE_PACK_PATH));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("  [FAIL] cannot open %s\n", PP_CORE_PACK_PATH);
        return 1;
    }
    const QByteArray   raw  = f.readAll();
    const PackLoadResult res = loadPack(raw, QStringLiteral("core.json"));
    const CharacteristicPack &p = res.pack;

    // ── It loads and validates ──────────────────────────────────────────────────
    {
        if (!res.loaded)
            for (const ValidationIssue &i : res.report.withSeverity(IssueSeverity::Error))
                std::printf("        error: %s\n", qPrintable(i.message));
        check(res.loaded, "the shipped pack loads and validates with no errors");
        check(p.id == QStringLiteral("core"), "pack id is 'core'");
        check(p.schemaVersion == kPackSchemaVersion, "pack declares the current schema version");
    }

    // ── Content census ──────────────────────────────────────────────────────────
    int observable = 0, screened = 0, asserted = 0;
    for (const Condition &c : p.conditions) {
        if (c.observability == Observability::Observable) ++observable;
        if (c.confirmedBy == ConfirmedBy::Screened) ++screened;
        if (c.confirmedBy == ConfirmedBy::Asserted) ++asserted;
    }
    {
        check(observable >= 25, "at least 25 observable characteristics ship on day one");
        check(screened >= 10, "the screened cause library is seeded");
        check(asserted >= 5, "behavioural causes are seeded");
        check(p.edges.size() >= 75, "the causal graph is seeded, not a stub");
        std::printf("        (%d observable, %d screened, %d behavioural, %d edges, %d measures)\n",
                    observable, screened, asserted, int(p.edges.size()), int(p.measures.size()));
    }

    // ── The kind census ─────────────────────────────────────────────────────────
    //
    // Thresholds, not exact counts: which kind a borderline condition lands in is an editorial call
    // that is meant to move, and pinning 69 would make every reclassification a test edit. What must
    // hold is the SHAPE — that the library is mostly about swing faults, and that the five kinds
    // whose absence would mean the axis had not really been authored are all populated.
    {
        std::map<ConditionKind, int> byKind;
        for (const Condition &c : p.conditions) ++byKind[c.kind];

        check(byKind[ConditionKind::Fault] >= 50, "the library is mostly swing faults, as it should be");
        check(byKind[ConditionKind::Delivery] >= 8, "the delivery layer is populated");
        check(byKind[ConditionKind::Outcome] >= 15, "the outcome layer is populated");
        check(byKind[ConditionKind::Capacity] >= 10, "the physical-capacity layer is populated");
        check(byKind[ConditionKind::Intent] >= 5, "the intent layer is populated");
        check(byKind[ConditionKind::Setup] >= 15, "the setup layer is populated");

        // The four kinds that the group axis CANNOT recover, which is the whole argument for the
        // field existing. All four used to sit in `setup` because there was nowhere else to put them.
        int miscastAsSetupGroup = 0;
        for (const Condition &c : p.conditions)
            if (c.group == ConditionGroup::Setup
                && (c.kind == ConditionKind::Capacity || c.kind == ConditionKind::Intent
                    || c.kind == ConditionKind::Equipment))
                ++miscastAsSetupGroup;
        check(miscastAsSetupGroup >= 20,
              "the `setup` group still holds capacities, intents and equipment — which is why the "
              "kind axis is not recoverable from it");

        for (const auto &kv : byKind)
            std::printf("        %-10s %d\n", qPrintable(conditionKindName(kv.first)), kv.second);
    }

    // ── Outcome and BallFlight are coextensive, in BOTH directions ──────────────
    //
    // `outcomeReachOf()` asks the KIND. It used to ask the group, and the move was landed only
    // because the two sets are provably the same set over shipped content — so this assertion is
    // what makes that a no-op rather than a hope. The second half is the one that matters: a new
    // ball-flight condition authored without a kind would default to Fault and quietly stop being
    // counted as an outcome by every "how many bad shots does this explain" number in the panel.
    //
    // The decision this pins, so it is not rediscovered as an accident: `strike_toe`/`strike_heel`
    // are Outcome, not Delivery, though strike location IS one of the ball-flight determinants. A
    // golfer sees the mark on the face; that makes it something the ball did. Move them to Delivery
    // and this assertion goes red for a correct reason — then the one-way form is what to keep, and
    // `swingEdges` below needs its own sentence.
    {
        QStringList outcomeNotBallFlight, ballFlightNotOutcome;
        for (const Condition &c : p.conditions) {
            if (c.kind == ConditionKind::Outcome && c.group != ConditionGroup::BallFlight)
                outcomeNotBallFlight << c.id;
            if (c.group == ConditionGroup::BallFlight && c.kind != ConditionKind::Outcome)
                ballFlightNotOutcome << c.id;
        }
        for (const QString &id : outcomeNotBallFlight)
            std::printf("        Outcome outside ballFlight: %s\n", qPrintable(id));
        for (const QString &id : ballFlightNotOutcome)
            std::printf("        ballFlight not an Outcome:  %s\n", qPrintable(id));
        check(outcomeNotBallFlight.isEmpty(), "every Outcome sits in the ball-flight group");
        check(ballFlightNotOutcome.isEmpty(), "and every ball-flight condition is an Outcome");
    }

    // ── Every shipped condition AUTHORS both new fields ─────────────────────────
    //
    // Read off the RAW JSON, and it has to be: Prominence has five legitimate values and no
    // sentinel, so by the time the loader is done a row nobody authored is byte-identical to a row
    // somebody authored at the default rung — and 58 conditions sit at that rung on purpose. The
    // validator therefore cannot ask this question at all (see the note in characteristic_pack.h).
    // This is the only layer where it is answerable, and the failure it catches is the real one: a
    // 146-row hand edit that stopped at 142.
    {
        const QJsonArray conds = QJsonDocument::fromJson(raw).object()
                                     .value(QStringLiteral("conditions")).toArray();
        QStringList noKind, noProminence;
        for (const QJsonValue &v : conds) {
            const QJsonObject o = v.toObject();
            const QString     id = o.value(QStringLiteral("id")).toString();
            if (!o.contains(QStringLiteral("kind")))       noKind << id;
            if (!o.contains(QStringLiteral("prominence"))) noProminence << id;
        }
        for (const QString &id : noKind)       std::printf("        no kind: %s\n", qPrintable(id));
        for (const QString &id : noProminence) std::printf("        no prominence: %s\n", qPrintable(id));
        check(int(conds.size()) == int(p.conditions.size()), "every condition in the file loaded");
        check(noKind.isEmpty(), "every shipped condition declares its kind");
        check(noProminence.isEmpty(), "and every one declares a prominence");
    }

    // ── Cause concentration ─────────────────────────────────────────────────────
    // The whole point of the model: a handful of latent causes explain most of the pack, and none of
    // them needs any capture hardware. If every characteristic had its own private cause, this would
    // be a restated fault list rather than a diagnosis.
    {
        std::map<int, QString, std::greater<int>> byCoverage;
        for (const Condition &c : p.conditions) {
            const int cov = coverageOf(p, c.id);
            if (cov > 0) byCoverage.insert({ cov, c.id });
        }

        auto coverage = [&](const char *id) { return coverageOf(p, QString::fromLatin1(id)); };
        // 13 -> 15 with reverse_pivot. Two edges, not one: the fault itself, and the pelvis drifting
        // to the lead side going back, which this capacity plainly explains and had never been wired
        // to. The second was the older gap — the same restriction already reached SWAY, the opposite
        // tail of the very same pelvis-sway measure, so the graph could account for one direction of
        // a two-tailed failure and not the other.
        check(coverage("poor_pelvic_disassociation") == 15, "poor pelvic disassociation explains 15");
        // 15 and 9, up from 13 and 8. The face-on producer batch made six authored conditions
        // gradeable for the first time, and the "everything that can fire has a cause" check below
        // caught that they had nothing behind them. Two of the six route to a thoracic-rotation
        // restriction (a chest that cannot turn is why the arms run away and why the finish stops
        // short) and one to the lead hip (a hip that cannot internally rotate spins the pelvis out
        // rather than turning over a stable lead leg).
        //
        // 15 -> 16 with reverse_pivot: a chest that cannot turn away from the target has to get to
        // the top some other way, and leaning toward the target is the cheapest substitute. Moderate
        // rather than Strong — the restriction makes the substitution available, it does not compel
        // it, and a golfer with the same restriction can shorten the backswing instead.
        check(coverage("limited_thoracic_rotation") == 16, "limited thoracic rotation explains 16");
        // 9 -> 10 with the lead-side audit: the hip that cannot internally rotate cannot accept the
        // load and turn over it, so the rotation stops rather than being slow to start — which is
        // late_pelvis_rotation, a fault it already explained and a different one.
        check(coverage("limited_lead_hip_ir") == 10, "limited lead-hip internal rotation explains 10");
        // 10 -> 11 when the trail hip was given its route to over-the-top: a pelvis that cannot
        // finish turning away leaves the downswing no room to rotate into, and the arms take it
        // over the top. Updated deliberately, per the note below on the pelvis-sway count — these
        // are the numbers somebody chose, and a `>=` here would stop the test noticing a change.
        //
        // 11 -> 13 with the lower-body producer: the trail hip gained the lead knee working in at
        // the top and the pelvis rising in the backswing. Both are OBSERVATIONS the restriction can
        // produce and neither is evidence of it on its own — see the design doc on why the face-on
        // signature is ambiguous — which is why both edges are Moderate and why the explanation
        // pass, not the detector, is what names the cause.
        //
        // 13 -> 14 and 10 -> 11 with the early-extension causes audit. Both restrictions are named
        // in the coaching account of early extension and neither had a route to it: a trail hip
        // that cannot internally rotate leaves the downswing no room to turn into, and a pelvis
        // nothing is holding in its hinge comes out of the hinge instead of rotating within it.
        // The core case was the conspicuous one — it already carried an edge to BACKING OFF THE
        // BALL, which is the opposite tail of the same pelvis-thrust axis, so the graph could
        // explain the rarer direction of that fault and not the common one.
        //
        // 14 -> 15 with reverse_pivot, and this is the Strong one. The trail hip is the joint the
        // backswing loads INTO; if it cannot internally rotate there is nowhere for the load to go,
        // and the mass stays on the lead side because it was never invited across.
        check(coverage("limited_trail_hip_ir") == 15, "limited trail-hip internal rotation explains 15");
        check(coverage("poor_core_stability") == 11, "poor core stability explains 11");

        const int topFive = coverage("poor_pelvic_disassociation") + coverage("limited_thoracic_rotation")
                          + coverage("limited_lead_hip_ir") + coverage("limited_trail_hip_ir")
                          + coverage("poor_core_stability");

        // Measured against the SWING-fault edges, not every edge in the pack.
        //
        // The claim being made is that a handful of physical capacities explain most of what goes
        // wrong IN THE SWING — that this is a diagnosis rather than a restated fault list. The
        // ball-flight layer added a second tier of edges (fault → outcome) that no screen result
        // reaches directly and never will: a tight trail hip does not cause a slice, it causes the
        // delivery that causes the slice. Counting those in the denominator would make the same
        // library look less concentrated purely for having become able to explain the shot the
        // golfer actually saw.
        // Asks the KIND, like outcomeReachOf() does, and for the same reason: "is this an outcome"
        // is a question about what sort of thing a condition is. The number is unchanged, because
        // the two sets are asserted coextensive above.
        int swingEdges = 0;
        for (const Edge &e : p.edges) {
            const Condition *to = p.condition(e.to);
            if (e.type == EdgeType::Causes && to && to->kind != ConditionKind::Outcome)
                ++swingEdges;
        }

        // The fraction was 3/10 and is now 1/5. It is a CALIBRATION, not the claim — the claim is
        // that a handful of physical capacities explain a large share of the swing, and the number
        // is one crude way of measuring it.
        //
        // What moved it: the unwatched-tails pass added a second causal LAYER. Its 23 nodes are
        // delivery faults caused mostly by other swing faults — excessive shaft lean comes from
        // excessive lag, not from a tight hip — so they land in the denominator while adding almost
        // nothing to the numerator. That is the same dilution the ballFlight scoping above already
        // corrected for once, and it says nothing about concentration: a library that grows DEPTH
        // looks less concentrated to a depth-1 out-degree ratio while being no less explained.
        //
        // The screened-cause edges that were genuinely true were authored rather than the threshold
        // simply dropped — that is what took the ratio from 23 % to the 25 % that ships. Reaching
        // for the old 30 % from there would have meant inventing links to satisfy a number, which
        // is the laundering this check exists to prevent. So the gate sits below what ships, with
        // room for the next content pass, and the honest reading of a failure here is unchanged:
        // new nodes were given private causes instead of being wired to the existing screen
        // library. Fix the edges.
        check(topFive >= swingEdges / 5,
              "five causes account for a substantial share of every SWING causal edge");
        std::printf("        (top five %d of %d swing causal edges, %d %%)\n",
                    topFive, swingEdges, swingEdges ? topFive * 100 / swingEdges : 0);

        // Every dominant cause must be screen-backed — that is what makes the output actionable
        // without any capture hardware at all.
        bool allScreened = true;
        for (const char *id : { "poor_pelvic_disassociation", "limited_thoracic_rotation",
                                "limited_lead_hip_ir", "limited_trail_hip_ir", "poor_core_stability" }) {
            const Condition *c = p.condition(QString::fromLatin1(id));
            if (!c || c->confirmedBy != ConfirmedBy::Screened) allScreened = false;
        }
        check(allScreened, "every dominant cause is screen-backed");
    }

    // ── Edge orientation, structurally ──────────────────────────────────────────
    // The seed tables read effect-first while Edge is cause-first, so every row flips on
    // transcription. A coverage count CANNOT catch a mistake here — totals are identical under edge
    // reversal — so this is the assertion that guards the whole graph.
    {
        bool noneInverted = true;
        for (const Condition &c : p.conditions) {
            if (c.confirmedBy != ConfirmedBy::Screened) continue;
            if (!causesOf(p, c.id).isEmpty()) noneInverted = false;      // in-degree must be 0
            if (effectsOf(p, c.id).isEmpty()) noneInverted = false;      // out-degree must be > 0
        }
        check(noneInverted, "every screened cause has in-degree 0 and out-degree > 0");

        // Two spot checks in plain English, so a reader can see the orientation is right.
        check(effectsOf(p, QStringLiteral("limited_hip_extension"))
                  .contains(QStringLiteral("s_posture")),
              "limited hip extension CAUSES S-posture (not the reverse)");
        check(causesOf(p, QStringLiteral("early_extension"))
                  .contains(QStringLiteral("limited_lead_hip_ir")),
              "early extension is CAUSED BY limited lead-hip internal rotation");
    }

    // ── Every characteristic resolves to Live, or to a NAMED missing measure ────
    {
        int live = 0, planned = 0, noProducer = 0, notCapturable = 0, externalDevice = 0;
        bool everyGapNamed = true;

        for (const Condition &c : p.conditions) {
            if (c.observability != Observability::Observable) continue;
            for (const QString &sid : c.detectedBy) {
                const Signal *s = p.signal(sid);
                if (!s) continue;
                for (const QString &mid : s->measures) {
                    const Measure *m = p.measure(mid);
                    if (!m) { everyGapNamed = false; continue; }
                    switch (m->status) {
                    case MeasureStatus::Live:          ++live; break;
                    case MeasureStatus::Planned:       ++planned; break;
                    case MeasureStatus::NoProducer:    ++noProducer; break;
                    case MeasureStatus::NotCapturable:
                        ++notCapturable;
                        if (m->gapReason.isEmpty()) everyGapNamed = false;
                        break;
                    // Held to the same standard as a capture gap: the status says something stands
                    // in the way, and only the reason says WHICH device. Two surfaces quote it.
                    case MeasureStatus::ExternalDevice:
                        ++externalDevice;
                        if (m->gapReason.isEmpty()) everyGapNamed = false;
                        break;
                    }
                }
            }
        }
        std::printf("        (measure bindings: %d live, %d planned, %d no-producer, "
                    "%d external-device, %d capture-gap)\n",
                    live, planned, noProducer, externalDevice, notCapturable);
        check(everyGapNamed, "every characteristic resolves to a real measure, gaps named");
        check(live > 0, "some characteristics are LIVE on day one, not all stubs");
    }

    // ── Provided measures bind to the catalogue, not to a parallel registry ─────
    {
        bool allBound = true;
        int  provided = 0;
        for (const Measure &m : p.measures) {
            if (m.kind != MeasureKind::Provided) continue;
            ++provided;
            if (m.metricKey.isEmpty()) allBound = false;
        }
        check(provided > 0 && allBound, "every Provided measure names a MetricCatalogue key");

        // The payoff of ranking series rather than reduced measures: one producer unblocks several
        // characteristics. pelvisSway carries sway, slide, hanging back and the finish read at four
        // different phases — one producer, four faults, which is the payoff being demonstrated. The
        // number is updated deliberately when a reducer is added, never loosened to a `>=`: the
        // claim is that these are the reducers somebody chose, not that there are some.
        int onPelvisSway = 0;
        for (const Measure &m : p.measures)
            if (m.metricKey == QStringLiteral("pelvisSway")) ++onPelvisSway;
        check(onPelvisSway == 4, "four characteristics sit on one pelvis-sway series (one producer)");
    }

    // ── The spinal measures are roadmap items, not capture gaps ─────────────────
    // They have no pose keypoint, but the back contour of a down-the-line silhouette carries both
    // the thoracic round and the lumbar arch — so the honest classification is "producer not
    // written" rather than "this product can never see it".
    {
        const Measure *thoracic = p.measure(QStringLiteral("m_thoracicCurve"));
        const Measure *lumbar   = p.measure(QStringLiteral("m_lumbarCurve"));
        check(thoracic && thoracic->status == MeasureStatus::NoProducer,
              "C-posture's measure is a roadmap item (DTL back contour would produce it)");
        check(lumbar && lumbar->status == MeasureStatus::NoProducer,
              "S-posture's measure likewise");
        check(thoracic && !thoracic->gapReason.isEmpty(),
              "and it still explains why the skeleton cannot supply it");

        // Carried deliberately despite being unmeasurable: four conditions cite C-posture as a
        // cause, so dropping it would cost them their strongest explanation.
        check(coverageOf(p, QStringLiteral("c_posture")) >= 2,
              "C-posture is carried because other conditions depend on it");
    }

    // ── Screened and Behavioural causes never enter the roadmap ─────────────────
    // One row implying a producer that will never be built corrupts the artefact for every other
    // row, so this is a hard rule rather than a presentation choice.
    {
        bool clean = true;
        for (const Condition &c : p.conditions) {
            if (!isOutsideCaptureReach(c.confirmedBy)) continue;
            for (const QString &sid : c.detectedBy) {
                const Signal *s = p.signal(sid);
                if (s && !s->measures.isEmpty()) clean = false;   // it would land in the roadmap
            }
        }
        check(clean, "no Physical/Behavioural cause carries a measure that could reach the roadmap");
    }

    // ── Tail splits ─────────────────────────────────────────────────────────────
    {
        for (const char *axis : { "ball_position", "stance_width", "alignment", "ball_body_distance" })
            check(tailsOfAxis(p, QString::fromLatin1(axis)).size() == 2,
                  "a two-sided characteristic has both tails authored");

        // Both tails must sit on the same measure — that is what makes them tails.
        const QStringList ballTails = tailsOfAxis(p, QStringLiteral("ball_position"));
        check(ballTails.size() == 2 && ballTails.contains(QStringLiteral("ball_forward"))
                  && ballTails.contains(QStringLiteral("ball_back")),
              "ball position splits into forward and back");

        // Mark's case, wired at BOTH ends: ball forward opens the shoulders, ball back closes them.
        check(effectsOf(p, QStringLiteral("ball_forward")).contains(QStringLiteral("alignment_open")),
              "ball forward causes an open shoulder line");
        check(effectsOf(p, QStringLiteral("ball_back")).contains(QStringLiteral("alignment_closed")),
              "ball back causes a closed shoulder line");
        check(!effectsOf(p, QStringLiteral("ball_forward")).contains(QStringLiteral("alignment_closed")),
              "the tails are not cross-wired");
    }

    // ── C-posture's two routes have different remedies ──────────────────────────
    {
        const QStringList causes = causesOf(p, QStringLiteral("c_posture"));
        check(causes.contains(QStringLiteral("thoracic_kyphosis")), "C-posture has a physical cause");
        check(causes.contains(QStringLiteral("ball_too_far")), "C-posture has a setup-induced cause");

        const Condition *phys  = p.condition(QStringLiteral("thoracic_kyphosis"));
        const Condition *setup = p.condition(QStringLiteral("ball_too_far"));
        check(phys && phys->confirmedBy == ConfirmedBy::Screened, "the physical route is screened");
        check(setup && setup->confirmedBy == ConfirmedBy::Measured, "the setup route is measured");
    }

    // ── No brand names anywhere in the content ──────────────────────────────────
    // Several conditions are named with terms popularised by a commercial screening system. The
    // TERMS are common domain and stay; the ATTRIBUTION must not enter the repo in any form — not a
    // citation, not an author, not an explanatory note. Checked against the raw bytes so a comment
    // or a stray field cannot slip past.
    //
    // ALL THREE reviewable content files, not just the pack. The rule was always stated over the
    // content as a whole, but the grep only ever covered core.json — and a vendor name duly sat
    // unnoticed in a norms.json citation note. A rule enforced over one of three files is a rule
    // that reads as enforced and is not.
    //
    // THE ONE SANCTIONED ESCAPE, because otherwise this rule silently costs the library citations:
    // a peer-reviewed paper whose PUBLISHED TITLE names such a system may still be recorded, with
    // the name replaced by a bracketed editorial redaction — `ref.gulgin2014` is the worked example.
    // The identifier is never touched, so the record resolves to the real title in one click, and
    // the redaction declares itself in `establishes` rather than being silent. That is a standard
    // scholarly abridgement, not a workaround: the alternative is dropping the only study that
    // tested physical screens against visible swing faults, which would leave the library's most
    // load-bearing negative result uncitable. Redact the BRAND, never the finding.
    {
        const char *forbidden[] = { "titleist", "tpi", "trackman", "flightscope", "foresight",
                                    "k-vest", "kvest", "gears", "swingcatalyst", "boditrak",
                                    "performance institute", "certified", "hackmotion" };
        const struct { const char *label; const char *path; } files[] = {
            { "core.json",       PP_CORE_PACK_PATH },
            { "norms.json",      PP_CORE_NORMS_PATH },
            { "references.json", PP_CORE_REFERENCES_PATH },
        };

        bool clean = true;
        for (const auto &file : files) {
            QFile cf(QString::fromUtf8(file.path));
            if (!cf.open(QIODevice::ReadOnly)) {
                std::printf("        cannot open %s for the brand grep\n", file.label);
                clean = false;
                continue;
            }
            const QString lower = QString::fromUtf8(cf.readAll()).toLower();
            for (const char *needle : forbidden) {
                if (lower.contains(QLatin1String(needle))) {
                    std::printf("        brand token '%s' found in %s\n", needle, file.label);
                    clean = false;
                }
            }
        }
        check(clean, "no commercial organisation, product or certification body is named");
    }

    // ── Uncited content is honestly badged ──────────────────────────────────────
    {
        // This used to assert `proposed > 0`, as a proxy for "uncited content is not laundered
        // into a cited tier". That proxy was only ever valid mid-search: `Proposed` means NOBODY
        // HAS LOOKED, so a search pass that reaches every condition legitimately empties the tier,
        // and re-adding a proposed row to satisfy the old check would be the dishonesty it was
        // written to prevent. DO NOT RESTORE IT.
        //
        // What it was really protecting is asserted directly instead: the untested majority must
        // still be recorded as untested. `practice` says the field agrees and has never measured
        // the named state, which is the true state of most of this library — and a pack where
        // more content claimed a source than admitted to coaching orthodoxy would be the
        // laundering worth failing over.
        int cited = 0, practice = 0;
        for (const Condition &c : p.conditions) {
            if (c.provenance.tier == ProvenanceTier::Practice) ++practice;
            if (citationRequired(c.provenance.tier))           ++cited;
        }
        check(practice > cited, "the untested majority is recorded as coaching practice, not as sourced");

        // And the pass actually happened: every condition names the day it was searched. This is
        // strictly stronger than the check it replaced — that one could pass with 111 conditions
        // never looked at.
        bool allSearched = true;
        for (const Condition &c : p.conditions)
            if (!c.provenance.searched()) allSearched = false;
        check(allSearched, "every condition records the date its search was made");

        std::printf("        (conditions: %d practice, %d cited)\n", practice, cited);

        // Keyed on citationRequired(), not on "above proposed". Two tiers are ABOVE proposed and
        // legitimately carry no citation: NoSourceFound asserts the absence of a source, and
        // Practice cites a body of coaching practice this repo is not permitted to name. Demanding
        // a citation for either would make the finding unrecordable — which is how they came to be
        // conflated with "nobody has looked" in the first place.
        bool noFakeCitations = true, noUndatedSearch = true;
        for (const Condition &c : p.conditions) {
            if (citationRequired(c.provenance.tier) && c.provenance.citation.isEmpty())
                noFakeCitations = false;
            if (searchDateRequired(c.provenance.tier) && !c.provenance.searched())
                noUndatedSearch = false;
        }
        check(noFakeCitations, "no condition claims a cited tier without a citation");
        check(noUndatedSearch, "no condition records a search outcome without the date it was made");

        // The same two rules over the edges, which until now could make neither claim at all.
        bool edgesHonest = true;
        for (const Edge &e : p.edges)
            if ((citationRequired(e.provenance.tier) && e.provenance.citation.isEmpty())
                || (searchDateRequired(e.provenance.tier) && !e.provenance.searched()))
                edgesHonest = false;
        check(edgesHonest, "no edge claims a cited tier without a citation, or a search without a date");

        std::map<QString, int> byTier;
        for (const Edge &e : p.edges) ++byTier[provenanceTierName(e.provenance.tier)];
        std::printf("        (edge provenance:");
        for (const auto &kv : byTier) std::printf(" %s=%d", qPrintable(kv.first), kv.second);
        std::printf(")\n");
    }

    // ── No peer tier rests on a book ────────────────────────────────────────────
    // `Supported` means a peer-reviewed source tested this cause and this effect; `Established`
    // means independent sources reproduced it. A book is neither, however good it is, and admitting
    // ISBNs to the bibliography without this check would let coaching doctrine be filed under the
    // two tiers that exist to mean the opposite of doctrine.
    //
    // The pack loader demotes this at load time (`bookCitationAtPeerTier`), and that is precisely
    // why the content needs its OWN check: a validator that fires is not the same as content that
    // is right, and a reader of core.json sees the authored tier, not the corrected one. Note that
    // this target deliberately runs with NO bibliography on the path — see its CMake comment — so
    // the tiers read here are exactly as authored, and the registry is loaded by hand below.
    {
        QFile rf(QStringLiteral(PP_CORE_REFERENCES_PATH));
        check(rf.open(QIODevice::ReadOnly), "the shipped reference registry is readable");
        const ReferenceLoadResult rres =
            loadReferenceSet(rf.readAll(), QStringLiteral("references.json"));
        check(rres.loaded, "the shipped reference registry loads and validates clean");

        // The identifiers of every record whose ONLY identifier is an ISBN. A record carrying both
        // an ISBN and a DOI is a paper that also happens to have been collected into a volume, and
        // it is peer-reviewed on the strength of the DOI.
        QSet<QString> bookOnly;
        for (const Reference &r : rres.pack.references)
            if (r.doi.isEmpty() && r.pmid.isEmpty() && !r.isbn.isEmpty()) bookOnly.insert(r.isbn);

        const auto peer = [](ProvenanceTier t) {
            return t == ProvenanceTier::Supported || t == ProvenanceTier::Established;
        };

        int overclaimed = 0;
        for (const Condition &c : p.conditions)
            if (peer(c.provenance.tier) && bookOnly.contains(c.provenance.citation)) {
                ++overclaimed;
                std::printf("        '%s' claims a peer tier on the book '%s'\n",
                            qPrintable(c.id), qPrintable(c.provenance.citation));
            }
        for (const Edge &e : p.edges)
            if (peer(e.provenance.tier) && bookOnly.contains(e.provenance.citation)) {
                ++overclaimed;
                std::printf("        edge '%s' -> '%s' claims a peer tier on the book '%s'\n",
                            qPrintable(e.from), qPrintable(e.to),
                            qPrintable(e.provenance.citation));
            }
        check(overclaimed == 0, "no condition or edge claims a peer-reviewed tier on a book");
        std::printf("        (%d book-only references in the registry)\n", int(bookOnly.size()));
    }

    // ── Every condition says what it costs the golfer ───────────────────────────
    {
        bool allHaveConsequence = true;
        for (const Condition &c : p.conditions)
            if (c.consequence.text().isEmpty()) allHaveConsequence = false;
        check(allHaveConsequence, "every condition carries a plain-English consequence");
    }

    // ── The health list is populated, and it is warnings not errors ─────────────
    {
        const int warnings = res.report.warningCount();
        check(warnings > 0, "the health list has content (uncited tiers, single-tail axes)");
        check(res.report.errorCount() == 0, "nothing in the health list is an error");
        std::printf("        (%d health-list warnings)\n", warnings);
    }

    // ── No LIVE corridor signal is left without a norm ─────────────────────────
    // "The pack is dark" was the state this whole exercise existed to end: 30 corridor signals and
    // not one norm to grade against, so the engine correctly reported Unavailable for every single
    // characteristic and the library detected nothing at all. It looked like a working library.
    //
    // Scoped to LIVE measures on purpose. A signal on a measure with no producer cannot fire
    // whatever norms exist, so requiring norms there would assert something that changes nothing;
    // a signal on a live measure with no norm is a real hole, and this is what makes it loud.
    {
        QFile nf(QStringLiteral(PP_CORE_NORMS_PATH));
        check(nf.open(QIODevice::ReadOnly), "the shipped norm set is readable");
        const NormPackLoadResult nres = loadNormPack(nf.readAll(), QStringLiteral("norms.json"));
        check(nres.loaded, "the shipped norm set loads and validates clean");

        int live = 0, dark = 0;
        for (const Signal &sig : p.signalDefs) {
            if (sig.test != SignalTest::OutsideCorridor || sig.measures.isEmpty()) continue;
            const Measure *m = p.measure(sig.measures.first());
            if (m == nullptr || m->status != MeasureStatus::Live) continue;
            ++live;
            if (nres.pack.contextsFor(m->id).isEmpty()) {
                ++dark;
                std::printf("        '%s' is live but has no norm in any context\n",
                            qPrintable(m->id));
            }
        }
        std::printf("        (%d live corridor signals, %d without a norm)\n", live, dark);
        check(live > 0, "there are live corridor signals to check");
        check(dark == 0, "every LIVE corridor signal resolves a norm — the pack cannot go dark");

        // ── Where the pelvis-thrust corridors call a fault, and where they SURFACE ──
        //
        // TWO DIFFERENT NUMBERS, and a corridor row states neither of them directly. The fault
        // line is `watchMaxZ * sigma`; the point where the detector first surfaces the condition
        // is `goodMaxZ * sigma`, two thirds of it, because evaluate() fires on a DEVIATION and
        // not merely on leaving Ideal. An author reading 1.33 cm off the row would guess wrong
        // about both. Worth pinning here rather than trusting a comment, because a Finding
        // carries no severity — Watch and Action reach a reader identically — so the gap between
        // the two is invisible everywhere else in the app.
        //
        // Asserted through grade() rather than by multiplying it out, so the day the policy table
        // or the deviation rule moves, this fails instead of quietly agreeing with stale
        // arithmetic. Shape is read off the MEASURE, never assumed: m_pelvisThrustBack is a
        // ceiling and passing Target here would grade its open tail.
        {
            const GradePolicy pol = gradePolicyByName(QStringLiteral("standard"));
            auto gradeOf = [&](const char *measureId, double cm) {
                const Measure *m = p.measure(QString::fromLatin1(measureId));
                const Norm    *n = nres.pack.find(QString::fromLatin1(measureId),
                                                  QStringLiteral("any"));
                return (m && n) ? grade(cm, *n, m->shape, pol) : Grade::NotMeasured;
            };
            auto isFault   = [&](const char *id, double cm) { return gradeOf(id, cm) == Grade::Action; };
            auto surfaces  = [&](const char *id, double cm) { return isDeviation(gradeOf(id, cm)); };

            check(!isFault("m_pelvisThrustBack", 3.9),
                  "3.9 cm of pelvis thrust toward the ball going back is not yet a fault");
            check(isFault("m_pelvisThrustBack", 4.1),
                  "…and beyond 4 cm it is — the line this corridor was authored for");
            check(!surfaces("m_pelvisThrustBack", 2.6) && surfaces("m_pelvisThrustBack", 2.8),
                  "…while the CONDITION surfaces from about 2.7 cm, two thirds of the fault line");
            // The open tail. A ceiling makes no claim about the pelvis sitting back going back,
            // and this is the assertion that would fail if somebody 'tidied' the shape to Target.
            check(gradeOf("m_pelvisThrustBack", -6.0) == Grade::Ideal,
                  "and moving AWAY from the ball going back is not graded at all");

            // The same quantity's DOWNSWING row, stated so the asymmetry is visible rather than
            // buried in two sigmas eight lines apart in a JSON file. Early extension itself does
            // not reach Action until 12 cm, three times the backswing figure. Some late thrust is
            // normal, so the two are not meant to match — but only the backswing number has been
            // sanctioned, and a reader should be able to see that here without computing it.
            check(!isFault("m_pelvisThrustDown", 11.9),
                  "early extension is not a fault at 11.9 cm of downswing thrust");
            check(isFault("m_pelvisThrustDown", 12.1), "…and is beyond 12 cm");
        }

        // ── The plumb bob's per-club corridors ─────────────────────────────────
        //
        // The one shipped measure whose corridor CHANGES SIGN between clubs, which is what makes it
        // worth pinning here rather than trusting four rows in a JSON file. An inch and a half ahead
        // of the stance centre is an Ideal wedge setup and a bad driver one; an inch behind is the
        // reverse. A reader who saw only the `any` row would conclude the metric barely moves.
        //
        // Graded through grade() and the measure's own shape rather than by reading mu back, so the
        // day the policy table moves this fails instead of quietly agreeing with stale arithmetic.
        // find() is an EXACT-context lookup and does not walk the tree — which is the right tool
        // here, because the claim is that each row exists under the node the club actually resolves
        // to. The inheritance half (a 7 iron reaching the `iron` family) is context_tree_test's.
        {
            const GradePolicy pol = gradePolicyByName(QStringLiteral("standard"));
            const Measure *pb = p.measure(QStringLiteral("m_plumbBobAddress"));
            check(pb != nullptr, "the plumb bob has a measure at address");
            auto gradeIn = [&](const char *ctx, double inches) {
                const Norm *n = nres.pack.find(QStringLiteral("m_plumbBobAddress"),
                                               QString::fromLatin1(ctx));
                return (pb && n) ? grade(inches, *n, pb->shape, pol) : Grade::NotMeasured;
            };

            check(gradeIn("wedge", 1.5) == Grade::Ideal,
                  "an inch and a half ahead of centre is an Ideal WEDGE setup");
            check(gradeIn("driver", 1.5) == Grade::Action,
                  "…and the same number with a DRIVER is a fault — the corridor changes sign");
            check(gradeIn("driver", -1.0) == Grade::Ideal,
                  "an inch BEHIND centre is where the driver wants the hips");
            check(gradeIn("wedge", -1.0) == Grade::Action, "…and is a fault with a wedge");
            check(gradeIn("iron", 0.5) == Grade::Ideal, "half an inch ahead suits a mid-iron");

            // The rows have to live under the nodes the clubs actually resolve to, or they are
            // authored where nothing reaches them and every swing grades against `any`.
            check(contextIdForClub(QStringLiteral("PITCHING WEDGE")).startsWith(QLatin1String("wedge"))
                      && contextIdForClub(QStringLiteral("DRIVER")) == QLatin1String("driver")
                      && contextIdForClub(QStringLiteral("7 IRON")).startsWith(QLatin1String("iron")),
                  "…and each club resolves into the family its row is authored under");

            // The unknown-club row is deliberately wide: it has to cover a corridor that spans two
            // and a half inches between the clubs above, and a narrow `any` row would call a
            // legitimate driver setup a fault on every swing whose club was never recorded.
            check(gradeIn("any", 1.5) != Grade::Action && gradeIn("any", -1.0) != Grade::Action,
                  "with the club unknown, neither a wedge nor a driver setup is called a fault");
        }

        // ── Nothing the app can DETECT is left without an EXPLANATION ───────────
        // The counterpart to the check above, and the one that nearly slipped. A signal that can
        // fire on a condition with no cause hands the coach "your head moved" — which the golfer
        // already knew — and the whole point of the model is the next sentence.
        //
        // This gap was invisible because it forms at the intersection of two healthy-looking
        // states: the causal work went in per GROUP, while the firing set is decided per PRODUCER,
        // and the eleven live-but-unclaimed producer keys arrived after their group had already
        // been wired. Four such conditions shipped isolated — no cause AND no effect — and six
        // more had no cause. All ten were among the sixteen that can actually fire, so the least
        // explicable part of the library was precisely the part a golfer would meet first.
        //
        // Scoped to what can fire, deliberately. `noCause` on a producer-less condition is a
        // content backlog and the validator already reports it; a condition that fires TODAY with
        // nothing behind it is a defect in what ships.
        int fireable = 0, unexplained = 0;
        for (const Condition &c : p.conditions) {
            if (c.observability == Observability::Latent) continue;

            const bool canFire = std::any_of(
                c.detectedBy.cbegin(), c.detectedBy.cend(), [&](const QString &sid) {
                    const Signal *sig = p.signal(sid);
                    if (sig == nullptr || sig->measures.isEmpty()) return false;
                    return std::all_of(
                        sig->measures.cbegin(), sig->measures.cend(), [&](const QString &mid) {
                            const Measure *m = p.measure(mid);
                            if (m == nullptr || m->status != MeasureStatus::Live) return false;
                            return sig->test != SignalTest::OutsideCorridor
                                   || !nres.pack.contextsFor(mid).isEmpty();
                        });
                });
            if (!canFire) continue;

            ++fireable;
            if (causesOf(p, c.id).isEmpty()) {
                ++unexplained;
                std::printf("        '%s' can fire today but has no cause — it would be reported "
                            "and never explained\n", qPrintable(c.id));
            }
        }
        std::printf("        (%d conditions can fire, %d of them unexplainable)\n",
                    fireable, unexplained);
        check(fireable > 0, "there are conditions that can fire");
        check(unexplained == 0, "every condition that can fire today has at least one cause");
    }

    // ── The pack enumeration: sibling registries out, every real pack in ───────
    // AppData/diagnostics is not exclusive to characteristic packs: file_norm_provider.cpp writes
    // `user.norms.json` there (see its note on why norms and packs share one directory), and
    // screen_pack.cpp / drill_pack.cpp / reference_pack.cpp write `screens.json` / `drills.json` /
    // `references.json` into the very same place, and file_norm_provider.cpp also reads a fixed
    // `contexts.json` from it. A `*.json` glob that does not know about them picks all five up: each
    // has no top-level `id` of its own, so it injects a spurious "Pack has no id" issue into a
    // provider's report — and worse, QDir::Name sorts `drills.json` ahead of `user.json`, so a
    // sibling that DOES carry a non-empty `id` can silently become a pack in the assembled library.
    // Regression, not a smoke test: this directory is shared on purpose, so the sibling files are
    // always going to be sitting right there.
    //
    // The skip list moved into characteristicPackFilesIn() when the directory stopped being one
    // provider, so that is what this now exercises — the enumeration AND the layering order it
    // decides. Extended rather than replaced: every original assertion is still made below.
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "temp directory for the shared-directory regression is created");
        QDir dir(tmp.path());

        auto write = [&](const char *name, const char *content) {
            QFile f(dir.filePath(QString::fromLatin1(name)));
            const bool opened = f.open(QIODevice::WriteOnly);
            check(opened, name);
            f.write(content);
        };

        write("user.json", R"({"id":"user_test"})");
        write("user.norms.json", "{}");
        write("screens.json", "{}");
        // A non-empty top-level `id`, deliberately: this is the file that USED TO be mistaken for
        // the user pack, because `drills.json` sorts ahead of `user.json` under QDir::Name and the
        // old glob had no way to tell a drill registry from a characteristic pack.
        write("drills.json", R"({"id":"drills_should_not_load"})");
        write("references.json", "{}");
        write("contexts.json", "{}");
        // A SECOND pack, sorting ahead of `user.json`. Two things ride on it. It is the file the
        // directory-wide provider used to swallow — parsed, reported, then discarded, because only
        // the first readable pack became "this provider's pack" while the comment beside that line
        // claimed a directory of several was represented by several providers. Nothing built the
        // several. And its name proves the ORDER rule below is the user-first one rather than plain
        // alphabetical, which sorting after `user.json` could not distinguish.
        write("community.json", R"({"id":"community_test"})");

        const QStringList packs = characteristicPackFilesIn(tmp.path());
        QStringList       names;
        for (const QString &p : packs) names << QFileInfo(p).fileName();
        check(names == QStringList({ QStringLiteral("user.json"),
                                     QStringLiteral("community.json") }),
              "the pack files are the two packs, user.json FIRST and the rest alphabetically");

        const std::vector<std::unique_ptr<ICharacteristicPackProvider>> fps =
            makeFilePackProviders(tmp.path());
        check(fps.size() == 2, "a directory holding two packs yields TWO providers, one per file");
        if (fps.size() == 2) {
            check(fps[0]->pack().id == QStringLiteral("user_test"),
                  "the real user pack loads, and no sibling registry is mistaken for it");
            check(fps[1]->pack().id == QStringLiteral("community_test"),
                  "…and the second pack is not silently discarded, as it was for as long as one "
                  "provider covered the whole directory");
            // Origin is decided by file NAME because that is the only evidence there is, and it
            // decides whether a pack may redefine shipped content. A downloaded pack merged as
            // LocalUser could overwrite a core characteristic with no warning anywhere.
            check(fps[0]->origin() == PackOrigin::LocalUser,
                  "user.json is the user's own layer, so it may override core");
            check(fps[1]->origin() == PackOrigin::Community,
                  "anything else arrived from somebody else, so core wins over it");
            check(fps[0]->report().issues.empty() && fps[1]->report().issues.empty(),
                  "the sibling registries raise no issues in either pack provider's report");
        }
    }

    // ── A broken pack costs the library that pack and nothing else ─────────────
    // The containment claim one-provider-per-file is FOR. While a directory was one provider, a
    // malformed file's issues landed in the same report as the good pack's and there was no way to
    // read off which file was at fault; worse, the good pack could be the one that got dropped,
    // purely by sorting second. Both halves are asserted: the fault is attributed to its own file,
    // and the neighbour still loads.
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "temp directory for the broken-neighbour case is created");
        QDir dir(tmp.path());

        auto write = [&](const char *name, const char *content) {
            QFile f(dir.filePath(QString::fromLatin1(name)));
            check(f.open(QIODevice::WriteOnly), name);
            f.write(content);
        };

        write("user.json", R"({"id":"user_test"})");
        write("zbroken.json", "{ this is not json");

        const std::vector<std::unique_ptr<ICharacteristicPackProvider>> fps =
            makeFilePackProviders(tmp.path());
        check(fps.size() == 2, "the unreadable pack still gets a provider — it has a report to give");
        if (fps.size() == 2) {
            check(fps[0]->pack().id == QStringLiteral("user_test") && fps[0]->report().issues.empty(),
                  "the good pack loads clean, unaffected by its broken neighbour");
            check(fps[1]->pack().id.isEmpty(), "the broken pack contributes no content");

            bool blamedItsOwnFile = false;
            for (const ValidationIssue &i : fps[1]->report().issues)
                if (i.code == QLatin1String("parse")
                    && i.subject == dir.filePath(QStringLiteral("zbroken.json")))
                    blamedItsOwnFile = true;
            check(blamedItsOwnFile,
                  "and its parse failure names the FILE it came from, so the health list can say "
                  "which pack to fix");
        }
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
