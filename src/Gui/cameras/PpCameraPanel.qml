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

// Camera device panel for the session toolbar. Same shell + mode pattern as the
// IMU panel:
//   "list"      — Scan / Connect / Calibrate actions, an all-cameras "Live pose"
//                 toggle, and the camera rows with a per-camera enable toggle
//                 (session-local, owned by CameraManager so the per-screen video
//                 tiles share it). Connect starts the full capture pipeline on
//                 every enabled camera — the screens' video tiles stream from it.
//                 Calibrate is attention-framed once a camera is connected that is
//                 not "fixed in place" (appSettings.cameraFixedInPlace) — i.e.
//                 needs stereo cal.
//   "calibrate" — hosts CameraCalibrationFlow (a STUB for now) compactly in-panel.
// The attention frame wraps the WHOLE panel while calibrating.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root
    property string mode:   "list"        // "list" | "calibrate"

    implicitWidth:  Theme.sp(380)
    implicitHeight: mode === "calibrate"
        ? Theme.sp(46) + 1 + Theme.sp(420)
        : Theme.sp(46) + 1 + listCol.implicitHeight

    // Attention once a connected camera is not "fixed in place" — a non-fixed camera
    // needs (stereo) calibration this session. Fixed cameras keep their calibration.
    readonly property bool needsCalibration: {
        var _dep  = cameraManager.instances
        var list  = cameraManager.cameraList
        var fixed = appSettings.cameraFixedInPlace
        for (var i = 0; i < list.length; ++i)
            if (list[i].selected && fixed[list[i].cameraKey] !== true) return true
        return false
    }

    // ── CR-02 CB6 — the torch, here and not as a sixth toolbar pill ────────
    //
    // The toolbar deliberately holds only AGGREGATES over phones ("a phone's
    // only reason to be on this bar is that it is carrying a camera"), and a
    // torch is not an aggregate — it belongs to one device.  These rows already
    // carry `isPpcp`, so the control lives on the row whose camera the torch
    // actually lights.
    //
    // `ppcpHost` exists only in a `HAVE_PPCP_TRANSPORT` build (main.cpp), so
    // this guards the way everything PPCP on this bar already does.
    readonly property bool havePpcp: typeof ppcpHost !== "undefined"

    // ⭐ THE REVISION IS THE BINDING'S ONLY LINK TO THE SIGNAL, AND IT IS
    // PASSED AS AN ARGUMENT ON PURPOSE.  A bare `root._torchRev` statement
    // inside a function body is dropped by the QML compiler and the binding
    // then subscribes to nothing — the control would go stale the moment the
    // first ack arrived and never move again.  Passing it in makes the
    // dependency part of the expression the engine actually records.
    property int _torchRev: 0
    Connections {
        target: root.havePpcp ? ppcpHost : null
        // ⛔ A READING, NOT A STRUCTURAL CHANGE.  An ack, an `actuator_state`
        // and a heartbeat all arrive on phoneHealthChanged() precisely so this
        // panel refreshes a value without any list rebuilding its delegates.
        function onPhoneHealthChanged() { root._torchRev++ }
        // A phone appearing or leaving does change which controls exist.
        function onPhonesChanged() { root._torchRev++ }
    }

    // The phone row that owns `peerId`, or null.  Cross-referenced by
    // `serialNumber === counterpartId`, the same join PhonesPanel already uses
    // for its camera count — a PPCP camera's `serialNumber` IS the owning
    // peer's id.
    function _phoneForPeer(peerId) {
        if (!havePpcp || !peerId) return null
        var phones = ppcpHost.phones
        for (var i = 0; i < phones.length; ++i)
            if (phones[i].counterpartId === peerId) return phones[i]
        return null
    }

    // The torch this phone DECLARED, merged with what the ack and
    // `actuator_state` have said about it — or null where it declared none.
    //
    // ⚠ `Peer.actuators` MAY LEGITIMATELY BE EMPTY (5.19c), on exactly the
    // terms `sources` may: a phone owning no Actuators omits the key from
    // `declare` entirely.  That is a complete declaration and not a fault, so
    // the control is ABSENT rather than shown disabled.
    function torchFor(peerId, rev) {
        var ph = _phoneForPeer(peerId)
        if (!ph || !ph.actuators) return null
        for (var i = 0; i < ph.actuators.length; ++i) {
            var a = ph.actuators[i]
            if (a.kind === "torch")
                return { pairingId: ph.pairingId, id: a.id, control: a.control,
                         label: a.label, state: a.state, pending: a.pending,
                         refusedReason: a.refusedReason }
        }
        return null
    }

    // ⚠ ONE TORCH, ONE CONTROL.  PPCP binds an Actuator to a PEER (5.19), not
    // to a Source — there is no actuator→source key on the wire — so a phone
    // offering two cameras would otherwise show the same torch twice and a
    // toggle on one would silently move the other.  Shown on the FIRST row of
    // that peer instead.  ⛔ This is not the CB5 defect in disguise: nothing
    // here is KEYED on the peer id that should be keyed on a `source_id`; the
    // torch genuinely is a per-peer thing and the per-Source readings elsewhere
    // are untouched.
    function torchOwnerRow(index) {
        var list = cameraManager.cameraList
        if (index < 0 || index >= list.length) return false
        var me = list[index]
        if (!me.isPpcp || !me.serialNumber) return false
        for (var i = 0; i < index; ++i)
            if (list[i].isPpcp && list[i].serialNumber === me.serialNumber) return false
        return true
    }

    // Per-session camera enablement lives in CameraManager
    // (cameraManager.sessionCameraExcluded) so the per-screen video tiles,
    // every toolbar instance AND the start-session wizard share one list.
    // Seeded from appSettings.cameraExcluded at startup, re-seeded by the
    // wizard on open; never written back to settings (global enablement is
    // owned by Settings).

    // True when at least one camera is session-enabled and every enabled one is
    // connected — drives the Connect ⇄ Disconnect action toggle.
    readonly property bool allConnected: {
        var list = cameraManager.cameraList
        var enabled = 0
        for (var i = 0; i < list.length; ++i)
            if (list[i].sessionEnabled) { ++enabled; if (!list[i].selected) return false }
        return enabled > 0
    }

    // Connect every session-enabled, not-yet-selected camera, then start the
    // capture pipeline so the visible video tiles stream (same path as the Play
    // capture tab — pose/ball pipeline, ring buffer, swing replay).
    function startConnect() {
        var list = cameraManager.cameraList
        for (var i = 0; i < list.length; ++i)
            if (list[i].sessionEnabled && !list[i].selected)
                cameraManager.setSelected(list[i].index, true)
        if (!cameraManager.isRecording && cameraManager.anySelected)
            cameraManager.startAll()
    }

    // Disconnect every connected camera (enabled or not). Stop the recording
    // session first so isRecording doesn't stay true with zero instances.
    function disconnectAll() {
        if (cameraManager.isRecording) cameraManager.stopAll()
        var list = cameraManager.cameraList
        for (var i = 0; i < list.length; ++i)
            if (list[i].selected) cameraManager.setSelected(list[i].index, false)
    }

    // ── Header ──────────────────────────────────────────────────────────────
    Item {
        id: hdr
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.sp(46)

        RowLayout {
            anchors { fill: parent; leftMargin: Theme.sp(15); rightMargin: Theme.sp(15) }
            visible: root.mode === "list"
            Text {
                Layout.fillWidth: true
                text: qsTr("CAMERAS")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                font.letterSpacing: Theme.trackingLabel; color: Theme.colorText2
            }
            Text {
                text: cameraManager.instances.length + " / " + cameraManager.cameraList.length
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
            text: qsTr("Calibrate cameras")
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
            font.letterSpacing: Theme.trackingLabel; color: Theme.colorText2
        }
    }
    Rectangle {
        id: hairline
        anchors { left: parent.left; right: parent.right; top: hdr.bottom }
        height: 1; color: Theme.colorBorderMid
    }

    // ── LIST view ───────────────────────────────────────────────────────────
    Column {
        id: listCol
        visible: root.mode === "list"
        anchors { left: parent.left; right: parent.right; top: hairline.bottom }

        Row {
            width: parent.width; padding: Theme.sp(12); spacing: Theme.sp(8)
            ScopedAction { glyph: "⟳"; label: qsTr("Scan");    onTriggered: cameraManager.enumerate() }
            ScopedAction {
                glyph: "⇄"
                label: root.allConnected ? qsTr("Disconnect") : qsTr("Connect")
                connecting: !root.allConnected && cameraManager.anyConnecting
                onTriggered: root.allConnected ? root.disconnectAll() : root.startConnect()
            }
            ScopedAction {
                glyph: "◳"; label: qsTr("Calibrate")
                primary: root.needsCalibration
                onTriggered: root.mode = "calibrate"
            }
        }
        Rectangle { width: parent.width; height: 1; color: Theme.colorBorder }

        // Overlay control (pose skeleton / shaft / ball) now lives per-view in the
        // View menu — see PpViewPanel's OVERLAYS section. This panel is device
        // management only (Scan / Connect / Calibrate + camera rows).

        Repeater {
            model: cameraManager.cameraList
            delegate: CamRow {
                required property var modelData
                required property int index
                width: listCol.width
                // CB6 — the torch, where this phone declared one and this is
                // the row that owns it.  Null everywhere else, which is what
                // makes the control absent rather than disabled.
                torch: root.havePpcp && modelData.isPpcp && root.torchOwnerRow(index)
                       ? root.torchFor(modelData.serialNumber, root._torchRev)
                       : null
                camKey:   modelData.cameraKey
                camName: modelData.alias && modelData.alias !== "" ? modelData.alias
                                                                   : modelData.description
                serial: modelData.serialNumber
                isPpcp: modelData.isPpcp === true
                perspective: modelData.perspective    // CameraInstance.Perspective value
                iface:  modelData.interface
                selected: modelData.selected
                deviceEnabled: modelData.sessionEnabled
            }
        }
    }

    // ── CALIBRATE view — compact in-panel stub ──────────────────────────────
    CameraCalibrationFlow {
        id: calibFlow
        visible: root.mode === "calibrate"
        anchors { left: parent.left; right: parent.right; top: hairline.bottom; bottom: parent.bottom }
        layoutMode: "compact"
        showHeader: false
        onCompleted: root.mode = "list"
        onCancelled: root.mode = "list"
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

    component ScopedAction: Rectangle {
        property string glyph: ""; property string label: ""; property bool primary: false
        // Drives the traveling-light frame while a connect attempt is in flight.
        property bool connecting: false
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
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: glyph
                   font.family: Theme.fontSymbol; font.pixelSize: Theme.sp(16)
                   color: primary ? Theme.colorAttention : Theme.colorText2 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: label
                   font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                   color: primary ? Theme.colorAttention : Theme.colorText }
        }
        PpPressable { id: saMa; onClicked: parent.triggered() }
        PpConnectingFrame { anchors.fill: parent; radius: parent.radius; running: parent.connecting }
    }

    component TogglePill: Rectangle {
        property bool checked: false
        signal toggled(bool value)
        width:  Theme.sp(34)
        height: Theme.sp(18)
        radius: Theme.sp(9)
        color:  checked ? Theme.colorAccent : Theme.colorBg3
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        Rectangle {
            width:  Theme.sp(12)
            height: Theme.sp(12)
            radius: Theme.sp(6)
            color:  "white"
            anchors.verticalCenter: parent.verticalCenter
            x: parent.checked ? parent.width - width - Theme.sp(3) : Theme.sp(3)
            Behavior on x { NumberAnimation { duration: 120 } }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.toggled(!parent.checked)
        }
    }

    component CamRow: Item {
        id: camRow
        property string camKey:   ""
        property string camName:  ""
        property int    perspective: 0
        property string serial: ""; property string iface: ""; property bool selected: false
        // A PPCP camera's `serial` is its owning peer's id, not a serial —
        // long, shared between that phone's cameras, and elided differently.
        property bool   isPpcp: false
        property bool   deviceEnabled: true   // session enablement, from cameraList

        // ⭐ CR-02 — the phone's torch, or null.  See root.torchFor().
        property var    torch: null
        readonly property bool hasTorch: torch !== null && torch !== undefined

        // ⛔ ⭐ THE LIT STATE, AND WHERE IT COMES FROM.  `torch.state` is
        // written by `actuator_command_ack` (MSG 12.1c, the ACHIEVED state) and
        // by `actuator_state` (12.2a, a thermal cutoff or a local control),
        // inside PpcpLiveSession::observe(), and by NOTHING ELSE.  The click
        // path — TogglePill.onToggled -> ppcpHost.setPhoneActuator ->
        // PpcpLiveSession::setActuator -> ppcp_peer_actuator_command — writes
        // only `commandPending`, because that call returning success means the
        // command is on a QUEUE and no byte has left.  So this binding cannot
        // light on the click: there is no path from the click to `state`.
        //
        // That is trap 3, and this codebase has now learned it three times —
        // for `arm` (PpcpLiveSession::isArmed()'s warning), for `stream_open`
        // (VideoInputPpcp::onStreamOpenAck, whose comment reads "we had the
        // comment without the code"), and here.
        readonly property bool torchOn: hasTorch && torch.state === "on"
        // ⚠ "unknown" IS A THIRD ANSWER, NOT A DARK BULB.  12.2 is push, so an
        // Actuator nobody has commanded and that has not moved has told us
        // nothing — different from "off", and shown differently.
        readonly property bool torchUnknown: hasTorch && torch.state === "unknown"
        readonly property bool torchPending: hasTorch && torch.pending === true
        // 12.1b's open registry — `no_actuator`, `busy`, `thermal_limit`,
        // `permission_denied`, `unsupported` — rendered VERBATIM.  A word the
        // device chose, never mapped onto one this host already knows
        // (10.3a / I13), and never swallowed: a torch that refused and a torch
        // nobody asked look identical without it.
        readonly property string torchRefusal:
            hasTorch && torch.refusedReason ? torch.refusedReason : ""

        // Live controller for this camera (reactive on cameraManager.instances).
        readonly property var realInstance: {
            var insts = cameraManager.instances
            for (var i = 0; i < insts.length; ++i)
                if (insts[i].cameraKey === camKey) return insts[i]
            return null
        }
        readonly property bool connected: realInstance !== null

        readonly property string perspLabel: perspective === CameraInstance.FaceOn ? qsTr("Face-on")
                                            : perspective === CameraInstance.DownTheLine ? qsTr("Down-the-line")
                                            : qsTr("Unassigned")
        height: Theme.sp(60) + (camRow.torchRefusal !== "" ? Theme.sp(16) : 0)

        Rectangle {  // row hairline
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
            height: 1; color: Theme.colorBorder
        }

        RowLayout {
            anchors { fill: parent; leftMargin: Theme.sp(15); rightMargin: Theme.sp(15)
                      bottomMargin: camRow.torchRefusal !== "" ? Theme.sp(16) : 0 }
            spacing: Theme.sp(11)

            // Status dot — good when connected, muted otherwise.
            Rectangle {
                Layout.preferredWidth: Theme.sp(8); Layout.preferredHeight: Theme.sp(8)
                radius: Theme.sp(4)
                opacity: deviceEnabled ? 1.0 : 0.45
                color: connected ? Theme.colorGood : Theme.colorText3
            }

            // ⛔ A `ColumnLayout`, NOT a `Column`.  A plain `Column` gives its
            // children no width, so `elide` never fires and each Text grows to
            // its full implicit width — a whole peer id for a PPCP camera.  That
            // inflated this item's implicit width past the space available, and
            // a RowLayout cannot shrink a child below its implicit width, so the
            // row overflowed and the toggles were painted over the text.
            // `Layout.minimumWidth: 0` is what actually lets it give way.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: Theme.sp(2)
                opacity: deviceEnabled ? 1.0 : 0.45
                Text {
                    Layout.fillWidth: true
                    text: perspLabel + " · " + camName
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                    color: Theme.colorText; elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    // A PPCP camera's "serial" is its owning peer's id, which is
                    // far too long to read and is shown for identification only —
                    // so it elides from the LEFT, keeping the distinctive tail
                    // rather than the shared `peer:` prefix.
                    text: !deviceEnabled ? qsTr("disabled — won't connect")
                        : [serial !== "" ? "SN " + serial : "", iface]
                              .filter(function(s){ return s !== "" }).join(" · ")
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingData; color: Theme.colorText3
                    elide: camRow.isPpcp ? Text.ElideMiddle : Text.ElideRight
                }
            }

            // ── ⭐ THE TORCH ────────────────────────────────────────────
            //
            // Present only where this phone actually DECLARED one (5.19c makes
            // an empty `Peer.actuators` a complete declaration), and lit only
            // by the ack and by `actuator_state`.
            Row {
                Layout.alignment: Qt.AlignVCenter
                visible: camRow.hasTorch
                spacing: Theme.sp(6)

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    // The device's own label where it gave one (informational,
                    // 5.19), otherwise the kind.
                    text: camRow.hasTorch && camRow.torch.label && camRow.torch.label !== ""
                          ? camRow.torch.label : qsTr("Torch")
                    font.family: Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingData
                    color: camRow.torchRefusal !== "" ? Theme.colorWarn
                         : camRow.torchUnknown        ? Theme.colorText3
                                                      : Theme.colorText2
                }

                TogglePill {
                    id: torchPill
                    anchors.verticalCenter: parent.verticalCenter
                    // ⛔ BOUND TO THE ACK, NOT TO THE CLICK.  `torchOn` reads
                    // `torch.state`, which only PpcpLiveSession::observe()
                    // writes.  Clicking sends a command and moves `pending`;
                    // the pill does not move until the device answers.
                    checked: camRow.torchOn
                    // Half-lit while a command is outstanding, so an operator
                    // can see that we asked without being told it worked.
                    opacity: camRow.torchPending ? 0.55
                           : camRow.torchUnknown ? 0.75 : 1.0
                    onToggled: (v) => {
                        if (!root.havePpcp || !camRow.hasTorch) return
                        // ⚠ `v` IS WHAT THE OPERATOR ASKED FOR, and it is used
                        // for exactly one thing: the value sent.  It is never
                        // written back into `checked`.
                        ppcpHost.setPhoneActuator(camRow.torch.pairingId,
                                                  camRow.torch.id, v)
                    }
                }
            }

            // A gap the eye can read, so the torch and the enable toggle are not
            // one undifferentiated cluster of two identical pills.
            Item {
                visible: camRow.hasTorch
                Layout.preferredWidth: Theme.sp(10)
                Layout.preferredHeight: 1
            }

            // Enable toggle — session-local; disabling also disconnects.
            // Labelled only where a torch sits beside it: with two pills on one
            // row an unlabelled pair is a guess, and this is the one that drops
            // the camera from the session.
            Text {
                visible: camRow.hasTorch
                Layout.alignment: Qt.AlignVCenter
                text: qsTr("Enable")
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingData
                color: Theme.colorText2
                opacity: camRow.deviceEnabled ? 1.0 : 0.45
            }

            TogglePill {
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: camRow.hasTorch ? 0 : Theme.sp(6)
                checked: camRow.deviceEnabled
                onToggled: (v) => cameraManager.setSessionCameraEnabled(camRow.camKey, v)
            }
        }

        // 12.1b — the refusal, verbatim and on its own line.  Rendered rather
        // than swallowed: without it a torch that refused for `thermal_limit`
        // and a torch nobody touched look exactly the same.
        Text {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                      leftMargin: Theme.sp(15); rightMargin: Theme.sp(15)
                      bottomMargin: Theme.sp(4) }
            visible: camRow.torchRefusal !== ""
            text: qsTr("torch refused — %1").arg(camRow.torchRefusal)
            font.family: Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color: Theme.colorWarn
            elide: Text.ElideRight
        }
    }
}
