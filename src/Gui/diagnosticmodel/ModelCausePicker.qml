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

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

// "Which cause?" — asked with ONE text field that does both jobs: it narrows the conditions that
// already exist, and it names the one that does not.
//
// ── Why both halves are here rather than in two controls ────────────────────────────────────────
//
// The graph draws a NEIGHBOURHOOD, and a neighbourhood is drawn because its members are connected
// to the focus. So nearly every node on the canvas is already linked to it, and "already linked" is
// a refusal — which leaves a link drag with almost nothing legal to land on, and makes dragging to
// an on-screen node the RARE case rather than the common one. The cause an author actually wants is
// usually off the canvas entirely.
//
// A create-only popover at the end of that drag therefore answered the wrong question: it offered
// the one thing you can always do (make another condition) and withheld the one you usually want
// (name an existing one). The library filling with near-duplicates is the predictable consequence,
// and it is the failure mode that cannot be corrected after the fact because nobody merges them.
//
// So the list comes first and creation is the fallback, which is also the order of the two answers'
// likelihood. The candidates are PRE-FILTERED to legal ones in C++ — no self, no existing edge, no
// cycle — so an illegal claim cannot be constructed here, only refused before it is offered.
Popup {
    id: root

    property string title: qsTr("Add a cause")
    // (searchText) -> [{ id, label, detail, reachLabel }]. Asked again on every keystroke, so the
    // filtering and the legality both stay in C++.
    property var candidateSource: null

    // Creation is offered from the canvas, where the author is standing at a point they chose and
    // "make one here" is meaningful. It is offered from the inspector too, because §7's rule is
    // that the two entrances reach the same verbs.
    property bool allowCreate: true

    // Which way round the claim being made runs. The candidate list is already filtered for the
    // right end by the caller — this only decides the WORDS, and it decides them here rather than
    // at each call site so the two directions cannot drift into two vocabularies.
    property bool upstream: true
    // Only the canvas has somewhere to put a ghost box.
    property bool hasGhost: false
    property real ghostX: 0
    property real ghostY: 0

    signal picked(string id)
    signal created(string objType, string name)

    width:   Theme.sp(380)
    padding: 0
    modal:   false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string _type: "characteristics"
    property bool   _took: false

    background: Rectangle {
        color:        Theme.colorSurface
        radius:       Theme.radius
        border.width: 1
        border.color: Theme.colorAccent
    }

    readonly property var _candidates:
        candidateSource ? candidateSource(nameField.text) : []

    // The list an author navigates: every legal existing condition, and then — only once something
    // has been typed to name it with — the row that makes a new one. One list, so Down/Down/Enter
    // reaches either without the hand leaving the keyboard.
    readonly property var _rows: {
        var out = []
        var c = root._candidates
        for (var i = 0; i < c.length; i++)
            out.push({ kind: "pick", id: c[i].id, label: c[i].label,
                       detail: c[i].detail || "", tone: c[i].reachLabel || "" })
        if (root.allowCreate && nameField.text.trim().length > 0)
            out.push({ kind: "create", id: "", label: nameField.text.trim(), detail: "", tone: "" })
        return out
    }

    function openAt(canvasX, canvasY, layoutX, layoutY) {
        root.hasGhost = true
        root.ghostX = layoutX - Theme.sp(80)
        root.ghostY = layoutY - Theme.sp(17)
        var pw = root.parent ? root.parent.width  : 0
        var ph = root.parent ? root.parent.height : 0
        root.x = Math.max(Theme.sp(8), Math.min(pw - root.width - Theme.sp(8),
                                                canvasX - root.width / 2))
        root.y = Math.min(ph - Theme.sp(150), canvasY + Theme.sp(26))
        root.open()
    }

    onOpened: {
        root._took = false
        root._type = "characteristics"
        nameField.text = ""
        nameField.forceActiveFocus()
        listView.currentIndex = 0
    }
    // Dismissed without a choice is a cancelled gesture, and a cancelled gesture is not an error —
    // nothing is said about it, here or anywhere.
    onClosed: { root.hasGhost = false; if (!root._took) root.rejected() }
    signal rejected()

    function _take() {
        var rows = root._rows
        if (rows.length === 0) return
        var i = Math.max(0, Math.min(listView.currentIndex, rows.length - 1))
        var r = rows[i]
        root._took = true
        if (r.kind === "create") root.created(root._type, r.label)
        else                     root.picked(r.id)
        root.close()
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp(6)

        Text {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(12)
            Layout.rightMargin: Theme.sp(12)
            Layout.topMargin:   Theme.sp(10)
            text:                root.title
            font.family:         Theme.fontBody
            font.pixelSize:      Theme.fontSzMicro
            font.letterSpacing:  Theme.trackingMicro
            font.capitalization: Font.AllUppercase
            color:               Theme.colorText3
        }

        PpTextField {
            id: nameField
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(10)
            Layout.rightMargin: Theme.sp(10)
            placeholderText: root.allowCreate ? qsTr("Find one, or name a new one…")
                                              : qsTr("Type to narrow…")

            // Three characters is usually enough to leave one candidate, which is the whole budget
            // this control is designed around.
            Keys.onReturnPressed: root._take()
            Keys.onEnterPressed:  root._take()
            Keys.onDownPressed:   listView.currentIndex = Math.min(listView.currentIndex + 1,
                                                                   listView.count - 1)
            Keys.onUpPressed:     listView.currentIndex = Math.max(listView.currentIndex - 1, 0)
            // Typing changes what the list holds, so the highlight goes back to the top rather than
            // staying on whichever row happens to be at that index now.
            onTextChanged: listView.currentIndex = 0
        }

        Text {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(12)
            Layout.rightMargin: Theme.sp(12)
            text: root._candidates.length === 0
                      ? (root.allowCreate ? qsTr("Nothing existing fits — name it and press Enter")
                                          : qsTr("No legal target"))
                      : root.upstream
                            ? qsTr("%n existing condition(s) could cause this", "",
                                   root._candidates.length)
                            : qsTr("%n existing condition(s) this could cause", "",
                                   root._candidates.length)
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
            wrapMode:       Text.WordWrap
        }

        ListView {
            id: listView
            Layout.fillWidth:       true
            Layout.preferredHeight: Math.min(Theme.sp(220),
                                             Math.max(Theme.sp(28), count * Theme.sp(28)))
            Layout.bottomMargin:    Theme.sp(2)
            clip:  true
            model: root._rows
            currentIndex: 0

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: rowItem
                required property var modelData
                required property int index

                width:  listView.width
                height: Theme.sp(28)
                color: listView.currentIndex === index ? Theme.colorAccentLight
                     : rowHover.hovered                ? Theme.colorBg2
                                                       : "transparent"

                HoverHandler { id: rowHover }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin:  Theme.sp(12)
                    anchors.rightMargin: Theme.sp(12)
                    spacing: Theme.sp(8)

                    // The create row is marked rather than merely last: a list where the final
                    // entry silently means something different from the rest is a list that will
                    // be chosen by accident.
                    Text {
                        text: rowItem.modelData.kind === "create" ? "＋" : "◇"
                        font.family:    Theme.fontSymbol
                        font.pixelSize: Theme.fontSzBody2
                        color: rowItem.modelData.kind === "create" ? Theme.colorAccent
                                                                   : Theme.colorText3
                    }

                    Text {
                        Layout.fillWidth: true
                        text: rowItem.modelData.kind === "create"
                                  ? qsTr("Create “%1”").arg(rowItem.modelData.label)
                                  : rowItem.modelData.label
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color:          Theme.colorText
                        elide:          Text.ElideRight
                    }

                    Text {
                        text: rowItem.modelData.kind === "create"
                                  ? root._typeLabel(root._type)
                                  : rowItem.modelData.detail
                        visible: text.length > 0
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        elide:          Text.ElideRight
                        Layout.maximumWidth: root.width * 0.36
                    }
                }

                PpPressable {
                    hoverScale: 1.0
                    onClicked: { listView.currentIndex = rowItem.index; root._take() }
                }
            }
        }

        // The four types §5.3 asks for, and what each one MEANS once it lands: only a condition can
        // be a cause. A screen settles the characteristic, a drill answers it, a measure detects it
        // — so the chips do not change what a cause is, they change what is being made here and how
        // it attaches. Hidden entirely when there is nothing to create.
        RowLayout {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(10)
            Layout.rightMargin: Theme.sp(10)
            visible: root.allowCreate && nameField.text.trim().length > 0
            spacing: Theme.sp(6)

            component Chip: Rectangle {
                id: chip
                required property string label
                required property string key

                radius: height / 2
                implicitWidth: chipText.implicitWidth + Theme.sp(18)
                Layout.preferredWidth:  implicitWidth
                Layout.preferredHeight: Theme.sp(22)
                color: root._type === key ? Theme.colorAccentLight : Theme.colorBg2
                border.width: 1
                border.color: root._type === key ? Theme.colorAccent : Theme.colorBorderMid

                Text {
                    id: chipText
                    anchors.centerIn: parent
                    text: chip.label
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color: root._type === chip.key ? Theme.colorAccent : Theme.colorText2
                }

                PpPressable {
                    hoverScale: 1.0
                    onClicked: {
                        root._type = chip.key
                        // Picking a type is saying "make one", so the highlight moves to the row
                        // that would — otherwise the chip would silently arm an Enter that still
                        // took whatever condition happened to be at the top of the list.
                        listView.currentIndex = root._rows.length - 1
                        nameField.forceActiveFocus()
                    }
                }
            }

            Chip { label: qsTr("characteristic"); key: "characteristics" }
            Chip { label: qsTr("measure");        key: "measures" }
            Chip { label: qsTr("screen");         key: "screens" }
            Chip { label: qsTr("drill");          key: "drills" }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(12)
            Layout.rightMargin:  Theme.sp(12)
            Layout.bottomMargin: Theme.sp(10)
            visible: root.allowCreate && nameField.text.trim().length > 0
            text: root._type === "measures"
                      ? qsTr("Measures are minted from their facets — Enter opens that.")
                      : qsTr("Created as a draft, attached, and undone as one step.")
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
            wrapMode:       Text.WordWrap
        }
    }

    function _typeLabel(t) {
        switch (t) {
        case "measures": return qsTr("new measure")
        case "screens":  return qsTr("new screen")
        case "drills":   return qsTr("new drill")
        }
        return qsTr("new characteristic")
    }
}
