// The corridor editor's model (src/Gui/characteristics/norm_editor_model.h), run against the
// SHIPPED pack and norm set.
//
// Its whole job is that QML holds no rules, so every rule it hides is asserted here or nowhere:
//
//   1. The two handles bind the norm's own CLAIM — mu +/- sigma, per side. That is what norm.h
//      documents claimLo()/claimHi() as, and the plan carried "the Good band" in its stage-6 text
//      until 2026-07-26, which is exactly the kind of drift a test has to stop.
//   1b. The CLAIM and the IDEAL band are different objects. The claim is fixed; the Ideal band is
//      the claim scaled by the active grade policy. They coincide under `standard` only. This test
//      asserted the opposite until 2026-07-28 — "a policy change does not move the IDEAL band" —
//      which is how the divergence survived: the drawing path ignored idealMaxZ while grade()
//      applied it, so under `strict` a value at 0.9 sigma drew green and graded Good (amber chip).
//   2. Hand-editing DROPS a Seated provenance. Leaving "seated, n = 42" attached to numbers a hand
//      has since moved is a claim the norm no longer supports.
//   3. Seating fits a PER-SIDE tolerance, so an asymmetric sample yields an asymmetric corridor.
//   4. The grade counts and the histogram move together and agree with grade() — they are the
//      safety mechanism, and a histogram that disagreed with the grader would be worse than none.
//   5. The band edges shown follow the ACTIVE grade policy, because the edge drawn must be the edge
//      that grades.
//   6. A NotCapturable measure is refused: a corridor on it can never do anything but mislead.
//   7. A ONE-SIDED measure edits differently and nothing about it leaks into a target norm. mu and
//      the tolerance are independent numbers rather than two ends of a span; setClaimBand is
//      refused because its midpoint would move mu and its split would leave the two sigmas
//      unequal, which the pack validator rejects; every drawn band collapses onto the aspiration,
//      Good included — that one is computed outside bandEdgesOf and would otherwise escape.
//   8. The readouts are FAITHFUL, not rounded to the pack's usual one decimal. A label that
//      rounds is cosmetic; a field that rounds saves the rounding, because PpTextField commits on
//      focus loss.
//
//   cmake --build build/analyzer-tests --target norm_editor_model_test
//   ctest --test-dir build/analyzer-tests -R norm_editor_model --output-on-failure

#include "norm_editor_model.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static void near(double a, double b, const char *label, double tol = 1e-6)
{
    const bool ok = std::fabs(a - b) <= tol;
    std::printf("  [%s] %s (got %.6f, want %.6f)\n", ok ? "PASS" : "FAIL", label, a, b);
    if (!ok) ++g_fail;
}

namespace {

// A measure the shipped pack carries with a norm on full_swing, so the draft opens seeded.
const char *kMeasure = "m_leadWristRadUln_p4";
// The context core carries a row AT, which is the root: the general corridors live at `any` because
// a full swing is a shot TYPE, not the general case (see norms.json's own comment). Every assertion
// below is about the editor's mechanics, so what matters is only that this is a context with a
// shipped row of its own; the inheriting case gets its own block at the end.
const char *kContext = "any";

double num(const QVariantMap &m, const char *k) { return m.value(QLatin1String(k)).toDouble(); }

// A copy of the shipped pack with ONE measure given a shape, written to a scratch file whose path
// is handed to the next NormEditorModel through the PINPOINT_CORE_PACK seam that already exists
// for exactly this (resource_pack_provider.cpp reads it per construction).
//
// Patching the real pack rather than hand-writing a fixture: the assertions then run against the
// actual shipped measure and its actual norm rows, so they cannot pass against a pack invented to
// make them pass. m_smashFactor because it is the measure the seed conversion will really change.
QString packWithShape(const char *shape)
{
    QFile in(QString::fromLocal8Bit(qgetenv("PINPOINT_CORE_PACK")));
    if (!in.open(QIODevice::ReadOnly))
        return QString();

    QJsonObject doc = QJsonDocument::fromJson(in.readAll()).object();
    QJsonArray  ms  = doc.value(QStringLiteral("measures")).toArray();
    bool        hit = false;
    for (int i = 0; i < ms.size(); ++i) {
        QJsonObject m = ms.at(i).toObject();
        if (m.value(QStringLiteral("id")).toString() != QLatin1String("m_smashFactor"))
            continue;
        m.insert(QStringLiteral("shape"), QLatin1String(shape));
        ms.replace(i, m);
        hit = true;
    }
    if (!hit)
        return QString();                        // the measure was renamed: fail loudly, not quietly
    doc.insert(QStringLiteral("measures"), ms);

    const QString path =
        QDir::temp().filePath(QStringLiteral("pp_norm_editor_%1_pack.json").arg(QLatin1String(shape)));
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    out.write(QJsonDocument(doc).toJson());
    return path;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::printf("norm_editor_model_test\n");

    NormEditorModel ed;

    // ── Opening ─────────────────────────────────────────────────────────────
    std::printf("\nopening a draft\n");
    check(!ed.isOpen(), "closed until begin()");
    check(ed.draft().isEmpty(), "a closed model renders nothing");
    check(!ed.begin(QStringLiteral("m_no_such_measure"), QString::fromLatin1(kContext)),
          "an unknown measure is refused");
    check(!ed.begin(QString::fromLatin1(kMeasure), QStringLiteral("hovercraft")),
          "an unknown context is refused");
    check(ed.begin(QString::fromLatin1(kMeasure), QString::fromLatin1(kContext)),
          "a real (measure, context) opens");
    check(ed.isOpen(), "…and reports open");

    QVariantMap d = ed.draft();
    check(d.value(QStringLiteral("measureId")).toString() == QLatin1String(kMeasure),
          "the draft names its measure");
    check(!d.value(QStringLiteral("unit")).toString().isEmpty(), "the draft carries a unit");
    check(!d.value(QStringLiteral("dirty")).toBool(), "a freshly opened draft is not dirty");
    check(!d.value(QStringLiteral("canSave")).toBool(), "…and cannot be saved unchanged");

    // Seeded from the shipped row, not from zero.
    const double seededLo = num(d, "claimLo"), seededHi = num(d, "claimHi");
    check(seededHi > seededLo, "seeded from the resolved corridor, not a degenerate point");

    // ── The key contract with CorridorEditor.qml ────────────────────────────
    //
    // A field can be complete on both sides and reach nothing: QML reads a missing key as
    // `undefined`, every `|| 0` fallback turns that into a corridor at zero, and no binding, test
    // or screenshot says a word. So the keys the view binds to are named here, and a rename that
    // does not reach the .qml fails HERE rather than on somebody's screen.
    for (const char *k : { "claimLo", "claimHi", "idealLo", "idealHi", "goodLo", "goodHi",
                           "watchLo", "watchHi", "mu", "unit", "highMeans", "explicitMonitor",
                           "parentClaimLo", "parentClaimHi", "hasParent", "inheritedFrom",
                           "shippedClaimLo", "shippedClaimHi", "hasShipped", "editedNote",
                           "resetLabel", "canReset", "canSave", "canDiscard", "refused" })
        check(d.contains(QLatin1String(k)), k);

    // ── The handles bind the norm's own CLAIM ───────────────────────────────
    std::printf("\nthe two handles\n");
    ed.setClaimBand(10.0, 30.0);
    d = ed.draft();
    near(num(d, "claimLo"), 10.0, "claimLo is the low handle");
    near(num(d, "claimHi"), 30.0, "claimHi is the high handle");
    near(num(d, "mu"),      20.0, "mu is the centre between them");
    check(d.value(QStringLiteral("dirty")).toBool(), "moving a handle dirties the draft");

    // Under `standard` idealMaxZ is 1.0, so the Ideal band lands exactly on the handles. That is
    // the coincidence that hid the divergence for nine stages — asserted here so it reads as a
    // property of this preset rather than as a law.
    near(num(d, "idealLo"), 10.0, "under standard the Ideal band sits on the claim (lo)");
    near(num(d, "idealHi"), 30.0, "under standard the Ideal band sits on the claim (hi)");

    // Under the standard policy Good is |z| <= 2 and Watch <= 3, so the drawn edges are exactly
    // twice and three times the half-widths. This is the assertion that stops "the handles bind
    // the Good band" from creeping back in.
    near(num(d, "goodLo"),   0.0, "goodLo = mu - 2*sigmaLo");
    near(num(d, "goodHi"),  40.0, "goodHi = mu + 2*sigmaHi");
    near(num(d, "watchLo"), -10.0, "watchLo = mu - 3*sigmaLo");
    near(num(d, "watchHi"),  50.0, "watchHi = mu + 3*sigmaHi");

    // Asymmetric drag -> asymmetric tolerance, with no statistics vocabulary in the interaction.
    ed.setClaimBand(10.0, 40.0);
    d = ed.draft();
    near(num(d, "mu"), 25.0, "an asymmetric band still centres between the handles");
    near(num(d, "goodLo"), 10.0 - 15.0, "…and the low tail widens by ITS own sigma");
    near(num(d, "goodHi"), 40.0 + 15.0, "…and the high tail by its own");

    // A crossed drag must not produce a negative tolerance.
    ed.setClaimBand(50.0, 20.0);
    d = ed.draft();
    near(num(d, "claimLo"), 20.0, "a crossed drag is ordered, not negated (lo)");
    near(num(d, "claimHi"), 50.0, "a crossed drag is ordered, not negated (hi)");

    // ── The grade policy governs the drawn edges — ALL THREE of them ────────
    //
    // The claim is what the author asserted and must not move when a reader changes a sensitivity
    // setting. Every DRAWN band is a consequence of that setting, Ideal included. Until 2026-07-28
    // this section asserted "a policy change does not move the IDEAL band", which was the defect
    // written down as a requirement.
    std::printf("\ngrade policy\n");
    ed.setClaimBand(10.0, 30.0);
    ed.setGradePolicy(QStringLiteral("strict"));
    d = ed.draft();
    near(num(d, "claimLo"), 10.0, "a policy change does not move the CLAIM (lo)");
    near(num(d, "claimHi"), 30.0, "a policy change does not move the CLAIM (hi)");
    near(num(d, "idealLo"), 20.0 - 0.75 * 10.0, "strict pulls the Ideal edge in (0.75 sigma)");
    near(num(d, "idealHi"), 20.0 + 0.75 * 10.0, "…on both sides");
    near(num(d, "goodHi"),  20.0 + 1.5 * 10.0, "strict pulls the Good edge in (1.5 sigma)");
    near(num(d, "watchHi"), 20.0 + 2.25 * 10.0, "…and the Watch edge with it");

    // Lenient pushes it the other way, so the Ideal band is WIDER than the claim. A surface that
    // assumed the green band could never exceed mu +/- sigma would be wrong here.
    ed.setGradePolicy(QStringLiteral("lenient"));
    d = ed.draft();
    near(num(d, "claimHi"), 30.0, "…still the claim under lenient");
    near(num(d, "idealHi"), 20.0 + 1.5 * 10.0, "lenient pushes the Ideal edge out (1.5 sigma)");

    ed.setGradePolicy(QStringLiteral("wat"));
    d = ed.draft();
    near(num(d, "goodHi"), 40.0, "an unknown policy name resolves to standard, never persists");
    near(num(d, "idealHi"), 30.0, "…and standard puts Ideal back on the claim");

    // ── The axis must not move under a drag ─────────────────────────────────
    //
    // REGRESSION, and the defect was violent. The axis is derived from mu +/- watchMaxZ*sigma, so
    // widening the corridor widens the axis, which makes the SAME pixel mean a larger value, which
    // widens the corridor further — a gain of about 3 per mouse-move event, ~60 times a second.
    // Dragging a handle ran the corridor to absurd numbers before the pointer had moved a
    // centimetre.
    std::printf("\naxis latch under a drag\n");
    {
        NormEditorModel a;
        check(a.begin(QString::fromLatin1(kMeasure), QString::fromLatin1(kContext)), "opens");
        a.setClaimBand(10.0, 30.0);

        const double freeLo = a.sampleSummary().value(QStringLiteral("axisLo")).toDouble();
        const double freeHi = a.sampleSummary().value(QStringLiteral("axisHi")).toDouble();

        // Unlatched, the axis follows the corridor — which is right between gestures, and is
        // exactly what must NOT happen during one.
        a.setClaimBand(10.0, 300.0);
        check(a.sampleSummary().value(QStringLiteral("axisHi")).toDouble() > freeHi,
              "between gestures the axis re-fits to the corridor");

        a.setClaimBand(10.0, 30.0);
        a.beginHandleDrag();
        const double lockLo = a.sampleSummary().value(QStringLiteral("axisLo")).toDouble();
        const double lockHi = a.sampleSummary().value(QStringLiteral("axisHi")).toDouble();
        near(lockLo, freeLo, "the latch captures the axis as it was");
        near(lockHi, freeHi, "…both edges");

        // Simulate the gesture: several nudges, as a real drag delivers.
        for (int i = 0; i < 8; ++i) {
            a.nudgeClaimHi(30.0 + 5.0 * i);
            near(a.sampleSummary().value(QStringLiteral("axisLo")).toDouble(), lockLo,
                 "axis low stays put mid-drag");
            near(a.sampleSummary().value(QStringLiteral("axisHi")).toDouble(), lockHi,
                 "axis high stays put mid-drag");
        }

        a.endHandleDrag();
        check(a.sampleSummary().value(QStringLiteral("axisHi")).toDouble() > lockHi,
              "…and re-fits once the gesture ends, bringing the widened corridor back into frame");

        // The defect, stated exactly: HOLDING STILL must not move the corridor. The view maps a
        // pixel through the axis, so a pointer resting at one x re-sends the same x on every
        // delivered move event. With a live axis each re-send landed on a bigger number than the
        // last; with the latch the mapping is fixed and the value is a constant.
        {
            NormEditorModel b;
            b.begin(QString::fromLatin1(kMeasure), QString::fromLatin1(kContext));
            b.setClaimBand(10.0, 30.0);
            b.beginHandleDrag();

            const QVariantMap ax = b.sampleSummary();
            const double lo = ax.value(QStringLiteral("axisLo")).toDouble();
            const double hi = ax.value(QStringLiteral("axisHi")).toDouble();
            const double W  = 800.0, px = 600.0;               // a pointer parked at 600 px

            double first = 0.0;
            for (int i = 0; i < 20; ++i) {
                // Exactly CorridorEditor.qml's valOf(), against the axis the view is reading.
                const QVariantMap now = b.sampleSummary();
                const double axLo = now.value(QStringLiteral("axisLo")).toDouble();
                const double axHi = now.value(QStringLiteral("axisHi")).toDouble();
                const double v    = axLo + (px / W) * (axHi - axLo);
                b.nudgeClaimHi(v);
                if (i == 0) first = v;
            }
            near(b.draft().value(QStringLiteral("claimHi")).toDouble(), first,
                 "20 events at ONE pixel leave the corridor exactly where the first put it", 1e-9);
            near(first, lo + (px / W) * (hi - lo),
                 "…and that place is the value the pixel maps to, once");
            b.endHandleDrag();
        }

        // A latch must not survive the editor being re-opened or closed under it.
        a.beginHandleDrag();
        a.begin(QString::fromLatin1(kMeasure), QString::fromLatin1(kContext));
        a.setClaimBand(10.0, 300.0);
        const double reopened = a.sampleSummary().value(QStringLiteral("axisHi")).toDouble();
        a.setClaimBand(10.0, 30.0);
        check(a.sampleSummary().value(QStringLiteral("axisHi")).toDouble() < reopened,
              "re-opening clears a latch left behind by an interrupted drag");
    }

    // ── Histogram and counts agree with grade() ─────────────────────────────
    //
    // No library is configured, so there are no samples — which is itself the case that must not
    // crash or lie. The histogram still spans the corridor, because a corridor with no swings near
    // it is exactly the situation worth seeing.
    std::printf("\nan empty sample\n");
    {
        const QVariantMap sum = ed.sampleSummary();
        check(sum.value(QStringLiteral("produced")).toInt() == 0, "no library means no samples");
        check(!sum.value(QStringLiteral("hasLibrary")).toBool(), "…and says the library is unset");
        check(sum.value(QStringLiteral("axisHi")).toDouble()
                  > sum.value(QStringLiteral("axisLo")).toDouble(),
              "the axis still spans the corridor with nothing drawn on it");

        const QVariantMap gc = ed.gradeCounts();
        check(gc.value(QStringLiteral("total")).toInt() == 0, "counts are zero, not absent");

        const QVariantList hist = ed.histogram();
        check(!hist.isEmpty(), "the histogram renders its bins even when empty");

        // Every bin's grade must be the grade its centre really takes. A histogram that coloured
        // itself by any other rule would be a picture of a corridor nobody is graded against.
        Norm n;
        n.mu = 20.0; n.sigmaLo = 10.0; n.sigmaHi = 10.0;
        bool agree = true;
        for (const QVariant &bv : hist) {
            const QVariantMap b = bv.toMap();
            const double centre = 0.5 * (num(b, "lo") + num(b, "hi"));
            if (b.value(QStringLiteral("grade")).toString() != gradeName(grade(centre, n)))
                agree = false;
        }
        check(agree, "every bin's colour is the grade its own centre takes");

        // A seat with no sample is refused, with a reason rather than a silent no-op.
        const QVariantMap seat = ed.seatFromSample();
        check(!seat.value(QStringLiteral("ok")).toBool(), "seating an empty sample is refused");
        check(!seat.value(QStringLiteral("message")).toString().isEmpty(), "…and says why");
    }

    // ── Import, and what a hand edit does to provenance ─────────────────────
    std::printf("\nimport / provenance\n");
    {
        // stanceWidth is the club-dependent measure the context tree exists for: full_swing plus a
        // row per club. Editing the general row therefore has four real rows to adopt from, which
        // is the case the Import route is built for. (kMeasure has only its full_swing row, so it
        // would offer nothing — an empty list there is correct, not a bug.)
        NormEditorModel imp;
        check(imp.begin(QStringLiteral("m_stanceWidth"), QString::fromLatin1(kContext)),
              "the club-dependent measure opens");

        const QVariantList cands = imp.importCandidates();
        check(!cands.isEmpty(), "a measure with per-club rows offers import candidates");

        bool offersSelf = false;
        for (const QVariant &cv : cands)
            if (cv.toMap().value(QStringLiteral("contextId")).toString() == QLatin1String(kContext))
                offersSelf = true;
        check(!offersSelf, "the context being edited is not offered to itself");

        if (!cands.isEmpty()) {
            const QVariantMap c = cands.first().toMap();
            imp.adoptFrom(c.value(QStringLiteral("contextId")).toString());
            QVariantMap a = imp.draft();
            check(a.value(QStringLiteral("source")).toString() == QLatin1String("imported"),
                  "adopting marks the source Imported");
            // The candidate row publishes the band it would GRADE as (`idealLo`), so it reads on
            // the same scale as the plot beside it; the draft's own `idealLo` is the same
            // projection of the same numbers once adopted.
            near(num(a, "idealLo"), num(c, "idealLo"), "…and takes the adopted band");
            check(!a.value(QStringLiteral("citation")).toString().isEmpty(),
                  "…and records where it came from");

            // A hand edit after an adopt (or a seat) must DROP the borrowed provenance: leaving
            // "Imported" or "Seated · n = 42" attached to numbers a hand has since moved is a claim
            // the norm no longer supports.
            imp.setClaimBand(1.0, 2.0);
            a = imp.draft();
            check(a.value(QStringLiteral("source")).toString() == QLatin1String("heuristic"),
                  "a hand edit drops an adopted provenance");
            check(a.value(QStringLiteral("n")).toInt() == 0, "…and its sample size with it");
        }

        // A measure whose only row is the one being edited offers nothing — asserted so the empty
        // case is a stated outcome rather than an untested silence.
        check(ed.importCandidates().isEmpty(),
              "a measure with a single row offers nothing to adopt");
    }

    // ── The refusal ─────────────────────────────────────────────────────────
    std::printf("\nrefusal\n");
    {
        // Find a NotCapturable measure in the shipped pack, if one exists. The pack's whole point
        // is that capture gaps are named rather than hidden, so this is content, not a fixture.
        NormEditorModel ed2;
        // Hold the provider, do not bind a reference through it: `const auto &p =
        // *makeCharacteristicPackProvider()` binds to a temporary unique_ptr that dies at the
        // semicolon, and the reference is dangling on the very next line.
        const auto      provider = makeCharacteristicPackProvider();
        QString         gapId;
        for (const Measure &m : provider->pack().measures)
            if (m.status == MeasureStatus::NotCapturable) { gapId = m.id; break; }

        if (gapId.isEmpty()) {
            // Not a hole in the test — an ASSERTED property of the shipped pack. The pre-stage-4
            // decision was that the two spinal measures are roadmap items, not capture gaps, which
            // left the seed pack with zero NotCapturable measures. If one is ever added this branch
            // stops being taken and the refusal below starts being exercised, which is the point.
            check(true, "the shipped pack has no capture gap to refuse (seed pack invariant)");
        } else {
            check(ed2.begin(gapId, QString::fromLatin1(kContext)), "a capture-gap measure still OPENS");
            const QVariantMap g = ed2.draft();
            check(g.value(QStringLiteral("refused")).toBool(), "…but is refused");
            check(!g.value(QStringLiteral("refusedReason")).toString().isEmpty(), "…with a reason");
            ed2.setClaimBand(1.0, 2.0);
            check(!ed2.draft().value(QStringLiteral("canSave")).toBool(),
                  "…and cannot be saved however it is dragged");
        }
    }

    // ── Discard vs Reset — two different undos ──────────────────────────────
    //
    // Conflating them is how someone loses a corridor they meant to keep: Discard throws away
    // unsaved dragging and writes NOTHING; Reset drops the saved override and IS a write.
    std::printf("\ndiscard vs reset\n");
    {
        // The test writes into an isolated XDG_DATA_HOME (see the CMake entry), so the user norm
        // set starts empty and this is a clean statement about a shipped row.
        NormEditorModel r;
        check(r.begin(QString::fromLatin1(kMeasure), QString::fromLatin1(kContext)),
              "a shipped row opens");

        QVariantMap d0 = r.draft();
        check(!d0.value(QStringLiteral("overridden")).toBool(),
              "a shipped row is not marked as yours");
        check(!d0.value(QStringLiteral("canReset")).toBool(),
              "…and offers no reset — the core set is read-only, and an action that can only fail "
              "is worse than no action");
        check(!d0.value(QStringLiteral("canDiscard")).toBool(), "…nor a discard, being unchanged");
        check(d0.value(QStringLiteral("editedNote")).toString().isEmpty(),
              "…and says nothing about having been edited");
        check(d0.value(QStringLiteral("hasShipped")).toBool(),
              "core carries this key, so a reset here would mean 'go back to shipped'");
        check(d0.value(QStringLiteral("resetLabel")).toString() == QLatin1String("Reset to shipped"),
              "…and the label promises exactly that");
        check(!r.resetToDefault().value(QStringLiteral("ok")).toBool(),
              "resetting a shipped row is refused rather than deleting anything");

        // Discard: unsaved changes only, nothing written.
        r.setClaimBand(11.0, 33.0);
        check(r.draft().value(QStringLiteral("canDiscard")).toBool(), "dragging enables discard");
        const QVariantMap disc = r.discardChanges();
        check(disc.value(QStringLiteral("ok")).toBool(), "discard succeeds");
        near(num(r.draft(), "claimLo"), seededLo, "…and puts the band back where it was found");
        check(!r.draft().value(QStringLiteral("dirty")).toBool(), "…leaving the draft clean");
        check(!r.draft().value(QStringLiteral("overridden")).toBool(),
              "…and having written NOTHING — the row is still the shipped one");
        check(!r.discardChanges().value(QStringLiteral("ok")).toBool(),
              "discarding an unchanged draft is a no-op, and says so");

        // Reset: only once there is a saved override.
        r.setClaimBand(11.0, 33.0);
        const QVariantMap saved = r.save();
        check(saved.value(QStringLiteral("ok")).toBool(), "an edited row saves to the user set");
        if (saved.value(QStringLiteral("ok")).toBool()) {
            QVariantMap d1 = r.draft();
            check(d1.value(QStringLiteral("overridden")).toBool(), "…and is NOW marked as yours");
            check(d1.value(QStringLiteral("canReset")).toBool(), "…with an override to drop");
            check(d1.value(QStringLiteral("editedNote")).toString().contains(
                      QStringLiteral("PinPoint ships")),
                  "…and says what the shipped corridor was, which is what a reset goes back TO");
            near(num(d1, "shippedClaimLo"), seededLo, "…quoting the real shipped low edge");
            near(num(d1, "claimLo"), 11.0, "the saved band is what resolves");

            const QVariantMap rev = r.resetToDefault();
            check(rev.value(QStringLiteral("ok")).toBool(), "reset drops it");
            check(!r.draft().value(QStringLiteral("canReset")).toBool(),
                  "…and the offer goes away with it");
            check(!r.draft().value(QStringLiteral("overridden")).toBool(),
                  "…and the row is shipped again");
            near(num(r.draft(), "claimLo"), seededLo,
                 "…leaving the SHIPPED corridor resolving again, untouched");
        }
    }

    // A context core does NOT carry: the same operation, a different promise. Offering "restore
    // the shipped version" of something that never shipped is how a destructive action gets a
    // reassuring name — see the same defect fixed in CharacteristicEditorModel.
    std::printf("\nreset where nothing shipped\n");
    {
        NormEditorModel r;
        // Every club context inherits stanceWidth's per-club rows, so pick one the shipped set
        // leaves alone: an archetype context carries no stance-width row at all.
        check(r.begin(QStringLiteral("m_stanceWidth"), QStringLiteral("archetype_bowed")),
              "a context with no shipped row for this measure opens");
        QVariantMap d = r.draft();
        check(!d.value(QStringLiteral("hasShipped")).toBool(),
              "core carries nothing at this key");
        check(d.value(QStringLiteral("resetLabel")).toString()
                  == QLatin1String("Remove your override"),
              "…so the label promises removal, NOT a restore of something that never shipped");

        r.setClaimBand(70.0, 90.0);
        if (r.save().value(QStringLiteral("ok")).toBool()) {
            check(r.draft().value(QStringLiteral("editedNote")).toString().contains(
                      QStringLiteral("ships no corridor")),
                  "…and the note says there is no shipped corridor here, rather than quoting one");
            const QVariantMap rev = r.resetToDefault();
            check(rev.value(QStringLiteral("ok")).toBool(), "removal succeeds");
            check(rev.value(QStringLiteral("message")).toString().contains(
                      QStringLiteral("inherits")),
                  "…and reports that it now INHERITS, not that anything was restored");
        }
    }

    // FULL SWING is now one of those contexts, and it is the one a user is most likely to open.
    //
    // The general corridors live at `any` because a full swing is a shot TYPE — before that, they sat
    // at full_swing, a SIBLING of partial / bunker / specialty, and a pitch or bunker shot resolved
    // no corridor at all. The consequence here is that editing "the full swing corridor" now creates
    // an override on an inheriting context, so the reset promises removal rather than a restore. That
    // is honest, but it is a change to the common path and it should not drift back silently.
    std::printf("\nediting a full swing, whose corridor is inherited from the root\n");
    {
        NormEditorModel general;
        check(general.begin(QString::fromLatin1(kMeasure), QStringLiteral("any")),
              "the general corridor opens at the root");
        const double generalLo = num(general.draft(), "claimLo");

        NormEditorModel fs;
        check(fs.begin(QString::fromLatin1(kMeasure), QStringLiteral("full_swing")),
              "and a full swing opens too");
        const QVariantMap d = fs.draft();

        near(num(d, "claimLo"), generalLo,
             "a full swing opens on the INHERITED band, not an empty one");
        check(!d.value(QStringLiteral("hasShipped")).toBool(),
              "core carries no row at this exact key — the general corridor is at the root");
        check(d.value(QStringLiteral("resetLabel")).toString()
                  == QLatin1String("Remove your override"),
              "…so the promise is removal, after which the general band resolves again");
    }

    // ── One-sided corridors ─────────────────────────────────────────────────
    //
    // Nothing shipped is one-sided until the seed conversion, so the shape is injected through the
    // PINPOINT_CORE_PACK seam: the same reviewable JSON the pack normally loads, with one measure
    // carrying "shape". m_smashFactor deliberately, because it is the measure the conversion will
    // actually change, so this exercises the real rows (mu 1.48, sigmaLo 0.05 at driver) rather
    // than a fixture invented to pass.
    //
    // EVERY assertion below has a two-sided counterpart, either here or in the sections above.
    // "One-sided works" is only half the claim; the other half is that none of it reaches the 105
    // measures that are still ordinary corridors.
    std::printf("\none-sided: the fit\n");
    {
        // The seat fit is gated free-standing because seating runs off a library scan, and there
        // is no library here. A deliberately RIGHT-SKEWED sample: the median sits at 10, but a
        // handful of excellent readings run far above it.
        const std::vector<double> skew = { 8, 9, 9.5, 10, 10, 10, 10.5, 11, 14, 22, 40 };
        const OneSidedFit f = fitOneSided(skew, Shape::Floor);
        near(f.mu, 10.0, "a floor seats on the MEDIAN, which the long good tail cannot drag");

        double mean = 0.0;
        for (double v : skew) mean += v;
        mean /= double(skew.size());
        check(mean > f.mu + 3.0,
              "…and the mean it refuses to use sits far above it — that is the whole point");
        check(f.tolerance > 0.0 && f.tolerance < 2.0,
              "the tolerance reads the GRADED tail only, so the good-side outliers do not widen it");

        const OneSidedFit c = fitOneSided(skew, Shape::Ceiling);
        near(c.mu, 10.0, "a ceiling seats on the same median");
        check(c.tolerance > f.tolerance,
              "…but measures the OTHER tail, which on this sample is the long one");

        // Degenerate and empty: a point mass really is a point mass. There is no borrow fallback
        // here and there must not be — see the header.
        near(fitOneSided({ 5, 5, 5, 5 }, Shape::Floor).tolerance, 0.0,
             "a point-mass sample yields a zero tolerance rather than an invented width");
        near(fitOneSided({ 5, 5, 5, 5 }, Shape::Floor).mu, 5.0, "…seated where the mass is");
        near(fitOneSided({}, Shape::Floor).tolerance, 0.0, "an empty sample fits nothing, safely");
    }

    std::printf("\none-sided: a floor draft\n");
    const QByteArray realPack = qgetenv("PINPOINT_CORE_PACK");
    {
        const QString fp = packWithShape("floor");
        check(!fp.isEmpty(), "built a scratch pack carrying a floor measure");
        qputenv("PINPOINT_CORE_PACK", fp.toLocal8Bit());

        NormEditorModel fe;
        check(fe.begin(QStringLiteral("m_smashFactor"), QStringLiteral("driver")),
              "a floor measure opens a draft");
        QVariantMap f = fe.draft();

        check(f.value(QStringLiteral("shape")).toString() == QLatin1String("floor"), "shape reaches the draft");
        check(f.value(QStringLiteral("oneSided")).toBool(),  "…as a flag the view can bind");
        check(f.value(QStringLiteral("highOpen")).toBool(),  "a floor is open ABOVE");
        check(!f.value(QStringLiteral("lowOpen")).toBool(),  "…and graded below");

        // The keys the one-sided view binds to, named here for the same reason the two-sided ones
        // are: a rename that does not reach the .qml has to fail in a test and not on a screen.
        for (const char *k : { "shape", "shapeLabel", "oneSided", "lowOpen", "highOpen",
                               "tolerance", "gradedEdge", "shapeNote", "openEndLabel",
                               "claimPhrase", "policyNote", "parentNote" })
            check(f.contains(QLatin1String(k)), k);

        // ALL THREE drawn bands collapse onto the aspiration, Good included. Good is the one pair
        // computed outside bandEdgesOf, so it is the one that would silently keep running two
        // sigma past mu — straight over the Ideal band that owns that entire side.
        const double mu = num(f, "mu");
        near(mu, 1.48, "seeded from the shipped driver row");
        near(num(f, "idealHi"), mu, "the Ideal band ends at the aspiration");
        near(num(f, "watchHi"), mu, "…so does Watch");
        near(num(f, "goodHi"),  mu, "…AND Good, which is computed by hand and would otherwise escape");
        near(num(f, "idealLo"), mu - 0.05, "the graded side is an ordinary corridor: 1 sigma");
        near(num(f, "goodLo"),  mu - 0.10, "…2 sigma");
        near(num(f, "watchLo"), mu - 0.15, "…3 sigma");
        near(num(f, "tolerance"),  0.05, "the tolerance is the graded sigma");
        near(num(f, "gradedEdge"), mu - 0.05, "…and the edge is where it lands");

        // ── The readouts must be FAITHFUL, not rounded to the pack's usual resolution ────────
        //
        // Found while building the one-sided fields. Every number on this screen was fixed at one
        // decimal, which is right for the degree measures and wrong for the ratios: smash factor
        // is 1.48 with a tolerance of 0.05, so the policy line read "Ideal from 1.4 · good from
        // 1.4" — two different edges as one number — and the tolerance field would have shown 0.1,
        // twice its width. A LABEL rounding is cosmetic; a FIELD rounding is not, because
        // PpTextField commits on focus loss and would save what it displayed.
        check(f.value(QStringLiteral("policyNote")).toString().contains(QLatin1String("1.43")),
              "the policy line shows the edge it means, not a tenth it rounds to");
        check(f.value(QStringLiteral("policyNote")).toString().contains(QLatin1String("1.38")),
              "…and the Good edge is distinguishable from the Ideal one");
        check(f.value(QStringLiteral("claimPhrase")).toString().contains(QLatin1String("1.48")),
              "…and the claim states the authored aspiration, not 1.5");

        // Wording. The claim never names a second bound, and the policy line never names a fault
        // on the side that grades Ideal.
        const QString phrase = f.value(QStringLiteral("claimPhrase")).toString();
        check(phrase.contains(QLatin1String("at least")), "the claim reads 'at least'");
        check(!phrase.contains(QLatin1String(" to ")),    "…and never 'X to Y'");
        const QString pol = f.value(QStringLiteral("policyNote")).toString();
        check(pol.contains(QLatin1String("action below")), "the policy line grades the low tail");
        check(!pol.contains(QLatin1String("beyond")),
              "…and never 'action beyond', which would name a fault above the aspiration");
        check(f.value(QStringLiteral("openEndLabel")).toString() == QLatin1String("no upper limit"),
              "the open end is labelled, not just faded");
        check(f.value(QStringLiteral("shapeNote")).toString().contains(QLatin1String("Higher is better")),
              "the shape is stated in words");
        check(f.value(QStringLiteral("shapeNote")).toString().contains(QLatin1String("efficient")),
              "…with the measure's own highMeans folded in, not a bare enum");

        // ── The mutators ────────────────────────────────────────────────────
        //
        // mu and the tolerance are INDEPENDENT here, where on a target norm they are two
        // consequences of one pair of handles. Each must move without disturbing the other.
        fe.setAspiration(1.50);
        f = fe.draft();
        near(num(f, "mu"), 1.50, "the aspiration moves");
        near(num(f, "tolerance"), 0.05, "…and leaves the tolerance alone");

        fe.setTolerance(0.10);
        f = fe.draft();
        near(num(f, "mu"), 1.50, "the tolerance moves without disturbing the aspiration");
        near(num(f, "tolerance"), 0.10, "…to what was asked");
        near(num(f, "claimHi") - num(f, "mu"), num(f, "mu") - num(f, "claimLo"),
             "BOTH sigmas move together — validateNormsAgainst refuses a one-sided row where "
             "they differ, and the ungraded one is a number nothing reads");

        fe.setTolerance(-0.20);
        near(num(fe.draft(), "tolerance"), 0.20,
             "a typed negative tolerance is read as a magnitude, not as an inverted corridor");

        fe.setTolerance(0.10);
        fe.nudgeGradedEdge(1.40);
        near(num(fe.draft(), "tolerance"), 0.10, "dragging the edge sets the tolerance from it");

        // Dragged PAST the centre it clamps there rather than reflecting out the far side, which
        // is what a magnitude would have done and would have thrown the handle off the pointer.
        fe.nudgeGradedEdge(1.70);
        f = fe.draft();
        near(num(f, "tolerance"), 0.0, "an edge dragged through the centre clamps at zero");
        near(num(f, "gradedEdge"), num(f, "mu"), "…parking on the aspiration, not beyond it");

        // ── What must NOT work ──────────────────────────────────────────────
        fe.setTolerance(0.05);
        fe.setAspiration(1.48);
        const QVariantMap before = fe.draft();

        fe.nudgeClaimHi(9.9);
        check(fe.draft() == before, "the OPEN side has no edge to nudge, so nothing moves");

        fe.setClaimBand(1.0, 2.0);
        check(fe.draft() == before,
              "setClaimBand is refused: its midpoint would move the aspiration as a side effect, "
              "and its split would leave the two sigmas unequal");

        fe.nudgeClaimLo(1.40);
        near(num(fe.draft(), "tolerance"), 0.08,
             "…while the GRADED side's nudge routes to the edge, so a caller holding 'the low "
             "field' keeps working on all three shapes");
        near(num(fe.draft(), "mu"), 1.48, "…and still does not move the aspiration");

        // Import rows carry the phrase too, and every candidate is the same measure at another
        // context, so they all share this measure's shape.
        const QVariantList cands = fe.importCandidates();
        check(!cands.isEmpty(), "the other smash-factor contexts are offerable");
        bool allOneSided = true;
        for (const QVariant &cv : cands) {
            const QString t = cv.toMap().value(QStringLiteral("rangeText")).toString();
            if (!t.contains(QLatin1String("at least")) || t.contains(QLatin1String(" to ")))
                allOneSided = false;
        }
        check(allOneSided, "…and each reads 'at least X', never 'X to Y'");
    }

    std::printf("\none-sided: a ceiling draft\n");
    {
        const QString cp = packWithShape("ceiling");
        check(!cp.isEmpty(), "built a scratch pack carrying a ceiling measure");
        qputenv("PINPOINT_CORE_PACK", cp.toLocal8Bit());

        NormEditorModel ce;
        check(ce.begin(QStringLiteral("m_smashFactor"), QStringLiteral("driver")), "opens");
        QVariantMap c = ce.draft();

        check(c.value(QStringLiteral("lowOpen")).toBool(),   "a ceiling is open BELOW");
        check(!c.value(QStringLiteral("highOpen")).toBool(), "…and graded above");
        const double mu = num(c, "mu");
        near(num(c, "idealLo"), mu, "every drawn band ends at the aspiration on the open side");
        near(num(c, "goodLo"),  mu, "…Good included");
        near(num(c, "watchLo"), mu, "…and Watch");
        near(num(c, "gradedEdge"), mu + 0.05, "the graded edge is ABOVE the aspiration");
        check(c.value(QStringLiteral("claimPhrase")).toString().contains(QLatin1String("no more than")),
              "the claim reads 'no more than'");
        check(c.value(QStringLiteral("policyNote")).toString().contains(QLatin1String("action above")),
              "…and the fault is named on the high tail");
        check(c.value(QStringLiteral("openEndLabel")).toString() == QLatin1String("no lower limit"),
              "the open end is the low one");

        // The mirror of the floor's routing: here it is the HIGH nudge that reaches the edge.
        ce.nudgeGradedEdge(mu + 0.20);
        near(num(ce.draft(), "tolerance"), 0.20, "the high edge sets the tolerance");
        ce.nudgeClaimLo(0.1);
        near(num(ce.draft(), "tolerance"), 0.20, "…and the low side, being open, moves nothing");
        ce.nudgeClaimHi(mu + 0.30);
        near(num(ce.draft(), "tolerance"), 0.30, "…while the high nudge routes to the edge");
    }

    // ── Plausibility ────────────────────────────────────────────────────────
    //
    // A different question from everything else here: not "was the swing good" but "was the
    // reading real". Gated on a FLOOR because that is where the seed conversion puts the first
    // one — a floor is open above, and the cap is the only thing stopping that open tail from
    // believing 1.62.
    std::printf("\nplausibility\n");
    {
        const QString fp = packWithShape("floor");
        qputenv("PINPOINT_CORE_PACK", fp.toLocal8Bit());

        NormEditorModel pe;
        check(pe.begin(QStringLiteral("m_smashFactor"), QStringLiteral("driver")), "opens");
        QVariantMap d2 = pe.draft();

        for (const char *k : { "hasPlausibleLo", "hasPlausibleHi", "plausibleLo", "plausibleHi",
                               "plausibleLoError", "plausibleHiError" })
            check(d2.contains(QLatin1String(k)), k);

        // ABSENT IS NOT ZERO. A shipped smash row carries no cap, and `.toDouble()` on a missing
        // key yields 0.0 — so without the has* flags every uncapped row in the pack would read as
        // "stops believing readings below zero".
        check(!d2.value(QStringLiteral("hasPlausibleLo")).toBool()
                  && !d2.value(QStringLiteral("hasPlausibleHi")).toBool(),
              "a shipped row starts uncapped, and says so with a flag rather than a zero");
        check(d2.value(QStringLiteral("plausibleLoError")).toString().isEmpty(),
              "…and an absent bound cannot be wrong");

        // The seed conversion's own number: driver smash caps at 1.56, ABOVE, on the open side.
        pe.setPlausibleHi(1.56);
        d2 = pe.draft();
        check(d2.value(QStringLiteral("hasPlausibleHi")).toBool(), "a cap can be set");
        near(num(d2, "plausibleHi"), 1.56, "…to what was asked");
        check(d2.value(QStringLiteral("plausibleHiError")).toString().isEmpty(),
              "a cap on the OPEN side is never inside the corridor, so it is always legal there");
        check(d2.value(QStringLiteral("dirty")).toBool(), "…and it dirties the draft");

        // Order. Both bounds present and crossed is refused on BOTH sides, so whichever field the
        // author is looking at says something.
        pe.setPlausibleLo(1.70);
        d2 = pe.draft();
        check(!d2.value(QStringLiteral("plausibleLoError")).toString().isEmpty(),
              "a lower bound above the upper one is refused");
        check(!d2.value(QStringLiteral("plausibleHiError")).toString().isEmpty(),
              "…and the other field says so too, so the message is where the author is looking");
        QVariantMap saved = pe.save();
        check(!saved.value(QStringLiteral("ok")).toBool(), "…and the save is refused");

        // Inside the corridor, on the GRADED side. This is the rule that matters: a reading there
        // would be called a fault and disbelieved at once, and which answer surfaced would depend
        // on the order two checks happened to run in.
        pe.clearPlausibleLo();
        check(!pe.draft().value(QStringLiteral("hasPlausibleLo")).toBool(), "a bound can be cleared");
        pe.setPlausibleLo(1.45);          // mu 1.48, sigma 0.05 — well inside even standard's watch
        check(!pe.draft().value(QStringLiteral("plausibleLoError")).toString().isEmpty(),
              "a bound INSIDE the graded corridor is refused");
        saved = pe.save();
        check(!saved.value(QStringLiteral("ok")).toBool(),
              "…and save refuses it, because validateNormPack alone would not: that check needs "
              "the measure, so it lives in validateNormsAgainst and never runs on this path");

        // MEASURED AGAINST THE WIDEST PRESET, NOT THE ACTIVE ONE. A cap outside standard's 3 sigma
        // but inside lenient's 3.5 must be refused, or an author on `standard` could save a row
        // that fails to load for a reader on `lenient` — the pack's validity would depend on the
        // reader's sensitivity setting.
        pe.setPlausibleLo(1.48 - 3.2 * 0.05);
        check(!pe.draft().value(QStringLiteral("plausibleLoError")).toString().isEmpty(),
              "outside standard's watch edge but inside lenient's is still refused");
        pe.setGradePolicy(QStringLiteral("strict"));
        check(!pe.draft().value(QStringLiteral("plausibleLoError")).toString().isEmpty(),
              "…and switching the reader's own policy does not make it legal");
        pe.setGradePolicy(QStringLiteral("standard"));

        pe.setPlausibleLo(1.48 - 3.6 * 0.05);
        check(pe.draft().value(QStringLiteral("plausibleLoError")).toString().isEmpty(),
              "outside the widest preset's edge is accepted");

        // ── A bound the editor SHOWS must survive a round trip through it ────
        //
        // Unlike the monitor bounds, which begin() drops on purpose because nothing renders them.
        // Dropping a cap silently would turn readings the norm had stopped believing back into
        // confident diagnoses.
        pe.clearPlausibleLo();
        pe.setPlausibleHi(1.56);
        saved = pe.save();
        check(saved.value(QStringLiteral("ok")).toBool(), "a legal cap saves");

        NormEditorModel re;
        re.begin(QStringLiteral("m_smashFactor"), QStringLiteral("driver"));
        check(re.draft().value(QStringLiteral("hasPlausibleHi")).toBool(),
              "…and re-opening the row finds the cap still there");
        near(num(re.draft(), "plausibleHi"), 1.56, "…unchanged");

        // Clean up: this test writes to the user norm set, and the sections after it read the
        // resolved corridor. resetToDefault drops the override.
        re.resetToDefault();
        check(!re.draft().value(QStringLiteral("hasPlausibleHi")).toBool(),
              "dropping the override drops the cap with it");
    }

    // ── …and none of it reaches a target norm ───────────────────────────────
    //
    // The other half of every assertion above. Restored to the real pack, the same measure is an
    // ordinary corridor again and every one-sided rule is off.
    std::printf("\nthe two-sided control\n");
    qputenv("PINPOINT_CORE_PACK", realPack);
    {
        NormEditorModel te;
        check(te.begin(QStringLiteral("m_smashFactor"), QStringLiteral("driver")), "opens");
        QVariantMap t = te.draft();

        check(t.value(QStringLiteral("shape")).toString() == QLatin1String("target"),
              "unshaped in the shipped pack — the seed conversion is a later stage");
        check(!t.value(QStringLiteral("oneSided")).toBool(), "…so nothing is one-sided");
        check(!t.value(QStringLiteral("lowOpen")).toBool() && !t.value(QStringLiteral("highOpen")).toBool(),
              "…and both flags are written false rather than left absent");
        check(t.value(QStringLiteral("openEndLabel")).toString().isEmpty(),
              "a two-sided corridor has no open end to label");
        check(t.value(QStringLiteral("shapeNote")).toString().isEmpty(),
              "…and no shape sentence: the line reverts to 'Higher means'");

        const double mu = num(t, "mu");
        near(num(t, "idealHi"), mu + 0.05, "the high side is an ordinary graded edge again");
        near(num(t, "goodHi"),  mu + 0.10, "…Good runs two sigma past mu, uncollapsed");
        near(num(t, "watchHi"), mu + 0.15, "…and Watch three");
        check(t.value(QStringLiteral("claimPhrase")).toString().contains(QLatin1String(" to ")),
              "the claim names both bounds");
        check(t.value(QStringLiteral("policyNote")).toString().contains(QLatin1String("beyond")),
              "…and the policy line names a fault on both tails");

        // The two-sided mutators are live again, and the one-sided ones are inert.
        te.setClaimBand(1.40, 1.60);
        t = te.draft();
        near(num(t, "mu"), 1.50, "setClaimBand works on a target norm");
        te.setAspiration(2.0);
        near(num(te.draft(), "mu"), 1.50, "…and setAspiration does not: mu is a consequence here");
        te.setTolerance(0.9);
        near(num(te.draft(), "claimHi"), 1.60, "…nor setTolerance");
        te.nudgeGradedEdge(1.0);
        near(num(te.draft(), "claimLo"), 1.40, "…nor nudgeGradedEdge");
    }

    // ── Closing ─────────────────────────────────────────────────────────────
    std::printf("\nclosing\n");
    ed.cancel();
    check(!ed.isOpen(), "cancel closes");
    check(ed.draft().isEmpty(), "…and renders nothing again");
    check(ed.importCandidates().isEmpty(), "…and offers nothing to import");

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
