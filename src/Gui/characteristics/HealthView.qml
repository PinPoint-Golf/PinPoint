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

    readonly property var _coverage: root.library.causeCoverage()
    readonly property var _health:   root.library.health()

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
        case "observableNoSignal": return qsTr("Nothing detects it")
        case "needsRevalidation":  return qsTr("Flagged for revalidation")
        case "inconsistentReach":  return qsTr("Reach and detection disagree")
        case "screenedHasCause":   return qsTr("A screen result something claims to cause — check edge direction")
        case "duplicateId":        return qsTr("Redefined by another pack")
        }
        return code
    }

    function _healthCodes() {
        var seen = []
        for (var i = 0; i < root._health.length; ++i)
            if (seen.indexOf(root._health[i].code) === -1) seen.push(root._health[i].code)
        return seen
    }

    function _healthFor(code) {
        var out = []
        for (var i = 0; i < root._health.length; ++i)
            if (root._health[i].code === code) out.push(root._health[i])
        return out
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

            Repeater {
                model: root._coverage
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

            Text {
                Layout.fillWidth: true
                text: root._health.length === 0
                      ? qsTr("Nothing outstanding.")
                      : qsTr("%n thing(s) worth a look. None of these stop the library working — "
                             + "they are gaps and unfinished edges, not errors.", "",
                             root._health.length)
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

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

                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.sp(6)
                        bottomPadding: Theme.sp(8)

                        Repeater {
                            model: codeBlock.rows
                            delegate: Rectangle {
                                required property var modelData
                                implicitWidth:  subjTxt.implicitWidth + Theme.sp(18)
                                implicitHeight: Theme.sp(24)
                                radius: height / 2
                                color:  subjMa.containsMouse ? Theme.colorBg2 : Theme.colorBg

                                Text {
                                    id: subjTxt
                                    anchors.centerIn: parent
                                    text:           modelData.subject
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }
                                MouseArea {
                                    id: subjMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked: root.openCondition(modelData.subject)
                                }
                            }
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }
}
