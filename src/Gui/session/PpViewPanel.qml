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

// View panel for the session toolbar. Two sections: an ARRANGEMENT schematic
// group (tabs/split/stage) and a PANELS toggle grid. Both edit the CURRENT
// session mode's layout via ViewLayout, persisted per SessionMode.mode.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    implicitWidth:  Theme.sp(300)
    implicitHeight: col.implicitHeight + Theme.sp(26)

    readonly property var panelMeta: [
        { key: "camera",      label: qsTr("Camera"),                ready: true  },
        { key: "sessionDiagnostics", label: qsTr("Session diagnostics"), ready: true },
        { key: "launchMonitor", label: qsTr("Launch monitor"),      ready: true  },
        { key: "wristMotion", label: qsTr("Wrist motion analysis"), ready: true  },
        { key: "carousel",    label: qsTr("Carousel"),              ready: true  },
        { key: "timeline",    label: qsTr("Timeline"),              ready: true  },
        { key: "charts",      label: qsTr("Charts"),                ready: false },
        { key: "table",       label: qsTr("Table"),                 ready: false },
        { key: "markup",      label: qsTr("Markup"),                ready: true  }
    ]

    Column {
        id: col
        anchors { fill: parent; margins: Theme.sp(13) }
        spacing: Theme.sp(12)

        // ── ARRANGEMENT ─────────────────────────────────────────────────────
        Column {
            width: parent.width
            spacing: Theme.sp(9)
            Text {
                text: qsTr("ARRANGEMENT")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
            }
            Row {
                width: parent.width
                spacing: Theme.sp(7)
                readonly property real cardW: (width - 2 * Theme.sp(7)) / 3
                ArrangeCard { value: "tabs";  label: qsTr("Tabs");  width: parent.cardW }
                ArrangeCard { value: "split"; label: qsTr("Split"); width: parent.cardW }
                ArrangeCard { value: "stage"; label: qsTr("Stage"); width: parent.cardW }
            }
        }

        PpDivider { width: parent.width }

        // ── TIMELINE ────────────────────────────────────────────────────────
        // Replay-mode timeline orientation + snap, persisted globally on appSettings
        // (not per-mode, unlike arrangement/panels above).
        Column {
            width: parent.width
            spacing: Theme.sp(9)
            Text {
                text: qsTr("TIMELINE")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
            }
            Row {
                width: parent.width
                spacing: Theme.sp(7)
                readonly property real cardW: (width - Theme.sp(7)) / 2
                OrientCard { value: "horizontal"; label: qsTr("Horizontal"); horizontalGlyph: true;  width: parent.cardW }
                OrientCard { value: "vertical";   label: qsTr("Vertical");   horizontalGlyph: false; width: parent.cardW }
            }
            Item {
                width: parent.width
                height: Theme.sp(20)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Snap to phases")
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                    color: appSettings.timelineSnapToPhases ? Theme.colorText : Theme.colorText2
                }
                MiniToggle {
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    checked: appSettings.timelineSnapToPhases
                    onToggled: appSettings.timelineSnapToPhases = !checked
                }
            }
        }

        PpDivider { width: parent.width }

        // ── REPLAY ──────────────────────────────────────────────────────────
        // Global replay behaviour (like TIMELINE above, stored on appSettings, not
        // per-mode): whether a just-captured shot auto-replays, and whether replay
        // playback is trimmed to the detected swing (Address → Finish). Both apply
        // to post-capture playback AND replaying a selected past swing.
        Column {
            width: parent.width
            spacing: Theme.sp(9)
            Text {
                text: qsTr("REPLAY")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
            }
            Item {
                width: parent.width
                height: Theme.sp(20)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Auto-replay after capture")
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                    color: appSettings.autoReplayAfterCapture ? Theme.colorText : Theme.colorText2
                }
                MiniToggle {
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    checked: appSettings.autoReplayAfterCapture
                    onToggled: appSettings.autoReplayAfterCapture = !checked
                }
            }
            Item {
                width: parent.width
                height: Theme.sp(20)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Trim to swing (address → finish)")
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                    color: appSettings.replayTrimToSwing ? Theme.colorText : Theme.colorText2
                }
                MiniToggle {
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    checked: appSettings.replayTrimToSwing
                    onToggled: appSettings.replayTrimToSwing = !checked
                }
            }
        }

        PpDivider { width: parent.width }

        // ── PANELS ──────────────────────────────────────────────────────────
        Column {
            width: parent.width
            spacing: Theme.sp(9)
            Text {
                text: qsTr("PANELS")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
            }
            Grid {
                width: parent.width
                columns: 2
                columnSpacing: Theme.sp(14)
                rowSpacing: Theme.sp(9)
                Repeater {
                    model: root.panelMeta
                    delegate: Item {
                        required property var modelData
                        // Capture has no timeline concept — hide that toggle there.
                        // The Grid skips invisible items, so the others reflow.
                        readonly property bool _avail: !(modelData.key === "timeline"
                                                         && SessionMode.mode === SessionMode.capture)
                        visible: _avail
                        width: (parent.width - Theme.sp(14)) / 2
                        height: Theme.sp(20)
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                            color: ViewLayout.isPanelOn(SessionMode.mode, modelData.key)
                                   ? Theme.colorText : Theme.colorText2
                        }
                        MiniToggle {
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            checked: ViewLayout.isPanelOn(SessionMode.mode, modelData.key)
                            onToggled: ViewLayout.setPanel(SessionMode.mode, modelData.key, !checked)
                        }
                    }
                }
            }
        }
    }

    // ── arrangement schematic card ──────────────────────────────────────────
    component ArrangeCard: Rectangle {
        property string value: ""
        property string label: ""
        readonly property bool active: ViewLayout.arrangementFor(SessionMode.mode) === value
        height: Theme.sp(46)
        radius: Theme.radius
        // Selected: accent wash + accent border. Hover (unselected): faint bg fill
        // (alpha-ramped → no flash) + border eases to accentMid — the chip/tile
        // language; pairs with the PpPressable scale/dip below.
        color: active              ? Theme.colorAccentLight
             : cardMa.containsMouse ? Theme.colorBg2
             :                        Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
        border.width: 1
        border.color: active              ? Theme.colorAccent
                    : cardMa.containsMouse ? Theme.colorAccentMid
                    :                        Theme.colorBorderMid
        Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

        Column {
            anchors.centerIn: parent
            spacing: Theme.sp(5)
            // schematic glyph — two/one/dominant rects
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 2
                Repeater {
                    model: value === "split" ? 2 : 1
                    Rectangle {
                        width: value === "stage" ? Theme.sp(13) : Theme.sp(9)
                        height: Theme.sp(13); radius: 2
                        color: "transparent"
                        border.width: 1
                        border.color: parent.parent.parent.active ? Theme.colorAccent : Theme.colorText3
                    }
                }
                Column {
                    visible: value === "stage"
                    spacing: 2
                    Rectangle { width: Theme.sp(5); height: Theme.sp(5.5); radius: 1; color: "transparent"; border.width: 1; border.color: parent.parent.parent.parent.active ? Theme.colorAccent : Theme.colorText3 }
                    Rectangle { width: Theme.sp(5); height: Theme.sp(5.5); radius: 1; color: "transparent"; border.width: 1; border.color: parent.parent.parent.parent.active ? Theme.colorAccent : Theme.colorText3 }
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: label
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzMicro
                color: active ? Theme.colorAccent : Theme.colorText2
            }
        }
        PpPressable { id: cardMa; onClicked: ViewLayout.setArrangement(SessionMode.mode, value) }
    }

    // ── timeline orientation card (mirrors ArrangeCard, writes appSettings) ──
    component OrientCard: Rectangle {
        id: orientCard
        property string value: ""
        property string label: ""
        property bool   horizontalGlyph: true
        readonly property bool active: appSettings.timelineOrientation === value
        height: Theme.sp(46)
        radius: Theme.radius
        // Selected: accent wash + accent border. Hover (unselected): faint bg fill
        // (alpha-ramped → no flash) + border eases to accentMid — the chip/tile
        // language; pairs with the PpPressable scale/dip below.
        color: active                ? Theme.colorAccentLight
             : orientMa.containsMouse ? Theme.colorBg2
             :                          Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
        border.width: 1
        border.color: active                ? Theme.colorAccent
                    : orientMa.containsMouse ? Theme.colorAccentMid
                    :                          Theme.colorBorderMid
        Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

        Column {
            anchors.centerIn: parent
            spacing: Theme.sp(5)
            Item {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Theme.sp(20); height: Theme.sp(14)
                Rectangle {   // a line oriented per the choice
                    anchors.centerIn: parent
                    width:  orientCard.horizontalGlyph ? Theme.sp(18) : Theme.sp(2)
                    height: orientCard.horizontalGlyph ? Theme.sp(2)  : Theme.sp(12)
                    radius: 1
                    color: orientCard.active ? Theme.colorAccent : Theme.colorText3
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: orientCard.label
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzMicro
                color: orientCard.active ? Theme.colorAccent : Theme.colorText2
            }
        }
        PpPressable { id: orientMa; onClicked: appSettings.timelineOrientation = orientCard.value }
    }

    // ── compact on/off toggle ───────────────────────────────────────────────
    component MiniToggle: Rectangle {
        property bool checked: false
        signal toggled()
        width: Theme.sp(34); height: Theme.sp(18); radius: height / 2
        color: checked ? Theme.colorAccent : Theme.colorBg3
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        Rectangle {
            width: Theme.sp(14); height: width; radius: width / 2
            y: Theme.sp(2)
            x: checked ? parent.width - width - Theme.sp(2) : Theme.sp(2)
            color: checked ? (Theme.dark ? Theme.colorBg : "#FFFFFF") : Theme.colorText3
            Behavior on x { NumberAnimation { duration: Theme.durationFast } }
        }
        MouseArea {
            anchors.fill: parent; hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.toggled()
        }
    }
}
