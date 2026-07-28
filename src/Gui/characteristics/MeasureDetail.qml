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

// One measure, and every norm that judges it. Read-only — the corridor editor is a separate pass.
//
// The page is built around two things nothing else in the app shows:
//
//   1. The direction sentence. `highMeans` says what a HIGH value means in the measure's own words
//      — "further back, toward the trail foot" — and it exists because three signals shipped
//      pointing the wrong way. An inverted signal fires happily on the wrong swings with
//      correct-sounding text attached. See docs/design/pinpoint_sign_conventions.md.
//   2. Norms by context, in tree order, each row saying whether it was authored here or inherited
//      and FROM WHERE. Inheritance is the mechanism that lets one measure hold a driver corridor
//      and a wedge corridor without duplicating itself, and a list that showed only authored rows
//      would leave most contexts looking ungraded when they are graded.
//
// Weak provenance is called out ON THE NORM ROW and nowhere else. A grade derived from a heuristic
// norm renders exactly like one derived from a norm seated on 500 swings, because colour encodes
// distance from the norm and nothing else — softening a finding because its corridor is young
// would hide a real deviation behind a caveat about the ruler.
Item {
    id: root

    property var detail: ({})     // NormModel.measureDetail()

    signal back()
    signal openCondition(string conditionId)
    // Open the corridor editor for one (measure, context). A row that already has its own norm
    // edits it; an inherited row overrides it — the same gesture, because "override for this
    // context" IS opening the editor seeded from what it inherits.
    // `cohort` is the row identity's third term, spelled as the JSON spells it ({} = unqualified).
    // It has to travel: two rows can now sit at one context, and an editor opened without it would
    // silently edit the universal corridor while the row that was clicked describes one segment.
    signal editCorridor(string measureId, string contextId, var cohort)

    // The cohort axes a corridor can be authored against, from NormModel.cohortVocabulary().
    // Passed in rather than looked up here for the same reason `detail` is: this page renders a map
    // and holds no model.
    property var cohortVocab: ({})

    readonly property var _cohortSexes: root.cohortVocab.sexes || []
    readonly property var _cohortAges:  root.cohortVocab.ages  || []

    function _labelsOf(list) {
        var out = []
        for (var i = 0; i < list.length; ++i) out.push(list[i].label)
        return out
    }
    function _valueForLabel(list, label) {
        for (var i = 0; i < list.length; ++i)
            if (list[i].label === label) return list[i].value
        return ""
    }
    function _labelForValue(list, value) {
        for (var i = 0; i < list.length; ++i)
            if (list[i].value === value) return list[i].label
        return list.length > 0 ? list[0].label : ""
    }
    // The picker's own preview of what it is about to author. Assembled from the two labels rather
    // than from C++'s cohortLabel() because there is no Cohort here yet — nothing has been saved —
    // and the labels are the same strings that function formats.
    function _cohortLabelOf(sexValue, ageValue) {
        var parts = []
        if (sexValue !== "") parts.push(root._labelForValue(root._cohortSexes, sexValue))
        if (ageValue !== "") parts.push(root._labelForValue(root._cohortAges, ageValue))
        return parts.join(" ")
    }

    readonly property var _norms:  detail.norms  || []
    readonly property var _usedBy: detail.usedBy || []
    readonly property string _unit: detail.unit || ""

    // Display formatting only — one decimal is the resolution every corridor in the pack is
    // authored at, and trailing noise reads as false precision.
    function _num(v) { return Number(v).toFixed(1) }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(18)

            // ── Back ─────────────────────────────────────────────────────────
            Text {
                text:           "← " + qsTr("All measures")
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
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                PpDisplayText { text: root.detail.label || "" }
                PpTypePill { label: qsTr("Measure") }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                spacing: Theme.sp(8)

                Repeater {
                    model: {
                        var tags = []
                        if (root.detail.group) tags.push(root.detail.group)
                        if (root.detail.statusLabel) tags.push(root.detail.statusLabel)
                        if (root.detail.reducerLabel) tags.push(root.detail.reducerLabel)
                        // The unit is NOT a tag: every corridor row below carries it, and a
                        // fifth chip repeating it just crowds the three that say something.
                        return tags
                    }
                    delegate: Rectangle {
                        required property var modelData
                        implicitWidth:  tagText.implicitWidth + Theme.sp(16)
                        implicitHeight: Theme.sp(22)
                        radius: height / 2
                        color:  Theme.colorBg2

                        Text {
                            id: tagText
                            anchors.centerIn: parent
                            text:           modelData
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                    }
                }
            }

            // ── What it is ───────────────────────────────────────────────────
            Text {
                Layout.fillWidth: true
                visible:        (root.detail.what || "").length > 0
                text:           root.detail.what || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody
                color:          Theme.colorText
                wrapMode:       Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                visible:        (root.detail.howToRead || "").length > 0
                text:           root.detail.howToRead || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            // ── The direction sentence ───────────────────────────────────────
            //
            // Given its own framed block, not a footnote: this is the statement an author reads
            // instead of guessing at High/Low, and its absence is what let inverted signals ship.
            Rectangle {
                Layout.fillWidth: true
                visible:        (root.detail.highMeans || "").length > 0
                implicitHeight: highCol.implicitHeight + Theme.sp(20)
                radius:         Theme.radius
                color:          Theme.colorBg2

                ColumnLayout {
                    id: highCol
                    x:       Theme.sp(12)
                    y:       Theme.sp(10)
                    width:   parent.width - Theme.sp(24)
                    spacing: Theme.sp(3)

                    Text {
                        text:                qsTr("A HIGHER VALUE MEANS")
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                    }
                    Text {
                        Layout.fillWidth: true
                        text:           root.detail.highMeans || ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                        wrapMode:       Text.WordWrap
                    }
                }
            }

            // ── Availability ─────────────────────────────────────────────────
            Text {
                Layout.fillWidth: true
                visible:        (root.detail.availability || "").length > 0
                text:           root.detail.availability || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            // ── Norms by context ─────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Text {
                    text:                qsTr("NORMS BY CONTEXT")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Text {
                    Layout.fillWidth: true
                    visible:        root._norms.length === 0
                    text: qsTr("No norm judges this measure, in any context. A corridor signal on "
                               + "it reports unavailable rather than firing — which looks exactly "
                               + "like a golfer who does not have the fault.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible: (root.detail.editedNormCount || 0) > 0
                    // Phrased to avoid a count/verb disagreement at n = 1 — Qt's "(s)" fallback
                    // handles the noun, but "1 … are yours" would still read wrong.
                    text: qsTr("%n corridor(s) changed from shipped. Open one to see what it was, "
                               + "or to reset it.", "", root.detail.editedNormCount || 0)
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorAccent
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible: root._norms.length > 0
                    text: qsTr("Resolution walks UP the tree, so a context with no row of its own "
                               + "inherits its parent's. Only the rows marked as its own were "
                               + "deliberately distinguished.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                    bottomPadding:  Theme.sp(4)
                }

                Repeater {
                    model: root._norms
                    delegate: Item {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: normCol.implicitHeight + Theme.sp(14)

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radius
                            // An inherited row is the same information seen from further away, so
                            // it recedes; an own row is a decision somebody made. Hover lifts
                            // either, because either can be opened.
                            color:  normMa.containsMouse ? Theme.colorBg3
                                  : modelData.own        ? Theme.colorBg2
                                  :                        "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        // Tapping a row opens the corridor editor at THAT context. Placed under
                        // the content so the row's own text stays selectable-looking rather than
                        // swallowed by a full-bleed hit target on top of it.
                        MouseArea {
                            id: normMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape:  Qt.PointingHandCursor
                            onClicked: root.editCorridor(root.detail.id || "",
                                                         modelData.askedContextId,
                                                         modelData.askedCohort || ({}))
                        }

                        ColumnLayout {
                            id: normCol
                            // Indentation IS the tree. Depth comes from C++ so the hierarchy
                            // rendered is the hierarchy resolution actually walks.
                            x:       Theme.sp(10) + modelData.depth * Theme.sp(16)
                            y:       Theme.sp(7)
                            width:   parent.width - x - Theme.sp(12)
                            spacing: Theme.sp(2)

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.sp(8)

                                Text {
                                    text:           modelData.askedContextLabel
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          modelData.own ? Theme.colorText : Theme.colorText2
                                }

                                // Which population this row grades. Absent — not blank — on a
                                // universal corridor, so the common row reads exactly as it always
                                // did rather than gaining an empty column. The words come from
                                // C++ (normAt.cohortLabel): "men 55–64" is one segment's name and
                                // assembling it from two tokens in a binding is how two surfaces
                                // end up spelling it differently.
                                Text {
                                    visible:        (modelData.cohortLabel || "").length > 0
                                    text:           modelData.cohortLabel || ""
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText2
                                }

                                Item { Layout.fillWidth: true }

                                // The corridor, in the measure's own units. The Ideal band, because
                                // it is what the two editor handles will bind to. The unit trails
                                // quieter so the figures stay scannable down the column — with a
                                // unit like "% stance width" it would otherwise dominate the row.
                                // Composed in C++ (normAt.bandPhrase): a one-sided corridor reads
                                // "at least 1.48", never "1.48 to 1.53" — the second number there
                                // is mu plus a tolerance nothing grades, so naming it states a
                                // bound on the side the norm explicitly refuses to grade.
                                Text {
                                    text:               modelData.bandPhrase || ""
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzDataSm
                                    font.weight:        Font.Light
                                    font.letterSpacing: Theme.trackingData
                                    color:              Theme.colorText
                                }
                                Text {
                                    visible:        root._unit.length > 0
                                    text:           root._unit
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.sp(6)

                                Text {
                                    text: modelData.own
                                            ? qsTr("its own")
                                            : qsTr("inherited from %1").arg(modelData.contextLabel)
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }

                                // Not what PinPoint ships. An inherited row reads "edited" too,
                                // because it IS being graded by the user's corridor — saying
                                // otherwise would be false about the number beside it.
                                Text {
                                    visible: modelData.overridden === true
                                    text: modelData.hasShipped
                                            ? qsTr("· edited, ships %1")
                                                .arg(modelData.shippedPhrase || "")
                                            : qsTr("· added by you")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorAccent
                                }

                                // The affordance appears on hover rather than sitting on every
                                // row: thirteen contexts each carrying a permanent "Override"
                                // button would read as thirteen things to do.
                                Text {
                                    visible: normMa.containsMouse
                                    text:    modelData.own ? qsTr("· edit")
                                                           : qsTr("· override for this context")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorAccent
                                }

                                // Authoring a SEGMENTED corridor. Its own affordance and not a mode
                                // on the one above, because it answers a different question — that
                                // one is "which shot", this is "which golfer" — and because the
                                // universal corridor must stay the thing a single click reaches.
                                //
                                // Offered only on the universal row of each context: a segmented row
                                // is already a cohort, and hanging "for a cohort…" off it would
                                // invite authoring a cohort of a cohort, which the key cannot hold.
                                Text {
                                    visible: normMa.containsMouse
                                             && (modelData.cohortLabel || "").length === 0
                                    text:    qsTr("· for a cohort…")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorAccent

                                    // On TOP of the row's own full-bleed handler, which sits under
                                    // the content, so this wins the click.
                                    //
                                    // hoverEnabled MUST stay false. This Text is `visible` only
                                    // while `normMa` reports hover, and a hoverEnabled child on top
                                    // of `normMa` STEALS that hover — which hides this Text, which
                                    // kills the hover, which shows it again. The row strobes every
                                    // frame and the click can never land. A MouseArea that does not
                                    // accept hover is skipped for hover delivery, so `normMa` keeps
                                    // it, and clicks still hit the topmost item. `cursorShape` does
                                    // not need hover either.
                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.margins: -Theme.sp(3)
                                        hoverEnabled: false
                                        cursorShape:  Qt.PointingHandCursor
                                        onClicked: {
                                            cohortPicker.contextId    = modelData.askedContextId
                                            cohortPicker.contextLabel = modelData.askedContextLabel
                                            cohortPicker.sexValue     = ""
                                            cohortPicker.ageValue     = ""
                                            cohortPicker.open()
                                        }
                                    }
                                }

                                // "action below 1.33" on a floor, never "beyond 1.33 to 1.48":
                                // the open tail has no fault edge to be beyond. The parenthetical
                                // stays here because it is about WHERE the edge came from, which
                                // is a fact about this row and not about the measure.
                                Text {
                                    text: modelData.explicitMonitor
                                            ? qsTr("· %1").arg(modelData.actionPhrase || "")
                                            : qsTr("· %1 (from the grade policy)")
                                                .arg(modelData.actionPhrase || "")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }

                                Item { Layout.fillWidth: true }

                                Text {
                                    text: {
                                        var bits = [modelData.sourceLabel]
                                        if (modelData.n > 0) bits.push(qsTr("n=%1").arg(modelData.n))
                                        if (modelData.setOn) bits.push(modelData.setOn)
                                        return bits.join(" · ")
                                    }
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }
                            }

                            // Why this number is what it is: weak provenance and the citation are
                            // the same question answered from two sides, so they share one line
                            // rather than stacking. Every shipped norm is heuristic today, and two
                            // stacked lines per row turned the list into a wall of caveats.
                            //
                            // Weak provenance is called out HERE and nowhere else — never on a
                            // finding, a chip or a chart band. Colour on a finding encodes distance
                            // from the norm and nothing else; softening one because its corridor is
                            // young would hide a real deviation behind a caveat about the ruler.
                            Text {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: {
                                    var bits = []
                                    if (modelData.weak && (modelData.weakReason || "").length > 0)
                                        bits.push(modelData.weakReason)
                                    if ((modelData.citation || "").length > 0)
                                        bits.push(modelData.citation)
                                    return bits.join("  ")
                                }
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                font.italic:    modelData.weak
                                color:          Theme.colorText3
                                wrapMode:       Text.WordWrap
                                topPadding:     Theme.sp(2)
                            }
                        }
                    }
                }
            }

            // ── Used by ──────────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(4)

                Text {
                    text:                qsTr("USED BY")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                    topPadding:          Theme.sp(6)
                }

                Text {
                    Layout.fillWidth: true
                    visible:        root._usedBy.length === 0
                    text:           qsTr("No characteristic reads this measure.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText3
                }

                Repeater {
                    model: root._usedBy
                    delegate: Item {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: Theme.sp(32)

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radius
                            color:  useMa.containsMouse ? Theme.colorBg2 : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin:  Theme.sp(10)
                            anchors.rightMargin: Theme.sp(10)
                            spacing: Theme.sp(8)

                            Text {
                                Layout.fillWidth: true
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorText
                                elide:          Text.ElideRight
                            }
                            Text {
                                text:           modelData.groupLabel
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }

                        MouseArea {
                            id: useMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape:  Qt.PointingHandCursor
                            onClicked:    root.openCondition(modelData.id)
                        }
                    }
                }
            }

            // ── Identity ─────────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(2)

                Text {
                    text:                qsTr("IDENTITY")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                    topPadding:          Theme.sp(6)
                }

                Text {
                    Layout.fillWidth: true
                    text: {
                        var bits = [root.detail.id || ""]
                        if (root.detail.metricKey) bits.push(root.detail.metricKey)
                        if (root.detail.kind) bits.push(root.detail.kind)
                        return bits.join(" · ")
                    }
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible:        (root.detail.gapReason || "").length > 0
                    text:           root.detail.gapReason || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }

    // ── Author a corridor for one population ────────────────────────────────
    //
    // At FILE SCOPE and not inside the Repeater delegate, deliberately. A popup declared in a
    // delegate is destroyed with its row and its handlers resolve against delegate scope, which is
    // the trap that throws only on click — nothing in a binding, a test or a screenshot would show
    // it. The row sets three properties here and opens it.
    Popup {
        id: cohortPicker
        objectName: "cohortPicker"

        property string contextId:    ""
        property string contextLabel: ""
        property string sexValue:     ""   // "" = any, which is how an unset axis is spelled
        property string ageValue:     ""

        // Both axes "any" IS the universal corridor, which the row itself already opens. Refusing
        // it here rather than letting it through keeps one corridor reachable by one gesture.
        readonly property bool _qualified: sexValue !== "" || ageValue !== ""

        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        dim: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: Theme.sp(20)
        width: Math.min(Theme.sp(420), (parent ? parent.width : Theme.sp(420)) - Theme.sp(48))

        // The same surface + border pair PpAboutDialog uses, which is the app's one modal
        // precedent. `colorBg1` is not a token — the assignment silently failed and the Rectangle
        // kept its own default, which is white, so the panel ignored the dark theme entirely.
        background: Rectangle {
            radius:       Theme.radiusLg
            color:        Theme.colorSurface
            border.width: 1
            border.color: Theme.colorBorderStrong
        }

        ColumnLayout {
            width:   parent.width
            spacing: Theme.sp(14)

            Text {
                Layout.fillWidth: true
                text:           qsTr("A corridor for one population")
                font.family:    Theme.fontDisplay
                font.pixelSize: Theme.fontSzBody
                color:          Theme.colorText
                wrapMode:       Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("In %1. Everyone outside the group you pick keeps being graded by the "
                           + "corridor that is there now — this adds one beside it, it does not "
                           + "replace it.").arg(cohortPicker.contextLabel)
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            Column {
                Layout.fillWidth: true
                spacing: Theme.sp(4)
                Text {
                    text:               qsTr("SEX")
                    font.family:        Theme.fontData
                    font.pixelSize:     Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingLabel
                    color:              Theme.colorText3
                }
                PpChipGroup {
                    options:  root._labelsOf(root._cohortSexes)
                    selected: root._labelForValue(root._cohortSexes, cohortPicker.sexValue)
                    onSelectionChanged: function(v) {
                        cohortPicker.sexValue = root._valueForLabel(root._cohortSexes, v)
                    }
                }
            }

            Column {
                Layout.fillWidth: true
                spacing: Theme.sp(4)
                Text {
                    text:               qsTr("AGE")
                    font.family:        Theme.fontData
                    font.pixelSize:     Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingLabel
                    color:              Theme.colorText3
                }
                PpChipGroup {
                    options:  root._labelsOf(root._cohortAges)
                    selected: root._labelForValue(root._cohortAges, cohortPicker.ageValue)
                    onSelectionChanged: function(v) {
                        cohortPicker.ageValue = root._valueForLabel(root._cohortAges, v)
                    }
                }
                // "18+" is authorable in its own right and is not merely the parent of the three
                // bands under it — most published provenance is no better than "adult male", and
                // without it that common case would need three duplicate rows that then drift.
                Text {
                    width:          parent.width
                    text:           qsTr("Pick 18+ when the source says only \"adults\". An exact band is always tried first, so a row for it still resolves for everyone the exact bands do not cover.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                Text {
                    Layout.fillWidth: true
                    text: cohortPicker._qualified
                            ? qsTr("Grades %1 only.")
                                .arg(root._cohortLabelOf(cohortPicker.sexValue, cohortPicker.ageValue))
                            : qsTr("Pick at least one — with neither, this is the corridor you already have.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          cohortPicker._qualified ? Theme.colorAccent : Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                PpButton {
                    label: qsTr("Cancel")
                    onClicked: cohortPicker.close()
                }
                PpButton {
                    label:   qsTr("Open editor")
                    primary: true
                    enabled: cohortPicker._qualified
                    onClicked: {
                        var c = ({})
                        if (cohortPicker.sexValue !== "") c.sex = cohortPicker.sexValue
                        if (cohortPicker.ageValue !== "") c.age = cohortPicker.ageValue
                        cohortPicker.close()
                        root.editCorridor(root.detail.id || "", cohortPicker.contextId, c)
                    }
                }
            }
        }
    }
}
