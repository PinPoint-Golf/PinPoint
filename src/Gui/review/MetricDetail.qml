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

// Detail view for a single metric. Reads the full descriptor from MetricCatalog and
// renders it in plain language: what it means, how to read it, its normative corridors
// (one NormativeBar per phase), what it needs to compute, and where it is used. Purely
// read-only; it emits back() for the parent to return to the directory.

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

Item {
    id: root

    // The MetricCatalog façade and TimelineLabels solver, injected by the host.
    property var    mc
    property var    labels
    property string metricKey: ""

    signal back()
    // Through to where this corridor is DEFINED — the measure's norm rows in Diagnostics, from which
    // the corridor editor opens. A metric page states what good looks like; this is the way to the
    // place that decides it, so a reader who disagrees with a band can go and change it rather than
    // hunting for it in another panel.
    signal openNorm(string measureId)

    // Full descriptor map for the current key (empty when unset / unknown).
    readonly property var d: (mc && metricKey && metricKey.length > 0) ? mc.descriptor(metricKey) : ({})

    readonly property var  _norm:      (d && d.normative) ? d.normative : ({})
    readonly property var  _corridors: (_norm && _norm.corridors) ? _norm.corridors : []
    readonly property var  _usedBy:    (d && d.usedBy) ? d.usedBy : []
    readonly property bool _planned:   (d && d.planned === true)   // roadmap placeholder

    // ── plain-language helpers ────────────────────────────────────────────────
    function _typeName(t) {
        switch (t) {
        case "summary":     return qsTr("Summary")
        case "pointInTime": return qsTr("Point in time")
        case "timeSeries":  return qsTr("Time series")
        case "sequence":    return qsTr("Sequence")
        }
        return t || ""
    }

    // One line naming the norm behind this metric's corridors: which context it resolved in,
    // whether that context is its own or an ancestor's, how well founded it is, and whether it is
    // still the shipped corridor. Every part comes marshalled from C++ — the words for a norm's
    // provenance live with the enum, not here.
    function _normProvenance() {
        var n = root._norm
        if (!n) return ""
        var parts = []
        if (n.contextLabel)
            parts.push(n.inherited === true ? qsTr("Inherited from %1").arg(n.contextLabel)
                                            : n.contextLabel)
        // Which population the corridor describes, second — right after where it came from, because
        // both answer "whose corridor is this". Absent on a universal corridor, which is every
        // shipped row: cohortLabel is empty there on purpose, so this term simply does not appear.
        if (n.cohortLabel)
            parts.push(qsTr("graded against %1").arg(n.cohortLabel))
        if (n.sourceLabel)
            parts.push(n.n > 0 ? qsTr("%1, n = %2").arg(n.sourceLabel).arg(n.n) : n.sourceLabel)
        if (n.overridden === true)
            parts.push(qsTr("edited by you"))
        return parts.join(" · ")
    }

    // "LeadForearm" → "Lead forearm"
    function _humanRole(r) {
        var s = String(r).replace(/([A-Z])/g, " $1").trim().toLowerCase()
        return s.charAt(0).toUpperCase() + s.slice(1)
    }

    // "chart:review" → "Review chart"
    function _humanUsage(u) {
        var parts = String(u).split(":")
        if (parts.length === 2 && parts[1].length > 0)
            return parts[1].charAt(0).toUpperCase() + parts[1].slice(1) + " " + parts[0]
        return u
    }

    // The requirement, rendered as plain-language capability lines.
    function _measures() {
        var out = []
        var r = (d && d.requires) ? d.requires : ({})
        var roles = r.imuRoles || []
        if (roles.length > 0) {
            var names = roles.map(_humanRole)
            out.push(names.join(" + ") + (roles.length > 1 ? qsTr(" IMUs") : qsTr(" IMU")))
        }
        if (r.faceOnCamera) out.push(qsTr("Face-on camera"))
        if (r.clubTrack)    out.push(qsTr("Club tracking"))
        if (r.ballTrack)    out.push(qsTr("Ball tracking"))
        if (r.minTier && r.minTier !== "angles2D")
            out.push(qsTr("Requires %1 reconstruction").arg(r.minTier))
        return out
    }

    // ── reusable section eyebrow ──────────────────────────────────────────────
    component Eyebrow: Text {
        font.family:         Theme.fontBody
        font.pixelSize:      Theme.fontSzMicro
        font.letterSpacing:  Theme.trackingMicro
        font.capitalization: Font.AllUppercase
        color:               Theme.colorText3
    }

    // ── reusable body paragraph ───────────────────────────────────────────────
    component Body: Text {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.sp(26)
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzBody2
        font.weight:    Theme.fontBodyWeight
        color:          Theme.colorText2
        wrapMode:       Text.WordWrap
        lineHeight:     1.35
    }

    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            id: contentCol
            x:       Theme.sp(32)
            y:       Theme.sp(24)
            width:   scrollView.availableWidth - Theme.sp(64)
            spacing: Theme.sp(20)

            // ── Back affordance ───────────────────────────────────────────────
            Item {
                Layout.fillWidth: true
                implicitHeight: backRow.implicitHeight + Theme.sp(4)

                Row {
                    id: backRow
                    spacing: Theme.sp(6)
                    Text {
                        text: "‹"
                        anchors.verticalCenter: parent.verticalCenter
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzHeading
                        color: backMa.containsMouse ? Theme.colorText : Theme.colorText3
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }
                    Text {
                        text: qsTr("Metric catalogue")
                        anchors.verticalCenter: parent.verticalCenter
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color: backMa.containsMouse ? Theme.colorText2 : Theme.colorText3
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }
                }
                PpPressable { id: backMa; hoverScale: 1.0; onClicked: root.back() }
            }

            // ── Header ─────────────────────────────────────────────────────────
            Eyebrow { text: (d && d.group) ? d.group : "" }

            RowLayout {
                Layout.fillWidth: true
                spacing:          Theme.sp(12)

                // Capped rather than filling. A filling title eats the whole row and strands the
                // pill against the right margin, where it reads as a page-level badge instead of
                // a label on this title. The cap still lets a long metric name wrap.
                //
                // Measured off contentCol, NOT off titleRow: the row's width is derived from its
                // children, so capping a child against it is a layout cycle — Qt detects it,
                // gives up after two passes, and leaves the header mis-sized.
                PpDisplayText {
                    Layout.maximumWidth: Math.max(Theme.sp(120),
                                                  contentCol.width - typePill.implicitWidth
                                                  - Theme.sp(24))
                    text: (d && d.label) ? d.label : ""
                    wrapMode: Text.WordWrap
                }
                // Top-aligned rather than centred, because this title is the one that wraps: beside
                // a three-line heading a vertically-centred pill floats free of the words it names.
                PpTypePill {
                    id: typePill
                    Layout.alignment: Qt.AlignTop
                    label: qsTr("Metric")
                }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                // Type pill
                Rectangle {
                    implicitWidth:  hdrType.implicitWidth + Theme.sp(12)
                    implicitHeight: Theme.sp(18)
                    radius: Theme.radius
                    color: Theme.colorBg3
                    Text {
                        id: hdrType
                        anchors.centerIn: parent
                        text: root._typeName(d ? d.type : "")
                        font.family:        Theme.fontData
                        font.pixelSize:     Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText2
                    }
                }

                // Unit
                Text {
                    visible: (d.unit || "").length > 0
                    text: qsTr("Unit: %1").arg(d.unit || "")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: Theme.colorText3
                }

                Item { Layout.fillWidth: true }

                // Scored badge
                Rectangle {
                    visible: d && d.scored === true
                    implicitWidth:  hdrScored.implicitWidth + Theme.sp(12)
                    implicitHeight: Theme.sp(18)
                    radius: Theme.radius
                    color: Theme.colorAccentLight
                    border.width: 1
                    border.color: Theme.colorAccentMid
                    Text {
                        id: hdrScored
                        anchors.centerIn: parent
                        text: qsTr("SCORED")
                        font.family:        Theme.fontData
                        font.pixelSize:     Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorAccent
                    }
                }
            }

            // ── Planned banner — this metric is a roadmap placeholder ──────────
            Rectangle {
                visible: root._planned
                Layout.fillWidth: true
                implicitHeight: plannedRow.implicitHeight + Theme.sp(20)
                radius: Theme.radius
                color: Theme.colorBg2
                border.width: 1
                border.color: Theme.colorBorderMid

                RowLayout {
                    id: plannedRow
                    anchors.fill: parent
                    anchors.margins: Theme.sp(10)
                    spacing: Theme.sp(10)

                    Text {
                        text: "◷"
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzHeading
                        color: Theme.colorText3
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Planned — not yet produced in this build. The requirements below describe what it will need.")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color: Theme.colorText2
                        wrapMode: Text.WordWrap
                    }
                }
            }

            PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

            // ── What it means ──────────────────────────────────────────────────
            Eyebrow { text: qsTr("What it means") }
            Body { text: (d && d.description) ? d.description : "" }

            // ── How to read ────────────────────────────────────────────────────
            Eyebrow { text: qsTr("How to read") }
            Body { text: (d && d.howToRead) ? d.howToRead : "" }

            // ── Normative ──────────────────────────────────────────────────────
            Eyebrow { text: qsTr("Normative") }

            ColumnLayout {
                Layout.fillWidth:  true
                Layout.leftMargin: Theme.sp(26)
                spacing: Theme.sp(14)

                // Where the corridor came from: the norm set, resolved in this shot's context. The
                // inheritance is STATED rather than implied — a driver graded by the full-swing
                // corridor is the whole point of the context tree, and "mid-iron" printed as a note
                // used to be the only hint of it.
                Text {
                    visible: root._corridors.length > 0 && root._normProvenance().length > 0
                    Layout.fillWidth: true
                    text: root._normProvenance()
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: Theme.colorText3
                    wrapMode: Text.WordWrap
                }

                // One corridor per phase.
                Repeater {
                    model: root._corridors
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.sp(6)

                        // The phase, and — where the corridor grades a CHANGE from address rather
                        // than an absolute reading — which of the two it is. Two rows on one metric
                        // can now differ, so neither can be left to be assumed.
                        Text {
                            text: {
                                if (!root.labels || modelData.phase === undefined) return ""
                                var n = root.labels.phaseFullName(modelData.phase)
                                return modelData.deltaFromAddress === true
                                       ? qsTr("%1 · change from address").arg(n) : n
                            }
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText
                        }

                        NormativeBar {
                            Layout.fillWidth: true
                            corridor: modelData
                        }

                        // Through to where this corridor is DEFINED, so it can be edited. Named
                        // rather than labelled "edit": the measure and the context ARE the identity
                        // of a norm, and a metric key is not — two phases of one metric are two
                        // measures with two norms, which is why this sits per corridor.
                        //
                        // Styled as the app's text link (ScreenHome's "Switch →"): accent colour at
                        // REST, body font, trailing arrow, cursor change and nothing else. The
                        // muted-until-hover treatment is for secondary chrome; a way out of the page
                        // has to look like one before the pointer arrives.
                        Text {
                            Layout.fillWidth: true
                            text: {
                                var mlabel = modelData.measureLabel || modelData.measureId || ""
                                var clabel = modelData.contextLabel || ""
                                if (mlabel.length === 0) return ""
                                return (clabel.length > 0)
                                       ? qsTr("%1 · %2 →").arg(mlabel).arg(clabel)
                                       : qsTr("%1 →").arg(mlabel)
                            }
                            visible: text.length > 0
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color:          Theme.colorAccent
                            elide:          Text.ElideRight

                            // Plain MouseArea, and the handler calls through `root`: inside a
                            // Repeater delegate the only file-level id that resolves is the
                            // component root, and a handler on a composite type could not see even
                            // that. It throws only when clicked.
                            MouseArea {
                                anchors.fill: parent
                                cursorShape:  Qt.PointingHandCursor
                                onClicked: root.openNorm(modelData.measureId || "")
                            }
                        }
                    }
                }

                // Why this corridor should be read as a starting point. The norm's own words —
                // never derived here, because how well founded a corridor is is a property of the
                // norm and not of the page showing it.
                Text {
                    visible: root._corridors.length > 0 && (root._norm.weakReason || "").length > 0
                    Layout.fillWidth: true
                    text: (root._norm && root._norm.weakReason) ? root._norm.weakReason : ""
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: Theme.colorText3
                    wrapMode: Text.WordWrap
                }

                // The citation, which is where a provisional figure explains itself.
                Text {
                    visible: root._corridors.length > 0 && (root._norm.citation || "").length > 0
                    Layout.fillWidth: true
                    text: (root._norm && root._norm.citation) ? root._norm.citation : ""
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: Theme.colorText3
                    wrapMode: Text.WordWrap
                }

                // Emit-nothing-never-garbage: an honest note instead of an empty box.
                Text {
                    visible: root._corridors.length === 0
                    Layout.fillWidth: true
                    text: qsTr("No norm for this metric yet.")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: Theme.colorText3
                }
            }

            // ── How it's measured ──────────────────────────────────────────────
            Eyebrow { text: qsTr("How it's measured") }

            ColumnLayout {
                Layout.fillWidth:  true
                Layout.leftMargin: Theme.sp(26)
                spacing: Theme.sp(6)

                Repeater {
                    model: root._measures()
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.sp(8)
                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth:  Theme.sp(5)
                            implicitHeight: Theme.sp(5)
                            radius: width / 2
                            color: Theme.colorText3
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText2
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // ── Where it's used ────────────────────────────────────────────────
            Eyebrow {
                text: qsTr("Where it's used")
                visible: root._usedBy.length > 0
            }

            Flow {
                Layout.fillWidth:  true
                Layout.leftMargin: Theme.sp(26)
                visible: root._usedBy.length > 0
                spacing: Theme.sp(7)

                Repeater {
                    model: root._usedBy
                    delegate: Rectangle {
                        required property var modelData
                        width:  useLbl.implicitWidth + Theme.sp(16)
                        height: Theme.sp(22)
                        radius: Theme.radius
                        color: Theme.colorBg2
                        border.width: 1
                        border.color: Theme.colorBorderMid
                        Text {
                            id: useLbl
                            anchors.centerIn: parent
                            text: root._humanUsage(modelData)
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText2
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(8) }
        }
    }
}
