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

// PpDashboardPresetEditor — the dashboard's preset control, a popover anchored to
// the panel header (NOT the toolbar — that keeps the wall, which has no toolbar,
// editable from the same place). Pick a preset, show/hide + reorder zones, pin the
// metrics shown in each zone, and Save-as a named preset. Every edit calls a
// ViewLayout verb on the per-session-type store, so the in-app panel and any cast
// wall window update live; any hand-edit forks the active preset to "Custom".
//
// Layout follows the proven PpExportOptionsSheet pattern: contentItem = ColumnLayout
// (Popup resizes it to availableWidth), lists = Column + Repeater whose delegates
// take `width: parent.width` (a Repeater of nested Layouts does NOT propagate width
// reliably — that collapsed an earlier cut to "arrows, no content").

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import PinPointStudio

Popup {
    id: editor

    property int sessionType: -1
    property var catalog: null
    property var shotCtx: ({})

    property string _expandedZone: ""

    // Live (producible) metric keys of a zone's pool — the DEFAULT set shown when the
    // zone has no explicit pins. Swing-agnostic (config applies to any swing).
    function _liveKeys(zoneKey) {
        var q = _poolQuery(zoneKey)
        if (!q || !catalog) return []
        var rows = catalog.query(q, {})
        var out = []
        for (var i = 0; i < rows.length; ++i) if (!rows[i].planned) out.push(rows[i].key)
        return out
    }
    // The metrics this zone actually displays: the explicit pinned list, else the
    // default live set.
    function _effectiveKeys(zoneKey) {
        var pins = ViewLayout.dashboardZoneMetrics(sessionType, zoneKey)
        return (pins && pins.length > 0) ? pins : _liveKeys(zoneKey)
    }
    // Toggle a metric in/out of the zone's displayed set — seeds an explicit list from
    // the current effective set on the first edit (so "what you see is what you get").
    function _toggleMetric(zoneKey, key) {
        var cur = _effectiveKeys(zoneKey).slice()
        var i = cur.indexOf(key)
        if (i >= 0) cur.splice(i, 1)
        else cur.push(key)
        ViewLayout.setDashboardZoneMetrics(sessionType, zoneKey, cur)
    }

    readonly property string _active: ViewLayout.dashboardActive(sessionType)

    width: Theme.sp(300)
    padding: Theme.sp(14)
    margins: Theme.sp(8)
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.colorSurface; radius: Theme.radiusLg
        border.width: 1; border.color: Theme.colorBorderStrong
    }

    // MUST stay in lock-step with each zone's own query — a zone that renders a
    // metric this pool omits is a metric the user can see but cannot turn off.
    // Motion is deliberately NOT filtered on `scored`: the speeds (clubheadSpeed,
    // handSpeed) and the geometry reads (lagAngle, impactShaftLean) are live and
    // rendered as one-sided / plain band rails, they simply carry no band score.
    function _poolQuery(zoneKey) {
        if (zoneKey === "setup")    return { type: "pointInTime" }
        if (zoneKey === "motion")   return { type: "timeSeries" }
        if (zoneKey === "sequence") return { type: "sequence" }
        return null   // verdict has no metric pool (fixed content)
    }
    contentItem: ColumnLayout {
        spacing: Theme.sp(12)

        // ── header ────────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp(2)
            Text {
                text: qsTr("DASHBOARD PRESET")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                font.letterSpacing: Theme.trackingLabel; color: Theme.colorText2
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Show or hide zones and pin the metrics each one displays.")
                wrapMode: Text.WordWrap
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2; color: Theme.colorText3
            }
        }

        // ── presets ───────────────────────────────────────────────────────────
        Flow {
            Layout.fillWidth: true
            spacing: Theme.sp(6)
            Repeater {
                model: {
                    var out = ViewLayout.dashboardPresetCatalog()
                    var saved = ViewLayout.dashboardSavedNames(editor.sessionType)
                    for (var i = 0; i < saved.length; ++i) out.push({ id: saved[i], label: saved[i] })
                    if (editor._active === "custom") out.push({ id: "custom", label: qsTr("Custom") })
                    return out
                }
                delegate: Chip {
                    required property var modelData
                    label: modelData.label
                    on: editor._active === modelData.id
                    accent: modelData.id === "custom"
                    // "Custom" is a state, not an applicable preset — inert.
                    enabled: modelData.id !== "custom"
                    onClicked: ViewLayout.applyDashboardPreset(editor.sessionType, modelData.id)
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.colorBorderMid; opacity: Theme.borderOpacityNormal }

        // ── zones ─────────────────────────────────────────────────────────────
        Text {
            text: qsTr("ZONES")
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
            font.letterSpacing: Theme.trackingLabel; color: Theme.colorText3
        }
        Column {
            Layout.fillWidth: true
            spacing: Theme.sp(6)
            Repeater {
                model: ViewLayout.dashboardZoneCatalog()
                delegate: Column {
                    id: zrow
                    required property var modelData
                    width: parent.width
                    spacing: Theme.sp(4)

                    readonly property bool on: ViewLayout.isDashboardZoneOn(editor.sessionType, modelData.key)
                    readonly property bool hasPool: editor._poolQuery(modelData.key) !== null
                    readonly property bool expanded: editor._expandedZone === modelData.key

                    // Row: [on/off toggle]  Zone name  ............  [ Metrics ▸ ]
                    Item {
                        width: parent.width
                        height: Theme.sp(30)

                        Toggle {
                            id: tog
                            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                            on: zrow.on
                            onToggled: ViewLayout.setDashboardZoneEnabled(editor.sessionType, zrow.modelData.key, !zrow.on)
                        }
                        // "Metrics ▸" expander — only for zones that have a metric pool
                        // to pin (Verdict is fixed content, so it has none).
                        Rectangle {
                            id: ctl
                            visible: zrow.hasPool
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            implicitWidth: exRow.implicitWidth + Theme.sp(14)
                            height: Theme.sp(24); radius: Theme.radius
                            color: exMa.containsMouse || zrow.expanded ? Theme.colorBg3 : "transparent"
                            border.width: 1
                            border.color: zrow.expanded ? Theme.colorBorderMid : Theme.colorBorder
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Row {
                                id: exRow
                                anchors.centerIn: parent; spacing: Theme.sp(4)
                                Text { anchors.verticalCenter: parent.verticalCenter
                                       text: qsTr("Metrics"); font.family: Theme.fontBody
                                       font.pixelSize: Theme.fontSzBody2; color: Theme.colorText2 }
                                Text { anchors.verticalCenter: parent.verticalCenter
                                       text: zrow.expanded ? "▾" : "▸"; font.family: Theme.fontData
                                       font.pixelSize: Theme.fontSzLabel; color: Theme.colorText3 }
                            }
                            PpPressable { id: exMa; hoverScale: 1.0
                                onClicked: editor._expandedZone = (zrow.expanded ? "" : zrow.modelData.key) }
                        }
                        Text {
                            anchors { left: tog.right; leftMargin: Theme.sp(10)
                                      right: ctl.visible ? ctl.left : parent.right; rightMargin: Theme.sp(10)
                                      verticalCenter: parent.verticalCenter }
                            text: zrow.modelData.label
                            elide: Text.ElideRight
                            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
                            color: zrow.on ? Theme.colorText : Theme.colorText3
                        }
                    }

                    // Expanded: a one-line explanation + the pinnable metric chips.
                    Column {
                        width: parent.width - Theme.sp(24)
                        x: Theme.sp(24)
                        spacing: Theme.sp(5)
                        visible: zrow.expanded && zrow.hasPool
                        Text {
                            width: parent.width
                            text: qsTr("Choose the metrics this zone shows for every swing (✓ = shown). "
                                       + "None chosen = the standard set. Faint = planned (no data yet). "
                                       + "A chosen metric a swing can't measure shows NA on the dashboard.")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzMicro; color: Theme.colorText3
                        }
                        Flow {
                            width: parent.width
                            spacing: Theme.sp(5)
                            Repeater {
                                model: (zrow.expanded && zrow.hasPool && editor.catalog)
                                       ? editor.catalog.query(editor._poolQuery(zrow.modelData.key), {}) : []
                                delegate: Chip {
                                    required property var modelData
                                    readonly property bool _shown: editor._effectiveKeys(zrow.modelData.key).indexOf(modelData.key) >= 0
                                    small: true
                                    label: (modelData.shortLabel && modelData.shortLabel.length)
                                           ? modelData.shortLabel : modelData.label
                                    on: _shown
                                    accent: _shown
                                    check: _shown
                                    dim: modelData.planned === true       // planned ⇒ always NA
                                    onClicked: editor._toggleMetric(zrow.modelData.key, modelData.key)
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.colorBorderMid; opacity: Theme.borderOpacityNormal }

        // ── cast scale ────────────────────────────────────────────────────────
        // Lives HERE, with the rest of the dashboard's configuration, rather than
        // in Settings → Displays: it is a property of how this dashboard is read,
        // and it belongs next to the zones it resizes. Unlike the zone/metric
        // config it is NOT per-session-type or part of a preset — it is a single
        // global viewing-distance preference (appSettings.dashboardScale), so
        // changing it does NOT fork the active preset to "Custom".
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp(4)
            Text {
                text: qsTr("CAST SCALE")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                font.letterSpacing: Theme.trackingLabel; color: Theme.colorText2
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Content size on the cast display — match it to how far away it is read.")
                wrapMode: Text.WordWrap
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2; color: Theme.colorText3
            }
            Flow {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp(2)
                spacing: Theme.sp(6)
                Repeater {
                    model: [ { id: "small",  label: qsTr("Small")  },
                             { id: "medium", label: qsTr("Medium") },
                             { id: "large",  label: qsTr("Large")  } ]
                    delegate: Chip {
                        required property var modelData
                        label: modelData.label
                        on: appSettings.dashboardScale === modelData.id
                        onClicked: appSettings.dashboardScale = modelData.id
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.colorBorderMid; opacity: Theme.borderOpacityNormal }

        // ── save current custom as a named preset ───────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp(6)
            visible: editor._active === "custom"
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("Save as preset…")
                color: Theme.colorText
                placeholderTextColor: Theme.colorText3
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
                background: Rectangle {
                    radius: Theme.radius; color: Theme.colorBg2
                    border.width: 1
                    border.color: nameField.activeFocus ? Theme.colorAccent : Theme.colorBorderMid
                }
            }
            Chip {
                label: qsTr("Save"); accent: true
                enabled: nameField.text.trim().length > 0
                onClicked: {
                    ViewLayout.saveDashboardPreset(editor.sessionType, nameField.text.trim())
                    nameField.text = ""
                }
            }
        }
    }

    // ── small reusable controls ──────────────────────────────────────────────────
    component Chip: Rectangle {
        id: chip
        property string label: ""
        property bool on: false
        property bool accent: false
        property bool muted: false
        property bool small: false
        property bool check: false     // show a leading ✓ (pinned metric)
        property bool dim: false       // grey out (metric not measured in this shot)
        signal clicked()
        implicitWidth: cRow.implicitWidth + Theme.sp(small ? 12 : 16)
        implicitHeight: Theme.sp(small ? 22 : 26)
        radius: Theme.radius
        opacity: (enabled ? 1.0 : 0.4) * (dim ? 0.4 : 1.0) * (muted && !cMa.containsMouse ? 0.8 : 1.0)
        color: on ? (accent ? Theme.colorAccentLight : Theme.colorBg2)
                  : cMa.containsMouse ? Theme.colorBg3 : "transparent"
        border.width: 1
        border.color: on ? (accent ? Theme.colorAccentMid : Theme.colorBorderMid) : Theme.colorBorder
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        Row {
            id: cRow
            anchors.centerIn: parent; spacing: Theme.sp(3)
            Text {
                visible: chip.check; anchors.verticalCenter: parent.verticalCenter; text: "✓"
                font.family: Theme.fontData; font.pixelSize: chip.small ? Theme.fontSzMicro : Theme.fontSzLabel
                color: Theme.colorAccent
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter; text: chip.label
                font.family: Theme.fontData; font.pixelSize: chip.small ? Theme.fontSzMicro : Theme.fontSzLabel
                color: chip.on && chip.accent ? Theme.colorAccent : chip.on ? Theme.colorText : Theme.colorText2
            }
        }
        PpPressable { id: cMa; hoverScale: 1.0; onClicked: chip.clicked() }
    }

    component Toggle: Rectangle {
        id: tgl
        property bool on: false
        signal toggled()
        implicitWidth: Theme.sp(32); implicitHeight: Theme.sp(18); radius: height / 2
        color: on ? Theme.colorAccent : Theme.colorBg3
        border.width: 1; border.color: on ? Theme.colorAccentMid : Theme.colorBorderMid
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        Rectangle {
            width: Theme.sp(12); height: width; radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            x: tgl.on ? parent.width - width - Theme.sp(3) : Theme.sp(3)
            color: "white"
            Behavior on x { NumberAnimation { duration: Theme.durationFast } }
        }
        PpPressable { hoverScale: 1.0; onClicked: tgl.toggled() }
    }
}
