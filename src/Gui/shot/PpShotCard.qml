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
//
// ...and, when a session diagnostics panel is up, one more: a PIP ROW — one pip per tracked
// condition for this swing, plus the count that fired (design 13a, brief §6.1). It is what
// makes the carousel the way INTO the panel post-session rather than a strip of thumbnails
// beside it: the row is that swing's whole diagnostic read at a glance, so a reader picks the
// shot they want to open instead of stepping through them. It costs a null check when no
// panel is up — see the Loader below.

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
    required property bool   dataWarning   // an integrity block warns (frames lost in capture, or IMU re-fusion)
    required property var    dataWarningDetail   // { capture, imu, framesLost, worstHoleMs, preImpact, … }

    // The ⚠ tooltip, worded from the facts. Capture holes first: they mean the swing
    // itself (or its follow-through) is missing frames, which is the more serious of
    // the two and the one the session assessment excludes the shot for.
    readonly property string dataWarningText: {
        const d = dataWarningDetail || {}
        const parts = []
        if (d.capture) {
            const where = d.preImpact ? qsTr("during the swing")
                                      : qsTr("after impact, so the follow-through positions are unreliable")
            parts.push(qsTr("Frames were lost during capture (%1 frames in %2 hole%3, worst %4 ms) %5.")
                       .arg(d.framesLost).arg(d.holes).arg(d.holes === 1 ? "" : "s")
                       .arg(Math.round(d.worstHoleMs)).arg(where))
        }
        if (d.imu)
            parts.push(qsTr("IMU data integrity check failed — the recorded motion data is "
                            + "inconsistent (orientation re-fusion mismatch), so this shot "
                            + "cannot be re-analysed."))
        parts.push(qsTr("This shot is not included in the session assessment."))
        return parts.join(" ")
    }

    // A shot only a launch monitor saw: no video AND every metric it carries is an lm.
    // reading. Derived rather than carried as a role, because it is already implied by
    // the data — and the alternative, defaulting a new property to false, would label a
    // device-only shot "IMU ONLY" on every card that forgot to set it.
    readonly property bool deviceOnly: {
        if (card.hasVideo) return false
        var keys = Object.keys(card.metrics || {})
        if (keys.length === 0) return false
        for (var i = 0; i < keys.length; ++i)
            if (keys[i].indexOf("lm.") !== 0) return false
        return true
    }

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
            visible: !card.hasVideo && !card.deviceOnly
            points:  card.tracePoints
        }

        // A device-only shot has no trace to draw — no pose, no IMU, nothing over time.
        // Without this the card is simply blank, which reads as a broken tile rather
        // than as a shot whose only witness was the monitor.
        Text {
            anchors.centerIn: parent
            visible:        card.deviceOnly
            text:           "◎"
            font.family:    Theme.fontSymbol
            font.pixelSize: Math.round(Theme.sp(22) * Theme.symbolScale("◎"))
            color:          Theme.colorText3
        }

        Text {
            anchors { left: parent.left; bottom: parent.bottom
                      leftMargin: Theme.sp(7)
                      // Steps up out of the pip row's band when there is one. The provenance
                      // label and the diagnostic read are both worth a line and neither is
                      // worth covering the other, and this card has exactly one clear band.
                      bottomMargin: pipBand.visible ? Theme.sp(38) : Theme.sp(22) }
            visible:        !card.hasVideo
            // "IMU ONLY" on a shot with no IMU would be a small lie in the one place a
            // reader looks to find out what produced it.
            text:           card.deviceOnly ? qsTr("MONITOR ONLY") : qsTr("IMU ONLY")
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

    // ── the swing's diagnostic read, when a session diagnostics panel is up ──
    //
    // ZERO COST WHEN IT IS NOT. `SessionMode.sessionDiagnostics` is null unless a panel has
    // claimed the seam (the user turns the panel on in View), and every binding inside the
    // Loader — pipsFor(), firedCountFor(), the Connections on surfaceChanged — lives in the
    // component that null does not instantiate. A card in a session with no panel evaluates
    // one null check and nothing else, which is the same trade the panel itself makes by not
    // being a context property in main.cpp.
    //
    // ABOVE THE STAR SCRIM AND BELOW THE ORDINAL CHIP. The card's overlays occupy three
    // corners already; the band immediately over the star scrim is the one clear strip across
    // the full width, and the row needs the width — nine pips split across Theme.sp(139) is
    // already the least it can be read at.
    Rectangle {
        id: pipBand
        anchors { left: parent.left; right: parent.right
                  bottom: parent.bottom; bottomMargin: Theme.sp(24) }
        height: Theme.sp(14)
        z: 2
        visible: pipLoader.item && pipLoader.item.count > 0
        // Over media, so the same always-dark scrim the other overlays use — and, on the
        // selected card, the design's accent wash. The BORDER is already the card's
        // selection treatment (below), so this is the only part of 13a's selected cell that
        // is missing; painting the wash over the whole still would fight the thumbnail the
        // card exists to show, and painting it here puts it exactly where the diagnostic
        // read is.
        color: card.selected ? Theme.colorAccentLight : card.scrimColor

        Loader {
            id: pipLoader
            anchors { fill: parent
                      leftMargin: Theme.sp(6); rightMargin: Theme.sp(6) }
            active: SessionMode.sessionDiagnostics !== null
            sourceComponent: pipComponent
        }
    }

    Component {
        id: pipComponent

        PpShotPipRow {
            id: pipRow

            // Recomputed on every republication of the ledger: a shot's pips are its own row
            // in a ledger that is re-reduced whenever another shot lands, and a row that went
            // stale would show a condition as clean that the session has since learned was
            // not assessable. `_rev` is the only thing the signal touches, and both reads
            // depend on it — reading it inside the condition rather than as a bare statement,
            // which the QML compiler drops.
            property int _rev: 0

            pips:       (SessionMode.sessionDiagnostics && pipRow._rev >= 0)
                        ? SessionMode.sessionDiagnostics.pipsFor(card.shotId) : []
            firedCount: (SessionMode.sessionDiagnostics && pipRow._rev >= 0)
                        ? SessionMode.sessionDiagnostics.firedCountFor(card.shotId) : 0

            Connections {
                target: SessionMode.sessionDiagnostics
                function onSurfaceChanged() { pipRow._rev = pipRow._rev + 1 }
            }
        }
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

    // Data-integrity warning (bottom-right): frames were lost during capture, or the
    // IMU re-fusion parity failed. Either way the recording is known broken, the
    // tooltip says which, and the session assessment leaves the shot out.
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
        ToolTip.text:    card.dataWarningText
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
