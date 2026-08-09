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

// Muted placeholder for a stage panel whose producer hasn't landed yet
// (Charts / Table). Replace with the real component when ready.

import QtQuick
import PinPointStudio

Rectangle {
    property string title: ""
    radius: Theme.radius
    color: Theme.colorBg2
    border.width: 1; border.color: Theme.colorBorderMid

    Text {
        anchors { top: parent.top; left: parent.left; margins: Theme.sp(10) }
        text: title
        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
        font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
    }
    Text {
        anchors.centerIn: parent
        text: qsTr("Coming soon")
        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
        color: Theme.colorText3
    }
}
