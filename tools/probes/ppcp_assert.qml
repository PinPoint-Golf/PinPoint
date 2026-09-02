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

    // ⭐ HOW MANY CLIPS MUST LAND IN A SWING.  0 keeps the old shot-only
    // behaviour.  Anything above it asserts the whole leg: the phone was asked,
    // it answered, the bytes were written into the swing folder and the
    // document names them.
    readonly property int expectClips: {
        var i = Qt.application.arguments.indexOf("--expect-clips")
        return (i >= 0 && i + 1 < Qt.application.arguments.length)
               ? parseInt(Qt.application.arguments[i + 1]) : 0
    }

    // ⛔ THE PROCEDURE A PERSON PERFORMS, AND THE REASON A RUN WITHOUT IT PROVES
    // NOTHING.  Watching the manual test on 1 September: connect the phone's
    // camera, WAIT FOR CLOCK AGREEMENT UNDER 5 ms, and only then start the
    // capture session.  Automating the swing but not this left the host with no
    // session at all, so `armed()` was false, every arbitrated Shot was dropped,
    // and not one `capture_request` was ever sent — the rig measured a host that
    // was not listening and reported the phone's own shots as if they were
    // answers to ours.
    //
    // --no-drive skips it, for a run where a person is driving the UI.
    readonly property bool drive:
        Qt.application.arguments.indexOf("--no-drive") < 0

    // 6.1f — the arbitration gate.  Starting before the relation converges
    // arbitrates on a sigma wider than 5 ms, so Shots are excluded and the run
    // measures the warm-up.
    readonly property real syncGateMs: {
        var i = Qt.application.arguments.indexOf("--sync-gate-ms")
        return (i >= 0 && i + 1 < Qt.application.arguments.length)
               ? parseFloat(Qt.application.arguments[i + 1]) : 5.0
    }

    // Which session type to open. 1 = Wrist, matching the manual runs.
    readonly property int sessionType: {
        var i = Qt.application.arguments.indexOf("--session-type")
        return (i >= 0 && i + 1 < Qt.application.arguments.length)
               ? parseInt(Qt.application.arguments[i + 1]) : 1
    }

    // ⭐ A SWING FROM THE HOST'S SIDE, FOR A PHONE THAT CANNOT INJECT ONE.  The
    // automated device suite injects its own synthetic swing; the REAL app,
    // launched in the foreground for a cable run, has no such hook.  With
    // this set, the probe triggers one Manual host shot this many ms after the
    // session starts, so the host asks the phone for the footage its ring
    // already holds.
    // 0 = off.
    readonly property int injectShotAfterMs: {
        var i = Qt.application.arguments.indexOf("--inject-shot-after-ms")
        return (i >= 0 && i + 1 < Qt.application.arguments.length)
               ? parseInt(Qt.application.arguments[i + 1]) : 0
    }
    property int  sessionOpenedAt: -1
    property bool shotInjected:    false

    // ⭐ THE TORCH, AS A RUNG (2 Sept 2026).  The torch is a CAPTURE SETTING:
    // the probe sets "torch during capture" for the phone before the session
    // starts, and requires the light ON once the phone has armed (Studio lights
    // it on the phone's readiness) — and STILL on a few seconds later (the
    // phone used to rebuild its camera on every arm, which put it out).
    // Skipped, with a line, on a phone that declares no torch; --no-torch
    // skips it on any phone.
    readonly property bool torch:
        Qt.application.arguments.indexOf("--no-torch") < 0
    property bool torchAsked:   false
    property bool torchLit:     false
    property bool torchSkipped: false
    property int  torchAskedAt: -1
    // How long a torch command may go unanswered before the run fails.
    readonly property int torchWaitMs: 15000

    // ⭐ ARMED BY THE HOST, AS A RUNG.  Starting the session sends `arm` to
    // every phone (CameraManager::captureIntentChanged -> setCaptureWanted);
    // the phone's `readiness` answers it.  The run does not proceed to a
    // swing on the strength of a queued message.
    property bool phoneArmed:    false
    property int  phoneArmedAt:  -1
    readonly property int armWaitMs: 60000
    // Torch re-read this long after the phone reported armed.
    readonly property int torchRecheckMs: 3000
    property bool torchSurvived: false

    // ⛔ --linger: on a PASS, say so and KEEP RUNNING.  The rig's device half
    // dials this host several more times after the clip has landed; a host
    // that quit on its own green left those tests dialling a dead port and the
    // rig reporting "the DEVICE half failed" on the first run that ever
    // passed (1 Sept 2026, run eight).  The rig reads the verdict from the log
    // and kills this process when the device half is done.
    readonly property bool linger:
        Qt.application.arguments.indexOf("--linger") >= 0
    property bool passed: false

    readonly property int timeoutMs: {
        var i = Qt.application.arguments.indexOf("--probe-timeout-ms")
        return (i >= 0 && i + 1 < Qt.application.arguments.length)
               ? parseInt(Qt.application.arguments[i + 1]) : 120000
    }

    property int elapsed: 0
    property int lastSeen: -1

    // The ladder this run climbs, in order.  Each is the precondition of the
    // next, and the doctor line below names the first one that never happened.
    property bool camEnabled:    false
    property bool synced:        false
    property bool sessionOpened: false
    property real bestSigmaMs:   -1

    // The first phone's torch row, or null.  perPhone[i].actuators is what
    // actuator_command_ack / actuator_state write; `state` is "unknown" until
    // the phone has said something, which is a reading and not "off".
    function torchRow(s) {
        if (!s.ppcp.perPhone || !s.ppcp.perPhone.length) return null
        var ph = s.ppcp.perPhone[0]
        if (!ph.actuators) return null
        for (var i = 0; i < ph.actuators.length; i++)
            if (ph.actuators[i].kind === "torch") return ph.actuators[i]
        return null
    }
    function armStateOf(s) {
        if (!s.ppcp.perPhone || !s.ppcp.perPhone.length) return ""
        return s.ppcp.perPhone[0].armState
    }

    function snapshot() {
        return { "ppcp": ppcpHost.ppcpStats(), "shot": shotController.shotStats() }
    }

    // The phone's own clock agreement, or -1 while no relation exists.
    function sigmaMs(s) {
        if (!s.ppcp.perPhone || !s.ppcp.perPhone.length) return -1
        var worst = -1
        for (var i = 0; i < s.ppcp.perPhone.length; i++) {
            var v = s.ppcp.perPhone[i].syncSigmaMs
            if (v === undefined || v < 0) continue
            if (worst < 0 || v > worst) worst = v
        }
        return worst
    }

    // ⛔ WHERE DOES IT STOP? — answered in order, which is the only shape of
    // diagnostic that survives a chain this long.  Borrowed wholesale from
    // ppcp_preview_doctor.qml, because the alternative is reading four logs and
    // guessing, and that cost a full day on 1 September.
    function doctor(s) {
        if (!s.ppcp.phones)        return "no phone connected (listening=" + s.ppcp.listening
                                          + " port=" + s.ppcp.port + ")"
        if (!probe.camEnabled)     return "the phone's camera was never enabled for the session"
        if (!probe.synced)         return "clock agreement never reached " + probe.syncGateMs
                                          + " ms (best " + probe.bestSigmaMs + " ms) — 6.1f"
        if (probe.torch && !probe.torchLit && !probe.torchSkipped)
                                   return "the torch never lit from the host (asked=" + probe.torchAsked
                                          + ", " + JSON.stringify(probe.torchRow(s)) + ")"
        if (!probe.sessionOpened)  return "the capture session never started, so armed() was false"
        if (!probe.phoneArmed)     return "the phone never reported armed after the host's arm (armState="
                                          + probe.armStateOf(s) + ")"
        if (probe.torch && probe.torchLit && !probe.torchSurvived)
                                   return "the torch was not lit by capture ("
                                          + JSON.stringify(probe.torchRow(s)) + ")"
        if (!s.shot.committed)     return "no Shot was committed (corroboration "
                                          + s.shot.corroboratePass + " pass / "
                                          + s.shot.corroborateFail + " fail: "
                                          + s.shot.lastVerdict + ")"
        if (!s.ppcp.captureRequests) return "a Shot committed but NO capture_request went out — "
                                          + "no open shot_windowed Stream to ask for"
        if (!s.ppcp.clipsAnnounced)  return "asked, and the phone announced nothing"
        if (!s.ppcp.clipsConverted)  return "announced, but no clip converted (§6.1: no "
                                          + "achieved_frames, or no CaptureProfile)"
        if (!s.ppcp.clipsFiled)      return "converted, but nothing was filed into a swing"
        return "unknown"
    }

    // The manual procedure, performed in order and only once each.
    function driveOneStep(s) {
        if (!s.ppcp.phones) return

        if (!probe.camEnabled) {
            var list = cameraManager.cameraList
            for (var i = 0; i < list.length; i++) {
                if (!list[i].isPpcp) continue
                cameraManager.setSessionCameraEnabled(list[i].cameraKey, true)
                cameraManager.setSelected(list[i].index, true)
                probe.camEnabled = true
                console.warn("PROBE DRIVE camera enabled — " + list[i].description)
                break
            }
            return
        }

        if (!probe.synced) {
            var sig = probe.sigmaMs(s)
            if (sig >= 0 && (probe.bestSigmaMs < 0 || sig < probe.bestSigmaMs))
                probe.bestSigmaMs = sig
            if (sig < 0 || sig > probe.syncGateMs) return
            probe.synced = true
            console.warn("PROBE DRIVE clock agreed — " + sig + " ms (gate "
                         + probe.syncGateMs + " ms)")
            return
        }

        // The torch, from the host, BEFORE capture starts — so that arming has
        // a lit torch to put out.  The phone is warm by now: preview was asked
        // for at declare and warmed the camera.
        if (probe.torch && !probe.torchLit && !probe.torchSkipped) {
            var row = probe.torchRow(s)
            if (!row) {
                probe.torchSkipped = true
                console.warn("PROBE DRIVE torch skipped — the phone declares no torch")
                return
            }
            // The SETTING, before capture.  Nothing lights yet: the phone is
            // not armed, and a torch during capture is lit by the capture.
            ppcpHost.setPhoneTorchDuringCapture(s.ppcp.perPhone[0].pairingId, true)
            probe.torchAsked = true
            probe.torchAskedAt = probe.elapsed
            probe.torchLit = true
            console.warn("PROBE DRIVE torch during capture set — actuator " + row.id
                         + " (state now " + row.state + ")")
            return
        }

        if (!probe.sessionOpened) {
            // Exactly what Main.qml's session-start handler does, in its order.
            shotProcessor.beginSessionFolder(probe.sessionType, false)
            shotModel.loadSessionDir("")
            sessionController.start(probe.sessionType)
            cameraManager.startCapture()
            probe.sessionOpened = true
            probe.sessionOpenedAt = probe.elapsed
            console.warn("PROBE DRIVE session started — type " + probe.sessionType
                         + ", dir " + shotProcessor.activeSessionDir
                         + "; captureWanted=" + ppcpHost.captureWanted
                         + " armState=" + ppcpHost.armState)
            // ⭐ startCapture() is what arms the phones now — the session
            // screen's Capture drives them (2 Sept 2026).  No armAll() here:
            // if the join above ever breaks, this run must fail on the rung
            // below rather than paper over it.
            return
        }

        // The phone's own answer to the host's `arm`: a `readiness` that says
        // settled.  A swing before this lands in a ring that is not retaining.
        if (!probe.phoneArmed) {
            var st = probe.armStateOf(s)
            if (st === "armed") {
                probe.phoneArmed = true
                probe.phoneArmedAt = probe.elapsed
                console.warn("PROBE DRIVE phone armed by the host — "
                             + (probe.elapsed - probe.sessionOpenedAt) + " ms after capture started")
                return
            }
            if (st === "blocked" || st === "stalled")
                return probe.finish(false, "the host's arm ended " + st + " on the phone")
            if (probe.elapsed - probe.sessionOpenedAt > probe.armWaitMs)
                return probe.finish(false, "the phone never reported armed within "
                                    + probe.armWaitMs + " ms of the host's capture start (armState="
                                    + st + ", hostArmed=" + s.ppcp.perPhone[0].hostArmed + ")")
            return
        }

        // ⭐ The torch, AFTER the phone armed: Studio lights it on the phone's
        // readiness.  Read a few seconds after armed so the command, its ack,
        // and any 12.2a actuator_state from the phone's 1 Hz tick have all had
        // time to arrive — a light that went out on arm is reported on that
        // tick, and a reading taken before it would pass a run that failed.
        if (probe.torch && probe.torchLit && !probe.torchSurvived) {
            if (probe.elapsed - probe.phoneArmedAt < probe.torchRecheckMs) return
            var trow = probe.torchRow(s)
            if (trow && trow.state === "on") {
                probe.torchSurvived = true
                console.warn("PROBE DRIVE torch lit by capture — on "
                             + (probe.elapsed - probe.phoneArmedAt) + " ms after the phone armed")
                return
            }
            if (probe.elapsed - probe.phoneArmedAt < probe.torchWaitMs) return
            return probe.finish(false, "the torch was not lit by capture within "
                                + probe.torchWaitMs + " ms of the phone arming: "
                                + JSON.stringify(trow))
        }

        if (probe.injectShotAfterMs > 0 && !probe.shotInjected
                && probe.elapsed - probe.sessionOpenedAt >= probe.injectShotAfterMs) {
            probe.shotInjected = true
            // ⚠ triggerShot, NOT injectDetection: the latter is corroboration
            // EVIDENCE beside a phone's Candidate and commits nothing on its
            // own (measured 1 Sept: "committed 0 of 1").  A Manual shot is the
            // host's own button, and it commits, asks every phone, and files.
            // "Now": the phone's ring holds the last ten seconds, so the full
            // pre-roll is there to serve.
            // ⚠ reportCandidate(Acoustic), NOT triggerShot(Manual): a Manual shot
            // commits locally and asks no phone (captureRequested is emitted
            // only for an arbitrated PPCP Shot).  This is the host's own
            // microphone path: the Candidate is nominated on the wire, the
            // host arbitrates it, and the commit asks every phone.
            shotController.reportCandidate(4, shotController.nowUs(), 1.0)
            console.warn("PROBE DRIVE host acoustic candidate reported — " + probe.injectShotAfterMs
                         + " ms after the session started")
        }
    }

    function finish(ok, why) {
        if (probe.passed) return
        var s = probe.snapshot()
        console.warn("PROBE RESULT " + (ok ? "PASS" : "FAIL") + " — " + why)
        // ⛔ The ladder on every FAILURE, not only on a timeout.  A verdict that
        // says only "expected 1, got 0" sends the reader back to the logs, which
        // is the loop this probe exists to break.
        if (!ok) console.warn("PROBE DOCTOR — " + probe.doctor(s))
        console.warn("PROBE STATS " + JSON.stringify(s))
        if (ok && probe.linger) {
            probe.passed = true
            console.warn("PROBE LINGER — staying up for the device half; the rig ends this process")
            return
        }
        Qt.exit(ok ? 0 : 1)
    }

    Timer {
        interval: 250; running: true; repeat: true
        onTriggered: {
            if (probe.passed) return
            probe.elapsed += interval
            var s = probe.snapshot()

            // Connect the camera, wait for the clocks, start the session — the
            // procedure a person performs before a swing means anything.
            if (probe.drive) probe.driveOneStep(s)

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

            // ⚠ SHOTS ARE NO LONGER THE FINISH LINE WHEN CLIPS ARE ASKED FOR.
            // A committed Shot with no clip behind it is exactly the state that
            // was mistaken for success once already: `bulk 242550681` crossed
            // the wire and not one frame reached a swing.
            if (probe.expectClips > 0) {
                if (s.ppcp.clipsFiled >= probe.expectClips)
                    return probe.finish(true, "filed " + s.ppcp.clipsFiled + " clip(s) into "
                                              + s.shot.committed + " shot(s)")
            } else if (s.shot.committed >= probe.expectShots) {
                return probe.finish(true, "committed " + s.shot.committed + " shot(s)")
            }

            if (probe.elapsed >= probe.timeoutMs)
                return probe.finish(false,
                    "timed out after " + probe.timeoutMs + " ms — committed "
                    + s.shot.committed + " of " + probe.expectShots
                    + ", filed " + s.ppcp.clipsFiled + " of " + probe.expectClips
                    + " clip(s), corroboration " + s.shot.corroboratePass + " pass / "
                    + s.shot.corroborateFail + " fail, last verdict: "
                    + s.shot.lastVerdict)
        }
    }

    Component.onCompleted: console.warn("PROBE START expect-shots=" + expectShots
                                        + " expect-clips=" + expectClips
                                        + " drive=" + drive
                                        + " sync-gate=" + syncGateMs + "ms"
                                        + " corroborate=" + corroborate
                                        + " torch=" + torch
                                        + " inject-shot-after-ms=" + injectShotAfterMs
                                        + " timeout=" + timeoutMs + "ms")
}
