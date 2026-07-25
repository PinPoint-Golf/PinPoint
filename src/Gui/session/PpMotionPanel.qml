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

// Motion panel for the session toolbar's Motion pill — replaces the old single
// "Pose overlay" toggle (formerly PpViewPanel's OVERLAYS section) with the full
// ViewLayout motion model: a master on/off switch, a named-preset picker, and a
// per-element (arms/spine/shoulders/hips/legs/shaft/ball) off|frame|fan|trace
// customiser. Edits the CURRENT session mode's motion layout only — same
// "this mode's saved layout" contract as PpViewPanel.
//
// Two pages that slide horizontally (PRESET is the default landing page; the
// slide honours Theme.reduceMotion via Theme.durationFast, which is already
// zeroed when reduceMotion is set):
//   Page 1 — Presets: master toggle + preset list + "Customise…" disclosure.
//   Page 2 — Customise: back chevron + per-element segmented controls, gated
//            on data availability for the loaded swing (shotReplay.analysisDetail).
//
// Capture is heavily guarded: only the "Ball only" preset row is offered and
// Customise is unreachable (fan/trace are meaningless without a completed
// swing — ViewLayout.motionFor() itself hard-forces every non-ball element off
// in Capture regardless of stored state).

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    implicitWidth:  Theme.sp(300)
    implicitHeight: (page === 0 ? page1Col.implicitHeight : page2Col.implicitHeight) + Theme.sp(26)

    // 0 = Presets (default landing page), 1 = Customise.
    property int page: 0

    readonly property int  _mode:      SessionMode.mode
    readonly property bool _isCapture: _mode === SessionMode.capture

    // Presets offered on Page 1 — Capture only ever offers "Ball only" (fan/
    // trace are meaningless without a completed swing, and every other element
    // is hard-off in Capture per ViewLayout's guard).
    readonly property var _presetRows: {
        var all = ViewLayout.presetCatalog()
        if (!_isCapture) return all
        return all.filter(function (p) { return p.id === "ballOnly" })
    }

    // The ACTIVE motion config wants fan/trace on a body element — those read
    // the smoothed pose track exclusively (pose2d.smoothed, written at analysis
    // time). Swings analysed before the smoother existed lack it until they are
    // re-analysed, so the overlay would silently draw nothing; surface that
    // quietly on the preset page instead (Customise already greys per-cell).
    readonly property bool _wantsSmoothed: {
        if (_isCapture) return false
        var mo = ViewLayout.motionFor(_mode)
        if (!mo.on) return false
        var body = ["arms", "spine", "shoulders", "hips", "legs"]
        for (var i = 0; i < body.length; i++) {
            var m = mo.modes[body[i]]
            if (m === "fan" || m === "trace") return true
        }
        return false
    }
    readonly property bool _smoothedMissing: {
        var d = shotReplay.analysisDetail
        return !(shotReplay.active && d && d.pose2d && d.pose2d.smoothed
                 && d.pose2d.smoothed.length > 0)
    }

    readonly property var _bodyRows: [
        { key: "arms",      label: qsTr("Arms") },
        { key: "spine",     label: qsTr("Spine") },
        { key: "shoulders", label: qsTr("Shoulders") },
        { key: "hips",      label: qsTr("Hips") },
        { key: "legs",      label: qsTr("Legs") }
    ]
    readonly property var _objectRows: [
        { key: "shaft", label: qsTr("Shaft") },
        { key: "ball",  label: qsTr("Ball") }
    ]

    // Land back on Presets whenever the popup closes, and never leave Page 2
    // exposed if the session mode flips to Capture while the popup is open
    // (the mode-switch segmented control lives in the same toolbar and doesn't
    // close this popup on its own).
    onVisibleChanged: if (!visible) page = 0
    Connections {
        target: SessionMode
        function onModeChanged() { if (root._isCapture) root.page = 0 }
    }

    // ── Sliding two-page viewport ────────────────────────────────────────────
    Item {
        id: viewport
        anchors.fill: parent
        clip: true

        Row {
            id: pagesRow
            width: viewport.width * 2
            x: root.page === 0 ? 0 : -viewport.width
            Behavior on x { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

            // ── PAGE 1 — Presets ────────────────────────────────────────────
            Item {
                width: viewport.width
                height: page1Col.implicitHeight + Theme.sp(26)

                Column {
                    id: page1Col
                    anchors { fill: parent; margins: Theme.sp(13) }
                    spacing: Theme.sp(12)

                    Item {
                        width: parent.width
                        height: Theme.sp(20)
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Motion overlay")
                            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                            color: ViewLayout.motionOn(root._mode) ? Theme.colorText : Theme.colorText2
                        }
                        MiniToggle {
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            checked: ViewLayout.motionOn(root._mode)
                            onToggled: ViewLayout.setMotionOn(root._mode, !checked)
                        }
                    }

                    PpDivider { width: parent.width }

                    Text {
                        text: qsTr("PRESET")
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                    }

                    Column {
                        width: parent.width
                        spacing: Theme.sp(7)
                        Repeater {
                            model: root._presetRows
                            delegate: PresetRow {
                                required property var modelData
                                presetId:    modelData.id
                                presetLabel: modelData.label
                                presetHint:  modelData.hint
                            }
                        }
                    }

                    // Quiet guidance when the selected preset can't draw on the
                    // loaded swing (fan/trace need the smoothed track — absent on
                    // swings analysed before it existed, or with no swing loaded).
                    Text {
                        visible: root._wantsSmoothed && root._smoothedMissing
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: shotReplay.active
                              ? qsTr("This swing has no smoothed track — re-analyse it to enable fan · trace")
                              : qsTr("Load a swing to see fan · trace")
                        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzMicro
                        color: Theme.colorWarn
                    }

                    PpDivider { width: parent.width; visible: !root._isCapture }

                    // Footer — disclosure into Customise. Hidden in Capture:
                    // fan/trace are meaningless without a completed swing.
                    Item {
                        visible: !root._isCapture
                        width: parent.width
                        height: Theme.sp(30)
                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radius
                            color: custMa.containsMouse ? Theme.colorBg2 : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }
                        RowLayout {
                            anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Customise…")
                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                                color: Theme.colorText2
                            }
                            Text {
                                text: "›"
                                font.pixelSize: Theme.fontSzBody2; color: Theme.colorText3
                            }
                        }
                        MouseArea {
                            id: custMa
                            anchors.fill: parent; hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.page = 1
                        }
                    }
                }
            }

            // ── PAGE 2 — Customise ──────────────────────────────────────────
            Item {
                width: viewport.width
                height: page2Col.implicitHeight + Theme.sp(26)

                Column {
                    id: page2Col
                    anchors { fill: parent; margins: Theme.sp(13) }
                    spacing: Theme.sp(12)

                    // Header — back chevron + title + breadcrumb.
                    Item {
                        width: parent.width
                        height: Theme.sp(20)
                        Row {
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.sp(8)
                            Text {
                                text: "‹"
                                font.pixelSize: Theme.fontSzBody; color: Theme.colorText2
                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -Theme.sp(6)
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.page = 0
                                }
                            }
                            Text {
                                text: qsTr("Customise")
                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                                color: Theme.colorText
                            }
                        }
                        Text {
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            text: {
                                var id = ViewLayout.motionPreset(root._mode)
                                if (id === "custom") return qsTr("Custom")
                                var cat = ViewLayout.presetCatalog()
                                for (var i = 0; i < cat.length; ++i)
                                    if (cat[i].id === id) return qsTr("from %1").arg(cat[i].label)
                                return ""
                            }
                            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                            color: Theme.colorText3
                        }
                    }

                    PpDivider { width: parent.width }

                    // One-time legend — muted, compact.
                    Text {
                        text: "—  " + qsTr("off") + "   ▣  " + qsTr("frame")
                              + "   ⧉  " + qsTr("fan") + "   ∿  " + qsTr("trace")
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        color: Theme.colorText3
                    }

                    Text {
                        text: qsTr("BODY")
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                    }
                    Column {
                        width: parent.width
                        spacing: Theme.sp(9)
                        Repeater {
                            model: root._bodyRows
                            delegate: ElementRow {
                                required property var modelData
                                elementKey:   modelData.key
                                elementLabel: modelData.label
                            }
                        }
                    }

                    Text {
                        text: qsTr("OBJECTS")
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                    }
                    Column {
                        width: parent.width
                        spacing: Theme.sp(9)
                        Repeater {
                            model: root._objectRows
                            delegate: ElementRow {
                                required property var modelData
                                elementKey:   modelData.key
                                elementLabel: modelData.label
                            }
                        }
                    }
                }
            }
        }
    }

    // ── preset list row — ArrangeCard's selection language, row-shaped ──────
    // Self-contained: resolves its own active state and applies itself
    // directly, needing only the preset's id/label/hint from the caller.
    component PresetRow: Rectangle {
        property string presetId:    ""
        property string presetLabel: ""
        property string presetHint:  ""
        readonly property bool active: {
            var cur = ViewLayout.motionPreset(SessionMode.mode)
            return cur !== "custom" && cur === presetId
        }

        width: parent.width
        height: Theme.sp(46)
        radius: Theme.radius
        // Selected: accent wash + accent border. Hover (unselected): faint bg
        // fill (alpha-ramped → no flash) + border eases to accentMid — mirrors
        // PpViewPanel's ArrangeCard exactly, just row-shaped.
        color: active                ? Theme.colorAccentLight
             : rowPress.containsMouse ? Theme.colorBg2
             :                          Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
        border.width: 1
        border.color: active                ? Theme.colorAccent
                    : rowPress.containsMouse ? Theme.colorAccentMid
                    :                          Theme.colorBorderMid
        Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

        Column {
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                      leftMargin: Theme.sp(12); rightMargin: Theme.sp(12) }
            spacing: Theme.sp(2)
            Text {
                width: parent.width
                text: presetLabel; elide: Text.ElideRight
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                color: active ? Theme.colorAccent : Theme.colorText
            }
            Text {
                width: parent.width
                text: presetHint; elide: Text.ElideRight
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText3
            }
        }
        PpPressable { id: rowPress; onClicked: ViewLayout.applyPreset(SessionMode.mode, presetId) }
    }

    // ── element row: label + 4-segment off/frame/fan/trace control ─────────
    component ElementRow: Item {
        property string elementKey:   ""
        property string elementLabel: ""
        width: parent.width
        height: Theme.sp(24)

        Text {
            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
            text: elementLabel
            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
            color: Theme.colorText
        }
        ElementSegmented {
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
            elementKey: parent.elementKey
        }
    }

    // ── compact 4-segment off/frame/fan/trace control ───────────────────────
    // Local rather than PpSegmentedControl: segments need per-cell disable
    // (Ball's fan/trace are ALWAYS disabled; any mode missing analysed data
    // for the loaded swing is disabled too) and a glyph≠value mapping
    // PpSegmentedControl doesn't support. Self-contained — reads SessionMode
    // and shotReplay (both globally reachable) directly rather than needing
    // values threaded in from the panel root.
    component ElementSegmented: Rectangle {
        id: seg
        property string elementKey: ""
        readonly property var _segs: [
            { value: "off",   glyph: "—" },
            { value: "frame", glyph: "▣" },
            { value: "fan",   glyph: "⧉" },
            { value: "trace", glyph: "∿" }
        ]
        readonly property string _selected: ViewLayout.elementMode(SessionMode.mode, elementKey)

        // Data-availability for the loaded swing. shotReplay is the global
        // Review/Analyse replay facade (ShotReplayController) — Capture never
        // reaches this control, so shotProcessor's transient in-window replay
        // detail is irrelevant here. "off" is always available; Ball's
        // fan/trace fall out of the m !== "frame" branch — permanently
        // unavailable regardless of data (a point sample per frame has no
        // meaningful fan/trace).
        function _available(m) {
            if (m === "off") return true
            if (!shotReplay.active) return false
            var d = shotReplay.analysisDetail
            if (seg.elementKey === "ball")
                return m === "frame" && !!(d && d.ball && d.ball.samples && d.ball.samples.length > 0)
            if (seg.elementKey === "shaft")
                return !!(d && d.club && d.club.samples && d.club.samples.length > 0)
            if (m === "frame")
                return !!(d && d.pose2d && d.pose2d.frames && d.pose2d.frames.length > 0)
            // fan / trace — the additive smoothed series (re-analyse-gated;
            // missing on older swings, treated as unavailable).
            return !!(d && d.pose2d && d.pose2d.smoothed && d.pose2d.smoothed.length > 0)
        }

        width: Theme.sp(108); height: Theme.sp(24)
        radius: height / 2
        color: Theme.colorBg
        border.width: 1
        border.color: Theme.colorBorderMid

        Row {
            anchors.fill: parent
            anchors.margins: Theme.sp(2)
            spacing: Theme.sp(1)
            Repeater {
                model: seg._segs
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool active:    modelData.value === seg._selected
                    readonly property bool available: seg._available(modelData.value)

                    width:  (seg.width - Theme.sp(4) - Theme.sp(3)) / 4
                    height: parent.height
                    radius: height / 2
                    opacity: available ? 1.0 : 0.35
                    color: active               ? Theme.colorBg3
                         : segPress.containsMouse ? Theme.colorBg3
                         :                          Qt.rgba(Theme.colorBg3.r, Theme.colorBg3.g, Theme.colorBg3.b, 0)
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        anchors.centerIn: parent
                        text: modelData.glyph
                        font.family: Theme.fontSymbol; font.pixelSize: Theme.fontSzBody2
                        color: active ? Theme.colorAccent : Theme.colorText2
                    }
                    MouseArea {
                        id: segPress
                        anchors.fill: parent
                        enabled: available
                        hoverEnabled: true
                        cursorShape: available ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: ViewLayout.setElementMode(SessionMode.mode, seg.elementKey, modelData.value)
                    }
                }
            }
        }
    }

    // ── compact on/off toggle — mirrors PpViewPanel's MiniToggle ────────────
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
