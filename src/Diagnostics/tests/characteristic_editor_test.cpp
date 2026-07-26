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

    // ── Two overrides must survive each other ───────────────────────────────────
    //
    // REGRESSION. The constructor keyed the user pack off PackLoadResult::loaded, where the type's
    // own comment says a merging caller must key off `parsed`: an overlay routinely fails STANDALONE
    // referential validation because its edges point at core conditions it does not contain. So the
    // editor started with an EMPTY user pack whenever an ordinary override was on disk — and since
    // save() upserts into that pack and writes the whole thing back, the next save erased every
    // other override the user had ever made. Silently, and only visible on the NEXT launch.
    {
        CharacteristicEditorModel a;
        check(a.beginEdit(QStringLiteral("early_extension")), "open the first shipped entry");
        a.setConsequence(QStringLiteral("First override."));
        check(a.save().value(QStringLiteral("ok")).toBool(), "the first override saves");
    }
    {
        // A SECOND model, as a fresh launch would be, editing a DIFFERENT characteristic.
        CharacteristicEditorModel b;
        check(b.beginEdit(QStringLiteral("casting")), "open a second shipped entry");
        b.setConsequence(QStringLiteral("Second override."));
        check(b.save().value(QStringLiteral("ok")).toBool(), "the second override saves");
    }
    {
        CharacteristicEditorModel c;
        check(c.beginEdit(QStringLiteral("early_extension")), "the first is still there");
        check(c.draft().value(QStringLiteral("consequence"))
                  == QStringLiteral("First override."),
              "saving the SECOND override did not erase the first");
        check(c.hasUserOverride(), "…and it is still recognised as the user's");

        check(c.beginEdit(QStringLiteral("casting")), "the second is there too");
        check(c.draft().value(QStringLiteral("consequence"))
                  == QStringLiteral("Second override."),
              "…and holds its own text");
    }
    {
        // Leave the library as we found it.
        CharacteristicEditorModel d;
        d.beginEdit(QStringLiteral("early_extension"));
        d.revertToShipped();
        CharacteristicEditorModel e;
        e.beginEdit(QStringLiteral("casting"));
        e.revertToShipped();
    }

    // ── Dropping a characteristic that never shipped ────────────────────────────
    //
    // The same operation as reverting an override, and for a long time the same label and the same
    // message: "Restore shipped version" / "Restored the shipped definition". For a characteristic
    // the user wrote themselves there is nothing to restore — the row is simply DELETED, and a
    // destructive action wearing a reassuring name is how somebody loses work.
    {
        CharacteristicEditorModel ed;
        check(ed.beginEdit(newId), "a user-created characteristic opens for edit");
        check(!ed.shippedExists(), "nothing ships under this id");
        check(ed.hasUserOverride(), "…but the user's own row is there");

        const QVariantMap rev = ed.revertToShipped();
        check(rev.value(QStringLiteral("ok")).toBool(), "dropping it succeeds");
        check(!rev.value(QStringLiteral("message")).toString().contains(QStringLiteral("Restored")),
              "…and does NOT claim to have restored anything");
        check(rev.value(QStringLiteral("message")).toString().contains(QStringLiteral("Deleted")),
              "…it says the characteristic was deleted, which is what happened");
    }
    {
        CharacteristicEditorModel ed;
        check(!ed.beginEdit(newId), "…and it is gone");
    }

    // ── Overriding a SHIPPED characteristic ─────────────────────────────────────
    {
        CharacteristicEditorModel ed;
        check(ed.beginEdit(QStringLiteral("early_extension")), "a shipped characteristic opens for edit");
        check(ed.shippedExists(), "core ships this id");
        check(!ed.hasUserOverride(), "…and nothing of the user's stands on it yet");
        check(!ed.revertToShipped().value(QStringLiteral("ok")).toBool(),
              "…so there is nothing to revert, and it says so rather than deleting anything");

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

        check(ed.shippedExists() && ed.hasUserOverride(),
              "an edited shipped characteristic is BOTH shipped and overridden");

        // Reverting removes the override and restores the shipped text.
        const QVariantMap rev = ed.revertToShipped();
        check(rev.value(QStringLiteral("ok")).toBool(), "the override can be reverted");
        check(rev.value(QStringLiteral("message")).toString().contains(QStringLiteral("Restored")),
              "…and reports a RESTORE, because something does ship under this name");
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

    // ── Where it applies: the context list ──────────────────────────────────────
    {
        CharacteristicEditorModel ed;
        ed.beginEdit(QStringLiteral("early_extension"));

        const QVariantList ctx = ed.contexts();
        check(!ctx.isEmpty(), "the editor offers the context tree");

        // Tree order with depth, so the view indents off the model rather than walking parents.
        const QVariantMap first = ctx.first().toMap();
        check(first.value(QStringLiteral("depth")).toInt() == 0, "the first row is a root");
        bool sawDeep = false, allApplicable = true, anyOwn = false;
        for (const QVariant &v : ctx) {
            const QVariantMap r = v.toMap();
            if (r.value(QStringLiteral("depth")).toInt() > 0) sawDeep = true;
            if (!r.value(QStringLiteral("applicable")).toBool()) allApplicable = false;
            if (r.value(QStringLiteral("own")).toBool()) anyOwn = true;
        }
        check(sawDeep, "children carry a depth the view can indent from");
        check(allApplicable && !anyOwn,
              "a shipped characteristic applies everywhere and states nothing — bindings are exceptions");
    }

    // ── Setting, cascading and undoing a binding ────────────────────────────────
    {
        CharacteristicEditorModel ed;
        ed.beginEdit(QStringLiteral("early_extension"));

        const QVariantMap bad = ed.setBinding(QStringLiteral("no_such_context"), false, true);
        check(!bad.value(QStringLiteral("ok")).toBool(),
              "a binding on a context the tree does not know is refused, not written");

        // An explicit exception BENEATH what is about to be switched off. Without the cascade this
        // row would survive and the untick would silently not take for chips.
        check(ed.setBinding(QStringLiteral("chip"), true, true).value(QStringLiteral("ok")).toBool(),
              "an explicit row can be written at a child");

        const QVariantMap off = ed.setBinding(QStringLiteral("partial"), false, true);
        check(off.value(QStringLiteral("ok")).toBool(), "switching a parent off succeeds");
        check(off.value(QStringLiteral("cascaded")).toInt() == 1,
              "…and reports the one contradicting row beneath it that it cleared");
        check(off.value(QStringLiteral("message")).toString().contains(QStringLiteral("Partial")),
              "the message names the context, not an id");

        auto rowFor = [](const QVariantList &l, const char *id) {
            for (const QVariant &v : l)
                if (v.toMap().value(QStringLiteral("id")) == QLatin1String(id)) return v.toMap();
            return QVariantMap();
        };

        QVariantList ctx  = ed.contexts();
        const QVariantMap partial = rowFor(ctx, "partial");
        const QVariantMap chip    = rowFor(ctx, "chip");
        const QVariantMap driver  = rowFor(ctx, "driver");
        check(!partial.value(QStringLiteral("applicable")).toBool()
                  && partial.value(QStringLiteral("own")).toBool(),
              "the context that was switched off owns the row");
        check(!chip.value(QStringLiteral("applicable")).toBool()
                  && chip.value(QStringLiteral("inherited")).toBool()
                  && chip.value(QStringLiteral("inheritedFromLabel")).toString()
                         == QStringLiteral("Partial swing"),
              "a child inherits it and names where it came from");
        check(driver.value(QStringLiteral("applicable")).toBool(),
              "a sibling subtree is untouched");

        // The toast's UNDO. One level, and it restores the rows the cascade removed as well — the
        // whole point of snapshotting the vector rather than the one row the user named.
        check(ed.undoBindingChange(), "the last binding change can be undone");
        ctx = ed.contexts();
        check(rowFor(ctx, "partial").value(QStringLiteral("applicable")).toBool(),
              "…the parent is applicable again");
        check(rowFor(ctx, "chip").value(QStringLiteral("own")).toBool(),
              "…and the row the cascade cleared is back");
        check(!ed.undoBindingChange(), "there is only one level of undo, and it says so");

        // Clearing goes back to inheriting, and says what it now follows.
        const QVariantMap cleared = ed.clearBinding(QStringLiteral("chip"));
        check(cleared.value(QStringLiteral("ok")).toBool(), "an own row can be cleared");
        check(!ed.contexts().isEmpty()
                  && !rowFor(ed.contexts(), "chip").value(QStringLiteral("own")).toBool(),
              "…and the context inherits again");
        check(!ed.clearBinding(QStringLiteral("chip")).value(QStringLiteral("ok")).toBool(),
              "clearing what already inherits is refused rather than silently doing nothing");
    }

    // ── Bindings round-trip through the user pack ───────────────────────────────
    {
        {
            CharacteristicEditorModel ed;
            ed.beginEdit(QStringLiteral("early_extension"));
            ed.setBinding(QStringLiteral("bunker"), false, true);
            ed.setBinding(QStringLiteral("wedge"), true, false);
            check(ed.save().value(QStringLiteral("ok")).toBool(), "a narrowed characteristic saves");
        }

        CharacteristicEditorModel ed;
        check(ed.beginEdit(QStringLiteral("early_extension")), "it reopens");

        auto rowFor = [](const QVariantList &l, const char *id) {
            for (const QVariant &v : l)
                if (v.toMap().value(QStringLiteral("id")) == QLatin1String(id)) return v.toMap();
            return QVariantMap();
        };
        const QVariantList ctx = ed.contexts();
        check(!rowFor(ctx, "bunker").value(QStringLiteral("applicable")).toBool(),
              "an inapplicable context survives the round-trip");
        const QVariantMap wedge = rowFor(ctx, "wedge");
        check(wedge.value(QStringLiteral("applicable")).toBool()
                  && !wedge.value(QStringLiteral("material")).toBool(),
              "…and so does an applicable-but-immaterial one, which is a different statement");
    }

    // ── The direction control speaks the measure's own words ────────────────────
    {
        CharacteristicEditorModel ed;
        ed.beginNew();

        // With no stated convention it falls back to Too much / Too little — authorable, but the
        // sentence says nothing about the swing, which is the state the caller must not leave.
        const QVariantList bare = ed.directionOptions(QString());
        check(bare.size() == 2, "both tails are always offered");
        check(bare.first().toMap().value(QStringLiteral("means")).toString().isEmpty(),
              "with no highMeans there is nothing to say about a high value");

        const QString hm = QStringLiteral("further back, toward the trail foot");
        const QVariantList opts = ed.directionOptions(hm);
        check(opts.first().toMap().value(QStringLiteral("name")) == QStringLiteral("high")
                  && opts.last().toMap().value(QStringLiteral("name")) == QStringLiteral("low"),
              "high first, low second — the order the caller indexes by direction");
        check(opts.first().toMap().value(QStringLiteral("means")).toString() == hm,
              "the high tail is quoted verbatim, never paraphrased");
        check(opts.first().toMap().value(QStringLiteral("sentence")).toString().contains(hm),
              "the high sentence carries the measure's own words");
        check(opts.last().toMap().value(QStringLiteral("sentence")).toString().contains(hm),
              "so does the low one — the SAME words, stated as the other end, never an invented opposite");

        // A minted measure carries the convention it was authored with, and the signal row then
        // renders it instead of "too much".
        QVariantMap f = facets("leadHand", "distance", "leadAnkle", "at", "p1");
        f.insert(QStringLiteral("highMeans"), hm);
        const QString mid = ed.mintMeasure(f);
        check(!mid.isEmpty(), "the measure mints");
        check(ed.measureHighMeans(mid) == hm, "…carrying what a high value means");

        ed.attachMeasure(mid, QStringLiteral("low"));
        const QVariantList sigs = ed.draft().value(QStringLiteral("signals")).toList();
        check(sigs.size() == 1 && sigs.first().toMap()
                  .value(QStringLiteral("directionSentence")).toString().contains(hm),
              "the signal row states the tail in the measure's own words");

        // ── The tail can be changed AFTER it was set ────────────────────────
        //
        // It could not be until 2026-07-26: attaching was the only way to set it, so correcting an
        // inversion meant detach + re-attach — and re-attaching at the other tail added a SECOND
        // signal rather than replacing the first, because the minted id spells the direction out.
        // A condition fires if any of its signals fires, so the characteristic silently ended up
        // flagging both sides of the corridor.
        const QVariantList before = ed.draft().value(QStringLiteral("signals")).toList();
        const QString      sigId  = before.first().toMap().value(QStringLiteral("id")).toString();
        check(sigId == QStringLiteral("sig_%1_low").arg(mid), "a minted id spells its tail out");

        const QVariantMap flip = ed.setSignalDirection(sigId, QStringLiteral("high"));
        check(flip.value(QStringLiteral("ok")).toBool(), "the tail can be flipped in place");
        check(flip.value(QStringLiteral("message")).toString().contains(hm),
              "…and the result states the new tail in the measure's own words");

        const QVariantList after = ed.draft().value(QStringLiteral("signals")).toList();
        check(after.size() == 1, "flipping REPLACES the tail — it does not add the other one");
        check(after.first().toMap().value(QStringLiteral("direction")) == QStringLiteral("high"),
              "the direction is the new one");
        check(after.first().toMap().value(QStringLiteral("id")).toString()
                  == QStringLiteral("sig_%1_high").arg(mid),
              "an id WE minted is re-minted, so the file never spells the opposite of what it does");
        check(ed.setSignalDirection(sigId, QStringLiteral("low"))
                  .value(QStringLiteral("ok")).toBool() == false,
              "the retired id is gone — nothing can be flipped through a stale handle");
        check(ed.setSignalDirection(QStringLiteral("sig_%1_high").arg(mid), QStringLiteral("high"))
                  .value(QStringLiteral("ok")).toBool(),
              "setting the tail it already has is a no-op, not an error");

        // An existing measure with no stated convention can be given one, and it lands on the draft
        // so a save carries it.
        const QString bareId = ed.mintMeasure(facets("leadHand", "distance", "trailAnkle", "at", "p4"));
        check(!bareId.isEmpty() && ed.measureHighMeans(bareId).isEmpty(), "…and one minted without");
        ed.setMeasureHighMeans(bareId, QStringLiteral("the hands further from the trail ankle"));
        check(ed.measureHighMeans(bareId) == QStringLiteral("the hands further from the trail ankle"),
              "a missing convention can be authored where the direction is chosen");
    }

    // ── A flipped tail survives the round-trip, and leaves no dead row ──────────
    {
        QString measureId, signalId;
        {
            CharacteristicEditorModel ed;
            ed.beginEdit(QStringLiteral("early_extension"));

            QVariantMap f = facets("leadHand", "distance", "leadHeel", "at", "p7");
            f.insert(QStringLiteral("highMeans"), QStringLiteral("the hands further from the lead heel"));
            measureId = ed.mintMeasure(f);
            signalId  = ed.attachMeasure(measureId, QStringLiteral("high"));
            check(!signalId.isEmpty(), "a signal is attached at the high tail");
            check(ed.save().value(QStringLiteral("ok")).toBool(), "it saves");
        }
        {
            CharacteristicEditorModel ed;
            ed.beginEdit(QStringLiteral("early_extension"));
            check(ed.setSignalDirection(signalId, QStringLiteral("low"))
                      .value(QStringLiteral("ok")).toBool(),
                  "a SAVED signal's tail can be flipped on reopening");
            check(ed.save().value(QStringLiteral("ok")).toBool(), "the flip saves");
        }

        // The user pack must carry the new signal and NOT the retired one — otherwise every flip
        // leaves a dead row behind and the unused-signal warning fills with the user's history.
        QFile f(userPackPath());
        check(f.open(QIODevice::ReadOnly), "the user pack is readable");
        const PackLoadResult res = loadPack(f.readAll(), userPackPath());
        check(res.parsed, "it parses");
        bool hasNew = false, hasOld = false;
        for (const Signal &s : res.pack.signalDefs) {
            if (s.id == QStringLiteral("sig_%1_low").arg(measureId))  hasNew = true;
            if (s.id == QStringLiteral("sig_%1_high").arg(measureId)) hasOld = true;
        }
        check(hasNew, "the flipped signal round-trips");
        check(!hasOld, "…and the id it retired is gone from the pack");

        const Condition *c = res.pack.condition(QStringLiteral("early_extension"));
        check(c && c->detectedBy.contains(QStringLiteral("sig_%1_low").arg(measureId)),
              "the condition points at the new id, not the retired one");
        check(c && !c->detectedBy.contains(QStringLiteral("sig_%1_high").arg(measureId)),
              "…and not at both, which would fire either side of the corridor");
    }

    QFile::remove(userPackPath());

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
