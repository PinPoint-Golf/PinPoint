// plumb_bob_chart.qml — what the CHART LAYER sees for the "Plumb Bob" preset.
//
//   QT_QPA_PLATFORM=offscreen PINPOINT_LOG_STDERR=1 \
//     .../PinPointStudio.app/Contents/MacOS/PinPointStudio \
//     --probe-qml /abs/path/plumb_bob_chart.qml
//
// Verifies Phases 1 and 2 of docs/design/metric_presentation_honesty.md WITHOUT screenshots:
// Phase 1's validity mask / phase domains / suppressed samples, and Phase 2's shared
// reducers — the σ on the window a card actually reduces, and the STILL ADDRESS window that
// design §7 item 2 ("under 2 units per 100 ms on a golfer who has not moved") is judged on.
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
//   5. Reports, per series, everything the chart layer decorates and derives.
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
        var mask = raw.valid || []
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
        line("summary CLAMPED    ", sm(cs, ce), cs, ce, true)

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
            try { return chart._facetEndText(p) } catch (e) { return "?" }
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
                    try { return chart._valueTextAt(p, iu) } catch (e) { return "?" }
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

    // ── The live on-screen chart, read-only ──────────────────────────────────────────────
    // Not the subject of the report — just a cross-check that the real panel exists and is
    // sitting on the same vocabulary. It is absent unless the user's View layout has the
    // Charts panel on for this mode; that absence is reported, never corrected.
    function reportLiveChart() {
        probe.w("═══ 6. THE ON-SCREEN CHART (read-only cross-check) ═══")
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
            case 6: probe.safe("liveChart",     probe.reportLiveChart);   break
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
