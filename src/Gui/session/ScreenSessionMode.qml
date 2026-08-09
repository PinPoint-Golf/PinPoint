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

// Reusable session-mode screen (Swing, GRF and Coach rail screens; the Wrist
// screen has its own ScreenWrist). Hosts the persistent session toolbar then a
// View-driven layout — an optional timeline rail, the centre stage (camera tiles
// today; charts/table land later), and an optional shot carousel —
// each gated by the toolbar's View control (ViewLayout, per session type).
//
// Replay and Analyse use the Transit timeline, in one of two orientations
// (appSettings.timelineOrientation): HORIZONTAL lives in the top rail (opens down
// from the toolbar); VERTICAL lives in a fixed-width panel to the LEFT of the
// stage. Capture has no timeline (RmTimelineChart lives only on the resource monitor).

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root
    // Retained for Main.qml's per-instance labelling; the body is now View-driven.
    property string iconText:  ""
    property string titleText: ""
    // SessionController::Type of this screen (set per instance in Main.qml).
    property int sessionType: -1

    readonly property bool _timelineOn:  ViewLayout.isPanelOn(SessionMode.mode, "timeline")
    // Replay AND Analyse use the Transit timeline; Capture has no timeline rail.
    readonly property bool _transitMode: SessionMode.mode === SessionMode.replay
                                         || SessionMode.mode === SessionMode.analyse
    readonly property bool _vertical:    appSettings.timelineOrientation === "vertical"
    // All session screens stay alive in the StackLayout (this one at index
    // sessionType+1). The transit timeline's stationLayout recompute is heavy and
    // fires on every shotReplay span change, so keep it UNLOADED unless this screen
    // is visible — otherwise an off-screen timeline re-lays-out on every swing reload.
    readonly property bool _screenActive: navController.currentIndex === root.sessionType + 1

    // Ambient theme-reactive contour field behind the session layout, over the
    // window's Theme.colorBg. Subtle; pauses off-screen / backgrounded; respects
    // reduced-motion. Live camera tiles fully occlude it during Capture. Hover
    // lifts the terrain under the cursor; click/tap sends a ripple.
    PpTopoBackground {
        id: topo
        anchors.fill: parent
        z: -1
        colorLow:  Theme.gradientWarm
        colorMid:  Theme.gradientWarmLit
        colorHigh: Theme.gradientCool
        accentColor: Theme.colorAccent
        animated:  !appSettings.reduceMotion
        hoverPoint: topoHover.hovered
            ? Qt.point(topoHover.point.position.x / width,
                       topoHover.point.position.y / height)
            : Qt.point(-1, -1)
    }

    // Passive pointer handlers drive the background. DragThreshold keeps a passive
    // grab so the toolbar/stage controls still receive their clicks and drags.
    HoverHandler {
        id: topoHover
        enabled: topo.animated
    }
    TapHandler {
        enabled: topo.animated
        gesturePolicy: TapHandler.DragThreshold
        onTapped: (ep) => topo.ripple(ep.position.x / topo.width,
                                      ep.position.y / topo.height)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PpSessionToolbar {
            Layout.fillWidth: true
            sessionType: root.sessionType
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

            // Centre stage — camera tiles + the Replay charts trace; table falls back
            // to a muted PpStagePanel placeholder until its producer lands.
            // Packing (tabs/split/stage) is resolved by ViewLayout on the active mode.
            PpModeStage {
                Layout.fillWidth: true; Layout.fillHeight: true
                cameraDelegate: Component {
                    PpCameraTiles { sessionType: root.sessionType }
                }
                chartsDelegate: Component { PpReplayCharts { sessionType: root.sessionType } }
                // Session diagnostics — what KEEPS happening across this session's shots,
                // and what the model authors as its cause. Gated on available data like
                // everything else, never on sessionType; off until the user asks for it in
                // View, because it owns a characteristic pack and a detection thread nobody
                // who never opens it should pay for.
                sessionDiagnosticsDelegate: Component { PpSessionDiagnosticsPanel {} }
                // Launch monitor session board — per SESSION and neutral. Gated on
                // available data and devices, never on sessionType: a monitor is a
                // monitor whatever the screen was opened to do. Off until the user
                // asks for it in View.
                launchMonitorDelegate: Component { PpLaunchMonitorPanel {} }
                // Table panel — read-only inspector of the focused swing.json. The
                // focused swing is the active replay, else the carousel's selection.
                tableDelegate: Component {
                    PpDataViewer {
                        sessionType: root.sessionType
                        swingDir: shotReplay.swingDir !== "" ? shotReplay.swingDir
                                : (modeCarousel.selectedCard ? modeCarousel.selectedCard.swingDir : "")
                    }
                }
                // Markup panel — ground-truth labelling of the focused swing. Only the
                // visible screen's panel drives the shared markupController (panelActive).
                markupDelegate: Component {
                    PpMarkupPanel {
                        sessionType: root.sessionType
                        panelActive: root._screenActive
                        targetSwingDir: shotReplay.swingDir !== "" ? shotReplay.swingDir
                                : (modeCarousel.selectedCard ? modeCarousel.selectedCard.swingDir : "")
                    }
                }
            }
        }

        // Session-shot carousel — stub model for now (per-mode goal vocabulary
        // lands with the real analyzers). The replay transport (play/step/speed) rides
        // in the carousel's top strip for Replay and Analyse; Capture has no replay so
        // it stays hidden there. Works even when the timeline panel is hidden.
        PpShotCarousel {
            id: modeCarousel
            Layout.fillWidth: true
            visible: ViewLayout.isPanelOn(SessionMode.mode, "carousel")
            transport: Component { PpReplayTransport {} }
            transportActive: root._transitMode
            metricKeys: []
            traceLabel: qsTr("SESSION SHOTS")
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
