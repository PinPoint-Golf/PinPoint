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

Item {
    id: root

    // Set by ScreenSettings to the settings-screen visibility so calibration
    // capture is released both when switching tabs (our own `visible`) and when
    // navigating away from Settings entirely (`hostVisible`).
    property bool hostVisible: true

    // Effective visibility — the panel is actually on screen.
    readonly property bool shown: visible && hostVisible

    // Explicit opt-in: the microphone is only opened for live metering while the
    // user is calibrating, never just by viewing the panel.
    property bool calibrating: false

    // Capture is forced on only while calibrating AND on screen.
    readonly property bool calActive: calibrating && shown
    onCalActiveChanged: controller.setCalibrationActive(calActive)
    Component.onDestruction: if (calActive) controller.setCalibrationActive(false)

    // Enumerated input devices: [{ id, description, isDefault }, …]. Refreshed
    // on show and via Rescan — availableInputDevices() is a plain function call.
    property var devices: []
    function refreshDevices() { root.devices = controller.availableInputDevices() }

    function deviceIsActive(d) {
        var sel = appSettings.audioInputDevice
        return sel === "" ? (d.isDefault === true) : (d.id === sel)
    }

    Component.onCompleted: refreshDevices()
    onShownChanged: if (shown) refreshDevices()

    // ── Settings search support (mirrors the other Hardware panels) ───────────
    property string lastHighlightId: ""

    function findChild(parent, name) {
        for (var i = 0; i < parent.children.length; i++) {
            var child = parent.children[i]
            if (child.objectName === name) return child
            var found = findChild(child, name)
            if (found) return found
        }
        return null
    }

    function scrollToItem(itemId) {
        if (!itemId) return true
        var target = findChild(contentCol, itemId)
        if (!target) return false
        var mapped = target.mapToItem(contentCol, 0, 0)
        scrollView.contentItem.contentY = Math.max(0, Math.min(
            mapped.y - Theme.sp(24),
            scrollView.contentItem.contentHeight - scrollView.height
        ))
        target.searchHighlight = true
        lastHighlightId = itemId
        highlightTimer.restart()
        return true
    }

    Timer {
        id: highlightTimer
        interval: 1800
        onTriggered: {
            var target = findChild(contentCol, lastHighlightId)
            if (target) target.searchHighlight = false
        }
    }

    // ── Live detection feed (raw acoustic trigger, pre-arbiter) ───────────────
    property int  detectCount:    0
    property real lastConfidence: 0
    // Set when the detector fires; consumed by the next level sample so the
    // marker lands on the buffer that triggered it.
    property bool pendingDet:     false

    // Confirmation chime on detection — same C8 ting as IMU calibration.
    TingPlayer { id: shotTing; frequency: 4186.0 }

    Connections {
        target: controller
        function onImpactDetected(estImpactUs, confidence) {
            if (!root.calActive) return
            root.detectCount    += 1
            root.lastConfidence  = confidence
            root.pendingDet      = true
            meter.flash = 1.0
            shotTing.play()
        }
        function onAudioLevel(level, noiseFloor, threshold) {
            if (!root.calActive) return
            meter.pushSample(level, threshold, root.pendingDet)
            root.pendingDet = false
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Inline component — reusable toggle pill (matches Cameras/IMUs panels)
    // ─────────────────────────────────────────────────────────────────────────
    component TogglePill: Rectangle {
        id: pill
        property bool checked: false
        signal toggled(bool value)

        width:  Theme.sp(34)
        height: Theme.sp(18)
        radius: Theme.sp(9)
        color:  pill.checked ? Theme.colorAccent : Theme.colorBg3
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Rectangle {
            width:  Theme.sp(12)
            height: Theme.sp(12)
            radius: Theme.sp(6)
            color:  "white"
            anchors.verticalCenter: parent.verticalCenter
            x: pill.checked ? parent.width - width - Theme.sp(3) : Theme.sp(3)
            Behavior on x { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape:  Qt.PointingHandCursor
            onClicked:    pill.toggled(!pill.checked)
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Inline component — selectable input-device row (single active, radio-style)
    // ─────────────────────────────────────────────────────────────────────────
    component DeviceRow: Rectangle {
        id: devRow
        property var devData: ({})
        readonly property bool isActive: root.deviceIsActive(devData)

        Layout.fillWidth: true
        implicitHeight:   Theme.sp(46)
        radius:           Theme.radius
        color:            devRow.isActive ? Theme.colorAccentLight
                        : devRowPress.containsMouse ? Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0.5)
                        : Theme.colorSurface
        border.width:     1
        border.color:     devRow.isActive ? Theme.colorAccent
                        : devRowPress.containsMouse ? Theme.colorAccentMid
                        : Theme.colorBorderStrong
        Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

        RowLayout {
            anchors.fill:    parent
            anchors.margins: Theme.sp(12)
            spacing:         Theme.sp(10)

            // Radio dot
            Rectangle {
                width:  Theme.sp(12)
                height: Theme.sp(12)
                radius: Theme.sp(6)
                color:  "transparent"
                border.width: 1
                border.color: devRow.isActive ? Theme.colorAccent : Theme.colorBorderStrong
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.centerIn: parent
                    width:  Theme.sp(6)
                    height: Theme.sp(6)
                    radius: Theme.sp(3)
                    color:  Theme.colorAccent
                    visible: devRow.isActive
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(1)
                Text {
                    text:           devRow.devData.description || qsTr("Unknown device")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody
                    color:          Theme.colorText
                    elide:          Text.ElideRight
                    Layout.fillWidth: true
                }
                Text {
                    visible:        devRow.devData.isDefault === true
                    text:           qsTr("System default")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }

            Text {
                visible:        devRow.isActive
                text:           qsTr("ACTIVE")
                font.family:         Theme.fontData
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorAccent
                Layout.alignment:    Qt.AlignVCenter
            }
        }

        PpPressable {
            id: devRowPress
            hoverScale: 1.0
            // Persisting the id triggers the main.cpp → controller.setInputDevice
            // wiring (restarts capture in place if currently listening).
            onClicked:    appSettings.audioInputDevice = devRow.devData.id
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Content
    // ─────────────────────────────────────────────────────────────────────────
    ScrollView {
        id: scrollView
        anchors.fill:  parent
        contentWidth:  availableWidth
        contentHeight: contentCol.y + contentCol.implicitHeight + Theme.sp(28)

        ColumnLayout {
            id: contentCol
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(16)

            // ── Page header ────────────────────────────────────────────────
            Text {
                text:                qsTr("HARDWARE")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }
            PpDisplayText {
                text: qsTr("Microphone")
            }
            Text {
                text: qsTr("Select the audio input used for acoustic shot detection, then calibrate its sensitivity so every struck ball registers. Use a wired microphone — Bluetooth audio buffering delays the strike sound unpredictably and degrades impact timing. Voice control uses the same microphone and is unaffected by the toggle below.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                Layout.fillWidth: true
            }

            // ── Shot-detection enable ──────────────────────────────────────
            RowLayout {
                objectName: "setting_acousticShotDetection"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1; Behavior on opacity { NumberAnimation { duration: Theme.durationFast } } }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)
                    Text {
                        text:           qsTr("Use microphone for shot detection")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("Detects impact from the strike sound, fused with the IMU and vision modalities")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                TogglePill {
                    checked:   appSettings.acousticShotDetectionEnabled
                    onToggled: (v) => { appSettings.acousticShotDetectionEnabled = v }
                    Layout.alignment: Qt.AlignVCenter
                }
            }

            PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

            // ── Input devices ──────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Text {
                    text:                qsTr("INPUT DEVICES")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                    Layout.fillWidth:    true
                }
                Rectangle {
                    id: rescanBtn
                    implicitWidth:  rescanLabel.implicitWidth + Theme.sp(24)
                    implicitHeight: Theme.sp(28)
                    radius: Theme.radius
                    color:  rescanPress.containsMouse ? Theme.colorBg2 : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                    border.width: 1
                    border.color: rescanPress.containsMouse ? Theme.colorAccentMid : Theme.colorBorderStrong
                    Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
                    Text {
                        id: rescanLabel
                        anchors.centerIn: parent
                        text:           qsTr("Rescan")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color:          Theme.colorText2
                    }
                    PpPressable {
                        id: rescanPress
                        onClicked:    root.refreshDevices()
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth:  true
                Layout.leftMargin: Theme.sp(26)
                spacing: Theme.sp(8)

                Repeater {
                    model: root.devices
                    delegate: DeviceRow {
                        required property var modelData
                        devData: modelData
                    }
                }

                Text {
                    visible:        root.devices.length === 0
                    text:           qsTr("No audio input devices found.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.italic:    true
                    color:          Theme.colorText3
                    Layout.fillWidth: true
                }
            }

            PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

            // ── Microphone distance ────────────────────────────────────────
            // Impact is timed by back-dating the strike sound; the sound takes
            // ~2.9 ms/m to reach the microphone, so the distance to the hitting
            // strip is part of the timing model (AppSettings::micTravelUs).
            ColumnLayout {
                objectName: "setting_micDistance"
                Layout.fillWidth: true
                spacing: Theme.sp(4)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1; Behavior on opacity { NumberAnimation { duration: Theme.durationFast } } }

                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(3)
                        Text {
                            text:           qsTr("Distance to hitting strip")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color:          Theme.colorText
                        }
                        Text {
                            text:           qsTr("Sound travel over this distance is subtracted when timing impact — a rough measure is fine (±0.5 m is ±1.5 ms)")
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                    Text {
                        text:           appSettings.micDistanceM.toFixed(1) + qsTr(" m")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        Layout.alignment: Qt.AlignVCenter
                    }
                }

                Slider {
                    id: distSlider
                    Layout.fillWidth: true
                    from: 0.0; to: 3.0; stepSize: 0.1
                    value: appSettings.micDistanceM
                    onMoved: appSettings.micDistanceM = value

                    background: Rectangle {
                        x: distSlider.leftPadding
                        y: distSlider.topPadding + distSlider.availableHeight / 2 - height / 2
                        width:  distSlider.availableWidth
                        height: Theme.sp(3)
                        radius: Theme.sp(2)
                        color:  Theme.colorBg3
                        Rectangle {
                            width:  distSlider.visualPosition * parent.width
                            height: parent.height
                            radius: parent.radius
                            color:  Theme.colorAccent
                        }
                    }
                    handle: Rectangle {
                        x: distSlider.leftPadding + distSlider.visualPosition * (distSlider.availableWidth - width)
                        y: distSlider.topPadding + distSlider.availableHeight / 2 - height / 2
                        width:  Theme.sp(14)
                        height: Theme.sp(14)
                        radius: Theme.sp(7)
                        color:  Theme.colorAccent
                        border.width: 1
                        border.color: Theme.colorBorderStrong
                    }
                }
            }

            PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

            // ── Calibration ────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Text {
                    text:                qsTr("CALIBRATION")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                    Layout.fillWidth:    true
                }
                // Start/Stop calibration — opens the mic for live metering.
                // Sized via implicit size (it lives in a RowLayout, which ignores
                // plain width/height) against a hidden sizer measuring the longer
                // label so the width stays stable when it switches to "Stop".
                Rectangle {
                    id: calBtn
                    implicitWidth:  calBtnSizer.implicitWidth + Theme.sp(24)
                    implicitHeight: Theme.sp(28)
                    radius: Theme.radius
                    color:  root.calibrating ? Theme.colorAccentLight
                          : calPress.containsMouse ? Theme.colorBg2
                          : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                    border.width: 1
                    border.color: root.calibrating ? Theme.colorAccent
                                : calPress.containsMouse ? Theme.colorAccentMid
                                : Theme.colorBorderStrong
                    Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        id: calBtnSizer
                        visible:        false
                        text:           qsTr("Start calibration")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                    }

                    Text {
                        id: calBtnLabel
                        anchors.centerIn: parent
                        text:           root.calibrating ? qsTr("Stop") : qsTr("Start calibration")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color:          root.calibrating ? Theme.colorAccent : Theme.colorText2
                    }
                    PpPressable {
                        id: calPress
                        onClicked: {
                            if (!root.calibrating) { meter.clear(); root.detectCount = 0 }
                            root.calibrating = !root.calibrating
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.sp(26)
                text: root.calibrating
                          ? qsTr("Listening… hit shots and watch the trace. A green marker and the counter confirm each acoustic detection. Raise sensitivity if a strike is missed; lower it if background noise triggers false detections.")
                          : qsTr("Press Start calibration to open the microphone and watch the live sound level against the detection threshold.")
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                font.italic:    true
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            // Meter + controls card
            Rectangle {
                Layout.fillWidth:  true
                Layout.leftMargin: Theme.sp(26)
                radius: Theme.radius
                color:  Theme.colorBg2
                border.width: 1
                border.color: Theme.colorBorderMid
                implicitHeight: meterCol.implicitHeight + Theme.sp(28)

                ColumnLayout {
                    id: meterCol
                    anchors.left:    parent.left
                    anchors.right:   parent.right
                    anchors.top:     parent.top
                    anchors.margins: Theme.sp(14)
                    spacing: Theme.sp(12)

                    // ── Rolling level trace ──────────────────────────────────
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight:   Theme.sp(120)
                        radius:           Theme.radius
                        color:            Theme.colorBg
                        border.width:     1
                        border.color:     Theme.colorBorderStrong
                        clip:             true

                        Canvas {
                            id: meter
                            anchors.fill:    parent
                            anchors.margins: 1

                            // Rolling ring of { lvl, det } and the latest threshold.
                            property var  samples:   []
                            property real threshold: 0
                            property int  capacity:  240
                            property real flash:     0
                            Behavior on flash { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } }

                            function clear() { samples = []; threshold = 0; requestPaint() }

                            function pushSample(lvl, thr, det) {
                                var s = samples
                                s.push({ lvl: lvl, det: det === true })
                                if (s.length > capacity) s.shift()
                                samples   = s
                                threshold = thr
                            }

                            // Linear amplitude → y, on a -80..0 dB scale so the
                            // noise floor, threshold and impact peaks are all legible.
                            function toY(v) {
                                var db = 20 * Math.log(Math.max(v, 1e-5)) / Math.LN10
                                var t  = (db + 80) / 80
                                t = Math.max(0, Math.min(1, t))
                                return height - t * height
                            }

                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)

                                var n = samples.length
                                var dx = n > 1 ? width / (capacity - 1) : width

                                // Threshold line
                                if (threshold > 0) {
                                    var ty = toY(threshold)
                                    ctx.strokeStyle = Theme.colorWarn
                                    ctx.lineWidth = 1
                                    ctx.setLineDash([4, 4])
                                    ctx.beginPath(); ctx.moveTo(0, ty); ctx.lineTo(width, ty); ctx.stroke()
                                    ctx.setLineDash([])
                                }

                                // Level bars + detection markers
                                for (var i = 0; i < n; ++i) {
                                    var x = i * dx
                                    var y = toY(samples[i].lvl)
                                    ctx.strokeStyle = Theme.colorAccent
                                    ctx.globalAlpha = 0.9
                                    ctx.lineWidth = Math.max(1, dx)
                                    ctx.beginPath(); ctx.moveTo(x, height); ctx.lineTo(x, y); ctx.stroke()

                                    if (samples[i].det) {
                                        ctx.strokeStyle = Theme.colorGood
                                        ctx.globalAlpha = 1.0
                                        ctx.lineWidth = 2
                                        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke()
                                    }
                                }
                                ctx.globalAlpha = 1.0

                                // Detection flash overlay
                                if (flash > 0.001) {
                                    ctx.fillStyle = Theme.colorGood
                                    ctx.globalAlpha = 0.18 * flash
                                    ctx.fillRect(0, 0, width, height)
                                    ctx.globalAlpha = 1.0
                                }
                            }

                            // Repaint at ~30 fps while calibrating; idle otherwise.
                            Timer {
                                interval: 33
                                running:  root.calActive
                                repeat:   true
                                onTriggered: meter.requestPaint()
                            }

                            // Idle placeholder
                            Text {
                                anchors.centerIn: parent
                                visible: !root.calActive
                                text:    qsTr("Calibration idle")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }
                    }

                    // ── Sensitivity slider ───────────────────────────────────
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(4)

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text:           qsTr("Sensitivity")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody
                                color:          Theme.colorText
                                Layout.fillWidth: true
                            }
                            Text {
                                // Normalised percentage for repeatable tuning.
                                text:           Math.round(appSettings.acousticSensitivity * 100) + "%"
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }

                        Slider {
                            id: sensSlider
                            Layout.fillWidth: true
                            // 0 = least sensitive (high amplitude gate — only loud
                            // events fire), 1 = most sensitive (low gate). main.cpp maps
                            // this to the detector's absolute amplitude gate, drawn as the
                            // dashed line on the meter — sit it between your background
                            // noise and your club impacts.
                            from: 0.0; to: 1.0; stepSize: 0.01
                            value: appSettings.acousticSensitivity
                            onMoved: appSettings.acousticSensitivity = value

                            background: Rectangle {
                                x: sensSlider.leftPadding
                                y: sensSlider.topPadding + sensSlider.availableHeight / 2 - height / 2
                                width:  sensSlider.availableWidth
                                height: Theme.sp(3)
                                radius: Theme.sp(2)
                                color:  Theme.colorBg3
                                // Standard left-anchored fill: grows toward the handle as
                                // sensitivity increases (right).
                                Rectangle {
                                    width:  sensSlider.visualPosition * parent.width
                                    height: parent.height
                                    radius: parent.radius
                                    color:  Theme.colorAccent
                                }
                            }
                            handle: Rectangle {
                                x: sensSlider.leftPadding + sensSlider.visualPosition * (sensSlider.availableWidth - width)
                                y: sensSlider.topPadding + sensSlider.availableHeight / 2 - height / 2
                                width:  Theme.sp(14)
                                height: Theme.sp(14)
                                radius: Theme.sp(7)
                                color:  Theme.colorAccent
                                border.width: 1
                                border.color: Theme.colorBorderStrong
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text:           qsTr("Less sensitive")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                Layout.fillWidth: true
                            }
                            Text {
                                text:           qsTr("More sensitive")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }

                    PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

                    // ── Detection counter ────────────────────────────────────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(12)

                        Rectangle {
                            width:  Theme.sp(8); height: Theme.sp(8); radius: Theme.sp(4)
                            color:  meter.flash > 0.05 ? Theme.colorGood : Theme.colorText3
                            Layout.alignment: Qt.AlignVCenter
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }
                        Text {
                            text:           qsTr("Detected: ") + root.detectCount
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzBody
                            color:          Theme.colorText
                        }
                        Text {
                            visible:        root.detectCount > 0
                            text:           qsTr("last confidence ") + (root.lastConfidence * 100).toFixed(0) + "%"
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            Layout.fillWidth: true
                        }
                        Item { Layout.fillWidth: true; visible: root.detectCount === 0 }

                        Rectangle {
                            id: resetBtn
                            implicitWidth:  resetLabel.implicitWidth + Theme.sp(20)
                            implicitHeight: Theme.sp(26)
                            radius: Theme.radius
                            color:  resetPress.containsMouse ? Theme.colorBg2 : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                            border.width: 1
                            border.color: resetPress.containsMouse ? Theme.colorAccentMid : Theme.colorBorderStrong
                            Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
                            Text {
                                id: resetLabel
                                anchors.centerIn: parent
                                text:           qsTr("Reset")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText2
                            }
                            PpPressable {
                                id: resetPress
                                onClicked: { root.detectCount = 0; meter.clear() }
                            }
                        }
                    }
                }
            }
        }
    }
}
