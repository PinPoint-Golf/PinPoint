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
import QtQuick.Controls.Basic
import PinPointStudio

// One corridor, in the measure's own units.
//
// THE WORDS mu, sigma AND z NEVER APPEAR. A coach setting a corridor is saying "a good stance is
// about this wide, give or take this much" — which is exactly what two handles on a number line
// express, and exactly what a pair of statistics spinboxes does not.
//
// ── The histogram is the safety mechanism, not decoration ──────────────────────────────────────
//
// Real swings sit under the band, and the running count updates on every drag: "31 Ideal · 8 Watch ·
// 3 Action". A corridor grading almost everything Action is visibly wrong to someone who has never
// heard of a standard deviation — that is the whole point, and it is why this view is not just two
// number fields.
//
// ── The handles bind the norm's own CLAIM ──────────────────────────────────────────────────────
//
// `_d.claimLo` / `_d.claimHi` — mu +/- one tolerance either side, which is what THIS NORM asserts.
// norm.h's claimLo()/claimHi() are authoritative and their comment says exactly this.
//
// The three bands DRAWN beneath them — Ideal, Good, Watch — are all consequences of the active
// grade policy, not second settings. Under `standard` the Ideal band lands exactly on the handles.
// Under `strict` it is narrower and the handles sit outside it; under `lenient`, wider. That is the
// policy visibly making more or less of the corridor than Ideal, and it is stated rather than
// hidden — before this, the Ideal band was drawn at the claim on every preset while grade() scaled
// it, so one number could produce a green band and an amber chip.
//
// All arithmetic is in NormEditorModel. This file maps values to pixels and back.
//
// All arithmetic is in NormEditorModel. This file maps values to pixels and back, and holds no
// rule about what a corridor means.
Item {
    id: root

    required property var editor      // NormEditorModel

    signal back()

    readonly property var  _d:    editor.draft || ({})
    readonly property var  _sum:  editor.sampleSummary || ({})
    readonly property var  _gc:   editor.gradeCounts || ({})
    readonly property string _unit: _d.unit || ""

    // One decimal is the resolution most corridors in the pack are authored at; more reads as false
    // precision on a figure that came from a heuristic. Right for the AXIS ENDS, which are a
    // computed frame rather than a stated number.
    function _num(v) { return Number(v).toFixed(1) }

    // A FIELD COMMITS WHAT IT SHOWS, so it may not round. PpTextField fires editingFinished on
    // focus loss as well as Enter, and the handler parses whatever text is in the box — so a field
    // displaying a rounded 1.5 for a corridor authored at 1.48 will SAVE 1.5 the moment the user
    // tabs past it, having touched nothing. Smash factor is authored 1.48 ± 0.05, and a tolerance
    // of 0.05 shown as "0.1" is a corridor drawn at twice its width.
    //
    // So: as many decimals as the value needs and no more, capped, which leaves every
    // one-decimal measure reading exactly as it did.
    function _fieldNum(v) {
        if (!isFinite(v)) return "0"
        for (var dp = 1; dp <= 4; ++dp) {
            var s = Number(v).toFixed(dp)
            if (Math.abs(parseFloat(s) - v) < 1e-9) return s
        }
        return Number(v).toFixed(4)
    }

    function _gradeColor(g) {
        if (g === "ideal")  return Theme.colorRagGood
        if (g === "good")   return Theme.colorRagGood
        if (g === "watch")  return Theme.colorRagWatch
        if (g === "action") return Theme.colorRagFault
        return Theme.colorRagNone
    }
    // Good shares Ideal's hue at lower weight: they are the two "nothing to say here" bands, and
    // giving Good a hue of its own would imply a third kind of verdict.
    function _gradeOpacity(g) { return g === "good" ? 0.45 : 1.0 }

    ScrollView {
        id: scroller
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   scroller.availableWidth - Theme.sp(64)
            spacing: Theme.sp(16)

            // ── Back ─────────────────────────────────────────────────────────
            Text {
                text:           "← " + qsTr("Back to the measure")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          backMa.containsMouse ? Theme.colorText : Theme.colorText3

                MouseArea {
                    id: backMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape:  Qt.PointingHandCursor
                    onClicked:    root.back()
                }
            }

            // ── Title ────────────────────────────────────────────────────────
            PpDisplayText { text: root._d.measureLabel || "" }

            Text {
                Layout.fillWidth: true
                text: root._d.overridden
                        ? qsTr("The corridor for %1 — yours, not the shipped one.")
                            .arg(root._d.contextLabel || "")
                    : root._d.own
                        ? qsTr("The corridor for %1, set here.").arg(root._d.contextLabel || "")
                        : (root._d.hasParent
                             ? qsTr("%1 currently inherits from %2. Saving makes this its own.")
                                 .arg(root._d.contextLabel || "").arg(root._d.inheritedFrom || "")
                             : qsTr("%1 has no corridor yet.").arg(root._d.contextLabel || ""))
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            // ── Refusal ──────────────────────────────────────────────────────
            //
            // A capture gap opens so the reason is readable, and refuses to save. A corridor on a
            // measure no sensor can produce cannot grade anything — it can only sit in the library
            // looking like coverage.
            Rectangle {
                Layout.fillWidth: true
                visible:        root._d.refused === true
                implicitHeight: refusedText.implicitHeight + Theme.sp(20)
                radius:         Theme.radius
                color:          Theme.colorBg2
                border.width:   1
                border.color:   Theme.colorAttention

                Text {
                    id: refusedText
                    x:     Theme.sp(12)
                    y:     Theme.sp(10)
                    width: parent.width - Theme.sp(24)
                    text:  qsTr("This measure cannot hold a corridor. %1")
                             .arg(root._d.refusedReason || "")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText
                    wrapMode:       Text.WordWrap
                }
            }

            // ── What a high value means, and which way the corridor opens ────
            //
            // Kept in view while dragging: which end of the number line is "more" is the one thing
            // an author must not have to guess at. See docs/design/pinpoint_sign_conventions.md.
            //
            // On a one-sided measure this becomes the SHAPE line instead, with highMeans folded
            // into it by the model — "Higher is better: more of the clubhead's speed reaching the
            // ball" reads as a reason, where a bare "floor" beside a bare "Higher means" reads as
            // two facts the author has to join up themselves. Read-only either way: shape is a
            // property of the measure, and this screen edits a norm.
            Text {
                Layout.fillWidth: true
                readonly property bool _oneSided: root._d.oneSided === true
                visible: text.length > 0
                text: _oneSided ? (root._d.shapeNote || "")
                                : ((root._d.highMeans || "").length > 0
                                     ? qsTr("Higher means: %1").arg(root._d.highMeans) : "")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            // ══ The band ═════════════════════════════════════════════════════
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.sp(210)
                radius: Theme.radiusLg
                color:  Theme.colorBg2

                Item {
                    id: plot
                    x:      Theme.sp(28)
                    y:      Theme.sp(18)
                    width:  parent.width - Theme.sp(56)
                    height: parent.height - Theme.sp(70)

                    readonly property real axisLo: root._sum.axisLo !== undefined ? root._sum.axisLo : 0
                    readonly property real axisHi: root._sum.axisHi !== undefined ? root._sum.axisHi : 1
                    readonly property real span:   Math.max(axisHi - axisLo, 1e-9)

                    // The two mappings this file exists to provide. Everything else is C++.
                    function xOf(v)  { return (v - axisLo) / span * width }
                    function valOf(x) { return axisLo + Math.max(0, Math.min(1, x / width)) * span }

                    // Which handle is held, and which one a press here would take.
                    //   two-sided: "" | "lo" | "hi"     — the two ends of the claim
                    //   one-sided: "" | "mu" | "edge"   — the aspiration, and the tolerance
                    property string grabbed: ""
                    property string hovered: ""

                    readonly property bool oneSided: root._d.oneSided === true
                    readonly property bool lowOpen:  root._d.lowOpen  === true
                    readonly property bool highOpen: root._d.highOpen === true

                    readonly property real barsTop:    0
                    readonly property real barsHeight: height - Theme.sp(34)
                    readonly property real bandTop:    height - Theme.sp(30)
                    readonly property real bandHeight: Theme.sp(22)

                    // ── Band spans ───────────────────────────────────────────
                    //
                    // Drawn widest-first so each narrower band sits on top: Watch contains Good
                    // contains Ideal, and the nesting IS the grade rule made visible.
                    Rectangle {
                        x:      plot.xOf(root._d.watchLo || 0)
                        width:  Math.max(0, plot.xOf(root._d.watchHi || 0) - x)
                        y:      plot.bandTop
                        height: plot.bandHeight
                        color:  Theme.colorBandAmber
                        radius: Theme.sp(2)
                    }
                    Rectangle {
                        x:      plot.xOf(root._d.goodLo || 0)
                        width:  Math.max(0, plot.xOf(root._d.goodHi || 0) - x)
                        y:      plot.bandTop
                        height: plot.bandHeight
                        color:  Theme.colorBandGreen
                        radius: Theme.sp(2)
                    }
                    Rectangle {
                        x:      plot.xOf(root._d.idealLo || 0)
                        width:  Math.max(0, plot.xOf(root._d.idealHi || 0) - x)
                        y:      plot.bandTop
                        height: plot.bandHeight
                        color:  Theme.colorRagGood
                        opacity: 0.32
                        radius: Theme.sp(2)
                    }

                    // ── The open tail ────────────────────────────────────────
                    //
                    // Every band above ends at the aspiration on a one-sided norm, because that is
                    // where its numbers end. Ideal does NOT: past mu, a floor grades Ideal for
                    // ever. Drawn as green running off the end of the plot and fading out, never
                    // as a band terminating in a hard edge — an edge there would state a bound,
                    // and the entire claim of a one-sided norm is that there is not one.
                    //
                    // ADDITIVE, like the corridor bars: a target norm draws the three rectangles
                    // above and nothing else.
                    Rectangle {
                        visible: plot.highOpen
                        x:       plot.xOf(root._d.mu || 0)
                        width:   Math.max(0, plot.width - x)
                        y:       plot.bandTop
                        height:  plot.bandHeight
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0
                                           color: Qt.rgba(Theme.colorRagGood.r, Theme.colorRagGood.g,
                                                          Theme.colorRagGood.b, 0.32) }
                            GradientStop { position: 1.0
                                           color: Qt.rgba(Theme.colorRagGood.r, Theme.colorRagGood.g,
                                                          Theme.colorRagGood.b, 0.0) }
                        }
                    }
                    Rectangle {
                        visible: plot.lowOpen
                        x:       0
                        width:   Math.max(0, plot.xOf(root._d.mu || 0))
                        y:       plot.bandTop
                        height:  plot.bandHeight
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0
                                           color: Qt.rgba(Theme.colorRagGood.r, Theme.colorRagGood.g,
                                                          Theme.colorRagGood.b, 0.0) }
                            GradientStop { position: 1.0
                                           color: Qt.rgba(Theme.colorRagGood.r, Theme.colorRagGood.g,
                                                          Theme.colorRagGood.b, 0.32) }
                        }
                    }

                    // …and it is said in words as well as drawn. A fade is a convention, and a
                    // convention nobody has been taught is decoration.
                    Text {
                        visible: plot.oneSided
                        text:    root._d.openEndLabel || ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.italic:    true
                        color:          Theme.colorText3
                        y:              plot.bandTop + plot.bandHeight + Theme.sp(3)
                        x:              plot.highOpen ? Math.max(0, plot.width - width)
                                                      : 0
                    }

                    // ── Histogram ────────────────────────────────────────────
                    Repeater {
                        model: root.editor.histogram
                        delegate: Rectangle {
                            required property var modelData
                            // A count of zero draws nothing rather than a hairline: an empty bin is
                            // an absence, and a row of stubs across the axis reads as data.
                            visible: modelData.count > 0

                            readonly property real maxCount: {
                                var m = 1
                                var h = root.editor.histogram
                                for (var i = 0; i < h.length; ++i)
                                    m = Math.max(m, h[i].count)
                                return m
                            }

                            x:     plot.xOf(modelData.lo)
                            width: Math.max(Theme.sp(2), plot.xOf(modelData.hi) - plot.xOf(modelData.lo) - 1)
                            height: plot.barsHeight * (modelData.count / maxCount)
                            y:     plot.barsTop + plot.barsHeight - height

                            color:   root._gradeColor(modelData.grade)
                            opacity: root._gradeOpacity(modelData.grade)
                            radius:  Theme.sp(1)

                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }
                    }

                    // ── Centre mark ──────────────────────────────────────────
                    //
                    // A static tick on a target norm: mu there is a CONSEQUENCE of where the two
                    // handles are, and offering to drag it would be offering to move two things
                    // with one gesture.
                    //
                    // On a one-sided norm it is the headline and it is draggable. "At least 1.48"
                    // IS the corridor — the tolerance is the secondary number — so mu has to be
                    // the thing under your finger. Drawn as a handle when it is one, because a
                    // hairline that responds to a drag is a control nobody will find.
                    Rectangle {
                        visible: !plot.oneSided
                        x:       plot.xOf(root._d.mu || 0)
                        y:       plot.bandTop - Theme.sp(4)
                        width:   1
                        height:  plot.bandHeight + Theme.sp(8)
                        color:   Theme.colorText3
                    }
                    Item {
                        id: muHandle
                        visible: plot.oneSided
                        readonly property bool lit: plot.grabbed === "mu" || plot.hovered === "mu"

                        width:  Theme.sp(44)
                        height: plot.bandHeight + Theme.sp(26)
                        x:      plot.xOf(root._d.mu || 0) - width / 2
                        y:      plot.bandTop - Theme.sp(13)

                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y:      0
                            width:  Theme.sp(3)
                            height: parent.height
                            color:  muHandle.lit ? Theme.colorAccent : Theme.colorText
                        }
                    }

                    // ── The two handles ──────────────────────────────────────
                    //
                    // The marks are pure BINDINGS on the model — no drag.target anywhere. Two
                    // reasons, and both were bugs:
                    //
                    //   * drag.target assigns x imperatively, which permanently breaks the
                    //     `x: xOf(value)` binding. After one drag the handle stops tracking the
                    //     corridor, so typing in the numeric field below moved the number and not
                    //     the mark.
                    //   * Mapping a handle-RELATIVE offset while the handle itself is being moved
                    //     by the model is a feedback path. Absolute plot coordinates have none.
                    //
                    // The grab is a single MouseArea over the strip (below), which is also how the
                    // 44 pt touch target is honoured without two overlapping hit areas fighting
                    // over the middle when the corridor is narrow.
                    //
                    // ONE element on a one-sided norm, and the dead one is ABSENT rather than
                    // disabled. A greyed handle invites "why can't I drag this?" on every single
                    // visit, and the honest answer — "that side of the corridor does not exist" —
                    // is not something a disabled state can say. Nothing is there because there is
                    // nothing there.
                    Repeater {
                        model: plot.oneSided ? [{ which: "edge" }]
                                             : [{ which: "lo" }, { which: "hi" }]
                        delegate: Item {
                            id: handle
                            required property var modelData
                            readonly property bool isLo: modelData.which === "lo"
                            readonly property real value: modelData.which === "edge"
                                                            ? (root._d.gradedEdge || 0)
                                                            : (isLo ? (root._d.claimLo || 0)
                                                                    : (root._d.claimHi || 0))
                            readonly property bool lit: plot.grabbed === modelData.which
                                                        || plot.hovered === modelData.which

                            width:  Theme.sp(44)
                            height: plot.bandHeight + Theme.sp(26)
                            x:      plot.xOf(value) - width / 2
                            y:      plot.bandTop - Theme.sp(13)

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                y:      0
                                width:  Theme.sp(2)
                                height: parent.height
                                color:  handle.lit ? Theme.colorAccent : Theme.colorText2
                            }
                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.top: parent.top
                                width:  Theme.sp(9)
                                height: Theme.sp(9)
                                radius: width / 2
                                color:  handle.lit ? Theme.colorAccent : Theme.colorText2
                            }
                        }
                    }

                    // ── The grab ─────────────────────────────────────────────
                    //
                    // One area over the whole strip, mapping ABSOLUTE plot x to a value. The drag
                    // is therefore exactly 1:1 with the pointer, and the editor latches the axis
                    // for the duration (beginHandleDrag) so the number line cannot move underfoot —
                    // without that the axis widens as the corridor widens and the same pixel keeps
                    // meaning a larger number, which ran the corridor away to absurd values in a
                    // fraction of a second.
                    MouseArea {
                        id: grabArea
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        y:              plot.bandTop - Theme.sp(13)
                        height:         plot.bandHeight + Theme.sp(26)
                        hoverEnabled:   true
                        enabled:        root._d.refused !== true
                        preventStealing: true
                        cursorShape:    (plot.grabbed !== "" || plot.hovered !== "")
                                            ? Qt.SizeHorCursor : Qt.ArrowCursor

                        // Which handle a press at this x would take, or "" when neither is close
                        // enough. Half the 44 pt target either side; the nearer wins a tie so a
                        // narrow corridor is still separable.
                        //
                        // One-sided: the two grabbable things are the aspiration and the graded
                        // edge, which is the same two-target problem with different names.
                        function pick(px) {
                            var r = Theme.sp(22)
                            var aName = plot.oneSided ? "mu"   : "lo"
                            var bName = plot.oneSided ? "edge" : "hi"
                            var dA = Math.abs(px - plot.xOf(plot.oneSided ? (root._d.mu || 0)
                                                                         : (root._d.claimLo || 0)))
                            var dB = Math.abs(px - plot.xOf(plot.oneSided ? (root._d.gradedEdge || 0)
                                                                         : (root._d.claimHi || 0)))
                            if (dA > r && dB > r) return ""
                            return dA <= dB ? aName : bName
                        }

                        onPositionChanged: function(mouse) {
                            if (plot.grabbed === "") { plot.hovered = pick(mouse.x); return }

                            var v = plot.valOf(mouse.x)

                            // One-sided: no swap to follow. The aspiration and its tolerance are
                            // independent numbers rather than two ends of one span, and the edge
                            // clamps ON the centre instead of crossing it (nudgeGradedEdge), so
                            // there is nothing for the pointer to lose hold of.
                            if (plot.oneSided) {
                                if (plot.grabbed === "mu") root.editor.setAspiration(v)
                                else                       root.editor.nudgeGradedEdge(v)
                                return
                            }

                            if (plot.grabbed === "lo") root.editor.nudgeClaimLo(v)
                            else                       root.editor.nudgeClaimHi(v)

                            // Dragging one handle through the other swaps them in C++. Follow the
                            // swap, or the pointer would carry on pushing the edge it no longer
                            // holds and the corridor would collapse to a point.
                            var dLo = Math.abs(v - (root._d.claimLo || 0))
                            var dHi = Math.abs(v - (root._d.claimHi || 0))
                            plot.grabbed = dLo <= dHi ? "lo" : "hi"
                        }
                        onExited: plot.hovered = ""
                        onPressed: function(mouse) {
                            var which = pick(mouse.x)
                            if (which === "") { mouse.accepted = false; return }
                            plot.grabbed = which
                            root.editor.beginHandleDrag()
                        }
                        onReleased: {
                            if (plot.grabbed === "") return
                            plot.grabbed = ""
                            root.editor.endHandleDrag()
                        }
                        onCanceled: {
                            if (plot.grabbed === "") return
                            plot.grabbed = ""
                            root.editor.endHandleDrag()
                        }
                    }
                }

                // ── Axis ends + running counts ───────────────────────────────
                RowLayout {
                    x:     Theme.sp(28)
                    width: parent.width - Theme.sp(56)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.sp(14)
                    spacing: Theme.sp(10)

                    Text {
                        text: root._num(root._sum.axisLo !== undefined ? root._sum.axisLo : 0)
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                    Item { Layout.fillWidth: true }

                    // The safety line. It is the reason this view exists rather than two spinboxes.
                    Text {
                        text: {
                            var g = root._gc
                            if (!g || (g.total || 0) === 0)
                                return qsTr("no swings drawn")
                            var bits = []
                            if (g.ideal)  bits.push(qsTr("%1 Ideal").arg(g.ideal))
                            if (g.good)   bits.push(qsTr("%1 Good").arg(g.good))
                            if (g.watch)  bits.push(qsTr("%1 Watch").arg(g.watch))
                            if (g.action) bits.push(qsTr("%1 Action").arg(g.action))
                            return bits.join("  ·  ")
                        }
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color:          Theme.colorText
                    }

                    Item { Layout.fillWidth: true }
                    Text {
                        text: root._num(root._sum.axisHi !== undefined ? root._sum.axisHi : 1)
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }
            }

            // ── Numeric readouts ─────────────────────────────────────────────
            //
            // Beside the handles, per the brief: a precise corridor must be reachable without
            // pixel-accurate dragging, and typing 102.5 is the only way to hit 102.5.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                // TWO FIELDS EITHER WAY, but not the same two.
                //
                //   target    NORMAL FROM [1.43]  TO [1.53]   — the two ends of the claim
                //   floor     AT LEAST    [1.48]  TOLERANCE [0.05]
                //   ceiling   NO MORE THAN[12.0]  TOLERANCE [2.0]
                //
                // A one-sided corridor has an aspiration and a slack, not two ends, and the pair
                // it does have is exactly what the two draggable marks above bind to. There is no
                // field for the ungraded side because there is no number there — claimHi on a
                // floor is mu plus a tolerance nothing grades.
                ColumnLayout {
                    spacing: Theme.sp(3)
                    Text {
                        // The field edits the norm's own claim, which is what the handle beside it
                        // drags. "NORMAL FROM … TO" rather than "IDEAL FROM": the Ideal band is a
                        // grade, and this control does not set a grade.
                        text: root._d.shape === "floor"   ? qsTr("AT LEAST")
                            : root._d.shape === "ceiling" ? qsTr("NO MORE THAN")
                            :                               qsTr("NORMAL FROM")
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                    }
                    PpTextField {
                        id: loField
                        readonly property bool _oneSided: root._d.oneSided === true
                        readonly property real _bound: _oneSided ? (root._d.mu || 0)
                                                                 : (root._d.claimLo || 0)
                        Layout.preferredWidth: Theme.sp(110)
                        enabled: root._d.refused !== true
                        text:    root._fieldNum(_bound)
                        onEditingFinished: {
                            var v = parseFloat(text)
                            if (!isNaN(v)) {
                                if (_oneSided) root.editor.setAspiration(v)
                                else           root.editor.nudgeClaimLo(v)
                            }
                            text = root._fieldNum(_bound)
                        }
                    }
                }

                ColumnLayout {
                    spacing: Theme.sp(3)
                    Text {
                        text: root._d.oneSided === true ? qsTr("TOLERANCE") : qsTr("TO")
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                    }
                    PpTextField {
                        id: hiField
                        readonly property bool _oneSided: root._d.oneSided === true
                        readonly property real _bound: _oneSided ? (root._d.tolerance || 0)
                                                                 : (root._d.claimHi || 0)
                        Layout.preferredWidth: Theme.sp(110)
                        enabled: root._d.refused !== true
                        text:    root._fieldNum(_bound)
                        onEditingFinished: {
                            var v = parseFloat(text)
                            if (!isNaN(v)) {
                                if (_oneSided) root.editor.setTolerance(v)
                                else           root.editor.nudgeClaimHi(v)
                            }
                            text = root._fieldNum(_bound)
                        }
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignBottom
                    Layout.bottomMargin: Theme.sp(8)
                    visible:        root._unit.length > 0
                    text:           root._unit
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText3
                }

                Item { Layout.fillWidth: true }

                // What the grade policy makes of it. Read-only on purpose — these edges are a
                // consequence of the setting above, not a second setting.
                //
                // The IDEAL edge is quoted here alongside Good and Watch, and it has to be: under
                // any preset but `standard` it is not where the handles are, and an author who
                // could see Good and Watch move with the policy but not Ideal would reasonably
                // conclude the green band was fixed. It is not, and it never was — it was only
                // ever drawn as though it were.
                // Composed in C++ (draft.policyNote) rather than assembled here: which of the three
                // forms applies is a statement about the measure's shape, and "action beyond
                // 1.3 – 1.5" on a floor would name a fault on the side that grades Ideal.
                Text {
                    Layout.alignment: Qt.AlignBottom
                    Layout.bottomMargin: Theme.sp(8)
                    text:           root._d.policyNote || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }

            PpDivider { Layout.fillWidth: true }

            // ══ Route ════════════════════════════════════════════════════════
            PpSegmentedControl {
                Layout.fillWidth: true
                Layout.maximumWidth: Theme.sp(420)
                options:  [qsTr("Set by hand"), qsTr("Seat from swings"), qsTr("Import")]
                selected: root._d.route === "seat"   ? qsTr("Seat from swings")
                        : root._d.route === "import" ? qsTr("Import")
                        :                              qsTr("Set by hand")
                onActivated: function(value) {
                    if (value === qsTr("Seat from swings"))    root.editor.route = "seat"
                    else if (value === qsTr("Import"))         root.editor.route = "import"
                    else                                       root.editor.route = "hand"
                }
            }

            // ── Set by hand ──────────────────────────────────────────────────
            Text {
                Layout.fillWidth: true
                visible: root._d.route === "hand"
                text: root._d.oneSided === true
                        ? qsTr("Drag the centre mark to set the figure to reach, and the handle "
                               + "to set how much slack there is on the graded side. The other "
                               + "side has no limit; everything wider follows from the grade "
                               + "policy in Measures & norms.")
                        : qsTr("Drag the two handles, or type the numbers. The band between them "
                               + "is what counts as ideal; everything wider follows from the grade "
                               + "policy in Measures & norms.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            // ── Seat from swings ─────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                visible: root._d.route === "seat"
                spacing: Theme.sp(10)

                Text {
                    Layout.fillWidth: true
                    // The one-sided variant says what the fit will and will not do, because "fit"
                    // on a measure with an unbounded good side is a different operation: the model
                    // reads the graded tail only, and there is no ungraded side to offer to seat.
                    text: root._d.oneSided === true
                            ? qsTr("Mark the swings you consider well positioned, then fit. The "
                                   + "fit reads the graded side only — the open side has no "
                                   + "limit to find. The selector below picks which swings are "
                                   + "drawn; it does not narrow who the corridor applies to.")
                            : qsTr("Mark the swings you consider well positioned, then fit. The "
                                   + "selector below picks which swings are drawn — it does not "
                                   + "narrow who the corridor applies to.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    wrapMode:       Text.WordWrap
                }

                // Draw-from
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.sp(6)

                    Repeater {
                        model: root.editor.drawFromOptions
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root.editor.drawFrom === modelData.name

                            implicitWidth:  dfText.implicitWidth + Theme.sp(20)
                            implicitHeight: Theme.sp(26)
                            radius: height / 2
                            color:  active ? Theme.colorAccent : Theme.colorBg2
                            opacity: modelData.enabled ? 1.0 : 0.45

                            Text {
                                id: dfText
                                anchors.centerIn: parent
                                text: qsTr("%1 (%2)").arg(modelData.label).arg(modelData.count)
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          parent.active ? Theme.colorBg : Theme.colorText2
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled:     modelData.enabled
                                cursorShape: Qt.PointingHandCursor
                                onClicked:   root.editor.drawFrom = modelData.name
                            }
                        }
                    }
                }

                // Scan state, said plainly. "Nothing here" and "not looked yet" are different
                // situations and a coach must not have to guess which one they are in.
                Text {
                    Layout.fillWidth: true
                    text: {
                        var s = root._sum
                        if (!s.hasLibrary)
                            return qsTr("No swing library is configured, so there is nothing to "
                                        + "draw from. Set one in Settings.")
                        if (s.scanning)
                            return qsTr("Reading the library…")
                        if (!s.everScanned)
                            return ""
                        if ((s.scanned || 0) === 0)
                            return qsTr("The library has no swings in it yet.")
                        if ((s.produced || 0) === 0)
                            return qsTr("Looked at %1 swings; none of them carries this measure. "
                                        + "Its producer either did not run or does not reach the "
                                        + "positions this measure reads.").arg(s.scanned)
                        return qsTr("%1 of %2 swings carry this measure.")
                                 .arg(s.produced).arg(s.scanned)
                    }
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible: root._sum.truncated === true
                    text: qsTr("Only the first %1 swings were read. The fit below is over those, "
                               + "not the whole library.").arg(root._sum.scanLimit || 0)
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorAttention
                    wrapMode:       Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(8)
                    visible: (root._sum.produced || 0) > 0

                    PpButton {
                        label: qsTr("Fit to marked swings")
                        primary: true
                        onClicked: {
                            var r = root.editor.seatFromSample()
                            seatNote.text = r.message || ""
                        }
                    }
                    PpButton { label: qsTr("Mark all");   onClicked: root.editor.setAllIncluded(true) }
                    PpButton { label: qsTr("Unmark all"); onClicked: root.editor.setAllIncluded(false) }
                    Item { Layout.fillWidth: true }
                    Text {
                        id: seatNote
                        text: ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }

                // The drawn swings themselves. Rows are here so a coach can throw out the swing
                // they mishit rather than accepting or rejecting the whole sample.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(1)

                    Repeater {
                        model: root.editor.samples
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: Theme.sp(28)
                            radius: Theme.radius
                            color:  sMa.containsMouse ? Theme.colorBg2 : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin:  Theme.sp(10)
                                anchors.rightMargin: Theme.sp(10)
                                spacing: Theme.sp(10)

                                Rectangle {
                                    implicitWidth:  Theme.sp(11)
                                    implicitHeight: Theme.sp(11)
                                    radius: Theme.sp(2)
                                    color:  modelData.included ? Theme.colorAccent : "transparent"
                                    border.width: 1
                                    border.color: modelData.included ? Theme.colorAccent
                                                                     : Theme.colorBorderStrong
                                }
                                Text {
                                    text:           modelData.label
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          modelData.included ? Theme.colorText
                                                                       : Theme.colorText3
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text:           modelData.session
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                    elide:          Text.ElideRight
                                }
                                Text {
                                    text:               root._num(modelData.value)
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingData
                                    color:              Theme.colorText2
                                }
                                Rectangle {
                                    implicitWidth:  Theme.sp(7)
                                    implicitHeight: Theme.sp(7)
                                    radius: width / 2
                                    color:   root._gradeColor(modelData.grade)
                                    opacity: root._gradeOpacity(modelData.grade)
                                }
                            }

                            MouseArea {
                                id: sMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                onClicked: root.editor.setSampleIncluded(modelData.swingDir,
                                                                         !modelData.included)
                            }
                        }
                    }
                }
            }

            // ── Import ───────────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                visible: root._d.route === "import"
                spacing: Theme.sp(6)

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Adopt a corridor already set for another context, then adjust it. "
                               + "Adopting copies the numbers; it does not link the two.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible: root.editor.importCandidates.length === 0
                    text: qsTr("No other context sets a corridor for this measure, so there is "
                               + "nothing to adopt.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                Repeater {
                    model: root.editor.importCandidates
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: Theme.sp(34)
                        radius: Theme.radius
                        color:  iMa.containsMouse ? Theme.colorBg2 : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin:  Theme.sp(10)
                            anchors.rightMargin: Theme.sp(10)
                            spacing: Theme.sp(10)

                            Text {
                                Layout.fillWidth: true
                                text:           modelData.contextLabel
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorText
                            }
                            Text {
                                // The row's own claim, phrased by the model — "at least 1.4" on a
                                // one-sided measure, where "1.4 to 1.5" would name a second bound
                                // the row does not have.
                                text: modelData.rangeText || ""
                                font.family:        Theme.fontData
                                font.pixelSize:     Theme.fontSzMicro
                                font.letterSpacing: Theme.trackingData
                                color:              Theme.colorText2
                            }
                            Text {
                                text:           modelData.sourceLabel
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }

                        MouseArea {
                            id: iMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape:  Qt.PointingHandCursor
                            onClicked:    root.editor.adoptFrom(modelData.contextId)
                        }
                    }
                }
            }

            PpDivider { Layout.fillWidth: true }

            // ── Provenance ───────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(3)

                Text {
                    text:                qsTr("WHERE THIS CORRIDOR COMES FROM")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Text {
                    Layout.fillWidth: true
                    text: {
                        var bits = [root._d.sourceLabel || ""]
                        if ((root._d.n || 0) > 0) bits.push(qsTr("%n swing(s)", "", root._d.n))
                        if (root._d.setOn)        bits.push(root._d.setOn)
                        if (root._d.author)       bits.push(root._d.author)
                        return bits.join(" · ")
                    }
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible:        (root._d.citation || "").length > 0
                    text:           root._d.citation || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible:        root._d.weak === true
                    text:           root._d.weakReason || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    font.italic:    true
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                // Not what PinPoint ships. Given the accent and the shipped figure alongside,
                // because "is this still the default?" is the question you cannot answer by
                // looking at a number, and the whole point of a reset is knowing what it goes
                // back TO.
                Text {
                    Layout.fillWidth: true
                    visible:        (root._d.editedNote || "").length > 0
                    text:           root._d.editedNote || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorAccent
                    wrapMode:       Text.WordWrap
                    topPadding:     Theme.sp(2)
                }

                // The inheritance line, with the delta. "You are 37 wider than the full swing" is
                // the sentence that tells an author whether the override is worth having.
                //
                // CLAIM against CLAIM on both sides. Comparing policy-scaled bands would multiply
                // both widths by the same factor and change the sentence without changing anything
                // the author could act on.
                Text {
                    Layout.fillWidth: true
                    visible: root._d.hasParent === true
                    text:    root._d.parentNote || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }
            }

            // ── Commit ───────────────────────────────────────────────────────
            //
            // The save warning is required by the brief and is not a nicety: seating from THIS
            // athlete's swings still writes a POPULATION norm that everyone on the norm set is
            // graded against. The draw-from selector picks the sample, not the scope.
            Text {
                Layout.fillWidth: true
                text: qsTr("Saving sets the population norm for everyone using this norm set — "
                           + "not just this athlete.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                topPadding:     Theme.sp(4)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                PpButton {
                    label:   qsTr("Save corridor")
                    primary: true
                    enabled: root._d.canSave === true
                    onClicked: {
                        var r = root.editor.save()
                        saveNote.text = r.message || ""
                    }
                }
                // Two DIFFERENT undos, and conflating them is how someone loses a corridor they
                // meant to keep. Discard throws away this session's dragging and writes nothing;
                // Reset drops the saved override and IS a write.
                PpButton {
                    label:   qsTr("Discard changes")
                    visible: root._d.canDiscard === true
                    onClicked: {
                        var r = root.editor.discardChanges()
                        saveNote.text = r.message || ""
                    }
                }
                PpButton {
                    // "Reset to shipped" only where something WAS shipped; otherwise "Remove your
                    // override", because there is no shipped corridor here to go back to. The
                    // label comes from C++ for exactly that reason.
                    label:   root._d.resetLabel || qsTr("Reset to shipped")
                    visible: root._d.canReset === true
                    onClicked: {
                        var r = root.editor.resetToDefault()
                        saveNote.text = r.message || ""
                    }
                }
                PpButton { label: qsTr("Close"); onClicked: root.back() }

                Item { Layout.fillWidth: true }

                Text {
                    id: saveNote
                    Layout.maximumWidth: Theme.sp(360)
                    text: ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }
}
