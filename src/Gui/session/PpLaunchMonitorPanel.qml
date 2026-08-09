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
// PER SESSION, which is what makes it a stage panel of its own. It answers "what did
// the device measure, and how repeatable was I", and that question is worth asking
// whether or not anything is wrong.
// So the board's DEFAULT STATE IS SILENT: no comparison against our own optical estimates,
// no reassurance, no green, and the only standing colour is band identity.
//
// IT SPEAKS UP FOR EXACTLY ONE THING — a reading outside its corridor. The frame and the
// figure take the app's Watch amber, or its Action red, and nothing else on the tile
// changes. Three rules keep that from becoming grading by the back door:
//
//   · A reading INSIDE its corridor looks exactly like a reading with no corridor at all.
//     There is no Ideal colour and no Good colour, so the board says "this one is out"
//     and never "these others are fine" — which it has no standing to say about a metric
//     nobody has authored a norm for, and there are 18 of those.
//   · The colour and the ±1 SD strip mean DIFFERENT THINGS and are drawn in different
//     channels on purpose. The strip is the golfer against their own other shots — pure
//     geometry, no norm, unchanged — and the corridor is colour. Two shaded regions on one
//     tile would be the version of this that misreads, and neither this board nor the
//     schematics draw one.
//   · Only when the scope has a CLUB. An unknown club boards the whole session and says
//     "all clubs"; corridors are club-shaped, so there the board stays a plain readout.
//
// LmSessionModel resolves all of it — this file paints a string it is handed. See its
// gradesFor().
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
// THAT IS THE TILES BOARD'S RULE AND ONLY THE TILES BOARD'S. Graphics mode does the
// opposite deliberately — fixed type, scaled drawings — and the two are not inconsistent:
// a tile IS its figure, so growing the tile without growing the figure would leave a big
// card with small writing on it, while a schematic is a picture whose labels are captions
// on it. See PpLmGraphicsBody for the argument on that side.
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

    // THE SESSION ON SCREEN, not the live one: sessionReviewController.activeShots is
    // the same list the carousel is showing, so opening a past session boards THAT
    // session's readings. Binding the live `shotModel` context property instead drew the
    // live carousel over a loaded session — which, on a launch-monitor-only session
    // opened from the drawer, is an empty board over a folder full of readings.
    readonly property var sessionShots: sessionReviewController.activeShots
    readonly property bool reviewing:   sessionReviewController.reviewActive

    LmSessionModel {
        id: board
        shotModel:     root.sessionShots
        focusedShotId: SessionMode.focusedShotId
        // The two live-capture gates, and they answer for the LIVE session only — see
        // emptyText(). A saved session's readings are on disk whatever the device is
        // doing now.
        reviewing:     root.reviewing
        connected:     launchMonitor.configured
        saving:        appSettings.saveLaunchMonitorData
        deviceName:    launchMonitor.deviceName
        // The SAME setting MetricCatalog is bound to wherever it is hosted. A corridor
        // graded here against the default while the golfer has chosen Strict would put a
        // different colour on this panel than on the metric surfaces for one reading.
        gradePolicy:   appSettings.diagnosticsGradePolicy
        // Changes only how the two INFERRED reads are WORDED and which way the strike
        // face is drawn — never a reading. See lm_inferred_reads.h.
        leftHanded:    athleteController.currentHandedness === "Left"
    }

    // ── mode ─────────────────────────────────────────────────────────────────
    // TILES is the banded board; GRAPHICS is the same shot drawn on five schematics.
    // Neither is a subset of the other — both surface every metric the device reported.
    // Persisted per user rather than per session mode: which drawing of the same
    // readings a golfer prefers is a fact about them, not about what they are doing.
    readonly property bool graphicsMode: appSettings.lmPanelMode === "graphics"

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

    // THE HEADER DOES NOT SCALE WITH THE BOARD. It answers only to fontScale, like every
    // other band of chrome in the session. It is sp(44) rather than the carousel band's
    // sp(36) because it carries a real title now (fontSzHeading over a micro meta line,
    // the Diagnostic Model screen's pairing) instead of the ten-pixel tracked label that
    // made this panel the only content surface in the app whose name was smaller than its
    // own captions. sp(16) side margins and sp(22) controls are unchanged, so it still
    // lines up with the rail below it. An earlier version grew it with the board's own fit scale so it would
    // not look stranded beside big figures; on a bay TV that produced a title bar half
    // again too large, competing with the numbers it was captioning rather than framing
    // them. Chrome that grows with its content is not chrome.
    //
    // Fixing it also removes the reason the fit could not see the header's real height:
    // there is no longer a fit → k → header → body → fit cycle to break, so the reserve
    // below is simply the header, exactly.
    readonly property int headerReserve: Theme.sp(44)
    readonly property var fit: root._fitFor(root.width - 2 * Theme.sp(10),
                                            root.height - headerReserve - Theme.sp(10),
                                            board.bandCounts)
    readonly property real k: fit.k
    function px(base) { return Math.round(base * root.k) }

    // ── header ───────────────────────────────────────────────────────────────
    Item {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: root.headerReserve

        Row {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
            anchors.leftMargin: Theme.sp(16)
            spacing: Theme.sp(10)

            // THE PANEL'S TITLE, sized as a title. It was a ten-pixel tracked label, which
            // made this the only content surface in PinPoint whose name was smaller than
            // its own captions — set beside the Diagnostic Model's screen title it did not
            // read as the same application. It is the heading token now, paired with a
            // micro meta line exactly as that screen pairs "Diagnostic Model" with
            // "core v1.0.0 · schema 1" (DiagnosticModel.qml:721-733).
            //
            // fontSzHeading and NOT fontSzDisplay, deliberately. That screen is a screen;
            // this is one panel among several sharing a stage, and a 30 px serif title
            // inside it would be the loudest thing on a view it is not the subject of.
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Launch monitor")
                font.family: Theme.fontBody
                font.pixelSize: Theme.fontSzHeading
                font.weight: Theme.fontBodyWeight
                color: Theme.colorText
            }
            // The scope, in words. A mean whose scope you cannot see is a number you
            // cannot use — and when the club is unknown this says "all clubs" rather
            // than quietly averaging a driver into a wedge.
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: board.emptyText === "" && text !== ""
                text: board.scopeText
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText3
            }
        }

        Row {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            anchors.rightMargin: Theme.sp(16)
            spacing: Theme.sp(10)

            // What the tiles board's marks ARE. A reader who takes the ±1 SD band for a
            // corridor has been told the opposite of the truth, so the strip says so.
            // Graphics needs no such line: its geometry is labelled where it is drawn.
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: board.emptyText === "" && !root.graphicsMode
                text: qsTr("band ±1 SD · tick %1").arg(board.valueLabel)
                // Micro, like the scope on the other end of the band: two meta lines on
                // one header at two different sizes read as a mistake.
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText3
                elide: Text.ElideRight
                width: Math.min(implicitWidth, root.width * 0.4)
            }

            // WHAT THE COLOUR MEANS, and which corridors it came from. A separate Text
            // rather than more words on the line above, for two reasons: it applies to
            // BOTH modes where that one is the tiles board's strip legend only, and each
            // then elides on its own budget — appending it would have made the newest fact
            // on the header the first one to disappear in a narrow split.
            //
            // Absent unless a corridor actually resolved. A legend over a board showing no
            // colour is a promise the board is not keeping, and with 18 of the 25 readings
            // carrying no norm that is a state a golfer will genuinely be in.
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: board.emptyText === "" && board.corridorScope !== ""
                text: qsTr("colour: outside the %1 corridor").arg(board.corridorScope)
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText3
                elide: Text.ElideRight
                width: Math.min(implicitWidth, root.width * 0.4)
            }

            // THE standard selector (PpSegmentedControl), not a lookalike — the same
            // control the chart panel, the markup panel and the session toolbar use, so
            // a two-way choice looks and behaves the same wherever a golfer meets one.
            // Sized to the carousel header's own control height, sp(22), so a stage
            // panel's title bar and the rail below it read as one system. Appearance is
            // entirely the shared component's — only the box it fills is set here.
            PpSegmentedControl {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.sp(130)
                height: Theme.sp(22)
                options:  [qsTr("Tiles"), qsTr("Graphics")]
                selected: root.graphicsMode ? qsTr("Graphics") : qsTr("Tiles")
                onActivated: (v) => appSettings.lmPanelMode =
                                     (v === qsTr("Graphics")) ? "graphics" : "tiles"
            }
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

    // ── body: graphics ───────────────────────────────────────────────────────
    // A Loader, so the unused body is never built. The graphics view is five schematics
    // and a physics-modelled flight curve; a golfer reading the tiles board should not
    // be paying to keep it alive behind them.
    //
    // It takes the model's `graphics` map and NOTHING ELSE — see PpLmGraphicsBody for
    // why that decoupling is what makes the layout assertions cheap.
    Loader {
        anchors {
            top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom
            leftMargin: Theme.sp(10); rightMargin: Theme.sp(10); bottomMargin: Theme.sp(10)
        }
        active: root.graphicsMode && board.emptyText === ""
        visible: active
        sourceComponent: graphicsBodyComp
    }
    Component {
        id: graphicsBodyComp
        PpLmGraphicsBody { g: board.graphics }
    }

    // ── body: tiles ──────────────────────────────────────────────────────────
    Flickable {
        id: body
        anchors {
            top: header.bottom; left: parent.left; right: parent.right; bottom: parent.bottom
            leftMargin: Theme.sp(10); rightMargin: Theme.sp(10); bottomMargin: Theme.sp(10)
        }
        visible: board.emptyText === "" && !root.graphicsMode
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

                                // "" | "ideal" | "good" | "watch" | "action", from the
                                // model. Only the last two show — see the file header.
                                readonly property string grade:
                                    modelData.grade !== undefined ? modelData.grade : ""
                                readonly property bool flagged:
                                    grade === "watch" || grade === "action"
                                // The app's grading pair, not a pair of this panel's own.
                                // A golfer meets these two colours on the wrist grid and
                                // every corridor bar; a launch monitor tile is not the
                                // place to teach them a third meaning for a hue.
                                readonly property color flagColor:
                                    grade === "action" ? Theme.colorRagFault
                                                       : Theme.colorRagWatch

                                // THE FRAME SCALES WITH THE BOARD and the resting one does
                                // not. A 1 px hairline is the right weight for a border
                                // that only separates a tile from its neighbour, but this
                                // one carries a reading — on a bay TV at k = 3 it would be
                                // a third the apparent weight of the same mark on a laptop,
                                // which is exactly backwards for the screen it is for.
                                border.width: flagged ? Math.max(2, root.px(2)) : 1
                                border.color: flagged ? flagColor : Theme.colorBorderMid

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
                                        // THE SECOND CHANNEL, and the reason there are
                                        // two. A frame alone is a mark the eye reads at
                                        // the edge of a tile it may not be looking at; the
                                        // figure is what a reader is actually reading. The
                                        // mean and the SD beside it stay colorText2 — they
                                        // are the session, and the corridor judges this
                                        // shot.
                                        color: tile.flagged ? tile.flagColor : Theme.colorText
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
