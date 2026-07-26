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

// Faceted find-or-mint. The component the whole authoring flow turns on.
//
// Two decisions shape it:
//
//   * The typed phrase SEEDS facet chips; it is not a query. A search that returns nothing leaves
//     the author guessing at synonyms, whereas a wrong chip is corrected with one tap.
//
//   * The series and the reducer are TWO STEPS, not one flat row of five chips. Flattening them
//     hides the distinction the roadmap depends on: one producer for a series unblocks every
//     reducer over it, so "which curve" and "how it is read" are different questions.
Item {
    id: root

    required property var editor        // CharacteristicEditorModel

    signal measureChosen(string measureId, string direction)
    signal cancelled()

    // ── facet state ───────────────────────────────────────────────────────────
    property string _what:        ""
    property string _quantity:    ""
    property string _reference:   ""
    property string _reducerKind: "at"
    property string _anchor:      "p1"
    property string _windowStart: "p1"
    property string _windowEnd:   "p7"
    property string _direction:   "high"

    // What a HIGH value of this measure means, in the author's own words. Typed here when the
    // measure is being minted; taken from the measure when one already exists.
    property string _highMeans:   ""

    readonly property var _facets: ({
        what:        root._what,
        quantity:    root._quantity,
        reference:   root._reference,
        reducerKind: root._reducerKind,
        anchor:      root._anchor,
        windowStart: root._windowStart,
        windowEnd:   root._windowEnd,
        sense:       "max"
    })

    // The convention is deliberately NOT part of _facets: it names nothing about the series, and
    // folding it in would re-run the preview on every keystroke of a sentence.
    function _mintFacets() {
        var f = {}
        for (var k in root._facets) f[k] = root._facets[k]
        f.highMeans = root._highMeans
        return f
    }

    // A near-duplicate the author chose to reuse instead of minting. Selecting one does not commit:
    // the tail is still chosen below, against THIS measure's convention.
    property string _reuseId:        ""
    property string _reuseLabel:     ""
    property string _reuseHighMeans: ""

    // An existing measure already says what a high value means; a new one says what the author
    // types. Whichever it is, the direction control speaks THOSE words.
    readonly property string _existingHighMeans:
        root._reuseId.length > 0
            ? root._reuseHighMeans
            : ((root._preview.exactMatch && root._preview.exactMatch.highMeans) || "")
    readonly property string _convention:
        root._existingHighMeans.length > 0 ? root._existingHighMeans : root._highMeans
    readonly property var _directions: root.editor.directionOptions(root._convention)

    readonly property var _preview: (root._what.length > 0 && root._quantity.length > 0
                                     && root._reference.length > 0)
                                    ? root.editor.previewMeasure(root._facets) : ({})

    readonly property bool _usesWindow: root._reducerKind !== "at"

    function _seedFrom(phrase) {
        var seed = root.editor.seedFacetsFromPhrase(phrase)
        if (seed.what)      root._what      = seed.what
        if (seed.quantity)  root._quantity  = seed.quantity
        if (seed.reference) root._reference = seed.reference
        if (seed.anchor)    root._anchor    = seed.anchor
        // Clear facets the new subject can no longer support, rather than leaving an illegal pair.
        if (root._what.length > 0 && root._quantity.length > 0) {
            var qs = root.editor.quantitiesFor(root._what)
            var ok = false
            for (var i = 0; i < qs.length; ++i) if (qs[i].name === root._quantity) ok = true
            if (!ok) { root._quantity = ""; root._reference = "" }
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x:       Theme.sp(4)
            width:   parent.width - Theme.sp(8)
            spacing: Theme.sp(16)

            // ── 1. Free text seeds the chips ──────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Text {
                    text:                qsTr("DESCRIBE IT")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                PpTextField {
                    id: phraseField
                    Layout.fillWidth: true
                    placeholderText: qsTr("e.g. pelvis sway at the top")
                    onTextChanged: root._seedFrom(text)
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("This fills in the chips below — correct anything it got wrong by "
                               + "tapping.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }
            }

            // ── 2. What is being measured ─────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                Text {
                    text:                qsTr("WHAT")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Repeater {
                    model: root.editor.anatomyGroups
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.sp(4)
                        // World datums describe a reference, never a subject.
                        visible: modelData.label !== "World"

                        Text {
                            text:           modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }

                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.sp(6)

                            Repeater {
                                model: modelData.roles
                                delegate: Rectangle {
                                    required property var modelData
                                    readonly property bool active: root._what === modelData.name

                                    implicitWidth:  roleText.implicitWidth + Theme.sp(18)
                                    implicitHeight: Theme.sp(24)
                                    radius: height / 2
                                    color:  active ? Theme.colorAccent : Theme.colorBg2

                                    Text {
                                        id: roleText
                                        anchors.centerIn: parent
                                        // A role no sensor can resolve is marked, so the author
                                        // knows before building a characteristic on it.
                                        text:           modelData.label
                                                        + (modelData.noSensor ? " ⊘" : "")
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          parent.active ? Theme.colorBg : Theme.colorText2
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            root._what      = modelData.name
                                            root._quantity  = ""
                                            root._reference = ""
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── 3. Quantity — gated by the validity table ─────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)
                visible: root._what.length > 0

                Text {
                    text:                qsTr("MEASURED AS")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.sp(6)

                    Repeater {
                        // Only legal quantities are offered — the picker never shows a chip it
                        // would then reject.
                        model: root._what.length > 0 ? root.editor.quantitiesFor(root._what) : []
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root._quantity === modelData.name

                            implicitWidth:  qText.implicitWidth + Theme.sp(18)
                            implicitHeight: Theme.sp(24)
                            radius: height / 2
                            color:  active ? Theme.colorAccent : Theme.colorBg2

                            Text {
                                id: qText
                                anchors.centerIn: parent
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          parent.active ? Theme.colorBg : Theme.colorText2
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: { root._quantity = modelData.name; root._reference = "" }
                            }
                        }
                    }
                }
            }

            // ── 4. Reference ──────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)
                visible: root._quantity.length > 0

                Text {
                    text:                qsTr("RELATIVE TO")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Text {
                    Layout.fillWidth: true
                    // The distinction that keeps "ball too close to the body" and "ball too far
                    // forward" apart — they are otherwise indistinguishable.
                    text: qsTr("Distance to a line is measured across it; distance to a point is "
                               + "measured along the stance.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                    visible:        root._quantity === "distance"
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.sp(6)

                    Repeater {
                        model: root._quantity.length > 0
                               ? root.editor.referencesFor(root._what, root._quantity) : []
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root._reference === modelData.name

                            implicitWidth:  refText.implicitWidth + Theme.sp(18)
                            implicitHeight: Theme.sp(24)
                            radius: height / 2
                            color:  active ? Theme.colorAccent : Theme.colorBg2

                            Text {
                                id: refText
                                anchors.centerIn: parent
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          parent.active ? Theme.colorBg : Theme.colorText2
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root._reference = modelData.name
                            }
                        }
                    }
                }
            }

            PpDivider { Layout.fillWidth: true; visible: root._reference.length > 0 }

            // ── 5. Reducer — a SEPARATE step ──────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)
                visible: root._reference.length > 0

                Text {
                    text:                qsTr("READ AS")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.sp(6)

                    Repeater {
                        model: root.editor.reducerKinds
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root._reducerKind === modelData.name

                            implicitWidth:  redText.implicitWidth + Theme.sp(18)
                            implicitHeight: Theme.sp(24)
                            radius: height / 2
                            color:  active ? Theme.colorAccent : Theme.colorBg2

                            Text {
                                id: redText
                                anchors.centerIn: parent
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          parent.active ? Theme.colorBg : Theme.colorText2
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root._reducerKind = modelData.name
                            }
                        }
                    }
                }

                // Phase pickers. `At` needs one; the rest need a span.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(8)

                    Text {
                        text:           root._usesWindow ? qsTr("from") : qsTr("at")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }

                    Repeater {
                        model: root.editor.phases
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root._usesWindow
                                                           ? root._windowStart === modelData.token
                                                           : root._anchor === modelData.token
                            implicitWidth:  Theme.sp(30)
                            implicitHeight: Theme.sp(22)
                            radius: height / 2
                            color:  active ? Theme.colorAccent : Theme.colorBg2

                            Text {
                                anchors.centerIn: parent
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          parent.active ? Theme.colorBg : Theme.colorText2
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (root._usesWindow) {
                                        root._windowStart = modelData.token
                                        root._anchor      = modelData.token
                                    } else {
                                        root._anchor = modelData.token
                                    }
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(8)
                    visible: root._usesWindow

                    Text {
                        text:           qsTr("to")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }

                    Repeater {
                        model: root.editor.phases
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root._windowEnd === modelData.token
                            implicitWidth:  Theme.sp(30)
                            implicitHeight: Theme.sp(22)
                            radius: height / 2
                            color:  active ? Theme.colorAccent : Theme.colorBg2

                            Text {
                                anchors.centerIn: parent
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          parent.active ? Theme.colorBg : Theme.colorText2
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root._windowEnd = modelData.token
                            }
                        }
                    }
                }
            }

            PpDivider { Layout.fillWidth: true; visible: root._reference.length > 0 }

            // ── 6. Live result ────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)
                visible: root._reference.length > 0

                // Rejected combination: say why, in the author's terms.
                Text {
                    Layout.fillWidth: true
                    visible:        root._preview.valid === false
                    text:           root._preview.reason || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorRagFault
                    wrapMode:       Text.WordWrap
                }

                // The generated canonical name — identity is the facets, this is for humans.
                Text {
                    Layout.fillWidth: true
                    visible:        root._preview.valid === true
                    text:           root._preview.label || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody
                    color:          Theme.colorText
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible: root._preview.valid === true
                    text: {
                        var bits = []
                        if (root._preview.viewNeeded && root._preview.viewNeeded !== "any")
                            bits.push(root._preview.viewNeeded === "faceOn"
                                      ? qsTr("needs the face-on view")
                                      : qsTr("needs the down-the-line view"))
                        if (root._preview.status === "notCapturable")
                            bits.push(qsTr("not measurable from capture"))
                        else if (root._preview.status === "noProducer")
                            bits.push(qsTr("no producer yet — this is roadmap work"))
                        return bits.join(" · ")
                    }
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible:        (root._preview.gapReason || "").length > 0
                    text:           root._preview.gapReason || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                // "Did you mean" — shown prominently, AT THE MOMENT OF CREATION. Afterwards nobody
                // merges near-duplicates, so this is the only cheap moment to prevent them.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(4)
                    visible: root._preview.valid === true
                             && (root._preview.nearDuplicates || []).length > 0

                    Text {
                        text:           qsTr("Did you mean one of these?")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color:          Theme.colorRagWatch
                    }

                    Repeater {
                        model: root._preview.nearDuplicates || []
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: Theme.sp(36)
                            radius: Theme.radius
                            color:  dupMa.containsMouse ? Theme.colorBg2 : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin:  Theme.sp(10)
                                anchors.rightMargin: Theme.sp(10)
                                spacing: Theme.sp(8)

                                Text {
                                    Layout.fillWidth: true
                                    text:           modelData.label
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorText
                                    elide:          Text.ElideRight
                                }
                                Text {
                                    text: modelData.sameSeries
                                          ? qsTr("same measurement")
                                          : qsTr("one facet different")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }
                            }

                            MouseArea {
                                id: dupMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                // SELECTS, it does not commit. This used to emit measureChosen on
                                // the spot, which attached a signal at whatever tail the chips
                                // happened to be showing — a default the author had not seen yet,
                                // read against a different measure's convention.
                                onClicked: {
                                    root._reuseId        = modelData.id
                                    root._reuseLabel     = modelData.label
                                    root._reuseHighMeans = modelData.highMeans || ""
                                }
                            }
                        }
                    }
                }
            }

            // ── 7. Which tail fires ───────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)
                // Also shown for a reused near-duplicate: reusing is still choosing a tail.
                visible: root._preview.valid === true || root._reuseId.length > 0

                // Which measure the tail below belongs to, when it is not the one being built.
                Rectangle {
                    Layout.fillWidth: true
                    visible:        root._reuseId.length > 0
                    implicitHeight: Theme.sp(34)
                    radius:         Theme.radius
                    color:          Theme.colorBg2

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin:  Theme.sp(12)
                        anchors.rightMargin: Theme.sp(10)
                        spacing: Theme.sp(8)

                        Text {
                            Layout.fillWidth: true
                            text:           qsTr("Reusing: %1").arg(root._reuseLabel)
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color:          Theme.colorText
                            elide:          Text.ElideRight
                        }
                        Text {
                            text:           "✕"
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color:          Theme.colorText3
                            PpPressable {
                                anchors.margins: -Theme.sp(6)
                                onClicked: { root._reuseId = ""; root._reuseLabel = ""
                                             root._reuseHighMeans = "" }
                            }
                        }
                    }
                }

                Text {
                    text:                qsTr("FLAG WHEN IT IS")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Text {
                    Layout.fillWidth: true
                    // A condition is one tail of one corridor, so this is not optional.
                    text: qsTr("A characteristic is one side of a normal range. If both sides are "
                               + "worth naming, author them as two.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                // ── What a HIGH value means ───────────────────────────────────
                //
                // Asked BEFORE the tail is chosen, because the tail cannot be chosen correctly
                // without it. Three shipped signals pointed the wrong way for exactly this reason:
                // an author picked High or Low against a sign convention that was unstated, and an
                // inverted signal then fires happily on the wrong swings with correct-sounding
                // consequence text attached. See docs/design/pinpoint_sign_conventions.md.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(4)

                    Text {
                        text:                qsTr("A HIGHER VALUE MEANS")
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                    }

                    // Already stated by the measure: quoted, not re-asked.
                    Text {
                        Layout.fillWidth: true
                        visible:        root._existingHighMeans.length > 0
                        text:           root._existingHighMeans
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color:          Theme.colorText
                        wrapMode:       Text.WordWrap
                    }

                    PpTextField {
                        Layout.fillWidth: true
                        visible: root._existingHighMeans.length === 0
                        text:    root._highMeans
                        placeholderText: qsTr("e.g. “further back, toward the trail foot”")
                        onEditingFinished: root._highMeans = text
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root._existingHighMeans.length === 0
                                 && root._highMeans.trim().length === 0
                        text: qsTr("Say it the way a coach would, not as a sign. Where the outside "
                                   + "world already has a convention, follow it; otherwise positive "
                                   + "is toward the lead side.")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                    }
                }

                RowLayout {
                    spacing: Theme.sp(6)

                    Repeater {
                        model: root._directions
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root._direction === modelData.name

                            implicitWidth:  dirText.implicitWidth + Theme.sp(20)
                            implicitHeight: Theme.sp(26)
                            radius: height / 2
                            color:  active ? Theme.colorAccent : Theme.colorBg2

                            Text {
                                id: dirText
                                anchors.centerIn: parent
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          parent.active ? Theme.colorBg : Theme.colorText2
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root._direction = modelData.name
                            }
                        }
                    }
                }

                // The chosen tail, spelled out. Composed in C++ so which sentence belongs to which
                // direction is testable rather than a delegate's opinion.
                Text {
                    Layout.fillWidth: true
                    text: (root._direction === "low" ? root._directions[1] : root._directions[0])
                              .sentence
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    wrapMode:       Text.WordWrap
                }
            }

            // A missing sign convention is the one gap that cannot be caught later: an inverted
            // signal fires happily, with correct-sounding text, on the wrong swings. So both
            // actions below wait for it — but only for it. A measure with no PRODUCER still goes
            // through, because that is a roadmap item, not a mistake.
            Text {
                Layout.fillWidth: true
                visible: (root._preview.valid === true || root._reuseId.length > 0)
                         && root._convention.trim().length === 0
                text:    qsTr("Say what a higher value means before choosing which side fires.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorRagWatch
                wrapMode:       Text.WordWrap
            }

            // ── 8. Actions ────────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(10)

                PpButton {
                    label: qsTr("Cancel")
                    onClicked: root.cancelled()
                }

                Item { Layout.fillWidth: true }

                PpButton {
                    // A near-duplicate the author chose to reuse. Takes precedence over both
                    // buttons below: they act on the measure being BUILT, which is no longer what
                    // is being attached.
                    visible: root._reuseId.length > 0
                    enabled: root._convention.trim().length > 0
                    label:   qsTr("Use this measure")
                    primary: true
                    onClicked: {
                        if (root._reuseHighMeans.length === 0)
                            root.editor.setMeasureHighMeans(root._reuseId, root._highMeans)
                        root.measureChosen(root._reuseId, root._direction)
                    }
                }

                PpButton {
                    // Reuse first when the exact measure already exists.
                    visible: root._reuseId.length === 0
                             && root._preview.exactMatch !== undefined
                             && root._preview.exactMatch !== null
                    enabled: root._convention.trim().length > 0
                    label:   qsTr("Use existing")
                    primary: true
                    onClicked: {
                        // The author may have supplied the convention this existing measure lacked
                        // — carry it back, or the next author faces the same blank.
                        if (root._existingHighMeans.length === 0)
                            root.editor.setMeasureHighMeans(root._preview.exactMatch.id,
                                                            root._highMeans)
                        root.measureChosen(root._preview.exactMatch.id, root._direction)
                    }
                }

                PpButton {
                    // Never block the author: a measure with no producer is an expected outcome and
                    // is exactly what the roadmap is built from.
                    visible: root._reuseId.length === 0
                             && root._preview.valid === true
                             && (root._preview.exactMatch === undefined
                                 || root._preview.exactMatch === null)
                    enabled: root._convention.trim().length > 0
                    label:   qsTr("Create this measure")
                    primary: true
                    onClicked: {
                        var id = root.editor.mintMeasure(root._mintFacets())
                        if (id.length > 0) root.measureChosen(id, root._direction)
                    }
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(20) }
        }
    }
}
