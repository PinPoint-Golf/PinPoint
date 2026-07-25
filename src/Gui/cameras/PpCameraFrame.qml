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

// Reusable camera video frame — the one video view used by every rail screen
// (Wrist, swing analysis, the Play capture tab, …). Just the aspect-locked
// frame and its in-frame overlays; NO surrounding controls. Which overlays
// are active is configured per screen via the show* flags below — the same
// frame shows skeleton-only on the Wrist screen and the full hitting-area +
// ball chrome on capture screens.
//
// instance is nullable: a tile can be shown for a session-enabled camera
// before it is connected (dim "Not connected" placeholder) and subscribes to
// the instance's frame feed as soon as the instance arrives.

import QtQuick
import QtQuick.Shapes
import PinPointStudio
import QtMultimedia

Item {
    id: root

    // Live CameraInstance, or null while the camera is not connected.
    property QtObject instance: null

    // Replay-stream mode: when >= 0 this tile renders shotReplay stream N — the
    // reviewed swing's OWN video (disk-backed, source-agnostic) — instead of a
    // live CameraInstance. Default -1 = live-camera tile (instance path).
    property int replayStreamIndex: -1
    readonly property bool _isReplay: replayStreamIndex >= 0

    // Replay-overlay source — resolves to the disk replay (Review tile) or the
    // live in-window transient (Capture tile), so the analyzed skeleton/club
    // overlay reads ONE set of values regardless of which surface is driving it.
    readonly property bool _replayActive:     _isReplay ? shotReplay.active        : shotProcessor.isReplaying
    readonly property var  _replayDetail:     _isReplay ? shotReplay.analysisDetail : shotProcessor.replayAnalysisDetail
    readonly property real _replayPlayheadUs: _isReplay ? shotReplay.positionUs     : shotProcessor.replayPositionUs
    readonly property int  _replayPerspective: _isReplay
            ? (shotReplay.streams[replayStreamIndex] !== undefined
               ? shotReplay.streams[replayStreamIndex].perspective : -1)
            : (instance ? instance.perspective : -1)

    // Swing-window gate — during replay, analysis overlays draw ONLY while the
    // playhead is between the Address (phase 0) and Finish (phase 7) timeline
    // positions; the pre-swing setup/waggle and the post-Finish settling carry no
    // overlay. Phase timestamps come from the same source-agnostic detail the
    // overlay geometry does, in the shared window-relative µs domain as the
    // playhead (t0 subtracted), so no conversion is needed. If either boundary
    // phase is absent (a swing whose segmentation lacked them) the window is left
    // open — overlays behave exactly as before rather than vanishing entirely.
    function _phaseUs(idx) {
        var d = root._replayDetail
        var ph = (d && d.phases) ? d.phases : []
        for (var i = 0; i < ph.length; ++i)
            if (ph[i].phase === idx) return ph[i].t_us
        return -1
    }
    readonly property real _addressUs: _phaseUs(0)   // Phase::Address
    readonly property real _finishUs:  _phaseUs(7)   // Phase::Finish
    readonly property bool _inSwingWindow:
            (_addressUs < 0 || _finishUs < 0)
            || (_replayPlayheadUs >= _addressUs && _replayPlayheadUs <= _finishUs)

    // Camera name shown in-frame (muted overlay; also the placeholder title).
    property string displayName: ""

    // ── Per-screen overlay configuration ────────────────────────────────────
    property bool showPoseOverlay:      true   // skeleton canvas
    property bool showHittingArea:      true   // ROI overlay + detected ball circle
    property bool showHittingAreaHint:  false  // faint label-free ROI outline (live pose)
    property bool showBallOverlay:      false  // detected ball circle during live capture (honors live-pose)
    property bool roiEditable:          false  // enables the ROI drag-select interaction
    property bool showPerspectiveBadge: true
    property bool showStatsOverlay:     true   // resolution / fps
    property bool showReplayOverlay:    true   // analyzed skeleton + club shaft during replay
    property bool showPredictedShaft:   false  // R7 dev overlay: dashed ghost of the kinematic-model club
    property bool showPredictedEnvelope:false  // R7 dev overlay: faint ±k·σ_β kinematic cone (needs showPredictedShaft)
    // Layer C dev overlay (shaft_position_first §2): dim synthesized-tier ghost
    // + P-position badges. Property-level toggle only for this phase — the View
    // menu (ViewLayout) owns user-facing overlay toggles and gets wired to this
    // in a later UX pass, per the standing rule.
    property bool showSynthTier:        true
    property bool annotationsEnabled:   false  // telestrator overlay (analyse view only)

    // ── Motion overlay (per-element render modes) ─────────────────────────────
    // The analysed replay overlay renders each skeleton/club element in one of
    // four modes — off / frame / fan / trace — resolved by ViewLayout per session
    // mode and passed down by the host. motionOn is the master switch (mirrors the
    // legacy showReplayOverlay gate; wired to the same value). motionModes is the
    // {element: mode} map — an EMPTY map means "all frame" (legacy full-skeleton
    // draw). The LIVE skeletonCanvas is deliberately NOT gated by these: it stays
    // pixel-identical to the pre-motion live overlay (see skeletonCanvas.onPaint).
    property bool   motionOn: true
    property var    motionModes: ({})
    property string motionTraceTarget: ""      // "" ⇒ per-element default anchors
    property bool   leadIsLeft: true           // RH golfer ⇒ lead side is left (kp5/7/9)
    // Dev-only: swap the smoothed pose series back to raw detector frames for
    // frame/fan debugging (trace is always smoothed-only). No UI hook today —
    // flip in QML to inspect the raw detections. If a project-wide dev-overlay
    // AppSettings flag is added later, bind it here.
    property bool   showRawDetections: false
    // Fan-mode trailing window (ms of history before the playhead) and the cap on
    // lines drawn in that window (subsample stride bounds per-paint cost). The fan
    // reads the dense 240 Hz synth series (_clubFan), so this cap — not the source
    // frame rate — governs how many trail lines appear. Both are tuning candidates.
    readonly property int fanWindowMs: 240
    readonly property int kFanMaxFrames: 24

    // "shaft" traces the CLUBHEAD end; "shaftGrip" traces the GRIP end (both from
    // the same dense club series) — the "Club track" preset turns both on.
    readonly property var _traceElements: ["arms", "spine", "shoulders", "hips", "legs", "shaft", "shaftGrip"]

    // Per-element trace stroke. Body + clubhead ("shaft") use the accent; the grip
    // end uses a lighter accent so the club's two ends read as one instrument.
    function _traceColor(elem) {
        return elem === "shaftGrip" ? Qt.lighter(Theme.colorAccent, 1.7) : Theme.colorAccent
    }

    // Per-element render mode, master-gated. Empty map ⇒ "frame" (legacy full draw).
    function _elemMode(key) {
        if (!root.motionOn)
            return "off"
        var m = root.motionModes
        return (m && m[key]) ? m[key] : "frame"
    }

    // Normalized [x,y] anchor point for an element's trace at one smoothed frame,
    // or null to skip (tier Off / a missing derived-midpoint parent). `target` is
    // the explicit motionTraceTarget override, or "" for the element's default.
    function _traceAnchor(entry, elem, target, leadLeft) {
        var kp = entry.kp, tier = entry.tier
        function jtOff(j)  { return tier ? (tier[j] === 0) : (kp[j * 3 + 2] <= 0) }
        function pt(j)     { return jtOff(j) ? null : [kp[j * 3], kp[j * 3 + 1]] }
        function mid(a, b) { var pa = pt(a), pb = pt(b)
                             return (pa && pb) ? [(pa[0] + pb[0]) * 0.5, (pa[1] + pb[1]) * 0.5] : null }
        var tt = target
        if (tt === "") {
            switch (elem) {
                case "arms":      tt = "leadWrist";    break
                case "spine":     tt = "neckMid";      break
                case "shoulders": tt = "leadShoulder"; break
                case "hips":      tt = "pelvisMid";    break
                case "legs":      tt = "leadAnkle";    break
                default:          tt = "clubhead";     break
            }
        }
        switch (tt) {
            case "head": {
                // Ear-midpoint preferred — a wider, steadier baseline than the
                // nose alone (the same two points head_track.cpp's per-frame
                // head-scale computation favours) — nose is the fallback when
                // either ear drops below confidence (occlusion / DTL-ish turn).
                var earMid = mid(3, 4)
                return earMid ? earMid : pt(0)
            }
            case "leadShoulder": return pt(leadLeft ? 5 : 6)
            case "leadWrist":    return pt(leadLeft ? 9 : 10)
            case "leadAnkle":    return pt(leadLeft ? 15 : 16)
            case "neckMid":      return mid(5, 6)
            case "pelvisMid":    return mid(11, 12)
        }
        return null   // clubhead resolved by the shaft-sample path, not here
    }

    // Time-parameterised constant-velocity RTS (Rauch–Tung–Striebel) smoother for a
    // club-end trace polyline: a forward Kalman (CV / white-acceleration prior) plus
    // a backward smoothing pass, run independently on x(t) and y(t). Ported from the
    // eyeball-validated club-trace prototype with two production changes — RTS-only
    // (the Savitzky–Golay comparison arm is gone) and TIME parameterisation instead
    // of sample index. The club series (_clubFan) is dense 240 Hz synth between the
    // P-anchors but source-rate in the measured tails, so a per-step dt keeps the
    // smoothing time-constant honest across that seam. dt is normalized to the
    // series' MEDIAN interval, which makes the filter scale-free in time and reduces
    // EXACTLY to the validated index model on any uniform series (median ⇒ dtn ≈ 1),
    // while the sparser tails get a proportionally larger F(dt)/Q(dt) — less
    // over-smoothing across a real gap. `xs`,`ys`,`ts` are parallel arrays (ts = t_us
    // ascending); returns a list of smoothed Qt.point. `strength` fixes the
    // measurement/process-noise ratio (R = strength², process q = 1); the Kalman gain
    // depends only on that ratio, so the absolute normalized scale is irrelevant.
    // Viz-tier only — the trace is never read by any metric/scoring/estimand, so this
    // never touches a measured series (same discipline as club.synth / pose2d.synth).
    function _rtsSmoothClub(xs, ys, ts) {
        var n = xs.length
        if (n < 3) {
            var passthru = new Array(n)
            for (var q = 0; q < n; ++q) passthru[q] = Qt.point(xs[q], ys[q])
            return passthru
        }
        var strength = 7.0
        var R = strength * strength                     // meas. noise (process q = 1)
        // Per-step dt normalized to the median sample interval (dtn[0] unused — k=0
        // is the prior). Median ⇒ dtn ≈ 1 on a uniform series ⇒ the validated model.
        var intervals = new Array(n - 1)
        for (var k = 1; k < n; ++k) {
            var di = ts[k] - ts[k - 1]
            intervals[k - 1] = di > 0 ? di : 1
        }
        var sorted = intervals.slice().sort(function (a, b) { return a - b })
        var med = sorted[sorted.length >> 1]
        if (!(med > 0)) med = 1
        var dtn = new Array(n)
        for (var kk = 1; kk < n; ++kk) dtn[kk] = intervals[kk - 1] / med

        function smooth1d(z) {
            var xf0 = new Array(n), xf1 = new Array(n), xp0 = new Array(n), xp1 = new Array(n)
            var Pf00 = new Array(n), Pf01 = new Array(n), Pf10 = new Array(n), Pf11 = new Array(n)
            var Pp00 = new Array(n), Pp01 = new Array(n), Pp10 = new Array(n), Pp11 = new Array(n)
            var x0 = z[0], x1 = 0, P00 = R, P01 = 0, P10 = 0, P11 = R
            for (var i = 0; i < n; ++i) {
                var px0, px1, pp00, pp01, pp10, pp11
                if (i === 0) {                          // prior = predicted[0]
                    px0 = x0; px1 = x1; pp00 = P00; pp01 = P01; pp10 = P10; pp11 = P11
                } else {                                // predict: F = [[1,dt],[0,1]], white-accel Q(dt)
                    var dt = dtn[i], dt2 = dt * dt, dt3 = dt2 * dt
                    px0 = x0 + dt * x1; px1 = x1
                    pp00 = P00 + dt * (P01 + P10) + dt2 * P11 + dt3 / 3.0
                    pp01 = P01 + dt * P11 + 0.5 * dt2
                    pp10 = P10 + dt * P11 + 0.5 * dt2
                    pp11 = P11 + dt
                }
                xp0[i] = px0; xp1[i] = px1
                Pp00[i] = pp00; Pp01[i] = pp01; Pp10[i] = pp10; Pp11[i] = pp11
                var S = pp00 + R, K0 = pp00 / S, K1 = pp10 / S, innov = z[i] - px0
                x0 = px0 + K0 * innov; x1 = px1 + K1 * innov
                P00 = (1 - K0) * pp00; P01 = (1 - K0) * pp01
                P10 = -K1 * pp00 + pp10; P11 = -K1 * pp01 + pp11
                xf0[i] = x0; xf1[i] = x1
                Pf00[i] = P00; Pf01[i] = P01; Pf10[i] = P10; Pf11[i] = P11
            }
            var s0 = new Array(n), s1 = new Array(n)
            s0[n - 1] = xf0[n - 1]; s1[n - 1] = xf1[n - 1]
            for (var j = n - 2; j >= 0; --j) {
                var dtj = dtn[j + 1]                    // dt used to predict j+1 from j
                var A00 = Pf00[j] + dtj * Pf01[j], A01 = Pf01[j]
                var A10 = Pf10[j] + dtj * Pf11[j], A11 = Pf11[j]
                var d = Pp00[j + 1] * Pp11[j + 1] - Pp01[j + 1] * Pp10[j + 1]
                if (Math.abs(d) < 1e-12) { s0[j] = xf0[j]; s1[j] = xf1[j]; continue }
                var i00 = Pp11[j + 1] / d, i01 = -Pp01[j + 1] / d, i10 = -Pp10[j + 1] / d, i11 = Pp00[j + 1] / d
                var C00 = A00 * i00 + A01 * i10, C01 = A00 * i01 + A01 * i11
                var C10 = A10 * i00 + A11 * i10, C11 = A10 * i01 + A11 * i11
                var e0 = s0[j + 1] - xp0[j + 1], e1 = s1[j + 1] - xp1[j + 1]
                s0[j] = xf0[j] + C00 * e0 + C01 * e1
                s1[j] = xf1[j] + C10 * e0 + C11 * e1
            }
            return s0
        }
        var sx = smooth1d(xs), sy = smooth1d(ys)
        var out = new Array(n)
        for (var m = 0; m < n; ++m) out[m] = Qt.point(sx[m], sy[m])
        return out
    }

    // Normalized polyline for an element's trace from track start → playhead.
    // Body elements read the SMOOTHED series ONLY (absent ⇒ empty, matching the
    // panel greying); the club ends read the synth-preferring series (_clubFan) grip/
    // head points (conf>0) and are RTS/Kalman-smoothed (_rtsSmoothClub), so the traced
    // club path is both dense at 240 Hz and denoised rather than stepped/jittery at the
    // source frame rate. O(playhead index) — rebuilt on playhead/series/element change;
    // the bisect only locates the cut point, the polyline itself is inherently that long.
    function _traceNormPoints(elem) {
        var t = root._replayPlayheadUs
        var out = []
        var i, pi
        if (elem === "shaft" || elem === "shaftGrip") {
            // Both club ends trace the same dense club series — "shaft" the head
            // terminus, "shaftGrip" the grip terminus — then RTS/Kalman-smoothed.
            // Collect the conf-gated points with their timestamps so the smoother can
            // parameterise by real time (the series is non-uniform in the tails).
            var useGrip = (elem === "shaftGrip")
            var cs = replayOverlay._clubFan
            pi = replayOverlay._indexFor(cs, t)
            var cx = [], cy = [], ct = []
            for (i = 0; i <= pi; ++i) {
                var s = cs[i]
                if (!s || s.conf <= 0) continue
                cx.push(useGrip ? s.grip[0] : s.head[0])
                cy.push(useGrip ? s.grip[1] : s.head[1])
                ct.push(s.t_us)
            }
            return root._rtsSmoothClub(cx, cy, ct)
        }
        var sm = replayOverlay._smoothedDense
        pi = replayOverlay._indexFor(sm, t)
        for (i = 0; i <= pi; ++i) {
            var a = root._traceAnchor(sm[i], elem, root.motionTraceTarget, root.leadIsLeft)
            if (a) out.push(Qt.point(a[0], a[1]))
        }
        return out
    }

    // Skeleton edge definitions, shared by the live pose canvas and the
    // replay overlay — colours mapped to theme tokens. Left-body edges use
    // colorGood; right-body use colorAccent; mid-line connections use colorWarn.
    // "Biomech Blueprint" bones whose endpoints are both real keypoints (w =
    // width as a fraction of torso length). The neck bone (neck→nose) and the
    // spine (neck→pelvis) are anchored on derived midpoints and drawn separately
    // in paintBlueprint(). Mirrors kBones in src/Video/video_overlay_pose.cpp.
    readonly property var kBlueprintBones: [
        {a:5,  b:6,  w:0.037}, {a:11, b:12, w:0.037},   // shoulder + hip crossbars
        {a:5,  b:7,  w:0.049}, {a:7,  b:9,  w:0.032},   // left arm
        {a:6,  b:8,  w:0.049}, {a:8,  b:10, w:0.032},   // right arm
        {a:11, b:13, w:0.054}, {a:13, b:15, w:0.037},   // left leg
        {a:12, b:14, w:0.054}, {a:14, b:16, w:0.037},   // right leg
        // Feet (WB2/WB3, wholebody_pose_design.md §2.1): COCO-WholeBody indices
        // 17-22 — mirrors kBones in src/Video/video_overlay_pose.cpp exactly
        // (keep the two lists consistent). boneElem() below gives these their
        // OWN "feet" category (NOT "legs" — see that function's comment for
        // why) so they default ON. Drawn only when maxJoint (passed by the
        // caller) covers them — the live 17-kp array never does, so these are
        // inert there and only draw during replay over a widened (133-kp)
        // offline pose track.
        {a:15, b:19, w:0.026}, {a:19, b:17, w:0.026}, {a:17, b:18, w:0.026},   // left ankle-heel-bigtoe-smalltoe
        {a:16, b:22, w:0.026}, {a:22, b:20, w:0.026}, {a:20, b:21, w:0.026},   // right ankle-heel-bigtoe-smalltoe
        // Hands (WB4, wholebody_pose_design.md §2.2): COCO-WholeBody indices
        // 91-132 — mirrors kBones in src/Video/video_overlay_pose.cpp exactly.
        // boneElem() below gives these their OWN "hands" category, registered
        // "off" in every ViewLayout preset so hands default OFF (unlike feet).
        // Per hand: wrist-root → {thumb-CMC, index/middle/ring/pinky-MCP} + the
        // index→pinky knuckle line. Drawn only when maxJoint covers 91+ (a widened
        // replay track); the live 17-kp array never does, so these stay inert there.
        {a:91,  b:92,  w:0.018}, {a:91,  b:96,  w:0.018}, {a:91,  b:100, w:0.018},   // left wrist → thumb/index/middle-MCP
        {a:91,  b:104, w:0.018}, {a:91,  b:108, w:0.018}, {a:96,  b:108, w:0.018},   // left wrist → ring/pinky-MCP + knuckle line
        {a:112, b:113, w:0.018}, {a:112, b:117, w:0.018}, {a:112, b:121, w:0.018},   // right wrist → thumb/index/middle-MCP
        {a:112, b:125, w:0.018}, {a:112, b:129, w:0.018}, {a:117, b:129, w:0.018}    // right wrist → ring/pinky-MCP + knuckle line
    ]

    // Blueprint scale reference — torso length, with graceful fallbacks (1.5×
    // shoulder width, then 0.15× video height). Shared by the skeleton and the
    // club overlay so their stroke widths lock to one dimension. Returns the raw
    // scale (spine case unfloored so callers can apply their own collapse bail).
    function _blueprintScale(gx, gy, gs, cr) {
        var kMinScore = 0.25
        function vis(j) { return gs(j) >= kMinScore }
        if (vis(5) && vis(6) && vis(11) && vis(12)) {
            var nX = (gx(5)  + gx(6))  * 0.5, nY = (gy(5)  + gy(6))  * 0.5
            var pX = (gx(11) + gx(12)) * 0.5, pY = (gy(11) + gy(12)) * 0.5
            return Math.hypot(pX - nX, pY - nY)
        }
        if (vis(5) && vis(6))
            return Math.max(Math.hypot(gx(6) - gx(5), gy(6) - gy(5)) * 1.5, 20)
        return Math.max(0.15 * cr.height, 20)
    }

    // Shared pose-skeleton renderer — used by BOTH the live (skeletonCanvas) and
    // replay (replayOverlay) canvases so the two never drift. A faithful port of
    // VideoOverlayPose::drawSkeleton (src/Video/video_overlay_pose.cpp): face
    // keypoints dropped, derived neck/pelvis midpoints, graduated bone weights
    // scaled from torso length, the fixed cyan Theme.pose* palette. gx(j)/gy(j)
    // return pixel coords for keypoint j; gs(j) its score; alphaScale mutes the
    // whole overlay (1.0 live, <1 when it sits over replay footage). `maxJoint`
    // is how many keypoints the caller's gx/gy/gs can actually answer for — the
    // live tile's poseKeypoints is always exactly 17 objects (kps[j].score on an
    // out-of-range j throws, unlike a flat array's harmless `undefined`), while
    // the replay tile's flat kp array is 51 (legacy) or 399 (wholebody) floats
    // wide. Defaults to 17 (pre-WB2/3 behaviour) so any future call site that
    // forgets the argument degrades to "body only", never a crash.
    function paintBlueprint(ctx, cr, gx, gy, gs, alphaScale, emOn, maxJoint) {
        var kMinScore = 0.25
        var maxJ = (maxJoint === undefined) ? 17 : maxJoint
        function vis(j) { return j < maxJ && gs(j) >= kMinScore }
        // Per-element render gate — emOn(key) is true when that element should
        // draw in FRAME mode. When emOn is undefined the whole skeleton draws
        // (legacy / live path: pixel-identical to the pre-motion behaviour).
        function on(k) { return !emOn || emOn(k) }
        function boneElem(a, b) {
            if (a === 5 && b === 6)   return "shoulders"
            if (a === 11 && b === 12) return "hips"
            if ((a === 5 && b === 7) || (a === 7 && b === 9)
             || (a === 6 && b === 8) || (a === 8 && b === 10)) return "arms"
            // Feet (WB2/WB3) get their OWN category rather than falling into
            // "legs" — "legs" defaults to "off" in every existing preset
            // (ViewLayout.qml's "clean"/"ballOnly"/etc all list legs:"off"),
            // so reusing it would make feet default OFF, the opposite of the
            // design intent (wholebody_pose_design.md §4.4: "default OFF
            // except feet"). "feet" is deliberately NOT one of ViewLayout's
            // known element keys, so root._elemMode("feet") always falls
            // through its own "unknown key ⇒ frame" default — on whenever the
            // master motion switch is on, off only with it, and not yet
            // individually toggleable (same not-wired-to-UI-yet precedent as
            // showSynthTier above).
            // Hands (WB4) get their OWN "hands" category — checked BEFORE feet
            // (91 >= 17 would otherwise catch them). Unlike "feet", "hands" IS a
            // known ViewLayout element key, registered "off" in every preset so it
            // defaults OFF (wholebody_pose_design.md §4.4: "default OFF except
            // feet"). _elemMode("hands") therefore reads the preset — off unless
            // the user (later UI) turns it on, off entirely with the master switch.
            if (a >= 91 || b >= 91) return "hands"
            if (a >= 17 || b >= 17) return "feet"
            return "legs"   // 11-13, 13-15, 12-14, 14-16
        }
        function jointElem(j) {
            if (j === 5 || j === 6)   return "shoulders"
            if (j === 7 || j === 8 || j === 9 || j === 10) return "arms"
            if (j === 11 || j === 12) return "hips"
            return "legs"   // 13, 14, 15, 16
        }

        // Derived midpoints (visible only when both parents are).
        var neckVis   = vis(5)  && vis(6)
        var pelvisVis = vis(11) && vis(12)
        var nX = (gx(5)  + gx(6))  * 0.5, nY = (gy(5)  + gy(6))  * 0.5
        var pX = (gx(11) + gx(12)) * 0.5, pY = (gy(11) + gy(12)) * 0.5
        var neckScore = Math.min(gs(5), gs(6))

        // Scale reference: torso length, with graceful fallbacks (shoulder width,
        // then video height) — shared with the club overlay via _blueprintScale.
        // Bail on a collapsed spine.
        var haveSpine = neckVis && pelvisVis
        var scale = root._blueprintScale(gx, gy, gs, cr)
        if (haveSpine && scale < 20) return

        var cBone  = Qt.rgba(Theme.poseBone.r,        Theme.poseBone.g,        Theme.poseBone.b,        1)
        var cTop   = Qt.rgba(Theme.poseSpineTop.r,    Theme.poseSpineTop.g,    Theme.poseSpineTop.b,    1)
        var cBot   = Qt.rgba(Theme.poseSpineBottom.r, Theme.poseSpineBottom.g, Theme.poseSpineBottom.b, 1)
        var cTick2 = Qt.rgba(Theme.poseSpineTick2.r,  Theme.poseSpineTick2.g,  Theme.poseSpineTick2.b,  1)
        var cInk   = Qt.rgba(Theme.poseInk.r, Theme.poseInk.g, Theme.poseInk.b, Theme.poseInk.a)

        ctx.lineCap = "round"

        // 1. Faint torso sides — background structure, drawn first. (spine)
        if (on("spine")) {
            ctx.strokeStyle = cBone
            ctx.lineWidth   = 0.017 * scale
            ctx.globalAlpha = 0.28 * alphaScale
            if (vis(5) && vis(11)) { ctx.beginPath(); ctx.moveTo(gx(5), gy(5)); ctx.lineTo(gx(11), gy(11)); ctx.stroke() }
            if (vis(6) && vis(12)) { ctx.beginPath(); ctx.moveTo(gx(6), gy(6)); ctx.lineTo(gx(12), gy(12)); ctx.stroke() }
        }

        // 2. Bones — graduated width, confidence-driven alpha; gated per element.
        ctx.strokeStyle = cBone
        for (var i = 0; i < root.kBlueprintBones.length; ++i) {
            var b = root.kBlueprintBones[i]
            if (!vis(b.a) || !vis(b.b)) continue
            if (!on(boneElem(b.a, b.b))) continue
            ctx.globalAlpha = (0.4 + 0.6 * Math.min(gs(b.a), gs(b.b))) * alphaScale
            ctx.lineWidth   = b.w * scale
            ctx.beginPath(); ctx.moveTo(gx(b.a), gy(b.a)); ctx.lineTo(gx(b.b), gy(b.b)); ctx.stroke()
        }
        // Neck bone: derived neck → Nose. (shoulders)
        if (on("shoulders") && neckVis && vis(0)) {
            ctx.globalAlpha = (0.4 + 0.6 * Math.min(neckScore, gs(0))) * alphaScale
            ctx.lineWidth   = 0.037 * scale
            ctx.beginPath(); ctx.moveTo(nX, nY); ctx.lineTo(gx(0), gy(0)); ctx.stroke()
        }

        // 3. Spine — hero element — gradient pen + a tick at each end. (spine)
        if (on("spine") && haveSpine) {
            var grad = ctx.createLinearGradient(nX, nY, pX, pY)
            grad.addColorStop(0, cTop)
            grad.addColorStop(1, cBot)
            ctx.strokeStyle = grad
            ctx.globalAlpha = alphaScale
            ctx.lineWidth   = 0.068 * scale
            ctx.beginPath(); ctx.moveTo(nX, nY); ctx.lineTo(pX, pY); ctx.stroke()

            var dx = pX - nX, dy = pY - nY
            var len = Math.hypot(dx, dy)
            if (len > 1e-3) {
                var ux = -dy / len, uy = dx / len   // unit normal
                var half = 0.080 * scale
                ctx.lineWidth   = 0.037 * scale
                ctx.strokeStyle = cTop
                ctx.beginPath(); ctx.moveTo(nX - ux * half, nY - uy * half); ctx.lineTo(nX + ux * half, nY + uy * half); ctx.stroke()
                ctx.strokeStyle = cTick2
                ctx.beginPath(); ctx.moveTo(pX - ux * half, pY - uy * half); ctx.lineTo(pX + ux * half, pY + uy * half); ctx.stroke()
            }
        }

        // 4. Joints — concentric rings for body keypoints 5–16; shoulders/hips
        //    also get a solid centre dot. No ring on the nose.
        for (var j = 5; j <= 16; ++j) {
            if (!vis(j) || !on(jointElem(j))) continue
            var big = (j === 5 || j === 6 || j === 11 || j === 12)
            var r = (big ? 0.056 : 0.049) * scale
            ctx.globalAlpha = alphaScale
            ctx.fillStyle   = cInk
            ctx.beginPath(); ctx.arc(gx(j), gy(j), r, 0, Math.PI * 2); ctx.fill()
            ctx.globalAlpha = (0.5 + 0.5 * gs(j)) * alphaScale
            ctx.strokeStyle = cBone
            ctx.lineWidth   = 0.020 * scale
            ctx.beginPath(); ctx.arc(gx(j), gy(j), r, 0, Math.PI * 2); ctx.stroke()
            if (big) {
                ctx.globalAlpha = alphaScale
                ctx.fillStyle   = cBone
                ctx.beginPath(); ctx.arc(gx(j), gy(j), 0.017 * scale, 0, Math.PI * 2); ctx.fill()
            }
        }

        // 5. Head marker — the only head geometry: an unfilled ring on the Nose. (shoulders)
        if (on("shoulders") && vis(0)) {
            ctx.globalAlpha = (0.5 + 0.5 * gs(0)) * alphaScale
            ctx.strokeStyle = cBone
            ctx.lineWidth   = 0.025 * scale
            ctx.beginPath(); ctx.arc(gx(0), gy(0), 0.123 * scale, 0, Math.PI * 2); ctx.stroke()
        }

        // 6. Diamonds — filled markers on the derived midpoints, drawn last.
        ctx.globalAlpha = alphaScale
        ctx.fillStyle   = cTop
        var dd = 0.052 * scale
        if (on("shoulders") && neckVis) {
            ctx.beginPath(); ctx.moveTo(nX, nY - dd); ctx.lineTo(nX + dd, nY)
            ctx.lineTo(nX, nY + dd); ctx.lineTo(nX - dd, nY); ctx.closePath(); ctx.fill()
        }
        if (on("hips") && pelvisVis) {
            ctx.beginPath(); ctx.moveTo(pX, pY - dd); ctx.lineTo(pX + dd, pY)
            ctx.lineTo(pX, pY + dd); ctx.lineTo(pX - dd, pY); ctx.closePath(); ctx.fill()
        }

        ctx.globalAlpha = 1.0
    }

    // One skeleton element drawn for FAN mode (mouse-trail). `full` adds the joint
    // rings / diamonds / head ring (used only for the current playhead frame);
    // otherwise bones only — the decaying historical frames. `leadLeft` selects the
    // lead arm for the arms element (fan draws the lead arm only). Stroke styles and
    // widths mirror paintBlueprint so a fan's current frame reads like a frame-mode
    // element. The scale S is passed in (shared across the trail so widths don't
    // shimmer frame-to-frame). Used only by the replay overlay's fan pass.
    function _paintElem(ctx, cr, kp, elem, alpha, full, leadLeft, S) {
        var kMinScore = 0.25
        function px(j) { return kp[j * 3]     * cr.width  + cr.x }
        function py(j) { return kp[j * 3 + 1] * cr.height + cr.y }
        function ps(j) { return kp[j * 3 + 2] }
        function vis(j) { return ps(j) >= kMinScore }
        var cBone = Qt.rgba(Theme.poseBone.r, Theme.poseBone.g, Theme.poseBone.b, 1)
        var cInk  = Qt.rgba(Theme.poseInk.r, Theme.poseInk.g, Theme.poseInk.b, Theme.poseInk.a)
        var cTop  = Qt.rgba(Theme.poseSpineTop.r,    Theme.poseSpineTop.g,    Theme.poseSpineTop.b,    1)
        var cBot  = Qt.rgba(Theme.poseSpineBottom.r, Theme.poseSpineBottom.g, Theme.poseSpineBottom.b, 1)
        ctx.lineCap = "round"

        function bone(a, b, w) {
            if (!vis(a) || !vis(b)) return
            ctx.strokeStyle = cBone
            ctx.globalAlpha = (0.4 + 0.6 * Math.min(ps(a), ps(b))) * alpha
            ctx.lineWidth   = w * S
            ctx.beginPath(); ctx.moveTo(px(a), py(a)); ctx.lineTo(px(b), py(b)); ctx.stroke()
        }
        function node(j, big) {
            if (!vis(j)) return
            var r = (big ? 0.056 : 0.049) * S
            ctx.globalAlpha = alpha
            ctx.fillStyle   = cInk
            ctx.beginPath(); ctx.arc(px(j), py(j), r, 0, Math.PI * 2); ctx.fill()
            ctx.globalAlpha = (0.5 + 0.5 * ps(j)) * alpha
            ctx.strokeStyle = cBone
            ctx.lineWidth   = 0.020 * S
            ctx.beginPath(); ctx.arc(px(j), py(j), r, 0, Math.PI * 2); ctx.stroke()
            if (big) {
                ctx.globalAlpha = alpha
                ctx.fillStyle   = cBone
                ctx.beginPath(); ctx.arc(px(j), py(j), 0.017 * S, 0, Math.PI * 2); ctx.fill()
            }
        }
        function diamond(cx, cy) {
            var ddd = 0.052 * S
            ctx.globalAlpha = alpha
            ctx.fillStyle   = cTop
            ctx.beginPath(); ctx.moveTo(cx, cy - ddd); ctx.lineTo(cx + ddd, cy)
            ctx.lineTo(cx, cy + ddd); ctx.lineTo(cx - ddd, cy); ctx.closePath(); ctx.fill()
        }

        if (elem === "arms") {
            if (leadLeft) { bone(5, 7, 0.049); bone(7, 9, 0.032) }
            else          { bone(6, 8, 0.049); bone(8, 10, 0.032) }
            if (full) { node(leadLeft ? 7 : 8, false); node(leadLeft ? 9 : 10, false) }
        } else if (elem === "shoulders") {
            bone(5, 6, 0.037)
            if (vis(5) && vis(6) && vis(0)) {   // derived neck → nose
                var snx = (px(5) + px(6)) * 0.5, sny = (py(5) + py(6)) * 0.5
                ctx.strokeStyle = cBone
                ctx.globalAlpha = (0.4 + 0.6 * Math.min(Math.min(ps(5), ps(6)), ps(0))) * alpha
                ctx.lineWidth   = 0.037 * S
                ctx.beginPath(); ctx.moveTo(snx, sny); ctx.lineTo(px(0), py(0)); ctx.stroke()
            }
            if (full) {
                node(5, true); node(6, true)
                if (vis(0)) {
                    ctx.globalAlpha = (0.5 + 0.5 * ps(0)) * alpha
                    ctx.strokeStyle = cBone
                    ctx.lineWidth   = 0.025 * S
                    ctx.beginPath(); ctx.arc(px(0), py(0), 0.123 * S, 0, Math.PI * 2); ctx.stroke()
                }
                if (vis(5) && vis(6)) diamond((px(5) + px(6)) * 0.5, (py(5) + py(6)) * 0.5)
            }
        } else if (elem === "spine") {
            if (vis(5) && vis(6) && vis(11) && vis(12)) {
                var sX = (px(5) + px(6)) * 0.5,   sY = (py(5) + py(6)) * 0.5
                var pX = (px(11) + px(12)) * 0.5, pY = (py(11) + py(12)) * 0.5
                var grad = ctx.createLinearGradient(sX, sY, pX, pY)
                grad.addColorStop(0, cTop); grad.addColorStop(1, cBot)
                ctx.strokeStyle = grad
                ctx.globalAlpha = alpha
                ctx.lineWidth   = 0.068 * S
                ctx.beginPath(); ctx.moveTo(sX, sY); ctx.lineTo(pX, pY); ctx.stroke()
                if (full) {   // faint torso sides accompany the current-frame spine
                    ctx.strokeStyle = cBone
                    ctx.globalAlpha = 0.28 * alpha
                    ctx.lineWidth   = 0.017 * S
                    if (vis(5) && vis(11)) { ctx.beginPath(); ctx.moveTo(px(5), py(5)); ctx.lineTo(px(11), py(11)); ctx.stroke() }
                    if (vis(6) && vis(12)) { ctx.beginPath(); ctx.moveTo(px(6), py(6)); ctx.lineTo(px(12), py(12)); ctx.stroke() }
                }
            }
        } else if (elem === "hips") {
            bone(11, 12, 0.037)
            if (full) {
                node(11, true); node(12, true)
                if (vis(11) && vis(12)) diamond((px(11) + px(12)) * 0.5, (py(11) + py(12)) * 0.5)
            }
        } else if (elem === "legs") {
            bone(11, 13, 0.054); bone(13, 15, 0.037)
            bone(12, 14, 0.054); bone(14, 16, 0.037)
            if (full) { node(13, false); node(15, false); node(14, false); node(16, false) }
        }
        ctx.globalAlpha = 1.0
    }

    // ROI editor drag state (internal — driven by the unified MouseArea
    // below). roiDragging is true only while a NEW rectangle is being drawn
    // (rubber band); move/resize manipulate the committed rect live.
    property point roiDragStart:  Qt.point(0, 0)
    property point roiDragEnd:    Qt.point(0, 0)
    property bool  roiDragging:   false

    readonly property bool roiIsSet: instance !== null
                                     && instance.roi.width > 0 && instance.roi.height > 0

    // The video item's inset inside the frame border. Every overlay maps
    // video coordinates through this ONE value — hardcoded 2s drift from
    // Theme.sp(2) as soon as the font scale leaves 1.0.
    readonly property real videoInset: Theme.sp(2)

    // Pre-connect placeholder aspect — hosts with a camera-list entry pass
    // its crop-aware initialWidth/initialHeight so a disconnected tile
    // already opens at the aspect the stream will have once connected.
    property real placeholderAspect: 16.0 / 9.0

    // Video aspect ratio — used to centre the frame rect within the allocated
    // slot. Falls back to placeholderAspect while no instance/frame exists.
    readonly property real videoAspect: (instance && instance.frameWidth > 0
                                                     && instance.frameHeight > 0)
                                         ? instance.frameWidth / instance.frameHeight
                                         : placeholderAspect

    // The on-screen video rectangle (x,y,w,h in frame pixels) — the ONE mapping
    // every overlay uses to place normalized [0..1] source coordinates. Accounts
    // for the video inset and the letterboxing (PreserveAspectFit) inside it. The
    // skeleton / club canvases recompute this inline at paint time; the annotation
    // layer binds to it so it reflows the marks live on resize.
    readonly property bool _useBayer: instance !== null && instance.needsDebayer
    readonly property rect contentRect: _useBayer
        ? Qt.rect(videoInset, videoInset, bayerView.width, bayerView.height)
        : Qt.rect(videoInset + videoOut.contentRect.x,
                  videoInset + videoOut.contentRect.y,
                  videoOut.contentRect.width, videoOut.contentRect.height)

    // ── Frame subscription ───────────────────────────────────────────────────
    // The CameraInstance publishes every display/replay frame to all subscribed
    // views, so any number of frames can show the same camera at once (screens
    // live side by side in a StackLayout, all instantiated). Subscribe when the
    // instance arrives, unsubscribe from the old one on reassignment and on
    // destruction. _subscribed tracks the instance we registered with; it goes
    // null automatically if that instance is destroyed (QML QObject tracking),
    // and the C++ side's QPointer list prunes our sinks in that case too.
    property QtObject _subscribed: null
    function _syncSubscription() {
        if (root._subscribed === root.instance)
            return
        if (root._subscribed) {
            root._subscribed.removeVideoSink(videoOut.videoSink)
            root._subscribed.removeBayerItem(bayerView)
        }
        root._subscribed = root.instance
        if (root._subscribed) {
            root._subscribed.addVideoSink(videoOut.videoSink)
            root._subscribed.addBayerItem(bayerView)
        }
    }
    // Replay tiles must keep the disk-replay player bound to THIS tile's live video
    // sink. A one-shot bind in onCompleted is not enough: the player is recreated on
    // every load(), and tiles get churned by mode/layout transitions — so we also
    // rebind when the replay (re)starts and when our stream index resolves.
    // Qt.callLater coalesces the triggers and never fires for a tile that has already
    // been destroyed, so the surviving (visible) tile is the one that ends up bound.
    function _bindReplaySink() {
        if (root._isReplay && videoOut.videoSink)
            shotReplay.setVideoSink(root.replayStreamIndex, videoOut.videoSink)
    }
    Component.onCompleted: {
        _syncSubscription()
        if (root._isReplay)
            Qt.callLater(root._bindReplaySink)
    }
    onInstanceChanged:          _syncSubscription()
    onReplayStreamIndexChanged: if (root._isReplay) Qt.callLater(root._bindReplaySink)

    Connections {
        target: shotReplay
        enabled: root._isReplay
        // Every (re)load creates a fresh player that needs this tile's sink.
        function onActiveChanged() { if (shotReplay.active) Qt.callLater(root._bindReplaySink) }
    }
    Component.onDestruction: {
        if (root._subscribed) {
            root._subscribed.removeVideoSink(videoOut.videoSink)
            root._subscribed.removeBayerItem(bayerView)
        }
    }

    Rectangle {
        id: frameRect
        anchors.centerIn: parent
        width:  Math.min(parent.width, parent.height * root.videoAspect)
        height: width / root.videoAspect
        color: Theme.colorBg2
        radius: Theme.radius
        border.width: 1
        border.color: Theme.colorBorderMid

        VideoOutput {
            id: videoOut
            anchors.fill: parent
            anchors.margins: root.videoInset
            fillMode: VideoOutput.PreserveAspectFit
            visible: root._isReplay || (root.instance !== null && !root.instance.needsDebayer)
        }

        BayerVideoItem {
            id: bayerView
            anchors.fill: parent
            anchors.margins: root.videoInset
            visible: root.instance !== null && root.instance.needsDebayer
        }

        // ── Placeholder states ────────────────────────────────────────────
        // Not connected: dim frame with the camera name; the layout doesn't
        // jump when video starts. Connected but idle: "No camera feed".
        // (Replay tiles never show this — their video is disk-backed.)
        Column {
            anchors.centerIn: parent
            spacing: Theme.sp(4)
            visible: root.instance === null && !root._isReplay
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: root.displayName !== ""
                text: root.displayName
                color: Theme.colorText2
                font.family: Theme.fontBody
                font.pixelSize: Theme.fontSzBody
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Not connected")
                color: Theme.colorText3
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingData
            }
        }

        Text {
            anchors.centerIn: parent
            visible: root.instance !== null && !root.instance.isRecording
            text: qsTr("No camera feed")
            color: Theme.colorText3
            font.family: Theme.fontBody
            font.pixelSize: Theme.fontSzBody
        }

        // ── Camera name (muted, bottom-left, only while connected) ─────────
        Text {
            visible: root.instance !== null && root.displayName !== ""
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.margins: Theme.sp(8)
            text: root.displayName
            color: Theme.colorText3
            font.family: Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingData
        }

        // ── Perspective badge (top-left overlay) ──────────────────────────
        Rectangle {
            visible: root.showPerspectiveBadge
                     && root.instance !== null
                     && root.instance.perspective !== CameraInstance.None
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: Theme.sp(8)
            width: perspBadgeText.implicitWidth + Theme.sp(10)
            height: Theme.sp(20)
            radius: Theme.radius
            color: Theme.colorAccentMid
            border.width: 1
            border.color: Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b, 0.4)

            Text {
                id: perspBadgeText
                anchors.centerIn: parent
                text: !root.instance ? ""
                    : root.instance.perspective === CameraInstance.DownTheLine ? "DTL"
                    : root.instance.perspective === CameraInstance.FaceOn ? "Face On"
                    : "Other"
                color: Theme.colorAccent
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                font.weight: Font.Normal
                font.letterSpacing: Theme.trackingData
            }
        }

        // ── Resolution / FPS overlay (bottom-right) ──────────────────────
        Rectangle {
            visible: root.showStatsOverlay && root.instance !== null
                     && root.instance.isRecording && root.instance.frameWidth > 0
            anchors.bottom: parent.bottom
            anchors.right:  parent.right
            anchors.margins: Theme.sp(8)
            width:  resLabel.implicitWidth + Theme.sp(10)
            height: Theme.sp(18)
            radius: Theme.radius - 1
            color: Qt.rgba(0, 0, 0, 0.55)

            Text {
                id: resLabel
                anchors.centerIn: parent
                text: !root.instance ? ""
                    : root.instance.frameWidth + "x"
                      + root.instance.frameHeight + "  "
                      + (root.instance.configuredFps > 0
                             ? root.instance.configuredFps
                             : root.instance.cameraFps).toFixed(0) + " fps"
                color: Theme.colorText
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
            }
        }

        // ── Skeleton overlay ──────────────────────────────────────────────
        Canvas {
            id: skeletonCanvas
            anchors.fill: parent
            // poseEnabled: hide immediately when live pose detection is toggled
            // off — don't wait for the cleared-keypoints repaint.
            visible: root.showPoseOverlay
                     && root.instance !== null && root.instance.isRecording
                     && root.instance.poseEnabled

            Connections {
                target: root.instance
                function onPoseKeypointsChanged() { skeletonCanvas.requestPaint() }
            }
            onVisibleChanged: if (visible) requestPaint()

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)

                if (!root.instance)
                    return
                var kps = root.instance.poseKeypoints
                if (!kps || kps.length < 17)
                    return

                // Both branches include the video item's inset inside the
                // frame: the canvas fills the FRAME, while contentRect /
                // bayerView dims are in the inset video item's space.
                var cr = root.instance.needsDebayer
                    ? Qt.rect(root.videoInset, root.videoInset,
                              bayerView.width, bayerView.height)
                    : Qt.rect(root.videoInset + videoOut.contentRect.x,
                              root.videoInset + videoOut.contentRect.y,
                              videoOut.contentRect.width,
                              videoOut.contentRect.height)
                if (cr.width <= 0 || cr.height <= 0)
                    return

                // Biomech Blueprint — full strength on the live tile. The live
                // poseKeypoints array is always exactly 17 objects (the live
                // 60 Hz MoveNet contract never widens), so maxJoint = kps.length
                // keeps the foot bones permanently inert here — kps[17] on this
                // object-array shape would throw, unlike a flat array's harmless
                // out-of-range `undefined`, which is exactly why paintBlueprint's
                // vis() bounds-checks against maxJoint before ever calling gs(j).
                var gx = function(j) { return kps[j].x * cr.width  + cr.x }
                var gy = function(j) { return kps[j].y * cr.height + cr.y }
                var gs = function(j) { return kps[j].score }
                root.paintBlueprint(ctx, cr, gx, gy, gs, 1.0, undefined, kps.length)
            }
        }

        // ── Replay overlay: analyzed skeleton + club shaft ────────────────
        // Drawn from the shot's analyzed detail (offline ViTPose pose2d + the
        // ShaftTracker club track), scrubbing with the replay playhead. The
        // live skeletonCanvas is recording-gated, so the two never co-draw.
        // Face-on tiles only — the tracks were measured on that camera.
        Canvas {
            id: replayOverlay
            anchors.fill: parent
            z: 21
            visible: root.showReplayOverlay
                     && root._replayActive
                     && root._replayPerspective === 2
                     && root._inSwingWindow
                     && (_poseFrames.length > 0 || _clubSamples.length > 0
                         || _ballSamples.length > 0)

            // Bound from the active replay's detail — pose kp flat [x,y,c]×17 and
            // club samples with normalized grip/head (toAnalysisDetail shapes).
            // root._replayDetail resolves to disk (Review) or in-window (Capture).
            // Smoothed-first: the Phase-2 smoothed series carries the render-alpha
            // contract (bridged points ≥0.5) and kills detector jitter; fall back to
            // raw detector frames on old swings (no smoothed) or when the dev raw
            // toggle is set. Same {t_us, kp[51]} shape either way — the frame-mode
            // painter is unchanged. Drives the FRAME pass.
            readonly property var _poseFrames: {
                var d = root._replayDetail
                if (!root.showRawDetections && d && d.pose2d && d.pose2d.smoothed && d.pose2d.smoothed.length)
                    return d.pose2d.smoothed
                return (d && d.pose2d && d.pose2d.frames) ? d.pose2d.frames : []
            }
            // Smoothed-ONLY series — the BODY fan and trace read this (no raw
            // fallback: a jittery trail/trace is the artefact this feature removes).
            // Empty on old swings without a smoothed series ⇒ body fan/trace draw
            // nothing (shaft fan/trace still work — club samples always exist).
            readonly property var _smoothed: {
                var d = root._replayDetail
                return (d && d.pose2d && d.pose2d.smoothed && d.pose2d.smoothed.length) ? d.pose2d.smoothed : []
            }
            // Dense synthesized pose tier (pose_synthesis.h) — the smoothed skeleton
            // temporally upsampled to 240 Hz for smooth replay scrub. Same flat
            // { t_us, kp[x,y,c]×133 } shape as `smoothed` minus tier/sigma (Off joints
            // carry conf 0, which the overlays' conf-gate skips). Viz-only (metrics
            // read frames/smoothed); empty on swings analysed before this tier.
            readonly property var _poseSynth: {
                var d = root._replayDetail
                return (d && d.pose2d && d.pose2d.synth && d.pose2d.synth.length) ? d.pose2d.synth : []
            }
            // Dense-preferring series the BODY overlays consume: the synth tier when
            // present, else the existing series. FRAME keeps its raw fallback (and
            // yields to the dev raw toggle); FAN/TRACE keep smoothed-only (no raw), so
            // a swing without the synth tier renders exactly as before.
            readonly property var _poseFramesDense: (!root.showRawDetections && _poseSynth.length) ? _poseSynth : _poseFrames
            readonly property var _smoothedDense:   _poseSynth.length ? _poseSynth : _smoothed
            readonly property var _clubSamples: {
                var d = root._replayDetail
                return (d && d.club && d.club.valid && d.club.samples) ? d.club.samples : []
            }
            // Ball samples ({t_us,x,y,r,conf,found}, normalized 0..1) — scrubs with
            // the playhead like the club shaft; drawn only on found frames.
            readonly property var _ballSamples: {
                var d = root._replayDetail
                return (d && d.ball && d.ball.samples) ? d.ball.samples : []
            }
            // R7 predicted (pure R6 model) series — drawn as a dashed ghost.
            readonly property var _clubPredicted: {
                var d = root._replayDetail
                return (d && d.club && d.club.predicted) ? d.club.predicted : []
            }
            // Layer C synthesized tier (shaft_position_first §2C) — kinematic
            // boundary-value fit interpolated between P anchors, each flagged
            // ShaftSynthesized (0x100). Same shape as `samples` minus lineConf;
            // absent/empty on pre-v3.5 swings and when synth extraction is off.
            readonly property var _clubSynth: {
                var d = root._replayDetail
                return (d && d.club && d.club.synth) ? d.club.synth : []
            }
            // Fan visualization series: the dense 240 Hz synth tier. Metrics never
            // read this (synth is ShaftSynthesized-flagged and excluded from all
            // scoring/estimands — the real per-frame track stays in `samples`), so
            // the fan just shows synth with no measured/synth distinction. Measured
            // samples are appended ONLY outside the synth time-span (before the first
            // / after the last P-anchor, where synth doesn't exist by construction)
            // so the ends of the replay don't go blank. Empty synth (feature off /
            // pre-v3.5 swing) ⇒ plain measured samples, i.e. the legacy fan. Derived
            // — recomputed on club-data change, not per paint.
            readonly property var _clubFan: {
                var syn = _clubSynth
                if (syn.length === 0) return _clubSamples
                var t0 = syn[0].t_us, t1 = syn[syn.length - 1].t_us
                var out = []
                var meas = _clubSamples
                for (var i = 0; i < meas.length; ++i)
                    if (meas[i] && (meas[i].t_us < t0 || meas[i].t_us > t1)) out.push(meas[i])
                for (var s = 0; s < syn.length; ++s) out.push(syn[s])
                out.sort(function(a, b) { return a.t_us - b.t_us })
                return out
            }
            // Coaching P-positions P1–P8 (shaft_position_first §2B) — grip/head
            // normalized like `samples`; absent/empty on pre-v3.5 swings and when
            // position extraction is off.
            readonly property var _clubPositions: {
                var d = root._replayDetail
                return (d && d.club && d.club.positions) ? d.club.positions : []
            }
            readonly property int kTrail: 10

            // Greatest index with t_us <= t (−1 when empty).
            function _indexFor(arr, t) {
                var hi = arr.length - 1
                if (hi < 0 || t < arr[0].t_us) return hi < 0 ? -1 : 0
                if (t >= arr[hi].t_us) return hi
                var lo = 0
                while (hi - lo > 1) {
                    var mid = (lo + hi) >> 1
                    if (arr[mid].t_us <= t) lo = mid; else hi = mid
                }
                return lo
            }

            // Clamp grip→head to the content rect (Liang–Barsky, head-side exit
            // only — grip is checked inside cr first, so the entry t is always 0).
            // Returns the clamped [x,y], or null when grip itself is outside cr —
            // an honest overlay never draws a segment anchored off-frame.
            function _clampHeadToRect(gx, gy, hx, hy, cr) {
                if (gx < cr.x || gx > cr.x + cr.width || gy < cr.y || gy > cr.y + cr.height)
                    return null
                var dx = hx - gx, dy = hy - gy
                var tMax = 1
                var p = [-dx, dx, -dy, dy]
                var q = [gx - cr.x, cr.x + cr.width - gx, gy - cr.y, cr.y + cr.height - gy]
                for (var i = 0; i < 4; ++i) {
                    if (p[i] <= 0) continue    // parallel, or the entering half — grip is inside
                    var r = q[i] / p[i]
                    if (r < tMax) tMax = r
                }
                return [gx + dx * tMax, gy + dy * tMax]
            }

            // Repaint as the playhead advances — from whichever surface drives it.
            Connections {
                target: shotProcessor
                enabled: !root._isReplay
                function onReplayPositionChanged() {
                    if (replayOverlay.visible) replayOverlay.requestPaint()
                }
            }
            Connections {
                target: shotReplay
                enabled: root._isReplay
                function onPositionChanged() {
                    if (replayOverlay.visible) replayOverlay.requestPaint()
                }
            }
            onVisibleChanged: if (visible) requestPaint()
            on_PoseFramesChanged:  if (visible) requestPaint()
            on_SmoothedChanged:    if (visible) requestPaint()
            on_ClubSamplesChanged: if (visible) requestPaint()
            on_BallSamplesChanged: if (visible) requestPaint()
            on_ClubSynthChanged:     if (visible) requestPaint()
            on_ClubPositionsChanged: if (visible) requestPaint()

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                // Live tile needs an instance; replay tiles paint over videoOut.
                if (!root._isReplay && !root.instance)
                    return
                var cr = (!root._isReplay && root.instance.needsDebayer)
                    ? Qt.rect(root.videoInset, root.videoInset,
                              bayerView.width, bayerView.height)
                    : Qt.rect(root.videoInset + videoOut.contentRect.x,
                              root.videoInset + videoOut.contentRect.y,
                              videoOut.contentRect.width,
                              videoOut.contentRect.height)
                if (cr.width <= 0 || cr.height <= 0)
                    return
                var t = root._replayPlayheadUs
                var cGood   = Qt.rgba(Theme.colorGood.r,   Theme.colorGood.g,   Theme.colorGood.b,   1)
                var cAccent = Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b, 1)
                // Accent "Biomech Blueprint" palette for the club — derived from
                // the theme accent (reskins per aesthetic); node ink shared with
                // the skeleton so club and body nodes read as one instrument.
                var cClub    = cAccent
                var cClubHi  = Qt.lighter(Theme.colorAccent, 1.35)
                var cClubInk = Qt.rgba(Theme.poseInk.r, Theme.poseInk.g, Theme.poseInk.b, Theme.poseInk.a)
                var clubMute = 0.9

                // Frame-mode pose index + the shared club/fan scale (torso
                // reference; fall back to frame height when the pose is absent/frozen
                // at this playhead). Hoisted so every club-family tier and the fan
                // pass lock their stroke widths to one dimension.
                var pi = _indexFor(_poseFramesDense, t)
                var S
                if (pi >= 0) {
                    var pkp = _poseFramesDense[pi].kp
                    S = root._blueprintScale(
                            function(j) { return pkp[j * 3]     * cr.width  + cr.x },
                            function(j) { return pkp[j * 3 + 1] * cr.height + cr.y },
                            function(j) { return pkp[j * 3 + 2] }, cr)
                } else {
                    S = Math.max(0.15 * cr.height, 20)
                }
                var shaftMode = root._elemMode("shaft")

                // FRAME pass — Biomech Blueprint, muted (sits over footage); only the
                // body elements in "frame" mode draw. The mask returns true for every
                // element when motionModes is empty ⇒ legacy full skeleton.
                if (pi >= 0) {
                    var kp = _poseFramesDense[pi].kp
                    var gx = function(j) { return kp[j * 3]     * cr.width  + cr.x }
                    var gy = function(j) { return kp[j * 3 + 1] * cr.height + cr.y }
                    var gs = function(j) { return kp[j * 3 + 2] }
                    // maxJoint = the flat kp array's own keypoint count — 17 on a
                    // legacy pre-WB0 track (foot bones stay inert, matching the
                    // live tile) or 133 on a wholebody track (foot bones eligible,
                    // still per-endpoint score-gated by vis()).
                    root.paintBlueprint(ctx, cr, gx, gy, gs, 0.7,
                                        function(key) { return root._elemMode(key) === "frame" },
                                        Math.floor(kp.length / 3))
                }

                // FAN pass — body elements in "fan" mode: the last fanWindowMs of the
                // SMOOTHED series before the playhead as a decaying mouse-trail (oldest
                // faintest), then the current frame full. Subsampled to ≤ kFanMaxFrames
                // so per-paint cost stays bounded at high capture fps. arms fans the
                // LEAD arm only; the other body groups fan their bones.
                var sm = _smoothedDense
                if (sm.length > 0) {
                    var eFi = _indexFor(sm, t)
                    if (eFi >= 0) {
                        var sFi = _indexFor(sm, t - root.fanWindowMs * 1000)
                        if (sFi < 0) sFi = 0
                        var fspan   = Math.max(1, eFi - sFi)
                        var fstride = Math.max(1, Math.ceil((eFi - sFi + 1) / root.kFanMaxFrames))
                        var fanEls  = ["arms", "spine", "shoulders", "hips", "legs"]
                        for (var fe = 0; fe < fanEls.length; ++fe) {
                            var fel = fanEls[fe]
                            if (root._elemMode(fel) !== "fan") continue
                            for (var fk = sFi; fk < eFi; fk += fstride) {
                                var fa = 0.08 + 0.47 * (fk - sFi) / fspan     // 0.08 → 0.55
                                root._paintElem(ctx, cr, sm[fk].kp, fel, fa * 0.7, false, root.leadIsLeft, S)
                            }
                            root._paintElem(ctx, cr, sm[eFi].kp, fel, 0.7, true, root.leadIsLeft, S)
                        }
                    }
                }

                // Layer C synth is no longer drawn as a separate dim ghost here: the
                // frame-mode shaft below sources its current position from the dense
                // synth-preferring series (_clubFan), so the shaft line IS the synth
                // tier — it scrubs smoothly at 240 Hz instead of stepping at the source
                // frame rate. Metrics still read the measured `samples` only.

                // Club: fading head trail, then the current shaft in the Biomech
                // Blueprint language (sheath + gradient hero pen + end ticks + ring
                // nodes), tinted with the theme accent. A projected head
                // (ShaftHeadProjected, flags & 0x10) is an assumed-length guess, not
                // a measurement — drawn as a lone dim pen, no ticks/nodes. A Stage-2
                // off-frame head (flags & 0x80) is edge-clamped and always co-set
                // with 0x10, so it renders in the same projected-dim style. The
                // trail only bridges two measured heads (no fabricated arcs).
                var ci = _indexFor(_clubSamples, t)
                if (shaftMode === "frame" && ci >= 0) {
                    var kHeadProjected = 0x10

                    // Head trail — measured heads only, accent, scale-relative width.
                    ctx.strokeStyle = cClub
                    ctx.lineCap     = "round"
                    var k0 = Math.max(0, ci - kTrail)
                    for (var k = k0; k < ci; ++k) {
                        var cs0 = _clubSamples[k], cs1 = _clubSamples[k + 1]
                        if ((cs0.flags & kHeadProjected) || (cs1.flags & kHeadProjected))
                            continue
                        var h0 = cs0.head, h1 = cs1.head
                        ctx.globalAlpha = 0.45 * (k + 1 - k0) / (ci - k0 + 1) * clubMute
                        ctx.lineWidth   = 0.018 * S
                        ctx.beginPath()
                        ctx.moveTo(h0[0] * cr.width + cr.x, h0[1] * cr.height + cr.y)
                        ctx.lineTo(h1[0] * cr.width + cr.x, h1[1] * cr.height + cr.y)
                        ctx.stroke()
                    }

                    // Current shaft from the dense synth-preferring series (_clubFan)
                    // so it scrubs smoothly at 240 Hz; the faint head trail above
                    // stays on the measured breadcrumbs (its time-span). Falls back to
                    // the measured sample when synth is absent (old swings) / off-span.
                    var fi = _indexFor(_clubFan, t)
                    var s  = (fi >= 0) ? _clubFan[fi] : _clubSamples[ci]
                    var projected = (s.flags & kHeadProjected) !== 0
                    var gx = s.grip[0] * cr.width + cr.x, gy = s.grip[1] * cr.height + cr.y
                    var hx = s.head[0] * cr.width + cr.x, hy = s.head[1] * cr.height + cr.y
                    var hd = _clampHeadToRect(gx, gy, hx, hy, cr)
                    if (hd) {
                        var a = 0.4 + 0.6 * s.conf

                        if (projected) {
                            // Assumed-length guess — a lone dim pen, no decoration.
                            ctx.strokeStyle = cClub
                            ctx.globalAlpha = (0.35 + 0.5 * s.conf) * 0.5 * clubMute
                            ctx.lineWidth   = Math.max(1, 0.018 * S)
                            ctx.beginPath(); ctx.moveTo(gx, gy); ctx.lineTo(hd[0], hd[1]); ctx.stroke()
                        } else {
                            // Measured club — full blueprint treatment.
                            var dx = hd[0] - gx, dy = hd[1] - gy
                            var len = Math.hypot(dx, dy)
                            var ux = len > 1e-3 ? -dy / len : 0
                            var uy = len > 1e-3 ?  dx / len : 0

                            // 1. Sheath — faint wide underlay (∼ the faint torso sides).
                            ctx.strokeStyle = cClub
                            ctx.globalAlpha = 0.22 * a * clubMute
                            ctx.lineWidth   = 0.060 * S
                            ctx.beginPath(); ctx.moveTo(gx, gy); ctx.lineTo(hd[0], hd[1]); ctx.stroke()

                            // 2. Hero pen — gradient grip→head (∼ the spine).
                            var grad = ctx.createLinearGradient(gx, gy, hd[0], hd[1])
                            grad.addColorStop(0, cClubHi)
                            grad.addColorStop(1, cClub)
                            ctx.strokeStyle = grad
                            ctx.globalAlpha = a * clubMute
                            ctx.lineWidth   = 0.034 * S
                            ctx.beginPath(); ctx.moveTo(gx, gy); ctx.lineTo(hd[0], hd[1]); ctx.stroke()

                            // 3. End ticks — perpendicular caps that protrude past the
                            //    ring nodes at grip and head (∼ the spine end ticks).
                            var half = 0.080 * S
                            ctx.lineWidth   = 0.026 * S
                            ctx.globalAlpha = a * clubMute
                            ctx.strokeStyle = cClubHi
                            ctx.beginPath(); ctx.moveTo(gx - ux * half, gy - uy * half); ctx.lineTo(gx + ux * half, gy + uy * half); ctx.stroke()
                            ctx.strokeStyle = cClub
                            ctx.beginPath(); ctx.moveTo(hd[0] - ux * half, hd[1] - uy * half); ctx.lineTo(hd[0] + ux * half, hd[1] + uy * half); ctx.stroke()

                            // 4. Head node — concentric ring, the club's hero node (a
                            //    touch larger than a body joint since it is the measured
                            //    impact point); ring + centre-dot alpha honour the
                            //    Stage-2 headConf (honest confidence).
                            var hc = (s.headConf !== undefined && s.headConf >= 0)
                                     ? Math.max(0.25, Math.min(1, s.headConf)) : 1.0
                            ctx.globalAlpha = clubMute
                            ctx.fillStyle   = cClubInk
                            ctx.beginPath(); ctx.arc(hd[0], hd[1], 0.058 * S, 0, Math.PI * 2); ctx.fill()
                            ctx.globalAlpha = (0.5 + 0.5 * s.conf) * hc * clubMute
                            ctx.strokeStyle = cClub
                            ctx.lineWidth   = 0.022 * S
                            ctx.beginPath(); ctx.arc(hd[0], hd[1], 0.058 * S, 0, Math.PI * 2); ctx.stroke()
                            ctx.globalAlpha = (0.35 + 0.5 * s.conf) * hc * clubMute
                            ctx.fillStyle   = cClub
                            ctx.beginPath(); ctx.arc(hd[0], hd[1], 0.020 * S, 0, Math.PI * 2); ctx.fill()

                            // 5. Grip node — smaller ring, subordinate to the head.
                            ctx.globalAlpha = clubMute
                            ctx.fillStyle   = cClubInk
                            ctx.beginPath(); ctx.arc(gx, gy, 0.040 * S, 0, Math.PI * 2); ctx.fill()
                            ctx.globalAlpha = a * clubMute
                            ctx.strokeStyle = cClub
                            ctx.lineWidth   = 0.017 * S
                            ctx.beginPath(); ctx.arc(gx, gy, 0.040 * S, 0, Math.PI * 2); ctx.stroke()
                        }
                    }
                    ctx.globalAlpha = 1.0
                }

                // Coaching P-positions P1–P8 (shaft_position_first design §2
                // Layer B). Persistent faint dots mark every extracted
                // position's head point so the coach can see where the P
                // moments land on screen; the position nearest the playhead
                // (within ±40 ms) is additionally highlighted with its own
                // shaft line + a "P<n>" label. Green-ish (cGood) marks a
                // milestone boundary-value fit (PositionSource::MilestoneFit,
                // source === 1); the standard club accent marks a raw track
                // sample (source === 0).
                if (shaftMode === "frame" && root.showSynthTier && _clubPositions.length > 0) {
                    var kMilestoneFit = 1
                    var kPosWindowUs  = 40000

                    // Faint dots — every position, cheap fixed-size loop (≤ 8).
                    ctx.fillStyle = cClub
                    for (var dj = 0; dj < _clubPositions.length; ++dj) {
                        var dp  = _clubPositions[dj]
                        var dhx = dp.head[0] * cr.width + cr.x, dhy = dp.head[1] * cr.height + cr.y
                        ctx.globalAlpha = 0.25 * clubMute
                        ctx.beginPath(); ctx.arc(dhx, dhy, 0.030 * S, 0, Math.PI * 2); ctx.fill()
                    }

                    // Nearest position to the playhead — highlighted line + label.
                    var poi    = _indexFor(_clubPositions, t)
                    var poiN   = (poi + 1 < _clubPositions.length) ? poi + 1 : poi
                    var poBest = Math.abs(_clubPositions[poiN].t_us - t) < Math.abs(_clubPositions[poi].t_us - t)
                                 ? poiN : poi
                    var po = _clubPositions[poBest]
                    if (Math.abs(po.t_us - t) <= kPosWindowUs) {
                        var mgx    = po.grip[0] * cr.width + cr.x, mgy = po.grip[1] * cr.height + cr.y
                        var mhx    = po.head[0] * cr.width + cr.x, mhy = po.head[1] * cr.height + cr.y
                        var mColor = (po.source === kMilestoneFit) ? cGood : cClub

                        ctx.strokeStyle = mColor
                        ctx.globalAlpha = 0.9 * clubMute
                        ctx.lineWidth   = Math.max(1, 0.026 * S)
                        ctx.beginPath(); ctx.moveTo(mgx, mgy); ctx.lineTo(mhx, mhy); ctx.stroke()

                        ctx.fillStyle    = mColor
                        ctx.font         = Theme.fontSzMicro + "px '" + Theme.fontData + "'"
                        ctx.textAlign    = "left"
                        ctx.textBaseline = "bottom"
                        ctx.fillText("P" + po.p, mgx + 0.06 * S, mgy - 0.06 * S)
                    }
                    ctx.globalAlpha = 1.0
                }

                // FAN mode for the shaft — a decaying grip→head trail over the last
                // fanWindowMs of measured club samples, then the current sample as a
                // full-alpha hero pen + head dot. Distinct from the frame-mode kTrail
                // (a short measured-head trail on the sheathed shaft); the fan is the
                // longer mouse-trail motion read.
                if (shaftMode === "fan" && _clubFan.length > 0) {
                    // Trail from the merged measured+synth series (dense) so the fan
                    // fills the inter-frame gaps regardless of source fps.
                    var feI = _indexFor(_clubFan, t)
                    if (feI >= 0) {
                        var fsI = _indexFor(_clubFan, t - root.fanWindowMs * 1000)
                        if (fsI < 0) fsI = 0
                        var cspan = Math.max(1, feI - fsI)
                        var cstr  = Math.max(1, Math.ceil((feI - fsI + 1) / root.kFanMaxFrames))
                        ctx.lineCap     = "round"
                        ctx.strokeStyle = cClub
                        for (var fck = fsI; fck < feI; fck += cstr) {
                            var fcs = _clubFan[fck]
                            if (!fcs || fcs.conf <= 0) continue
                            var ffgx = fcs.grip[0] * cr.width + cr.x, ffgy = fcs.grip[1] * cr.height + cr.y
                            var ffhx = fcs.head[0] * cr.width + cr.x, ffhy = fcs.head[1] * cr.height + cr.y
                            var ffhd = _clampHeadToRect(ffgx, ffgy, ffhx, ffhy, cr)
                            if (!ffhd) continue
                            ctx.globalAlpha = (0.08 + 0.47 * (fck - fsI) / cspan) * 0.7 * clubMute
                            ctx.lineWidth   = 0.020 * S
                            ctx.beginPath(); ctx.moveTo(ffgx, ffgy); ctx.lineTo(ffhd[0], ffhd[1]); ctx.stroke()
                        }
                        var fcur = _clubFan[feI]
                        if (fcur && fcur.conf > 0) {
                            var cgx = fcur.grip[0] * cr.width + cr.x, cgy = fcur.grip[1] * cr.height + cr.y
                            var chx = fcur.head[0] * cr.width + cr.x, chy = fcur.head[1] * cr.height + cr.y
                            var chd = _clampHeadToRect(cgx, cgy, chx, chy, cr)
                            if (chd) {
                                var gradF = ctx.createLinearGradient(cgx, cgy, chd[0], chd[1])
                                gradF.addColorStop(0, cClubHi); gradF.addColorStop(1, cClub)
                                ctx.strokeStyle = gradF
                                ctx.globalAlpha = (0.4 + 0.6 * fcur.conf) * 0.7 * clubMute
                                ctx.lineWidth   = 0.034 * S
                                ctx.beginPath(); ctx.moveTo(cgx, cgy); ctx.lineTo(chd[0], chd[1]); ctx.stroke()
                                ctx.globalAlpha = 0.7 * clubMute
                                ctx.fillStyle   = cClub
                                ctx.beginPath(); ctx.arc(chd[0], chd[1], 0.020 * S, 0, Math.PI * 2); ctx.fill()
                            }
                        }
                        ctx.globalAlpha = 1.0
                    }
                }

                // Ball: the detected circle at the current playhead — matches the
                // live green ball circle. found=false (post-launch) draws nothing,
                // so the ball vanishes at impact. Hidden when the ball element is off.
                var bi = _indexFor(_ballSamples, t)
                if (root._elemMode("ball") !== "off" && bi >= 0 && _ballSamples[bi].found) {
                    var b  = _ballSamples[bi]
                    var bx = b.x * cr.width + cr.x, by = b.y * cr.height + cr.y
                    var br = Math.max(4, b.r * cr.width)
                    ctx.globalAlpha = 1.0
                    ctx.strokeStyle = cGood
                    ctx.lineWidth   = 2
                    ctx.beginPath()
                    ctx.arc(bx, by, br, 0, Math.PI * 2)
                    ctx.stroke()
                }

                // Predicted (R6 kinematic-model) shaft — dashed ghost behind the
                // solid actual line; lets you watch model vs measurement diverge.
                // Dev/test chrome: off by default. σ_β recovered from conf for the
                // optional envelope cone (conf = 1 − σ_βDeg/60 in the analyzer).
                if (shaftMode === "frame" && root.showPredictedShaft && _clubPredicted.length > 0) {
                    var ppi = _indexFor(_clubPredicted, t)
                    if (ppi >= 0) {
                        var ps  = _clubPredicted[ppi]
                        var pgx = ps.grip[0] * cr.width + cr.x, pgy = ps.grip[1] * cr.height + cr.y
                        var phx = ps.head[0] * cr.width + cr.x, phy = ps.head[1] * cr.height + cr.y
                        if (root.showPredictedEnvelope) {
                            var sigDeg = (1.0 - ps.conf) * 60.0
                            var half   = (3.0 * sigDeg) * Math.PI / 180.0
                            var ang    = Math.atan2(phy - pgy, phx - pgx)
                            var len    = Math.sqrt((phx - pgx) * (phx - pgx) + (phy - pgy) * (phy - pgy))
                            ctx.strokeStyle = cAccent
                            ctx.globalAlpha = 0.15
                            ctx.lineWidth   = 1
                            ctx.setLineDash([])
                            for (var sgn = -1; sgn <= 1; sgn += 2) {
                                ctx.beginPath()
                                ctx.moveTo(pgx, pgy)
                                ctx.lineTo(pgx + len * Math.cos(ang + sgn * half),
                                           pgy + len * Math.sin(ang + sgn * half))
                                ctx.stroke()
                            }
                        }
                        ctx.strokeStyle = cAccent
                        ctx.globalAlpha = 0.40
                        ctx.lineWidth   = 2
                        ctx.setLineDash([6, 5])
                        ctx.beginPath()
                        ctx.moveTo(pgx, pgy)
                        ctx.lineTo(phx, phy)
                        ctx.stroke()
                        ctx.setLineDash([])
                        ctx.beginPath()
                        ctx.arc(phx, phy, 4, 0, Math.PI * 2)
                        ctx.stroke()
                    }
                }
                ctx.globalAlpha = 1.0
            }
        }

        // ── Motion TRACE polylines ────────────────────────────────────────
        // One Shape per traceable element; visible only when that element is in
        // "trace" mode. Coordinates map through root.contentRect exactly as the
        // replay Canvas maps normalized → scene (the Shape occupies contentRect and
        // PathPolyline scales by its width/height — mirrors PpTrace.qml), so a trace
        // lands pixel-aligned with the skeleton. Body traces come from the smoothed
        // series (empty ⇒ nothing draws), the shaft trace from club-sample heads.
        // The point list rebuilds when the playhead / series / element changes.
        Repeater {
            model: root._traceElements
            delegate: Shape {
                id: traceShape
                required property string modelData
                readonly property bool _active: root.motionOn
                        && root._replayActive
                        && root._replayPerspective === 2
                        && root._inSwingWindow
                        && root._elemMode(modelData) === "trace"
                property var pts: _active ? root._traceNormPoints(modelData) : []
                visible: _active && pts.length > 1
                x: root.contentRect.x
                y: root.contentRect.y
                width:  root.contentRect.width
                height: root.contentRect.height
                z: 21
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: root._traceColor(modelData)
                    strokeWidth: Theme.sp(1)
                    fillColor:   "transparent"
                    joinStyle:   ShapePath.RoundJoin
                    capStyle:    ShapePath.RoundCap
                    PathPolyline {
                        path: traceShape.pts.map(function (p) {
                            return Qt.point(p.x * traceShape.width, p.y * traceShape.height)
                        })
                    }
                }
            }
        }

        // ── Telestrator annotation layer (analyse view only) ──────────────
        // Draw/edit circles, lines and hollow squares over the replay video.
        // Marks are normalized to contentRect (reflow on resize) and held in the
        // AnnotationTool store, cleared when the focused swing is deselected.
        PpAnnotationLayer {
            anchors.fill: parent
            z: 28
            active: root.annotationsEnabled
            contentRect: root.contentRect
            tileKey: (root.annotationsEnabled && root._isReplay && shotReplay.swingDir !== "")
                     ? shotReplay.swingDir + "#" + root.replayStreamIndex : ""
        }

        // ── Faint hitting-area hint ───────────────────────────────────────
        // Label-free, low-alpha outline of the ball detector ROI — enough to
        // see where the detector is looking while live pose runs, without the
        // full editor chrome (which showHittingArea owns; hint yields to it).
        Rectangle {
            id: roiHint
            visible: root.showHittingAreaHint && !root.showHittingArea
                     && root.roiIsSet
            z: 19
            color: Theme.colorWarnLight
            border.color: Qt.alpha(Theme.colorWarn, 0.35)
            border.width: 1

            x: roiOverlay.crX + (root.instance ? root.instance.roi.x * roiOverlay.crW : 0)
            y: roiOverlay.crY + (root.instance ? root.instance.roi.y * roiOverlay.crH : 0)
            width:  root.instance ? root.instance.roi.width  * roiOverlay.crW : 0
            height: root.instance ? root.instance.roi.height * roiOverlay.crH : 0
        }

        // ── Persistent ROI overlay ────────────────────────────────────────
        Rectangle {
            id: roiOverlay
            visible: root.showHittingArea && root.roiIsSet && !root.roiDragging
            z: 20
            color: "transparent"
            border.color: Theme.colorWarn
            border.width: 2

            property real crX: root.videoInset + (root.instance && root.instance.needsDebayer
                               ? 0 : videoOut.contentRect.x)
            property real crY: root.videoInset + (root.instance && root.instance.needsDebayer
                               ? 0 : videoOut.contentRect.y)
            property real crW: root.instance && root.instance.needsDebayer
                               ? bayerView.width : videoOut.contentRect.width
            property real crH: root.instance && root.instance.needsDebayer
                               ? bayerView.height : videoOut.contentRect.height

            x: crX + (root.instance ? root.instance.roi.x * crW : 0)
            y: crY + (root.instance ? root.instance.roi.y * crH : 0)
            width:  root.instance ? root.instance.roi.width  * crW : 0
            height: root.instance ? root.instance.roi.height * crH : 0

            Text {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: Theme.sp(3)
                text: qsTr("Hitting Area")
                color: Theme.colorWarn
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                font.weight: Font.Normal
                font.letterSpacing: Theme.trackingData
            }

            // Corner handle squares — editor affordance only (drag handled by
            // the unified MouseArea below; same pattern as the crop editor).
            readonly property int hs: Theme.sp(10)
            Rectangle { visible: root.roiEditable; width: roiOverlay.hs; height: roiOverlay.hs; x: -roiOverlay.hs/2;                    y: -roiOverlay.hs/2;                     color: Theme.colorWarn; border.width: 1; border.color: "black" }
            Rectangle { visible: root.roiEditable; width: roiOverlay.hs; height: roiOverlay.hs; x: roiOverlay.width-roiOverlay.hs/2;    y: -roiOverlay.hs/2;                     color: Theme.colorWarn; border.width: 1; border.color: "black" }
            Rectangle { visible: root.roiEditable; width: roiOverlay.hs; height: roiOverlay.hs; x: -roiOverlay.hs/2;                    y: roiOverlay.height-roiOverlay.hs/2;    color: Theme.colorWarn; border.width: 1; border.color: "black" }
            Rectangle { visible: root.roiEditable; width: roiOverlay.hs; height: roiOverlay.hs; x: roiOverlay.width-roiOverlay.hs/2;    y: roiOverlay.height-roiOverlay.hs/2;    color: Theme.colorWarn; border.width: 1; border.color: "black" }
        }

        // ── Rubber-band while dragging ────────────────────────────────────
        Rectangle {
            id: rubberBand
            visible: root.roiDragging
            z: 25
            x: Math.min(root.roiDragStart.x, root.roiDragEnd.x)
            y: Math.min(root.roiDragStart.y, root.roiDragEnd.y)
            width:  Math.abs(root.roiDragEnd.x - root.roiDragStart.x)
            height: Math.abs(root.roiDragEnd.y - root.roiDragStart.y)
            color: Theme.colorWarnLight
            border.color: Theme.colorWarn
            border.width: 2
        }

        // ── Detected ball circle ──────────────────────────────────────────
        // Shown in the ROI editors (showHittingArea) AND during live capture
        // (showBallOverlay, gated on the live-pose setting by the host) — both
        // read the live per-frame ballX/ballY/ballRadius.
        Rectangle {
            id: ballCircle
            visible: (root.showHittingArea || root.showBallOverlay) && root.roiIsSet
                     && root.instance !== null && root.instance.ballDetected
            z: 22
            color: "transparent"
            border.color: Theme.colorGood
            border.width: 2

            property real crX: root.videoInset + (root.instance && root.instance.needsDebayer
                               ? 0 : videoOut.contentRect.x)
            property real crY: root.videoInset + (root.instance && root.instance.needsDebayer
                               ? 0 : videoOut.contentRect.y)
            property real crW: root.instance && root.instance.needsDebayer
                               ? bayerView.width : videoOut.contentRect.width
            property real crH: root.instance && root.instance.needsDebayer
                               ? bayerView.height : videoOut.contentRect.height

            property real screenR: Math.max(4, (root.instance ? root.instance.ballRadius : 0) * crW)

            x: crX + (root.instance ? root.instance.ballX : 0) * crW - screenR
            y: crY + (root.instance ? root.instance.ballY : 0) * crH - screenR
            width:  screenR * 2
            height: screenR * 2
            radius: screenR
        }

        // ── Unified ROI editor MouseArea (crop-editor interaction model) ──
        // Always-on while roiEditable: drag inside the rect to move it, drag
        // a corner handle to resize, drag anywhere else to draw a new rect.
        // Move/resize update the instance live (no persistence churn); the
        // committed rect persists once on release via the manager.
        MouseArea {
            id: roiEditArea
            anchors.fill: parent
            enabled: root.roiEditable && root.instance !== null
            visible: enabled
            hoverEnabled: true
            preventStealing: true
            z: 30

            readonly property real hr: Theme.sp(16)   // handle hit radius
            property string dragMode: "none"          // none|new|move|tl|tr|bl|br
            property real origX: 0
            property real origY: 0
            property real origW: 0
            property real origH: 0

            function hitZone(mx, my) {
                if (!root.roiIsSet) return "new"
                var rx = roiOverlay.x,     ry = roiOverlay.y
                var rw = roiOverlay.width, rh = roiOverlay.height
                var h  = hr
                if (Math.abs(mx - rx)      < h && Math.abs(my - ry)      < h) return "tl"
                if (Math.abs(mx - (rx+rw)) < h && Math.abs(my - ry)      < h) return "tr"
                if (Math.abs(mx - rx)      < h && Math.abs(my - (ry+rh)) < h) return "bl"
                if (Math.abs(mx - (rx+rw)) < h && Math.abs(my - (ry+rh)) < h) return "br"
                if (mx > rx && mx < rx+rw && my > ry && my < ry+rh)           return "move"
                return "new"                          // outside = draw a replacement
            }

            cursorShape: {
                switch (hitZone(mouseX, mouseY)) {
                case "tl": case "br": return Qt.SizeFDiagCursor
                case "tr": case "bl": return Qt.SizeBDiagCursor
                case "move":          return Qt.SizeAllCursor
                default:              return Qt.CrossCursor
                }
            }

            onPressed: (mouse) => {
                dragMode = hitZone(mouse.x, mouse.y)
                root.roiDragStart = Qt.point(mouse.x, mouse.y)
                if (dragMode === "new") {
                    root.roiDragEnd  = Qt.point(mouse.x, mouse.y)
                    root.roiDragging = true
                } else if (root.instance) {
                    var r = root.instance.roi
                    origX = r.x; origY = r.y; origW = r.width; origH = r.height
                }
            }

            onPositionChanged: (mouse) => {
                if (dragMode === "none" || !root.instance) return
                if (dragMode === "new") {
                    root.roiDragEnd = Qt.point(mouse.x, mouse.y)
                    return
                }
                var crW = roiOverlay.crW, crH = roiOverlay.crH
                if (crW <= 0 || crH <= 0) return
                var dx = (mouse.x - root.roiDragStart.x) / crW
                var dy = (mouse.y - root.roiDragStart.y) / crH
                var ox = origX, oy = origY, ow = origW, oh = origH
                var nx, ny, nw, nh
                switch (dragMode) {
                case "move":
                    nx = Math.max(0, Math.min(1.0 - ow, ox + dx))
                    ny = Math.max(0, Math.min(1.0 - oh, oy + dy))
                    root.instance.setRoi(Qt.rect(nx, ny, ow, oh))
                    break
                case "tl":
                    nx = Math.max(0,    Math.min(ox + ow - 0.02, ox + dx))
                    ny = Math.max(0,    Math.min(oy + oh - 0.02, oy + dy))
                    nw = Math.max(0.02, ow - (nx - ox))
                    nh = Math.max(0.02, oh - (ny - oy))
                    root.instance.setRoi(Qt.rect(nx, ny, nw, nh))
                    break
                case "tr":
                    ny = Math.max(0,    Math.min(oy + oh - 0.02, oy + dy))
                    nw = Math.max(0.02, Math.min(1.0 - ox, ow + dx))
                    nh = Math.max(0.02, oh - (ny - oy))
                    root.instance.setRoi(Qt.rect(ox, ny, nw, nh))
                    break
                case "bl":
                    nx = Math.max(0,    Math.min(ox + ow - 0.02, ox + dx))
                    nw = Math.max(0.02, ow - (nx - ox))
                    nh = Math.max(0.02, Math.min(1.0 - oy, oh + dy))
                    root.instance.setRoi(Qt.rect(nx, oy, nw, nh))
                    break
                case "br":
                    nw = Math.max(0.02, Math.min(1.0 - ox, ow + dx))
                    nh = Math.max(0.02, Math.min(1.0 - oy, oh + dy))
                    root.instance.setRoi(Qt.rect(ox, oy, nw, nh))
                    break
                }
            }

            onReleased: (mouse) => {
                if (!root.instance) { dragMode = "none"; root.roiDragging = false; return }

                if (dragMode === "new") {
                    root.roiDragging = false
                    var crX = roiOverlay.crX, crY = roiOverlay.crY
                    var crW = roiOverlay.crW, crH = roiOverlay.crH
                    if (crW > 0 && crH > 0) {
                        var x1  = Math.min(root.roiDragStart.x, mouse.x)
                        var y1  = Math.min(root.roiDragStart.y, mouse.y)
                        var x2  = Math.max(root.roiDragStart.x, mouse.x)
                        var y2  = Math.max(root.roiDragStart.y, mouse.y)
                        var nx  = Math.max(0, Math.min(1, (x1 - crX) / crW))
                        var ny  = Math.max(0, Math.min(1, (y1 - crY) / crH))
                        var nx2 = Math.max(0, Math.min(1, (x2 - crX) / crW))
                        var ny2 = Math.max(0, Math.min(1, (y2 - crY) / crH))
                        // Via the manager so the hitting area persists per
                        // camera (restored on connect when fixed in place).
                        cameraManager.setBallRoi(root.instance, Qt.rect(nx, ny, nx2 - nx, ny2 - ny))
                    }
                } else if (dragMode !== "none") {
                    // Move/resize updated the instance live — persist once.
                    cameraManager.setBallRoi(root.instance, root.instance.roi)
                }
                dragMode = "none"
            }
        }
    }
}
