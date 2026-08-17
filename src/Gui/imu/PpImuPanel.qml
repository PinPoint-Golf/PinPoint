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

// IMU device panel for the session toolbar. Two modes:
//   "list"      — scoped action row (Scan / Connect / Calibrate) + device rows.
//                 Connected-but-uncalibrated rows + the Calibrate action are
//                 drawn with the amber attention framing (colorAttention*).
//   "calibrate" — hosts ImuCalibrationFlow compactly IN-PANEL; the panel grows to
//                 fit it. Calibration NEVER leaves this panel / the Wrist screen.
// The attention framing lives ONLY in this list — never inside the flow.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root
    property string mode: "list"          // "list" | "calibrate"

    implicitWidth:  Theme.sp(380)
    implicitHeight: mode === "calibrate"
        ? Theme.sp(46) + 1 + Theme.sp(560)        // header + hairline + compact flow (room for fail messages)
        : Theme.sp(46) + 1 + listCol.implicitHeight

    // True when at least one connected IMU is not yet *successfully* calibrated.
    // A sensor counts as calibrated only when anatCalibrated AND the mount check
    // passes (same thresholds the flow uses: deviation ≤ 15°, gravity ≤ 25°).
    // anatCalibrated alone flips true at the phase-1 arm-down capture, before
    // phase-2 mount validation — so a failed calibration keeps Calibrate lit.
    function _imuCalibratedOk(inst) {
        return inst && inst.anatCalibrated
            && inst.mountDeviationDeg    <= 15.0
            && inst.mountGravityErrorDeg <= 25.0
    }
    readonly property bool needsCalibration: {
        var _dep = imuManager.instances
        var list = imuManager.imuDeviceList
        for (var i = 0; i < list.length; ++i) {
            var inst = imuManager.instanceFor(list[i].id)
            if (inst && inst.imuConnected && !_imuCalibratedOk(inst)) return true
        }
        return false
    }

    // ── Per-session IMU enablement ─────────────────────────────────────────────
    // Manager-owned (imuManager.sessionImuExcluded / setSessionImuEnabled) so the
    // start wizard and every toolbar panel instance share ONE list — same pattern
    // as cameras. Seeded from appSettings.imuExcluded at startup and re-seeded by
    // the wizard on open; never written back to settings (global enablement is
    // owned by the Settings screen). The per-device state is read from the
    // sessionEnabled field on imuDeviceList entries.

    // True when at least one IMU is session-enabled and every enabled one is
    // *actually* connected (live instance with imuConnected — not merely
    // selected/pending) — drives the Connect ⇄ Disconnect action toggle.
    readonly property bool allConnected: {
        var _dep = imuManager.instances
        var list = imuManager.imuDeviceList
        var enabled = 0
        for (var i = 0; i < list.length; ++i) {
            if (!list[i].sessionEnabled) continue
            ++enabled
            var inst = imuManager.instanceFor(list[i].id)
            if (!inst || !inst.imuConnected) return false
        }
        return enabled > 0
    }

    // Disconnect every connected device (enabled or not), cancelling any
    // in-flight paced-connect queue first so it can't reconnect afterwards.
    // Disconnects are immediate — the 2 s BlueZ gap only matters between
    // *connection* attempts.
    function disconnectAll() {
        imuConnectTimer.stop()
        _connecting   = false
        _connectQueue = []
        var list = imuManager.imuDeviceList
        for (var i = 0; i < list.length; ++i)
            if (imuManager.instanceFor(list[i].id) !== null)
                imuManager.setSelected(list[i].index, false)
    }

    // Paced connect of every enabled, not-yet-connected device. Sequential with a
    // 2 s gap so BlueZ can reset its GATT state between connections (wizard pattern).
    property var  _connectQueue: []
    property int  _connectIdx:   0
    property bool _connecting:   false
    function startConnect() {
        var queue = []
        var list = imuManager.imuDeviceList
        for (var j = 0; j < list.length; ++j) {
            if (list[j].sessionEnabled) {
                var inst = imuManager.instanceFor(list[j].id)
                if (!inst || !inst.imuConnected) queue.push(list[j].index)
            }
        }
        if (queue.length === 0) return
        _connectQueue = queue
        _connectIdx   = 0
        _connecting   = true
        imuManager.setSelected(queue[0], true)
        _connectIdx   = 1
        if (_connectIdx < queue.length) imuConnectTimer.start()
        else                            _connecting = false
    }
    Timer {
        id: imuConnectTimer
        interval: 2000
        repeat:   false
        onTriggered: {
            var queue = root._connectQueue
            var idx   = root._connectIdx
            if (idx < queue.length) {
                imuManager.setSelected(queue[idx], true)
                root._connectIdx = idx + 1
                if (root._connectIdx < queue.length) imuConnectTimer.restart()
                else                                 root._connecting = false
            } else {
                root._connecting = false
            }
        }
    }

    // ── Header — count in list mode; back affordance in calibrate mode ─────────
    Item {
        id: hdr
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.sp(46)

        // List-mode title + count
        RowLayout {
            anchors { fill: parent; leftMargin: Theme.sp(15); rightMargin: Theme.sp(15) }
            visible: root.mode === "list"
            Text {
                Layout.fillWidth: true
                text: qsTr("IMUS")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                font.letterSpacing: Theme.trackingLabel; color: Theme.colorText2
            }
            Text {
                text: imuManager.imuCount + " " + qsTr("connected")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText3
            }
        }

        // Calibrate-mode title — no top navigation (the flow's Cancel returns to
        // the list); just a non-interactive heading for context.
        Text {
            anchors { fill: parent; leftMargin: Theme.sp(15); rightMargin: Theme.sp(15) }
            verticalAlignment: Text.AlignVCenter
            visible: root.mode === "calibrate"
            text: qsTr("Calibrate sensors")
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
            font.letterSpacing: Theme.trackingLabel; color: Theme.colorText2
        }
    }
    Rectangle {
        id: hairline
        anchors { left: parent.left; right: parent.right; top: hdr.bottom }
        height: 1; color: Theme.colorBorderMid
    }

    // ── LIST view ──────────────────────────────────────────────────────────────
    Column {
        id: listCol
        visible: root.mode === "list"
        anchors { left: parent.left; right: parent.right; top: hairline.bottom }

        // Scoped actions: Scan / Connect / Calibrate
        Row {
            width: parent.width
            padding: Theme.sp(12); spacing: Theme.sp(8)
            ScopedAction {
                glyph: "⟳"; label: qsTr("Scan")
                onTriggered: imuManager.rescanImu()
            }
            ScopedAction {
                glyph: "⇄"
                label: root.allConnected ? qsTr("Disconnect") : qsTr("Connect")
                connecting: !root.allConnected && (root._connecting || imuManager.anyConnecting)
                onTriggered: {
                    if (root.allConnected)          root.disconnectAll()
                    else if (!root._connecting)     root.startConnect()
                }
            }
            ScopedAction {
                glyph: "◳"; label: qsTr("Calibrate")
                primary: root.needsCalibration                // attention-framed when work to do
                onTriggered: root.mode = "calibrate"
            }
        }
        Rectangle { width: parent.width; height: 1; color: Theme.colorBorder }

        // Device rows
        Repeater {
            model: imuManager.imuDeviceList
            delegate: ImuRow {
                required property var modelData
                width: listCol.width
                devId:    modelData.id
                devIndex: modelData.index
                devName: modelData.alias && modelData.alias !== "" ? modelData.alias
                                                                   : modelData.description
                deviceEnabled: modelData.sessionEnabled
                // Dim when absent from the latest scan (kept listed so a known
                // device stays manageable); a selected device stays present.
                opacity: modelData.present ? 1.0 : 0.45
                Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
                placement: {
                    var p = appSettings.imuPlacement
                    return p[modelData.id] ? p[modelData.id] : ""
                }
            }
        }
    }

    // ── CALIBRATE view — the SAME component, compact, hosted in-panel ───────────
    ImuCalibrationFlow {
        id: calibFlow
        visible: root.mode === "calibrate"
        anchors { left: parent.left; right: parent.right; top: hairline.bottom; bottom: parent.bottom }
        layoutMode: "compact"
        showHeader: false
        onCompleted: root.mode = "list"
        onCancelled: root.mode = "list"
        onVisibleChanged: if (visible) calibFlow.begin()   // auto-start on entering calibrate mode
    }

    // Attention frame around the WHOLE panel while calibrating — drawn on top,
    // inset slightly from the popup edge with a thin border.
    Rectangle {
        anchors.fill: parent
        anchors.margins: Theme.sp(6)
        visible: root.mode === "calibrate"
        color: "transparent"
        radius: Theme.radius
        border.width: Theme.sp(1)
        border.color: Theme.colorAttention
        z: 10
    }

    // ── Scoped action button ────────────────────────────────────────────────
    component ScopedAction: Rectangle {
        property string glyph:   ""
        property string label:   ""
        property bool   primary: false
        // Drives the traveling-light frame while a connect attempt is in flight.
        property bool   connecting: false
        signal triggered()
        width: (root.width - Theme.sp(24) - Theme.sp(16)) / 3
        height: Theme.sp(50); radius: Theme.radius
        // Hover brighten + scale/dip — the PpButton language. Primary (amber)
        // lightens; the outline variant fades a faint fill in (alpha-ramped rest
        // so no colour flash).
        readonly property color _rest: primary ? Theme.colorAttentionLight
                                               : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
        color:        saMa.containsMouse ? (primary ? Qt.lighter(_rest, 1.08) : Theme.colorBg2)
                                         : _rest
        border.width: 1
        border.color: primary ? Theme.colorAttention : Theme.colorBorderStrong
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        Column {
            anchors.centerIn: parent; spacing: Theme.sp(4)
            Text {
                anchors.horizontalCenter: parent.horizontalCenter; text: glyph
                font.family: Theme.fontSymbol; font.pixelSize: Theme.sp(16)
                color: primary ? Theme.colorAttention : Theme.colorText2
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter; text: label
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                color: primary ? Theme.colorAttention : Theme.colorText
            }
        }
        PpPressable { id: saMa; onClicked: parent.triggered() }
        PpConnectingFrame { anchors.fill: parent; radius: parent.radius; running: parent.connecting }
    }

    // ── Per-IMU row ─────────────────────────────────────────────────────────
    component ImuRow: Item {
        property string devId:     ""
        property int    devIndex:  -1
        property string devName:   ""
        property string placement: ""

        // Live instance (reactive on imuManager.instances).
        property QtObject inst: {
            var _dep = imuManager.instances
            return imuManager.instanceFor(devId)
        }
        readonly property bool connected:     inst ? inst.imuConnected  : false
        // Live connection state (driver labels: Scanning… / Connecting… /
        // Discovering services… / Retrying… / Connected / Error / Not found).
        readonly property string stateLabel: inst ? inst.stateLabel : ""
        readonly property bool failed:  inst !== null && !connected
                                        && (stateLabel === "Error" || stateLabel === "Not found")
        readonly property bool pending: inst !== null && !connected && !failed
        // Session enablement — set from the imuDeviceList entry (manager-owned;
        // the Repeater model rebinds on imuDeviceListChanged).
        property bool deviceEnabled: true

        height: Theme.sp(56)

        Rectangle {  // row hairline
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
            height: 1; color: Theme.colorBorder
        }

        RowLayout {
            anchors { fill: parent; leftMargin: Theme.sp(15); rightMargin: Theme.sp(15) }
            spacing: Theme.sp(11)

            // Status LED: grey (idle), flashing grey↔green (connecting/pending),
            // green (connected), red (failed). Disabled rows stay grey + dimmed.
            Rectangle {  // status led
                id: statusLed
                Layout.preferredWidth: Theme.sp(8); Layout.preferredHeight: Theme.sp(8)
                radius: Theme.sp(4)
                opacity: deviceEnabled ? 1.0 : 0.45
                color: !deviceEnabled ? Theme.colorText3
                     : failed    ? Theme.colorError
                     : connected ? Theme.colorGood
                     :             Theme.colorText3
                // Flash grey↔green while a connection is pending. The value source
                // drives the colour only while running; otherwise the binding above
                // applies (idle/connected/failed).
                SequentialAnimation on color {
                    running: pending && deviceEnabled && !Theme.reduceMotion
                    loops:   Animation.Infinite
                    ColorAnimation { from: Theme.colorText3; to: Theme.colorGood;  duration: Theme.durationSlow }
                    ColorAnimation { from: Theme.colorGood;  to: Theme.colorText3; duration: Theme.durationSlow }
                }
            }
            // ⚠ THE THREE WIDTH LINES BELOW ARE WHAT MAKES elide WORK. A Text with
            // no width uses its implicitWidth, and elide is inert against an
            // unconstrained width — so `elide: Text.ElideRight` alone did nothing.
            // Worse, a plain Column reports the widest child as its own
            // implicitWidth, and a RowLayout will not shrink a fillWidth item below
            // that, so one long name pushed the chips and the toggle past the right
            // edge and drew them over the text. The default IMU alias is
            // "<description> <device id>", and on macOS that id is a 36-character
            // per-host UUID (CoreBluetooth never exposes a MAC), so a HackMotion row
            // hit it every time while a short-named sensor never did.
            // preferredWidth 0 + minimumWidth 0: take the space that is left after
            // the fixed-size siblings, never demand more.
            Column {
                Layout.fillWidth: true; spacing: Theme.sp(2)
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
                opacity: deviceEnabled ? 1.0 : 0.45
                Text {
                    width: parent.width
                    text: devName
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                    color: Theme.colorText; elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    // Battery moved to its own colour-coded chip (below); the
                    // subtitle now carries connection state + data rate only.
                    text: {
                        if (!deviceEnabled) return qsTr("disabled — won't connect")
                        if (!inst)          return qsTr("not connected")
                        if (failed)         return qsTr("connection failed")
                        if (!connected)     return inst.stateLabel   // Scanning… / Connecting… / Retrying…
                        return inst.dataRateHz > 0 ? (Math.round(inst.dataRateHz) + " Hz")
                                                   : qsTr("connected")
                    }
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingData
                    color: failed  ? Theme.colorError
                         : pending ? Theme.colorWarn
                         :           Theme.colorText3
                }
            }

            // Battery level chip — colour-coded by charge (good >60%, warn >20%,
            // critical ≤20%). The tinted border draws the eye as a sensor runs
            // low; only shown once a connected device reports a level.
            Rectangle {
                id: batChip
                visible: connected && inst && inst.batteryPercent >= 0
                opacity: deviceEnabled ? 1.0 : 0.45
                readonly property int pct: inst ? inst.batteryPercent : 0
                readonly property color lvlColor: pct > 60 ? Theme.colorGood
                                                 : pct > 20 ? Theme.colorWarn
                                                 :            Theme.colorError
                implicitWidth: batChipLbl.implicitWidth + Theme.sp(14)
                implicitHeight: Theme.sp(20); radius: Theme.sp(4)
                color: Qt.rgba(lvlColor.r, lvlColor.g, lvlColor.b, 0.12)
                border.width: 1
                border.color: Qt.rgba(lvlColor.r, lvlColor.g, lvlColor.b, 0.35)
                Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
                Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                Text {
                    id: batChipLbl; anchors.centerIn: parent
                    text: qsTr("BAT %1%").arg(batChip.pct)
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingData; color: batChip.lvlColor
                }
            }

            // Configured location chip — read from appSettings.imuPlacement, shown
            // as a non-interactive placeholder (styled like a chip but no handler).
            // TODO: per-session IMU location override (defaults to appSettings.imuPlacement)
            Rectangle {
                visible: placement !== ""
                opacity: deviceEnabled ? 1.0 : 0.45
                implicitWidth: placementLbl.implicitWidth + Theme.sp(14)
                implicitHeight: Theme.sp(20); radius: Theme.sp(4)
                color: "transparent"
                border.width: 1; border.color: Theme.colorBorderStrong
                Text {
                    id: placementLbl; anchors.centerIn: parent
                    text: qsTr("IMU %1").arg(placement)
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingData; color: Theme.colorText2
                }
            }

            // Enable toggle — session-local. IMUs are calibrated collectively (the
            // T-pose flow calibrates every connected sensor at once), so there is no
            // per-row calibrate affordance. Toggling on marks the sensor for Connect;
            // toggling off disconnects it if connected. Mirrors the wizard. Styled to
            // match the standard app toggle (GeneralPanel): accent track, white knob.
            Rectangle {
                id: enableToggle
                Layout.alignment: Qt.AlignVCenter
                width:  Theme.sp(34)
                height: Theme.sp(18)
                radius: Theme.sp(9)
                color:  deviceEnabled ? Theme.colorAccent : Theme.colorBg3
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                Rectangle {
                    width:  Theme.sp(12)
                    height: Theme.sp(12)
                    radius: Theme.sp(6)
                    color:  "white"
                    anchors.verticalCenter: parent.verticalCenter
                    x: deviceEnabled ? parent.width - width - Theme.sp(3) : Theme.sp(3)
                    Behavior on x { NumberAnimation { duration: 120 } }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    // Manager-owned; disabling a connected device also disconnects it.
                    onClicked: imuManager.setSessionImuEnabled(devId, !deviceEnabled)
                }
            }
        }
    }
}
