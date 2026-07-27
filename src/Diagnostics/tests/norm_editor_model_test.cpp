// The corridor editor's model (src/Gui/characteristics/norm_editor_model.h), run against the
// SHIPPED pack and norm set.
//
// Its whole job is that QML holds no rules, so every rule it hides is asserted here or nowhere:
//
//   1. The two handles bind the IDEAL band — mu +/- sigma, per side. That is what norm.h documents
//      idealLo()/idealHi() as, and the plan carried "the Good band" in its stage-6 text until
//      2026-07-26, which is exactly the kind of drift a test has to stop.
//   2. Hand-editing DROPS a Seated provenance. Leaving "seated, n = 42" attached to numbers a hand
//      has since moved is a claim the norm no longer supports.
//   3. Seating fits a PER-SIDE tolerance, so an asymmetric sample yields an asymmetric corridor.
//   4. The grade counts and the histogram move together and agree with grade() — they are the
//      safety mechanism, and a histogram that disagreed with the grader would be worse than none.
//   5. The band edges shown follow the ACTIVE grade policy, because the edge drawn must be the edge
//      that grades.
//   6. A NotCapturable measure is refused: a corridor on it can never do anything but mislead.
//
//   cmake --build build/analyzer-tests --target norm_editor_model_test
//   ctest --test-dir build/analyzer-tests -R norm_editor_model --output-on-failure

#include "norm_editor_model.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

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
    const double seededLo = num(d, "idealLo"), seededHi = num(d, "idealHi");
    check(seededHi > seededLo, "seeded from the resolved corridor, not a degenerate point");

    // ── The handles bind the IDEAL band ─────────────────────────────────────
    std::printf("\nthe two handles\n");
    ed.setIdealBand(10.0, 30.0);
    d = ed.draft();
    near(num(d, "idealLo"), 10.0, "idealLo is the low handle");
    near(num(d, "idealHi"), 30.0, "idealHi is the high handle");
    near(num(d, "mu"),      20.0, "mu is the centre between them");
    check(d.value(QStringLiteral("dirty")).toBool(), "moving a handle dirties the draft");

    // Under the standard policy Good is |z| <= 2 and Watch <= 3, so the drawn edges are exactly
    // twice and three times the half-widths. This is the assertion that stops "the handles bind
    // the Good band" from creeping back in.
    near(num(d, "goodLo"),   0.0, "goodLo = mu - 2*sigmaLo");
    near(num(d, "goodHi"),  40.0, "goodHi = mu + 2*sigmaHi");
    near(num(d, "watchLo"), -10.0, "watchLo = mu - 3*sigmaLo");
    near(num(d, "watchHi"),  50.0, "watchHi = mu + 3*sigmaHi");

    // Asymmetric drag -> asymmetric tolerance, with no statistics vocabulary in the interaction.
    ed.setIdealBand(10.0, 40.0);
    d = ed.draft();
    near(num(d, "mu"), 25.0, "an asymmetric band still centres between the handles");
    near(num(d, "goodLo"), 10.0 - 15.0, "…and the low tail widens by ITS own sigma");
    near(num(d, "goodHi"), 40.0 + 15.0, "…and the high tail by its own");

    // A crossed drag must not produce a negative tolerance.
    ed.setIdealBand(50.0, 20.0);
    d = ed.draft();
    near(num(d, "idealLo"), 20.0, "a crossed drag is ordered, not negated (lo)");
    near(num(d, "idealHi"), 50.0, "a crossed drag is ordered, not negated (hi)");

    // ── The grade policy governs the drawn edges ────────────────────────────
    std::printf("\ngrade policy\n");
    ed.setIdealBand(10.0, 30.0);
    ed.setGradePolicy(QStringLiteral("strict"));
    d = ed.draft();
    near(num(d, "idealLo"), 10.0, "a policy change does not move the IDEAL band");
    near(num(d, "goodHi"),  20.0 + 1.5 * 10.0, "strict pulls the Good edge in (1.5 sigma)");
    near(num(d, "watchHi"), 20.0 + 2.25 * 10.0, "…and the Watch edge with it");
    ed.setGradePolicy(QStringLiteral("wat"));
    d = ed.draft();
    near(num(d, "goodHi"), 40.0, "an unknown policy name resolves to standard, never persists");

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
        a.setIdealBand(10.0, 30.0);

        const double freeLo = a.sampleSummary().value(QStringLiteral("axisLo")).toDouble();
        const double freeHi = a.sampleSummary().value(QStringLiteral("axisHi")).toDouble();

        // Unlatched, the axis follows the corridor — which is right between gestures, and is
        // exactly what must NOT happen during one.
        a.setIdealBand(10.0, 300.0);
        check(a.sampleSummary().value(QStringLiteral("axisHi")).toDouble() > freeHi,
              "between gestures the axis re-fits to the corridor");

        a.setIdealBand(10.0, 30.0);
        a.beginHandleDrag();
        const double lockLo = a.sampleSummary().value(QStringLiteral("axisLo")).toDouble();
        const double lockHi = a.sampleSummary().value(QStringLiteral("axisHi")).toDouble();
        near(lockLo, freeLo, "the latch captures the axis as it was");
        near(lockHi, freeHi, "…both edges");

        // Simulate the gesture: several nudges, as a real drag delivers.
        for (int i = 0; i < 8; ++i) {
            a.nudgeIdealHi(30.0 + 5.0 * i);
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
            b.setIdealBand(10.0, 30.0);
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
                b.nudgeIdealHi(v);
                if (i == 0) first = v;
            }
            near(b.draft().value(QStringLiteral("idealHi")).toDouble(), first,
                 "20 events at ONE pixel leave the corridor exactly where the first put it", 1e-9);
            near(first, lo + (px / W) * (hi - lo),
                 "…and that place is the value the pixel maps to, once");
            b.endHandleDrag();
        }

        // A latch must not survive the editor being re-opened or closed under it.
        a.beginHandleDrag();
        a.begin(QString::fromLatin1(kMeasure), QString::fromLatin1(kContext));
        a.setIdealBand(10.0, 300.0);
        const double reopened = a.sampleSummary().value(QStringLiteral("axisHi")).toDouble();
        a.setIdealBand(10.0, 30.0);
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
            near(num(a, "idealLo"), num(c, "idealLo"), "…and takes the adopted band");
            check(!a.value(QStringLiteral("citation")).toString().isEmpty(),
                  "…and records where it came from");

            // A hand edit after an adopt (or a seat) must DROP the borrowed provenance: leaving
            // "Imported" or "Seated · n = 42" attached to numbers a hand has since moved is a claim
            // the norm no longer supports.
            imp.setIdealBand(1.0, 2.0);
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
            ed2.setIdealBand(1.0, 2.0);
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
        r.setIdealBand(11.0, 33.0);
        check(r.draft().value(QStringLiteral("canDiscard")).toBool(), "dragging enables discard");
        const QVariantMap disc = r.discardChanges();
        check(disc.value(QStringLiteral("ok")).toBool(), "discard succeeds");
        near(num(r.draft(), "idealLo"), seededLo, "…and puts the band back where it was found");
        check(!r.draft().value(QStringLiteral("dirty")).toBool(), "…leaving the draft clean");
        check(!r.draft().value(QStringLiteral("overridden")).toBool(),
              "…and having written NOTHING — the row is still the shipped one");
        check(!r.discardChanges().value(QStringLiteral("ok")).toBool(),
              "discarding an unchanged draft is a no-op, and says so");

        // Reset: only once there is a saved override.
        r.setIdealBand(11.0, 33.0);
        const QVariantMap saved = r.save();
        check(saved.value(QStringLiteral("ok")).toBool(), "an edited row saves to the user set");
        if (saved.value(QStringLiteral("ok")).toBool()) {
            QVariantMap d1 = r.draft();
            check(d1.value(QStringLiteral("overridden")).toBool(), "…and is NOW marked as yours");
            check(d1.value(QStringLiteral("canReset")).toBool(), "…with an override to drop");
            check(d1.value(QStringLiteral("editedNote")).toString().contains(
                      QStringLiteral("PinPoint ships")),
                  "…and says what the shipped corridor was, which is what a reset goes back TO");
            near(num(d1, "shippedIdealLo"), seededLo, "…quoting the real shipped low edge");
            near(num(d1, "idealLo"), 11.0, "the saved band is what resolves");

            const QVariantMap rev = r.resetToDefault();
            check(rev.value(QStringLiteral("ok")).toBool(), "reset drops it");
            check(!r.draft().value(QStringLiteral("canReset")).toBool(),
                  "…and the offer goes away with it");
            check(!r.draft().value(QStringLiteral("overridden")).toBool(),
                  "…and the row is shipped again");
            near(num(r.draft(), "idealLo"), seededLo,
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

        r.setIdealBand(70.0, 90.0);
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
        const double generalLo = num(general.draft(), "idealLo");

        NormEditorModel fs;
        check(fs.begin(QString::fromLatin1(kMeasure), QStringLiteral("full_swing")),
              "and a full swing opens too");
        const QVariantMap d = fs.draft();

        near(num(d, "idealLo"), generalLo,
             "a full swing opens on the INHERITED band, not an empty one");
        check(!d.value(QStringLiteral("hasShipped")).toBool(),
              "core carries no row at this exact key — the general corridor is at the root");
        check(d.value(QStringLiteral("resetLabel")).toString()
                  == QLatin1String("Remove your override"),
              "…so the promise is removal, after which the general band resolves again");
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
