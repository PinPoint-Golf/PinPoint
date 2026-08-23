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


// "Pair to my phone" — PPCP-RV §4, the primary rendezvous path.  Work package
// H6, second pass.
//
// This replaces PpcpPairPanel.qml, which sat inline in the DEVICES area of the
// home screen and was visible whenever the listener was up — which, in a build
// with libppcp and OpenSSL, is always.  So every user saw a heading, an
// engineer's status line, a paragraph of narrative, a raw IP:port list and a
// table of 16-hex pairing handles, on every launch, whether or not they had a
// phone.  One button now opens this, and the button is the only thing the home
// screen carries.
//
// ⚠ A QML Popup AND NOT A NATIVE DIALOG.  The house rule is no menus and no
// native dialogs; an in-app modal over Overlay.overlay is neither, and it is
// what PpAboutDialog and the quit-confirm in Main.qml already are.
//
// ⚠ THE URI IS NEVER EXPOSED TO QML.  RV 4.4c and 7.2b forbid a payload
// reaching a log, a crash report or a diagnostic export, and a QML property
// carrying it is one console.warn away from all three.  `qrRows` — one string
// of '0'/'1' per module row — is the only form of the code that leaves C++.
//
// ⚠ AND THE CONTROLLER IS INJECTED, NOT REACHED FOR.  `ppcpHost` is a context
// property installed only when libppcp AND OpenSSL are both present, so it must
// degrade to nothing (H0); and the offscreen QML suite installs only
// `appSettings`, so a component that reached for `ppcpHost` directly could not
// be driven by a test at all.

import QtQuick
import QtQuick.Controls
import PinPointStudio

Popup {
    id: root
    objectName: "ppcpPairDialog"

    // The live host service, a test's fake, or null on a build without PPCP.
    property var controller: (typeof ppcpHost !== "undefined") ? ppcpHost : null

    readonly property bool showing:   controller !== null && controller.codeLive
                                      && controller.qrSize > 0
    readonly property bool connected: controller !== null && controller.connected
    readonly property string peerName: controller ? controller.peerName : ""

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    dim: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: Theme.sp(24)
    width: Math.min(Theme.sp(520), (parent ? parent.width : Theme.sp(520)) - Theme.sp(48))

    // 4.4a — expiry is reported as expiry and never as a failure to connect, and
    // 7.3e puts the authoritative clock at the publisher, so this countdown is
    // the real one and not an estimate drawn alongside it.
    readonly property int secondsLeft: (controller && controller.codeLive)
                                       ? controller.codeSecondsLeft : 0
    readonly property string countdownText: {
        if (!root.showing) return ""
        var s = root.secondsLeft
        if (s <= 0) return qsTr("This code has expired.")
        var m = Math.floor(s / 60)
        var r = s % 60
        return qsTr("This code expires in %1:%2.")
                 .arg(m).arg(r < 10 ? "0" + r : "" + r)
    }

    // The staged line, in the PpAboutDialog `pal` idiom.  The controller's own
    // `status` string is shown UNDER it rather than instead of it: it is honest
    // and useful ("Device connected — TLS1.3 …") and it is not a sentence a
    // golfer should have to read to know whether their phone arrived.
    // A phone that arrived and did not become a link.  ⚠ THE COUNT IS READ, NOT
    // JUST THE TEXT: a phone that fails twice for the same reason produces the
    // same string, and a binding on the text alone would not re-evaluate — the
    // panel would sit on a stale-looking message through a second attempt.  See
    // [[qml-dead-statement-bindings]] for why this is read INSIDE the
    // expression rather than as a bare statement.
    readonly property int failureCount: controller ? controller.failureCount : 0
    readonly property string failureText: (controller && root.failureCount > 0)
                                          ? controller.lastFailureText : ""

    readonly property var stage: {
        if (root.connected && root.peerName !== "")
            return { text: qsTr("Connected to %1.").arg(root.peerName), fg: Theme.colorGood }
        if (root.connected)
            return { text: qsTr("Phone connected — securing the link…"), fg: Theme.colorAccent }
        // ⚠ ABOVE "waiting" and BELOW "connected".  Above, because displacing
        // that line is the whole point: a refused phone used to leave it reading
        // "Waiting for your phone to scan…" for ever.  Below, because a phone
        // that succeeds afterwards has answered the question.
        //
        // ⚠ AND NOT ABOVE THE COUNTDOWN, which is a different binding entirely
        // (`countdownText`) and stays that way: RV 4.4a requires expiry to be
        // reported AS expiry and never as a failure to connect, so the two must
        // not be able to displace one another.
        if (root.failureText !== "")
            return { text: root.failureText, fg: Theme.colorWarn }
        if (root.showing && root.secondsLeft > 0)
            return { text: qsTr("Waiting for your phone to scan…"), fg: Theme.colorText2 }
        return { text: qsTr("No code is showing."), fg: Theme.colorText3 }
    }

    // 7.3d — a fresh psk and sid every time; a code is never regenerated with
    // the same secret.  Opening with no live code publishes one, so the panel
    // shows a code rather than a button that produces one.
    onOpened: {
        if (root.controller && !root.controller.codeLive)
            root.controller.publishPairingCode()
    }

    // 7.3b — a displayed code is invalidated when its panel goes, used or not.
    // Unless a phone got in first: that code is already spent on a live link,
    // and closing its session would take the link down with it.
    onClosed: {
        if (root.controller && root.controller.codeLive && !root.connected)
            root.controller.closePairingCode()
        root._showEndpoints = false
    }

    property bool _showEndpoints: false

    background: Rectangle {
        color: Theme.colorSurface
        radius: Theme.radiusLg
        border.width: 1
        border.color: Theme.colorBorderStrong
    }

    contentItem: Column {
        spacing: Theme.sp(16)

        // ── Title, and the way out ─────────────────────────────────────────
        Item {
            width:  parent.width
            height: Theme.sp(28)

            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                text:            qsTr("Pair to my phone")
                font.family:     Theme.fontDisplay
                font.italic:     Theme.fontDisplayItalic
                font.weight:     Theme.fontDisplayWeight
                font.pixelSize:  Math.min(Theme.sp(20), Theme.fontSzDisplay)
                color:           Theme.colorText
            }

            Text {
                id: closeGlyph
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text:           "✕"
                font.family:    Theme.fontSymbol
                font.pixelSize: Theme.fontSzBody
                color:          closeMa.containsMouse ? Theme.colorText : Theme.colorText3
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                PpPressable {
                    id: closeMa
                    objectName: "ppcpPairDialogClose"
                    onClicked: root.close()
                }
            }
        }

        // ── The symbol, and what to do with it ─────────────────────────────
        Row {
            width:   parent.width
            spacing: Theme.sp(20)

            // ⚠ WHITE, IN EVERY THEME.  A QR is dark-on-light by definition and
            // a reader looking at an inverted one may or may not cope; twelve
            // aesthetics and a dark mode are not worth finding out.
            Item {
                id: plate
                width:   Theme.sp(200)
                height:  Theme.sp(200)
                visible: root.showing

                Rectangle {
                    anchors.fill: parent
                    color:        "#ffffff"
                    radius:       Theme.radius
                }

                Canvas {
                    id: qrCanvas
                    objectName: "ppcpPairQr"
                    anchors.fill: parent
                    antialiasing: false

                    // Repainted when the CODE changes.  The countdown shares
                    // `codeChanged`, but since the host was fixed to emit it
                    // once a second rather than once per 20 ms tick, a repaint
                    // per emission is a second's worth of work and not fifty.
                    Connections {
                        target: root.controller
                        ignoreUnknownSignals: true
                        function onCodeChanged() { qrCanvas.requestPaint() }
                    }

                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        if (!root.controller) return
                        var rows = root.controller.qrRows
                        var n = root.controller.qrSize
                        if (!rows || n <= 0) return

                        // Four modules of quiet zone on every side.  Added here
                        // and not in the encoder because ISO 18004's quiet zone
                        // depends on what the code is drawn onto, and that is a
                        // rendering decision.
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
                                ctx.fillRect(pad + (x + quiet) * s,
                                             pad + (y + quiet) * s, s, s)
                            }
                        }
                    }
                }
            }

            Column {
                width:   parent.width - (root.showing ? (plate.width + Theme.sp(20)) : 0)
                spacing: Theme.sp(10)

                Repeater {
                    model: [
                        qsTr("Open PinPoint Capture on your phone."),
                        qsTr("Tap Connect to Studio and point the camera at this code."),
                        qsTr("Your phone's cameras join the devices list.")
                    ]

                    Row {
                        required property string modelData
                        required property int index
                        width:   parent.width
                        spacing: Theme.sp(8)

                        Text {
                            text:               (parent.index + 1) + "."
                            font.family:        Theme.fontData
                            font.pixelSize:     Theme.fontSzBody2
                            font.letterSpacing: Theme.trackingData
                            color:              Theme.colorText3
                        }
                        Text {
                            width:          parent.width - Theme.sp(20)
                            text:           parent.modelData
                            wrapMode:       Text.WordWrap
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                            color:          Theme.colorText2
                            lineHeight:     1.4
                        }
                    }
                }
            }
        }

        PpDivider { width: parent.width }

        // ── Where the pairing has got to ───────────────────────────────────
        Column {
            width:   parent.width
            spacing: Theme.sp(6)

            Text {
                objectName:     "ppcpPairStage"
                width:          parent.width
                text:           root.stage.text
                wrapMode:       Text.WordWrap
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody
                color:          root.stage.fg
            }

            Text {
                objectName:     "ppcpPairCountdown"
                width:          parent.width
                visible:        root.countdownText !== ""
                text:           root.countdownText
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }

            // The controller's own words, kept as detail rather than as the
            // headline.  It is the only place a TLS description or a listener
            // failure is ever said out loud.
            Text {
                width:          parent.width
                visible:        root.controller && root.controller.status !== ""
                text:           root.controller ? root.controller.status : ""
                wrapMode:       Text.WordWrap
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
            }
        }

        // ── Controls, and the addresses behind the disclosure ──────────────
        Row {
            width:   parent.width
            spacing: Theme.sp(10)

            PpButton {
                objectName: "ppcpPairNewCode"
                label:      root.showing && root.secondsLeft > 0 ? qsTr("New code")
                                                                 : qsTr("Get a new code")
                primary:    !root.showing || root.secondsLeft === 0
                // 7.3d — a fresh psk and sid.  Pressing this while a code shows
                // REPLACES it, and 7.3b invalidates the one it replaced.
                onClicked:  if (root.controller) root.controller.publishPairingCode()
            }

            // 4.3d — every address the code carries.  Behind a disclosure
            // because when discovery fails (and at a range it will — 3.6a) this
            // list is the whole of why a pairing did or did not work, and the
            // rest of the time it is four numbers nobody needs.
            PpButton {
                label:   root._showEndpoints ? qsTr("Hide addresses")
                                             : qsTr("Trouble connecting?")
                visible: root.controller && root.controller.codeEndpoints.length > 0
                onClicked: root._showEndpoints = !root._showEndpoints
            }
        }

        Text {
            width:          parent.width
            visible:        root._showEndpoints && root.controller
            text:           qsTr("Your phone must be able to reach this computer at one of: %1")
                                .arg(root.controller ? root.controller.codeEndpoints.join("   ") : "")
            wrapMode:       Text.WordWrap
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
        }
    }
}
