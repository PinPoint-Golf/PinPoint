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

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

// Detail page for one condition.
//
// Causes and effects are ONE graph, not two lists. They used to be rendered as a pair of stacked
// lists here, which said how many of each there were and nothing about the shape they sat in — and
// following a chain back to its root meant a page load per step. DagView replaces both: it is a
// navigation surface, and it is where the same-condition-is-both-a-cause-and-an-effect structure
// this pack is built on finally shows.
Item {
    id: root

    property var    detail: ({})       // CharacteristicLibraryModel.detail()
    property string locale: "en"

    // The graph asks the model for its own layout, so the detail map is not enough.
    required property var library      // CharacteristicLibraryModel
    property var          editor: null // CharacteristicEditorModel — omit for a read-only graph

    signal back()
    signal edit()
    signal openCondition(string conditionId)
    signal openMeasure(string measureId)
    signal openReference(string referenceId)
    signal graphChanged()

    readonly property var _measures: detail.measures || []

    // ── The non-causal relations, and editing them ───────────────────────────
    //
    // Authored HERE rather than on the graph, and that is not a placement preference. The validator
    // refuses corroboration wherever a causal path already exists, so the conditions the DAG draws
    // are precisely the ones that cannot corroborate — a "relate these two" action on a drawn node
    // would be offered almost exclusively where it must be refused. Adding one needs a picker over
    // the whole library, which is what this is.
    property int    _relRevision: 0
    property string _pickerKind:  ""     // "" = closed | "corroborates" | "excludes"
    property string _pickerSearch: ""

    readonly property var _relations:
        (root.editor && root._relRevision >= 0 && root.detail.id)
            ? root.editor.relationsOf(root.detail.id) : []

    function _relationIsMine(otherId) {
        for (var i = 0; i < root._relations.length; ++i)
            if (root._relations[i].id === otherId) return root._relations[i].mine === true
        return false
    }

    function _relationKind(otherId) {
        for (var i = 0; i < root._relations.length; ++i)
            if (root._relations[i].id === otherId) return root._relations[i].relation
        return ""
    }

    function _openRelationPicker(kind) {
        root._pickerSearch = ""
        root._pickerKind   = kind
    }

    function _addRelation(otherId) {
        if (!root.editor || !root.detail.id) return
        var r = root.editor.linkRelation(root.detail.id, otherId, root._pickerKind, "moderate")
        root._pickerKind = ""
        root._afterRelationWrite(r, false)
    }

    function _flipRelation(otherId) {
        if (!root.editor || !root.detail.id) return
        var from = root._relationKind(otherId)
        var to   = from === "corroborates" ? "excludes" : "corroborates"
        var r = root.editor.editRelation(root.detail.id, otherId, from, to,
                                         to === "corroborates" ? "moderate" : "")
        root._afterRelationWrite(r, false)
    }

    function _removeRelation(otherId) {
        if (!root.editor || !root.detail.id) return
        var r = root.editor.unlinkRelation(root.detail.id, otherId, root._relationKind(otherId))
        root._afterRelationWrite(r, r.ok === true && r.canUndo === true)
    }

    function _afterRelationWrite(r, canUndo) {
        relToast.severity = r.ok ? "info" : "warn"
        relToast.showUndo = canUndo
        relToast.show(r.message || "")
        if (r.ok) {
            root._relRevision++
            // The detail map and the graph both hold a copy of the edge set, so the page has to be
            // told rather than left to notice.
            root.graphChanged()
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(18)

            // ── Back ─────────────────────────────────────────────────────────
            Text {
                text:           "← " + qsTr("All diagnostics")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          backMa.containsMouse ? Theme.colorText : Theme.colorText3

                MouseArea {
                    id: backMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape:  Qt.PointingHandCursor
                    onClicked:    root.back()
                }
            }

            // ── Title ────────────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                PpDisplayText { text: root.detail.label || "" }
                Item { Layout.fillWidth: true }
                PpButton { label: qsTr("Edit"); onClicked: root.edit() }
            }

            RowLayout {
                spacing: Theme.sp(8)

                Repeater {
                    model: {
                        var tags = []
                        if (root.detail.groupLabel) tags.push(root.detail.groupLabel)
                        if (root.detail.reachLabel && root.detail.reach !== "measured")
                            tags.push(root.detail.reachLabel)
                        if (root.detail.proposed === true) tags.push(qsTr("Proposed"))
                        if (root.detail.resolvabilityLabel) tags.push(root.detail.resolvabilityLabel)
                        return tags
                    }
                    delegate: Rectangle {
                        required property var modelData
                        implicitWidth:  tagText.implicitWidth + Theme.sp(16)
                        implicitHeight: Theme.sp(22)
                        radius: height / 2
                        color:  Theme.colorBg2

                        Text {
                            id: tagText
                            anchors.centerIn: parent
                            text:           modelData
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                    }
                }
            }

            // ── The other names for it ───────────────────────────────────────
            // Above the consequence, not below: a reader who arrived by searching a coach term needs
            // to see their own word confirmed before they read anything else, or they cannot tell
            // whether they landed on the right page.
            Text {
                Layout.fillWidth: true
                text: (root.detail.aliases && root.detail.aliases.length > 0)
                      ? qsTr("Also called ") + root.detail.aliases.join(qsTr(", "))
                      : ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                font.italic:    true
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                visible:        text.length > 0
            }

            // ── What it costs the golfer ─────────────────────────────────────
            Text {
                Layout.fillWidth: true
                text:           root.detail.consequence || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody
                color:          Theme.colorText
                wrapMode:       Text.WordWrap
                visible:        text.length > 0
            }

            // ── Injury note — a separate axis from performance ───────────────
            Rectangle {
                Layout.fillWidth: true
                visible:        (root.detail.injuryNote || "").length > 0
                implicitHeight: injuryText.implicitHeight + Theme.sp(20)
                radius:         Theme.radius
                color:          Theme.colorBg2

                Text {
                    id: injuryText
                    x:              Theme.sp(12)
                    y:              Theme.sp(10)
                    width:          parent.width - Theme.sp(24)
                    text:           root.detail.injuryNote || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    wrapMode:       Text.WordWrap
                }
            }

            // ── Where it applies ─────────────────────────────────────────────
            //
            // Shown ONLY when somebody has narrowed it. A characteristic that applies everywhere —
            // which is all 50 shipped ones — says nothing here, because a line reading "applies to
            // every kind of shot" on every page would train the reader to skip the one page where
            // it does not.
            Text {
                Layout.fillWidth: true
                visible:        (root.detail.appliesSummary || "").length > 0
                text:           root.detail.appliesSummary || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            // ── How it is detected ───────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)
                visible: root._measures.length > 0

                Text {
                    text:                qsTr("DETECTED FROM")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Repeater {
                    model: root._measures
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.sp(2)

                        Text {
                            Layout.fillWidth: true
                            text:           modelData.label || modelData.id
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color:          Theme.colorText
                            wrapMode:       Text.WordWrap
                        }
                        // WHICH SIDE FIRES. The first thing a reader has to check before deciding
                        // a characteristic is right, and until now it could only be seen by opening
                        // the editor. Composed in C++ by the same rule as the control that sets it.
                        Text {
                            Layout.fillWidth: true
                            visible:        (modelData.directionSentence || "").length > 0
                            text:           modelData.directionSentence || ""
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText2
                            wrapMode:       Text.WordWrap
                        }
                        // No stated convention means nothing can check that tail is the right one.
                        Text {
                            Layout.fillWidth: true
                            visible:        (modelData.direction || "").length > 0
                                            && (modelData.highMeans || "").length === 0
                            text: qsTr("Nothing says what a higher value of this measure means, so "
                                       + "which side fires cannot be checked.")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorRagWatch
                            wrapMode:       Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            text: {
                                var bits = [modelData.statusLabel]
                                if (modelData.metricKey) bits.push(modelData.metricKey)
                                if (modelData.usedBy > 1)
                                    bits.push(qsTr("used by %n characteristics", "", modelData.usedBy))
                                return bits.join(" · ")
                            }
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                        }
                        // A capture gap says why, so nobody files it as pipeline work.
                        Text {
                            Layout.fillWidth: true
                            visible:        (modelData.gapReason || "").length > 0
                            text:           modelData.gapReason || ""
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                        }
                    }
                }
            }

            // ── How it connects ──────────────────────────────────────────────
            // The whole causes/effects picture, walkable. Replaces the two lists that used to sit
            // here; every coordinate in it comes from dag_layout.cpp.
            DagView {
                Layout.fillWidth: true
                library: root.library
                editor:  root.editor
                rootId:  root.detail.id || ""

                onOpenCondition: function (conditionId) { root.openCondition(conditionId) }
                onOpenMeasure:   function (measureId)   { root.openMeasure(measureId) }
                onGraphChanged:  root.graphChanged()
            }

            // ── Relations that are not causal ────────────────────────────────
            //
            // Below the graph rather than in it. The DAG ranks by signed causal distance, and a
            // symmetric relation has no direction to rank by — drawing one in that flow would state
            // a claim the pack does not hold. These sit beneath it as what they are: two readings of
            // one event, and pairs that cannot both be true of one swing.
            Repeater {
                model: [{ kind: "corroborates",
                          rows: root.detail.corroboratedBy || [],
                          head: qsTr("ALSO SEEN AS"),
                          hint: qsTr("The same event, read another way. Independent confirmation — no causal claim either direction.") },
                        { kind: "excludes",
                          rows: root.detail.excludes || [],
                          head: qsTr("CANNOT ALSO BE"),
                          hint: qsTr("One swing cannot be both. If both fire, the explanation keeps the more confident reading and says which it dropped.") }]

                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.sp(6)
                    // The heading stays when the list is empty IF this page can author one —
                    // otherwise "add a link" has nowhere to live and the relation is only ever
                    // reachable from a graph that does not draw it until it exists.
                    visible: modelData.rows.length > 0 || root.editor !== null

                    RowLayout {
                        id: relHead
                        // The section's own kind, hoisted so the handlers below reach it from the
                        // delegate scope rather than from a file-level id, which a Repeater
                        // delegate cannot see.
                        readonly property string kind: modelData.kind
                        Layout.fillWidth: true
                        spacing: Theme.sp(8)

                        Text {
                            text:                modelData.head
                            font.family:         Theme.fontBody
                            font.pixelSize:      Theme.fontSzMicro
                            font.letterSpacing:  Theme.trackingMicro
                            font.capitalization: Font.AllUppercase
                            color:               Theme.colorText3
                        }

                        Item { Layout.fillWidth: true }

                        // ADD lives here rather than on the graph, because a corroborating partner
                        // is almost never already drawn: the validator refuses corroboration over a
                        // causal path, so the very conditions the DAG shows are the ones that
                        // cannot corroborate. It needs a picker over the whole library.
                        Text {
                            visible:        root.editor !== null
                            text:           qsTr("Add a link →")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorAccent

                            MouseArea {
                                anchors.fill: parent
                                cursorShape:  Qt.PointingHandCursor
                                onClicked:    root._openRelationPicker(relHead.kind)
                            }
                        }
                    }

                    Repeater {
                        model: modelData.rows
                        delegate: RowLayout {
                            id: relRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Theme.sp(8)

                            Text {
                                Layout.fillWidth: true
                                text:           relRow.modelData.label + " · " + relRow.modelData.groupLabel
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody
                                color:          Theme.colorAccent
                                elide:          Text.ElideRight

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked:    root.openCondition(relRow.modelData.id)
                                }
                            }

                            // EDIT — change what the link says. Offered for a shipped relation too:
                            // the user pack overrides it per PAIR, so the change takes without
                            // leaving two contradictory rows in the assembled library.
                            Text {
                                visible:        root.editor !== null
                                text: root._relationKind(relRow.modelData.id) === "corroborates"
                                      ? qsTr("cannot both be")
                                      : qsTr("same thing twice")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorAccent

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked:    root._flipRelation(relRow.modelData.id)
                                }
                            }

                            // DELETE — including a relation the shipped pack states, which the user
                            // pack retires with a tombstone. Editing only from the graph would have
                            // been poor: the list is where a reader is already looking at the thing
                            // they want gone.
                            Text {
                                visible:        root.editor !== null
                                text:           qsTr("remove")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorError

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked:    root._removeRelation(relRow.modelData.id)
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text:           modelData.hint
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                    }
                }
            }

            // ── Provenance ───────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Text {
                    text:                qsTr("PROVENANCE")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                // The citation, and — where it resolves — the way to the paper behind it. An
                // identifier a reader cannot follow is the state this whole registry exists to
                // fix, so it links to the row in References rather than merely to the view: the
                // question being asked is "what IS this?", and landing on a list of twenty-one
                // papers restates the question.
                //
                // The link affordance is strictly gated on the citation resolving. A citation from
                // a user layer naming a paper the registry has never heard of renders as plain
                // text, because a link that lands nowhere is worse than no link.
                Text {
                    id: citationText
                    Layout.fillWidth: true

                    readonly property bool _linkable:
                        (root.detail.citationReferenceId || "").length > 0

                    text: (root.detail.citation && root.detail.citation.length > 0)
                          ? root.detail.citation
                          : qsTr("No citation. This entry states a direction and a phase reasoned "
                                 + "from mechanics, not taken from a source, and is badged Proposed "
                                 + "wherever it appears.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    // Accent AT REST, brightening on hover — the app's link convention. Reserving
                    // the accent for hover hides the affordance from anyone not already pointing at
                    // it, which is exactly the reader who does not yet know the identifier leads
                    // anywhere.
                    color: !citationText._linkable   ? Theme.colorText3
                         : citationMa.containsMouse  ? Qt.lighter(Theme.colorAccent, 1.08)
                                                     : Theme.colorAccent
                    wrapMode:       Text.WordWrap

                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                    MouseArea {
                        id:           citationMa
                        anchors.fill: parent
                        enabled:      citationText._linkable
                        hoverEnabled: citationText._linkable
                        cursorShape:  Qt.PointingHandCursor
                        onClicked:    root.openReference(root.detail.citationReferenceId)
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible:        (root.detail.screenRef || "").length > 0
                    text:           qsTr("Screen: %1").arg(root.detail.screenRef || "")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }

    // ══ The relation picker ═══════════════════════════════════════════════════
    //
    // Only LEGAL candidates are listed. The model filters out the condition itself, anything it is
    // already related to, and — for a corroboration — anything a causal path already reaches, so
    // the list cannot offer a choice the write would refuse.
    Rectangle {
        id: relPicker
        anchors.fill: parent
        color:   Theme.colorBg
        opacity: 0.97
        visible: root._pickerKind !== ""
        z: 20

        // Swallows anything aimed at the page underneath, so a click meant for the picker cannot
        // navigate the page it is covering.
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.sp(32)
            spacing: Theme.sp(12)

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: root._pickerKind === "corroborates"
                          ? qsTr("What else is this same event, read another way?")
                          : qsTr("What could this not also be, in one swing?")
                    font.family:    Theme.fontDisplay
                    font.pixelSize: Theme.fontSzHeading
                    color:          Theme.colorText
                    wrapMode:       Text.WordWrap
                }
                Text {
                    text:           qsTr("Cancel")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    MouseArea {
                        anchors.fill: parent
                        cursorShape:  Qt.PointingHandCursor
                        onClicked:    root._pickerKind = ""
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: root._pickerKind === "corroborates"
                      ? qsTr("Anything already causally linked to this is left out: a pair that both "
                             + "causes and corroborates would count twice when the explanation is ranked.")
                      : qsTr("An exclusion has no degree — the pair is incompatible or it is not.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            PpTextField {
                Layout.fillWidth: true
                placeholderText: qsTr("Search — the coach term works too")
                onTextChanged: root._pickerSearch = text
            }

            ScrollView {
                Layout.fillWidth:  true
                Layout.fillHeight: true
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: Theme.sp(2)

                    Repeater {
                        model: (root.editor && root._pickerKind !== "" && root.detail.id)
                               ? root.editor.relationCandidates(root.detail.id, root._pickerKind,
                                                                root._pickerSearch)
                               : []

                        delegate: Rectangle {
                            id: candRow
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: Theme.sp(40)
                            color: candMa.containsMouse ? Theme.colorBg2 : "transparent"
                            radius: Theme.sp(4)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin:  Theme.sp(10)
                                anchors.rightMargin: Theme.sp(10)
                                spacing: Theme.sp(8)

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Text {
                                        Layout.fillWidth: true
                                        text:           candRow.modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody
                                        color:          Theme.colorText
                                        elide:          Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: (candRow.modelData.aliases || []).length > 0
                                              ? candRow.modelData.groupLabel + " · "
                                                + (candRow.modelData.aliases || []).join(", ")
                                              : candRow.modelData.groupLabel
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                        elide:          Text.ElideRight
                                    }
                                }
                            }

                            MouseArea {
                                id: candMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                // Through a method on the component root: a handler in a Repeater
                                // delegate can see that and nothing else at file scope.
                                onClicked:    root._addRelation(candRow.modelData.id)
                            }
                        }
                    }
                }
            }
        }
    }

    PpToast {
        id: relToast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom:           parent.bottom
        z: 30
        glyph: "◇"
        showUndo: false
    }

    // The undo handler cannot live inside the PpToast block: PpToast declares its own `id: root`,
    // which shadows this file's. A Connections block declares none, so `root` here is this file's.
    Connections {
        target: relToast
        function onUndoClicked() {
            if (!root.editor) return
            var r = root.editor.undoUnlinkRelation()
            relToast.showUndo = false          // one level: the undo is consumed by using it
            root._afterRelationWrite(r, false)
        }
    }
}
