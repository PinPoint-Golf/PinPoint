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
import QtTest
import PinPointStudio

// PpcpPairDialog, loaded for real and driven through its injected controller.
//
// ⚠ THE FAKE IS WHY THE DIALOG TAKES A `controller` PROPERTY AT ALL.  The real
// one is `ppcpHost`, a context property installed only when libppcp and OpenSSL
// are both present, and this binary's setup installs `appSettings` and nothing
// else — so a component that reached for `ppcpHost` directly could not be stood
// up here.  The deleted PpcpPairPanel had the same seam and no test used it.
//
// What is asserted: opening publishes a code rather than waiting for a press,
// the countdown is formatted for a human, each of the four staged status lines
// appears for its own condition, closing invalidates the code (RV 7.3b) but not
// when a phone already got in, and a build with no PPCP renders nothing.
Item {
    id: probe
    width: 900; height: 700

    // Stands in for PpcpHostService.  Only the surface PpcpPairDialog binds to,
    // and every name matches the real Q_PROPERTY so a rename breaks this file
    // rather than silently passing against a fake that drifted.
    QtObject {
        id: fakeHost

        signal codeChanged()

        property bool   listening:       true
        property bool   codeLive:        false
        property int    qrSize:          0
        property var    qrRows:          []
        property int    codeSecondsLeft: 0
        property var    codeEndpoints:   []
        property bool   connected:       false
        property string peerName:        ""
        property string status:          "Waiting for a device on port 7788."
        // A phone that arrived and did not become a link.  Names match the real
        // Q_PROPERTYs so a rename breaks this file rather than drifting.
        property string lastFailureText: ""
        property int    failureCount:    0

        property int publishCount: 0
        property int closeCount:   0

        function publishPairingCode() {
            publishCount += 1
            // A 21-module symbol, which is a real version-1 size.  The contents
            // are not a code and do not need to be: the dialog draws modules.
            var rows = []
            for (var y = 0; y < 21; ++y) {
                var r = ""
                for (var x = 0; x < 21; ++x) r += ((x + y) % 3 === 0) ? "1" : "0"
                rows.push(r)
            }
            qrRows = rows
            qrSize = 21
            codeLive = true
            codeSecondsLeft = 272
            codeEndpoints = ["192.168.1.24:7788", "10.0.0.4:7788"]
            status = "Scan the code with the capture device."
            lastFailureText = ""
            codeChanged()
            return true
        }

        function closePairingCode() {
            closeCount += 1
            codeLive = false
            qrSize = 0
            qrRows = []
            codeSecondsLeft = 0
            codeChanged()
        }
    }

    PpcpPairDialog {
        id: dialog
        controller: fakeHost
    }

    // The H0 case: no libppcp, so no context property and no controller.
    PpcpPairDialog {
        id: nullDialog
        controller: null
    }

    // Smoke-loaded, not driven.  Neither has behaviour worth a suite of its own
    // yet, but both are new and both are reachable only from a screen this
    // binary does not stand up, so "it instantiates without a QML error" is the
    // difference between a typo found here and a typo found by Mark.
    PpQrGlyph {
        id: glyph
        width: 18; height: 18
    }

    PhonesPanel {
        id: phones
        width: 600; height: 400
        controller: null            // the H0 build, which is what a test is
    }

    function findByName(item, name) {
        if (!item) return null
        if (item.objectName === name) return item
        for (var i = 0; i < item.children.length; ++i) {
            var found = findByName(item.children[i], name)
            if (found) return found
        }
        return null
    }

    TestCase {
        name: "PpcpPairDialog"
        when: windowShown

        function init() {
            fakeHost.publishCount = 0
            fakeHost.closeCount   = 0
            fakeHost.connected    = false
            fakeHost.peerName     = ""
            if (fakeHost.codeLive) fakeHost.closePairingCode()
            fakeHost.closeCount   = 0
            if (dialog.opened) dialog.close()
        }

        function cleanup() {
            if (dialog.opened) dialog.close()
        }

        function test_opening_shows_a_code_rather_than_a_button_that_makes_one() {
            compare(fakeHost.publishCount, 0)
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            verify(dialog.showing)
        }

        // A code already showing is not displaced by reopening the panel: 7.3d
        // mints a fresh psk and sid every publish, so a needless one would
        // invalidate a code the user may already be scanning.
        function test_reopening_does_not_replace_a_code_already_showing() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            dialog.close()
            // Closing invalidated it, so this open publishes a second.
            dialog.open()
            tryCompare(fakeHost, "publishCount", 2)
            // Now force a live code and reopen without closing it.
            dialog.close()
            fakeHost.publishPairingCode()
            var n = fakeHost.publishCount
            dialog.open()
            compare(fakeHost.publishCount, n)
        }

        // 4.4a — expiry is reported as expiry, and to a human. "272s" was what
        // the deleted panel printed.
        function test_the_countdown_is_minutes_and_seconds() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)

            var line = findByName(dialog.contentItem, "ppcpPairCountdown")
            verify(line !== null)
            verify(line.text.indexOf("4:32") >= 0,
                   "expected 4:32 in \"" + line.text + "\"")

            fakeHost.codeSecondsLeft = 65
            fakeHost.codeChanged()
            verify(line.text.indexOf("1:05") >= 0,
                   "a single-digit second must be padded: \"" + line.text + "\"")
        }

        function test_an_expired_code_says_so_rather_than_counting_below_zero() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            fakeHost.codeSecondsLeft = 0
            fakeHost.codeChanged()

            var line = findByName(dialog.contentItem, "ppcpPairCountdown")
            verify(line.text.indexOf("expired") >= 0, line.text)
        }

        // The four stages, each under its own condition.
        function test_the_staged_line_follows_the_pairing() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            var stage = findByName(dialog.contentItem, "ppcpPairStage")
            verify(stage !== null)

            verify(stage.text.indexOf("Waiting") >= 0, stage.text)
            compare(stage.color, Theme.colorText2)

            // Scanned: a link exists but the peer has not declared yet.
            fakeHost.connected = true
            verify(stage.text.indexOf("securing") >= 0, stage.text)
            compare(stage.color, Theme.colorAccent)

            // Declared: MSG 3.3 gave us the phone's own name.
            fakeHost.peerName = "Pixel 8"
            verify(stage.text.indexOf("Pixel 8") >= 0, stage.text)
            compare(stage.color, Theme.colorGood)

            // No code and no link.
            fakeHost.connected = false
            fakeHost.peerName = ""
            fakeHost.closePairingCode()
            verify(stage.text.indexOf("No code") >= 0, stage.text)
            compare(stage.color, Theme.colorText3)
        }

        // A refused phone displaces "Waiting…", and a phone that gets in
        // afterwards displaces the refusal.
        function test_a_failed_arrival_displaces_the_waiting_line() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            var stage = findByName(dialog.contentItem, "ppcpPairStage")
            verify(stage !== null)
            verify(stage.text.indexOf("Waiting") >= 0, stage.text)

            // A phone arrived and was refused.  ⚠ BOTH properties move: the
            // count is what a repeat of the same failure changes, and the arm
            // reads it so the binding re-evaluates.
            fakeHost.lastFailureText = "A phone connected but did not finish "
                                     + "securing the link in time, and was dropped."
            fakeHost.failureCount = 1
            verify(stage.text.indexOf("did not finish securing") >= 0, stage.text)
            compare(stage.color, Theme.colorWarn)

            // RV 4.4a — expiry is reported AS expiry, on its own line, and a
            // failure must not have swallowed it.
            var countdown = findByName(dialog.contentItem, "ppcpPairCountdown")
            verify(countdown !== null)
            verify(countdown.text.indexOf("expires") >= 0, countdown.text)

            // A phone that gets in afterwards has answered the question.
            fakeHost.connected = true
            fakeHost.peerName = "Pixel 8"
            verify(stage.text.indexOf("Pixel 8") >= 0, stage.text)
            compare(stage.color, Theme.colorGood)

            fakeHost.connected = false
            fakeHost.peerName = ""
            fakeHost.closePairingCode()
        }

        // 7.3b — a displayed code is invalidated when its panel goes, used or
        // not.  The close button and Esc are the same path.
        function test_closing_invalidates_the_code_that_was_showing() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            compare(fakeHost.closeCount, 0)

            var x = findByName(dialog.contentItem, "ppcpPairDialogClose")
            verify(x !== null, "the dialog has no close control")
            mouseClick(x)

            tryCompare(dialog, "opened", false)
            tryCompare(fakeHost, "closeCount", 1)
        }

        // ...unless a phone got in first.  That code is spent on a live link,
        // and closing its session would take the link down with it.
        function test_closing_after_a_phone_connected_leaves_the_link_alone() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            fakeHost.connected = true

            dialog.close()
            tryCompare(dialog, "opened", false)
            compare(fakeHost.closeCount, 0)
        }

        // H0 — a build with neither libppcp nor OpenSSL has no `ppcpHost`, and
        // the dialog must be inert rather than throwing on every binding.
        // The icon is a QR that must NOT be scannable: three finders and a
        // scatter with no timing pattern, no format stripe and no data. What is
        // checkable here is that it holds still — a seed re-rolled per paint
        // would make the button flicker on every hover.
        function test_the_qr_icon_is_drawn_once_and_holds_still() {
            verify(glyph.modules >= 9)
            var seed = glyph._seed
            verify(seed !== 1, "the seed was never rolled")
            glyph.requestPaint()
            wait(50)
            compare(glyph._seed, seed, "a repaint advanced the generator")
        }

        // RV 7.4b lives here now that the home-screen table is gone: a
        // remembered pairing must stay visible and individually revocable.
        function test_the_phones_panel_stands_up_with_no_ppcp() {
            compare(phones.havePpcp, false)
            compare(phones.rows.length, 0)
            compare(phones.scrollToItem(""), true)
            compare(phones.scrollToItem("no_such_row"), false)
        }

        function test_with_no_controller_there_is_nothing_to_show() {
            compare(nullDialog.showing, false)
            compare(nullDialog.connected, false)
            compare(nullDialog.countdownText, "")
            nullDialog.open()
            tryCompare(nullDialog, "opened", true)
            var stage = findByName(nullDialog.contentItem, "ppcpPairStage")
            verify(stage !== null)
            verify(stage.text.indexOf("No code") >= 0, stage.text)
            nullDialog.close()
        }
    }
}
