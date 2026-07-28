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

// Two lists that answer different questions.
//
// Cause coverage answers "what should a coach actually do?" — and its screened block is the
// highest-value output in the product, because those causes need no capture hardware at all.
//
// The health list answers "what is wrong with this library?" — and it is literally the validator's
// warnings, not a second opinion computed somewhere else.
Item {
    id: root

    required property var library     // CharacteristicLibraryModel

    signal openCondition(string conditionId)
    // A row about a NORM opens the measure, not a characteristic: that page carries the corridor, the
    // shipped-vs-yours markers and the reset. Following a name has to land somewhere you can act.
    signal openMeasure(string measureId)

    readonly property var _coverage: root.library.causeCoverage()

    // ── Search ────────────────────────────────────────────────────────────────
    // One box over all three lists on this page, because they are read as one page: a
    // measure id you are chasing appears in the health list AND the corridor scan, and two
    // boxes would mean finding it twice.
    //
    // Filtered HERE rather than in the model: these lists are computed products — a coverage
    // ranking, the validator's own findings, a corpus scan — not queries that take filters,
    // and none is long enough for a JS pass to matter.
    property string _search: ""

    function _hitText(s) {
        if (root._search.length === 0) return true
        return s !== undefined && s !== null
               && String(s).toLowerCase().indexOf(root._search.toLowerCase()) !== -1
    }

    function _searchCoverage(list) {
        if (root._search.length === 0) return list
        return list.filter(function(r) {
            return root._hitText(r.label) || root._hitText(r.id) || root._hitText(r.reachLabel)
        })
    }

    // The code LABEL is matched as well as the code: the heading is what the reader sees, so
    // searching the words in front of them has to work.
    function _searchFindings(list) {
        if (root._search.length === 0) return list
        return list.filter(function(r) {
            return root._hitText(r.message) || root._hitText(r.subject)
                   || root._hitText(r.measureId) || root._hitText(r.code)
                   || root._hitText(root._codeLabel(r.code))
        })
    }

    // Re-read when the library says so — the health list spans the norm set now, so a corridor edit
    // can add or clear a row while this view is open.
    property int _revision: 0
    readonly property var _health: (root._revision >= 0) ? root.library.health() : []
    readonly property var _corpus: (root._revision >= 0) ? root.library.corpusHealth() : []

    Connections {
        target: root.library
        function onHealthChanged() { root._revision++ }
        function onCorpusChanged() { root._revision++ }
    }

    // Human-readable heading per validator code. An author should never have to look up what
    // "singleTailAxis" means to act on it.
    function _codeLabel(code) {
        switch (code) {
        case "proposedTier":       return qsTr("No citation — badged Proposed")
        case "singleTailAxis":     return qsTr("Only one side of the range is named")
        case "noCause":            return qsTr("Can be reported but never explained")
        case "noResolvableCause":  return qsTr("Only Behavioural causes — can be offered, never concluded")
        case "orphanCause":        return qsTr("Explains nothing")
        case "unusedMeasure":      return qsTr("Measure nothing uses")
        case "duplicateMeasure":   return qsTr("One number described twice — a corridor on each grades it two ways")
        case "observableNoSignal": return qsTr("Nothing detects it")
        case "needsRevalidation":  return qsTr("Flagged for revalidation")
        case "inconsistentReach":  return qsTr("Reach and detection disagree")
        case "screenedHasCause":   return qsTr("A screen result something claims to cause — check edge direction")
        case "duplicateId":        return qsTr("Redefined by another pack")
        case "bothTailsOneCondition": return qsTr("Flags both sides at once — it cannot tell too much from too little")
        case "duplicateAlias":     return qsTr("Two characteristics answer to one coach term")
        case "unknownScreenRef":   return qsTr("Names a screen that is not in the screen set")
        case "unknownDrillRef":    return qsTr("Names a drill that is not in the drill set")
        case "screenNoProtocol":   return qsTr("A screen with no protocol — nobody could run it")
        case "screenNoPass":       return qsTr("A screen with no pass criterion — the answer is unrecordable")
        case "screenIdNamespace":  return qsTr("A screen id outside the screen. namespace")
        case "screenUnitMissing":  return qsTr("A numeric pass floor with no unit")
        case "drillNoInstruction": return qsTr("A drill that does not say what the golfer does")
        case "drillNoTarget":      return qsTr("A drill that does not say what it is trying to change")
        case "drillIdNamespace":   return qsTr("A drill id outside the drill. namespace")
        case "externalDeviceNoReason": return qsTr("Reads from an external device but does not say which")

        // The norm side. These reached this list for the first time at stage 10: the norm set's own
        // warnings were promised to be part of it and were not, and the referential checks below had
        // never been run by anything but their test.
        case "zeroSigma":          return qsTr("A corridor with no tolerance — only its exact centre is Ideal")
        case "noProvenance":       return qsTr("Claims a published source with no citation")
        case "normUnitMismatch":   return qsTr("Corridor and measure are in different units")
        case "unknownNormMeasure": return qsTr("Corridor on a measure the library does not have")
        case "unknownNormContext": return qsTr("Corridor in a context the tree does not have")
        case "normNotCapturable":  return qsTr("Corridor on something no sensor can ever produce")

        // The assembled-library checks.
        case "signalNoNorm":           return qsTr("Nothing to compare against — the signal cannot fire")
        case "signalOnOpenTail":       return qsTr("Watching a tail this measure never grades")
        case "personalNormNoSample":   return qsTr("Your corridor, seated on no swings")
        case "clubDependentNoContext": return qsTr("Club-dependent, but graded by one corridor")
        case "emptyContext":           return qsTr("A context that changes no grade")
        case "ungradedContext":        return qsTr("A context graded by nothing at all")
        case "overrideCoreChanged":    return qsTr("The shipped corridor has been revised since you changed it")
        case "oneBandCorpus":          return qsTr("Grades almost the whole library into one band")
        }
        return code
    }

    // Every row's own code, in the order the codes first appear, over whichever list is being shown.
    function _codesOf(list) {
        var seen = []
        for (var i = 0; i < list.length; ++i)
            if (seen.indexOf(list[i].code) === -1) seen.push(list[i].code)
        return seen
    }
    function _rowsOf(list, code) {
        var out = []
        for (var i = 0; i < list.length; ++i)
            if (list[i].code === code) out.push(list[i])
        return out
    }

    // What the page actually renders, once the search has had its say.
    function _shownHealth()   { return root._searchFindings(root._health) }
    function _shownCorpus()   { return root._searchFindings(root._corpus) }
    function _shownCoverage() { return root._searchCoverage(root._coverage) }

    function _healthCodes() { return root._codesOf(root._shownHealth()) }
    function _healthFor(code) { return root._rowsOf(root._shownHealth(), code) }

    // Following a row: a norm key opens the measure, anything else opens the characteristic.
    function _followRow(row) {
        if (row.measureId && row.measureId.length > 0) root.openMeasure(row.measureId)
        else if (row.subject && row.subject.length > 0) root.openCondition(row.subject)
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(12)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(18)

            Text {
                text:                qsTr("MAINTENANCE")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            PpDisplayText { text: qsTr("Causes and health") }

            // Sits under the title, above the first list, because it governs all three.
            PpTextField {
                Layout.fillWidth: true
                Layout.maximumWidth: Theme.sp(380)
                placeholderText: qsTr("Search causes and findings")
                text: root._search
                onTextChanged: root._search = text
            }

            // ══ Cause coverage ════════════════════════════════════════════════
            Text {
                Layout.fillWidth: true
                text: qsTr("Causes ranked by how much of the library they explain. A handful "
                           + "explaining most of it is the point of the model — a graph where every "
                           + "characteristic has its own private cause is a restated fault list, "
                           + "not a diagnosis.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                visible:        root._search.length > 0 && root._shownCoverage().length === 0
                text:           qsTr("No cause matches “%1”.").arg(root._search)
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText3
            }

            Repeater {
                model: root._shownCoverage()
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: Theme.sp(50)
                    radius: Theme.radius
                    color:  covMa.containsMouse ? Theme.colorBg2 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin:  Theme.sp(12)
                        anchors.rightMargin: Theme.sp(12)
                        spacing: Theme.sp(12)

                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth:  Theme.sp(28)
                            implicitHeight: Theme.sp(28)
                            radius: width / 2
                            color:  Theme.colorBg2

                            Text {
                                anchors.centerIn: parent
                                text:           modelData.coverage
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorText
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(2)

                            Text {
                                Layout.fillWidth: true
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorText
                                elide:          Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                // Says plainly that this can never be measured here, so nobody reads
                                // it as a producer that has not arrived yet.
                                text: modelData.outsideCaptureReach
                                      ? modelData.reachHint + " · " + qsTr("never measured from a swing")
                                      : modelData.reachHint
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                elide:          Text.ElideRight
                            }
                        }

                        Rectangle {
                            visible: modelData.reach !== "measured"
                            implicitWidth:  reachTxt.implicitWidth + Theme.sp(14)
                            implicitHeight: Theme.sp(20)
                            radius: height / 2
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.colorText3

                            Text {
                                id: reachTxt
                                anchors.centerIn: parent
                                text:           modelData.reachLabel
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }
                    }

                    MouseArea {
                        id: covMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape:  Qt.PointingHandCursor
                        onClicked: root.openCondition(modelData.id)
                    }
                }
            }

            PpDivider { Layout.fillWidth: true }

            // ══ Health list ═══════════════════════════════════════════════════
            Text {
                text:                qsTr("HEALTH")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            // Counts what is SHOWN. Under a search, "nothing outstanding" would be a lie about
            // the library rather than a statement about the filter, so the two are worded apart.
            Text {
                Layout.fillWidth: true
                text: {
                    var n = root._shownHealth().length
                    if (n > 0)
                        return qsTr("%n thing(s) worth a look. None of these stop the library "
                                    + "working — they are gaps and unfinished edges, not errors.",
                                    "", n)
                    return root._search.length > 0 ? qsTr("No finding matches “%1”.").arg(root._search)
                                                   : qsTr("Nothing outstanding.")
                }
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            // One row per finding, grouped under its code.
            //
            // The MESSAGE is rendered, which it never used to be: the list showed a subject chip and
            // dropped the sentence explaining it. That was survivable while every code was a
            // one-word structural gap an author could infer, and it is not survivable now — half
            // these rows carry the numbers, the context and the reason, and a row you cannot read is
            // a row nobody can act on.
            Repeater {
                model: root._healthCodes()
                delegate: ColumnLayout {
                    id: codeBlock
                    required property var modelData
                    readonly property var rows: root._healthFor(modelData)

                    Layout.fillWidth: true
                    spacing: Theme.sp(4)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(8)
                        Text {
                            text:           root._codeLabel(codeBlock.modelData)
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color:          Theme.colorText
                        }
                        Text {
                            text:           codeBlock.rows.length
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Repeater {
                        model: codeBlock.rows
                        delegate: Rectangle {
                            id: rowItem
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: rowCol.implicitHeight + Theme.sp(14)
                            radius: Theme.radius
                            color:  rowMa.containsMouse ? Theme.colorBg2 : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin:  Theme.sp(12)
                                anchors.rightMargin: Theme.sp(12)
                                spacing: Theme.sp(10)

                                // An error among warnings has to look different. These are not pack
                                // load errors — the library on screen IS the one they describe — so
                                // they are shown rather than swallowed, and marked rather than
                                // levelled with the gaps around them.
                                Rectangle {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.topMargin: Theme.sp(4)
                                    implicitWidth:  Theme.sp(6)
                                    implicitHeight: Theme.sp(6)
                                    radius: width / 2
                                    color: rowItem.modelData.severity === "error"
                                           ? Theme.colorError : Theme.colorText3
                                }

                                ColumnLayout {
                                    id: rowCol
                                    Layout.fillWidth: true
                                    spacing: Theme.sp(2)

                                    Text {
                                        Layout.fillWidth: true
                                        text:           rowItem.modelData.message || ""
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        color:          Theme.colorText2
                                        wrapMode:       Text.WordWrap
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text:           rowItem.modelData.subject || ""
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                        elide:          Text.ElideRight
                                    }
                                }
                            }

                            MouseArea {
                                id: rowMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                // Plain MouseArea, and the handler calls a method on `root`: inside a
                                // Repeater delegate the only file-level id that resolves is the
                                // component root, and a handler on a composite type cannot see even
                                // that. It throws only when clicked.
                                onClicked: root._followRow(rowItem.modelData)
                            }
                        }
                    }

                    Item { Layout.fillWidth: true; implicitHeight: Theme.sp(6) }
                }
            }

            PpDivider { Layout.fillWidth: true }

            // ══ Corridors against the library ═════════════════════════════════
            //
            // The only check here that can catch a corridor which is merely WRONG rather than
            // malformed — every other row is decidable from the registries alone. It needs a pass
            // over the swing library, so it is opt-in and says what it has and has not looked at:
            // "nothing found" and "nothing checked" must not read the same.
            Text {
                text:                qsTr("CORRIDORS vs THE LIBRARY")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Grades every stored swing against every corridor and looks for one that "
                           + "puts almost everything in a single band. A corridor grading the whole "
                           + "library Action is visibly wrong to someone who has never heard of a "
                           + "standard deviation; so is one nothing can ever fall outside.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                PpButton {
                    // `label`, not `text` — PpButton is a Rectangle with its own vocabulary.
                    label:   root.library.corpusScanning ? qsTr("Checking…") : qsTr("Check corridors")
                    enabled: !root.library.corpusScanning
                             && root.library.libraryRoot && root.library.libraryRoot.length > 0
                    onClicked: root.library.startCorpusCheck()
                }

                Text {
                    Layout.fillWidth: true
                    text: {
                        if (!root.library.libraryRoot || root.library.libraryRoot.length === 0)
                            return qsTr("No swing library is set, so there is nothing to check against.")
                        if (root.library.corpusScanning)
                            return qsTr("Reading the library…")
                        if (!root.library.corpusEverScanned)
                            return qsTr("Not checked yet.")
                        var n = root.library.corpusSwings
                        var capped = root.library.corpusTruncated
                                     ? qsTr(" The scan stopped at its cap, so this is not the whole library.")
                                     : ""
                        return (root._corpus.length === 0
                                ? qsTr("%n swing(s) checked. No corridor grades the library into one band.", "", n)
                                : qsTr("%n swing(s) checked.", "", n)) + capped
                    }
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }
            }

            Repeater {
                model: root._shownCorpus()
                delegate: Rectangle {
                    id: corpusRow
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: corpusCol.implicitHeight + Theme.sp(14)
                    radius: Theme.radius
                    color:  corpusMa.containsMouse ? Theme.colorBg2 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                    ColumnLayout {
                        id: corpusCol
                        anchors.left:           parent.left
                        anchors.right:          parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin:     Theme.sp(12)
                        anchors.rightMargin:    Theme.sp(12)
                        spacing: Theme.sp(2)

                        Text {
                            Layout.fillWidth: true
                            text:           corpusRow.modelData.message || ""
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color:          Theme.colorText2
                            wrapMode:       Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            text:           corpusRow.modelData.subject || ""
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            elide:          Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: corpusMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape:  Qt.PointingHandCursor
                        onClicked: root._followRow(corpusRow.modelData)
                    }
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }
}
