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

// RT-20c's driver for THIS side — the real application, headless.
//
//   PinPointStudio --probe-qml <abs path to this file> \
//                  --rv6-endpoint 127.0.0.1:PORT \
//                  --rv6-action  match|different
//
// Loaded by Main.qml's `--probe-qml` loader, so it runs inside the REAL
// application with the real `ppcpHost`: the same engine, the same OpenSSL, the
// same five frames and the same single-attempt rule the operator's build has.
// Needs a build configured with `-DPP_PPCP_RV6_HARNESS=ON`, which a shipping
// build refuses to configure.
//
// ⛔⛔ IT SUPPLIES THE TAP AND NEVER THE COMPARISON (11.1d, TRAP 8).
//
// Read what this file does with `digits`: it PRINTS them and nothing else.  It
// never receives the counterpart's, never compares two values, and never lets
// any value decide which control it presses — `--rv6-action` is a constant the
// harness was told to send before the attempt began.  A peer that compared the
// digits in software, or accepted the counterpart's word that they matched,
// "removes the entire security of the path while leaving every byte on the wire
// unchanged" and passes every static test in the document.
//
// ⚠ WHO IS ALLOWED TO COMPARE, AND WHY IT IS NOT THIS FILE.  RT-20c's assertion
// is that the two applications either side of the interposed relay see
// DIFFERENT digits.  That comparison is made OUTSIDE both peers, by the harness
// reading two printed values — an observation about the protocol that decides
// nothing about authentication, and it is made by neither peer.  Trap 8 is a
// PEER comparing digits to decide whether to trust; this is a test asserting
// that two peers disagree.  The distinction is the whole row, so it is written
// down here rather than left to be re-derived.

import QtQuick

Item {
    id: probe
    anchors.fill: parent

    property var host: (typeof ppcpHost !== "undefined") ? ppcpHost : null
    property string endpoint: ""
    property string action: "different"      // the SAFE default, deliberately
    property bool   started: false
    property bool   tapped: false

    function arg(name, fallback) {
        var a = Qt.application.arguments
        var i = a.indexOf(name)
        return (i >= 0 && i + 1 < a.length) ? a[i + 1] : fallback
    }

    Component.onCompleted: {
        probe.endpoint = arg("--rv6-endpoint", "")
        // ⚠ DEFAULTS TO `different`, NOT `match`.  A harness invoked without an
        // action must not affirm: 11.7d's principle — "a dialogue whose default
        // is *Continue* is a dialogue that authenticates whatever is on the
        // other end" — applies to a driver's defaults as much as to a button's.
        probe.action = arg("--rv6-action", "different")

        if (!probe.host) { console.warn("RV6-PROBE fail no ppcpHost"); return }
        console.warn("RV6-PROBE available " + probe.host.guidedAvailable)

        if (probe.endpoint === "") {
            console.warn("RV6-PROBE fail no --rv6-endpoint")
            return
        }
        var c = probe.endpoint.lastIndexOf(":")
        var h = probe.endpoint.substring(0, c)
        var p = parseInt(probe.endpoint.substring(c + 1))
        // 3.7h — an endpoint learned out of band is a conformant way to reach a
        // window; §11 constrains the handshake and not how it was found.  It
        // becomes ONE candidate, and nothing dials it here.
        if (!probe.host.addGuidedEndpoint(h, p, "relay")) {
            console.warn("RV6-PROBE fail addGuidedEndpoint refused "
                       + "(is PP_PPCP_RV6_HARNESS on?)")
            return
        }
        var w = probe.host.guidedWindows
        if (w.length === 0) { console.warn("RV6-PROBE fail no candidate"); return }
        // ⛔ ONE name, the selection 11.3d1 requires BEFORE the attempt begins.
        probe.started = probe.host.beginGuidedPairing(w[w.length - 1].instanceName)
        console.warn("RV6-PROBE began " + probe.started)
    }

    Connections {
        target: probe.host
        function onGuidedChanged() {
            if (!probe.host) return
            var ph = probe.host.guidedPhase
            console.warn("RV6-PROBE phase " + ph)

            if (ph === "comparing" && !probe.tapped) {
                // ⛔ PRINTED, NOT COMPARED.  This is the value a person would
                // read off this screen, emitted so the harness outside both
                // peers can hold it against the other peer's.
                console.warn("RV6-PROBE digits " + probe.host.guidedDigits)
                probe.tapped = true
                // ⛔ THE TAP.  `probe.action` was fixed before any digits
                // existed; nothing above changes it and nothing reads a digit
                // to decide it.
                probe.host.guidedUserAction(probe.action === "match"
                                            ? "match" : "different")
                console.warn("RV6-PROBE tapped " + probe.action)
            }
            if (ph === "paired")
                console.warn("RV6-PROBE result paired")
            if (ph === "failed")
                console.warn("RV6-PROBE result failed "
                           + "mayRetry=" + probe.host.guidedMayRetry
                           + " offerCode=" + probe.host.guidedOfferCode
                           + " msg=" + probe.host.guidedMessage)
        }
    }

    // 11.3e allows 30 seconds to the exchange and 60 to an affirmation, and
    // 3.7b bounds the window at 180.  A driver that outlived all three would
    // report a hang as a pass, so it says so and stops.
    Timer {
        interval: 90000
        running: true
        onTriggered: console.warn("RV6-PROBE fail timed out in phase "
                                + (probe.host ? probe.host.guidedPhase : "?"))
    }
}
