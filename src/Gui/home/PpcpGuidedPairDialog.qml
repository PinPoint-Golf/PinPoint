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

// PPCP-RV §11 — RV-6 guided pairing, the screen a person actually reads.
// Work package H10.
//
// ⛔⛔ THIS FILE IS TWO OF THE PROTOCOL'S REQUIREMENTS AND THEY ARE NOT CODE.
// CR-01 §4 lists nine implementation traps and then adds: "two more that are
// UX, not code, and are MUSTs anyway".  Both of them live here, and an
// implementation can satisfy every byte of §11 and still fail them.
//
//   (1) 11.7d — THE AFFIRMATIVE CONTROL IS NOT THE DEFAULT AND NOT WHERE A
//       STRAY TAP LANDS, AND THE PROMPT ASKS WHETHER THE NUMBERS *MATCH*.
//       "A dialogue whose default is *Continue* is a dialogue that
//       authenticates whatever is on the other end."
//
//       ⚠ THIS DELIBERATELY INVERTS THIS APPLICATION'S OWN FOOTER CONVENTION,
//       AND THE INVERSION IS THE POINT.  Everywhere else — ModelMint,
//       PpExportOptionsSheet, PpAboutDialog — the pattern is a right-aligned
//       row with Cancel first and the affirmative last carrying `primary: true`.
//       That is correct for "Export" and catastrophic here: it puts the button
//       that authenticates a stranger exactly where the operator's thumb
//       already is, and paints it as the thing to do.  So here the SAFE answer
//       is last and `primary`, the affirmative is first and plain, and Esc
//       (`CloseOnEscape`, "cancel = safe default") does not affirm.
//       ⛔ DO NOT "FIX" THIS TO MATCH THE OTHER DIALOGUES.
//
//   (2) 11.9c — A MISMATCH OR A MAC FAILURE IS NOT REPORTED IN TERMS THAT
//       INVITE A RETRY.  "A mismatch is the ONE signal this path produces that
//       an attack is under way, and a peer whose dialogue makes retrying the
//       reflex has converted its single-attempt bound into an unbounded one by
//       way of the operator's muscle memory."  So there is NO retry control at
//       all when `guidedMayRetry` is false — not a greyed-out one, not one
//       behind a confirmation.  None.
//
// ⛔ AND TRAP 3 (11.3d1) IS WHY THE LIST HAS NO NUMBERS IN IT.  `dl` exists so
// a browser seeing four open windows can tell them apart, and a list is the
// obvious interface — so showing a candidate number beside each row is the
// obvious next step.  It would give an attacker advertising N windows N blind
// draws against ONE confirmation, and worse, THE OPERATOR DOES THE SELECTING:
// shown a list of numbers one of which matches the phone in their hand, they
// tap the match and read it as success.  The user picks a device BEFORE any
// attempt begins; digits exist only for the one attempt that is running.
//
// ⛔ AND TRAP 8 (11.1d) IS WHY THERE IS NOWHERE HERE TO TYPE THE OTHER NUMBER.
// The comparison is worth something only because it crosses a channel the
// attacker is not on, and the only such channel is a person looking at two
// screens.  A field that took the phone's digits, or a control that accepted
// the phone's word that they matched, would remove the entire security of this
// path while leaving every byte on the wire unchanged.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PinPointStudio

Popup {
    id: root
    objectName: "ppcpGuidedPairDialog"

    // The live host service, a test's fake, or null on a build without PPCP.
    property var controller: (typeof ppcpHost !== "undefined") ? ppcpHost : null

    readonly property var  windows:    controller ? controller.guidedWindows : []
    readonly property string phase:    controller ? controller.guidedPhase : "idle"
    // ⛔ 11.7e / 11.7f — EMPTY outside the one window in which the digits exist.
    // C++ enforces it; this binding only renders what it is given, and must
    // never cache what it last saw.
    readonly property string digits:   controller ? controller.guidedDigits : ""
    readonly property string message:  controller ? controller.guidedMessage : ""
    readonly property bool   mayRetry: controller ? controller.guidedMayRetry : false
    readonly property bool   offerCode: controller ? controller.guidedOfferCode : false
    readonly property bool   comparing: root.phase === "comparing"
    readonly property bool   failed:    root.phase === "failed"
    readonly property bool   paired:    root.phase === "paired"
    readonly property bool   busy:      root.phase === "dialling" ||
                                        root.phase === "exchanging" ||
                                        root.phase === "confirming"

    // Offered instead of a guided pairing, never as a "try again" (11.9d/11.9d1).
    signal pairingCodeRequested()

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    dim: true
    // Esc = cancel, which is this application's stated safe default and is also
    // 11.7d's: the escape hatch must not be the affirmative one.
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: Theme.sp(24)
    width: Math.min(Theme.sp(520), (parent ? parent.width : Theme.sp(520)) - Theme.sp(48))

    background: Rectangle {
        color: Theme.colorSurface
        radius: Theme.radiusLg
        border.width: 1
        border.color: Theme.colorBorderStrong
    }

    onClosed: {
        // 11.9a — closing mid-exchange ends the attempt and leaves no pairing
        // at either peer.  ⛔ It does NOT affirm: a dialogue dismissed is a
        // dialogue in which nobody said the numbers matched.
        if (!controller) return
        if (controller.guidedActive) controller.cancelGuidedPairing()
        controller.dismissGuidedResult()
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp(16)

        Text {
            objectName: "guidedTitle"
            Layout.fillWidth: true
            text: root.comparing ? qsTr("Do these numbers match?")
                 : root.paired   ? qsTr("Paired")
                 : root.failed   ? qsTr("Pairing stopped")
                 : root.busy     ? qsTr("Pairing…")
                                 : qsTr("Pair with a phone that is waiting")
            color: Theme.colorText
            font.pixelSize: Theme.fontSzHeading
            font.bold: true
            wrapMode: Text.WordWrap
        }

        // ── The candidate list.  ⛔ NO DIGITS HERE (11.3d1, trap 3) ─────────
        ColumnLayout {
            objectName: "guidedWindowList"
            Layout.fillWidth: true
            spacing: Theme.sp(8)
            visible: root.phase === "idle" && !root.paired && !root.failed

            Text {
                Layout.fillWidth: true
                visible: root.windows.length === 0
                text: qsTr("No phone is offering to pair right now. On the phone, "
                         + "start pairing, then look here again.")
                color: Theme.colorText2
                font.pixelSize: Theme.fontSzBody
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: root.windows
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(12)

                    Text {
                        Layout.fillWidth: true
                        // ⛔ 4.4d, reached through 3.3g — UNTRUSTED DISPLAY
                        // TEXT.  `dl` is shown before anything has been
                        // authenticated, so it is whatever a stranger put on
                        // the wire.  C++ has already escaped and truncated it;
                        // `elide` and `WordWrap` are what stop a wide one
                        // pushing the buttons off the dialogue.
                        //
                        // ⚠ AND IT IS NOT AN IDENTITY.  A device calling itself
                        // "Mark's iPhone" has asserted nothing — the six digits
                        // are the only thing in this dialogue that authenticates
                        // anybody, which is why the label is styled as a dim
                        // caption and not as a name.
                        text: modelData.hasLabel ? modelData.label
                                                 : qsTr("A phone (no name given)")
                        color: Theme.colorText2
                        font.pixelSize: Theme.fontSzBody
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }

                    PpButton {
                        objectName: "guidedSelect_" + index
                        label: qsTr("Pair with this")
                        // ⛔ ONE NAME, ONE ATTEMPT.  This is the selection
                        // 11.3d1 requires to happen BEFORE the attempt begins,
                        // and C++ refuses a second while one is live.
                        onClicked: if (root.controller)
                                       root.controller.beginGuidedPairing(modelData.instanceName)
                    }
                }
            }
        }

        // ── Working ────────────────────────────────────────────────────────
        Text {
            Layout.fillWidth: true
            visible: root.busy
            text: root.phase === "confirming"
                  ? qsTr("Waiting for the other device…")
                  : qsTr("Connecting…")
            color: Theme.colorText2
            font.pixelSize: Theme.fontSzBody
            wrapMode: Text.WordWrap
        }

        // ── ⛔ THE COMPARISON.  11.7a, 11.7d ───────────────────────────────
        ColumnLayout {
            objectName: "guidedCompare"
            Layout.fillWidth: true
            spacing: Theme.sp(12)
            // 11.7e — the digits AND any control that affirms them appear only
            // once 11.5d has completed.  `comparing` is exactly that state.
            visible: root.comparing

            Text {
                objectName: "guidedDigits"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                // 11.7a — exactly six decimal digits with leading zeros, and
                // 11.7d — grouped identically at both peers ("435 948").  C++
                // formats it; nothing here reformats or re-spaces it, because
                // two peers grouping differently make an operator compare two
                // shapes rather than two numbers.
                text: root.digits
                color: Theme.colorText
                font.pixelSize: Theme.fontSzDataLg * 2
                font.bold: true
                font.letterSpacing: Theme.trackingData
                // The tabular data face, so 1 and 7, and 0 and 8, cannot be
                // confused at arm's length.  The operator is the component most
                // likely to fail, and six digits is already the compromise
                // §11.7 made in their favour ("a person asked to compare ten
                // digits at a range, in daylight, forty times a day, stops
                // comparing").
                font.family: Theme.fontData
            }

            Text {
                objectName: "guidedPrompt"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                // ⛔ 11.7d — THE PROMPT ASKS WHETHER THE NUMBERS **MATCH**,
                // not whether to trust, continue, allow or connect.  Every one
                // of those asks the operator to make a judgement they have no
                // basis for; "do these match?" asks them to read.
                text: qsTr("Compare these with the numbers on the phone. "
                         + "Do they match?")
                color: Theme.colorText2
                font.pixelSize: Theme.fontSzBody
                wrapMode: Text.WordWrap
            }

            // ⛔⛔ 11.7d's FOOTER, AND IT IS DELIBERATELY BACKWARDS FROM EVERY
            // OTHER DIALOGUE IN THIS APPLICATION.  See the file header.
            //
            //   • The AFFIRMATIVE is FIRST and PLAIN — not `primary`, not
            //     `attention`, and not in the right-hand slot the operator's
            //     thumb goes to by habit.
            //   • The SAFE answer is LAST and `primary` — it is the one a stray
            //     tap reaches, and a stray tap must not authenticate a stranger.
            //   • Neither has focus; `PpButton` takes none, and Esc cancels.
            //
            // ⛔ DO NOT REORDER THESE TO MATCH THE HOUSE CONVENTION.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                PpButton {
                    objectName: "guidedAffirm"
                    // ⛔ NOT `primary`.  NOT pre-selected.  NOT last.
                    label: qsTr("Yes, they match")
                    // ⛔ 11.7c — THIS IS AN AFFIRMATIVE ACT BY A PERSON AT THIS
                    // END, and it is the only thing that reaches
                    // `confirmGuidedDigitsMatch()` in a shipping build.  The
                    // RT-20c harness presses this same control by name
                    // ("match") under PP_PPCP_RV6_HARNESS; it supplies the tap
                    // and never the comparison.
                    onClicked: if (root.controller)
                                   root.controller.confirmGuidedDigitsMatch()
                }

                Item { Layout.fillWidth: true }

                PpButton {
                    objectName: "guidedReject"
                    label: qsTr("No, they are different")
                    // The safe answer is the prominent one.
                    primary: true
                    onClicked: if (root.controller)
                                   root.controller.rejectGuidedDigits()
                }
            }
        }

        // ── The outcome ────────────────────────────────────────────────────
        Text {
            objectName: "guidedMessage"
            Layout.fillWidth: true
            visible: (root.failed || root.paired) && root.message !== ""
            text: root.message
            color: root.failed ? Theme.colorWarn : Theme.colorText2
            font.pixelSize: Theme.fontSzBody
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.failed || root.paired
            spacing: Theme.sp(12)

            Item { Layout.fillWidth: true }

            // 11.9d1 / 11.9d — the pairing code, offered INSTEAD of a guided
            // pairing.  §4's path is REQUIRED of every implementation (2a), it
            // does not depend on multicast, and it is the answer to both
            // plausible causes.  ⚠ It is not a retry: it is a different path.
            PpButton {
                objectName: "guidedUseCode"
                visible: root.offerCode
                label: qsTr("Use a pairing code instead")
                onClicked: {
                    root.pairingCodeRequested()
                    root.close()
                }
            }

            // ⛔⛔ 11.9c — THIS CONTROL DOES NOT EXIST AFTER A MISMATCH OR A MAC
            // FAILURE.  `guidedMayRetry` is false for `rejected`,
            // `commitment_mismatch` and `invalid_key`, and true only for a
            // timeout or a closed connection, which "carry no such implication
            // and may be reported as the ordinary failure they are".
            //
            // ⚠ `visible: false` AND NOT `enabled: false`.  A greyed-out *Try
            // again* still teaches the operator that trying again is the shape
            // of the answer, and 11.9c is about the reflex, not the click.
            PpButton {
                objectName: "guidedRetry"
                visible: root.failed && root.mayRetry
                label: qsTr("Look again")
                onClicked: {
                    // 11.9b — this IS the further explicit user action, and it
                    // returns to the LIST.  It does not re-dial anything: the
                    // user selects again, before any attempt begins (11.3d1).
                    if (root.controller) root.controller.dismissGuidedResult()
                }
            }

            PpButton {
                objectName: "guidedDone"
                label: root.paired ? qsTr("Done") : qsTr("Close")
                primary: root.paired
                onClicked: root.close()
            }
        }

        // ── Cancel, while something is in flight ───────────────────────────
        RowLayout {
            Layout.fillWidth: true
            visible: root.busy || root.phase === "idle"
            spacing: Theme.sp(12)

            Item { Layout.fillWidth: true }

            PpButton {
                objectName: "guidedCancel"
                label: qsTr("Cancel")
                onClicked: root.close()
            }
        }
    }
}
