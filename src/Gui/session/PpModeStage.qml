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

// Centre-stage arranger. Shows the enabled, host-wired stage panels (camera/charts/
// dashboard/table) packed per ViewLayout.arrangementFor(SessionMode.mode): tabs | split |
// stage. A panel a screen does not provide a delegate for is omitted (see `active`).
//
// Each arrangement's Loaders are gated on the active `arrangement` (not just the
// container's visible:), so only the selected layout instantiates its panels —
// a hidden arrangement must not spin up a second PpCameraTiles (and its camera
// frame subscriptions) behind the visible one.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: stage

    property Component cameraDelegate:      null
    property Component chartsDelegate:      null
    property Component dashboardDelegate:   null
    property Component launchMonitorDelegate: null
    property Component wristMotionDelegate: null
    property Component tableDelegate:       null
    property Component markupDelegate:      null

    // Layout resolves on the active session MODE, not the session type.
    readonly property string arrangement: ViewLayout.arrangementFor(SessionMode.mode)

    readonly property var _defs: [
        { key: "camera",      label: qsTr("Camera"),                comp: cameraDelegate },
        { key: "dashboard",   label: qsTr("Dashboard"),             comp: dashboardDelegate },
        { key: "launchMonitor", label: qsTr("Launch monitor"),      comp: launchMonitorDelegate },
        { key: "wristMotion", label: qsTr("Wrist motion analysis"), comp: wristMotionDelegate },
        { key: "charts",      label: qsTr("Charts"),                comp: chartsDelegate },
        { key: "table",       label: qsTr("Table"),                 comp: tableDelegate },
        { key: "markup",      label: qsTr("Markup"),                comp: markupDelegate }
    ]
    // ordered; enabled AND actually wired by the host screen. A panel a screen does not provide a
    // delegate for (e.g. "dashboard" on Swing/GRF/Coach, which only ScreenWrist wires today) is
    // simply omitted rather than shown as an empty placeholder.
    readonly property var active: _defs.filter(function(d) {
        return ViewLayout.isPanelOn(SessionMode.mode, d.key) && d.comp !== null
    })

    property int tabIndex: 0
    onActiveChanged: if (tabIndex >= active.length) tabIndex = 0

    // empty state
    Text {
        anchors.centerIn: parent
        visible: stage.active.length === 0
        text: qsTr("No panels selected — pick some in View")
        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
        color: Theme.colorText3
    }

    // ── SPLIT — even row ─────────────────────────────────────────────────────
    RowLayout {
        anchors.fill: parent; anchors.margins: Theme.sp(10)
        spacing: Theme.sp(8)
        visible: stage.arrangement === "split" && stage.active.length > 0
        Repeater {
            model: stage.arrangement === "split" ? stage.active : []
            delegate: Loader {
                required property var modelData
                Layout.fillWidth: true; Layout.fillHeight: true
                sourceComponent: modelData.comp || placeholderComp
                onLoaded: if (item && !modelData.comp) item.title = modelData.label
            }
        }
    }

    // ── STAGE — first panel dominant, rest in a side column ──────────────────
    RowLayout {
        anchors.fill: parent; anchors.margins: Theme.sp(10)
        spacing: Theme.sp(8)
        visible: stage.arrangement === "stage" && stage.active.length > 0
        Loader {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.preferredWidth: stage.width * 0.62
            sourceComponent: (stage.arrangement === "stage" && stage.active.length > 0)
                             ? (stage.active[0].comp || placeholderComp) : null
            onLoaded: if (item && stage.active.length > 0 && !stage.active[0].comp)
                          item.title = stage.active[0].label
        }
        ColumnLayout {
            visible: stage.active.length > 1
            Layout.preferredWidth: stage.width * 0.30
            Layout.fillHeight: true
            spacing: Theme.sp(8)
            Repeater {
                model: (stage.arrangement === "stage" && stage.active.length > 1)
                       ? stage.active.slice(1) : []
                delegate: Loader {
                    required property var modelData
                    Layout.fillWidth: true; Layout.fillHeight: true
                    sourceComponent: modelData.comp || placeholderComp
                    onLoaded: if (item && !modelData.comp) item.title = modelData.label
                }
            }
        }
    }

    // ── TABS — tab strip + single loader ─────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Theme.sp(10)
        spacing: Theme.sp(6)
        visible: stage.arrangement === "tabs" && stage.active.length > 0
        Row {
            spacing: Theme.sp(5)
            Repeater {
                model: stage.arrangement === "tabs" ? stage.active : []
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    readonly property bool sel: index === stage.tabIndex
                    height: Theme.sp(30); width: tabTxt.implicitWidth + Theme.sp(26)
                    radius: Theme.radius
                    // Contiguous tab strip — brighten only (no scale would break the seam);
                    // unselected tab fades its fill in on hover (alpha-ramped colorBg2 rest).
                    color: sel || tabMa.containsMouse
                               ? Theme.colorBg2
                               : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    Rectangle {  // active underline
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 2; color: sel ? Theme.colorAccent : "transparent"
                    }
                    Text {
                        id: tabTxt; anchors.centerIn: parent; text: modelData.label
                        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                        color: sel ? Theme.colorText : Theme.colorText3
                    }
                    MouseArea {
                        id: tabMa
                        anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: stage.tabIndex = index
                    }
                }
            }
        }
        Loader {
            Layout.fillWidth: true; Layout.fillHeight: true
            sourceComponent: (stage.arrangement === "tabs" && stage.active.length > 0)
                             ? (stage.active[Math.min(stage.tabIndex, stage.active.length - 1)].comp || placeholderComp)
                             : null
            onLoaded: {
                var d = stage.active[Math.min(stage.tabIndex, stage.active.length - 1)]
                if (item && d && !d.comp) item.title = d.label
            }
        }
    }

    Component { id: placeholderComp; PpStagePanel {} }
}
