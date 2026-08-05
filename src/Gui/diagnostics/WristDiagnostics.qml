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

// Wrist Motion diagnostics — the read-only Tier-1 surface for a Wrist session's Analyse view
// (design §8). It is fed the focused swing's real analysis (shotReplay.analysisDetail) and tracks
// the replay playhead; the selected checkpoint comes from the transit timeline, so there is no
// in-panel scrubber. Headline score pill, per-DOF trajectory strips (band corridor + player line +
// RAG markers + consequence poles), the collapsible position×phase grid, and a legend. Findings +
// "Working well" land in Phase 2. Pure binding.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    WristDiagnosticsModel {
        id: dx
        analysisDetail: shotReplay.analysisDetail
        playheadUs:     shotReplay.positionUs
        // The PREVIOUS swing is "the one before this in the list on screen", so it has to be
        // asked of the list on screen: the live carousel, or a loaded session's own shots
        // while reviewing. Asking the live model unconditionally meant every past session's
        // ghost came back empty — the replayed swing was not in that list to have a
        // predecessor in it.
        previousAnalysisDetail:  sessionReviewController.activeShots
                                     .previousAnalysisDetail(shotReplay.swingDir)
        // The REFERENCE is a benchmark, not a neighbour: it outlives the session it was
        // marked in, so the lookup falls through to the document on disk when no loaded
        // list holds it. Asked of the list on screen only because that is where a hit is
        // likeliest and costs no parse — the answer is the same from either.
        referenceAnalysisDetail: sessionReviewController.activeShots
                                     .analysisDetailForSwingDir(appSettings.wristReferenceSwingDir)
    }

    readonly property var _pos: dx.positions[dx.selectedPosition]

    function _ragColor(r) {
        return r === "green" ? Theme.colorRagGood
             : r === "amber" ? Theme.colorRagWatch
             : r === "red"   ? Theme.colorRagFault
             :                 Theme.colorRagNone
    }

    // ── Empty state (no analysed wrist swing) — mirrors PpReplayCharts ─────────────────
    Column {
        anchors.centerIn: parent
        spacing: Theme.sp(6)
        visible: !dx.hasData
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: shotReplay.active ? qsTr("No wrist metrics for this swing")
                                    : qsTr("Select a swing to review")
            color: Theme.colorText2; font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Wrist diagnostics show the analysed joint angles vs the expected corridor")
            color: Theme.colorText3; font.family: Theme.fontData
            font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        visible: dx.hasData
        contentHeight: body.height + Theme.sp(32)
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: body
            y: Theme.sp(16)
            x: Theme.sp(16)
            width: flick.width - Theme.sp(32)
            // Fill the panel vertically: when the content is shorter than the viewport the body
            // stretches to it (the GridLayout below absorbs the slack so the strips grow rather
            // than the dashboard hugging the top); when it is taller, it grows past and scrolls.
            height: Math.max(implicitHeight, flick.height - Theme.sp(32))
            spacing: Theme.sp(16)

            // Side-by-side strips/dock when there's room; stacked on a narrow panel.
            readonly property bool _wide: width > Theme.sp(680)

            // ── Header: title + phase context · score pill ──────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(1)
                    Text {
                        text: qsTr("Wrist Motion Dashboard")
                        font.family: Theme.fontDisplay; font.pixelSize: Theme.fontSzHeading
                        color: Theme.colorText
                    }
                    Row {
                        spacing: Theme.sp(2)
                        Text {
                            text: root._pos ? root._pos.name : ""
                            font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody2
                            font.weight: Font.Medium
                            color: Theme.colorText2
                        }
                        Text {
                            text: root._pos ? ("· " + root._pos.note) : ""
                            font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText3
                        }
                    }
                }
                WristScorePill {
                    Layout.alignment: Qt.AlignTop
                    score: dx.score
                    band: dx.scoreBand
                    breakdown: dx.scoreBreakdown
                    breakdownText: dx.scoreBreakdownText
                }
            }

            // ── Model (band archetype, Auto-detect) + Compare-to (ghost) controls ───
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(16)

                // Band model — Auto detects from the swing; the rest are manual overrides.
                RowLayout {
                    spacing: Theme.sp(6)
                    Text {
                        text: qsTr("Model")
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        font.capitalization: Font.AllUppercase; font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                    }
                    PpSegmentedControl {
                        Layout.preferredWidth: Theme.sp(230)
                        options: dx.archetypes                       // [Auto, Neutral, Bowed, Cupped]
                        selected: dx.archetypes[dx.archetype + 1]    // mode −1..2 → option index 0..3
                        onActivated: (v) => { dx.archetype = dx.archetypes.indexOf(v) - 1 }
                    }
                    Text {
                        visible: dx.archetype === -1
                        text: "→ " + dx.effectiveArchetypeName
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        color: Theme.colorText3
                    }
                }

                // Compare-to ghost.
                RowLayout {
                    spacing: Theme.sp(6)
                    Text {
                        text: qsTr("Compare")
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        font.capitalization: Font.AllUppercase; font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                    }
                    PpSegmentedControl {
                        Layout.preferredWidth: Theme.sp(210)
                        options: [qsTr("Address"), qsTr("Previous"), qsTr("Reference")]
                        selected: dx.compareTo === "previous"  ? qsTr("Previous")
                                : dx.compareTo === "reference" ? qsTr("Reference") : qsTr("Address")
                        onActivated: (v) => { dx.compareTo = (v === qsTr("Previous")  ? "previous"
                                                            : v === qsTr("Reference") ? "reference" : "address") }
                    }
                }

                // "Set as reference" toggle — marks the focused swing as the comparison reference.
                Rectangle {
                    id: refBtn
                    readonly property bool isRef: shotReplay.swingDir !== ""
                                                  && shotReplay.swingDir === appSettings.wristReferenceSwingDir
                    readonly property bool canSet: shotReplay.swingDir !== ""
                    implicitWidth: refRow.implicitWidth + Theme.sp(14)
                    implicitHeight: Theme.sp(28)
                    radius: Theme.radius
                    color: isRef ? Theme.colorAccentLight
                                 : refMa.containsMouse ? Theme.colorAccentMid : "transparent"
                    border.width: Theme.borderWidth
                    border.color: isRef ? Theme.colorAccentMid
                                        : refMa.containsMouse ? Theme.colorAccentMid : Theme.colorBorderMid
                    opacity: canSet ? 1.0 : 0.4
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
                    Row {
                        id: refRow
                        anchors.centerIn: parent
                        spacing: Theme.sp(3)
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "★"
                            font.family: Theme.fontSymbol; font.pixelSize: Theme.fontSzLabel
                            color: refBtn.isRef ? Theme.colorAccent : Theme.colorText3
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: refBtn.isRef ? qsTr("Reference") : qsTr("Set as reference")
                            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                            color: refBtn.isRef ? Theme.colorAccent : Theme.colorText2
                        }
                    }
                    PpPressable {
                        id: refMa
                        enabled: refBtn.canSet
                        onClicked: appSettings.wristReferenceSwingDir = (refBtn.isRef ? "" : shotReplay.swingDir)
                    }
                }

                Item { Layout.fillWidth: true }
            }

            // ── Resemblance readouts ─────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                visible: dx.resemblance && dx.resemblance.neutral !== undefined
                spacing: Theme.sp(8)
                Text {
                    text: qsTr("Archetype Similarity Index")
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.capitalization: Font.AllUppercase; font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText3
                }
                Text {
                    text: qsTr("Neutral %1%").arg(dx.resemblance.neutral !== undefined ? dx.resemblance.neutral : 0)
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody2
                    font.weight: dx.effectiveArchetype === 0 ? Font.Bold : Font.Normal
                    color: dx.effectiveArchetype === 0 ? Theme.colorAccent : Theme.colorText2
                }
                Text { text: "·"; color: Theme.colorText3 }
                Text {
                    text: qsTr("Bowed %1%").arg(dx.resemblance.bowed !== undefined ? dx.resemblance.bowed : 0)
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody2
                    font.weight: dx.effectiveArchetype === 1 ? Font.Bold : Font.Normal
                    color: dx.effectiveArchetype === 1 ? Theme.colorAccent : Theme.colorText2
                }
                Text { text: "·"; color: Theme.colorText3 }
                Text {
                    text: qsTr("Cupped %1%").arg(dx.resemblance.cupped !== undefined ? dx.resemblance.cupped : 0)
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody2
                    font.weight: dx.effectiveArchetype === 2 ? Font.Bold : Font.Normal
                    color: dx.effectiveArchetype === 2 ? Theme.colorAccent : Theme.colorText2
                }
                Item { Layout.fillWidth: true }
            }

            // ── Body: trajectory strips + grid (left) · Working-well + Findings dock (right) ──
            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: body._wide ? 2 : 1
                columnSpacing: Theme.sp(16)
                rowSpacing: Theme.sp(16)

                // Left — strips card + collapsible data grid.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: Theme.sp(16)

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: stripsCol.implicitHeight + Theme.sp(24)
                        color: Theme.colorSurface
                        border.width: Theme.borderWidth
                        border.color: Theme.colorBorder
                        radius: Theme.radiusLg

                        ColumnLayout {
                            id: stripsCol
                            anchors { fill: parent; margins: Theme.sp(12) }
                            spacing: Theme.sp(16)
                            Repeater {
                                model: dx.strips
                                delegate: DofTrajectoryStrip {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Layout.minimumHeight: implicitHeight
                                    strip: modelData
                                    positions: dx.positions
                                    selected: dx.selectedPosition
                                }
                            }
                        }
                    }

                    PositionAngleGrid {
                        Layout.fillWidth: true
                        gridRows: dx.gridRows
                        positions: dx.positions
                        selected: dx.selectedPosition
                    }
                }

                // Right — "Working well" leads the dock, then "Findings".
                ColumnLayout {
                    Layout.fillWidth: !body._wide
                    Layout.preferredWidth: body._wide ? Theme.sp(330) : 0
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.sp(16)

                    StrengthsList {
                        Layout.fillWidth: true
                        visible: dx.strengths.length > 0
                        strengths: dx.strengths
                        onSeek: (u) => shotReplay.seekToUs(u)
                    }
                    FindingsList {
                        Layout.fillWidth: true
                        findings: dx.findings
                        onSeek: (u) => shotReplay.seekToUs(u)
                    }
                }
            }

            // ── Legend ──────────────────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp(4)
                spacing: Theme.sp(16)
                Repeater {
                    model: [ { g: "●", t: qsTr("in range"), c: "green" },
                             { g: "▲", t: qsTr("watch"),    c: "amber" },
                             { g: "■", t: qsTr("fault"),    c: "red"   },
                             { g: "◆", t: qsTr("no data"),  c: "none"  } ]
                    delegate: Row {
                        required property var modelData
                        spacing: Theme.sp(2)
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.modelData.g
                            font.family: Theme.fontSymbol; font.pixelSize: Theme.fontSzMicro
                            color: root._ragColor(parent.modelData.c)
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.modelData.t
                            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzLabel
                            color: Theme.colorText2
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: qsTr("Δ from address · degrees")
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    color: Theme.colorText3
                }
            }
        }
    }
}
