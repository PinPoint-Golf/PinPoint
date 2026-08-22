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

// The sessions a connected capture device is offering.  Work package H5.
//
// ⚠ THIS IS WHERE H3'S "File -> Import Session..." WENT.  This application has
// no menus and no native dialogs, and the user's intent was never to import a
// file: a connected device says what it holds and the host picks from the list
// (plan §9, 22 August 2026).  So there is no file picker here, no format
// chooser, and no "import" verb — one row per offered Session and one control on
// each, in the DEVICES area of the home screen where the device it came from is
// already listed.
//
// ⚠ AND IT DEGRADES TO NOTHING.  `ppcpOffers` is a context property installed
// only when libppcp is linked (H0: a build without it must still run), so every
// reference here is guarded.  A build with no PPCP shows no heading and no rows,
// not an empty section with a title.

import QtQuick
import PinPointStudio

Column {
    id: root

    // The controller, or `null` on a build without libppcp.  Passed in rather
    // than reached for, so this component is drivable from a harness.
    property var controller: (typeof ppcpOffers !== "undefined") ? ppcpOffers : null

    readonly property int offerCount: controller ? controller.count : 0

    width:   parent ? parent.width : 0
    height:  visible ? implicitHeight : 0
    spacing: 0
    visible: offerCount > 0

    // ── Heading ────────────────────────────────────────────────────────────
    Item { width: 1; height: Theme.sp(24) }

    Item {
        width:  root.width
        height: Theme.sp(20)

        Text {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
            text:               qsTr("SESSIONS OFFERED")
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
            width:               Math.max(0, parent.width * 0.5)
            horizontalAlignment: Text.AlignRight
        }
    }
    Item { width: 1; height: 12 }

    // ── One row per offered Session ────────────────────────────────────────
    Repeater {
        model: root.controller

        Item {
            id: offerRow

            // Role names come from PpcpOfferController::roleNames().  Every one
            // of them is something the DEVICE asserted (I10: completeness is
            // asserted by the owner and never inferred by us), or something our
            // own ledger knows.
            required property string  sessionId
            required property string  mintingPeerId
            required property string  epochLabel
            required property bool    hasEpoch
            required property string  completeness
            required property string  bytesLabel
            required property bool    hasBytes
            required property bool    alreadyHeld
            required property bool    accepted
            required property int     index

            width:  root.width
            height: Theme.sp(44)

            Rectangle {
                anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                height: 1
                color:  Theme.colorBorder
            }

            Row {
                anchors.fill: parent
                spacing:      0

                // Completeness dot.  `partial` is amber and `unknown` is neither
                // green nor amber: the device did not say, and "did not say" is a
                // different answer from "fine" (I10).
                Item {
                    width:  Theme.sp(20)
                    height: parent.height

                    Rectangle {
                        width:  Theme.sp(6)
                        height: Theme.sp(6)
                        radius: Theme.sp(3)
                        anchors.centerIn: parent
                        color: offerRow.completeness === "complete" ? Theme.colorGood
                             : offerRow.completeness === "partial"  ? Theme.colorWarn
                                                                    : Theme.colorBorderStrong
                    }
                }

                Column {
                    width:  parent.width - Theme.sp(20) - Theme.sp(150) - Theme.sp(96)
                    height: parent.height
                    spacing: Theme.sp(2)

                    Item { width: 1; height: Theme.sp(6) }

                    Text {
                        width:             parent.width
                        // The epoch is a LABEL and nothing else (I15 / CORE
                        // 5.3b): it is formatted for a human and never
                        // subtracted from anything.  Where the device sent none,
                        // the Session's own opaque id is what there is to show,
                        // and inventing a date would be worse than showing one.
                        text:              offerRow.hasEpoch ? offerRow.epochLabel
                                                             : offerRow.sessionId
                        font.family:       Theme.fontBody
                        font.pixelSize:    Theme.fontSzBody
                        color:             Theme.colorText
                        elide:             Text.ElideRight
                    }

                    Text {
                        width:          parent.width
                        text: {
                            var bits = []
                            // 8.5c scopes identity by the MINTING peer, which
                            // may not be the device handing over the bytes — so
                            // it is shown rather than assumed.
                            bits.push(offerRow.mintingPeerId)
                            if (offerRow.completeness !== "complete")
                                bits.push(offerRow.completeness)
                            if (offerRow.hasBytes) bits.push(offerRow.bytesLabel)
                            return bits.join("  ·  ")
                        }
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color:          Theme.colorText3
                        elide:          Text.ElideRight
                    }
                }

                // I34 — a re-import is a no-op, and the user is told BEFORE they
                // press rather than after a replay that changed nothing.
                Text {
                    width:               Theme.sp(150)
                    height:              parent.height
                    text:                offerRow.accepted    ? qsTr("receiving…")
                                       : offerRow.alreadyHeld ? qsTr("already held")
                                                              : ""
                    font.family:         Theme.fontData
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingData
                    color:               Theme.colorText3
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment:   Text.AlignVCenter
                    elide:               Text.ElideRight
                }

                Item {
                    width:  Theme.sp(96)
                    height: parent.height

                    PpButton {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        label:   offerRow.alreadyHeld ? qsTr("Get again") : qsTr("Get")
                        primary: !offerRow.alreadyHeld
                        enabled: !offerRow.accepted
                        // MSG 9.2 — `session_accept`, carrying the digests this
                        // host already holds so their payloads are not replayed
                        // (9.1a).  The device then puts its stored frames onto
                        // the live link and they arrive through the ordinary
                        // ingest path; there is no importer at this end.
                        onClicked: if (root.controller) root.controller.acceptOffer(offerRow.index)
                    }
                }
            }
        }
    }
}
