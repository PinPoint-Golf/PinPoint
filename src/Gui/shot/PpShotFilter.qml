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

// Filter popover content — traffic-light quality bands (same colours as the
// card score pills), an exact star-rating picker (only shots rated exactly N;
// tap the same star again to clear), and a has-video toggle. Pure bindings to
// the carousel's ShotFilterProxyModel Q_PROPERTYs; the filtering itself lives
// in C++.
//
// ...and, below the filters, a SHOT PICKER that appears only when the host's film
// strip is folded away (showShots). With the cards up they ARE the selector and this
// menu is just the filter, which is all it has ever been; collapsed, there is no other
// way to reach a swing, and re-opening the strip to pick one defeats the fold. The
// picker is a contact sheet of ordinals tinted by the same quality ramp as the card
// pills — one glance finds the good ones, one click puts a swing on the stage, one
// click on the chip already on the stage takes it off again. It lists the rows of the
// model the filters drive, so a filtered-out swing is absent here exactly as it is
// absent from the strip: filtering is honoured by construction, not by a second rule.
// Ordinals alone would not identify a swing, so the readout line under the grid names
// whatever the cursor is over (falling back to the swing on the stage).

import QtQuick
import PinPointStudio

Item {
    id: root

    required property var proxy   // the carousel's ShotFilterProxyModel

    // ── Shot picker ──────────────────────────────────────────────────────────
    // Host drives this from its collapsed state; false leaves the panel exactly the
    // filter it was, with no section, no grid and no delegates.
    property bool showShots: false
    // The rows the picker lists. Defaults to the proxy — the same filtered set the
    // film strip shows — and is separable ONLY so a test can drive the list without
    // standing up a real proxy and its C++ source model.
    property var  shots: proxy
    // The swing on the stage: its chip carries the accent ring, and clicking it is
    // the deselect. -1 = nothing focused.
    property int  focusedShotId: -1
    // ShotListModel::shotSummary() of that swing — the readout's resting line, so the
    // picker can name the focused swing without holding a delegate for it (the chip
    // may be scrolled out of view, or filtered away entirely).
    property var  focusedSummary: ({})

    // One-click pick. The host TOGGLES — promote to the stage, or clear it if this is
    // already the focused swing — exactly as it does for a film-strip card; this panel
    // decides nothing and knows nothing of SessionMode.
    signal shotToggled(int shotId, string swingDir)

    // Bring the focused swing's chip into view — called by the host when the popover
    // opens, so a picked swing is never hidden below the fold of a long session.
    // Guarded: `shots` may be a plain model with no proxy behind it (tests).
    function positionAtFocused() {
        if (!root.showShots || root.focusedShotId < 0 || !root.proxy) return
        if (typeof root.proxy.visibleShotIds !== "function") return
        const i = root.proxy.visibleShotIds().indexOf(root.focusedShotId)
        if (i >= 0) shotGrid.positionViewAtIndex(i, GridView.Contain)
    }

    // The chip the cursor is over. Item-typed (not var) so QML nulls it when the
    // delegate is destroyed under a scroll — a stale readout would name a swing the
    // grid is no longer showing.
    property Item _hoverChip: null

    implicitWidth:  Theme.sp(256)
    implicitHeight: content.implicitHeight + Theme.sp(27)

    Column {
        id: content
        anchors { left: parent.left; right: parent.right; top: parent.top
                  leftMargin: Theme.sp(14); rightMargin: Theme.sp(14); topMargin: Theme.sp(13) }

        Item {   // header: FILTER · Clear all
            width: parent.width; height: clearText.implicitHeight
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text:           qsTr("FILTER")
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                color:          Theme.colorText3
            }
            Text {
                id: clearText
                anchors.right: parent.right
                text:           qsTr("Clear all")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          clearMa.containsMouse ? Qt.lighter(Theme.colorAccent, 1.08)
                                                      : Theme.colorAccent
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                opacity:        root.proxy.filterActive ? 1.0 : 0.45
                PpPressable {
                    id: clearMa
                    anchors.margins: -Theme.sp(4)
                    onClicked:       root.proxy.clearAll()
                }
            }
        }

        Item { width: 1; height: Theme.sp(13) }

        Text {
            text:           qsTr("QUALITY")
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingLabel
            color:          Theme.colorText3
        }

        Item { width: 1; height: Theme.sp(7) }

        Row {   // traffic-light band chips — exact band select, tap again to clear
            width: parent.width
            spacing: Theme.sp(6)

            Repeater {
                model: Theme.qualityBands

                Rectangle {
                    readonly property bool bandSelected: root.proxy.qualityLo === modelData.lo

                    width:  (parent.width - Theme.sp(18)) / 4
                    height: Theme.sp(32)
                    radius: Theme.radius
                    color:  bandSelected ? Theme.qualityColor(modelData.lo)
                          : bandMa.containsMouse
                                ? Qt.lighter(Theme.qualityColorLight(modelData.lo), 1.08)
                                : Theme.qualityColorLight(modelData.lo)
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    border.width: 1
                    border.color: bandSelected ? Theme.colorSurface : Theme.qualityColor(modelData.lo)

                    Rectangle {   // selection ring
                        anchors.fill: parent
                        anchors.margins: -Theme.sp(3)
                        radius:  Theme.radius + Theme.sp(2)
                        visible: bandSelected
                        color:   "transparent"
                        border.width: 1
                        border.color: Theme.qualityColor(modelData.lo)
                    }

                    Text {
                        anchors.centerIn: parent
                        text:           modelData.label
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          bandSelected ? (Theme.dark ? Theme.colorBg : "#FFFFFF")
                                                     : Theme.qualityColor(modelData.lo)
                    }

                    PpPressable {
                        id: bandMa
                        // Single atomic call — see ShotFilterProxyModel::setQualityBand.
                        onClicked: root.proxy.setQualityBand(bandSelected ? -1 : modelData.lo,
                                                             bandSelected ? -1 : modelData.hi)
                    }
                }
            }
        }

        Item { width: 1; height: Theme.sp(13) }

        Text {
            text:           qsTr("RATING")
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingLabel
            color:          Theme.colorText3
        }

        Item { width: 1; height: Theme.sp(7) }

        Item {   // exact star-rating picker + hint
            width: parent.width; height: ratingStars.implicitHeight
            PpStarRating {
                id: ratingStars
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                interactive: true
                value:       root.proxy.ratingFilter
                starSize:    Theme.sp(19)
                spacing:     Theme.sp(4)
                onRated: (n) => root.proxy.ratingFilter = n
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text:           root.proxy.ratingFilter > 0 ? qsTr("Exactly %1★").arg(root.proxy.ratingFilter)
                                                            : qsTr("Any")
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }
        }

        Item { width: 1; height: Theme.sp(13) }

        Rectangle {   // hairline before the toggle group
            width: parent.width; height: 1
            color: Theme.colorBorderMid
            opacity: Theme.borderOpacityNormal
        }

        Item { width: 1; height: Theme.sp(13) }

        Item {   // has-video toggle (the CamerasPanel TogglePill idiom)
            width: parent.width; height: toggle.height
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text:           qsTr("Has video only")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText
            }
            Rectangle {
                id: toggle
                anchors.right: parent.right
                width:  Theme.sp(34)
                height: Theme.sp(18)
                radius: Theme.sp(9)
                color:  root.proxy.hasVideoOnly ? Theme.colorAccent : Theme.colorBg3
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                Rectangle {
                    width:  Theme.sp(12)
                    height: Theme.sp(12)
                    radius: Theme.sp(6)
                    color:  "white"
                    anchors.verticalCenter: parent.verticalCenter
                    x: root.proxy.hasVideoOnly ? parent.width - width - Theme.sp(3) : Theme.sp(3)
                    Behavior on x { NumberAnimation { duration: Theme.durationFast } }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape:  Qt.PointingHandCursor
                    onClicked:    root.proxy.hasVideoOnly = !root.proxy.hasVideoOnly
                }
            }
        }

        // ── SHOTS — the picker that stands in for the folded film strip ──────
        //    Every item below is gated on showShots, and a Column neither sizes nor
        //    positions an invisible child, so with the strip up the section adds no
        //    height; the grid is handed a null model then, so no chip is ever built.

        Item { width: 1; height: Theme.sp(13); visible: root.showShots }

        Rectangle {   // hairline — the picker is its own group, below the filters
            visible: root.showShots
            width: parent.width; height: 1
            color: Theme.colorBorderMid
            opacity: Theme.borderOpacityNormal
        }

        Item { width: 1; height: Theme.sp(13); visible: root.showShots }

        Text {
            visible:        root.showShots
            text:           qsTr("SHOTS")
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingLabel
            color:          Theme.colorText3
        }

        Item { width: 1; height: Theme.sp(7); visible: root.showShots }

        // Contact sheet of ordinals, newest first — the film strip's own order, so the
        // chip on the left is the card on the left. Five to a row and capped at five
        // rows: twenty-five swings at a glance, and a long session scrolls rather than
        // pushing the popover past the top of the window.
        Item {
            id: gridBand
            visible: root.showShots
            width:   parent.width
            height:  root.showShots
                         ? Math.max(shotGrid.cellHeight,
                                    Math.min(_rows, 5) * shotGrid.cellHeight)
                         : 0
            readonly property int _rows: Math.ceil(shotGrid.count / 5)

            GridView {
                id: shotGrid
                objectName: "shotPickerGrid"
                anchors.fill: parent
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.showShots ? root.shots : null
                cellWidth:  Math.floor(gridBand.width / 5)
                cellHeight: Theme.sp(29)

                // Nothing survived the filter — say so, rather than showing a blank band
                // that reads as a picker that failed to load.
                Text {
                    anchors.centerIn: parent
                    visible: shotGrid.count === 0
                    text:    root.proxy && root.proxy.filterActive
                                 ? qsTr("No shots match the filter")
                                 : qsTr("No shots yet")
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                    color: Theme.colorText3
                }

                delegate: Item {
                    id: cell

                    required property int    shotId
                    required property int    ordinal
                    required property string club
                    required property string timestampLabel
                    required property int    score
                    required property string swingDir

                    width:  shotGrid.cellWidth
                    height: shotGrid.cellHeight

                    // On the stage: filled in its quality colour and ringed in the accent,
                    // the film-strip card's own selection treatment at chip scale.
                    readonly property bool picked: cell.shotId === root.focusedShotId

                    Rectangle {
                        id: chip
                        anchors.centerIn: parent
                        width:  shotGrid.cellWidth  - Theme.sp(6)
                        height: shotGrid.cellHeight - Theme.sp(6)
                        radius: Theme.radius
                        color:  cell.picked      ? Theme.qualityColor(cell.score)
                              : chipMa.containsMouse
                                    ? Qt.lighter(Theme.qualityColorLight(cell.score), 1.08)
                                    : Theme.qualityColorLight(cell.score)
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                        Rectangle {   // selection ring (the quality-band chips' idiom)
                            anchors.fill: parent
                            anchors.margins: -Theme.sp(2)
                            radius:  Theme.radius + Theme.sp(2)
                            visible: cell.picked
                            color:   "transparent"
                            border.width: 1
                            border.color: Theme.colorAccent
                        }

                        Text {
                            anchors.centerIn: parent
                            text:           cell.ordinal
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          cell.picked ? (Theme.dark ? Theme.colorBg : "#FFFFFF")
                                                        : Theme.qualityColor(cell.score)
                        }

                        PpPressable {
                            id: chipMa
                            onClicked: root.shotToggled(cell.shotId, cell.swingDir)
                            // Publish/withdraw this chip as the readout's subject. The
                            // withdrawal is conditional because the pointer reaches the
                            // next chip before this one loses hover.
                            onContainsMouseChanged: {
                                if (containsMouse)              root._hoverChip = cell
                                else if (root._hoverChip === cell) root._hoverChip = null
                            }
                        }
                    }
                }
            }
        }

        Item { width: 1; height: Theme.sp(8); visible: root.showShots }

        // Readout — what the cursor is over, else what is on the stage, else the
        // invitation. Fixed height, so the panel never jumps as the pointer crosses
        // the grid.
        Item {
            visible: root.showShots
            width:   parent.width
            height:  Theme.sp(15)

            readonly property var   _sum:   root.focusedSummary
            readonly property bool  _onStage: !root._hoverChip
                                              && _sum && _sum.valid === true
            readonly property int   _score: root._hoverChip ? root._hoverChip.score
                                          : _onStage        ? _sum.score : -1

            Text {
                anchors { left: parent.left; right: scoreText.left
                          rightMargin: Theme.sp(8); verticalCenter: parent.verticalCenter }
                elide: Text.ElideRight
                text: {
                    const c = root._hoverChip
                    if (c)
                        return "#" + c.ordinal
                             + (c.club ? " · " + c.club : "")
                             + (c.timestampLabel ? " · " + c.timestampLabel : "")
                    if (parent._onStage)
                        return "#" + parent._sum.ordinal
                             + (parent._sum.club ? " · " + parent._sum.club : "")
                             + (parent._sum.timestampLabel ? " · " + parent._sum.timestampLabel : "")
                    return qsTr("Pick a shot to put it on the stage")
                }
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          (root._hoverChip || parent._onStage) ? Theme.colorText2
                                                                     : Theme.colorText3
            }
            Text {
                id: scoreText
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                visible:        parent._score >= 0
                text:           parent._score
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.qualityColor(parent._score)
            }
        }
    }
}
