// Standalone tests for the authoring round-trip
// (src/Gui/characteristics/characteristic_editor_model.*).
//
// Covers what a user actually does: mint a measure, attach it, pick causes, save, and see the
// library change. Plus the two things that would quietly lose work — that an override never touches
// the shipped pack, and that removing a cause actually removes it rather than leaving the core edge
// alive underneath.
//
// QStandardPaths test mode redirects AppDataLocation, so this never writes to the real profile.
//
//   cmake --build build/analyzer-tests --target characteristic_editor_test
//   ctest --test-dir build/analyzer-tests -R characteristic_editor --output-on-failure

#include "../../Gui/characteristics/characteristic_editor_model.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static QVariantMap facets(const char *what, const char *quantity, const char *reference,
                          const char *reducer = "at", const char *anchor = "p1")
{
    QVariantMap f;
    f.insert(QStringLiteral("what"), QString::fromLatin1(what));
    f.insert(QStringLiteral("quantity"), QString::fromLatin1(quantity));
    f.insert(QStringLiteral("reference"), QString::fromLatin1(reference));
    f.insert(QStringLiteral("reducerKind"), QString::fromLatin1(reducer));
    f.insert(QStringLiteral("anchor"), QString::fromLatin1(anchor));
    f.insert(QStringLiteral("windowStart"), QString::fromLatin1("p1"));
    f.insert(QStringLiteral("windowEnd"), QString::fromLatin1("p7"));
    return f;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("PinPointTest"));
    QCoreApplication::setApplicationName(QStringLiteral("DiagnosticsEditorTest"));
    QStandardPaths::setTestModeEnabled(true);

    // Start from a clean slate so a previous run cannot mask a regression.
    QFile::remove(userPackPath());

    std::printf("characteristic_editor_test\n");

    // ── The picker vocabulary is gated, never free-form ─────────────────────────
    {
        CharacteristicEditorModel ed;

        check(!ed.anatomyGroups().isEmpty(), "the anatomy vocabulary is offered as grouped chips");
        check(!ed.phases().isEmpty(), "P-positions are offered");
        check(ed.reducerKinds().size() == 4, "all four reducers are offered");

        // A point has no angle, so the picker must not offer one.
        const QVariantList kneeQ = ed.quantitiesFor(QStringLiteral("leadKnee"));
        bool               kneeAngle = false;
        for (const QVariant &v : kneeQ)
            if (v.toMap().value(QStringLiteral("name")) == QStringLiteral("angle")) kneeAngle = true;
        check(!kneeAngle, "the picker does not offer an angle on a point");

        const QVariantList shinQ = ed.quantitiesFor(QStringLiteral("leadShin"));
        bool               shinAngle = false;
        for (const QVariant &v : shinQ)
            if (v.toMap().value(QStringLiteral("name")) == QStringLiteral("angle")) shinAngle = true;
        check(shinAngle, "it does offer an angle on a segment");

        check(!ed.referencesFor(QStringLiteral("spine"), QStringLiteral("angle")).isEmpty(),
              "references are offered for a legal subject/quantity pair");
    }

    // ── A phrase SEEDS facets; it is not a query ────────────────────────────────
    {
        CharacteristicEditorModel ed;

        const QVariantMap seed = ed.seedFacetsFromPhrase(QStringLiteral("pelvis sway at p4"));
        check(seed.value(QStringLiteral("what")) == QStringLiteral("pelvisCentre"),
              "a phrase seeds the subject");
        check(seed.value(QStringLiteral("quantity")) == QStringLiteral("distance"),
              "a phrase seeds the quantity");
        check(seed.value(QStringLiteral("anchor")) == QStringLiteral("p4"),
              "a P-position in the phrase seeds the reducer anchor");

        // Longest match wins, so a two-word role beats its first word.
        const QVariantMap shoulder = ed.seedFacetsFromPhrase(QStringLiteral("shoulder angle to ground"));
        check(shoulder.value(QStringLiteral("what")) == QStringLiteral("shoulderLine"),
              "the longest matching role wins");
        check(shoulder.value(QStringLiteral("reference")) == QStringLiteral("ground"),
              "a phrase seeds the reference");

        // A phrase that matches nothing yields nothing — better than a wrong guess.
        check(ed.seedFacetsFromPhrase(QStringLiteral("zzzz")).isEmpty(),
              "an unrecognised phrase seeds nothing rather than guessing");
    }

    // ── Preview: validity, naming, and duplicate detection ──────────────────────
    {
        CharacteristicEditorModel ed;

        const QVariantMap bad = ed.previewMeasure(facets("leadKnee", "angle", "ground"));
        check(bad.value(QStringLiteral("valid")).toBool() == false, "an illegal combination previews as invalid");
        check(!bad.value(QStringLiteral("reason")).toString().isEmpty(),
              "and says why, in the author's terms");

        const QVariantMap good = ed.previewMeasure(facets("spine", "angle", "ground", "at", "p4"));
        check(good.value(QStringLiteral("valid")).toBool(), "a legal combination previews as valid");
        check(good.value(QStringLiteral("label")).toString().contains(QStringLiteral("P4")),
              "the generated name speaks in P-positions");

        // A role no sensor can resolve must be flagged BEFORE the author builds on it.
        const QVariantMap gap = ed.previewMeasure(facets("thoracicSegment", "angle", "ground"));
        check(gap.value(QStringLiteral("status")) == QStringLiteral("notCapturable"),
              "a spinal-region measure previews as a capture gap");
        check(!gap.value(QStringLiteral("gapReason")).toString().isEmpty(),
              "the capture gap explains itself");

        // The seed pack already carries pelvis-lateral measures, so a near-miss must be caught at
        // the moment of creation — afterwards nobody merges duplicates.
        const QVariantMap dup = ed.previewMeasure(facets("thoraxCentre", "distance", "trailAnkle",
                                                         "delta", "p1"));
        check(!dup.value(QStringLiteral("nearDuplicates")).toList().isEmpty(),
              "a near-duplicate series is surfaced at creation time");
    }

    // ── Authoring round-trip ────────────────────────────────────────────────────
    QString newId;
    {
        CharacteristicEditorModel ed;
        ed.beginNew();
        check(ed.editing() && ed.isNew(), "beginNew opens a draft");
        check(!ed.dirty(), "a fresh draft is not dirty");

        ed.setLabel(QStringLiteral("Test lateral drift"));
        check(ed.dirty(), "an edit marks the draft dirty");
        ed.setGroup(QStringLiteral("lateral"));
        ed.setConsequence(QStringLiteral("It moves the low point behind the ball."));

        const QString measureId = ed.mintMeasure(facets("thoraxCentre", "distance", "leadAnkle",
                                                        "delta", "p1"));
        check(!measureId.isEmpty(), "a stub measure mints without blocking the author");

        const QString sigId = ed.attachMeasure(measureId, QStringLiteral("high"));
        check(!sigId.isEmpty(), "the measure attaches as a signal");

        ed.addCause(QStringLiteral("poor_core_stability"), QStringLiteral("strong"));
        ed.addCause(QStringLiteral("limited_lead_hip_ir"), QStringLiteral("moderate"));

        const QVariantMap draft = ed.draft();
        check(draft.value(QStringLiteral("signals")).toList().size() == 1, "the draft carries its signal");
        check(draft.value(QStringLiteral("causes")).toList().size() == 2, "the draft carries both causes");

        const QVariantMap res = ed.save();
        if (!res.value(QStringLiteral("ok")).toBool())
            std::printf("        save failed: %s\n",
                        qPrintable(res.value(QStringLiteral("message")).toString()));
        check(res.value(QStringLiteral("ok")).toBool(), "the draft saves");
        check(!ed.dirty() && !ed.editing(), "saving closes the draft");

        newId = QStringLiteral("test_lateral_drift");
        check(QFile::exists(userPackPath()), "a user pack file is written");
    }

    // ── It survives a reload, and the shipped pack is untouched ─────────────────
    {
        CharacteristicEditorModel ed;
        check(ed.beginEdit(newId), "the saved characteristic is in the library after a reload");

        const QVariantMap d = ed.draft();
        check(d.value(QStringLiteral("label")) == QStringLiteral("Test lateral drift"),
              "its label round-trips");
        check(d.value(QStringLiteral("causes")).toList().size() == 2, "its causes round-trip");
        check(d.value(QStringLiteral("signals")).toList().size() == 1, "its signal round-trips");

        // The core pack must be byte-identical: edits live in the user's file only, so an app
        // update can never clobber them and they can always be undone.
        QFile core(QStringLiteral(PP_CORE_PACK_PATH));
        check(core.open(QIODevice::ReadOnly), "the shipped pack is still readable");
        const PackLoadResult res = loadPack(core.readAll(), QStringLiteral("core"));
        check(res.loaded && res.pack.condition(newId) == nullptr,
              "the shipped pack does NOT contain the user's new characteristic");
    }

    // ── Removing a cause actually removes it ────────────────────────────────────
    // The failure this guards: a user pack that only ADDS edges would leave the removed edge alive
    // in core, so the removal would appear to work in the editor and be undone on reload.
    {
        CharacteristicEditorModel ed;
        ed.beginEdit(newId);
        ed.removeCause(QStringLiteral("poor_core_stability"));
        check(ed.draft().value(QStringLiteral("causes")).toList().size() == 1,
              "removing a cause updates the draft");
        check(ed.save().value(QStringLiteral("ok")).toBool(), "the removal saves");
    }
    {
        CharacteristicEditorModel ed;
        ed.beginEdit(newId);
        check(ed.draft().value(QStringLiteral("causes")).toList().size() == 1,
              "the removal survives a reload");
    }

    // ── Overriding a SHIPPED characteristic ─────────────────────────────────────
    {
        CharacteristicEditorModel ed;
        check(ed.beginEdit(QStringLiteral("early_extension")), "a shipped characteristic opens for edit");
        check(ed.overridesCore(), "editing a shipped entry is flagged as creating an override");

        ed.setConsequence(QStringLiteral("Edited consequence for the test."));
        check(ed.save().value(QStringLiteral("ok")).toBool(), "the override saves");
    }
    {
        CharacteristicEditorModel ed;
        ed.beginEdit(QStringLiteral("early_extension"));
        check(ed.draft().value(QStringLiteral("consequence"))
                  == QStringLiteral("Edited consequence for the test."),
              "the user's version wins over the shipped one");

        // And the shipped file is still pristine.
        QFile core(QStringLiteral(PP_CORE_PACK_PATH));
        core.open(QIODevice::ReadOnly);
        const PackLoadResult res = loadPack(core.readAll(), QStringLiteral("core"));
        const Condition     *c   = res.pack.condition(QStringLiteral("early_extension"));
        check(c && c->consequence.text() != QStringLiteral("Edited consequence for the test."),
              "the shipped definition is unchanged on disk");

        // Reverting removes the override and restores the shipped text.
        check(ed.revertToShipped().value(QStringLiteral("ok")).toBool(), "the override can be reverted");
    }
    {
        CharacteristicEditorModel ed;
        ed.beginEdit(QStringLiteral("early_extension"));
        check(ed.draft().value(QStringLiteral("consequence"))
                  != QStringLiteral("Edited consequence for the test."),
              "after reverting, the shipped definition is back");
    }

    // ── Guards ──────────────────────────────────────────────────────────────────
    {
        CharacteristicEditorModel ed;
        check(!ed.editing(), "nothing is being edited before beginEdit/beginNew");
        check(ed.draft().isEmpty(), "the draft is empty when not editing");
        check(!ed.save().value(QStringLiteral("ok")).toBool(), "saving nothing is refused");

        ed.beginNew();
        check(!ed.save().value(QStringLiteral("ok")).toBool(), "saving an unnamed characteristic is refused");

        check(!ed.beginEdit(QStringLiteral("no_such_condition")), "an unknown id refuses to open");
        check(ed.mintMeasure(facets("leadKnee", "angle", "ground")).isEmpty(),
              "an illegal measure does not mint");
    }

    // ── Causes rank by what they already explain ────────────────────────────────
    {
        CharacteristicEditorModel ed;
        ed.beginNew();
        const QVariantList cands = ed.candidateCauses();
        check(!cands.isEmpty(), "the cause picker offers the existing library");
        check(!cands.isEmpty()
                  && cands.first().toMap().value(QStringLiteral("explains")).toInt()
                         >= cands.last().toMap().value(QStringLiteral("explains")).toInt(),
              "causes that already explain a lot rank first (reuse over invention)");

        const QVariantList filtered = ed.candidateCauses(QStringLiteral("thoracic"));
        check(!filtered.isEmpty() && filtered.size() < cands.size(), "the cause picker filters");
    }

    QFile::remove(userPackPath());

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
