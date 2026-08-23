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

// "Pair a device" — PPCP-RV §4, the primary rendezvous path.  Work package H6.
//
// ⚠ IN-SCREEN, IN THE DEVICES AREA, BESIDE THE CAMERAS IT ADDS.  This
// application has no menus and no native dialogs; a new user action goes on the
// screen where its result appears, and the result of pairing is a camera in the
// DEVICES list.  So the code is displayed here, under the devices, above the
// sessions that device then offers.
//
// ⚠ AND IT DEGRADES TO NOTHING.  `ppcpHost` is a context property installed
// only when libppcp AND OpenSSL are both present (a build without either must
// still run), so every reference is guarded and the whole panel hides rather
// than showing a heading over an empty space.
//
// The QR is drawn on a Canvas from `qrRows` — one string of '0'/'1' per module
// row.  ⚠ THE URI ITSELF IS NEVER EXPOSED TO QML.  RV 4.4c and 7.2b forbid a
// payload reaching a log, a crash report or a diagnostic export, and a QML
// property carrying it is one console.warn away from all three.  The modules
// are the only form of the code that leaves C++.

import QtQuick
import PinPointStudio

Column {
    id: root

    property var controller: (typeof ppcpHost !== "undefined") ? ppcpHost : null

    readonly property bool available: controller !== null && controller.listening
    readonly property bool showing:   available && controller.codeLive && controller.qrSize > 0

    width:   parent ? parent.width : 0
    height:  visible ? implicitHeight : 0
    spacing: 0
    visible: available

    // ── Heading ────────────────────────────────────────────────────────────
    Item { width: 1; height: Theme.sp(24) }

    Item {
        width:  root.width
        height: Theme.sp(20)

        Text {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
            text:               qsTr("PAIR A DEVICE")
            font.family:        Theme.fontData
            font.pixelSize:     Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro
            color:              Theme.colorText3
        }

        Text {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            text:                root.controller ? root.controller.status : ""
            font.family:         Theme.fontBody
            font.pixelSize:      Theme.fontSzBody2
            font.weight:         Theme.fontBodyWeight
            color:               Theme.colorText3
            elide:               Text.ElideRight
            width:               Math.max(0, parent.width * 0.6)
            horizontalAlignment: Text.AlignRight
        }
    }
    Item { width: 1; height: 12 }

    // ── The code, or the control that produces one ─────────────────────────
    Row {
        width:   root.width
        spacing: Theme.sp(20)

        // The symbol.  A four-module quiet zone is added HERE and not in the
        // encoder, because ISO 18004's quiet zone depends on what the code is
        // drawn onto and that is a rendering decision.
        Item {
            width:   root.showing ? Theme.sp(180) : 0
            height:  root.showing ? Theme.sp(180) : 0
            visible: root.showing

            Rectangle {
                anchors.fill: parent
                color:        "#ffffff"
                radius:       Theme.radius
            }

            Canvas {
                id: qrCanvas
                anchors.fill: parent
                antialiasing: false

                // Repainted whenever the code changes.  The countdown ticks
                // once a second and does NOT repaint: the modules are the same
                // modules, and redrawing 1681 rectangles every second on the
                // home screen would be a cost for no picture.
                Connections {
                    target: root.controller
                    function onCodeChanged() { qrCanvas.requestPaint() }
                }

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    if (!root.controller) return
                    var rows = root.controller.qrRows
                    var n = root.controller.qrSize
                    if (!rows || n <= 0) return

                    // Four modules of quiet zone on every side (ISO 18004).
                    var quiet = 4
                    var total = n + quiet * 2
                    var s = Math.floor(Math.min(width, height) / total)
                    if (s < 1) s = 1
                    var pad = Math.floor((Math.min(width, height) - s * total) / 2)

                    ctx.fillStyle = "#ffffff"
                    ctx.fillRect(0, 0, width, height)
                    ctx.fillStyle = "#000000"
                    for (var y = 0; y < n; ++y) {
                        var row = rows[y]
                        for (var x = 0; x < n; ++x) {
                            if (row.charAt(x) !== "1") continue
                            ctx.fillRect(pad + (x + quiet) * s, pad + (y + quiet) * s, s, s)
                        }
                    }
                }
            }
        }

        Column {
            width:   root.width - (root.showing ? Theme.sp(200) : 0)
            spacing: Theme.sp(8)

            Text {
                width: parent.width
                text:  root.showing
                       // 4.4a — expiry is reported as expiry and never as a
                       // failure to connect, and the publisher holds the
                       // authoritative clock (7.3e) so this countdown is the
                       // real one.
                       ? qsTr("Scan this with the capture device. The code expires in %1s and works once.")
                            .arg(root.controller.codeSecondsLeft)
                       : qsTr("Show a code the capture device can scan. It works once and expires in five minutes.")
                wrapMode:       Text.WordWrap
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText2
            }

            // 4.3d — every address the code carries, shown because when
            // discovery fails (and at a range it will — 3.6a) this list is the
            // whole of why a pairing did or did not work.
            Text {
                width:          parent.width
                visible:        root.showing && root.controller.codeEndpoints.length > 0
                text:           qsTr("Reachable at: %1").arg(
                                    root.controller ? root.controller.codeEndpoints.join("   ") : "")
                wrapMode:       Text.WordWrap
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }

            Row {
                spacing: Theme.sp(10)

                PpButton {
                    label:   root.showing ? qsTr("New code") : qsTr("Show pairing code")
                    primary: !root.showing
                    // 7.3d — a fresh psk and sid every time; a code is never
                    // regenerated with the same secret.  Pressing this while one
                    // is showing therefore REPLACES it, and 7.3b invalidates the
                    // one it replaced.
                    onClicked: if (root.controller) root.controller.publishPairingCode()
                }

                PpButton {
                    label:   qsTr("Hide")
                    visible: root.showing
                    // 7.3b — invalidated when its session closes, used or not.
                    onClicked: if (root.controller) root.controller.closePairingCode()
                }
            }
        }
    }

    // ── Outstanding codes and remembered devices (7.3a, 7.4b) ─────────────
    // 7.4b makes persistence "opt-in, visible to the user, and individually
    // revocable", and all three of those are this table.  A remembered pairing
    // is a standing ability to complete a handshake with this host, so it is
    // shown rather than kept out of the way.
    Repeater {
        model: root.controller ? root.controller.outstandingCodes : []

        Item {
            id: codeRow
            required property var modelData
            required property int index

            width:   root.width
            height:  visible ? Theme.sp(34) : 0
            // A live code is already the QR above; this table is for what is
            // left over — a spent code that became a pairing, and a remembered
            // device.
            visible: modelData.persisted || modelData.usesRemaining === 0

            Rectangle {
                anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                height: 1
                color:  Theme.colorBorder
            }

            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                text: {
                    var bits = []
                    bits.push(codeRow.modelData.persisted ? qsTr("Remembered device")
                                                          : qsTr("Paired this session"))
                    bits.push(codeRow.modelData.pairingId)
                    if (codeRow.modelData.invalidated) bits.push(qsTr("revoked"))
                    return bits.join("  ·  ")
                }
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText2
                elide:          Text.ElideRight
                width:          Math.max(0, parent.width - Theme.sp(200))
            }

            Row {
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                spacing: Theme.sp(8)

                PpButton {
                    label:   qsTr("Remember")
                    visible: !codeRow.modelData.persisted && !codeRow.modelData.invalidated
                    // 7.4a/7.4b — opt-in, and it stores PRK in the platform
                    // keychain and nothing else (5.1c).  Refused for a code
                    // whose `mu` exceeded 1: that pairing's key material is held
                    // by every peer that scanned it (7.4f).
                    onClicked: if (root.controller)
                                   root.controller.rememberPairing(codeRow.modelData.pairingId)
                }

                PpButton {
                    label:   qsTr("Forget")
                    visible: codeRow.modelData.persisted
                    // 7.4d — honoured immediately by this side, which means the
                    // next handshake from that device resolves nothing and fails
                    // like any stranger (7.7c).
                    onClicked: if (root.controller)
                                   root.controller.forgetPairing(codeRow.modelData.pairingId)
                }
            }
        }
    }
}
