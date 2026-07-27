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

import QtQuick
import QtQuick.Layouts
import PinPointStudio

// PpTypePill — what KIND of thing this page is about, beside its title.
//
// The diagnostics library is six views over five kinds of object, and its detail pages hide the
// switcher bar: drill into anything and the page is a big title over stacked sections, which is
// the same shape whether you are looking at a characteristic, a measure or a metric. The reader
// who arrived by following a link two pages back has nothing on screen telling them which.
//
// OUTLINE, not filled, and that is the whole design. The filled capsules under these titles are
// ATTRIBUTES of the thing — its group, its status, its tier — and a sixth filled capsule saying
// "MEASURE" would read as a seventh attribute rather than as the class the other six belong to.
// An outline reads as a frame around the subject, which is what it is.
//
// Deliberately not `PpPill`: the design system reserves that name for the status pill
// (neutral/live/rec/warn/good, with the blinking REC dot), and two components differing only in
// what they mean is how a design system stops being one.
Rectangle {
    id: root

    property string label: ""

    implicitWidth:  pillText.implicitWidth + Theme.sp(18)
    implicitHeight: Theme.sp(22)          // the tag-capsule height, so the header keeps one rhythm
    radius:         height / 2
    color:          "transparent"
    border.width:   1
    border.color:   Theme.colorBorderMid

    Layout.alignment: Qt.AlignVCenter

    Text {
        id:               pillText
        anchors.centerIn: parent
        text:             root.label
        font.family:         Theme.fontData
        font.pixelSize:      Theme.fontSzMicro
        font.capitalization: Font.AllUppercase
        font.letterSpacing:  Theme.trackingMicro
        color:               Theme.colorText3
    }
}
