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


// Settings -> Phones.  Where a remembered PPCP pairing is seen and revoked.
//
// ⚠ THIS IS A CONFORMANCE SURFACE AND NOT A CONVENIENCE.  RV 7.4b: persistence
// is "visible to the user, and individually revocable", and both of those are
// this panel — its whole reason to exist.  A remembered pairing is a standing
// ability to complete a handshake with this host, so it is shown rather than
// kept out of the way.  It used to be a table at the foot of the home screen;
// deleting that table without providing this would have been a regression
// against 7.4b, which is why the two changes are one commit.
//
// ⚠ THERE IS NO "REMEMBER" BUTTON HERE, AND THAT IS NOT AN OMISSION.  7.4b's
// third clause — persistence is opt-in — was itself downgraded to a SHOULD by
// libppcp erratum E57 (25 August 2026): a user who has just paired a device
// has already given the consent a separate opt-in step used to ask for twice,
// so `PpcpRendezvous` now remembers a pairing the moment it completes.  What
// this panel offers instead is the opt-OUT — "Forget" — which is what 7.4b's
// surviving clauses actually require: visible, and individually revocable.
//
// ⚠ IT USED TO CARRY A WARNING FOR WINDOWS AND LINUX, AND NO LONGER NEEDS ONE.
// makePlatformPairingStore() returned a keychain store on macOS and NULL
// everywhere else, so persist() refused and no phone was remembered across a
// launch at all — an empty list on such a machine would have read as "you have
// never paired one", which is a different and untrue statement, so it was said
// out loud.  libppcp erratum E56 (25 August 2026) made RV 7.2c a SHOULD and the
// PRK moved into the app's own settings on EVERY platform, so the condition the
// banner described no longer exists and the banner has gone with it.
//
// Rows come from `ppcpHost.phones` — the SAME list the home screen's DEVICES
// section and the resource monitor draw from, because a paired phone is a
// device and there should be one answer to what phones this host knows about.
// What is extra here is the pair of controls 7.4b requires.
//
// ⚠ REBUILT 25 Aug 2026 TO MATCH CamerasPanel.qml / ImusPanel.qml.  A phone is
// a device the same way a camera or an IMU is one, so its row wears the same
// frame: a status dot, an editable alias, a connection indicator and a strip
// of read-only facts.  The alias is new — `PpcpHostService::setPhoneAlias()`
// — and sits beside `cameraAlias`/`imuAlias` in the settings file rather than
// through `AppSettings`, since `Ppcp` has no dependency on `Gui/app` and this
// keeps it that way.  There is deliberately no expandable "test" panel the way
// IMUs have one: a phone has no host-driven live view to show, and a fake one
// would be worse than none.  What IS deliberately here, empty of content for
// now, is room for more: the facts strip and the row's structure both exist so
// a future preference (a placement, a rate, anything RT-20/RV-6 add) has a
// slot to land in without another redesign.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

Item {
    id: root

    // Injected rather than reached for: `ppcpHost` exists only where libppcp
    // AND OpenSSL are both present (H0), and the offscreen QML suite installs
    // no context property but `appSettings`.
    property var controller: (typeof ppcpHost !== "undefined") ? ppcpHost : null

    readonly property bool havePpcp: controller !== null

    // `cameraManager` is the same global context property CamerasPanel.qml
    // reads directly — but the offscreen QML suite that stands this panel up
    // for its H0 (no-ppcp) test installs only `appSettings`, so a bare
    // reference would throw ReferenceError there. Guarded for the same reason
    // `controller` above is.
    readonly property bool haveCameraManager: typeof cameraManager !== "undefined"

    // The same rows the DEVICES list and the resource monitor show — one list,
    // built once in PpcpHostService::phones(), rather than this panel deciding
    // for itself what counts as a phone.  A live, unscanned code is not in it:
    // that is a QR on screen and belongs to the pairing dialog.
    readonly property var rows: root.controller ? root.controller.phones : []

    // ─────────────────────────────────────────────────────────────────────────
    // Inline component — one phone's row, the same frame CameraDeviceRow and
    // ImuDeviceRow use: background surface, coloured border, status dot,
    // editable alias, a strip of read-only facts underneath.
    // ─────────────────────────────────────────────────────────────────────────
    component PhoneDeviceRow: Item {
        id: phoneRow

        property var phoneData: ({})   // one entry from ppcpHost.phones

        readonly property bool isConnected: phoneRow.phoneData.status === "connected"
        readonly property bool isAvailable: phoneRow.phoneData.status === "available"
        readonly property bool isRevoked:   phoneRow.phoneData.invalidated === true
        readonly property bool isRemembered: phoneRow.phoneData.persisted === true

        // Design §6.1 — which path this phone is actually on: "cable", "wifi",
        // or empty. ⛔ EMPTY IS "WE DO NOT KNOW", NOT "WI-FI". Transport is a
        // property of a LIVE link, so a remembered-but-absent phone has none,
        // and rendering that as Wi-Fi would assert something we never observed.
        readonly property string transport: phoneRow.isConnected
                                          ? (phoneRow.phoneData.transport || "") : ""

        // Cameras this phone is currently contributing, cross-referenced
        // against `cameraManager.cameraList` by peer id — `VideoInputPpcp`'s
        // `serialNumber` IS the PPCP peer id (see ResourceMonitorController) —
        // rather than a second count of the same Sources invented here.
        readonly property int cameraCount: {
            if (!root.haveCameraManager) return 0
            var pid = phoneRow.phoneData.counterpartId
            if (!pid) return 0
            var list = cameraManager.cameraList
            var n = 0
            for (var i = 0; i < list.length; i++)
                if (list[i].serialNumber === pid) n++
            return n
        }

        // ⚠ ONE ColumnLayout OWNS THE HEIGHT, RATHER THAN header+facts ANCHORED
        // SEPARATELY WITH A HAND-SUMMED implicitHeight.  That was the first cut
        // here and it under-reported: a bare RowLayout anchored straight to an
        // Item (not itself inside a managing Layout) does not reliably resolve
        // its own implicitHeight from a Repeater's delegates before the card's
        // height binding reads it, so the facts strip's VALUE line rendered
        // half-clipped with no bottom margin — exactly Mark's report, confirmed
        // by grabToImage() rather than trusted from source. `mainCol` below is
        // what CameraDeviceRow/ImuDeviceRow actually do: header and facts are
        // both children of one ColumnLayout, which sizes its RowLayout children
        // itself and reports a correct sum — the same mechanism `bodyWrap` in
        // ImusPanel.qml relies on for its own capabilities strip.
        implicitHeight: mainCol.implicitHeight

        opacity: phoneRow.isRevoked ? 0.5 : 1.0
        Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

        // Background fill
        Rectangle {
            anchors.fill: parent
            color:        Theme.colorSurface
            radius:       Theme.radius
        }

        clip: true

        // Border overlay — z:100 so content never occludes it
        Rectangle {
            anchors.fill: parent
            color:        "transparent"
            border.width: 1
            border.color: phoneRow.isConnected ? Theme.colorGood
                        : phoneRow.isRevoked    ? Theme.colorBorderMid
                        :                          Theme.colorBorderStrong
            radius:       Theme.radius
            z:            100
            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
        }

        ColumnLayout {
            id: mainCol
            anchors.left:  parent.left
            anchors.right: parent.right
            anchors.top:   parent.top
            spacing: 0

            // ── Header row ───────────────────────────────────────────────────
            RowLayout {
                id: headerRow
                Layout.fillWidth:       true
                Layout.topMargin:       Theme.sp(14)
                Layout.leftMargin:      Theme.sp(14)
                Layout.rightMargin:     Theme.sp(14)
                Layout.preferredHeight: Theme.sp(54)
                spacing: Theme.sp(10)

                // Status dot — green connected, accent while merely discovered
                // on this network, warn while remembered but neither, grey
                // once forgotten.  Same three-state vocabulary Cameras/IMUs
                // use for "is this thing actually usable right now".
                Rectangle {
                    width:  Theme.sp(6)
                    height: Theme.sp(6)
                    radius: Theme.sp(3)
                    color: phoneRow.isConnected ? Theme.colorGood
                         : phoneRow.isRevoked    ? Theme.colorText3
                         : phoneRow.isAvailable  ? Theme.colorAccent
                         :                          Theme.colorWarn
                    Layout.alignment: Qt.AlignVCenter
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }

                // Alias (editable) + meta
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(2)

                    PpTextField {
                        Layout.fillWidth: true
                        placeholderText:   qsTr("Device alias…")
                        text:              phoneRow.phoneData.alias || ""
                        enabled:           !phoneRow.isRevoked
                        onEditingFinished: if (root.controller)
                                               root.controller.setPhoneAlias(phoneRow.phoneData.pairingId, text)
                    }

                    Row {
                        spacing: Theme.sp(10)
                        // What the phone called itself in its MSG `declare`
                        // (or the shortened handle, for one that never has) —
                        // kept visible under the alias rather than replaced
                        // by it, the same way a camera's alias field sits
                        // above its hardware description.
                        Text {
                            text:           phoneRow.phoneData.declaredName || ""
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                        Text {
                            text:           qsTr("PPCP")
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                        Text {
                            text:           phoneRow.phoneData.pairingId || ""
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            elide:          Text.ElideRight
                        }
                    }
                }

                // Connection indicator — the same role the
                // "connected"/"available" wording used to play buried in the
                // meta line, promoted to its own slot so it reads at a glance
                // the way a camera's status dot label or an IMU's state text
                // does.
                Text {
                    text: phoneRow.isRevoked    ? qsTr("Revoked")
                        : phoneRow.isConnected  ? qsTr("Connected")
                        : phoneRow.isAvailable  ? qsTr("On this network")
                        :                          qsTr("Disconnected")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: phoneRow.isConnected ? Theme.colorGood
                         : phoneRow.isRevoked    ? Theme.colorText3
                         : phoneRow.isAvailable  ? Theme.colorAccent
                         :                          Theme.colorText3
                    Layout.alignment: Qt.AlignVCenter
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }

                // ── Which path this phone is on (design §6.1) ───────────────
                // "Surface which path a phone is on ... an operator who cannot
                // see that the cable did nothing cannot act on it."  A cable
                // that silently changed nothing is the failure this exists to
                // make visible, and it is the same fact the app log carries as
                // `transport=usb|wifi`.
                //
                // ⛔ HIDDEN when there is no live link rather than defaulting to
                // Wi-Fi: "we do not know" and "on the radio" are different
                // facts, and only one of them is ever observed.
                Rectangle {
                    visible: phoneRow.transport !== ""
                    implicitWidth:  transportLabel.implicitWidth + Theme.sp(12)
                    implicitHeight: transportLabel.implicitHeight + Theme.sp(4)
                    radius: height / 2
                    color: "transparent"
                    border.width: 1
                    border.color: phoneRow.transport === "cable" ? Theme.colorGood
                                                                 : Theme.colorBorderMid
                    opacity: Theme.borderOpacityNormal + 0.4
                    Layout.alignment: Qt.AlignVCenter

                    Text {
                        id: transportLabel
                        anchors.centerIn: parent
                        text: phoneRow.transport === "cable" ? qsTr("Cable") : qsTr("Wi-Fi")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color: phoneRow.transport === "cable" ? Theme.colorGood
                                                              : Theme.colorText3
                    }
                }

                // 7.4d — honoured immediately by this side, which means the
                // next handshake from that phone resolves nothing and fails
                // like any stranger's (7.7c).  Shown only for a row that IS
                // remembered: a row that is not (a `mu`>1 code's pairing,
                // 7.4f — this application never publishes one itself) has
                // nothing stored to forget, and it goes on its own when the
                // session that produced it closes (7.3b).
                // ── CORE 7.3a / MSG 5.2 — arming, from the host ─────────────
                //
                // ⚠ WHAT THIS SHOWS IS THE DEVICE'S ANSWER, NOT OUR MESSAGE.
                // `arm` sets a flag here the moment it is queued, so a control
                // that went green on click would be reporting that a packet
                // left the building.  5.2a makes the answer a `readiness`, and
                // this reads that: "Arming" until one arrives, "Armed" only
                // when the phone said `settled`, and the reason where 7.3c says
                // it cannot.
                //
                // ⛔ NOT part of the MVP — a capture device arms itself in the
                // shipping product, and this sits beside that.
                Text {
                    readonly property string st: phoneRow.isConnected
                                                 ? (phoneRow.phoneData.armState || "") : ""
                    readonly property string why: phoneRow.phoneData.armBlockedReason || ""
                    readonly property int    ms: phoneRow.phoneData.armReadyMs === undefined
                                                 ? -1 : phoneRow.phoneData.armReadyMs
                    visible: st !== "" && st !== "disarmed"
                    // ⚠ "stalled" is OUR conclusion and is worded as one. The
                    // phone did not say it; we waited past its own estimate and
                    // nothing terminal arrived. A device that is disarmed at the
                    // handset has no way to tell a host so, so this is also what
                    // that looks like from here — which is why it does not claim
                    // a fault.
                    text: st === "armed"   ? qsTr("Armed")
                        : st === "blocked" ? (why !== "" ? qsTr("Cannot arm — %1").arg(why)
                                                         : qsTr("Cannot arm"))
                        : st === "stalled" ? qsTr("No answer to arm")
                        : ms >= 0          ? qsTr("Arming — %1 ms").arg(ms)
                        :                    qsTr("Arming…")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: st === "armed"   ? Theme.colorGood
                         : st === "blocked" ? Theme.colorError
                         : st === "stalled" ? Theme.colorWarn
                         :                     Theme.colorAccent
                    Layout.alignment: Qt.AlignVCenter
                }

                PpButton {
                    readonly property bool isArmed:
                        ["armed", "arming", "stalled"].indexOf(
                            phoneRow.phoneData.armState || "") >= 0
                    label:            isArmed ? qsTr("Disarm") : qsTr("Arm")
                    visible:          phoneRow.isConnected
                    Layout.alignment: Qt.AlignVCenter
                    // Host arming is all-or-nothing across the bay: MSG 5.2's
                    // empty stream list means every open capture Stream, and
                    // this application's armed is a property of the whole
                    // capture path rather than of one phone's camera.
                    onClicked: if (root.controller) {
                                   if (isArmed) root.controller.disarmAll()
                                   else         root.controller.armAll()
                               }
                }

                PpButton {
                    label:            qsTr("Forget")
                    destructive:      true
                    visible:          phoneRow.isRemembered
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: if (root.controller)
                                   root.controller.forgetPairing(phoneRow.phoneData.pairingId)
                }
            }

            // ── Facts strip ──────────────────────────────────────────────────
            RowLayout {
                id: capsRow
                Layout.fillWidth:    true
                // The card's bottom breathing room — the "no margin" half of
                // Mark's report — the same role `bodyWrap`'s trailing
                // `Theme.sp(20)` plays under ImusPanel's capabilities strip.
                Layout.bottomMargin: Theme.sp(14)
                spacing: 0

                // Battery/thermal are 7.4b's `heartbeat_ack` readings — only a
                // CONNECTED phone has one; the "—" a remembered-but-absent
                // phone shows is the same "no reading" sentinel the other
                // facts already use, not a claim that the battery is dead.
                readonly property int    batteryPct: phoneRow.isConnected ? (phoneRow.phoneData.batteryPct === undefined ? -1 : phoneRow.phoneData.batteryPct) : -1
                readonly property string thermal:    phoneRow.isConnected ? (phoneRow.phoneData.thermal || "") : ""
                // 6.1f's clock agreement — THIS phone's own worst related-
                // timebase sigma (PpcpHostService::worstSyncSigmaMsFor()), not
                // the toolbar's cross-phone aggregate. -1 while unconnected or
                // while no relation has arrived yet (§6.3a not satisfied).
                readonly property real   syncSigmaMs: phoneRow.isConnected ? (phoneRow.phoneData.syncSigmaMs === undefined ? -1 : phoneRow.phoneData.syncSigmaMs) : -1
                // Design §6.1 — which path this phone is actually on. A
                // property of the LINK, so a remembered-but-absent phone has
                // none and shows the same "—" every other reading uses. ⛔ An
                // empty value must never render as "Wi-Fi": "we don't know"
                // and "on the radio" are different facts.
                readonly property string transport:  phoneRow.transport

                Repeater {
                    id: factsRepeater
                    model: [
                        { key: qsTr("Status"),     val: phoneRow.isRemembered ? qsTr("Remembered")
                                                       : phoneRow.isRevoked    ? qsTr("Forgotten")
                                                       :                          qsTr("Session only") },
                        { key: qsTr("Pairing ID"), val: (phoneRow.phoneData.pairingId || "—") },
                        { key: qsTr("Cameras"),    val: phoneRow.cameraCount > 0 ? String(phoneRow.cameraCount) : "—" },
                        // ⚠ This used to read a constant "PPCP", which told an
                        // operator nothing they did not already know. Since the
                        // wired transport there is a real answer here, and §6.1
                        // needs it visible: a cable that silently did nothing is
                        // the failure this line exists to make actionable.
                        { key: qsTr("Transport"),  val: capsRow.transport === "cable" ? qsTr("Cable")
                                                      : capsRow.transport === "wifi"  ? qsTr("Wi-Fi")
                                                      :                                 "—" },
                        // Colour-coded the same way ImusPanel's battery chip is:
                        // ≤20% / thermal critical reads red, <50% / elevated-or-
                        // serious reads amber, otherwise the ordinary text colour.
                        { key: qsTr("Battery"),    val: capsRow.batteryPct >= 0 ? (capsRow.batteryPct + "%") : "—",
                                                    tint: capsRow.batteryPct < 0 ? "" : capsRow.batteryPct <= 20 ? "error"
                                                        : capsRow.batteryPct < 50 ? "warn" : "" },
                        { key: qsTr("Thermal"),    val: capsRow.thermal || "—",
                                                    tint: capsRow.thermal === "critical" ? "error"
                                                        : (capsRow.thermal === "serious" || capsRow.thermal === "elevated") ? "warn" : "" },
                        // Same 5ms line the session toolbar's Cameras pill
                        // warns at — comfortably inside PPS's 50ms default
                        // coincidence window, but a plain visual "is this
                        // phone actually synced" check independent of it.
                        { key: qsTr("Clock agreement"), val: capsRow.syncSigmaMs >= 0 ? qsTr("± %1 ms").arg(capsRow.syncSigmaMs.toFixed(1)) : "—",
                                                    tint: capsRow.syncSigmaMs >= 0 && capsRow.syncSigmaMs > 5.0 ? "warn" : "" }
                    ]

                    delegate: Rectangle {
                        required property var modelData
                        required property int index

                        Layout.fillWidth: true
                        implicitHeight: capCol.implicitHeight + Theme.sp(18)
                        color: Theme.colorBg2

                        // Vertical separator (except last)
                        Rectangle {
                            anchors.right:  parent.right
                            anchors.top:    parent.top
                            anchors.bottom: parent.bottom
                            width: 1
                            color: Theme.colorBorderMid
                            opacity: Theme.borderOpacityNormal
                            visible: index < factsRepeater.count - 1
                        }

                        ColumnLayout {
                            id: capCol
                            anchors {
                                left:   parent.left
                                right:  parent.right
                                top:    parent.top
                                leftMargin:  Theme.sp(14)
                                rightMargin: Theme.sp(14)
                                topMargin:   Theme.sp(9)
                            }
                            spacing: Theme.sp(3)

                            Text {
                                text:            modelData.key
                                font.family:     Theme.fontData
                                font.pixelSize:  Theme.fontSzMicro
                                font.letterSpacing: Theme.trackingMicro
                                font.capitalization: Font.AllUppercase
                                color:           Theme.colorText3
                            }
                            Text {
                                text:            modelData.val
                                font.family:     Theme.fontData
                                font.pixelSize:  Theme.fontSzBody2
                                color:           modelData.tint === "error" ? Theme.colorError
                                                : modelData.tint === "warn"  ? Theme.colorWarn
                                                :                              Theme.colorText
                                elide:           Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }   // mainCol
    }

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
                text: qsTr("Phones")
            }
            Text {
                text: qsTr("A phone running PinPoint Capture pairs by scanning a code from the home screen, and its cameras then join the devices list. Once paired, a phone is remembered and can reconnect without a new code — until you forget it here.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                Layout.fillWidth: true
            }

            // ── The build has no PPCP at all ───────────────────────────────
            Text {
                visible: !root.havePpcp
                text:    qsTr("This build was made without the PinPoint Connect libraries, so it cannot pair with a phone.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                Layout.fillWidth: true
            }

            // ── Enumerated devices header ──────────────────────────────────
            RowLayout {
                visible: root.havePpcp
                Layout.fillWidth: true

                Text {
                    text:                qsTr("REMEMBERED PHONES")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                    Layout.fillWidth:    true
                }
            }

            // ── Nothing paired yet ─────────────────────────────────────────
            Text {
                visible: root.havePpcp && root.rows.length === 0
                text:    qsTr("No phone has paired with this computer yet. Use “Pair a device” on the home screen to scan a code.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                font.italic:    true
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                Layout.fillWidth: true
            }

            // ── One row per held pairing ───────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.sp(26)
                spacing: Theme.sp(8)
                visible: root.rows.length > 0

                Repeater {
                    model: root.rows

                    delegate: PhoneDeviceRow {
                        id: pairingRow
                        objectName: "setting_pairedPhones"
                        required property var modelData
                        property bool searchHighlight: false

                        phoneData:        modelData
                        Layout.fillWidth: true
                    }
                }
            }

            // ── Status summary ─────────────────────────────────────────────
            Rectangle {
                id: summaryRect
                visible: root.rows.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: Theme.sp(26)
                height:  Theme.sp(40)
                color:   Theme.colorBg2
                radius:  Theme.radius
                border.width: 1
                border.color: Theme.colorBorderMid

                readonly property int rememberedCount: root.rows.filter(function(r) { return r.persisted }).length
                readonly property int connectedCount:  root.rows.filter(function(r) { return r.status === "connected" }).length

                RowLayout {
                    anchors.fill:    parent
                    anchors.margins: Theme.sp(12)
                    spacing:         Theme.sp(16)

                    Text {
                        text:  summaryRect.connectedCount + qsTr(" connected")
                        color: summaryRect.connectedCount > 0 ? Theme.colorGood : Theme.colorText2
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }

                    Rectangle { width: 1; height: Theme.sp(14); color: Theme.colorBorderStrong; opacity: 0.4 }

                    Text {
                        text:  summaryRect.rememberedCount + qsTr(" remembered")
                        color: Theme.colorText2
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}
