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

// PpDashboardPanel — the configurable post-shot dashboard. It joins the metric
// catalogue (identity / availability / normative corridors) to the focused shot's
// analysisDetail (measured values) BY KEY, and composes four glanceable zones —
// Verdict, Setup, Motion, Sequence — chosen by a per-session-type preset. Read at
// distance: no charts, headline numbers only. The preset control lives in this
// panel's header (never the toolbar); editing it updates the per-type store so the
// in-app panel and any cast wall window stay in lock-step.
//
// The dashboard is a pure DISPLAY SURFACE: the config (per session type) is
// swing-agnostic, and each zone renders its configured metrics showing the metric
// layer's value, or "NA" (live metric with no value this swing) / "soon" (planned,
// no producer yet). Availability is the pipeline's concern, not the dashboard's.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    // Set by the host screen (Swing 0, Wrist 1, GRF 2, Coach 3), like the other
    // stage delegates (PpReplayCharts { sessionType: … }).
    property int sessionType: -1

    // The interactive layer — playhead sweep, click-to-seek, hover-scrub, tile
    // click-through. GUARDRAIL: the resting (non-hovered, non-playing) render is
    // identical either way, so the auto-closing wall cast and the interactive desk
    // window are genuinely one component. A wall cast passes false; the in-app stage
    // and a persistent desk window pass true.
    property bool interactive: false

    // Cast zoom for viewing distance (PpDashboardWindow.contentScale), applied to
    // the ZONE CONTENT ONLY — never to the header. The zoom exists so DATA can be
    // read across a room; the preset control is chrome, and chrome that grows with
    // the room stops being the control the user knows from the session toolbar.
    // Keeping the header out of the transform also keeps its Popup honest: a Popup
    // is reparented into the window overlay, so it could never inherit the
    // transform anyway, and a scaled anchor with an unscaled popup cannot align.
    property real uiScale: 1.0

    // True while the pointer is anywhere over the dashboard — the cast window holds
    // its auto-close dwell open on this, so a presenter reading the wall is never
    // cut off mid-sentence.
    readonly property bool hovered: panelHover.hovered

    // Playhead, forwarded to every rail and the sequence strip at once. -1 (idle)
    // whenever the surface is non-interactive, which is what keeps the resting render
    // of the wall cast free of a sweep line.
    readonly property real playheadUs: interactive && shotReplay.active
                                       ? shotReplay.positionUs : -1

    // Click-through target: the catalogue's MetricDetail page for one key. Routed
    // through the MetricRoute singleton because this panel is instantiated deep
    // inside a stage delegate (and again inside the cast Window) — Main.qml owns the
    // navigation. The signal stays public so a host can intercept it instead.
    signal metricDetailRequested(string key)
    onMetricDetailRequested: (key) => MetricRoute.open(key)

    HoverHandler { id: panelHover }

    // The focused shot's flattened analysis. Recomputed shotCtx tracks it.
    readonly property var detail: shotReplay.analysisDetail
    readonly property var shotCtx: root._ctx(detail, sessionType)
    readonly property bool hasShot: shotReplay.active && detail && detail.overall !== undefined

    // shotCtx is a function result, so bind it through a helper that reads the two
    // tracked properties (detail, sessionType) to re-evaluate when either changes.
    function _ctx(d, st) { return shotReplay.shotContext(st) }

    // The catalogue façade — one instance per panel (QML_ELEMENT, not a singleton).
    // NB: id must NOT be `catalog` — the zones have a `catalog` property, and an
    // unqualified `catalog: catalog` binding would resolve to the zone's OWN (null)
    // property (id/property shadowing — see CLAUDE.md / commit 39857cc).
    MetricCatalog { id: metricCat }

    // Wrist findings/strengths for the Verdict zone (wrist-only; empty elsewhere).
    // id must NOT be `wristModel` (same shadowing trap as `catalog` above).
    WristDiagnosticsModel {
        id: wristDx
        analysisDetail: root.detail
        playheadUs:     shotReplay.positionUs
    }

    // Active preset id + its display label for the header chip.
    readonly property string _activeId: ViewLayout.dashboardActive(sessionType)
    function _presetLabel(id) {
        if (id === "custom") return qsTr("Custom")
        var cat = ViewLayout.dashboardPresetCatalog()
        for (var i = 0; i < cat.length; ++i)
            if (cat[i].id === id) return cat[i].label
        return id   // a saved preset — show its name
    }

    // ── Header: title · right-aligned preset chip + edit affordance ─────────────
    Rectangle {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        // EXACTLY the session toolbar's geometry — sp(60) tall, sp(16)/sp(14)
        // margins, an sp(44) PpToolPill — so the dashboard's control row and the
        // session toolbar are the same bar, not two that merely resemble each other.
        height: Theme.sp(60)
        color: "transparent"

        Text {
            anchors { left: parent.left; leftMargin: Theme.sp(16); verticalCenter: parent.verticalCenter }
            text: qsTr("Dashboard")
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            font.capitalization: Font.AllUppercase; font.letterSpacing: Theme.trackingMicro
            color: Theme.colorText3
        }

        // Preset control — THE standard toolbar item (PpToolPill), not a lookalike:
        // it does the same job as the View/Motion pills and must read as the same
        // control. Shows the active preset (with a '*' when Custom).
        PpToolPill {
            id: presetPill
            anchors { right: parent.right; rightMargin: Theme.sp(14); verticalCenter: parent.verticalCenter }
            glyph: "▤"
            microLabel: qsTr("DASHBOARD")
            label: root._presetLabel(root._activeId)
                   + (root._activeId === "custom" ? " *" : "")
            active: editor.opened
            onClicked: editor.opened ? editor.close() : editor.open()
        }
    }

    // ── Empty state ─────────────────────────────────────────────────────────────
    Text {
        anchors.centerIn: parent
        visible: !root.hasShot
        text: shotReplay.active ? qsTr("No analysis for this shot")
                                : qsTr("Select a shot to review")
        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
        color: Theme.colorText3
    }

    // ── Zone column ─────────────────────────────────────────────────────────────
    Flickable {
        id: flick
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom
                  topMargin: Theme.sp(6) }
        visible: root.hasShot
        contentWidth: width
        // Content is laid out at 1/uiScale and drawn scaled, so the SCROLLABLE
        // height is the laid-out height times the zoom.
        contentHeight: zoneWrap.implicitHeight * root.uiScale
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // The cast zoom lives HERE, not on the panel — the header above stays at
        // native size. A `transform: Scale` (origin 0,0) is used rather than the
        // `scale` property so the top-left corner is the fixed point and the
        // content grows down-and-right from the header, with no transformOrigin
        // bookkeeping.
        Item {
            id: zoneWrap
            width: flick.width / root.uiScale
            implicitHeight: zoneCol.implicitHeight + Theme.sp(24)
            height: implicitHeight
            transform: Scale { xScale: root.uiScale; yScale: root.uiScale }

            ColumnLayout {
                id: zoneCol
                // A dashboard earns its keep by filling the surface — the gutters here
                // are a seam between cards, not a document margin. The zone cards carry
                // their own internal padding, so this only needs to keep their borders
                // off the edge.
                x: Theme.sp(4)
                width: zoneWrap.width - Theme.sp(8)
                spacing: Theme.sp(6)

                Repeater {
                    model: ViewLayout.dashboardZones(root.sessionType)
                    delegate: Loader {
                        required property var modelData      // zone key string
                        Layout.fillWidth: true
                        Layout.preferredHeight: item ? item.implicitHeight : 0
                        sourceComponent: root._zoneComp(modelData)
                    }
                }
            }
        }
    }

    function _zoneComp(key) {
        switch (key) {
        case "verdict":  return verdictComp
        case "setup":    return setupComp
        case "motion":   return motionComp
        case "sequence": return sequenceComp
        }
        return null
    }

    // Inline wrappers — each binds the shared join inputs down to a zone file and
    // resolves that zone's pinned metric list from the per-type preset store.
    Component {
        id: verdictComp
        PpDashboardVerdictZone {
            width: parent ? parent.width : 0
            detail: root.detail; wristModel: wristDx
            catalog: metricCat; shotCtx: root.shotCtx
            interactive: root.interactive
            onMetricActivated: (key) => root.metricDetailRequested(key)
        }
    }
    Component {
        id: setupComp
        PpDashboardSetupZone {
            width: parent ? parent.width : 0
            catalog: metricCat; shotCtx: root.shotCtx; detail: root.detail
            sessionType: root.sessionType
            pinnedMetrics: ViewLayout.dashboardZoneMetrics(root.sessionType, "setup")
            interactive: root.interactive
            onMetricActivated: (key) => root.metricDetailRequested(key)
        }
    }
    Component {
        id: motionComp
        PpDashboardMotionZone {
            width: parent ? parent.width : 0
            catalog: metricCat; shotCtx: root.shotCtx; detail: root.detail
            sessionType: root.sessionType
            pinnedMetrics: ViewLayout.dashboardZoneMetrics(root.sessionType, "motion")
            interactive: root.interactive
            playheadUs:  root.playheadUs
            onMetricActivated: (key) => root.metricDetailRequested(key)
            onSeekRequested:   (tUs) => shotReplay.seekToUs(tUs)
        }
    }
    Component {
        id: sequenceComp
        PpDashboardSequenceZone {
            width: parent ? parent.width : 0
            catalog: metricCat; shotCtx: root.shotCtx; detail: root.detail
            sessionType: root.sessionType
            interactive: root.interactive
            playheadUs:  root.playheadUs
            onSeekRequested: (tUs) => shotReplay.seekToUs(tUs)
        }
    }

    // ── Preset editor popover ────────────────────────────────────────────────────
    PpDashboardPresetEditor {
        id: editor
        sessionType: root.sessionType
        catalog: metricCat
        shotCtx: root.shotCtx
        // The toolbar's popup-anchoring convention, verbatim (see PpToolPill):
        // PARENT the popup to the pill, then position in the pill's own space.
        // Both the pill and this popup render at native scale — the cast zoom
        // stops at the header — so no scale compensation is needed and this is
        // byte-for-byte what the View/Motion/Club popups do.
        parent: presetPill
        y: presetPill.height + Theme.sp(10)
        x: presetPill.width - width
    }
}
