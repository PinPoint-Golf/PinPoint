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

// PpChartSummary — the per-window summary cards row: one card per series, each fed by
// ChartMetrics.summary(series.t_us, series.value, startUs, endUs). A card shows @impact /
// peak / Δ-segment / peak-rate over the active window, with the @impact value tinted by the
// band of the swing state at impact. @impact is the value at the impact landmark (read from
// the whole series, a fixed reference more useful to compare against PEAK than the window
// edge); peak/Δ/rate stay window-scoped, so selecting TOP→IMP shows the downswing's numbers.
// Recomputes live as the segment chips / brush move the window. Pure binding; all stats come
// from ChartMetrics (value-at-impact via TimelineLabels.valueAtNearest).

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic   // ToolTip on the σ chip
import QtQuick.Layouts
import PinPointStudio

ColumnLayout {
    id: root

    // series: [{ key, label, unit, t_us, value, phaseSamples, color, sigma? }]
    // `sigma` is OPTIONAL and its absence is meaningful — see the chip below.
    property var    series:      []
    property real   startUs:     0
    property real   endUs:       0
    property real   impactUs:    -1       // impact instant; the @impact card reads the series here
    property string segmentName: ""
    property bool   showHeader:  true     // false when a host SectionHeader labels this

    spacing: Theme.sp(9)

    ChartMetrics   { id: cm }
    TimelineLabels { id: labels }         // value-at-time lookup (impact landmark)

    // Format a value in its series' own unit (see PpMetricChart._fmt): degrees keep the
    // signed "+12°" convention, word units read as "75 mph"; omitted ⇒ degrees.
    function _fmt(v, unit) {
        var u = (unit === undefined || unit === null || unit === "") ? "°" : unit
        var r = Math.round(v)
        var sign = (r > 0 && u === "°") ? "+" : ""
        var sep  = (u === "°") ? "" : " "
        return sign + r + sep + u
    }
    function _bandColor(b) {
        return b === "warn"      ? Theme.colorWarn
             : b === "attention" ? Theme.colorAttention
             :                     Theme.colorGood
    }

    // Header — "SUMMARY · <segment>" + a hairline rule.
    RowLayout {
        visible: root.showHeader
        Layout.fillWidth: true
        spacing: Theme.sp(9)
        Text {
            text: qsTr("SUMMARY") + (root.segmentName ? " · " + root.segmentName : "")
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingLabel
            color: Theme.colorText3
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.colorBorder }
    }

    // Cards — equal-width columns, wrapping to the available width.
    GridLayout {
        id: grid
        Layout.fillWidth: true
        columnSpacing: Theme.sp(10); rowSpacing: Theme.sp(10)
        columns: Math.max(1, Math.min(root.series.length,
                                      Math.floor(grid.width / Theme.sp(150))))

        Repeater {
            model: root.series
            delegate: Rectangle {
                id: card
                required property var modelData
                readonly property var    st:  cm.summary(card.modelData.t_us, card.modelData.value,
                                                         root.startUs, root.endUs)
                // Value at the impact landmark — a fixed anatomical reference, so it reads the
                // whole series (not the view window); the more useful thing to compare against
                // PEAK. Falls back to the window @end when no impact is known.
                readonly property real   impVal: root.impactUs > 0
                                                 ? labels.valueAtNearest(card.modelData.t_us,
                                                       card.modelData.value, root.impactUs)
                                                 : card.st.end
                readonly property string bnd: cm.bandAtNearest(card.modelData.phaseSamples,
                                                  root.impactUs > 0 ? root.impactUs : root.endUs)
                readonly property string nm:  cm.shortLabel(card.modelData.key)
                                              || card.modelData.label || card.modelData.key

                Layout.fillWidth: true
                Layout.preferredWidth: 1            // equal columns
                implicitHeight: cardCol.implicitHeight + Theme.sp(22)
                radius: Theme.sp(10)
                color: Theme.colorBg
                border.width: 1; border.color: Theme.colorBorder
                clip: true

                Rectangle {                          // series colour edge
                    width: Theme.sp(3); height: parent.height
                    color: card.modelData.color
                }

                ColumnLayout {
                    id: cardCol
                    anchors { left: parent.left; right: parent.right; top: parent.top
                              leftMargin: Theme.sp(13); rightMargin: Theme.sp(11)
                              topMargin: Theme.sp(11) }
                    spacing: Theme.sp(10)

                    RowLayout {                       // name + unit
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: card.nm; elide: Text.ElideRight
                            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
                            color: Theme.colorText
                        }
                        Text {
                            text: (card.modelData.unit || qsTr("deg"))
                            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                            font.letterSpacing: Theme.trackingData
                            color: Theme.colorText3
                        }
                        // Measurement NOISE on this series, when its producer characterised one.
                        // Sits by the unit rather than on each of the four values because it is
                        // one number for the whole curve — repeating it four times would read as
                        // four separate error bars. Absent ⇒ nothing is drawn: a series with no
                        // σ has not been characterised, which is not the same as being exact.
                        Text {
                            visible: card.modelData.sigma !== undefined
                                     && card.modelData.sigma > 0
                            text: "± " + card.modelData.sigma.toFixed(1)
                                       + (card.modelData.unit || "°")
                            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                            font.letterSpacing: Theme.trackingData
                            color: Theme.colorText3
                            HoverHandler { id: sigmaHover }
                            ToolTip.visible: sigmaHover.hovered
                            ToolTip.delay: 400
                            // Deliberately says what it is NOT. This is frame-to-frame noise in
                            // the detector, not the accuracy of the reading — the projection
                            // error on a camera-derived angle is larger and uncharacterised, and
                            // a reader who took ±σ for total accuracy would trust it too far.
                            ToolTip.text: qsTr("Frame-to-frame measurement noise on this curve. "
                                               + "Not the overall accuracy of the reading.")
                        }
                    }

                    GridLayout {                      // 2×2 metric grid
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Theme.sp(12); rowSpacing: Theme.sp(9)

                        // @ impact — the landmark value, tinted by band at impact.
                        Column {
                            Layout.fillWidth: true
                            Text { text: qsTr("@ IMPACT"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            Text { text: root._fmt(card.impVal, card.modelData.unit); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzData; color: root._bandColor(card.bnd) }
                        }
                        Column {
                            Layout.fillWidth: true
                            Text { text: qsTr("PEAK"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            Text { text: root._fmt(card.st.peak, card.modelData.unit); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzData; color: Theme.colorText }
                        }
                        Column {
                            Layout.fillWidth: true
                            Text { text: qsTr("Δ SEGMENT"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            Text { text: root._fmt(card.st.delta, card.modelData.unit); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzData; color: Theme.colorText }
                        }
                        Column {
                            Layout.fillWidth: true
                            Text { text: qsTr("PK RATE"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            Row {
                                spacing: Theme.sp(3)
                                Text { id: rateVal
                                       text: Math.round(card.st.rate); font.family: Theme.fontData
                                       font.pixelSize: Theme.fontSzData; color: Theme.colorText }
                                Text { anchors.baseline: rateVal.baseline
                                       text: (card.modelData.unit || "°") + qsTr("/100ms")
                                       font.family: Theme.fontData
                                       font.pixelSize: Theme.fontSzMicro; color: Theme.colorText3 }
                            }
                        }
                    }
                }
            }
        }
    }
}
