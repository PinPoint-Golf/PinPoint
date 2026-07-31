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

// Does the inspector actually REDRAW after a write?
//
// model_browser_test proves the façade answers correctly — inspect() returns the new cause the
// instant addLink() lands. That is one link in a chain of four, and the other three are QML:
//
//   1. the command emits modelChanged()                        (C++, covered there)
//   2. the panel's Connections bumps `_revision`                 ← here
//   3. `_inspectorDetail` re-evaluates because it reads it       ← here
//   4. the value crosses into ModelInspector's own `var detail`  ← here
//
// Every one of those is invisible in a unit test of the façade and invisible in a screenshot of a
// pane that happens to be right. The failure they produce — a details pane showing yesterday's
// answer while the table shows today's — is exactly the failure this panel's premise cannot afford,
// because "no modals, no Edit button, selecting a row IS opening its editor" only works if what is
// on screen is what is in the model.
//
// So this builds the REAL chain in a QQmlEngine: a revision counter driven by the same signal, a
// binding written the same way, and a second component taking the value through a `var` property of
// its own exactly as ModelInspector does. Then it writes and asks what QML thinks.
//
//   cmake --build build/analyzer-tests --target inspector_refresh_test
//   ctest --test-dir build/analyzer-tests -R inspector_refresh --output-on-failure

#include "model_browser.h"

#include "../pack_provider.h"
#include "../norm_provider.h"
#include "../drill_pack.h"
#include "../screen_pack.h"

#include <QCoreApplication>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// The panel's chain, written the way DiagnosticModel.qml writes it. Deliberately a COPY of the
// idiom rather than an import of the real file: the real one needs the whole app module, and what
// is under test is the idiom — a Q_INVOKABLE result, nudged by a signal, handed across a component
// boundary through a `var` property.
static const char *kQml = R"QML(
import QtQml
import QtQuick
import Probe

Item {
    id: root

    property ModelBrowser browser: ModelBrowser {}

    // "The façade is read through Q_INVOKABLEs, which are not properties and so cannot be bound to.
    //  This is nudged by modelChanged and every list binding depends on it."
    property int revision: 0
    property Connections conn: Connections {
        target: root.browser
        function onModelChanged() { root.revision++ }
    }

    property string selectedType: "characteristics"
    property string selectedId:   ""

    readonly property var inspectorDetail: {
        root.revision
        if (root.selectedId === "") return ({})
        return root.browser.inspect(root.selectedType, root.selectedId)
    }

    // The hop that only the inspector makes: the map crosses into another component's own `var`
    // property, and everything that pane draws is read back off THAT.
    property Item pane: Item {
        property var detail: root.inspectorDetail

        readonly property string title: detail.label || ""

        // What the map SAYS the pane should hold.
        readonly property int expectedRows: {
            var n = 0
            var s = detail.sections || []
            for (var i = 0; i < s.length; i++) n += (s[i].rows || []).length
            return n
        }

        // …and what the delegate tree ACTUALLY instantiated. ModelInspector nests a Repeater over
        // `detail.sections` inside which a second Repeater runs over that section's `rows`, with
        // `modelData` passed down as a required property. A map that updates while the delegates do
        // not is a stale pane, and it is invisible to any assertion that reads the map instead of
        // counting what was built — which is the whole point of doing this in a live QQmlEngine.
        property int builtRows: 0

        Repeater {
            model: pane.detail.sections || []
            delegate: Item {
                id: sectionItem
                required property var modelData

                Repeater {
                    model: sectionItem.modelData.rows
                    delegate: Item {
                        required property var modelData
                        // Counted on construction and uncounted on destruction, so the total tracks
                        // the live delegate set rather than how many were ever made.
                        Component.onCompleted:  pane.builtRows++
                        Component.onDestruction: pane.builtRows--
                    }
                }
            }
        }
    }
}
)QML";

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QFile::remove(userPackPath());
    QFile::remove(userNormPath());
    QFile::remove(userScreenSetPath());
    QFile::remove(userDrillSetPath());
    resetSharedScreenSet();
    resetSharedDrillSet();

    qmlRegisterType<ModelBrowser>("Probe", 1, 0, "ModelBrowser");

    QQmlEngine    engine;
    QQmlComponent component(&engine);
    component.setData(QByteArray(kQml), QUrl(QStringLiteral("probe.qml")));
    if (component.isError()) {
        for (const QQmlError &e : component.errors())
            std::printf("  QML ERROR: %s\n", qPrintable(e.toString()));
        std::printf("\nFAIL (component would not load)\n");
        return 1;
    }

    QObject *root = component.create();
    check(root != nullptr, "the panel's binding chain loads");
    if (!root) return 1;

    auto *browser = root->property("browser").value<ModelBrowser *>();
    check(browser != nullptr, "and the façade is reachable from it");
    if (!browser) return 1;

    QObject *pane = root->property("pane").value<QObject *>();
    check(pane != nullptr, "and the pane that reads it");
    if (!pane) return 1;

    const auto rowCount = [pane] { return pane->property("expectedRows").toInt(); };
    const auto builtRows = [pane] { return pane->property("builtRows").toInt(); };
    const auto title    = [pane] { return pane->property("title").toString(); };

    // Somewhere with room for one more cause.
    QString target, cause;
    for (const QVariant &v : browser->rows(QStringLiteral("characteristics"))) {
        const QString      id    = v.toMap().value(QStringLiteral("id")).toString();
        const QVariantList cands = browser->linkCandidates(QStringLiteral("causes"), id);
        if (cands.isEmpty()) continue;
        target = id;
        cause  = cands.first().toMap().value(QStringLiteral("id")).toString();
        break;
    }
    check(!target.isEmpty(), "there is a characteristic to add a cause to");

    root->setProperty("selectedId", target);
    const int before = rowCount();
    check(before > 0, "selecting it fills the pane");
    check(builtRows() == before, "and the delegate tree built exactly those rows");

    // ── The thing that was reported ─────────────────────────────────────────
    check(browser->addLink(cause, target, QStringLiteral("causes"))
              .value(QStringLiteral("ok")).toBool(), "a cause is added");
    check(rowCount() == before + 1, "and the PANE redraws with one more row");
    check(builtRows() == before + 1, "and the DELEGATES were rebuilt to match — not just the map");

    check(browser->removeLink(cause, target, QStringLiteral("causes"))
              .value(QStringLiteral("ok")).toBool(), "removing it succeeds");
    check(rowCount() == before, "and the pane follows that too");
    check(builtRows() == before, "delegates included");

    // A field edit has to reach the pane's header, not only its rows.
    const QString renamed = QStringLiteral("Renamed while the pane was open");
    check(browser->setField(QStringLiteral("characteristics"), target, QStringLiteral("label"),
                            renamed).value(QStringLiteral("ok")).toBool(), "a rename lands");
    check(title() == renamed, "and the pane's title is the new name");

    check(browser->undo().value(QStringLiteral("ok")).toBool(), "undo puts it back");
    check(title() != renamed, "and the pane follows the undo");

    // Undo/redo are writes like any other as far as the pane is concerned.
    check(browser->redo().value(QStringLiteral("ok")).toBool(), "redo re-applies it");
    check(title() == renamed, "and the pane follows the redo");
    while (browser->canUndo()) browser->undo();

    // ── The same question for the panes added in ADDENDUM-02 ────────────────
    const QVariantList screens = browser->rows(QStringLiteral("screens"));
    if (!screens.isEmpty()) {
        const QString screenId = screens.first().toMap().value(QStringLiteral("id")).toString();
        root->setProperty("selectedType", QStringLiteral("screens"));
        root->setProperty("selectedId", screenId);
        const int settlesBefore = rowCount();

        const QVariantList cands = browser->screenCandidates(screenId);
        if (!cands.isEmpty()) {
            const QString cond = cands.first().toMap().value(QStringLiteral("id")).toString();
            check(browser->addScreenSettles(screenId, cond).value(QStringLiteral("ok")).toBool(),
                  "a screen is made to settle a characteristic");
            check(rowCount() == settlesBefore + 1,
                  "and the SCREEN's pane redraws, though the write went to the condition");
            check(browser->undo().value(QStringLiteral("ok")).toBool(), "undo detaches it");
            check(rowCount() == settlesBefore, "and the pane follows the undo");
        }

        // Editing a field of the screen itself.
        check(browser->setField(QStringLiteral("screens"), screenId, QStringLiteral("name"),
                                QStringLiteral("Renamed screen"))
                  .value(QStringLiteral("ok")).toBool(), "a screen renames");
        check(title() == QStringLiteral("Renamed screen"), "and its pane's title follows");
        while (browser->canUndo()) browser->undo();
    }

    const QVariantList drills = browser->rows(QStringLiteral("drills"));
    if (!drills.isEmpty()) {
        const QString drillId = drills.first().toMap().value(QStringLiteral("id")).toString();
        root->setProperty("selectedType", QStringLiteral("drills"));
        root->setProperty("selectedId", drillId);
        const int answersBefore = rowCount();

        const QVariantList cands = browser->drillCandidates(drillId);
        if (!cands.isEmpty()) {
            const QString cond = cands.first().toMap().value(QStringLiteral("id")).toString();
            check(browser->addDrillAnswers(drillId, cond).value(QStringLiteral("ok")).toBool(),
                  "a drill is made to answer a characteristic");
            check(rowCount() == answersBefore + 1, "and the DRILL's pane redraws");
            check(browser->undo().value(QStringLiteral("ok")).toBool(), "undo detaches it");
            check(rowCount() == answersBefore, "and the pane follows the undo");
        }
    }

    // A corridor is edited through the plot, which reads a different binding — but its pane's rows
    // come through the same one.
    const QVariantList corridors = browser->rows(QStringLiteral("corridors"));
    if (!corridors.isEmpty()) {
        const QString cid = corridors.first().toMap().value(QStringLiteral("id")).toString();
        root->setProperty("selectedType", QStringLiteral("corridors"));
        root->setProperty("selectedId", cid);
        const QString titleBefore = title();
        check(browser->setField(QStringLiteral("corridors"), cid, QStringLiteral("source"),
                                QStringLiteral("heuristic")).value(QStringLiteral("ok")).toBool(),
              "a corridor's source is set");
        check(rowCount() > 0, "and its pane still answers");
        Q_UNUSED(titleBefore)
        while (browser->canUndo()) browser->undo();
    }

    delete root;

    QFile::remove(userPackPath());
    QFile::remove(userNormPath());
    QFile::remove(userScreenSetPath());
    QFile::remove(userDrillSetPath());

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
