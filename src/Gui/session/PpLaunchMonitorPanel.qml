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

// The launch monitor session board — every reading the connected device produced this
// session, banded, each tile showing this shot's value, the session mean beside it and
// the session SD as the ±, over a strip that puts the shot against its own spread.
//
// PER SESSION AND NEUTRAL, which is what makes it a stage panel of its own rather than
// a dashboard zone. PpDashboardPanel is per shot and opinionated; this answers "what
// did the device measure, and how repeatable was I", and that question needs no verdict
// to be worth asking. So: NO grading, no RAG colour, no corridors, no comparison
// against our own optical estimates. The only colour here is band identity.
//
// IT SIZES ITSELF TO THE SCREEN IT IS ON. This board's normal home is a bay TV read
// from across a hitting area, so a fixed tile size is the wrong answer twice over: on
// a large screen it wastes most of the glass and leaves the figures too small to read
// from the mat, and in a narrow split it would clip. Everything — tile, gutter,
// spacing, padding, strip, and every font — is one scale factor `k` away from the
// design's own proportions, and _fitFor() picks the largest k at which the WHOLE board
// still fits. See that function for why the column count is searched rather than
// derived. k never drops below 1: below that the board scrolls instead of shrinking,
// because a value too small to read is not a smaller board, it is a useless one.
//
// Every number and every string on it comes from LmSessionModel — this file positions
// and paints and does no arithmetic beyond fitting the grid.
//
// Off by default (ViewLayout.defaultLayout omits it): a golfer with no monitor must
// never be shown an empty board they did not ask for. View → Launch monitor turns it on.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Rectangle {
    id: root

    // Set by PpModeStage only for the muted placeholder path; the real panel titles
    // itself. Present so the two are interchangeable in the arranger's Loaders.
    property string title: qsTr("Launch monitor")

    radius: Theme.radius
    color: Theme.colorBg2
    border.width: 1
    border.color: Theme.colorBorderMid
    clip: true

    // The session-global carousel list. Aliased because binding `shotModel: shotModel`
    // inside the model would resolve to the model's OWN property, not the context one.
    readonly property var sessionShots: shotModel

    LmSessionModel {
        id: board
        shotModel:     root.sessionShots
        focusedShotId: SessionMode.focusedShotId
        connected:     launchMonitor.configured
        saving:        appSettings.saveLaunchMonitorData
        deviceName:    launchMonitor.deviceName
    }

    // ── the design's own proportions, at k = 1 ───────────────────────────────
    // The mock's pixels, and the only place they appear. Everything else is one of
    // these times `k`, so the board keeps its proportions at every size.
    readonly property int baseGutter:  Theme.sp(76)
    readonly property int baseTileW:   Theme.sp(150)
    readonly property int baseTileH:   Theme.sp(78)
    readonly property int baseGap:     Theme.sp(6)    // between tiles
    readonly property int baseBandGap: Theme.sp(8)    // between bands

    // The largest scale at which the whole board fits `w` × `h`, and the column count
    // that achieves it.
    //
    // THE COLUMN COUNT IS SEARCHED, NOT DERIVED, and it has to be: the two constraints
    // pull opposite ways. More columns means less width per tile (so a smaller k), but
    // also fewer wrapped rows (so a larger k). Neither end is the answer — with 25
    // tiles in five bands on a 16:9 bay screen the optimum sits in the middle, and the
    // only honest way to find it is to price every candidate. It is at most 9
    // iterations of arithmetic, on resize.
    //
    // A candidate that cannot fit its columns even at k = 1 is rejected outright rather
    // than accepted and clipped. If none fits — a very narrow split — the board falls
    // to one column and SCROLLS, which is the one degradation that never costs a digit.
    function _fitFor(w, h, counts) {
        const fallback = { k: 1, cols: 1, tileW: baseTileW, tileH: baseTileH,
                           gutter: baseGutter, gap: baseGap, bandGap: baseBandGap }
        const nb = counts ? counts.length : 0
        if (nb === 0 || w <= 0 || h <= 0)
            return fallback

        let widest = 0
        for (let i = 0; i < nb; ++i)
            widest = Math.max(widest, counts[i])
        if (widest <= 0)
            return fallback

        let best = null
        let bestK = 0
        for (let c = 1; c <= widest; ++c) {
            // Width-limited k: the gutter, c tiles and c-1 gaps, all scaling together.
            const kw = w / (baseGutter + c * baseTileW + (c - 1) * baseGap)
            if (kw < 1 && c > 1)
                continue                       // will not fit even at the design size

            // Height-limited k. A band of n tiles in c columns takes ceil(n/c) rows,
            // so the intra-band gaps number (rows - nb) across the whole board.
            let rows = 0
            for (let j = 0; j < nb; ++j)
                rows += Math.ceil(counts[j] / c)
            const denom = rows * baseTileH + (rows - nb) * baseGap + (nb - 1) * baseBandGap
            const kh = denom > 0 ? h / denom : kw

            const k = Math.max(1, Math.min(4, Math.min(kw, kh)))
            // >= so a tie goes to MORE columns: same legibility, less wasted glass.
            if (k < bestK)
                continue

            // FLOOR, not round, on every term the totals are built from. Rounding each
            // independently can push the sum a pixel or two past the budget k was
            // solved for, and the board would then scroll for want of two pixels —
            // the one outcome the fit exists to avoid. Flooring can only undershoot.
            const gutter = Math.floor(baseGutter * k)
            const gap    = Math.floor(baseGap * k)
            const tileH  = Math.floor(baseTileH * k)
            // When k is height-limited there is width left over; spend it on tile
            // WIDTH rather than leaving a margin, up to the point where a tile starts
            // reading as a letterbox rather than a card.
            const tileW  = Math.min(Math.max(Math.floor(baseTileW * k),
                                             Math.floor((w - gutter - (c - 1) * gap) / c)),
                                    Math.floor(tileH * 3.0))
            bestK = k
            best  = { k: k, cols: c, tileW: tileW, tileH: tileH,
                      gutter: gutter, gap: gap, bandGap: Math.floor(baseBandGap * k) }
        }
        return best ? best : fallback
    }

    // The header is chrome, not data. It scales so it does not look stranded beside a
    // board twice its size, but at a slower rate — a title that grows with the figures
    // competes with them.
    readonly property real headerK: Math.min(k, 1.6)

    // The fit is measured against the panel MINUS a fixed header reserve, not against
    // the body's actual height. That is deliberate and load-bearing: the header scales
    // with k, so feeding its real height back into the fit would be a binding loop
    // (fit → k → header height → body height → fit). Reserving the header's maximum
    // costs a few pixels of board and cannot oscillate.
    readonly property int headerReserve: Math.round(Theme.sp(34) * 1.6)
    readonly property var fit: root._fitFor(root.width - 2 * Theme.sp(10),
                                            root.height - headerReserve - Theme.sp(10),
                                            board.bandCounts)
    readonly property real k: fit.k
    function px(base) { return Math.round(base * root.k) }

    // ── header ───────────────────────────────────────────────────────────────
    Item {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: Math.round(Theme.sp(34) * root.headerK)

        Row {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
            anchors.leftMargin: Math.round(Theme.sp(12) * root.headerK)
            spacing: Math.round(Theme.sp(10) * root.headerK)

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("LAUNCH MONITOR")
                font.family: Theme.fontData
                font.pixelSize: Math.round(Theme.fontSzMicro * root.headerK)
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }
            // The scope, in words. A mean whose scope you cannot see is a number you
            // cannot use — and when the club is unknown this says "all clubs" rather
            // than quietly averaging a driver into a wedge.
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: board.emptyText === "" && text !== ""
                text: board.scopeText
                font.family: Theme.fontBody
                font.pixelSize: Math.round(Theme.fontSzBody2 * root.headerK)
                color: Theme.colorText3
            }
        }

        // The legend for the strip. It says what the band and the tick ARE, because
        // nothing else on the tile can: a reader who thinks the band is a corridor has
        // been told the opposite of the truth.
        Text {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            anchors.rightMargin: Math.round(Theme.sp(12) * root.headerK)
            visible: board.emptyText === ""
            text: qsTr("band ±1 SD · tick %1").arg(board.valueLabel)
            font.family: Theme.fontBody
            font.pixelSize: Math.round(Theme.fontSzBody2 * root.headerK)
            color: Theme.colorText3
            elide: Text.ElideRight
            width: Math.min(implicitWidth, root.width * 0.4)
        }
    }

    // ── empty and degraded states ────────────────────────────────────────────
    // One line, no icon, no illustration. Which line is LmSessionModel's decision:
    // "no monitor", "not saving", "nothing yet" and "nothing for this club" are four
    // different problems with four different fixes, and only the model knows which.
    Text {
        anchors.centerIn: parent
        width: parent.width - Theme.sp(40)
        visible: board.emptyText !== ""
        text: board.emptyText
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
        color: Theme.colorText3
    }

    // ── body ─────────────────────────────────────────────────────────────────
    Flickable {
        id: body
        anchors {
            top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom
            leftMargin: Theme.sp(10); rightMargin: Theme.sp(10); bottomMargin: Theme.sp(10)
        }
        visible: board.emptyText === ""
        clip: true
        contentHeight: bands.implicitHeight
        contentWidth: width
        // Only when it does not fit — which, now that the board sizes itself, is only
        // the narrow-split case. A panel that scrolls when it has no need to feels
        // broken.
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        ColumnLayout {
            id: bands
            width: body.width
            spacing: root.fit.bandGap

            Repeater {
                model: board

                // ── one band ──────────────────────────────────────────────────
                RowLayout {
                    id: bandRow
                    required property int index
                    required property string band
                    required property int count
                    required property var tiles

                    // Band identity, and nothing else in the panel is tinted. This
                    // palette's own header says it carries no fixed meaning, which is
                    // exactly why it is the right one here: the hue tells you which
                    // band you are reading, never whether the number is good.
                    readonly property color hue: Theme.chartSeriesColor(index)

                    Layout.fillWidth: true
                    spacing: root.fit.bandGap

                    // gutter — a rule in the band's hue, its name, its count
                    Item {
                        // fillHeight, not AlignTop: the rule spans the band's FULL
                        // height, which is what makes a band that wrapped onto three
                        // rows read as one band rather than as three.
                        Layout.preferredWidth: root.fit.gutter
                        Layout.fillHeight: true

                        Rectangle {
                            id: rule
                            anchors { top: parent.top; bottom: parent.bottom; left: parent.left }
                            width: root.px(Theme.sp(2))
                            radius: width / 2
                            color: bandRow.hue
                        }
                        Column {
                            anchors { left: rule.right; leftMargin: root.px(Theme.sp(8))
                                      top: parent.top; right: parent.right }
                            spacing: root.px(Theme.sp(2))
                            Text {
                                width: parent.width
                                text: bandRow.band.toUpperCase()
                                font.family: Theme.fontData
                                font.pixelSize: root.px(Theme.fontSzMicro)
                                font.letterSpacing: Theme.trackingMicro
                                color: bandRow.hue
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                // Spelled out rather than a %n plural: with no
                                // translation catalogue loaded Qt falls back to the
                                // SOURCE string, and this read "9 metric(s)".
                                text: bandRow.count === 1 ? qsTr("1 metric")
                                                          : qsTr("%1 metrics").arg(bandRow.count)
                                font.family: Theme.fontBody
                                font.pixelSize: root.px(Theme.fontSzMicro)
                                color: Theme.colorText3
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // the band's tiles — wrap onto extra rows; bands never share a row
                    Grid {
                        id: grid
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        columns: root.fit.cols
                        spacing: root.fit.gap

                        Repeater {
                            model: bandRow.tiles

                            Rectangle {
                                id: tile
                                required property var modelData

                                width: root.fit.tileW
                                height: root.fit.tileH
                                radius: root.px(Theme.radius)
                                color: Theme.colorSurface
                                border.width: 1
                                border.color: Theme.colorBorderMid

                                readonly property int padX: root.px(Theme.sp(10))
                                readonly property int padY: root.px(Theme.sp(7))
                                readonly property int fontMicro: root.px(Theme.fontSzMicro)

                                // EVERY NUMBER ON A TILE IS colorText2. colorText3 is
                                // chrome only (the header meta, the band counts) — on
                                // colorSurface it is 2.1:1 in Studio dark, which is
                                // illegible for a figure and cost a review round once.

                                // 1 — what it is, and the session mean beside it
                                Item {
                                    id: topRow
                                    anchors { top: parent.top; left: parent.left; right: parent.right
                                              topMargin: tile.padY
                                              leftMargin: tile.padX; rightMargin: tile.padX }
                                    height: meanTxt.implicitHeight

                                    Text {
                                        id: meanTxt
                                        anchors.right: parent.right
                                        text: tile.modelData.mean
                                        font.family: Theme.fontData
                                        font.pixelSize: tile.fontMicro
                                        color: Theme.colorText2
                                    }
                                    Text {
                                        anchors { left: parent.left; right: meanTxt.left
                                                  rightMargin: root.px(Theme.sp(6)) }
                                        text: tile.modelData.abbrev
                                        font.family: Theme.fontData
                                        font.pixelSize: tile.fontMicro
                                        font.letterSpacing: Theme.trackingMicro
                                        color: Theme.colorText2
                                        elide: Text.ElideRight
                                    }
                                }

                                // 3 — the dispersion strip (anchored first; row 2 hangs off it)
                                Item {
                                    id: strip
                                    anchors { bottom: parent.bottom; left: parent.left
                                              right: parent.right
                                              bottomMargin: tile.padY
                                              leftMargin: tile.padX; rightMargin: tile.padX }
                                    height: root.px(Theme.sp(3))
                                    // No spread ⇒ no strip. A ±1 SD band drawn from two
                                    // shots is a lie about how repeatable the golfer is.
                                    visible: tile.modelData.hasSpread

                                    Rectangle {         // track
                                        anchors.fill: parent
                                        radius: height / 2
                                        color: Qt.rgba(Theme.colorText.r, Theme.colorText.g,
                                                       Theme.colorText.b, 0.07)
                                    }
                                    Rectangle {         // ±1 SD of a ±3 SD axis — a literal
                                        x: parent.width / 3         // constant, not a computed width
                                        width: parent.width / 3
                                        height: parent.height
                                        radius: height / 2
                                        color: Qt.rgba(Theme.colorText.r, Theme.colorText.g,
                                                       Theme.colorText.b, 0.17)
                                    }
                                    Rectangle {         // this shot
                                        visible: tile.modelData.hasLatest
                                        width: root.px(Theme.sp(2))
                                        height: parent.height + root.px(Theme.sp(6))
                                        y: -root.px(Theme.sp(3))
                                        x: parent.width * tile.modelData.tickPct / 100 - width / 2
                                        radius: width / 2
                                        color: bandRow.hue
                                    }
                                }

                                // 2 — the reading, its unit, and the spread it sits in
                                Item {
                                    anchors { left: parent.left; right: parent.right
                                              bottom: strip.top
                                              bottomMargin: root.px(Theme.sp(6))
                                              leftMargin: tile.padX; rightMargin: tile.padX }
                                    height: valueTxt.implicitHeight

                                    Text {
                                        id: sdTxt
                                        anchors { right: parent.right; baseline: valueTxt.baseline }
                                        // The tag STAYS when there is no spread and reads
                                        // as an em dash. Hiding it reflows the row and
                                        // answers a question the reader did ask.
                                        text: tile.modelData.sd
                                        font.family: Theme.fontData
                                        font.pixelSize: tile.fontMicro
                                        color: Theme.colorText2
                                    }
                                    Text {
                                        id: unitTxt
                                        anchors { right: sdTxt.left
                                                  rightMargin: root.px(Theme.sp(6))
                                                  baseline: valueTxt.baseline }
                                        text: tile.modelData.unit
                                        font.family: Theme.fontData
                                        font.pixelSize: tile.fontMicro
                                        color: Theme.colorText2
                                    }
                                    Text {
                                        id: valueTxt
                                        anchors { left: parent.left; right: unitTxt.left
                                                  rightMargin: root.px(Theme.sp(6)) }
                                        text: tile.modelData.latest
                                        font.family: Theme.fontData
                                        font.pixelSize: root.px(Theme.fontSzDataLg)
                                        font.letterSpacing: -1.1 * root.k
                                        color: Theme.colorText
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
