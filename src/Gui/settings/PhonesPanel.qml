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
// is "opt-in, visible to the user, and individually revocable", and all three
// of those are this panel.  A remembered pairing is a standing ability to
// complete a handshake with this host, so it is shown rather than kept out of
// the way.  It used to be a table at the foot of the home screen; deleting that
// table without providing this would have been a regression against 7.4b, which
// is why the two changes are one commit.
//
// ⚠ AND IT TELLS THE TRUTH ABOUT WINDOWS AND LINUX.  makePlatformPairingStore()
// returns a keychain store on macOS and NULL everywhere else, so persist()
// refuses and no phone is remembered across a launch at all.  An empty list on
// such a machine would read as "you have never paired one", which is a
// different and untrue statement — so the state is said out loud.
//
// Rows come from `ppcpHost.phones` — the SAME list the home screen's DEVICES
// section and the resource monitor draw from, because a paired phone is a
// device and there should be one answer to what phones this host knows about.
// What is extra here is the pair of controls 7.4b requires.

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

    // The same rows the DEVICES list and the resource monitor show — one list,
    // built once in PpcpHostService::phones(), rather than this panel deciding
    // for itself what counts as a phone.  A live, unscanned code is not in it:
    // that is a QR on screen and belongs to the pairing dialog.
    readonly property var rows: root.controller ? root.controller.phones : []

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
                text: qsTr("A phone running PinPoint Capture pairs by scanning a code from the home screen, and its cameras then join the devices list. A pairing works once unless you choose to remember it; a remembered phone can reconnect without a new code until you forget it here.")
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

            // ── This machine cannot keep a pairing ─────────────────────────
            // Said plainly, because the alternative is an empty list that looks
            // like "nothing has ever paired" on a machine where remembering has
            // never been possible.
            Text {
                objectName: "setting_phonesNoProtectedStorage"
                property bool searchHighlight: false
                visible: root.havePpcp && Qt.platform.os !== "osx"
                text:    qsTr("This computer has no protected key storage, so phones cannot be remembered between launches. Pairing still works — you will be asked to scan a code each time.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorWarn
                wrapMode:       Text.WordWrap
                Layout.fillWidth: true
            }

            // ── Nothing paired yet ─────────────────────────────────────────
            Text {
                visible: root.havePpcp && root.rows.length === 0
                text:    qsTr("No phone has paired with this computer yet.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                Layout.fillWidth: true
            }

            // ── One row per held pairing ───────────────────────────────────
            Repeater {
                model: root.rows

                RowLayout {
                    id: pairingRow
                    objectName: "setting_pairedPhones"
                    required property var modelData
                    property bool searchHighlight: false

                    Layout.fillWidth: true
                    spacing: Theme.sp(16)

                    Rectangle {
                        anchors { bottom: parent.bottom; bottomMargin: -Theme.sp(8) }
                        width:  parent.width
                        height: 1
                        color:  Theme.colorBorder
                    }

                    // Green only while the pairing is actually usable.  A
                    // revoked one is a tombstone until reap() takes it (7.3b),
                    // and showing it as live would be a lie about what a
                    // handshake from that device would do now (7.7c).
                    Rectangle {
                        Layout.preferredWidth:  Theme.sp(6)
                        Layout.preferredHeight: Theme.sp(6)
                        radius: Theme.sp(3)
                        color: pairingRow.modelData.status === "connected" ? Theme.colorGood
                             : pairingRow.modelData.invalidated             ? Theme.colorBorderStrong
                             : pairingRow.modelData.persisted               ? Theme.colorAccent
                                                                            : Theme.colorBorderStrong
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(3)

                        Text {
                            // What the phone called itself in its MSG `declare`,
                            // remembered against its pairing; the shortened
                            // handle for one that has never declared.
                            text:           pairingRow.modelData.name
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color:          Theme.colorText
                        }
                        Text {
                            text: {
                                var bits = [pairingRow.modelData.persisted
                                                ? qsTr("Remembered")
                                                : qsTr("Paired this session")]
                                if (pairingRow.modelData.status === "connected")
                                    bits.push(qsTr("connected now"))
                                if (pairingRow.modelData.invalidated) bits.push(qsTr("revoked"))
                                bits.push(pairingRow.modelData.pairingId)
                                return bits.join("  ·  ")
                            }
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            elide:          Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    // 7.4a/7.4b — opt-in, and it stores PRK in the platform
                    // keychain and nothing else (5.1c).  Refused for a code
                    // whose `mu` exceeded 1: that pairing's key material is held
                    // by every peer that scanned it (7.4f).
                    PpButton {
                        label:   qsTr("Remember")
                        visible: !pairingRow.modelData.persisted
                                 && !pairingRow.modelData.invalidated
                        onClicked: if (root.controller)
                                       root.controller.rememberPairing(pairingRow.modelData.pairingId)
                    }

                    // 7.4d — honoured immediately by this side, which means the
                    // next handshake from that phone resolves nothing and fails
                    // like any stranger's (7.7c).
                    PpButton {
                        label:       qsTr("Forget")
                        destructive: true
                        visible:     pairingRow.modelData.persisted
                        onClicked: if (root.controller)
                                       root.controller.forgetPairing(pairingRow.modelData.pairingId)
                    }
                }
            }
        }
    }
}
