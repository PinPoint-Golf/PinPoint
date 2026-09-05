// plumb_bob_chart.qml — what the CHART LAYER sees for the "Plumb Bob" preset.
//
//   QT_QPA_PLATFORM=offscreen PINPOINT_LOG_STDERR=1 \
//     .../PinPointStudio.app/Contents/MacOS/PinPointStudio \
//     --probe-qml /abs/path/plumb_bob_chart.qml
//
// Verifies Phases 1 to 3 of docs/design/metric_presentation_honesty.md WITHOUT screenshots:
// Phase 1's validity mask / phase domains / suppressed samples, Phase 2's shared reducers —
// the σ on the window a card actually reduces, and the STILL ADDRESS window that design §7
// item 2 ("under 2 units per 100 ms on a golfer who has not moved") is judged on — and
// Phase 3's σ-governed display: each series' σ, the display STEP it buys, the card strings
// that come out of it (§8 open question 1 is decided by reading those), and whether the ±σ
// ribbon drew.
// Every line is prefixed "PBPROBE" so the run can be grepped out of the app log.
//
// ── WHAT IT DOES, AND WHY IN THAT ORDER ──────────────────────────────────────────────────
//   1. sessionReviewController.loadSession(<session dir>)   — enters Loaded-session review.
//      A session's id IS its absolute directory path (session_review_controller.h:63), so no
//      library-root setting has to be right for this to find the swing.
//   2. navController.navigate(sessionType + 1)              — the session screen (Wrist = 2).
//   3. SessionMode.enterReplay(shotId, swingDir, false)     — promotes the swing onto the stage,
//      which is what makes shotReplay.analysisDetail the focused swing's.
//   4. Feeds that analysisDetail into a PRIVATE PpMetricChart instance and calls its own
//      _applyPreset("Plumb Bob") — see the note on that below.
//   5. Reports, per series, everything the chart layer decorates and derives — including the
//      Phase 3 display step and the exact strings the summary card prints.
//   6. Reports whether the ±σ ribbon drew, by asking the plots, not by asserting the switch.
//   7. Reports what the traces DRAW (Phase 6: the windowed mean) and whether the raw samples are
//      still on screen behind them — again by asking the plots, not the switch.
//   8. Walks the live tree read-only to cross-check the on-screen chart.
//
// ⚠ WHY A PRIVATE PpMetricChart AND NOT THE ONE ON SCREEN. The on-screen chart is a lazily
// created panel delegate whose existence depends on the user's persisted View layout
// (ViewLayout.isPanelOn(mode,"charts") — PpModeStage.qml:59), and forcing it on would WRITE a
// persisted user setting from a diagnostic. A private instance runs the SAME PpMetricChart.qml
// code — _plottable, _domainWindow, _applyPreset, _measuredAt — on the same seriesList, and with
// sessionType:-1 it is contractually forbidden from persisting anything (_persistPref /
// _persistSection both return early). The probe ALSO walks the live tree read-only and reports
// whether an on-screen chart exists and what preset it is sitting on, so the two can be compared.
//
// ⚠ TIMEBASE. Both analysisDetail sources are window-relative (t0-subtracted): swing_doc.cpp's
// serializeAnalysis writes rel(t), and disk_replay_source.cpp's relUs() is idempotent over
// already-relative values. So the fallback source below is directly comparable to the replay one.

import QtQml
import QtQuick
import PinPointStudio

Item {
    id: probe
    anchors.fill: parent
    // ⚠ NOT visible:false, and not by accident — an OCCLUDED/hidden window starves the QML
    // animation driver and with it every Timer, and a probe that "hangs" is then indistinguishable
    // from a broken one. A bare Item draws nothing, so leaving it visible costs nothing.

    // ── Arguments ────────────────────────────────────────────────────────────────────────
    function _arg(name, dflt) {
        var i = Qt.application.arguments.indexOf(name)
        return (i >= 0 && i + 1 < Qt.application.arguments.length)
               ? Qt.application.arguments[i + 1] : dflt
    }

    readonly property string swingDir: probe._arg("--probe-swing",
        "/mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_01/swing_0001")
    // The session dir is the swing dir's parent — loadSession() takes it verbatim.
    readonly property string sessionDir: probe.swingDir.substring(0, probe.swingDir.lastIndexOf("/"))
    readonly property string presetName: probe._arg("--probe-preset", "Plumb Bob")
    // Wrist = 1 (screen index 2). Only affects which screen is navigated to.
    readonly property int    sessionType: parseInt(probe._arg("--probe-session-type", "1"))
    readonly property int    stepMs:      parseInt(probe._arg("--probe-step-ms", "2500"))

    // The series this run reports on. The three "Plumb Bob" members plus shoulderPlaneAngle,
    // which is NOT in the preset (it is one segment up, "Address → Impact" by the same argument)
    // and is reported anyway because the design table pairs them.
    readonly property var reportKeys: ["pelvisSway", "hipLineTilt", "plumbBobDistance",
                                       "shoulderPlaneAngle"]

    // ── Logging ──────────────────────────────────────────────────────────────────────────
    function w(s)  { console.warn("PBPROBE " + s) }
    function miss(what) { probe.w("MISSING: " + what) }
    function num(v, dp) {
        if (v === undefined || v === null) return "?"
        var n = Number(v)
        return isFinite(n) ? n.toFixed(dp === undefined ? 3 : dp) : String(v)
    }
    function ms(us) {
        if (us === undefined || us === null) return "?"
        var n = Number(us)
        return isFinite(n) ? (n / 1000).toFixed(1) + "ms" : String(us)
    }
    // Phase int → short tag, via the real TimelineLabels; falls back to the raw int.
    function ptag(p) {
        try {
            var t = labels.phaseShortTag(p)
            return (t && t !== "") ? (t + "(" + p + ")") : String(p)
        } catch (e) { return String(p) }
    }

    // ── The maths, reused rather than reimplemented ───────────────────────────────────────
    ChartMetrics   { id: cm }
    TimelineLabels { id: labels }

    // ── Data in hand ─────────────────────────────────────────────────────────────────────
    property var    detail:      ({})
    property string detailFrom:  ""
    property var    series:      []
    property var    phases:      []
    property real   axStartUs:   0
    property real   axEndUs:     0
    property real   impactUs:    0
    property int    shotId:      -1

    // ── The chart under test. Real PpMetricChart.qml, fed the same props PpReplayCharts feeds
    //    it (PpReplayCharts.qml:44-58), with sessionType:-1 so it can persist nothing. ────────
    PpMetricChart {
        id: chart
        width: 1200; height: 800
        visible: false
        sessionType: -1                  // ⚠ load-bearing: no writes to appSettings
        seriesList: probe.series
        phases:     probe.phases
        startUs:    probe.axStartUs
        endUs:      probe.axEndUs
        impactUs:   probe.impactUs
        playheadUs: probe.impactUs
        showPlayhead: false
        seekable:   false
        // ⚠ THE ONE THING IN THE TREE THAT TURNS THE ±σ RIBBON ON. Design §8 keeps it dark until
        // it has been looked at, so there is no control for it anywhere in the app and this probe
        // is the switch — see PpChartPlot.showSigmaBand. The report below says how many ribbon
        // polygons each plot actually drew, which is the question "did it draw" answered by the
        // drawing code rather than by this line being here.
        showSigmaBand: true
    }

    // Harvests (shotId, swingDir) out of the review model — a QAbstractListModel a probe
    // cannot index directly. Instantiator takes a QtObject delegate, so this costs no Items.
    Instantiator {
        id: rows
        model: {
            try { return sessionReviewController.activeShots } catch (e) { return null }
        }
        // Plain (not `required`) properties reading the model roles: a required property that a
        // model does not supply is a hard error at delegate creation, and this delegate exists
        // only to answer "which id is this swingDir", not to assert the model's shape.
        delegate: QtObject {
            property int    sid:  (model && model.shotId   !== undefined) ? model.shotId   : -1
            property string sdir: (model && model.swingDir !== undefined) ? model.swingDir : ""
        }
    }

    // ── Reporting ────────────────────────────────────────────────────────────────────────
    function reportEnvironment() {
        probe.w("═══ ENVIRONMENT ═══")
        try { probe.w("devBuild            = " + appInfo.devBuild) } catch (e) { probe.miss("appInfo") }
        try { probe.w("athleteLibraryPath  = '" + appSettings.athleteLibraryPath + "'"
                      + "   (QSettings General/athleteLibraryPath — NOT used to find the swing;"
                      + " loadSession takes the absolute dir)") }
        catch (e) { probe.miss("appSettings.athleteLibraryPath") }
        try { probe.w("athlete             = '" + athleteController.currentName + "'") }
        catch (e) { probe.miss("athleteController.currentName") }
        try { probe.w("replayTrimToSwing   = " + appSettings.replayTrimToSwing) } catch (e) {}
        probe.w("sessionDir          = " + probe.sessionDir)
        probe.w("swingDir            = " + probe.swingDir)
        probe.w("preset asked for    = '" + probe.presetName + "'")
    }

    function stepLoadSession() {
        probe.w("═══ 1. LOAD SESSION ═══")
        if (typeof sessionReviewController === "undefined") return probe.miss("sessionReviewController")
        sessionReviewController.loadSession(probe.sessionDir)
        probe.w("reviewActive        = " + sessionReviewController.reviewActive
                + "   activeSessionId = " + sessionReviewController.activeSessionId
                + "   shots = " + sessionReviewController.activeShotCount)
        if (typeof navController === "undefined") return probe.miss("navController")
        navController.navigate(probe.sessionType + 1)
        probe.w("navController.currentIndex = " + navController.currentIndex
                + " (session screen for sessionType " + probe.sessionType + ")")
    }

    function stepFocusSwing() {
        probe.w("═══ 2. FOCUS THE SWING ═══")
        // The real shot id, so the carousel and SessionMode agree with the stage.
        probe.shotId = -1
        try {
            for (var i = 0; i < rows.count; ++i) {
                var o = rows.objectAt(i)
                if (o && o.sdir === probe.swingDir) { probe.shotId = o.sid; break }
            }
        } catch (e) { probe.miss("sessionReviewController.activeShots rows (" + e + ")") }
        if (probe.shotId < 0) {
            probe.w("NOTE: no row in the review model matches that swingDir "
                    + "(rows=" + rows.count + ") — using shotId 0. "
                    + "ShotReplayController::start does not validate the id.")
            probe.shotId = 0
        }
        probe.w("shotId              = " + probe.shotId)
        if (typeof SessionMode === "undefined") return probe.miss("SessionMode singleton")
        SessionMode.enterReplay(probe.shotId, probe.swingDir, false)
        try {
            probe.w("SessionMode.mode    = " + SessionMode.mode + " (1 = replay)"
                    + "   focusedSwingDir = " + SessionMode.focusedSwingDir)
        } catch (e) {}
    }

    function stepCollectDetail() {
        probe.w("═══ 3. COLLECT analysisDetail ═══")
        var d = null
        try {
            probe.w("shotReplay.active   = " + shotReplay.active
                    + "   streams = " + shotReplay.streamCount
                    + "   swingDir = " + shotReplay.swingDir)
            probe.w("shotReplay span     = [" + probe.ms(shotReplay.startUs) + ", "
                    + probe.ms(shotReplay.endUs) + "]  impact = " + probe.ms(shotReplay.impactUs))
            d = shotReplay.analysisDetail
            if (d && d.series && d.series.length > 0) {
                probe.detail     = d
                probe.detailFrom = "shotReplay.analysisDetail (disk_replay_source)"
                probe.axStartUs  = shotReplay.startUs
                probe.axEndUs    = shotReplay.endUs
                probe.impactUs   = shotReplay.impactUs
            }
        } catch (e) { probe.miss("shotReplay (" + e + ")") }

        // ⚠ FALLBACK, AND WHY IT IS NOT A CHEAT. DiskReplaySource::load() refuses a swing with
        // no PLAYABLE video stream and InvalidMedia tears the source down — offscreen, with no
        // decoder, analysisDetail can therefore be empty for reasons that have nothing to do
        // with the chart. ShotListModel::analysisDetailForSwingDir reads the same swing.json
        // through SwingDocReader and yields the same window-relative shape (shot_list_model.cpp
        // :392), so the chart layer can still be exercised. The line below SAYS which was used.
        if (!probe.detail || !probe.detail.series || probe.detail.series.length === 0) {
            probe.w("shotReplay gave no series — falling back to the document reader")
            try {
                d = sessionReviewController.activeShots.analysisDetailForSwingDir(probe.swingDir)
            } catch (e) {
                probe.miss("activeShots.analysisDetailForSwingDir (" + e + ")")
                try { d = shotModel.analysisDetailForSwingDir(probe.swingDir) }
                catch (e2) { probe.miss("shotModel.analysisDetailForSwingDir (" + e2 + ")") }
            }
            if (d && d.series && d.series.length > 0) {
                probe.detail     = d
                probe.detailFrom = "ShotListModel.analysisDetailForSwingDir (SwingDocReader)"
                // No replay span available — let PpMetricChart fall back to the data extent
                // (_axisStart/_axisEnd with startUs == 0). Say so, because the axis IS the
                // padded swing in the real app and the Full window differs by the pad.
                probe.axStartUs = 0
                probe.axEndUs   = 0
                probe.impactUs  = probe.phaseUsIn(d, 5)
                probe.w("NOTE: no replay span — axis falls back to the DATA extent, not the "
                        + "padded swing window. impactUs taken from phases[phase==5].")
            }
        }

        if (!probe.detail || !probe.detail.series || probe.detail.series.length === 0)
            return probe.miss("analysisDetail.series (both sources empty) — cannot report")

        probe.series = probe.detail.series
        probe.phases = (probe.detail.phases !== undefined) ? probe.detail.phases : []
        probe.w("detail source       = " + probe.detailFrom)
        probe.w("tier / overall      = " + probe.detail.tier + " / "
                + JSON.stringify(probe.detail.overall))
        probe.w("series count        = " + probe.series.length
                + "   phases = " + probe.phases.length)
        var pl = []
        for (var i = 0; i < probe.phases.length; ++i)
            pl.push(probe.ptag(probe.phases[i].phase) + "@" + probe.ms(probe.phases[i].t_us))
        probe.w("phase ladder        = " + pl.join("  "))
        probe.w("chart axis          = [" + probe.ms(chart._axisStart) + ", "
                + probe.ms(chart._axisEnd) + "]   impactUs = " + probe.ms(probe.impactUs))
    }

    // The instant of one phase in the ladder this run collected, or 0 when the swing has none.
    // Same lookup as phaseUsIn() below, against probe.phases rather than a detail document, so a
    // report function does not have to know which of the two detail sources was used.
    function phaseUs(phase) {
        var ps = probe.phases || []
        for (var i = 0; i < ps.length; ++i) if (ps[i].phase === phase) return ps[i].t_us
        return 0
    }

    function phaseUsIn(d, phase) {
        try {
            var ps = d.phases || []
            for (var i = 0; i < ps.length; ++i) if (ps[i].phase === phase) return ps[i].t_us
        } catch (e) {}
        return 0
    }

    function stepApplyPreset() {
        probe.w("═══ 4. SELECT THE PRESET ═══")
        // ChartMetrics.seriesGroups — the combo's own list, from the C++ catalogue.
        var groups = []
        try { groups = cm.seriesGroups(probe.series) } catch (e) { probe.miss("cm.seriesGroups (" + e + ")") }
        probe.w("ChartMetrics.seriesGroups — " + groups.length + " entr" + (groups.length === 1 ? "y" : "ies")
                + " (groups first, then cross-cutting presets, then 'Other'):")
        for (var i = 0; i < groups.length; ++i) {
            var g = groups[i]
            probe.w("   [" + i + "] '" + g.group + "'  (" + g.keys.length + ") "
                    + g.keys.join(", "))
        }
        try {
            probe.w("chart._groups count = " + chart._groups.length
                    + "   _presetOptions = " + JSON.stringify(chart._presetOptions))
        } catch (e) { probe.miss("chart._groups / _presetOptions (" + e + ")") }

        // Is the preset even offered? seriesGroups omits a preset with fewer than two
        // plottable members (chart_metrics.cpp:514) — that absence is a finding, not an error.
        var offered = false
        for (var j = 0; j < groups.length; ++j) if (groups[j].group === probe.presetName) offered = true
        if (!offered)
            probe.w("⛔ preset '" + probe.presetName + "' is NOT OFFERED for this swing "
                    + "(fewer than two of its members are plottable, or none are)")

        try {
            chart._applyPreset(probe.presetName, false)     // false = do not persist
            probe.w("chart.preset        = '" + chart.preset + "'"
                    + "   _presetBase = '" + chart._presetBase + "'"
                    + "   _presetTitle = '" + chart._presetTitle + "'")
            probe.w("chart.enabledKeys   = " + JSON.stringify(chart.enabledKeys))
            var vis = []
            for (var k = 0; k < chart._visible.length; ++k) vis.push(chart._visible[k].key)
            probe.w("chart._visible      = " + vis.join(", ") + "   (" + vis.length + " facet(s))")
            var leg = []
            for (var m = 0; m < chart._legendSeries.length; ++m) leg.push(chart._legendSeries[m].key)
            probe.w("chart._legendSeries = " + leg.join(", "))
            probe.w("chart view window   = [" + probe.ms(chart.viewStartUs) + ", "
                    + probe.ms(chart.viewEndUs) + "]  segment = '" + chart._preset + "'")
        } catch (e) { probe.miss("chart._applyPreset / preset state (" + e + ")") }
    }

    // The series object the CHART decorated (validFromUs/validToUs added in _plottable),
    // or null when this swing cannot draw that key.
    function plottableFor(key) {
        try {
            for (var i = 0; i < chart._plottable.length; ++i)
                if (chart._plottable[i].key === key) return chart._plottable[i]
        } catch (e) { probe.miss("chart._plottable (" + e + ")") }
        return null
    }
    function rawFor(key) {
        for (var i = 0; i < probe.series.length; ++i)
            if (probe.series[i] && probe.series[i].key === key) return probe.series[i]
        return null
    }

    function reportSeries(key) {
        probe.w("")
        probe.w("── " + key + " ──────────────────────────────────────────────")
        var raw = probe.rawFor(key)
        if (!raw) return probe.miss("series '" + key + "' is not in analysisDetail.series at all")

        var t = raw.t_us || [], v = raw.value || []
        probe.w("label / unit        = '" + raw.label + "' / '" + raw.unit + "'"
                + "   shortLabel = '" + (function () { try { return cm.shortLabel(key) } catch (e) { return "?" } })()
                + "'   shortUnit = '" + (function () { try { return cm.shortUnit(raw.unit) } catch (e) { return "?" } })() + "'")
        probe.w("samples             = " + t.length + " t_us, " + v.length + " value"
                + (t.length === v.length ? "" : "   ⛔ LENGTH MISMATCH"))
        probe.w("first / last t_us   = " + probe.ms(t.length ? t[0] : null) + " / "
                + probe.ms(t.length ? t[t.length - 1] : null))
        probe.w("sigma               = " + (raw.sigma !== undefined
                ? probe.num(raw.sigma) + "  (§5.3 / DoD 4)"
                : "ABSENT — producer did not propagate an error budget"))

        // ── §5.3: WHAT σ DOES TO THE DIGITS ───────────────────────────────────────────────
        //
        // The display quantum every printed reading of this series is rounded to. `sg` is the
        // FORMATTING substitution the QML makes (PpChartSummary._sigma): absent → 0, which asks for
        // no coarsening and gives back exactly the pre-σ whole-unit rounding. Absent is NOT zero in
        // the data and nothing here pretends otherwise; this line reports both faces of it.
        //
        // A step of 1 on a series that HAS a σ is the answer to design §8 open question 1 being
        // moot for that metric; a step of 5 or more is the case the question is about, and the
        // formatted strings a few lines down are what Mark is being asked to look at.
        // ⚠ THE PROBE KEEPS ITS OWN COPY OF THE absent→0 RULE, on purpose and against the general
        // "one implementation" grain. The app's copies were collapsed into ChartMetrics.seriesSigma;
        // a probe that then asked seriesSigma what σ is could never catch seriesSigma being wrong.
        // So this reads the document directly and the next line CROSS-CHECKS the C++ against it.
        var sg = (raw.sigma !== undefined && raw.sigma !== null && isFinite(raw.sigma))
                 ? Number(raw.sigma) : 0
        var stepv = 1
        try { stepv = cm.displayStep(sg, raw.unit) } catch (e) { probe.miss("cm.displayStep (" + e + ")") }
        probe.w("displayStep         = " + probe.num(stepv, 2) + " " + cm.shortUnit(raw.unit)
                + "   (σ used for display = " + (raw.sigma === undefined ? "0 [ABSENT]" : probe.num(sg))
                + (stepv > 1 ? "  ⇒ readings COARSENED to multiples of " + probe.num(stepv, 0)
                             : "  ⇒ whole units, i.e. unchanged from before §5.3") + ")")
        // ── BOTH ARITIES REACH C++ FROM QML, CHECKED ON EVERY SERIES ──────────────────────
        //
        // formatBare/formatValue are ONE method each with a defaulted third argument, so QML calls
        // the moc CLONE at two arguments and the real method at three (chart_metrics.h explains why a
        // clone is the safe mechanism and an overload set is not). Both are exercised here rather
        // than assumed, and the 3-argument line is an ASSERTION, not a print: only a correct
        // resolution turns (12.6, 2.5) into "+15" — a call that silently dropped the σ would say
        // "+13", and one that failed to resolve would throw into the catch below. It runs
        // UNCONDITIONALLY, on every series, including the ones whose own σ is absent, because the
        // question is about the call path and not about this swing.
        try {
            var got3 = cm.formatBare(12.6, raw.unit, 2.5)
            var deg3 = cm.shortUnit(raw.unit) === "°"
            probe.w("   arity check         = formatBare(12.6, unit, 2.5) ⇒ '" + got3 + "'  "
                    + (got3 === (deg3 ? "+15" : "15") ? "PASS (3-arg resolves, step 5 applied)"
                       : "⛔ FAIL — expected '" + (deg3 ? "+15" : "15") + "'; the σ argument is "
                         + "not reaching C++")
                    + "   2-arg: formatBare(12.6, unit) ⇒ '" + cm.formatBare(12.6, raw.unit)
                    + "'   formatValue(12.6, unit) ⇒ '" + cm.formatValue(12.6, raw.unit) + "'")
            // And the one helper the app now shares — read against the probe's own copy above, so a
            // disagreement about what absence means shows up as a mismatch rather than as agreement
            // by construction.
            var cppSg = cm.seriesSigma(raw)
            probe.w("   cm.seriesSigma      = " + probe.num(cppSg)
                    + (Math.abs(cppSg - sg) < 1e-12 ? "   (agrees with the document)"
                       : "   ⛔ DISAGREES with the document's σ = " + probe.num(sg)))
        } catch (e) { probe.miss("formatBare arity / seriesSigma from QML (" + e + ")") }

        // ── valid[] ───────────────────────────────────────────────────────────────────────
        // ABSENT is the C4 contract: "every sample valid", which is what every series written
        // before the field existed carries, AND what a series with nothing to mark still
        // carries (swing_doc.cpp only emits it when at least one sample is 0).
        if (raw.valid === undefined) {
            probe.w("valid[]             = ABSENT  ⇒ every sample valid (C4). "
                    + "Nothing was gated on this series, so §5.1's mask cannot fire here.")
        } else {
            var zeros = 0, runs = 0, prev = 1
            for (var i = 0; i < raw.valid.length; ++i) {
                var f = raw.valid[i]
                if (f === 0) { zeros++; if (prev !== 0) runs++ }
                prev = f
            }
            probe.w("valid[]             = present, len " + raw.valid.length
                    + " (curve " + t.length + ")"
                    + "   zeros = " + zeros + " in " + runs + " run(s)"
                    + (raw.valid.length < t.length
                       ? "   ⛔ SHORT MASK — discarded wholesale by the short-mask rule"
                       : ""))
            // Where the invalid runs are, in phase terms — the useful half.
            var edges = []
            prev = 1
            for (var j = 0; j < raw.valid.length && edges.length < 24; ++j) {
                if (raw.valid[j] === 0 && prev !== 0) edges.push("[" + probe.ms(t[j]))
                if (raw.valid[j] !== 0 && prev === 0) edges[edges.length - 1] += ".." + probe.ms(t[j - 1]) + "]"
                prev = raw.valid[j]
            }
            if (edges.length && edges[edges.length - 1].indexOf("..") < 0)
                edges[edges.length - 1] += ".." + probe.ms(t[t.length - 1]) + "]"
            // For a NARROWED metric the first and last of these are expected to be the
            // out-of-domain regions themselves (see §5.1 domain marking below) — a leading run
            // from the series' start and a trailing one to its end are the mask doing its job, not
            // a gate misfiring. The runs in BETWEEN are the geometric gates.
            if (edges.length) probe.w("invalid runs        = " + edges.join(" "))
        }

        // ── The domain, per side ─────────────────────────────────────────────────────────
        var dom = null
        try { dom = cm.domainFor(key) } catch (e) { probe.miss("cm.domainFor('" + key + "') (" + e + ")") }
        if (dom) {
            probe.w("cm.domainFor        = first " + probe.ptag(dom.firstPhase)
                    + " (narrowed " + dom.firstNarrowed + "), last " + probe.ptag(dom.lastPhase)
                    + " (narrowed " + dom.lastNarrowed + "), narrowed " + dom.narrowed)
        }

        var p = probe.plottableFor(key)
        if (!p) {
            probe.w("chart._plottable    = ABSENT — not a drawable curve for this swing "
                    + "(needs >1 sample and value.length === t_us.length), so it has no "
                    + "validFromUs/validToUs and no facet.")
            return
        }
        probe.w("W3 validFromUs      = " + probe.ms(p.validFromUs)
                + "   (series first t_us " + probe.ms(t[0]) + ")"
                + (dom && dom.firstNarrowed ? "  ← clipped to " + probe.ptag(dom.firstPhase)
                                            : "  ← NOT clipped (side not narrowed)"))
        probe.w("W3 validToUs        = " + probe.ms(p.validToUs)
                + "   (series last t_us " + probe.ms(t[t.length - 1]) + ")"
                + (dom && dom.lastNarrowed ? "  ← clipped to " + probe.ptag(dom.lastPhase)
                                           : "  ← NOT clipped (side not narrowed)"))
        var outTail = 0, outHead = 0
        for (var k = 0; k < t.length; ++k) {
            if (t[k] < p.validFromUs) outHead++
            else if (t[k] > p.validToUs) outTail++
        }
        probe.w("samples out of dom  = " + outHead + " before, " + outTail + " after"
                + "   (" + (t.length - outHead - outTail) + " inside)")

        // ── THE PRODUCER INVARIANT for a narrowed metric ─────────────────────────────────
        //
        // Phase 2 closes the out-of-domain leak at the SOURCE rather than in the reducers: for the
        // ten metrics the manifest narrows, every sample outside the domain is written with
        // valid = 0, so the shared reducers exclude it for the same reason they exclude any other
        // bridged sample and no reducer needs a second notion of "outside". (The alternative,
        // clamping the extremum's support to the caller's window, was reverted — a span-cached
        // engine cannot agree with a clamped card.)
        //
        // So this is the line that says whether the producer did it. Two counts, and they mean
        // different things: an out-of-domain sample still marked VALID is a §5.1 violation for this
        // swing; an IN-domain sample marked invalid is not — that is a geometric gate firing where
        // it should (a foreshortened hip line), which §5.1 also asks for.
        //
        // ⚠ THE BOUNDARY MUST BE SNAPPED THE SAME WAY THE CHART SNAPS IT. validFromUs/validToUs are
        // the phase instant moved to the NEAREST sample (PpMetricChart._nearestSampleUs, ties to
        // the earlier frame), which is the same nearestIndex the phase samples use. A producer that
        // marked by the raw phase instant instead can leave the boundary sample invalid while the
        // card's window still starts on it — and then EVERY narrowed card on EVERY swing wears a
        // PARTIAL chip, because an invalid sample lies inside the window. The count below is 0/0
        // when the two conventions agree.
        if (dom && (dom.firstNarrowed || dom.lastNarrowed)) {
            var haveMask = raw.valid !== undefined && raw.valid.length >= t.length
            if (!haveMask) {
                probe.w("§5.1 domain marking = ⛔ NO USABLE MASK on a NARROWED metric — the "
                        + "producer has not marked the out-of-domain samples invalid (or the mask "
                        + "is short and discarded). Every reducer will read them.")
            } else {
                var outStillValid = 0, inMarked = 0, edgeMarked = 0
                for (var q = 0; q < t.length && q < raw.valid.length; ++q) {
                    var inDom = (t[q] >= p.validFromUs && t[q] <= p.validToUs)
                    if (!inDom && raw.valid[q] !== 0) outStillValid++
                    if (inDom && raw.valid[q] === 0) inMarked++
                    if ((t[q] === p.validFromUs || t[q] === p.validToUs) && raw.valid[q] === 0)
                        edgeMarked++
                }
                probe.w("§5.1 domain marking = out-of-domain still valid: " + outStillValid
                        + (outStillValid ? "  ⛔ §5.1 VIOLATION (these reach every reducer)" : "  ✓")
                        + "   |  in-domain marked invalid: " + inMarked
                        + " (a geometric gate, not a fault)"
                        + (edgeMarked ? "   ⛔ A DOMAIN-EDGE SAMPLE IS MARKED INVALID — every "
                                        + "narrowed card will wear a PARTIAL chip" : ""))
            }
        }

        // ── min / max inside vs outside the domain ───────────────────────────────────────
        function extremes(inside) {
            var lo = Infinity, hi = -Infinity, n = 0
            for (var i2 = 0; i2 < t.length && i2 < v.length; ++i2) {
                var within = (t[i2] >= p.validFromUs && t[i2] <= p.validToUs)
                if (within !== inside) continue
                n++
                if (v[i2] < lo) lo = v[i2]
                if (v[i2] > hi) hi = v[i2]
            }
            return n === 0 ? "none" : (probe.num(lo) + " … " + probe.num(hi) + "  (n=" + n + ")")
        }
        probe.w("value[] IN domain   = " + extremes(true))
        probe.w("value[] OUT domain  = " + extremes(false)
                + "   ← §5.1: this must contribute nothing to a card")

        // ── summaryMasked, unclamped vs domain-clamped ───────────────────────────────────
        // ⚠ THE COMPOSED MASK, WHICH IS WHAT THE APP REDUCES THROUGH (F4): PpMetricChart._plottable
        // folds `valid` AND the phase domain into one array (`reduceValid`) and hands the same one to
        // every reduction on the panel, so a probe reading `raw.valid` here would be reporting numbers
        // no surface in the app computes — and would report a false ⛔ on the ONE CURVE check below
        // for any swing whose producer never marked its out-of-domain samples. Falls back to
        // `raw.valid` if the decoration is ever absent.
        var mask = p.reduceValid || raw.valid || []
        var ws = chart.viewStartUs, we = chart.viewEndUs
        function sm(a, b) {
            try { return cm.summaryMasked(t, v, mask, Math.round(a), Math.round(b)) }
            catch (e) { probe.miss("cm.summaryMasked (" + e + ")"); return null }
        }
        // `rate` is SIGNED since Phase 2 and may be ABSENT — printed raw here, sign and all,
        // because the whole job of this probe is to show what the reducers said before the card
        // decides how to display it (the card prints the magnitude, and "—" when rateOk is false).
        function rateStr(s) {
            return s.rateOk === false ? "ABSENT (no ≥50ms window with ≥3 valid samples)"
                                      : probe.num(s.rate) + "@" + probe.ms(s.tRateUs)
        }
        function line(tag, s, a, b, withSigma) {
            if (!s) return
            probe.w(tag + " [" + probe.ms(a) + ".." + probe.ms(b) + "]"
                    + "  peak=" + probe.num(s.peak) + "@" + probe.ms(s.tPeakUs)
                    + "  rate=" + rateStr(s)
                    + "  delta=" + probe.num(s.delta)
                    + "  min/max=" + probe.num(s.min) + "/" + probe.num(s.max)
                    + "  start/end=" + probe.num(s.start) + "/" + probe.num(s.end)
                    + "  partial=" + s.partial
                    + (withSigma ? "  peakSigma=" + probe.num(s.peakSigma)
                                   + "  rateSigma=" + probe.num(s.rateSigma) : ""))
        }
        // ⚠ THE FULL ROW IS EXPECTED TO SAY partial=true ON A NARROWED METRIC once the producer
        // marks the out-of-domain samples invalid: the unclamped view window really does contain
        // samples that were not measured for THIS metric. It is the CLAMPED row below that the
        // cards render, and that one must stay partial=false unless a geometric gate fired inside
        // the domain.
        line("summary FULL       ", sm(ws, we), ws, we, false)
        // The clamp the cards actually apply (PpMetricChart._domWinStart/_domWinEnd).
        //
        // ⚠ THE σ GO ON THIS ROW, not on FULL: this is the window a CARD reduces, so peakSigma and
        // rateSigma here are the two numbers design §5.3 puts a "±" in front of on the panel, and
        // §7 item 3 ("every PEAK tile is a value on the drawn curve within σ") is judged against
        // exactly these. Reading them off the unclamped window would be judging a tile nobody sees.
        var cs = Math.max(ws, p.validFromUs !== undefined ? p.validFromUs : ws)
        var ce = Math.max(cs, Math.min(we, p.validToUs !== undefined ? p.validToUs : we))
        var csum = sm(cs, ce)
        line("summary CLAMPED    ", csum, cs, ce, true)

        // ── §5.3 / §8 OPEN QUESTION 1: THE CARD'S OWN STRINGS ─────────────────────────────
        //
        // Exactly what PpChartSummary renders for this series on the clamped window, produced by
        // the same C++ the card calls, so the step rule can be JUDGED on real numbers instead of
        // described. Design §8 leaves one decision open — "the step rule could feel coarse on the
        // degrees scale (σ = 2.5° → 5° steps); the alternative is whole units plus the ± chip" — and
        // these three strings are the evidence for it. Read them next to the raw `peak=` / `rate=`
        // on the CLAMPED line above: that is the difference the rule makes.
        //
        // The ± come from peakSigma / rateSigma (the NOISE of each reduction), not from the series σ,
        // which has its own chip beside the unit — and PK RATE is deliberately NOT step-quantised,
        // because its unit is per 100 ms and the series σ is not.
        if (csum) {
            var impMeasured = false
            try { impMeasured = chart._measuredAt(p, probe.impactUs) } catch (e) {}
            var impV = 0
            try { impV = labels.valueAtNearest(t, v, Math.round(probe.impactUs)) } catch (e) {}
            // Its own try: a throw in here must not cost the rest of THIS series' report, and the
            // step-level safe() wrapper is one level too coarse for that.
            try {
                probe.w("§5.3 CARD TEXT      "
                    + "@IMPACT '" + (impMeasured
                            ? cm.formatBare(impV, raw.unit, sg) : "—") + "'"
                    + "   PEAK '" + (csum.edgeOk === false ? "—"
                            : cm.formatBare(csum.peak, raw.unit, sg) + "  "
                              + cm.formatUncertainty(csum.peakSigma)) + "'"
                    + "   PK RATE '" + (csum.rateOk === true
                            ? Math.round(Math.abs(csum.rate)) + " " + cm.shortUnit(raw.unit) + "/100ms  "
                              + cm.formatUncertainty(csum.rateSigma)
                            : "—") + "'"
                    + "   Δ SEGMENT '" + (csum.edgeOk === false ? "—"
                            : cm.formatBare(csum.delta, raw.unit, sg)) + "'"
                    + "   σ CHIP '" + (sg > 0 ? cm.formatUncertainty(sg, cm.shortUnit(raw.unit))
                                              : "(hidden — no σ)") + "'")
                // The same reading with and without the rule, side by side — the one line that
                // answers "how much did the step actually change?" without arithmetic in the
                // reader's head. The "before" column is the genuine TWO-ARGUMENT call, so this line
                // also exercises the moc-cloned 2-arg entry from QML (see chart_metrics.h): if that
                // assumption were wrong, this throws and says so instead of failing silently in the
                // app's own bindings.
                // ── §7 ITEM 3: IS THE PEAK TILE A VALUE ON THE DRAWN CURVE? ───────────────
                //
                // PEAK is the extremum of a 40 ms windowed MEAN (§5.2), so it is NOT a sample — and
                // the design's item 3 asks that it still be a value on the curve WITHIN σ. This
                // prints the persisted value[] at the sample nearest tPeakUs beside the reduced peak
                // and the string the tile shows, which is the only way to see the three drift apart:
                // a reduced peak far from the drawn value means the window is averaging across a
                // feature, and a tile far from the reduced peak means the step is doing too much.
                var pi = -1, pbd = Infinity
                for (var q2 = 0; q2 < t.length; ++q2) {
                    var d2 = Math.abs(t[q2] - csum.tPeakUs)
                    if (d2 < pbd) { pbd = d2; pi = q2 }
                }
                if (pi >= 0)
                    probe.w("   PEAK on the curve   = value[" + pi + "] @" + probe.ms(t[pi])
                            + " = " + probe.num(v[pi])
                            + "   reduced peak = " + probe.num(csum.peak)
                            + "   |diff| = " + probe.num(Math.abs(v[pi] - csum.peak))
                            + (sg > 0 ? "  (σ = " + probe.num(sg) + " ⇒ "
                                        + (Math.abs(v[pi] - csum.peak) <= sg
                                           ? "WITHIN σ, §7 item 3 holds" : "OUTSIDE σ") + ")"
                                      : "  (no σ — §7 item 3 not judgeable on this series)")
                            + "   tile = '" + cm.formatBare(csum.peak, raw.unit, sg) + "'"
                            + "   nearest sample " + probe.ms(pbd) + " from tPeak")

                // ── PHASE 6 / C17: IS THE TILE A POINT ON THE LINE THE CHART DRAWS? ───────
                //
                // The line above compares PEAK against the persisted SAMPLE at tPeak, which after
                // Phase 6 is no longer what the chart strokes: the trace draws ChartMetrics
                // .windowedMean, the same per-sample 40 ms centred mean reduceExtremum ranks. So the
                // interesting comparison is now a three-way one — mean[pi] (what is drawn),
                // value[pi] (what was recorded, drawn as a faint dot behind it) and csum.peak (what
                // the tile says) — and the first and third must be the SAME DOUBLE, not merely close.
                //
                // Printed here AS WELL AS asserted in chart_metrics_test, because a fixture cannot
                // show what the reduction is worth on a real curve: the mean-vs-raw gap on
                // hipLineTilt at its peak IS the wobble the design is arguing about, in the metric's
                // own units, on a swing Mark has watched.
                if (pi >= 0) {
                    var wm = null
                    try { wm = cm.windowedMean(t, v, mask) }
                    catch (e) { probe.miss("cm.windowedMean (" + e + ")") }
                    if (wm && wm.mean && wm.mean.length === t.length) {
                        // The extremum of the DRAWN line over the same clamped window the card
                        // reduced, found by walking the array the chart draws — the identity is
                        // worth nothing if it is checked by calling the reducer a second time.
                        var bMin = Infinity, bMax = -Infinity, nAnchor = 0
                        var hasMask = mask && mask.length >= t.length
                        for (var q3 = 0; q3 < t.length; ++q3) {
                            if (t[q3] < cs || t[q3] > ce) continue
                            if (hasMask && mask[q3] === 0) continue
                            // A non-finite sample is not a measurement and cannot be an anchor —
                            // SeriesView::isValid folds this in on the C++ side, so a scan that did
                            // not would compare the reducer's answer against a different set of
                            // candidates and report a false ⛔ (F5).
                            if (!isFinite(v[q3])) continue
                            nAnchor++
                            if (wm.mean[q3] < bMin) bMin = wm.mean[q3]
                            if (wm.mean[q3] > bMax) bMax = wm.mean[q3]
                        }
                        var drawn = (nAnchor === 0) ? null
                                  : (Math.abs(bMax) >= Math.abs(bMin) ? bMax : bMin)
                        probe.w("   PEAK on the DRAWN   = mean[" + pi + "] = "
                                + probe.num(wm.mean[pi], 4)
                                + "   vs raw value[" + pi + "] = " + probe.num(v[pi], 4)
                                + "   (the reduction moved the line "
                                + probe.num(Math.abs(wm.mean[pi] - v[pi]), 4) + " "
                                + cm.shortUnit(raw.unit) + " here)"
                                + "   meanSigma = " + probe.num(wm.sigma[pi], 4))
                        probe.w("   ONE CURVE (C17)     = "
                                + (nAnchor === 0
                                   ? "no valid anchor in the clamped window — the card read its "
                                     + "interpolated edges, so there is no drawn peak to match"
                                   : (drawn === csum.peak
                                      ? "✓ csum.peak === max/min of the drawn mean over "
                                        + nAnchor + " anchor(s), EXACTLY (" + probe.num(drawn, 6) + ")"
                                      : "⛔ csum.peak " + probe.num(csum.peak, 6)
                                        + " ≠ the drawn extremum " + probe.num(drawn, 6)
                                        + " — the tile and the stroke are NOT one reduction")))
                    } else if (wm) {
                        probe.miss("cm.windowedMean returned " + (wm.mean ? wm.mean.length : "no")
                                   + " mean(s) for " + t.length + " samples — the chart will fall "
                                   + "back to drawing the raw curve")
                    }
                }
                if (stepv > 1)
                    probe.w("   step rule cost      = PEAK "
                            + cm.formatBare(csum.peak, raw.unit) + " → "
                            + cm.formatBare(csum.peak, raw.unit, sg)
                            + "   @IMPACT " + cm.formatBare(impV, raw.unit) + " → "
                            + cm.formatBare(impV, raw.unit, sg)
                            + "   (whole units → multiples of " + probe.num(stepv, 0) + ")")
            } catch (e) { probe.miss("§5.3 card text (" + e + ")") }
        }

        // ── STILL ADDRESS — the window design §7 item 2 is measured on ────────────────────
        //
        // Address − 300 ms → Address. The golfer has not started the swing yet, so a peak rate
        // over it is measuring the pipeline, not the athlete: whatever it reads IS the noise
        // floor. Phase 0's baseline read 39 (% stance width) and 291 (°) per 100 ms here with the
        // adjacent-frame definition; the gate is under 2 with the ≥50 ms least-squares one, and
        // this line is how a single swing is checked without re-running the corpus.
        //
        // "ABSENT" is a legitimate answer, not a failure: a series whose first sample is at
        // Address has nothing to fit in the 300 ms before it, and saying so beats a fitted 0.
        var au = probe.phaseUs(0)
        if (!(au > 0)) {
            probe.w("summary STILL ADDR  = unavailable — no " + probe.ptag(0)
                    + " instant in the phase ladder")
        } else {
            var sa = sm(au - 300000, au)
            line("summary STILL ADDR ", sa, au - 300000, au, true)
            if (sa) {
                var pk = (sa.rateOk === false) ? "n/a" : Math.abs(sa.rate)
                probe.w("   §7 item 2 gate     = " + (pk === "n/a" ? "no rate fitted"
                        : (probe.num(pk) + " per 100ms — " + (pk < 2.0 ? "PASS (<2)" : "FAIL (≥2)")))
                        + "   (samples in window: " + (function () {
                            var c = 0
                            for (var q = 0; q < t.length; ++q)
                                if (t[q] >= au - 300000 && t[q] <= au) c++
                            return c
                        })() + ")")
            }
        }
        probe.w("clamp emptied win?  = " + (function () {
            try { return chart._domWinEmpty(p, ws, we) } catch (e) { return "?" }
        })() + "   facet @end text = '" + (function () {
            // sg, not omitted: the σ is an argument now (the chart hoists it per plot so a per-frame
            // binding never marshals the series), and leaving it off would hand formatBare an
            // undefined → NaN → step 1 and quietly print an UNSTEPPED number on the one surface
            // this decision is being judged from.
            try { return chart._facetEndText(p, sg) } catch (e) { return "?" }
        })() + "'")

        // ── @impact ──────────────────────────────────────────────────────────────────────
        var iu = probe.impactUs
        var measured = "?"
        try { measured = chart._measuredAt(p, iu) } catch (e) { probe.miss("chart._measuredAt (" + e + ")") }
        var refMeasured = "?"
        try { refMeasured = cm.measuredAt(t, mask, Math.round(iu),
                                          Math.round(p.validFromUs), Math.round(p.validToUs)) }
        catch (e) {}
        probe.w("@impact " + probe.ms(iu) + "     measured = " + measured
                + " (C++ measuredAt " + refMeasured + ")"
                + "   value text = '" + (function () {
                    try { return chart._valueTextAt(p, iu, sg) } catch (e) { return "?" }
                })() + "'"
                + "   band = '" + (function () {
                    try { return cm.bandAtNearest(raw.phaseSamples || [], Math.round(iu)) }
                    catch (e) { return "?" }
                })() + "'")

        // ── phaseSamples ─────────────────────────────────────────────────────────────────
        // §5.1: "phase samples outside it are not emitted". A sample flagged OUT OF DOMAIN
        // below is a Phase-1 violation for this swing, not a display quirk.
        var ps = raw.phaseSamples || []
        probe.w("phaseSamples        = " + ps.length)
        for (var s2 = 0; s2 < ps.length; ++s2) {
            var e2 = ps[s2]
            var inDom = (e2.t_us >= p.validFromUs && e2.t_us <= p.validToUs)
            probe.w("   " + probe.ptag(e2.phase) + " @" + probe.ms(e2.t_us)
                    + "  value=" + probe.num(e2.value)
                    + "  band='" + e2.band + "'"
                    + (inDom ? "" : "   ⛔ OUTSIDE DOMAIN (§5.1 says this should not be emitted)"))
        }
    }

    // ── §5.3: DID THE ±σ RIBBON ACTUALLY DRAW? ───────────────────────────────────────────
    //
    // "It is the only way to SHOW that the wobble is inside the noise rather than telling the reader
    // so" (§5.3), and §8 keeps it dark until it has been seen. This probe is the only thing that
    // turns it on, so it also has to be the thing that says whether it drew — asking the DRAWING
    // CODE (PpChartPlot.sigmaBandRuns, the length of the run list the Repeater is instantiating)
    // rather than inferring it from showSigmaBand being true, which would prove only that the
    // property was set.
    //
    // Zero polygons is a legitimate answer with three different meanings and they are reported apart:
    // no plot exists (the preset drew nothing), no visible series carries a σ (absence, not zero —
    // W1's propagation has not reached these metrics), or a plot has σ and still drew nothing, which
    // is a defect in the run splitting.
    function reportSigmaBand() {
        probe.w("")
        probe.w("═══ 6. THE ±σ RIBBON (design §5.3 / §8 — dark by default) ═══")
        probe.w("chart.showSigmaBand = " + chart.showSigmaBand
                + "   (nothing in the app turns this on; this probe is the only switch)")

        var plots = [], guard = 0
        function walk(it) {
            if (!it || guard++ > 8000) return
            // Duck-typed on the property rather than matched on a type name: PpChartPlot is a
            // module-local QML type with nothing for `instanceof` to test from out here, and
            // sigmaBandRuns exists on exactly one component.
            if (it.sigmaBandRuns !== undefined) plots.push(it)
            var kids = it.children || []
            for (var i = 0; i < kids.length; ++i) walk(kids[i])
        }
        try { walk(chart) } catch (e) { probe.miss("walk for PpChartPlot (" + e + ")") }

        var withSigma = []
        try {
            for (var j = 0; j < chart._visible.length; ++j) {
                var s = chart._visible[j]
                if (s && s.sigma !== undefined && s.sigma !== null && isFinite(s.sigma) && s.sigma > 0)
                    withSigma.push(s.key + " σ=" + probe.num(s.sigma))
            }
        } catch (e) { probe.miss("chart._visible (" + e + ")") }
        probe.w("visible series with σ = " + (withSigma.length ? withSigma.join(", ") : "NONE"))

        var total = 0
        probe.w("PpChartPlot instances = " + plots.length)
        for (var i2 = 0; i2 < plots.length; ++i2) {
            var pl = plots[i2]
            var n = 0
            try { n = pl.sigmaBandRuns } catch (e) {}
            total += n
            // ── IS THE BAND CLIPPED? (F4) ─────────────────────────────────────────────
            //
            // The ribbon reaches σ past the curve on both sides, so a plot whose Y extents were
            // computed from the curve alone clips it FLAT at the axis edge — and a flat-topped noise
            // band claims the uncertainty stops there, which is worse than drawing none.
            // PpMetricChart._rangeFor therefore adds the largest visible σ to its padding WHEN THE
            // FLAG IS ON. This line is the check: headroom is what the axis grants beyond the drawn
            // extremum, and it must be at least maxσ for the band to close.
            var maxSg = 0, dLo = Infinity, dHi = -Infinity
            try {
                for (var q = 0; q < (pl.series ? pl.series.length : 0); ++q) {
                    var ps2 = pl.series[q]
                    if (!ps2 || !ps2.value) continue
                    var s2 = cm.seriesSigma(ps2)
                    if (s2 > maxSg) maxSg = s2
                    for (var r = 0; r < ps2.value.length; ++r) {
                        if (ps2.t_us[r] < pl.domStartUs || ps2.t_us[r] > pl.domEndUs) continue
                        if (ps2.value[r] < dLo) dLo = ps2.value[r]
                        if (ps2.value[r] > dHi) dHi = ps2.value[r]
                    }
                }
            } catch (e) {}
            var headLo = (dLo === Infinity) ? 0 : (dLo - pl.valueLo)
            var headHi = (dHi === -Infinity) ? 0 : (pl.valueHi - dHi)
            probe.w("   plot[" + i2 + "] facet='" + (pl.facetName || "(overlay)")
                    + "'  series=" + ((pl.series && pl.series.length) || 0)
                    + "  ribbon polygons=" + n
                    + "   maxσ=" + probe.num(maxSg)
                    + "   headroom lo/hi=" + probe.num(headLo) + "/" + probe.num(headHi)
                    + (maxSg > 0
                       ? (Math.min(headLo, headHi) >= maxSg - 1e-9
                          ? "   band CLOSES (headroom ≥ maxσ)"
                          : "   ⛔ band CLIPPED FLAT at the axis edge (headroom < maxσ)")
                       : ""))
        }
        if (total > 0)
            probe.w("RIBBON DREW         = YES — " + total + " polygon(s) at 0.06 opacity, one per "
                    + "MEASURED run (bridged runs get none: no measurement, no error band)")
        else if (plots.length === 0)
            probe.w("RIBBON DREW         = no — this preset instantiated no plot at all")
        else if (withSigma.length === 0)
            probe.w("RIBBON DREW         = no — no visible series carries a σ. ABSENT, not zero: a "
                    + "zero-width ribbon would claim perfect precision, so nothing is drawn.")
        else
            probe.w("RIBBON DREW         = ⛔ NO, and it should have — σ is present and the band is "
                    + "on, so the run splitting in PpChartPlot._sigmaRuns is at fault")
    }

    // ── 7. THE DRAWN LINE (Phase 6 / C17) ────────────────────────────────────────────────
    //
    // Same shape as the ribbon report above, and for the same reason: the only honest way to say
    // whether the raw dots drew is to ask the DRAWING CODE, not to infer it from showRawDots being
    // true. Two questions, and they are reported apart: how many series were HANDED dot data
    // (PpChartPlot.rawDotRuns, the model its Repeater got) and how many polylines the PathMultiline
    // elements are actually HOLDING (their pathCount, read back after assignment). The second is the
    // claim; the first only tells you what was asked for.
    //
    // Why it matters enough to have its own section: Phase 6 changed what the stroke IS. Drawing a
    // reduction is only defensible while the samples it reduces are still on screen, so the dots are
    // not decoration — they are the half of the change that keeps it honest. A plot stroking the mean
    // with no dots behind it is display-only smoothing, which design §4 principle 1 forbids outright,
    // and this line is how a run says which of the two shipped.
    function reportDrawnLine() {
        probe.w("")
        probe.w("═══ 7. THE DRAWN LINE = THE WINDOWED MEAN (design §4 principle 1, Phase 6) ═══")

        var plots = [], dotShapes = [], guard = 0
        function walk(it) {
            if (!it || guard++ > 8000) return
            // Duck-typed, like the ribbon walk: PpChartPlot is a module-local type with nothing for
            // `instanceof` to test from out here, and rawDotRuns exists on exactly one component.
            if (it.rawDotRuns !== undefined) plots.push(it)
            // ⚠ AND THE DOT SHAPES THEMSELVES (F7). rawDotRuns is the length of the JS model the
            // Repeater was GIVEN; pathCount is what the PathMultiline is holding after the assignment.
            // Only the second can fail, and it is the one thing here no test can reach: `paths` takes
            // a QVariant, so a JS array-of-arrays-of-points that did not convert would leave the
            // element empty and the dots absent with no warning anywhere. Reading it back is the
            // difference between "we asked for dots" and "there are dots".
            if (it.objectName === "rawDots" && it.pathCount !== undefined) dotShapes.push(it)
            var kids = it.children || []
            for (var i = 0; i < kids.length; ++i) walk(kids[i])
        }
        try { walk(chart) } catch (e) { probe.miss("walk for PpChartPlot (" + e + ")") }

        // Does every visible series actually CARRY a mean? A series the host failed to decorate falls
        // back to the raw curve (PpChartPlot._meanOf) — the pre-Phase-6 picture, silently, on that one
        // trace — so the count is reported rather than assumed.
        var withMean = 0, withoutMean = []
        try {
            for (var j = 0; j < chart._visible.length; ++j) {
                var s = chart._visible[j]
                if (!s) continue
                if (s.mean && s.t_us && s.mean.length === s.t_us.length) withMean++
                else withoutMean.push(s.key)
            }
        } catch (e) { probe.miss("chart._visible (" + e + ")") }
        probe.w("visible series with a mean = " + withMean
                + (withoutMean.length ? "   ⛔ NO MEAN (drawing raw): " + withoutMean.join(", ")
                                      : "   ✓ every visible series draws the reduction"))

        var asked = 0
        probe.w("PpChartPlot instances      = " + plots.length)
        for (var i2 = 0; i2 < plots.length; ++i2) {
            var pl = plots[i2]
            var n = 0
            try { n = pl.rawDotRuns } catch (e) {}
            asked += n
            probe.w("   plot[" + i2 + "] facet='" + (pl.facetName || "(overlay)")
                    + "'  series=" + ((pl.series && pl.series.length) || 0)
                    + "  showRawDots=" + pl.showRawDots
                    + "  raw-dot series=" + n)
        }

        // What the ELEMENTS hold, read back one by one.
        var drawnPts = 0, unreadable = 0
        for (var i3 = 0; i3 < dotShapes.length; ++i3) {
            var pc = -1
            try { pc = dotShapes[i3].pathCount } catch (e) {}
            if (pc < 0) unreadable++
            else drawnPts += pc
        }
        probe.w("PathMultiline read-back    = " + dotShapes.length + " dot Shape(s), "
                + drawnPts + " polyline(s) held"
                + (unreadable ? "   ⛔ " + unreadable + " whose `paths` could not be counted — the "
                                + "QVariant conversion is suspect, dots may be absent on screen"
                              : ""))

        if (plots.length === 0)
            probe.w("RAW DOTS DREW              = no — this preset instantiated no plot at all")
        else if (drawnPts > 0)
            probe.w("RAW DOTS DREW              = YES — " + drawnPts + " persisted samples at 0.35 "
                    + "opacity behind the stroke (counted off the PathMultiline, not off the model), "
                    + "so the wobble the 40 ms window declined to call a peak is still on screen")
        else if (withMean === 0)
            probe.w("RAW DOTS DREW              = no, and correctly — no visible series carries a "
                    + "mean, so the stroke IS the raw curve and dots would double it")
        else if (asked > 0)
            probe.w("RAW DOTS DREW              = ⛔ NO — " + asked + " series were HANDED dot data and "
                    + "the elements are holding none of it. The model is fine and the drawing is not: "
                    + "suspect the PathMultiline `paths` assignment or a zero stroke width.")
        else
            probe.w("RAW DOTS DREW              = ⛔ NO, and they should have — the traces are drawing "
                    + "a reduction with the samples it reduces hidden, which is display-only "
                    + "smoothing by any other name (check PpChartPlot.showRawDots / _rawDots)")
    }

    // ── The live on-screen chart, read-only ──────────────────────────────────────────────
    // Not the subject of the report — just a cross-check that the real panel exists and is
    // sitting on the same vocabulary. It is absent unless the user's View layout has the
    // Charts panel on for this mode; that absence is reported, never corrected.
    function reportLiveChart() {
        probe.w("═══ 8. THE ON-SCREEN CHART (read-only cross-check) ═══")
        var root = probe
        var guard = 0
        while (root.parent && guard++ < 64) root = root.parent
        var found = [], seen = 0
        function walk(it, depth) {
            if (!it || depth > 40 || seen > 8000) return
            seen++
            try {
                if (it !== chart && it.seriesList !== undefined && typeof it.preset === "string")
                    found.push(it)
            } catch (e) {}
            var ch = null
            try { ch = it.children } catch (e) { return }
            if (!ch) return
            for (var i = 0; i < ch.length; ++i) walk(ch[i], depth + 1)
        }
        walk(root, 0)
        probe.w("items walked        = " + seen + "   PpMetricChart instances found = " + found.length)
        if (found.length === 0) {
            probe.w("NOTE: no live chart. Expected when ViewLayout.isPanelOn(mode,'charts') is "
                    + "false for this mode — PpModeStage only instantiates enabled panels. The "
                    + "probe does NOT turn it on: that would write a persisted user setting.")
            return
        }
        for (var i2 = 0; i2 < found.length; ++i2) {
            var c = found[i2]
            try {
                probe.w("   live[" + i2 + "] preset='" + c.preset + "' base='" + c._presetBase
                        + "' series=" + (c.seriesList ? c.seriesList.length : 0)
                        + " plottable=" + (c._plottable ? c._plottable.length : 0)
                        + " sessionType=" + c.sessionType
                        + " options=" + JSON.stringify(c._presetOptions))
            } catch (e) { probe.miss("live chart property read (" + e + ")") }
        }
    }

    function stepReport() {
        probe.w("═══ 5. WHAT THE CHART LAYER SEES ═══")
        if (!probe.series || probe.series.length === 0) return probe.miss("series (nothing to report)")
        var pk = []
        try { for (var i = 0; i < chart._plottable.length; ++i) pk.push(chart._plottable[i].key) }
        catch (e) {}
        probe.w("chart._plottable    = " + pk.length + " drawable curve(s) of "
                + probe.series.length + " series")
        for (var j = 0; j < probe.reportKeys.length; ++j) probe.reportSeries(probe.reportKeys[j])
    }

    // ── The run ──────────────────────────────────────────────────────────────────────────
    // Generous intervals: layout settles late, and a screen change needs ~2.5 s before the
    // panels it brought up have run their bindings. Every step is wrapped so a throw prints a
    // located line instead of leaving the run to time out silently.
    property int step: 0
    function safe(name, fn) {
        try { fn() } catch (e) { probe.w("⛔ THREW in " + name + ": " + e
                                         + (e && e.stack ? "\n" + e.stack : "")) }
    }

    Timer {
        interval: probe.stepMs; running: true; repeat: true
        onTriggered: {
            probe.step++
            switch (probe.step) {
            case 1: probe.safe("loadSession",   probe.stepLoadSession);   break
            case 2: probe.safe("focusSwing",    probe.stepFocusSwing);    break
            case 3: probe.safe("collectDetail", probe.stepCollectDetail); break
            case 4: probe.safe("applyPreset",   probe.stepApplyPreset);   break
            case 5: probe.safe("report",        probe.stepReport);        break
            case 6: probe.safe("sigmaBand",     probe.reportSigmaBand);   break
            case 7: probe.safe("drawnLine",     probe.reportDrawnLine);   break
            case 8: probe.safe("liveChart",     probe.reportLiveChart);   break
            default:
                probe.w("═══ DONE ═══")
                Qt.quit()
            }
        }
    }

    // Watchdog — a starved animation driver (occluded window) stalls the Timer above, so this
    // is deliberately NOT the only exit; it is the one that fires when nothing else does.
    Timer {
        interval: probe.stepMs * 12 + 20000; running: true; repeat: false
        onTriggered: { probe.w("⛔ WATCHDOG — quitting at step " + probe.step); Qt.quit() }
    }

    Component.onCompleted: {
        probe.w("═══ PLUMB BOB CHART PROBE ═══  step=" + probe.stepMs + "ms")
        probe.safe("environment", probe.reportEnvironment)
    }
}
