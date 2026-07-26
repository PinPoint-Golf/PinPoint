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

// The fourth view of the diagnostics library: every measure the pack knows, and whether a norm
// judges it.
//
// The question this view exists to answer is "what can this library actually grade?", so the norm
// glyph is the row's most load-bearing element — hollow means a measure nothing can judge, which is
// a corridor signal that will report unavailable however well its producer works. That failure is
// invisible on every other screen in the app: a characteristic that never fires looks exactly like
// a golfer who does not have the fault.
//
// The grade policy and norm-set strip live here rather than in a settings form because this is the
// view where they apply — the panel IS the settings panel (ScreenSettings instantiates it as panel
// 10), and a control parked away from what it governs is a control nobody connects to its effect.
Item {
    id: root

    required property var norms       // NormModel

    // `appSettings` is the root context property from main.cpp — the ONE global instance, per the
    // single-shared-instance rule. Never construct a local AppSettings for a write: it would reach
    // QSettings but leave the QML-bound global's in-memory map stale until the next launch.

    signal openMeasure(string measureId)
    signal newMeasureRequested()

    // ── filter state ──────────────────────────────────────────────────────────
    property string _groupFilter:  ""   // "" = all groups
    property string _statusFilter: ""   // "" = any status
    property string _normFilter:   ""   // "" = any | "yes" | "no"
    property string _search:       ""

    function _filters() {
        var f = {}
        if (root._groupFilter.length  > 0) f.group   = root._groupFilter
        if (root._statusFilter.length > 0) f.status  = root._statusFilter
        if (root._normFilter.length   > 0) f.hasNorm = root._normFilter
        if (root._search.length       > 0) f.search  = root._search
        return f
    }

    // Rows for one group block. The active group chip has to be honoured HERE — setting f.group
    // per block would otherwise overwrite the chip and every block would render regardless of it.
    // Same shape as CharacteristicLibrary's directory, and for the same reason.
    function _rowsFor(group) {
        if (root._groupFilter.length > 0 && root._groupFilter !== group) return []
        var f = root._filters()
        f.group = group
        return root.norms.measures(f)
    }

    readonly property int _totalCount: root.norms.measures(root._filters()).length

    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(12)
            width:   scrollView.availableWidth - Theme.sp(64)
            spacing: Theme.sp(18)

            // ── Page header ────────────────────────────────────────────────────
            Text {
                text:                qsTr("REFERENCE")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                PpDisplayText { text: qsTr("Measures & norms") }
                Item { Layout.fillWidth: true }
                PpButton {
                    label: qsTr("New measure")
                    onClicked: root.newMeasureRequested()
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("A measure is one number read off a swing — a series sampled at a "
                           + "position. A norm is the population it is judged against. Without a "
                           + "norm a measure can be produced perfectly and still tell nobody "
                           + "anything, because there is nothing to be outside of.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("%1 measures · %2 with a norm · %3 norm rows across %4 contexts")
                        .arg(root.norms.measureCount)
                        .arg(root.norms.normedMeasureCount)
                        .arg(root.norms.normCount)
                        .arg(root.norms.contexts.length)
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }

            // ── Settings strip ─────────────────────────────────────────────────
            //
            // Two controls that govern every norm in the pack, not one measure. They sit above the
            // list because that is their scope.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: stripCol.implicitHeight + Theme.sp(24)
                radius: Theme.radius
                color:  Theme.colorBg2

                ColumnLayout {
                    id: stripCol
                    x:       Theme.sp(14)
                    y:       Theme.sp(12)
                    width:   parent.width - Theme.sp(28)
                    spacing: Theme.sp(10)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(16)

                        ColumnLayout {
                            Layout.preferredWidth: Theme.sp(200)
                            spacing: Theme.sp(3)

                            Text {
                                text:                qsTr("GRADE POLICY")
                                font.family:         Theme.fontBody
                                font.pixelSize:      Theme.fontSzMicro
                                font.letterSpacing:  Theme.trackingMicro
                                font.capitalization: Font.AllUppercase
                                color:               Theme.colorText3
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("How far from a norm counts as worth saying. One "
                                           + "setting for the whole pack — if Ideal meant "
                                           + "different things in different places, nothing "
                                           + "would be comparable.")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                wrapMode:       Text.WordWrap
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(5)

                            PpSegmentedControl {
                                Layout.fillWidth: true
                                Layout.maximumWidth: Theme.sp(320)
                                options:  root.norms.gradePolicies.map(function(p) { return p.label })
                                selected: {
                                    var ps = root.norms.gradePolicies
                                    for (var i = 0; i < ps.length; ++i)
                                        if (ps[i].name === root.norms.gradePolicy) return ps[i].label
                                    return ""
                                }
                                onActivated: function(value) {
                                    var ps = root.norms.gradePolicies
                                    for (var i = 0; i < ps.length; ++i)
                                        if (ps[i].label === value) {
                                            root.norms.gradePolicy = ps[i].name
                                            appSettings.diagnosticsGradePolicy = ps[i].name
                                            return
                                        }
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: {
                                    var ps = root.norms.gradePolicies
                                    for (var i = 0; i < ps.length; ++i)
                                        if (ps[i].name === root.norms.gradePolicy) return ps[i].hint
                                    return ""
                                }
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                wrapMode:       Text.WordWrap
                            }
                        }
                    }

                    PpDivider { Layout.fillWidth: true }

                    // ── Norm set ───────────────────────────────────────────────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(16)

                        Text {
                            Layout.preferredWidth: Theme.sp(200)
                            text:                qsTr("NORM SET")
                            font.family:         Theme.fontBody
                            font.pixelSize:      Theme.fontSzMicro
                            font.letterSpacing:  Theme.trackingMicro
                            font.capitalization: Font.AllUppercase
                            color:               Theme.colorText3
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(4)

                            Repeater {
                                model: root.norms.normSets
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: Theme.sp(8)

                                    Rectangle {
                                        implicitWidth:  Theme.sp(6)
                                        implicitHeight: Theme.sp(6)
                                        radius: width / 2
                                        color:  modelData.active ? Theme.colorAccent : Theme.colorBorderStrong
                                    }
                                    Text {
                                        text:           modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        color:          Theme.colorText
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: {
                                            var bits = [modelData.origin,
                                                        qsTr("%n norms", "", modelData.normCount)]
                                            if (modelData.readOnly) bits.push(qsTr("read-only"))
                                            return bits.join(" · ")
                                        }
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                        elide:          Text.ElideRight
                                    }
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: root.norms.normSets.length < 2
                                text: qsTr("One set today. Authoring a corridor creates your own "
                                           + "set, which layers over this one.")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                wrapMode:       Text.WordWrap
                            }
                        }
                    }
                }
            }

            // ── Search ─────────────────────────────────────────────────────────
            PpTextField {
                Layout.fillWidth: true
                Layout.maximumWidth: Theme.sp(380)
                placeholderText: qsTr("Search measures")
                text: root._search
                onTextChanged: root._search = text
            }

            // ── Group chips ────────────────────────────────────────────────────
            Flow {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                Rectangle {
                    readonly property bool active: root._groupFilter === ""
                    implicitWidth:  allText.implicitWidth + Theme.sp(20)
                    implicitHeight: Theme.sp(26)
                    radius: height / 2
                    color:  active ? Theme.colorAccent : Theme.colorBg2

                    Text {
                        id: allText
                        anchors.centerIn: parent
                        text:           qsTr("All")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          parent.active ? Theme.colorBg : Theme.colorText2
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._groupFilter = ""
                    }
                }

                Repeater {
                    model: root.norms.groups
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool active: root._groupFilter === modelData.name

                        implicitWidth:  gChip.implicitWidth + Theme.sp(20)
                        implicitHeight: Theme.sp(26)
                        radius: height / 2
                        color:  active ? Theme.colorAccent : Theme.colorBg2

                        Text {
                            id: gChip
                            anchors.centerIn: parent
                            text:           modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          parent.active ? Theme.colorBg : Theme.colorText2
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root._groupFilter = parent.active ? "" : modelData.name
                        }
                    }
                }
            }

            // ── Norm + status chips ────────────────────────────────────────────
            //
            // Muted next to the group row: these narrow a list, the group row navigates it.
            Flow {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Repeater {
                    model: [{ kind: "norm",   value: "yes", label: qsTr("Has a norm") },
                            { kind: "norm",   value: "no",  label: qsTr("No norm") },
                            { kind: "status", value: "live",          label: qsTr("Live") },
                            { kind: "status", value: "planned",       label: qsTr("Planned") },
                            { kind: "status", value: "noProducer",    label: qsTr("No producer") },
                            { kind: "status", value: "notCapturable", label: qsTr("Capture gap") }]

                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool active:
                            modelData.kind === "norm" ? root._normFilter   === modelData.value
                                                      : root._statusFilter === modelData.value

                        implicitWidth:  fChip.implicitWidth + Theme.sp(18)
                        implicitHeight: Theme.sp(24)
                        radius: height / 2
                        color:  "transparent"
                        border.width: 1
                        border.color: active ? Theme.colorAccent : Theme.colorBorderMid

                        Text {
                            id: fChip
                            anchors.centerIn: parent
                            text:           modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          parent.active ? Theme.colorAccent : Theme.colorText3
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (modelData.kind === "norm")
                                    root._normFilter = parent.active ? "" : modelData.value
                                else
                                    root._statusFilter = parent.active ? "" : modelData.value
                            }
                        }
                    }
                }
            }

            // ── Empty state ────────────────────────────────────────────────────
            Text {
                Layout.fillWidth: true
                visible:        root._totalCount === 0
                text:           qsTr("Nothing matches these filters.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText3
            }

            // ── Groups ─────────────────────────────────────────────────────────
            Repeater {
                model: root.norms.groups
                delegate: ColumnLayout {
                    id: groupBlock
                    required property var modelData
                    readonly property var rows: root._rowsFor(modelData.name)

                    Layout.fillWidth: true
                    spacing: Theme.sp(2)
                    visible: rows.length > 0

                    Text {
                        text:                modelData.label
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                        topPadding:          Theme.sp(8)
                        bottomPadding:       Theme.sp(4)
                    }

                    Repeater {
                        model: groupBlock.rows
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: Theme.sp(46)
                            radius: Theme.radius
                            color:  rowMa.containsMouse ? Theme.colorBg2 : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin:  Theme.sp(10)
                                anchors.rightMargin: Theme.sp(12)
                                spacing: Theme.sp(10)

                                // ── The norm glyph ──────────────────────────────
                                //
                                // Filled = a norm resolves; a ring = it is inherited rather than
                                // this measure's own; hollow = nothing judges this measure. The
                                // three states are the answer to what this view is for, so the
                                // glyph leads the row rather than trailing it.
                                Rectangle {
                                    Layout.alignment: Qt.AlignVCenter
                                    implicitWidth:  Theme.sp(9)
                                    implicitHeight: Theme.sp(9)
                                    radius: width / 2
                                    color: !modelData.hasNorm      ? "transparent"
                                         : modelData.normInherited ? "transparent"
                                         :                           Theme.colorAccent
                                    border.width: modelData.hasNorm ? 1 : 1
                                    border.color: modelData.hasNorm ? Theme.colorAccent
                                                                    : Theme.colorBorderStrong
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.sp(1)

                                    Text {
                                        Layout.fillWidth: true
                                        text:           modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        color:          Theme.colorText
                                        elide:          Text.ElideRight
                                    }
                                    // Only what is NOT already obvious from the row. A live
                                    // measure with its own norm has nothing to add here, and an
                                    // empty second line is quieter than a redundant one.
                                    Text {
                                        Layout.fillWidth: true
                                        visible: text.length > 0
                                        text: {
                                            var bits = []
                                            if (modelData.status !== "live")
                                                bits.push(modelData.statusLabel)
                                            if (modelData.hasNorm && modelData.normInherited)
                                                bits.push(qsTr("norm inherited from %1")
                                                            .arg(modelData.defaultContextLabel))
                                            else if (!modelData.hasNorm)
                                                bits.push(qsTr("no norm judges this"))
                                            return bits.join(" · ")
                                        }
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                        elide:          Text.ElideRight
                                    }
                                }

                                // The corridor itself, where there is one — the number is the
                                // point of the row and reads better than any badge for it. The
                                // unit trails in a quieter weight so the figures stay scannable
                                // down the column.
                                RowLayout {
                                    visible: modelData.hasNorm
                                    spacing: Theme.sp(4)

                                    Text {
                                        text: qsTr("%1 – %2")
                                                .arg(Number(modelData.idealLo).toFixed(1))
                                                .arg(Number(modelData.idealHi).toFixed(1))
                                        font.family:        Theme.fontData
                                        font.pixelSize:     Theme.fontSzMicro
                                        font.letterSpacing: Theme.trackingData
                                        color:              Theme.colorText2
                                    }
                                    Text {
                                        visible:        modelData.unit.length > 0
                                        text:           modelData.unit
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                    }
                                }

                                Text {
                                    text:           qsTr("used by %n", "", modelData.usedBy)
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          modelData.usedBy > 0 ? Theme.colorText3
                                                                         : Theme.colorBorderStrong
                                }
                            }

                            MouseArea {
                                id: rowMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                onClicked:    root.openMeasure(modelData.id)
                            }
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }
}
