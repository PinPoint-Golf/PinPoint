// PPCP automated-test probe — the machine-readable half of a PPC integration run.
//
//   QT_QPA_PLATFORM=offscreen PinPointStudio --probe-qml /abs/path/ppcp_assert.qml
//
// Loads over the live UI with the full app context (`ppcpHost`, `shotController`,
// `cameraManager`, `appSettings`, …), prints one JSON line per poll to the app
// log, and exits non-zero if the run did not do what was asked of it.
//
// ⚠ WHAT IT IS FOR.  A capture device driven in a simulator produces no ball
// strike and no sound, so a host with a working microphone hears nothing and the
// corroboration rule refuses every Shot it is sent.  `injectDetection()` supplies
// the one input a simulator cannot, and `ppcpStats()`/`shotStats()` let the run
// ASSERT rather than scrape a log — the corroboration verdict is otherwise a
// ppDebug line a release build does not even emit.
import QtQuick

Item {
    id: probe
    anchors.fill: parent

    // ── What this run expects.  Override on the command line if you like:
    //    --probe-qml … --expect-shots 3
    readonly property int expectShots: {
        var i = Qt.application.arguments.indexOf("--expect-shots")
        return (i >= 0 && i + 1 < Qt.application.arguments.length)
               ? parseInt(Qt.application.arguments[i + 1]) : 1
    }
    // Inject a host detection alongside each device Shot, so the corroboration
    // rule is exercised rather than side-stepped.  Off means "no host detector
    // available", which is the accept-unweighed path and the simplest first run.
    readonly property bool corroborate:
        Qt.application.arguments.indexOf("--corroborate") >= 0

    readonly property int timeoutMs: {
        var i = Qt.application.arguments.indexOf("--probe-timeout-ms")
        return (i >= 0 && i + 1 < Qt.application.arguments.length)
               ? parseInt(Qt.application.arguments[i + 1]) : 120000
    }

    property int elapsed: 0
    property int lastSeen: -1

    function snapshot() {
        return { "ppcp": ppcpHost.ppcpStats(), "shot": shotController.shotStats() }
    }

    function finish(ok, why) {
        console.warn("PROBE RESULT " + (ok ? "PASS" : "FAIL") + " — " + why)
        console.warn("PROBE STATS " + JSON.stringify(snapshot()))
        Qt.exit(ok ? 0 : 1)
    }

    Timer {
        interval: 250; running: true; repeat: true
        onTriggered: {
            probe.elapsed += interval
            var s = probe.snapshot()

            // ⚠ A device that nominated while we were not listening is the one
            // failure that used to be silent.  Fail loudly on it rather than
            // timing out with no explanation.
            if (s.ppcp.unarbitrated > 0)
                return probe.finish(false,
                    "a device nominated and the arbiter was not running (unarbitrated="
                    + s.ppcp.unarbitrated + ")")

            // A Shot arrived: give the host its own evidence if asked, so the
            // corroboration rule can pass on something other than absence.
            var seen = s.ppcp.observedForeign
            if (probe.corroborate && seen > probe.lastSeen) {
                probe.lastSeen = seen
                // Acoustic, "now" — inside the 50 ms window by construction.
                shotController.injectDetection(4, -1)
            }

            if (s.shot.committed >= probe.expectShots)
                return probe.finish(true, "committed " + s.shot.committed + " shot(s)")

            if (probe.elapsed >= probe.timeoutMs)
                return probe.finish(false,
                    "timed out after " + probe.timeoutMs + " ms — committed "
                    + s.shot.committed + " of " + probe.expectShots
                    + ", corroboration " + s.shot.corroboratePass + " pass / "
                    + s.shot.corroborateFail + " fail, last verdict: "
                    + s.shot.lastVerdict)
        }
    }

    Component.onCompleted: console.warn("PROBE START expect=" + expectShots
                                        + " corroborate=" + corroborate
                                        + " timeout=" + timeoutMs + "ms")
}
