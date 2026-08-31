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
        property int    connectedCount:  0
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

    // ── A host with one phone on it, for the alias field ─────────────────────
    //
    // ⚠ THE ALIAS FIELD HAS BEEN UNTYPEABLE TWICE, and both times the report was
    // the same sentence: "the phone heartbeat refreshes it while I am editing".
    // The first cure (30 Aug) moved `heartbeat_ack` off phonesChanged(); the
    // second (31 Aug) moved `relation_update` off it too.  Neither is what this
    // asserts.  Signals fired at a cadence are a C++ matter and this file cannot
    // see them; what it CAN pin down is the thing that made them fatal — that a
    // rebuild of the list threw away the text and the focus.  With this test the
    // third door, whatever it turns out to be, is an inconvenience rather than
    // an unusable field.
    QtObject {
        id: fakePhoneHost

        // Matches PpcpHostService: `phones` notifies phonesChanged, which QML
        // generates for a `var` property of that name.
        property var phones: [{
            kind: "Phone", pairingId: "pair-one", alias: "", declaredName: "Pixel 8",
            name: "Pixel 8", status: "connected", persisted: true, invalidated: false,
            transport: "wifi", counterpartId: "", batteryPct: 74, thermal: "nominal",
            syncSigmaMs: 3.5, armState: "disarmed", armBlockedReason: "", armReadyMs: -1,
            dataRateStr: "—", model: "", backend: "PPCP", identifier: "pair-one",
            hasWarning: false
        }]

        signal phoneHealthChanged()

        property int    aliasWrites: 0
        property string lastAlias:   ""

        function phoneHealth(pairingId) {
            return { batteryPct: 74, thermal: "nominal", syncSigmaMs: 3.5 }
        }

        // ⚠ EVERY REFERENCE IS `fakePhoneHost.`-QUALIFIED, AND THE UNQUALIFIED
        // VERSION IS NOT A STYLE CHOICE.  The H0 panel above is `id: phones`,
        // which shadows this object's own `phones` property inside its
        // functions — an id is not an lvalue, so the write failed at runtime
        // with "left-hand side of assignment operator is not an lvalue" and the
        // list never moved.
        //
        // ⚠ AND THE ROWS ARE REBUILT RATHER THAN PATCHED, for the same reason:
        // an object reached through a `var` property is not assignable in
        // place.  The real `phones()` builds fresh QVariantMaps on every read
        // anyway, so this is also the more faithful fake.
        function setPhoneAlias(pairingId, alias) {
            fakePhoneHost.aliasWrites += 1
            fakePhoneHost.lastAlias = alias
            fakePhoneHost.phones = fakePhoneHost.copyRows(pairingId, alias)
        }

        // What a phonesChanged() that is NOT an alias write looks like from the
        // panel's side: the list re-read with a READING moved in it, which is
        // exactly what a `heartbeat_ack` or a `relation_update` used to cause.
        //
        // ⚠ IT HAS TO MOVE SOMETHING.  A list rebuilt with identical contents
        // leaves the delegates standing — Qt diffs it — so an "identical copy"
        // version of this asserted nothing at all: the field it was meant to
        // destroy was never touched.  One changed reading is the difference
        // between a real regression test and a decoration.
        property int heartbeats: 0
        function heartbeat() {
            fakePhoneHost.heartbeats += 1
            var src = fakePhoneHost.phones
            var copy = []
            for (var i = 0; i < src.length; ++i) {
                var row = {}
                for (var k in src[i]) row[k] = src[i][k]
                row.batteryPct  = 74 - fakePhoneHost.heartbeats
                row.syncSigmaMs = 3.5 + fakePhoneHost.heartbeats
                copy.push(row)
            }
            fakePhoneHost.phones = copy
        }

        function copyRows(pairingId, alias) {
            var src = fakePhoneHost.phones
            var copy = []
            for (var i = 0; i < src.length; ++i) {
                var row = {}
                for (var k in src[i]) row[k] = src[i][k]
                if (pairingId && row.pairingId === pairingId) row.alias = alias
                copy.push(row)
            }
            return copy
        }
    }

    PhonesPanel {
        id: livePhones
        width: 600; height: 400
        controller: fakePhoneHost
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

        // ⚠ THIS TEST USED TO ASSERT THE OPPOSITE, AND THE RULE MOVED RATHER
        // THAN CHANGED.  The panel used to guard its own close with "unless a
        // phone got in first, because closing that code's session would take
        // the link down with it".  That rule now lives in PpcpHostService
        // (`displayedCodePairedAPhone()`), where every caller gets it instead
        // of only this one — and it has to, because the code on screen after a
        // phone pairs is no longer the spent one anyway: a fresh one is minted
        // the moment the link is adopted, so the next angle can just scan.
        //
        // What the panel owes is therefore simply: close the code you are
        // showing.  The invariant this replaced is pinned in C++ by
        // ppcp_host_service_test.
        function test_closing_after_a_phone_connected_still_closes_its_code() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            fakeHost.connected = true
            fakeHost.connectedCount = 1

            dialog.close()
            tryCompare(dialog, "opened", false)
            compare(fakeHost.closeCount, 1)
        }

        // Two angles is the ordinary case, so the panel says how many rather
        // than picking one phone's name to stand for both.
        function test_several_phones_are_counted_not_named() {
            dialog.open()
            tryCompare(fakeHost, "publishCount", 1)
            var stage = findByName(dialog.contentItem, "ppcpPairStage")

            fakeHost.connected = true
            fakeHost.connectedCount = 1
            fakeHost.peerName = "Pixel 8"
            verify(stage.text.indexOf("Pixel 8") >= 0, stage.text)

            // A second angle arrives.  `peerName` goes empty, exactly as the
            // real controller reports it, because there is no honest single
            // answer once there are two.
            fakeHost.connectedCount = 2
            fakeHost.peerName = ""
            verify(stage.text.indexOf("2") >= 0, stage.text)
            compare(stage.color, Theme.colorGood)

            fakeHost.connected = false
            fakeHost.connectedCount = 0
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

        // The report, twice over: "the edit alias still gets refreshed by the
        // phone heartbeat, making it impossible to edit whilst a phone is
        // connected."  A rebuild lands mid-word here on purpose.
        function test_an_alias_being_typed_survives_the_list_being_rebuilt() {
            var field = findByName(livePhones, "phoneAliasField")
            verify(field !== null, "no alias field in the panel")
            field.forceActiveFocus()
            tryVerify(function() { return field.activeFocus })

            // Unshifted, so lower case is what a key event actually produces.
            keyClick(Qt.Key_B); keyClick(Qt.Key_A); keyClick(Qt.Key_Y)
            compare(field.text, "bay")
            compare(fakePhoneHost.aliasWrites, 0, "a half-typed alias was written down")

            // The phone list is re-read while the caret is between "Bay" and
            // whatever came next.  Every delegate — this field included — is
            // destroyed and built again.
            fakePhoneHost.heartbeat()

            var again = findByName(livePhones, "phoneAliasField")
            verify(again !== field, "the delegate was not rebuilt — the test proves nothing")
            verify(again !== null, "the row did not come back")
            tryVerify(function() { return again.activeFocus }, 2000,
                      "the rebuilt field did not take the focus back")
            compare(again.text, "bay", "the half-typed alias was lost in the rebuild")
            compare(fakePhoneHost.aliasWrites, 0,
                    "the rebuild committed an unfinished alias")

            // Finishing it still writes it down exactly once.
            keyClick(Qt.Key_2)
            keyClick(Qt.Key_Return)
            tryCompare(fakePhoneHost, "aliasWrites", 1)
            compare(fakePhoneHost.lastAlias, "bay2")
        }

        // The other half: a real farewell IS a commit, and must not be mistaken
        // for a rebuild by the deferral that makes the test above pass.
        function test_leaving_the_field_writes_the_alias_down() {
            var field = findByName(livePhones, "phoneAliasField")
            verify(field !== null)
            fakePhoneHost.aliasWrites = 0
            field.forceActiveFocus()
            tryVerify(function() { return field.activeFocus })
            field.selectAll()
            keyClick(Qt.Key_D); keyClick(Qt.Key_T); keyClick(Qt.Key_L)
            probe.forceActiveFocus()          // the user moves on
            tryCompare(fakePhoneHost, "aliasWrites", 1)
            compare(fakePhoneHost.lastAlias, "dtl")
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
