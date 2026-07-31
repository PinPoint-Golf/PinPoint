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

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import PinPointStudio

// The trail: the chain you actually walked to get here. Its terminal item is ALWAYS the current
// selection, which is what makes it a trail rather than a history — a metric, the measure that
// reads it, the corridor that judges it, the characteristic that fires, read left to right.
//
// Clicking a step goes back to it and truncates everything after, so the trail never carries a
// future you have left.
//
// It is reset, not extended, whenever the author picks a row out of the table — see selectFresh() in
// DiagnosticModel.qml. Only a followed RELATIONSHIP is a step; choosing where to stand is the start of
// a different chain, and a breadcrumb that kept the old one would be describing a route nobody took.
//
// It is also the middle pane's HEADING (ADDENDUM-02, A2). It used to be a pill on the global toolbar
// labelled TRAIL, one band above a separate row that named the type — a breadcrumb and a pane title
// saying overlapping things in two places. Drawn as a heading, the terminal item IS the title, and
// with nothing walked yet it degrades to `fallbackLabel`, which is exactly what that second row said.
// So the pill, the eyebrow and the band all go, and nothing is lost.
Item {
    id: root

    property var    trail: []           // [{ type, id, label }]
    // What to show before anything has been walked: the type label, as the old header row rendered.
    property string fallbackLabel: ""

    signal stepPicked(string type, string id)

    implicitHeight: Theme.sp(24)
    implicitWidth:  layout.implicitWidth

    RowLayout {
        id: layout
        anchors.fill: parent
        spacing: Theme.sp(6)

        Text {
            visible: root.trail.length === 0
            text:    root.fallbackLabel
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzHeading
            font.weight:    Theme.fontBodyWeight
            color:          Theme.colorText
            elide:          Text.ElideRight
        }

        Repeater {
            model: root.trail
            delegate: RowLayout {
                id: step
                required property var modelData
                required property int index

                readonly property bool terminal: index === root.trail.length - 1

                spacing: Theme.sp(6)
                // Earlier steps give up their width first: what you are looking at NOW is the part
                // that must stay readable, so the terminal item keeps its room and the rest elide.
                Layout.maximumWidth: terminal ? Theme.sp(260) : Theme.sp(110)

                Text {
                    Layout.fillWidth: true
                    text: step.modelData.label
                    font.family:    Theme.fontBody
                    // The terminal item is the pane's title and is sized as one; the steps behind it
                    // are the path taken to it and stay subordinate. One control, two weights, so the
                    // breadcrumb and the heading are visibly one thing.
                    font.pixelSize: step.terminal ? Theme.fontSzHeading : Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    color: step.terminal ? Theme.colorText : Theme.colorText3
                    elide: Text.ElideRight

                    PpPressable {
                        hoverScale: 1.0
                        enabled:    !step.terminal
                        onClicked:  root.stepPicked(step.modelData.type, step.modelData.id)
                    }
                }

                Text {
                    visible: !step.terminal
                    text:    "→"
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }
        }

        Item { Layout.fillWidth: true }
    }
}
