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

// The bibliography, and what each paper actually holds up.
//
// A reference list on its own is an appendix nobody opens. The question a reader has when they
// arrive here is "why does the app believe this?", so every entry pairs the paper with the causal
// claims resting on it, at the tier each one earned — and the list is ordered by how much of the
// library each carries, because that ordering IS the argument.
//
// Papers nothing cites are KEPT and marked. One of them contradicts two claims the pack does make,
// and a bibliography that quietly dropped it would be the most misleading version of this view we
// could ship.
//
// Two kinds of record, one list. Most rows are here because something cites them; the rows under
// GENERAL READING are the field's standard works, here so a reader can find the reading itself. The
// flag is ADDITIVE — a cited paper can also be standard reading, and one that is appears ONCE, in
// its normal position among the cited rows, wearing the badge rather than being duplicated into a
// second list. A record that is neither cited nor flagged is nobody's decision yet, sits at the tail
// of the first section, and raises `referenceOrphan` in the health list.
Item {
    id: root

    required property var library     // CharacteristicLibraryModel

    signal openCondition(string id)

    property int _revision: 0
    readonly property var _refs: (root._revision >= 0) ? root.library.references() : []

    // ── Arriving from a citation elsewhere ───────────────────────────────────
    //
    // Deep link to ONE paper. The caller switches the view and calls this in the SAME frame, so the
    // rows are not laid out yet and `itemAt()` can still be null — hence a deferred retry rather
    // than a direct scroll. `_focusId` outlives the scroll because the flash binding matches on it.
    property string _focusId: ""
    property bool   _flash:   false

    function focusReference(referenceId) {
        if (!referenceId || referenceId.length === 0) return false
        if (root._indexOf(referenceId) < 0) return false   // unknown paper: report it, don't jump
        root._focusId    = referenceId
        focusTimer._tries = 0
        focusTimer.restart()
        return true
    }

    function _indexOf(referenceId) {
        for (var i = 0; i < root._refs.length; ++i)
            if (root._refs[i].id === referenceId) return i
        return -1
    }

    Timer {
        id:       focusTimer
        interval: 16          // one frame: long enough for the Repeater to have positioned rows
        repeat:   false

        // Bounded, because "wait for the layout" with no ceiling is a timer that runs for the life
        // of the panel if the row never appears.
        property int _tries: 0

        onTriggered: {
            var i = root._indexOf(root._focusId)
            if (i < 0) return

            var item = refRepeater.itemAt(i)
            if (!item) {
                if (++focusTimer._tries < 10) focusTimer.restart()
                return
            }

            // Land the row at the TOP of the viewport. Merely scrolling it into view puts it
            // anywhere on the page, which leaves the reader hunting for what they just clicked.
            //
            // Driven through the attached scrollbar rather than contentItem.contentY: ScrollView
            // types its contentItem as a bare Item, so reaching for Flickable members there works
            // at runtime and cannot be checked at build time. `content` is the layout itself and
            // its height IS the content height.
            var vbar   = scroller.ScrollBar.vertical
            var maxTop = Math.max(0, content.height - scroller.height)
            var top    = Math.max(0, Math.min(item.y, maxTop))
            if (vbar && content.height > 0) vbar.position = top / content.height

            root._flash = true
            flashTimer.restart()
        }
    }

    // A brief accent on the two lines that identify the paper. The row is already at the top, so
    // this confirms the landing rather than carrying it — anything stronger would read as a
    // selection state, and nothing here is selected.
    Timer {
        id:       flashTimer
        interval: 1100
        repeat:   false
        onTriggered: root._flash = false
    }

    // healthChanged, not libraryChanged: `libraryChanged` belongs to CharacteristicLibrary.qml,
    // not to the model, and Connections cannot tell an unmatched handler from a typo at compile
    // time — it warns at INSTANTIATION, which for a lazily-loaded settings panel means the first
    // time somebody opens it. `refresh()` emits healthChanged, and both sibling views key off it.
    Connections {
        target: root.library
        function onHealthChanged() { root._revision++ }
    }

    component SectionHead : Text {
        font.family:         Theme.fontBody
        font.pixelSize:      Theme.fontSzMicro
        font.letterSpacing:  Theme.trackingMicro
        font.capitalization: Font.AllUppercase
        color:               Theme.colorText3
    }

    // Provenance, not verdict. Muted on purpose — a bright chip here would out-shout the claim it
    // is qualifying, and the tier is a caveat rather than a score.
    function _tierColor(tier) {
        switch (tier) {
        case "established":   return Theme.colorGood
        case "supported":     return Theme.colorGood
        case "indirect":      return Theme.colorAccent
        case "noSourceFound": return Theme.colorWarn
        }
        return Theme.colorText3          // practice, proposed
    }

    ScrollView {
        id:              scroller
        anchors.fill:    parent
        anchors.margins: Theme.sp(20)
        clip:            true

        ColumnLayout {
            id:      content
            width:   parent.width
            spacing: Theme.sp(16)

            SectionHead { text: qsTr("REFERENCES") }

            Text {
                Layout.fillWidth: true
                text: qsTr("Every source behind a citation in the library, with the claims resting "
                           + "on it and the tier each one earned. Ordered by how much each source "
                           + "holds up. Tap a source to open it — at the publisher for a paper, at "
                           + "a library catalogue for a book; tap a claim to go to it. "
                           + "Most of the library is coaching practice rather than published "
                           + "measurement — where that is so, no source is listed and the claim "
                           + "says as much.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            Repeater {
                id:    refRepeater
                model: root._refs

                delegate: ColumnLayout {
                    id: rrow
                    required property var modelData
                    required property int index

                    // True only for the row a citation link just landed on, and only while the
                    // flash runs.
                    readonly property bool _landed:
                        root._flash && root._focusId === rrow.modelData.id

                    readonly property bool _general: rrow.modelData.generalReading === true

                    // The GENERAL READING boundary. The model sorts the uncited tail so every
                    // flagged record follows every unflagged one, so the section starts at the
                    // first flagged row and this is simply "am I it?". Drawn inside the delegate
                    // rather than by splitting the Repeater — see the sort comment in
                    // characteristic_library_model.cpp for why the list must stay one sequence.
                    readonly property bool _startsGeneral: {
                        if (!rrow._general || rrow.modelData.citeCount !== 0) return false
                        if (rrow.index === 0) return true
                        var prev = root._refs[rrow.index - 1]
                        return !(prev.generalReading === true && prev.citeCount === 0)
                    }

                    Layout.fillWidth: true
                    spacing:          Theme.sp(4)

                    // ── The general-reading heading, when this row opens it ─────
                    ColumnLayout {
                        Layout.fillWidth:  true
                        Layout.topMargin:  Theme.sp(12)
                        Layout.bottomMargin: Theme.sp(4)
                        spacing:           Theme.sp(6)
                        visible:           rrow._startsGeneral

                        SectionHead { text: qsTr("GENERAL READING") }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("The field's standard works. Nothing in the library cites "
                                       + "these — they are here so a reader can find the reading, "
                                       + "not because a claim rests on them.")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                        }
                    }

                    // ── The paper ──────────────────────────────────────────────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing:          Theme.sp(8)

                        Text {
                            Layout.fillWidth: true
                            text:             rrow.modelData.title || rrow.modelData.identifier
                            wrapMode:         Text.WordWrap
                            font.family:      Theme.fontBody
                            font.pixelSize:   Theme.fontSzBody
                            color:            (paperMa.containsMouse || rrow._landed)
                                              ? Theme.colorAccent : Theme.colorText
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        // Only where it says something the layout does not. A flagged record in the
                        // GENERAL READING section already sits under a heading that reads the same
                        // way, and a badge on every row there would be the heading repeated N times.
                        // Up here, among the cited rows, it is the ONLY thing that says this paper
                        // would earn its place even if the claims resting on it went away — which is
                        // the case the additive flag exists for.
                        Text {
                            visible:        rrow._general && rrow.modelData.citeCount > 0
                            text:           qsTr("general reading")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            opacity:        0.75
                        }

                        // The count leads, because it is what the ordering is on: a reader
                        // scanning for what matters most should not have to count rows.
                        Text {
                            text: rrow.modelData.citeCount > 0
                                  ? qsTr("supports %1").arg(rrow.modelData.citeCount)
                                  : qsTr("cited by nothing")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            font.italic:    rrow.modelData.citeCount === 0
                            color:          Theme.colorText3
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode:         Text.WordWrap
                        font.family:      Theme.fontBody
                        font.pixelSize:   Theme.fontSzMicro
                        color:            Theme.colorText2
                        text: {
                            var bits = []
                            if (rrow.modelData.authors) bits.push(rrow.modelData.authors)
                            // A book has a publisher where a paper has a journal. One or the other,
                            // never both — the slot names the source's container, and a row that
                            // showed a journal AND a publisher would be claiming to be both.
                            if (rrow.modelData.journal)        bits.push(rrow.modelData.journal)
                            else if (rrow.modelData.publisher) bits.push(rrow.modelData.publisher)
                            if (rrow.modelData.year)    bits.push(String(rrow.modelData.year))
                            return bits.join("  ·  ")
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text:             rrow.modelData.identifier
                        wrapMode:         Text.WrapAnywhere
                        font.family:      Theme.fontData
                        font.pixelSize:   Theme.fontSzMicro
                        color:            (paperMa.containsMouse || rrow._landed)
                                          ? Theme.colorAccent : Theme.colorText3
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                        MouseArea {
                            id:           paperMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape:  Qt.PointingHandCursor
                            onClicked: if (rrow.modelData.url) Qt.openUrlExternally(rrow.modelData.url)
                        }
                    }

                    // What it establishes, in our words — the one thing a DOI cannot say, and what
                    // lets a reader tell an indirect citation from a supported one without opening
                    // the paper.
                    Text {
                        visible:          (rrow.modelData.establishes || "").length > 0
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.sp(2)
                        text:             rrow.modelData.establishes || ""
                        wrapMode:         Text.WordWrap
                        font.family:      Theme.fontBody
                        font.pixelSize:   Theme.fontSzMicro
                        color:            Theme.colorText2
                    }

                    // ── What rests on it ───────────────────────────────────────
                    Repeater {
                        model: rrow.modelData.cites || []

                        delegate: RowLayout {
                            id: crow
                            required property var modelData

                            Layout.fillWidth: true
                            Layout.leftMargin: Theme.sp(12)
                            spacing:           Theme.sp(8)

                            Rectangle {
                                implicitWidth:  tierTxt.implicitWidth + Theme.sp(12)
                                implicitHeight: Theme.sp(16)
                                radius:         height / 2
                                color:          "transparent"
                                border.width:   1
                                border.color:   root._tierColor(crow.modelData.tier)

                                Text {
                                    id:               tierTxt
                                    anchors.centerIn: parent
                                    text:             crow.modelData.tierLabel || ""
                                    font.family:      Theme.fontBody
                                    font.pixelSize:   Theme.fontSzMicro
                                    color:            root._tierColor(crow.modelData.tier)
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                wrapMode:         Text.WordWrap
                                font.family:      Theme.fontBody
                                font.pixelSize:   Theme.fontSzMicro
                                color:            claimMa.containsMouse ? Theme.colorAccent
                                                                        : Theme.colorText2
                                text: crow.modelData.kind === "edge"
                                      ? crow.modelData.from + "  →  " + crow.modelData.to
                                      : crow.modelData.from
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                                MouseArea {
                                    id:           claimMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape:  Qt.PointingHandCursor
                                    // The cause end of an edge, the condition itself otherwise —
                                    // either way it opens something real.
                                    onClicked: root.openCondition(crow.modelData.fromId)
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth:  true
                        Layout.topMargin:  Theme.sp(8)
                        implicitHeight:    1
                        color:             Theme.colorBorderMid
                    }
                }
            }
        }
    }
}
