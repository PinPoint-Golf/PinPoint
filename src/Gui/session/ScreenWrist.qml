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

// Wrist Motion screen (contentStack index 2). Hosts the persistent session
// toolbar, then a View-driven layout: an optional timeline rail, the centre
// stage (camera tiles today; charts/table land later), and an
// optional shot carousel. Which panels show, and how the stage packs them, is
// owned by the toolbar's View control (ViewLayout, per session type). The
// toolbar's device panels run calibration entirely in-panel, so this screen
// neither exposes nor routes any calibrate request.
//
// Replay and Analyse use the Transit timeline, in one of two orientations
// (appSettings.timelineOrientation): HORIZONTAL in the top rail, VERTICAL in a
// fixed-width panel to the LEFT of the stage. Kept in lockstep with
// ScreenSessionMode. Capture has no timeline (RmTimelineChart is resource-monitor only).

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    readonly property bool _timelineOn:  ViewLayout.isPanelOn(SessionMode.mode, "timeline")
    // Replay AND Analyse use the Transit timeline; Capture has no timeline rail.
    readonly property bool _transitMode: SessionMode.mode === SessionMode.replay
                                         || SessionMode.mode === SessionMode.analyse
    readonly property bool _vertical:    appSettings.timelineOrientation === "vertical"
    // All session screens stay alive in the StackLayout (this one at index Wrist+1).
    // The transit timeline's stationLayout recompute is heavy and fires on every
    // shotReplay span change, so keep it UNLOADED unless this screen is visible —
    // otherwise an off-screen timeline re-lays-out on every swing reload.
    readonly property bool _screenActive: navController.currentIndex === SessionController.Wrist + 1

    // THE focused swing — the active replay, else the carousel's selection. One definition, because
    // three panels here bind it (data viewer, markup, and the app-wide `currentSwing` below) and it
    // was previously written out twice; a third copy is how they start disagreeing about which
    // swing a screen is showing.
    readonly property string _focusedSwingDir:
        shotReplay.swingDir !== "" ? shotReplay.swingDir
                                   : (wristCarousel.selectedCard ? wristCarousel.selectedCard.swingDir : "")

    // Publish it app-wide.
    //
    // NOT gated on _screenActive, and that is the whole point rather than an oversight. Every
    // reader of this is BY DEFINITION on another screen — the Diagnostic Model panel lives in
    // Settings, which the session lock deliberately leaves reachable — so clearing it when this
    // screen stops being the visible one clears it exactly when somebody goes to look. It shipped
    // that way for one build and the column was never once on screen.
    //
    // What it means is therefore "the swing THIS SCREEN has loaded", not "the swing on the screen
    // you are looking at". It empties on its own when the screen genuinely has none — no replay and
    // nothing selected in the carousel — which is the honest condition. (The setter guards against
    // re-announcing the same path, so re-evaluating on every carousel change costs nothing.)
    Binding {
        target: currentSwing
        property: "swingDir"
        value: root._focusedSwingDir
        restoreMode: Binding.RestoreNone
    }

    // Today-scoped carousel: on entry (before capture) show today's most-recent
    // session folder, or an empty carousel when none exists for today. Guarded so
    // it never clobbers a live session's freshly-captured in-memory shots or an
    // open review — the toolbar reloads the carousel itself at session start/end
    // (those don't change the nav index, so this handler doesn't fire there).
    function _syncTodayCarousel() {
        if (!root._screenActive) return
        if (sessionController.running || sessionReviewController.reviewActive) return
        shotModel.loadSessionDir(shotProcessor.todaySessionDir(SessionController.Wrist))
    }
    Component.onCompleted: _syncTodayCarousel()
    Connections {
        target: navController
        function onCurrentIndexChanged() { root._syncTodayCarousel() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PpSessionToolbar {
            Layout.fillWidth: true
            sessionType: SessionController.Wrist
        }

        // Top rail — horizontal Transit timeline (Replay/Analyse only). Capture has
        // no timeline; the vertical orientation moves Transit to the left rail instead.
        Loader {
            id: topRail
            readonly property bool horizTransit: root._transitMode && root._timelineOn && !root._vertical
            Layout.fillWidth: true
            // Sized to the timeline's own content (line + labels), not a fixed
            // over-tall rail — otherwise dead space sits below the labels.
            Layout.preferredHeight: horizTransit ? (item ? item.contentHeight : Theme.sp(98)) : 0
            Behavior on Layout.preferredHeight {
                enabled: !Theme.reduceMotion
                NumberAnimation { duration: Theme.durationNormal; easing.type: Easing.InOutQuad }
            }
            visible: Layout.preferredHeight > 0
            active: visible && root._screenActive
            clip: true
            sourceComponent: transitHorizComp
        }

        // Stage row — optional vertical Transit panel (left) + the centre stage.
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 0

            Loader {
                id: leftRail
                readonly property bool transitVert: root._transitMode && root._vertical && root._timelineOn
                Layout.fillHeight: true
                Layout.preferredWidth: transitVert ? Theme.sp(184) : 0
                Behavior on Layout.preferredWidth {
                    enabled: !Theme.reduceMotion
                    NumberAnimation { duration: Theme.durationNormal; easing.type: Easing.InOutQuad }
                }
                visible: Layout.preferredWidth > 0
                active: visible && root._screenActive
                clip: true
                sourceComponent: transitVertComp
            }

            // Centre stage — camera tiles + the Replay charts trace + the Data table.
            // Packing (tabs/split/stage) and which panels show is resolved by ViewLayout
            // on the active mode; the View control toggles them.
            PpModeStage {
                Layout.fillWidth: true; Layout.fillHeight: true
                cameraDelegate: Component {
                    PpCameraTiles { sessionType: SessionController.Wrist; showHittingArea: false }
                }
                chartsDelegate: Component { PpReplayCharts { sessionType: SessionController.Wrist } }
                // Session diagnostics — what KEEPS
                // happening across this session's shots, and what the model authors as its
                // cause. Takes no sessionType (analysis is gated on available data, never on
                // what the screen was opened to do) and, unlike the Loaders above, needs no
                // _screenActive gate: PpModeStage already instantiates a delegate only when
                // the View control has that panel on, and the panel is off by default because
                // it owns a characteristic pack and a detection thread.
                sessionDiagnosticsDelegate: Component { PpSessionDiagnosticsPanel {} }
                // Launch monitor session board. Costs nothing to wire — the panel takes
                // no sessionType and reads the same shotModel this screen already shows,
                // and it stays off until the user turns it on in View.
                launchMonitorDelegate: Component { PpLaunchMonitorPanel {} }
                // Wrist motion analysis — the detailed Tier-1 assessment surface (per-DOF
                // trajectory strips, position×phase grid, findings). A normal stage panel:
                // place/arrange it via the View control like camera/charts/table.
                wristMotionDelegate: Component { WristDiagnostics {} }
                // Table panel — read-only inspector of the focused swing.json. The
                // focused swing is the active replay, else the carousel's selection.
                tableDelegate: Component {
                    PpDataViewer {
                        sessionType: SessionController.Wrist
                        swingDir: root._focusedSwingDir
                    }
                }
                // Markup panel — ground-truth labelling of the focused swing. Only the
                // visible screen's panel drives the shared markupController (panelActive).
                markupDelegate: Component {
                    PpMarkupPanel {
                        sessionType: SessionController.Wrist
                        panelActive: root._screenActive
                        targetSwingDir: root._focusedSwingDir
                    }
                }
            }
        }

        // Session-shot carousel — keys mirror the Wrist goal vocabulary
        // (goalDefsByType[1]); the stub model supplies placeholder values. The replay
        // transport (play/step/speed) rides in the carousel's top strip for Replay and
        // Analyse — both drive the loaded swing's playhead; Capture has no replay so it
        // stays hidden there. Works even when the timeline panel is hidden.
        PpShotCarousel {
            id: wristCarousel
            Layout.fillWidth: true
            visible: ViewLayout.isPanelOn(SessionMode.mode, "carousel")
            transport: Component { PpReplayTransport {} }
            transportActive: root._transitMode
            metricKeys: ["leadWristFlexExt", "leadWristRadUln", "forearmPronation", "leadArmFlexion"]
            traceLabel: qsTr("LEAD-WRIST FLEXION · ADDRESS → IMPACT")
        }
    }

    Component {
        id: transitHorizComp
        PpTransitTimeline { orientation: "horizontal"; snapToPhases: appSettings.timelineSnapToPhases }
    }
    Component {
        id: transitVertComp
        PpTransitTimeline { orientation: "vertical"; snapToPhases: appSettings.timelineSnapToPhases }
    }
}
