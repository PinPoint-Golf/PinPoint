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

// PpToolPill — THE standard toolbar item: a glyph tile, a micro label, the current
// value and a ▾ affordance, with the shared hover/press motion. Was an inline
// `component ViewPill` private to PpSessionToolbar; extracted here so surfaces
// OUTSIDE the toolbar can present the same control rather than a lookalike that
// drifts. Every toolbar pill — View, Motion, Club, Cameras, IMUs — is this
// component.
//
// Anchoring convention for the popup a pill owns (see PpSessionToolbar):
//     parent: <pill>;  y: <pill>.height + Theme.sp(10);  x: <pill>.width - width
// Parenting to the pill is what makes the popup track it — positioning against a
// coordinate computed in some ancestor's space silently misaligns.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Rectangle {
    property string label: ""
    property bool   active: false
    // Generalized for MotionPill (Phase 4): icon glyph + micro label above
    // the value line, defaulting to the original View pill's values.
    property string glyph:      "▦"
    property string microLabel: qsTr("VIEW")
    // Optional corner dot on the glyph tile (ClubPill uses it as the taped-club
    // marker). Off by default so View/Motion pills are unaffected.
    property bool   badge:      false
    property color  badgeColor: Theme.colorGood
    signal clicked()

    Layout.alignment: Qt.AlignVCenter
    implicitWidth: vpRow.implicitWidth + Theme.sp(24)
    implicitHeight: Theme.sp(44)
    radius: Theme.radius
    color: vpMa.containsMouse ? Theme.colorBg3 : Theme.colorBg2
    border.width: 1
    border.color: active             ? Theme.colorAccent
                : vpMa.containsMouse ? Theme.colorAccentMid
                :                      Theme.colorBorderMid

    // Hover/press motion — the home-tile / shot-card language adapted to a
    // toolbar pill: a subtle scale (no vertical lift — a pill must not float
    // out of the bar) that grows on hover, holds while its popup is open, and
    // dips on press. OutCubic + Theme.durationFast keeps it calm and snappy
    // (a frequently-touched control grates if it lingers), and reduceMotion
    // zeroes it. Both colour endpoints are opaque (bg2↔bg3), so unlike the
    // home card there is no tint-flash to guard against.
    transformOrigin: Item.Center
    scale: vpMa.pressed              ? 0.97
         : (active || vpMa.containsMouse) ? 1.02
         :                             1.0

    Behavior on color        { ColorAnimation  { duration: Theme.durationFast } }
    Behavior on border.color { ColorAnimation  { duration: Theme.durationFast } }
    Behavior on scale        { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

    RowLayout {
        id: vpRow
        anchors { fill: parent; leftMargin: Theme.sp(11); rightMargin: Theme.sp(11) }
        spacing: Theme.sp(11)
        Item {
            Layout.preferredWidth: Theme.sp(34); Layout.preferredHeight: Theme.sp(34)
            Layout.alignment: Qt.AlignVCenter
            Rectangle {
                anchors.fill: parent; radius: Theme.radius; color: Theme.colorSurface
                Text {
                    anchors.centerIn: parent; text: glyph
                    font.family: Theme.fontSymbol; font.pixelSize: Theme.sp(18)
                    color: active ? Theme.colorAccent : Theme.colorText2
                }
            }
            Rectangle {   // optional taped-club marker dot
                visible: badge
                anchors { right: parent.right; top: parent.top
                          rightMargin: -Theme.sp(3); topMargin: -Theme.sp(3) }
                width: Theme.sp(11); height: Theme.sp(11); radius: width / 2
                color: badgeColor
                border.width: 1; border.color: Theme.colorBg2
            }
        }
        Column {
            Layout.alignment: Qt.AlignVCenter; spacing: Theme.sp(2)
            Text {
                text: microLabel; font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }
            Row {
                spacing: Theme.sp(4)
                Text {
                    text: label
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                    color: Theme.colorText
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"  // ▾
                    font.pixelSize: Theme.fontSzMicro; color: Theme.colorText2
                }
            }
        }
    }
    MouseArea {
        id: vpMa; anchors.fill: parent; hoverEnabled: true
        cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked()
    }
}
