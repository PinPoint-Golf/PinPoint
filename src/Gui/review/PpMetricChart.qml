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

// PpMetricChart — reusable, prop-driven metric chart. Toggles between a single overlaid
// plot (all series, shared Y) and small multiples (one PpChartPlot per series, each Y
// auto-scaled). Owns the chart-LOCAL view window [viewStartUs,viewEndUs] (it never touches
// shotReplay / SwingDataSource, so different chart instances scope independently). The
// playhead and the view window are conceptually distinct — the window never moves the
// playhead; the playhead is simply clipped when outside the window.
//
// Phase 1: split/overlay, axes, phase bands, traces, P-dots, playhead, hover crosshair +
// tooltip, legend chips. Phase 2 adds the segment brush + chips → viewStartUs/viewEndUs.
// Phase 3 adds the summary cards. Phase 4 adds the METRICS preset combo — which catalogue
// group of series to plot, since a full capture now produces far more curves than one panel
// can show. Heavy maths lives in ChartMetrics; phase tags/value lookup in TimelineLabels;
// colours/sizes/fonts in Theme.
//
// TWO independent narrowings, deliberately kept apart: the preset chooses WHICH SERIES
// (enabledKeys), the segment chips/brush choose WHICH TIME SPAN (viewStartUs/viewEndUs).

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    // ── Data (host wires these from shotReplay) ───────────────────────────────────
    property var  seriesList: []         // [{ key, label, unit, t_us, value, phaseSamples }]
    property var  phases:     []         // [{ phase, t_us, conf }]
    property real startUs:    0
    property real endUs:      0
    property real impactUs:   0
    property real playheadUs: 0
    property bool showPlayhead: true

    // ── Chart-local view state (NOT bound to shotReplay / SwingDataSource) ─────────
    property real viewStartUs: root._axisStart      // segment/brush window (Phase 2)
    property real viewEndUs:   root._axisEnd
    property bool splitMode:   true                 // true = chart per series; false = overlay
    property var  enabledKeys: ({})                 // legend visibility ({} = all on)
    property bool showDots:    true
    property bool showCursor:  true

    // ── σ RIBBON — DARK, AND FORWARDED ONLY (design §5.3, §8 open question 3) ──────
    //
    // Passed straight through to every PpChartPlot, which owns the drawing and documents the shape
    // of it. Default FALSE and there is deliberately NO CONTROL for it anywhere: design §8 leaves
    // it "dark until seen", so the only thing that turns it on today is a probe setting the property
    // (tools/probes/plumb_bob_chart.qml). Adding a toolbar toggle would ship an unreviewed visual
    // to every reader and would also make it a persisted user preference, which is a decision
    // nobody has taken.
    property bool showSigmaBand: false

    // Click/drag on a plot seeks the replay to that time — the host wires these to
    // shotReplay (seekToUs / beginScrub / endScrub) so the video, overlay, timeline
    // and every other panel scrub together. Off by default: the chart stays prop-
    // driven and never touches the replay facade itself, and the compact transient
    // graph stays non-interactive.
    property bool seekable:    false
    signal seekRequested(real tUs)
    signal scrubBegan()
    signal scrubEnded()

    property string _preset:   "Full"               // active segment ("Full" / "ADR→TKW" / "Custom")

    // Compact mode: just the overlay plot (no toolbar / segment chips / brush / header /
    // legend / summary) — for small in-tile transients like the ¼× auto-replay graph.
    property bool compact: false
    readonly property bool _effSplit: root.splitMode && !root.compact

    // Collapsible sections (full mode only): CONTROLS / CHART / SUMMARY. Persisted
    // per screen+mode via AppSettings (key "<sessionType>:<mode>:<section>"); the
    // host passes sessionType (−1 for the compact transient, which never persists).
    // Restored on creation and whenever the screen+mode key changes; written on toggle.
    property int  sessionType: -1
    property bool controlsCollapsed: false
    property bool chartCollapsed:    false
    property bool summaryCollapsed:  false

    readonly property string _sectionKeyBase: root.sessionType + ":" + SessionMode.mode + ":"
    on_SectionKeyBaseChanged: { root._restoreSections(); root._restorePrefs() }
    Component.onCompleted:    { root._restoreSections(); root._restorePrefs() }

    function _restoreSections() {
        if (root.sessionType < 0) return            // compact / transient — keep defaults
        var m = appSettings.sectionCollapse, b = root._sectionKeyBase
        root.controlsCollapsed = m[b + "controls"] === true
        root.chartCollapsed    = m[b + "chart"]    === true
        root.summaryCollapsed  = m[b + "summary"]  === true
    }
    function _persistSection(name, val) {
        if (root.sessionType < 0) return            // compact / transient — don't persist
        var mm = {}
        for (var k in appSettings.sectionCollapse) mm[k] = appSettings.sectionCollapse[k]
        mm[root._sectionKeyBase + name] = val
        appSettings.sectionCollapse = mm
    }

    // Display controls (Split/Overlay, P dots, Cursor) + series visibility, persisted
    // per screen+mode alongside the section-collapse state — so Replay (mode 1) and
    // Analyse (mode 2) each remember their own chart, per session type. Same key base
    // ("<sessionType>:<mode>:"); stored in appSettings.chartPrefs. Restore resets a
    // field to its built-in default when this view never saved it, so the views stay
    // fully independent (no value bleeds from the one last edited).
    //
    // Persist is called explicitly from the control handlers (_toggle, the toolbar
    // toggles) — NOT from on<Property>Changed. This chart is a lazily-created panel
    // delegate, so it is constructed while already in Replay/Analyse; a `var`
    // property's construction-time change signal would otherwise fire _persistPref
    // with the default {} and clobber the saved selection before _restorePrefs reads
    // it. Interaction-only persistence has no such construction-time fire.
    function _restorePrefs() {
        if (root.sessionType < 0) return            // compact / transient — keep defaults
        var m = appSettings.chartPrefs, b = root._sectionKeyBase
        root.splitMode  = (m[b + "split"]  !== undefined) ? (m[b + "split"]  === true) : true
        root.showDots   = (m[b + "dots"]   !== undefined) ? (m[b + "dots"]   === true) : true
        root.showCursor = (m[b + "cursor"] !== undefined) ? (m[b + "cursor"] === true) : true
        root.enabledKeys = (m[b + "series"] && typeof m[b + "series"] === "object")
                           ? m[b + "series"] : ({})
        root.preset      = (m[b + "preset"]     !== undefined) ? String(m[b + "preset"])     : ""
        root._presetBase = (m[b + "presetBase"] !== undefined) ? String(m[b + "presetBase"]) : ""
        // Restore runs on creation and on every screen+mode change, and the series may already
        // be in hand by then — re-resolve so the restored preset actually reaches enabledKeys.
        root._syncPreset()
    }
    function _persistPref(name, val) {
        if (root.sessionType < 0) return            // compact / transient — don't persist
        var mm = {}
        for (var k in appSettings.chartPrefs) mm[k] = appSettings.chartPrefs[k]
        mm[root._sectionKeyBase + name] = val
        appSettings.chartPrefs = mm
    }

    // Shared geometry (one source of truth for plots + tooltip placement). Split needs a
    // wider gutter for the per-facet name + @end caption.
    readonly property real _gutterLeft: root._effSplit ? Theme.sp(92) : Theme.sp(54)
    readonly property real _padR:       Theme.sp(10)

    // ── Derived data ──────────────────────────────────────────────────────────────
    readonly property var _list: root.seriesList || []
    function _isOn(key) { return root.enabledKeys[key] !== false }
    function _toggle(key) {
        var e = Object.assign({}, root.enabledKeys)
        e[key] = !root._isOn(key)
        root.enabledKeys = e
        root._persistPref("series", root.enabledKeys)
        // The selection is no longer whatever group the combo names — say so.
        if (!root.compact && root.preset !== "Custom") {
            root.preset = "Custom"
            root._persistPref("preset", "Custom")
        }
    }
    function _color(i) { return Theme.chartSeriesColor(i) }

    // ── Metric presets ────────────────────────────────────────────────────────────
    // A fully-instrumented swing produces well over thirty plottable curves in six different
    // units, which is thirty stacked facets in Split and a meaningless shared Y in Overlay. The
    // combo picks ONE catalogue group at a time; the groups themselves come from C++
    // (ChartMetrics.seriesGroups, keyed on MetricDescriptor.group) so the chart and the Metric
    // Library section the same metrics the same way, and so a group this swing did not measure is
    // never offered.
    //
    // A preset is not a second visibility mechanism — it WRITES root.enabledKeys, the map the
    // legend chips already toggle and the plot already reads. Which is also why hand-toggling a
    // chip drops the combo to "Custom": the selection has left the group, and claiming otherwise
    // would be a label that lies about what is on screen.
    property string preset: ""                      // "" = unresolved; "All" / "Custom" / group name

    // The last preset that was actually PICKED — "Custom" is a state the chart falls into, never a
    // set of metrics, so it cannot answer "which metrics is the reader working with". This can,
    // and it is what the legend lists. Persisted with the preset so a restored "Custom" comes back
    // to the same vocabulary rather than to everything.
    property string _presetBase: ""

    readonly property var _groups: cm.seriesGroups(root.seriesList)
    readonly property var _presetOptions: {
        var out = ["All"]
        for (var i = 0; i < root._groups.length; ++i) out.push(root._groups[i].group)
        if (root.preset === "Custom") out.push("Custom")   // only reachable by hand, so listed only then
        return out
    }
    function _keysFor(name) {
        for (var i = 0; i < root._groups.length; ++i)
            if (root._groups[i].group === name) return root._groups[i].keys
        return []
    }
    // "All" clears the map — _isOn treats an absent key as on, so an empty map is every series
    // visible, and it stays correct for a swing that later produces a series this one did not.
    function _applyPreset(name, persist) {
        root.preset = name
        if (persist) root._persistPref("preset", name)
        if (name === "Custom") return               // enabledKeys already holds the hand selection
        root._presetBase = name
        if (persist) root._persistPref("presetBase", name)
        var e = {}
        if (name !== "All") {
            var keys = root._keysFor(name)
            for (var i = 0; i < root._plottable.length; ++i) {
                var k = root._plottable[i].key
                e[k] = keys.indexOf(k) >= 0
            }
        }
        root.enabledKeys = e
        if (persist) root._persistPref("series", e)
    }
    // What the panel's title says: THE VOCABULARY ON SCREEN, never the state of the selection.
    //
    // It used to append "· custom" once a legend chip was hand-toggled, on the reasoning that a
    // title naming a group the selection no longer matches would lie about what is on screen. The
    // reasoning was sound and the conclusion was still wrong: the COMBO already reads "Custom",
    // two controls apart, so the title was repeating a state the reader could already see — in
    // twice the point size, and at the cost of the one thing only the title says, which is which
    // family of metrics these curves belong to. A title is a name, not a status line.
    readonly property string _presetTitle: {
        // Under a hand-picked selection the name is still the group it was picked FROM: that is
        // the vocabulary being edited and what the legend is listing (see _legendSeries).
        const name = root.preset === "Custom" ? root._presetBase : root.preset
        // "" is the pre-resolution state — _syncPreset has not run, or this swing produced no
        // groups at all. Reading as "All" matches what enabledKeys actually is at that moment
        // (empty ⇒ every series on), so the title is never briefly wrong on the way in.
        if (name === "" || name === "All") return qsTr("All metrics")
        return name                 // a catalogue group name — manifest data, not a UI string
    }

    // Re-resolve against the swing on screen. Called when the groups change (a new swing) and
    // after prefs are restored. A remembered preset is re-APPLIED rather than trusted, because
    // the enabledKeys restored alongside it were computed for a different swing's series.
    function _syncPreset() {
        if (root.compact || root._groups.length === 0) return
        // Does the preset the chart is sitting on still mean anything for THIS swing? "All"
        // always does; a group does when the swing measured some of it. Custom is judged on the
        // group it was picked from — with that group unmeasured there is neither a plot nor a
        // legend left, so it is not a selection worth keeping.
        const name  = root.preset === "Custom" ? root._presetBase : root.preset
        const known = name === "All" || root._keysFor(name).length > 0
        if (!known) { root._applyPreset(root._groups[0].group, false); return }
        // The hand-picked map is the user's own — re-applying a preset over it would discard it.
        if (root.preset === "Custom") return
        root._applyPreset(root.preset, false)
    }
    on_GroupsChanged: root._syncPreset()
    function _name(s)  { return cm.shortLabel(s.key) || s.label || s.key }
    // ── Units on this panel: ONE SHORT TOKEN, STATED ONCE PER CONTEXT ─────────────
    //
    // Two rules, and between them they are why the summary grid stopped overprinting itself.
    //
    //   1. The token is SHORT. ChartMetrics.shortUnit turns "% stance width" into "%". The
    //      denominator lives in the metric's name and in the Metric Library; beside a number in a
    //      data face it was longer than the value it qualified.
    //   2. A unit is named ONCE per surface that has somewhere to name it. The summary card heads
    //      each card with its unit, so the four values under it are BARE. The split-mode gutter
    //      names it under the facet name, so the @end readout is bare. The legend chips and the
    //      hover tooltip have no header of their own and mix units row by row, so those carry it.
    //
    // Rule 2 is the one that generalises: a repeated unit is not redundancy to be tolerated, it is
    // a label in the wrong place. Put it on the container, not on every number inside it.
    //
    // The formatting itself is ChartMetrics.formatValue / .formatBare — one implementation in C++,
    // shared with PpChartSummary and PpTransitTimeline, and the only version of this rule that can
    // be asserted in a test.

    readonly property bool _hasAny: root._plottable.length > 0

    // Every series this swing can DRAW — a curve of at least two samples, with a matching value
    // array — decorated with its (stable, full-list) palette colour. The list also carries the
    // ones currently toggled off, because the legend has to keep offering them.
    //
    // The setup scalars (stance width, tempo, attack angle, low point …) are MetricSeries with an
    // empty curve and a single phaseSample, and they are dropped here. They had been reaching the
    // legend, which iterated the raw series list: a dozen chips that toggled a trace that does not
    // exist, on a panel already too crowded to read.
    readonly property var _plottable: {
        var out = []
        for (var i = 0; i < root._list.length; ++i) {
            var s = root._list[i]
            if (s && s.t_us && s.t_us.length > 1 && s.value && s.value.length === s.t_us.length) {
                var d = Object.assign({}, s)
                d.color = root._color(i)
                // The metric's PHASE DOMAIN as instants — decorated on here rather than resolved
                // in the plot and again in the summary, so both surfaces clip the same curve at
                // the same two times. (`valid` rides along from the bridge via Object.assign.)
                var dom = root._domainWindow(s.key, s)
                d.validFromUs = dom.fromUs
                d.validToUs   = dom.toUs
                out.push(d)
            }
        }
        return out
    }

    // ── Phase domains: where a metric's geometry means something ───────────────────
    //
    // ChartMetrics.domainFor gives the manifest's { firstPhase, lastPhase } as Phase ENUM values;
    // only the swing knows when its own P1 and P7 happened, so the ints are resolved here against
    // root.phases. Past impact a frontal-plane pelvis reading is measuring rotation rather than
    // translation, and the design's answer is not to hide the curve but to stop claiming it:
    // outside [fromUs, toUs] the trace is dashed, carries no phase dots, and contributes nothing
    // to a summary card (design §5.1).
    //
    // ⚠ A SIDE THE MANIFEST DID NOT NARROW IS NOT CLIPPED AT ALL. The default domain is
    // Address..Finish, but this chart's AXIS is the PADDED swing (Segmentation swingStart/End
    // ± boundPadUs = 250 ms), so Address is 250 ms inside the axis start and Finish 250 ms inside
    // its end. Clipping to the default would therefore dash a quarter-second off both ends of
    // EVERY whole-swing metric — headSway, xFactor, clubheadSpeed — and move its Full-window
    // PEAK/Δ/RATE on every swing, breaking the property this whole phase rests on: nothing changes
    // where the design does not fire. It would also empty any legitimately pre-address window.
    // So each side is clipped only when domainFor reports that side `firstNarrowed`/`lastNarrowed`.
    //
    // ⚠ AND A MISSING LANDMARK CLIPS NOTHING EITHER, per side. A swing whose ladder never resolved
    // the domain's last phase falls back to THIS SERIES' last sample time, which by construction
    // excludes not one sample. The series' own extent, not the axis: the axis is the FIRST series'
    // extent and would have dimmed the leading samples of any series that starts earlier. Absent
    // from the ladder is a reason to claim LESS about where the domain ends, not to hide a curve.
    function _phaseUs(phase, fallback) {
        for (var i = 0; i < root.phases.length; ++i)
            if (root.phases[i].phase === phase) return root.phases[i].t_us
        return fallback
    }
    function _domainWindow(key, s) {
        var d = cm.domainFor(key)
        var t = s.t_us
        var first = t[0], last = t[t.length - 1]
        // SNAPPED TO THE SERIES' OWN SAMPLE GRID. A phase instant lies between two frames, and the
        // producer's phase sample sits on the NEAREST frame (metric_channel.h nearestIndex) — on
        // the 08-18 probe swing the P7 sample was 0.6 ms after the impact instant, so an unsnapped
        // domain end excluded the very sample it was meant to include and hid every P7 dot on the
        // narrowed metrics. Same nearest rule, ties to the earlier frame, so the two agree exactly.
        return { fromUs: d.firstNarrowed ? root._nearestSampleUs(t, root._phaseUs(d.firstPhase, first)) : first,
                 toUs:   d.lastNarrowed  ? root._nearestSampleUs(t, root._phaseUs(d.lastPhase,  last))  : last }
    }
    function _nearestSampleUs(t, us) {
        if (!t || t.length === 0) return us
        var lo = 0, hi = t.length - 1
        while (lo < hi) { var mid = (lo + hi) >> 1; if (t[mid] < us) lo = mid + 1; else hi = mid }
        if (lo > 0 && (us - t[lo - 1]) <= (t[lo] - us)) lo = lo - 1
        return t[lo]
    }

    // A summary window CLAMPED to one series' domain. The reducers must search only inside the
    // domain (design §5.1), so every window handed to ChartMetrics goes through this pair.
    //
    // ORDERED on purpose: the end is floored at the start, so a window entirely past the domain
    // (say P7→P10 on a P1→P7 metric) collapses to a point at the domain's end instead of
    // inverting — summaryMasked would swap an inverted pair back into a window spanning the gap
    // between them, i.e. into exactly the out-of-domain region the clamp exists to remove.
    // ⚠ The same two lines live in PpChartSummary.qml, which clamps per CARD; they must agree.
    function _domWinStart(s, winStart) {
        return Math.max(winStart, s.validFromUs !== undefined ? s.validFromUs : winStart)
    }
    function _domWinEnd(s, winStart, winEnd) {
        return Math.max(root._domWinStart(s, winStart),
                        Math.min(winEnd, s.validToUs !== undefined ? s.validToUs : winEnd))
    }
    // A window the clamp EMPTIED: the reader picked a span (IMP→P8, say) that lies wholly outside
    // this metric's domain. Every reduction over it would be four confident numbers taken at one
    // instant, so the readouts print "—" instead. Guarded on a non-empty selection so a chart that
    // has not sized its window yet is not reported as out of domain.
    function _domWinEmpty(s, winStart, winEnd) {
        return winEnd > winStart && root._domWinEnd(s, winStart, winEnd) <= root._domWinStart(s, winStart)
    }

    // The split-mode gutter's "@end" readout — the same domain-clamped, mask-aware summary the
    // cards use, and the same "—" when the clamp emptied the window.
    //
    // BARE: PpChartPlot's split gutter prints unitLabel directly above this, so spelling it again
    // put the unit twice in one 40px column. Unclamped and unmasked it printed the FINISH value of
    // a metric that stops meaning anything at impact, in the gutter, as the facet's headline.
    // ── THE SERIES' σ, FOR FORMATTING ONLY (design §5.3) ───────────────────────────
    //
    // There is no `_sigma()` helper here any more: the absent→0 substitution is ChartMetrics
    // .seriesSigma, one implementation in C++ where it can be tested, replacing the three copies of
    // a four-clause guard that had grown here, in PpChartSummary and inside PpChartPlot._sigmaRuns.
    //
    // ⚠ BUT IT IS NEVER CALLED FROM A PER-FRAME BINDING, and that is why the two functions below
    // TAKE the σ instead of deriving it. seriesSigma's argument is a QVariantMap, so every call
    // marshals the whole series (t_us, value, valid, phaseSamples) across the QML boundary — the
    // same cost measuredAt() carries its own warning about. The hover tooltip re-evaluates on every
    // cursor move and the facet gutter on every frame of a brush drag; both would pay it per frame
    // per series. So each σ is resolved ONCE on a binding that changes with the DATA (the tooltip
    // row's `sig`, the legend chip's `sig`, the plot delegate's `facetSigma`) and passed down.

    function _facetEndText(s, sigma) {
        if (root._domWinEmpty(s, root.viewStartUs, root.viewEndUs)) return "@end —"
        return "@end " + cm.formatBare(
            cm.summaryMasked(s.t_us, s.value, s.valid || [],
                             root._domWinStart(s, root.viewStartUs),
                             root._domWinEnd(s, root.viewStartUs, root.viewEndUs)).end,
            s.unit, sigma)
    }

    // Was this series MEASURED at `t`, or is the value there bridged / outside the domain? Same
    // predicate PpChartPlot dashes the stroke and drops the phase dot on, so the readouts and the
    // curve cannot say different things.
    //
    // ⚠ ANSWERED IN JS, NOT VIA ChartMetrics.measuredAt, and that is a performance requirement
    // rather than a preference: every call into C++ marshals the WHOLE series across the QML
    // boundary, and both callers here re-evaluate per frame — the hover tooltip on every cursor
    // move, the legend chip on every replay frame at 240 fps. With twenty visible series of a few
    // hundred samples that is millions of QVariant conversions a second. The C++ form stays as the
    // reference implementation and serves the one binding that changes only with the data.
    //
    // The short-mask rule is the C++ one (chart_metrics.h): a mask that does not cover the curve
    // is discarded wholesale, not applied to the part it does cover.
    function _nearestIndex(s, t) {
        var tt = s.t_us
        if (!tt || tt.length === 0) return -1
        var best = -1, bd = Infinity
        for (var i = 0; i < tt.length; ++i) {
            var d = Math.abs(tt[i] - t)
            if (d < bd) { bd = d; best = i }
        }
        return best
    }
    function _measuredAt(s, t) {
        if (s.validFromUs !== undefined && s.validToUs !== undefined
            && s.validToUs > s.validFromUs && (t < s.validFromUs || t > s.validToUs))
            return false
        if (!s.valid || s.valid.length < (s.t_us ? s.t_us.length : 0)) return true
        var i = root._nearestIndex(s, t)
        return i < 0 || s.valid[i] !== 0
    }
    // …and the value there as TEXT, or "—" where there was no measurement. One helper for the
    // hover tooltip and the legend readout, both of which print a number with a unit beside it
    // and so both make the same claim. Printing the bridged number is the confident absurdity
    // this whole design exists to stop; a blank or a "0" would each read as a measurement
    // instead of as its absence.
    // The σ step goes on this one too, so the hover row and the legend chip say the same digits the
    // card does for the same reading — a tooltip that reads 11.4 beside a card reading 10 is the
    // "one curve, one number" rule broken at the last inch. `sigma` is PASSED, not derived: this
    // runs per cursor move per visible series (see the note above _facetEndText).
    function _valueTextAt(s, t, sigma) {
        return root._measuredAt(s, t)
             ? cm.formatValue(labels.valueAtNearest(s.t_us, s.value, t), s.unit, sigma)
             : "—"
    }

    // What the LEGEND lists — the preset's metrics, not the swing's. Listing every plottable
    // series here put the whole wall of metrics back on screen one row further down, which is
    // exactly what the combo exists to stop; the chips are part of the same crowding problem as
    // the facets, not an index to it.
    //
    // It lists the preset's MEMBERS rather than the visible ones, so a chip switched off stays
    // present and dimmed and can be switched back on — a legend you can only subtract from is a
    // one-way door.
    //
    // Under "Custom" it stays on _presetBase, the group the reader was in when they started
    // hand-picking: that is the vocabulary being edited. Picking freely across everything is what
    // "All" is for, and going there first widens the legend to match.
    readonly property var _legendSeries: {
        if (root._presetBase === "" || root._presetBase === "All") return root._plottable
        var keys = root._keysFor(root._presetBase), out = []
        for (var i = 0; i < root._plottable.length; ++i)
            if (keys.indexOf(root._plottable[i].key) >= 0) out.push(root._plottable[i])
        return out
    }

    // …of those, the ones currently switched on. Derived from _legendSeries, NOT from _plottable,
    // and that is the load-bearing part: nothing can be on screen that the legend does not offer.
    //
    // Taken off _plottable it was wrong under "Custom". enabledKeys only carries the keys the
    // preset wrote, and _isOn reads an ABSENT key as on, so the moment a new swing produced a
    // series the hand-picked map had never heard of, that series drew itself into a selection
    // nobody had chosen — a chart showing curves with no chip to switch them off.
    readonly property var _visible: {
        var out = []
        for (var i = 0; i < root._legendSeries.length; ++i)
            if (root._isOn(root._legendSeries[i].key)) out.push(root._legendSeries[i])
        return out
    }

    // Full-swing extents (the window defaults to these; the brush narrows them in Phase 2).
    readonly property real _dataStart: (root._list.length && root._list[0].t_us
                                        && root._list[0].t_us.length) ? root._list[0].t_us[0] : 0
    readonly property real _dataEnd: {
        var e = root._dataStart + 1
        for (var i = 0; i < root._list.length; ++i) {
            var t = root._list[i].t_us
            if (t && t.length) e = Math.max(e, t[t.length - 1])
        }
        return e
    }
    readonly property real _axisStart: (root.startUs > 0 && root.endUs > root.startUs) ? root.startUs : root._dataStart
    readonly property real _axisEnd:   (root.startUs > 0 && root.endUs > root.startUs) ? root.endUs   : root._dataEnd

    // Shared value range (overlay) / per-series range (split), clipped to the view window.
    function _rangeFor(arr) {
        var lo = Infinity, hi = -Infinity, sig = 0
        for (var k = 0; k < (arr ? arr.length : 0); ++k) {
            var s = arr[k]
            if (!s || !s.t_us || !s.value) continue   // skip transient/torn-down entries
            var t = s.t_us, v = s.value
            for (var j = 0; j < v.length; ++j) {
                if (t[j] < root.viewStartUs || t[j] > root.viewEndUs) continue
                if (v[j] < lo) lo = v[j]; if (v[j] > hi) hi = v[j]
            }
            // ── RIBBON HEADROOM, AND ONLY WHEN THE RIBBON IS ON ──────────────────────
            //
            // The ±σ band reaches σ past the curve on both sides, so on a series whose extremum sits
            // at the axis edge the band is CLIPPED FLAT against it — and a flat-topped noise band is
            // the most misleading thing it could draw: it says the uncertainty stops there. So the
            // extents grow by the largest σ among the series in THIS plot (per plot, because split
            // mode scales each facet on its own).
            //
            // ⚠ GATED ON showSigmaBand, which is dark by default, so no shipped axis moves. That
            // matters beyond tidiness: the Y range is what every value is drawn against, and quietly
            // widening it would change the look of every chart in the app for a feature nobody can
            // see yet. When the flag is off this costs one boolean test per series.
            if (root.showSigmaBand) {
                var ss = cm.seriesSigma(s)
                if (ss > sig) sig = ss
            }
        }
        if (lo === Infinity) { lo = 0; hi = 1 }
        var pad = Math.max((hi - lo) * 0.14, 2) + sig
        return { lo: lo - pad, hi: hi + pad }
    }

    // Plot specs — ONE uniform object shape for both modes so the Repeater delegate never
    // has to type-switch modelData (the [array]-as-modelData trick raced during toggles):
    //   split  → one spec per visible series ({ series:[s], facet:true })
    //   overlay→ a single spec over all visible series ({ series:_visible, facet:false })
    readonly property var _plots: {
        if (root._visible.length === 0) return []
        if (root._effSplit) {
            var out = []
            for (var i = 0; i < root._visible.length; ++i)
                out.push({ series: [root._visible[i]], facet: true,
                           last: i === root._visible.length - 1 })
            return out
        }
        return [{ series: root._visible, facet: false, last: true }]
    }

    // Cursor for the chip readout: the playhead during replay, else impact (else window end).
    readonly property real _readoutUs: root.showPlayhead ? root.playheadUs
                                     : (root.impactUs > 0 ? root.impactUs : root._axisEnd)
    function _addrValue(s) {
        var ps = s.phaseSamples || []
        for (var i = 0; i < ps.length; ++i) if (ps[i].phase === 0) return ps[i].value  // Address
        return (s.value && s.value.length) ? s.value[0] : 0
    }

    // ── Segments (chart-local window presets) ──────────────────────────────────────
    // [0]=Full, then adjacent phase pairs. Labels composed here via TimelineLabels.
    readonly property var _segments:  cm.segments(root.phases, root._axisEnd - root._axisStart)
    readonly property int _nearStart: cm.nearestPhase(root.phases, root.viewStartUs)
    readonly property int _nearEnd:   cm.nearestPhase(root.phases, root.viewEndUs)

    function _segLabel(seg) {
        return seg.phaseA === -1 ? qsTr("Full")
             : labels.phaseShortTag(seg.phaseA) + "→" + labels.phaseShortTag(seg.phaseB)
    }
    function _selectSegment(seg) {
        if (seg.phaseA === -1) {                         // Full — use the true axis extent
            root.viewStartUs = root._axisStart
            root.viewEndUs   = root._axisEnd
            root._preset     = "Full"
        } else {
            root.viewStartUs = seg.startUs
            root.viewEndUs   = seg.endUs
            root._preset     = root._segLabel(seg)
        }
    }

    // A new swing restores the full-window bindings (the imperative chip/brush writes break
    // them); the window is chart-local, so it always re-opens to the whole new swing.
    onSeriesListChanged: {
        root._preset     = "Full"
        root.viewStartUs = Qt.binding(function () { return root._axisStart })
        root.viewEndUs   = Qt.binding(function () { return root._axisEnd })
    }

    // Shared hover cursor, fanned back to every plot so the crosshair spans all facets.
    property real _cursorUs: -1
    function _tooltipX(areaW) {
        var span = root.viewEndUs - root.viewStartUs
        if (span <= 0) return root._gutterLeft
        return root._gutterLeft
             + (root._cursorUs - root.viewStartUs) / span * (areaW - root._gutterLeft - root._padR)
    }

    ChartMetrics   { id: cm }
    TimelineLabels { id: labels }

    // Collapsible section header (caret + title, click to toggle) — same idiom as
    // PpDataViewer's SectionHeader, ending in a hairline rule.
    component SectionHeader: Rectangle {
        id: sh
        property string title: ""
        property bool   collapsed: false
        signal toggled()
        Layout.fillWidth: true
        implicitHeight: Theme.sp(24)
        color: shMa.containsMouse ? Theme.colorBg3 : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        radius: Theme.sp(4)
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.sp(6); anchors.rightMargin: Theme.sp(6)
            spacing: Theme.sp(7)
            Text {
                text: "▸"; rotation: sh.collapsed ? 0 : 90
                color: Theme.colorText3; font.pixelSize: Theme.fontSzBody2
                Behavior on rotation { enabled: !Theme.reduceMotion
                                       NumberAnimation { duration: Theme.durationFast } }
            }
            Text {
                text: sh.title; color: Theme.colorText3
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.colorBorder }
        }
        PpPressable { id: shMa; hoverScale: 1.0; onClicked: sh.toggled() }
    }

    // ── Layout ────────────────────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.sp(8)
        visible: root._hasAny

        // ── Panel title ───────────────────────────────────────────────────────────
        // PpDisplayText is the app's one display-title component — brand warm→cool gradient
        // fill, Theme.fontSzDisplay, flat fallback under reduce-motion — the same call the
        // Metric Library and the metric detail sheet make. Not a Text with a gradient bolted
        // on: the mask/MultiEffect machinery lives in that component precisely so no caller
        // reimplements it.
        //
        // Outside the collapsible sections, and above CONTROLS, because it titles the PANEL
        // rather than any one section — collapsing all three must not take the panel's name
        // with them. Hidden in compact, which is plot-only by definition.
        PpDisplayText {
            objectName: "presetTitle"        // tst_chart_presets reaches it by name
            visible: !root.compact
            text: root._presetTitle
            // ⚠ CAPPED, NEVER Layout.fillWidth — the gradient is why. PpDisplayText paints the
            // brand sweep across a Rectangle anchored to its GLYPHS, and the glyphs track the
            // item's width. Filled to a 900px panel, "Wrist & forearm" occupies the first fifth
            // of that sweep and renders in near-flat warm: the gradient is still there and looks
            // switched off. Sized to its text it spans warm→cool over the words, which is what
            // every other caller gets by simply not stretching (MetricLibrary sizes naturally;
            // MetricDetail caps, like this).
            //
            // Capped against root.width rather than the enclosing ColumnLayout: the layout's own
            // width is derived from its children, so capping a child against it is a cycle Qt
            // gives up on after two passes. root's width comes from the panel above it.
            //
            // The group names are short, but "Tempo & sequence · custom" at display size in a
            // narrow split panel is not: elide rather than wrap, so the title can never push
            // the plot down a line.
            Layout.maximumWidth: root.width
            elide: Text.ElideRight
            maximumLineCount: 1
        }

        // ── CONTROLS section ──────────────────────────────────────────────────────
        SectionHeader {
            visible: !root.compact
            title: qsTr("CONTROLS"); collapsed: root.controlsCollapsed
            onToggled: { root.controlsCollapsed = !root.controlsCollapsed
                         root._persistSection("controls", root.controlsCollapsed) }
        }

        // Toolbar: Split/Overlay + P-dots / Cursor toggles.
        RowLayout {
            visible: !root.compact && !root.controlsCollapsed
            Layout.fillWidth: true
            spacing: Theme.sp(10)

            PpSegmentedControl {
                Layout.preferredWidth: Theme.sp(150)
                options:  [qsTr("Split"), qsTr("Overlay")]
                selected: root.splitMode ? qsTr("Split") : qsTr("Overlay")
                onActivated: (v) => { root.splitMode = (v === qsTr("Split"))
                                      root._persistPref("split", root.splitMode) }
            }

            Text {
                text: qsTr("METRICS")
                Layout.leftMargin: Theme.sp(4)
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingLabel
                color: Theme.colorText3
            }

            PpComboBox {
                Layout.preferredWidth: Theme.sp(190)
                // Hidden rather than empty when the catalogue matched nothing — with no groups
                // the only option would be "All", a control whose one choice is the state it is
                // already in.
                visible: root._groups.length > 0
                model: root._presetOptions
                currentIndex: root._presetOptions.indexOf(root.preset)
                onActivated: (i) => root._applyPreset(root._presetOptions[i], true)
            }

            Item { Layout.fillWidth: true }      // spacer

            Repeater {
                model: [{ t: qsTr("P dots"), on: root.showDots, k: "dots" },
                        { t: qsTr("Cursor"), on: root.showCursor, k: "cursor" }]
                delegate: Row {
                    id: tog
                    required property var modelData
                    spacing: Theme.sp(5)
                    Rectangle {
                        width: Theme.sp(14); height: Theme.sp(14); radius: Theme.sp(4)
                        anchors.verticalCenter: parent.verticalCenter
                        color: tog.modelData.on ? Theme.colorAccent : "transparent"
                        border.width: Theme.sp(1.5)
                        border.color: tog.modelData.on ? Theme.colorAccent : Theme.colorBorderStrong
                        Text {
                            anchors.centerIn: parent
                            visible: tog.modelData.on
                            text: "✓"; color: Theme.dark ? Theme.colorBg : "#FFFFFF"
                            font.pixelSize: Theme.fontSzMicro
                        }
                    }
                    Text {
                        text: tog.modelData.t
                        anchors.verticalCenter: parent.verticalCenter
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                        font.letterSpacing: Theme.trackingData
                        color: Theme.colorText2
                    }
                    TapHandler {
                        onTapped: {
                            if (tog.modelData.k === "dots") {
                                root.showDots = !root.showDots
                                root._persistPref("dots", root.showDots)
                            } else {
                                root.showCursor = !root.showCursor
                                root._persistPref("cursor", root.showCursor)
                            }
                        }
                    }
                }
            }
        }

        // Segment preset chips — Full + adjacent phase pairs (+ Custom on free-drag).
        Flow {
            visible: !root.compact && !root.controlsCollapsed
            Layout.fillWidth: true
            spacing: Theme.sp(6)

            Text {
                text: qsTr("SEGMENT")
                height: Theme.sp(28); verticalAlignment: Text.AlignVCenter
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingLabel
                color: Theme.colorText3
                rightPadding: Theme.sp(2)
            }

            Repeater {
                model: root._segments
                delegate: Rectangle {
                    id: segChip
                    required property var modelData
                    readonly property string lbl: root._segLabel(segChip.modelData)
                    readonly property bool active: root._preset === segChip.lbl
                    implicitWidth: segLblText.width + Theme.sp(20); height: Theme.sp(28)
                    radius: Theme.sp(7)
                    color: active ? Theme.colorAccentLight
                                  : segChipMa.containsMouse ? Theme.colorAccentMid : "transparent"
                    border.width: 1
                    border.color: active ? Theme.colorAccent
                                         : segChipMa.containsMouse ? Theme.colorAccentMid : Theme.colorBorderMid
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
                    Text {
                        id: segLblText
                        anchors.centerIn: parent
                        text: segChip.lbl.toUpperCase()
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                        font.letterSpacing: Theme.trackingData
                        color: segChip.active ? Theme.colorAccent : Theme.colorText2
                    }
                    MouseArea {
                        id: segChipMa
                        anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._selectSegment(segChip.modelData)
                    }
                }
            }

            // Custom chip — present only while a free-dragged window is active.
            Rectangle {
                visible: root._preset === "Custom"
                implicitWidth: customText.width + Theme.sp(20); height: Theme.sp(28)
                radius: Theme.sp(7)
                color: Theme.colorAccentLight
                border.width: 1; border.color: Theme.colorAccent
                Text {
                    id: customText
                    anchors.centerIn: parent
                    text: qsTr("CUSTOM")
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                    font.letterSpacing: Theme.trackingData
                    color: Theme.colorAccent
                }
            }
        }

        // Overview + draggable selection window → chart-local view window.
        PpSegmentBrush {
            visible: !root.compact && !root.controlsCollapsed
            Layout.fillWidth: true
            series:      root._visible
            phases:      root.phases
            axisStartUs: root._axisStart
            axisEndUs:   root._axisEnd
            winStartUs:  root.viewStartUs
            winEndUs:    root.viewEndUs
            impactUs:    root.impactUs
            onViewWindowChanged: (s, e) => {
                root.viewStartUs = s
                root.viewEndUs   = e
                root._preset     = "Custom"
            }
        }

        // Segment header — phase span + duration of the active window.
        RowLayout {
            visible: !root.compact && !root.controlsCollapsed
            Layout.fillWidth: true
            spacing: Theme.sp(12)
            Text {
                text: root._preset === "Full" ? qsTr("Full swing")
                    : labels.phaseFullName(root._nearStart) + " → " + labels.phaseFullName(root._nearEnd)
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzHeading
                color: Theme.colorText
            }
            Text {
                // "shown of plottable", not the raw series count: the raw count included the
                // setup scalars, so it never matched the number of traces on screen.
                text: Math.round((root.viewEndUs - root.viewStartUs) / 1000) + qsTr(" ms")
                      + " · " + root._visible.length + "/" + root._plottable.length
                      + qsTr(" metrics") + " · "
                      + labels.phaseShortTag(root._nearStart) + "→" + labels.phaseShortTag(root._nearEnd)
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                font.letterSpacing: Theme.trackingData
                color: Theme.colorText3
            }
        }

        // ── CHART section ─────────────────────────────────────────────────────────
        SectionHeader {
            visible: !root.compact
            title: qsTr("CHART"); collapsed: root.chartCollapsed
            onToggled: { root.chartCollapsed = !root.chartCollapsed
                         root._persistSection("chart", root.chartCollapsed) }
        }

        // Plot area — one plot (overlay) or one per visible series (split).
        Item {
            id: plotArea
            visible: root.compact || !root.chartCollapsed
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: Theme.sp(10)

                Repeater {
                    model: root._plots
                    delegate: PpChartPlot {
                        id: plot
                        required property var modelData
                        // modelData is briefly undefined while the Repeater swaps models on a
                        // Split↔Overlay toggle — every binding below tolerates that.
                        readonly property var  plotSeries: modelData ? modelData.series : []
                        readonly property bool facet: modelData ? modelData.facet : false
                        readonly property var  facetSeries: (facet && plotSeries.length > 0) ? plotSeries[0] : null
                        // The facet's σ, resolved once here rather than inside facetEndText: that
                        // binding re-evaluates on every frame of a brush drag (it depends on the view
                        // window) and seriesSigma marshals the whole series.
                        readonly property real facetSigma: facetSeries ? cm.seriesSigma(facetSeries) : 0
                        readonly property var  rng: root._rangeFor(plotSeries)

                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        series:     plotSeries
                        phases:     root.phases
                        valueLo:    rng.lo
                        valueHi:    rng.hi
                        domStartUs: root.viewStartUs
                        domEndUs:   root.viewEndUs
                        impactUs:   root.impactUs
                        // "°" rather than "deg": the fallback now matches what _fmt prints for a
                        // unitless series, where the two spellings used to sit inches apart.
                        unitLabel:  cm.shortUnit((plotSeries.length > 0 && plotSeries[0].unit)
                                                 ? plotSeries[0].unit : "°")
                        playheadUs:    root.playheadUs
                        showPlayhead:  root.showPlayhead
                        showDots:      root.showDots
                        showCrosshair: root.showCursor
                        showSigmaBand: root.showSigmaBand      // dark; see root.showSigmaBand
                        cursorUs:      root.showCursor ? root._cursorUs : -1
                        yTickCount:    facet ? 3 : 6
                        showFrame:     facet
                        showXAxis:     modelData ? modelData.last : true
                        gutterLeft:    root._gutterLeft
                        padR:          root._padR
                        facetName:     facetSeries ? root._name(facetSeries) : ""
                        // Domain-clamped, mask-aware, and "—" on an emptied window — see
                        // root._facetEndText, which states why for all three.
                        facetEndText:  facetSeries ? root._facetEndText(facetSeries, plot.facetSigma) : ""

                        onHoverMoved: (t) => root._cursorUs =
                            Math.max(root.viewStartUs, Math.min(root.viewEndUs, t))
                        onHoverExited: root._cursorUs = -1

                        // Seek forwarding (host wires to shotReplay). Clamp to the
                        // active view window — the same window the plot is drawn over.
                        seekEnabled: root.seekable
                        onSeekRequested: (t) => root.seekRequested(
                            Math.max(root.viewStartUs, Math.min(root.viewEndUs, t)))
                        onScrubBegan: root.scrubBegan()
                        onScrubEnded: root.scrubEnded()
                    }
                }
            }

            // Shared hover tooltip (all visible series at the cursor).
            Rectangle {
                id: tip
                visible: root.showCursor && root._cursorUs >= 0 && root._visible.length > 0
                readonly property real cx: root._tooltipX(plotArea.width)
                x: Math.min(cx + Theme.sp(14), plotArea.width - width - Theme.sp(2))
                y: Theme.sp(4)
                width: tipCol.width + Theme.sp(20); height: tipCol.height + Theme.sp(16)
                color: Theme.colorSurface; radius: Theme.sp(8)
                border.width: 1; border.color: Theme.colorBorderStrong

                Column {
                    id: tipCol
                    x: Theme.sp(10); y: Theme.sp(8)
                    spacing: Theme.sp(3)
                    Text {
                        text: {
                            var ms = Math.round((root._cursorUs - root.impactUs) / 1000)
                            return (ms > 0 ? "+" : "") + ms + qsTr(" ms · impact")
                        }
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingLabel
                        color: Theme.colorText3
                    }
                    Repeater {
                        model: root._visible
                        delegate: Row {
                            id: trow
                            required property var modelData
                            // Resolved per ROW, not inside the readout binding below: that one
                            // re-evaluates on every cursor move, and seriesSigma marshals the whole
                            // series. This changes only when the data does.
                            readonly property real sig: cm.seriesSigma(trow.modelData)
                            spacing: Theme.sp(8)
                            Rectangle {
                                width: Theme.sp(8); height: Theme.sp(8); radius: Theme.sp(2)
                                anchors.verticalCenter: parent.verticalCenter
                                color: trow.modelData.color
                            }
                            Text {
                                width: Theme.sp(70)
                                text: root._name(trow.modelData)
                                elide: Text.ElideRight
                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzLabel
                                color: Theme.colorText2
                            }
                            Text {
                                // "—" where the nearest sample was bridged or lies outside the
                                // metric's domain — see root._valueTextAt.
                                text: root._valueTextAt(trow.modelData, root._cursorUs, trow.sig)
                                font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                                font.weight: Font.Medium
                                color: Theme.colorText
                            }
                        }
                    }
                }
            }
        }

        // Legend chips = toggle + live value / Δ-from-address readout at the playhead.
        Flow {
            visible: !root.compact && !root.chartCollapsed
            Layout.fillWidth: true
            spacing: Theme.sp(10)

            Repeater {
                // The preset's metrics — see _legendSeries. The colour rides on the model entry, so
                // it stays the series' full-list palette colour and matches its trace whatever the
                // preset is showing.
                model: root._legendSeries
                delegate: Row {
                    id: chip
                    required property var modelData
                    readonly property bool on: root._isOn(chip.modelData.key)
                    readonly property real val: labels.valueAtNearest(chip.modelData.t_us,
                                                                      chip.modelData.value, root._readoutUs)
                    // Resolved once per chip — it governs the digits of both readouts below, and a
                    // whole-series marshal per replay frame is exactly what ChartMetrics.seriesSigma
                    // must not be asked to do.
                    readonly property real sig: cm.seriesSigma(chip.modelData)
                    spacing: Theme.sp(4)
                    opacity: chip.on ? 1.0 : 0.4

                    Rectangle {
                        width: Theme.sp(8); height: Theme.sp(8); radius: Theme.sp(2)
                        anchors.verticalCenter: parent.verticalCenter
                        color: chip.on ? chip.modelData.color : Theme.colorText3
                    }
                    Text {
                        text: root._name(chip.modelData)
                        anchors.verticalCenter: parent.verticalCenter
                        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzLabel
                        color: Theme.colorText2
                    }
                    Text {
                        id: chipVal
                        // Same rule as the hover tooltip: this chip prints a number with a unit
                        // beside it at the playhead, so it makes the same claim and has to be
                        // able to withdraw it. The Δ-from-address goes with it — a difference
                        // against an unmeasured reading is not a smaller claim, it is the same
                        // one arithmetically disguised.
                        readonly property bool measured:
                            root._measuredAt(chip.modelData, root._readoutUs)
                        // Both halves take the SERIES' step, the Δ included. A difference of two
                        // readings each ±σ is strictly noisier than either (σ√2), so a finer step on
                        // the Δ would be the least defensible digit on the panel; and a Δ printed at
                        // a different coarseness from the value it is a difference of cannot be
                        // checked against it by eye. One step per series, everywhere.
                        text: chipVal.measured
                              ? cm.formatValue(chip.val, chip.modelData.unit, chip.sig)
                                + "  Δ" + cm.formatValue(chip.val - root._addrValue(chip.modelData),
                                                         chip.modelData.unit, chip.sig)
                              : "—"
                        anchors.verticalCenter: parent.verticalCenter
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        color: Theme.colorText3
                    }
                    TapHandler { onTapped: root._toggle(chip.modelData.key) }
                }
            }
        }

        // ── SUMMARY section ───────────────────────────────────────────────────────
        SectionHeader {
            visible: !root.compact
            title: qsTr("SUMMARY"); collapsed: root.summaryCollapsed
            onToggled: { root.summaryCollapsed = !root.summaryCollapsed
                         root._persistSection("summary", root.summaryCollapsed) }
        }

        // Per-window summary cards — recompute over the active view window. The section's
        // own header is suppressed; the SectionHeader above labels it.
        PpChartSummary {
            visible: !root.compact && !root.summaryCollapsed
            Layout.fillWidth: true
            showHeader:  false
            series:      root._visible
            startUs:     root.viewStartUs
            endUs:       root.viewEndUs
            impactUs:    root.impactUs
            segmentName: root._preset === "Full" ? qsTr("full swing")
                       : (labels.phaseFullName(root._nearStart) + " → "
                          + labels.phaseFullName(root._nearEnd)).toLowerCase()
        }

        // Trailing spacer — when CHART (the natural fillHeight item) is collapsed there is
        // nothing to absorb the leftover height, so the remaining sections would float; this
        // fills ONLY then, pinning them to the top. (Inert while the chart is expanded, so it
        // never competes with the plot for space.)
        Item { Layout.fillWidth: true; Layout.fillHeight: root.chartCollapsed && !root.compact }
    }

    // Empty state.
    Text {
        anchors.centerIn: parent
        visible: !root._hasAny
        text: qsTr("No analysis")
        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
        color: Theme.colorText3
    }
}
