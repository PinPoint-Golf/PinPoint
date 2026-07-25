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

// Resolves the session-stage layout (which panels are visible + how the centre
// stage packs them) per session MODE (Capture | Replay | Analyse), persisting to
// appSettings.viewLayoutByMode. Three orthogonal concerns: VISIBILITY (panel
// on/off), ARRANGEMENT (stage packing), and MOTION (the per-element overlay
// drawn on the camera panel — pose skeleton + club shaft + ball). There are no
// named panel/arrangement presets — every panel/arrangement edit just updates
// that mode's layout in place. Motion DOES have named presets (see below).
//
// Panels split into two homes:
//   stage panels — camera/charts/dashboard/table, arranged inside PpModeStage
//   rail panels  — timeline/carousel, full-width rails the screen shows/hides
//
// Storage shape (per mode, key = String(modeInt)):
//   viewLayoutByMode[k] = {
//       panels:      QStringList,
//       arrangement: "tabs"|"split"|"stage",
//       motion: {
//           on:          bool,                         // master overlay switch
//           preset:      string,                       // preset id, or "custom"
//           modes:       { arms, spine, shoulders, hips, legs, shaft, shaftGrip,
//                          ball, hands:
//                          "off"|"frame"|"fan"|"trace" },   // hands (WB4) default off
//           traceTarget: string                        // optional — only present
//                                                       // when the preset/edit
//                                                       // overrides an element's
//                                                       // default trace anchor
//       }
//   }
//
// Element default trace anchors (owned by the renderer, noted here for
// context): arms → leadWrist, spine → neckMid, shoulders → leadShoulder,
// hips → pelvisMid, legs → leadAnkle, shaft → clubhead, shaftGrip → grip,
// ball → none.
//
// Back-compat: older stores wrote `overlays: bool` instead of `motion`. Reads
// migrate that losslessly forever (see _rawMotion); the first subsequent write
// of that mode replaces `overlays` with a full `motion` object — the old key
// is never written back. `overlaysOn()`/`setOverlays()` remain as thin shims
// over `motionOn()`/`setMotionOn()` so pre-existing callers keep working.
//
// Capture hard guard: in Capture mode every element except "ball" always
// resolves to "off", regardless of what's stored — Capture never renders body
// or shaft motion. Enforced once, in motionFor() (elementMode() inherits it by
// calling motionFor()).
//
// The resolver functions read the notifying appSettings.viewLayoutByMode property,
// so QML binding dependency-tracking re-evaluates visible:/arrangement: bindings
// whenever the map changes — no extra change signal needed.

pragma Singleton
import QtQuick
import PinPointStudio

QtObject {
    id: vl

    // mode → sensible default layout (used until the user edits a mode's layout).
    function defaultLayout(mode) {
        switch (mode) {
            case SessionMode.replay:
                return { panels: ["camera", "charts", "timeline", "carousel"], arrangement: "split", motion: _defaultMotion(mode) }
            case SessionMode.analyse:
                // Analyse plays the loaded shot's video (same disk source as Replay)
                // alongside the deeper-analysis panels — camera included so playback
                // follows the Replay↔Analyse toggle.
                return { panels: ["camera", "charts", "table", "timeline", "carousel"], arrangement: "split", motion: _defaultMotion(mode) }
            default: // capture
                return { panels: ["camera", "carousel"], arrangement: "stage", motion: _defaultMotion(mode) }
        }
    }

    function _key(mode) { return String(mode) }

    function _layout(mode) {
        var m = appSettings.viewLayoutByMode, k = _key(mode)
        return (m && m[k] !== undefined) ? m[k] : defaultLayout(mode)
    }

    // ── motion preset catalogue ─────────────────────────────────────────────
    // Global, read-only. Every entry lists all element keys explicitly (shaftGrip
    // = the club's GRIP-end trace, companion to shaft = the CLUBHEAD-end trace).
    readonly property var _presets: [
        { id: "clean", label: "Clean", hint: "frame · body + club",
          modes: { arms: "frame", spine: "frame", shoulders: "frame", hips: "frame", legs: "off", shaft: "frame", shaftGrip: "off", ball: "off", hands: "off" } },
        { id: "ballOnly", label: "Ball only", hint: "frame · ball",
          modes: { arms: "off", spine: "off", shoulders: "off", hips: "off", legs: "off", shaft: "off", shaftGrip: "off", ball: "frame", hands: "off" } },
        { id: "clubLeadArm", label: "Club + lead arm", hint: "fan · club + lead arm",
          modes: { arms: "fan", spine: "off", shoulders: "off", hips: "off", legs: "off", shaft: "fan", shaftGrip: "off", ball: "off", hands: "off" } },
        { id: "clubTrack", label: "Club track", hint: "trace · club grip + head",
          modes: { arms: "off", spine: "off", shoulders: "off", hips: "off", legs: "off", shaft: "trace", shaftGrip: "trace", ball: "off", hands: "off" } },
        { id: "core", label: "Core", hint: "frame · spine + hips + shoulders",
          modes: { arms: "off", spine: "frame", shoulders: "frame", hips: "frame", legs: "off", shaft: "off", shaftGrip: "off", ball: "off", hands: "off" } },
        { id: "tracePelvis", label: "Trace pelvis", hint: "trace · pelvis",
          modes: { arms: "off", spine: "off", shoulders: "off", hips: "trace", legs: "off", shaft: "off", shaftGrip: "off", ball: "off", hands: "off" } },
        { id: "traceHead", label: "Trace head", hint: "trace · head",
          modes: { arms: "off", spine: "off", shoulders: "trace", hips: "off", legs: "off", shaft: "off", shaftGrip: "off", ball: "off", hands: "off" },
          traceTarget: "head" },
        { id: "traceLeadSh", label: "Trace lead shoulder", hint: "trace · lead shoulder",
          modes: { arms: "off", spine: "off", shoulders: "trace", hips: "off", legs: "off", shaft: "off", shaftGrip: "off", ball: "off", hands: "off" } }
    ]

    // Fresh copies only — callers must not be able to mutate the catalogue
    // through the returned reference.
    function presetCatalog() {
        return _presets.map(function (p) {
            var copy = { id: p.id, label: p.label, hint: p.hint, modes: _cloneModes(p.modes) }
            if (p.traceTarget !== undefined) copy.traceTarget = p.traceTarget
            return copy
        })
    }

    // ── resolution (read) ───────────────────────────────────────────────────
    function enabledKeysFor(mode) {
        return _layout(mode).panels || []
    }

    function isPanelOn(mode, key) {
        return enabledKeysFor(mode).indexOf(key) >= 0
    }

    function arrangementFor(mode) {
        return _layout(mode).arrangement || "split"
    }

    // Master on/off switch for the motion overlay (pose skeleton + club shaft + ball).
    function motionOn(mode) {
        return motionFor(mode).on
    }

    // Selected preset id for a mode ("custom" once the user hand-edits an element).
    function motionPreset(mode) {
        return motionFor(mode).preset
    }

    // The whole motion object for a mode: migrated, default-filled (never an
    // undefined field; modes always has all 7 keys), and Capture-hard-guarded —
    // in Capture every element except "ball" is forced "off" regardless of
    // stored state.
    function motionFor(mode) {
        var mo = _rawMotion(mode)
        if (mode !== SessionMode.capture) return mo

        var keys = ["arms", "spine", "shoulders", "hips", "legs", "shaft", "shaftGrip", "ball", "hands"]
        var modes = {}
        for (var i = 0; i < keys.length; i++)
            modes[keys[i]] = (keys[i] === "ball") ? mo.modes[keys[i]] : "off"
        var out = { on: mo.on, preset: mo.preset, modes: modes }
        if (mo.traceTarget !== undefined) out.traceTarget = mo.traceTarget
        return out
    }

    // Per-element render mode ("off"|"frame"|"fan"|"trace"); inherits the
    // Capture guard via motionFor().
    function elementMode(mode, key) {
        return motionFor(mode).modes[key]
    }

    // The active trace anchor override, or "" when the element/preset uses its
    // own default anchor (the renderer owns the per-element defaults).
    function motionTraceTarget(mode) {
        var mo = motionFor(mode)
        return mo.traceTarget !== undefined ? mo.traceTarget : ""
    }

    // Shim — pre-existing callers (PpViewPanel.qml, PpCameraTiles.qml, Main.qml)
    // keep working unchanged.
    function overlaysOn(mode) {
        return motionOn(mode)
    }

    // True when the CURRENT athlete is right-handed, i.e. the lead side is the
    // left side (kp5/7/9). Mirrors the ArmVizView.qml handedness precedent.
    function leadIsLeft() {
        return athleteController.currentHandedness !== "Left"
    }

    // ── mutation (write) ────────────────────────────────────────────────────
    function setPanel(mode, key, on) {
        var keys = enabledKeysFor(mode).slice()
        var i = keys.indexOf(key)
        if (on && i < 0) keys.push(key)
        else if (!on && i >= 0) keys.splice(i, 1)
        _write(mode, keys, arrangementFor(mode))
    }

    function setArrangement(mode, name) {
        _write(mode, enabledKeysFor(mode).slice(), name)
    }

    function setMotionOn(mode, on) {
        var mo = _rawMotion(mode)
        mo.on = on
        _write(mode, enabledKeysFor(mode).slice(), arrangementFor(mode), mo)
    }

    // Applies a named preset wholesale (on stays as-is; preset + modes + any
    // traceTarget come from the catalogue entry). Unknown id → no-op.
    function applyPreset(mode, id) {
        var p = _presetById(id)
        if (!p) return

        var mo = _rawMotion(mode)
        var out = { on: mo.on, preset: id, modes: _cloneModes(p.modes) }
        if (p.traceTarget !== undefined) out.traceTarget = p.traceTarget
        _write(mode, enabledKeysFor(mode).slice(), arrangementFor(mode), out)
    }

    // Hand-edits one element's mode. Flips preset to "custom" and drops any
    // traceTarget — a custom edit reverts every element to its default anchor.
    function setElementMode(mode, key, m) {
        var mo = _rawMotion(mode)
        var modes = _cloneModes(mo.modes)
        modes[key] = m
        _write(mode, enabledKeysFor(mode).slice(), arrangementFor(mode), { on: mo.on, preset: "custom", modes: modes })
    }

    // Shim — pre-existing callers keep working unchanged.
    function setOverlays(mode, on) {
        setMotionOn(mode, on)
    }

    // motion is optional — omitted by setPanel/setArrangement, so preserve the
    // mode's current value (migrating an old bool `overlays` entry into a full
    // `motion` object at that moment; the old key is not written back).
    function _write(mode, panels, arrangement, motion) {
        var m = _clone(appSettings.viewLayoutByMode)
        var mo = (motion !== undefined) ? motion : _rawMotion(mode)
        m[_key(mode)] = { panels: panels, arrangement: arrangement, motion: mo }
        appSettings.viewLayoutByMode = m
    }

    function _clone(m) {
        var out = {}
        if (m) for (var key in m) out[key] = m[key]
        return out
    }

    // ── motion internals ────────────────────────────────────────────────────
    // This mode's motion object, migrated from an old bool `overlays` entry
    // (forever) and default-filled against its own preset. Never mutates the
    // stored map.
    function _rawMotion(mode) {
        var l = _layout(mode)
        if (l.motion !== undefined) return _fillMotion(mode, l.motion)
        if (l.overlays !== undefined) return _defaultMotion(mode, l.overlays)
        return _defaultMotion(mode) // no motion, no legacy overlays key: today's default is on
    }

    // The default motion object for a mode: the given on-flag (default true),
    // the mode's default preset id, and that preset's modes/traceTarget.
    function _defaultMotion(mode, on) {
        var id = _modeDefaultPresetId(mode)
        var p = _presetById(id)
        var out = { on: (on !== undefined) ? on : true, preset: id, modes: _cloneModes(p.modes) }
        if (p.traceTarget !== undefined) out.traceTarget = p.traceTarget
        return out
    }

    function _modeDefaultPresetId(mode) {
        return (mode === SessionMode.capture) ? "ballOnly" : "clean"
    }

    function _presetById(id) {
        for (var i = 0; i < _presets.length; i++)
            if (_presets[i].id === id) return _presets[i]
        return undefined
    }

    // Default-fills a possibly-partial motion object (e.g. a future version's
    // write missing a field this version doesn't know about yet) so callers
    // never see an undefined field. Gaps are filled from the object's own
    // preset when it resolves, else the mode's default preset. Never mutates
    // the input.
    function _fillMotion(mode, motion) {
        var presetId = (motion.preset !== undefined) ? motion.preset : _modeDefaultPresetId(mode)
        var p = _presetById(presetId) || _presetById(_modeDefaultPresetId(mode))
        var out = {
            on: (motion.on !== undefined) ? motion.on : true,
            preset: presetId,
            modes: _fillModes(motion.modes, p.modes)
        }
        var tt = (motion.traceTarget !== undefined) ? motion.traceTarget : p.traceTarget
        if (tt !== undefined) out.traceTarget = tt
        return out
    }

    function _fillModes(modes, fallback) {
        var keys = ["arms", "spine", "shoulders", "hips", "legs", "shaft", "shaftGrip", "ball", "hands"]
        var out = {}
        for (var i = 0; i < keys.length; i++) {
            var key = keys[i]
            out[key] = (modes && modes[key] !== undefined) ? modes[key] : fallback[key]
        }
        return out
    }

    function _cloneModes(modes) {
        var out = {}
        for (var key in modes) out[key] = modes[key]
        return out
    }

    // ── configurable post-shot dashboard (per SessionController::Type) ─────────
    // A SEPARATE store from the per-mode panel layout above: the dashboard is
    // composed per session TYPE (Swing 0, Wrist 1, GRF 2, Coach 3), keyed by
    // String(typeInt) in appSettings.dashboardPresetsByType. It reuses the same
    // fork-to-"custom" idiom as motion: applyDashboardPreset copies a catalogue
    // entry wholesale; any hand-edit (zone toggle/reorder, metric pin) flips the
    // active id to the literal "custom". Empty perZoneMetrics[zone] = "auto".
    //
    //   dashboardPresetsByType[String(type)] = {
    //       active: "full"|"verdict"|"focus"|"custom"|"<savedName>",
    //       zones:  [zoneKey…],                       // ordered, enabled zones
    //       perZoneMetrics: { zoneKey: [metricKey…] },// pinned; [] = auto
    //       saved:  { "<name>": { zones, perZoneMetrics } }
    //   }

    // Built-in preset catalogue — read-only. Full lists every zone (each auto).
    readonly property var _dashboardPresets: [
        { id: "full",    label: "Full",    zones: ["verdict", "setup", "motion", "sequence"] },
        { id: "verdict", label: "Verdict", zones: ["verdict"] },
        { id: "focus",   label: "Focus",   zones: ["verdict", "motion"] }
    ]

    // Zone catalogue — canonical order + display labels for the editor.
    readonly property var _dashboardZoneDefs: [
        { key: "verdict",  label: "Verdict" },
        { key: "setup",    label: "Setup" },
        { key: "motion",   label: "Motion" },
        { key: "sequence", label: "Sequence" }
    ]

    function dashboardPresetCatalog() {
        return _dashboardPresets.map(function (p) {
            return { id: p.id, label: p.label, zones: p.zones.slice() }
        })
    }

    function dashboardZoneCatalog() {
        return _dashboardZoneDefs.map(function (z) { return { key: z.key, label: z.label } })
    }

    function _dashKey(type) { return String(type) }

    function _dashPresetById(id) {
        for (var i = 0; i < _dashboardPresets.length; i++)
            if (_dashboardPresets[i].id === id) return _dashboardPresets[i]
        return undefined
    }

    function _dashZoneOrder(key) {
        for (var i = 0; i < _dashboardZoneDefs.length; i++)
            if (_dashboardZoneDefs[i].key === key) return i
        return 99
    }

    function _cloneMetricMap(m) {
        var out = {}
        if (m) for (var key in m) out[key] = (m[key] || []).slice()
        return out
    }

    function _defaultDash() {
        var p = _dashPresetById("full")
        return { active: "full", zones: p.zones.slice(), perZoneMetrics: {}, saved: {} }
    }

    // Fully-resolved, default-filled dashboard object for a type (never mutates the store).
    function _dashRaw(type) {
        var m = appSettings.dashboardPresetsByType, k = _dashKey(type)
        var e = (m && m[k] !== undefined) ? m[k] : undefined
        if (e === undefined) return _defaultDash()
        return {
            active: (e.active !== undefined) ? e.active : "full",
            zones:  (e.zones !== undefined) ? e.zones.slice() : _dashPresetById("full").zones.slice(),
            perZoneMetrics: _cloneMetricMap(e.perZoneMetrics),
            saved:  _cloneObj(e.saved)
        }
    }

    function _cloneObj(o) {
        var out = {}
        if (o) for (var key in o) out[key] = o[key]
        return out
    }

    function _writeDash(type, obj) {
        var m = _clone(appSettings.dashboardPresetsByType)
        m[_dashKey(type)] = obj
        appSettings.dashboardPresetsByType = m
    }

    // ── dashboard resolution (read) ───────────────────────────────────────────
    function dashboardActive(type)             { return _dashRaw(type).active }
    function dashboardZones(type)              { return _dashRaw(type).zones }
    function isDashboardZoneOn(type, zone)     { return _dashRaw(type).zones.indexOf(zone) >= 0 }
    function dashboardZoneMetrics(type, zone)  {
        var pm = _dashRaw(type).perZoneMetrics
        return (pm && pm[zone] !== undefined) ? pm[zone].slice() : []
    }
    function dashboardSavedNames(type) {
        var s = _dashRaw(type).saved, out = []
        for (var name in s) out.push(name)
        return out
    }

    // ── dashboard mutation (write) ────────────────────────────────────────────
    // Applies a built-in or saved preset wholesale (active + zones + perZoneMetrics).
    function applyDashboardPreset(type, id) {
        var cur = _dashRaw(type)
        var p = _dashPresetById(id)
        if (p) {
            _writeDash(type, { active: id, zones: p.zones.slice(),
                               perZoneMetrics: {}, saved: cur.saved })
            return
        }
        var sv = cur.saved[id]
        if (sv !== undefined) {
            _writeDash(type, { active: id, zones: (sv.zones || []).slice(),
                               perZoneMetrics: _cloneMetricMap(sv.perZoneMetrics), saved: cur.saved })
        }
    }

    // Enable/disable one zone (canonical order preserved). Forks to "custom".
    function setDashboardZoneEnabled(type, zone, on) {
        var cur = _dashRaw(type)
        var zones = cur.zones.slice()
        var i = zones.indexOf(zone)
        if (on && i < 0) {
            zones.push(zone)
            zones.sort(function (a, b) { return _dashZoneOrder(a) - _dashZoneOrder(b) })
        } else if (!on && i >= 0) {
            zones.splice(i, 1)
        }
        _writeDash(type, { active: "custom", zones: zones,
                           perZoneMetrics: cur.perZoneMetrics, saved: cur.saved })
    }

    // Reorder zones to the given key list (validated). Forks to "custom".
    function reorderDashboardZones(type, keys) {
        var cur = _dashRaw(type)
        var valid = []
        for (var i = 0; i < keys.length; i++)
            if (_dashZoneOrder(keys[i]) < 99 && valid.indexOf(keys[i]) < 0) valid.push(keys[i])
        _writeDash(type, { active: "custom", zones: valid,
                           perZoneMetrics: cur.perZoneMetrics, saved: cur.saved })
    }

    // Pin/unpin one metric to a zone (empty list = auto). Forks to "custom".
    function pinDashboardMetric(type, zone, key, on) {
        var cur = _dashRaw(type)
        var pm = _cloneMetricMap(cur.perZoneMetrics)
        var list = (pm[zone] || []).slice()
        var i = list.indexOf(key)
        if (on && i < 0) list.push(key)
        else if (!on && i >= 0) list.splice(i, 1)
        if (list.length > 0) pm[zone] = list
        else delete pm[zone]
        _writeDash(type, { active: "custom", zones: cur.zones,
                           perZoneMetrics: pm, saved: cur.saved })
    }

    // Set a zone's EXPLICIT metric list wholesale (empty clears → auto default set).
    // Used by the editor to convert an auto zone to an explicit selection in one step.
    // Forks to "custom".
    function setDashboardZoneMetrics(type, zone, keys) {
        var cur = _dashRaw(type)
        var pm = _cloneMetricMap(cur.perZoneMetrics)
        if (keys && keys.length > 0) pm[zone] = keys.slice()
        else delete pm[zone]
        _writeDash(type, { active: "custom", zones: cur.zones,
                           perZoneMetrics: pm, saved: cur.saved })
    }

    // Promote the current custom config into a named, saved preset.
    function saveDashboardPreset(type, name) {
        if (!name || name.length === 0) return
        var cur = _dashRaw(type)
        var saved = _cloneObj(cur.saved)
        saved[name] = { zones: cur.zones.slice(), perZoneMetrics: _cloneMetricMap(cur.perZoneMetrics) }
        _writeDash(type, { active: name, zones: cur.zones,
                           perZoneMetrics: cur.perZoneMetrics, saved: saved })
    }
}
