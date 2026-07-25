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

// One shot card in the carousel film-strip. Video still when recorded (or a
// neutral placeholder until real thumbnails arrive), wrist-angle trace
// fallback for IMU-only shots. Overlays: ordinal chip, quality pill, and a
// read-only star row on a bottom scrim. The delegate's required properties
// are auto-filled from the ShotFilterProxyModel roles; the carousel also
// reads them back to feed the review panel, so all roles are declared even
// where this card doesn't render them.

import QtQuick
import QtQuick.Controls
import PinPointStudio

Rectangle {
    id: card

    required property int    shotId
    required property int    ordinal
    required property string timestampLabel
    required property string club
    required property bool   hasVideo
    required property url    thumbnailSource
    required property var    tracePoints
    required property int    score
    required property int    rating
    required property string note
    required property var    metrics
    required property var    analysisDetail
    required property string swingDir
    required property bool   dataWarning   // IMU re-fusion parity failed → not re-analysable

    property bool selected: false
    property bool hovered:  hover.hovered

    signal tapped()
    signal rated(int newValue)

    // Overlays sit on imagery, so the scrim is always dark with light content
    // regardless of theme — the same reasoning as the "#FFFFFF" pill-text
    // idiom (legibility over media beats theme adaptation).
    readonly property color scrimColor: Qt.rgba(0.08, 0.06, 0.04, 0.55)

    // ── Hover / select motion (mirrors the home tiles' language, adapted to a
    //    media card). The film-strip viewport is clipped and the card fills it
    //    exactly, so growing the card would crop its border at the strip edges.
    //    Instead the media leans toward the viewer (inner zoom, contained by the
    //    card's own clip) on hover, holds a touch larger when selected, and the
    //    whole card dips on press (scaling DOWN never clips). Durations come from
    //    Theme so reduceMotion zeroes them. ──
    readonly property real _mediaZoom: selected ? 1.06 : hovered ? 1.035 : 1.0
    readonly property real _cardScale: clickArea.pressed ? 0.97 : 1.0

    // 16:9 media aspect
    width:  Theme.sp(139)
    height: Math.round(width * 9 / 16)
    radius: Theme.radius
    clip:   true
    color:  Theme.colorBg
    transformOrigin: Item.Center
    scale: _cardScale
    Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

    // ── Media (leans toward the viewer on hover/select — an inner zoom kept
    //    inside the frame by the card's clip): video still → placeholder →
    //    IMU trace fallback ────────────────────────────────────────────────────
    Item {
        id: media
        anchors.fill: parent
        transformOrigin: Item.Center
        scale: card._mediaZoom
        Behavior on scale { NumberAnimation { duration: Theme.durationNormal; easing.type: Easing.OutCubic } }

        Image {
            anchors.fill: parent
            visible:  card.hasVideo && card.thumbnailSource.toString() !== ""
            source:   card.thumbnailSource
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
        }

        Rectangle {   // stub placeholder until real thumbnail extraction lands
            anchors.fill: parent
            visible: card.hasVideo && card.thumbnailSource.toString() === ""
            color:   Theme.colorBg3
            Text {
                anchors.centerIn: parent
                text:           "◑"
                font.family:    Theme.fontSymbol
                font.pixelSize: Math.round(Theme.sp(22) * Theme.symbolScale("◑"))
                color:          Theme.colorText3
            }
        }

        PpTrace {
            anchors {
                left: parent.left; right: parent.right
                verticalCenter: parent.verticalCenter; verticalCenterOffset: -Theme.sp(6)
                leftMargin: Theme.sp(7); rightMargin: Theme.sp(7)
            }
            height:  Theme.sp(34)
            visible: !card.hasVideo
            points:  card.tracePoints
        }

        Text {
            anchors { left: parent.left; bottom: parent.bottom
                      leftMargin: Theme.sp(7); bottomMargin: Theme.sp(22) }
            visible:        !card.hasVideo
            text:           qsTr("IMU ONLY")
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingLabel
            color:          Theme.colorText3
        }
    }

    // ── Overlays ─────────────────────────────────────────────────────────────
    Rectangle {   // ordinal chip, top-left
        anchors { left: parent.left; top: parent.top; margins: Theme.sp(6) }
        width:  ordinalText.implicitWidth + Theme.sp(10)
        height: ordinalText.implicitHeight + Theme.sp(3)
        radius: Theme.sp(4)
        color:  card.scrimColor
        Text {
            id: ordinalText
            anchors.centerIn: parent
            text:           "#" + card.ordinal
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color:          "#FFFFFF"
        }
    }

    PpQualityPill {
        anchors { right: parent.right; top: parent.top; margins: Theme.sp(6) }
        score: card.score
    }

    Rectangle {   // bottom gradient scrim carrying the (clickable) stars
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: Theme.sp(23)
        z: 1   // lift the star hit-areas above the full-card MouseArea below
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: card.scrimColor }
        }
        PpStarRating {
            anchors { left: parent.left; bottom: parent.bottom
                      leftMargin: Theme.sp(7); bottomMargin: Theme.sp(7) }
            value:       card.rating
            starSize:    Math.round(Theme.fontSzMicro * 1.5)
            offColor:    Qt.rgba(1, 1, 1, 0.45)   // off-stars over media scrim
            interactive: true                     // tap to rate without opening the panel
            onRated:     (newValue) => card.rated(newValue)
        }
    }

    Rectangle {   // border drawn over the media so it is never obscured
        anchors.fill: parent
        radius: card.radius
        color:  "transparent"
        border.width: 1
        border.color: card.selected ? Theme.colorAccent
                    : card.hovered  ? Theme.colorAccentMid
                    :                 Theme.colorBorderMid
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
    }

    // IMU data-integrity warning (bottom-right): re-fusion parity failed, so the
    // recorded motion is internally inconsistent and the shot can't be re-analysed.
    Rectangle {
        id: dataWarnBadge
        visible: card.dataWarning
        anchors { right: parent.right; bottom: parent.bottom; margins: Theme.sp(6) }
        width:  warnGlyph.implicitWidth + Theme.sp(8)
        height: warnGlyph.implicitHeight + Theme.sp(4)
        radius: Theme.sp(4)
        color:  card.scrimColor
        z: 2    // above the star scrim (z:1) and the border

        Text {
            id: warnGlyph
            anchors.centerIn: parent
            text:           "⚠"            // ⚠ warning triangle with exclamation
            font.family:    Theme.fontSymbol
            font.pixelSize: Theme.sp(13)
            color:          Theme.colorWarn
        }

        HoverHandler { id: warnHover }
        ToolTip.visible: warnHover.hovered
        ToolTip.delay:   400
        ToolTip.text:    qsTr("IMU data integrity check failed — the recorded motion data is "
                            + "inconsistent (orientation re-fusion mismatch), so this shot "
                            + "cannot be re-analysed.")
    }

    // Hover lives on a HoverHandler (not the click MouseArea) so the interactive
    // star row below doesn't steal the card's hover state as the cursor crosses it.
    HoverHandler { id: hover }

    MouseArea {
        id: clickArea
        anchors.fill: parent
        cursorShape:  Qt.PointingHandCursor
        onClicked:    card.tapped()
    }
}
