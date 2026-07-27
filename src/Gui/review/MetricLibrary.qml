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

// The Metric Catalogue directory, shaped as a Settings panel. A filter chip row over
// the metric types, then the metrics grouped in manifest order, each row a MetricRow.
// Tapping a row swaps in MetricDetail (internal master→detail). Read-only: all data
// comes from the MetricCatalog façade; there is no shot loaded here, so availability is
// never computed (empty shotCtx everywhere) — a metric's needs are shown via its source
// glyphs and its detail "How it's measured" section, not a live verdict.

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

Item {
    id: root

    // Through to where a corridor is DEFINED, so a reader who disagrees with a band can edit it.
    // The destination is the Diagnostics panel, which this one cannot address — the host does.
    signal openNorm(string measureId)

    // ── façade + labels (declared declaratively; assembled once, read-only) ────
    // NB: ids must NOT collide with MetricDetail's `mc`/`labels` properties — inside the Loader's
    // Component, `mc: mc` would self-reference the (undefined) local property, blanking the detail.
    // The corridors it resolves are graded under the pack-wide policy the user chose in Diagnostics
    // settings, so it has to be told which one — a norm with no explicit monitor band takes its
    // Watch edge from the policy.
    MetricCatalog  { id: catalog; gradePolicy: appSettings.diagnosticsGradePolicy }
    TimelineLabels { id: metricLabels }

    // ── view state ────────────────────────────────────────────────────────────
    property string _typeFilter:  ""    // "" = all types
    // Hide roadmap-placeholder (PLANNED) metrics. Persisted via appSettings so the
    // choice survives across app launches; the setter round-trips to QSettings and
    // its NOTIFY keeps this in sync if the value changes elsewhere.
    property bool   _hidePlanned: appSettings.metricsHidePlanned
    property string _selectedKey: ""    // "" = directory (master)

    // Settings-search hook (see ScreenSettings.navigateToResult): return to the
    // directory and report success so the retry loop stops.
    function scrollToItem(itemId) {
        root._selectedKey = ""
        return true
    }

    // Deep link from elsewhere in the app (a dashboard tile → MetricRoute → Main →
    // ScreenSettings). Opens the detail view directly on `key`; an unknown key is
    // ignored so a stale link lands on the directory rather than a blank page.
    function showMetric(key) {
        if (!key || key.length === 0) return false
        if (Object.keys(catalog.descriptor(key, {})).length === 0) return false
        root._selectedKey = key
        return true
    }

    function _typeName(t) {
        switch (t) {
        case "summary":     return qsTr("Summary")
        case "pointInTime": return qsTr("Point in time")
        case "timeSeries":  return qsTr("Time series")
        case "sequence":    return qsTr("Sequence")
        }
        return t || ""
    }

    // Base filter (type only) — reads _typeFilter so bindings re-run on change.
    function _baseFilters() {
        return root._typeFilter.length > 0 ? { type: root._typeFilter } : ({})
    }
    // Post-filter: drop planned (roadmap-placeholder) rows when the toggle is on.
    // Reads _hidePlanned so callers' bindings re-run when the toggle flips.
    function _applyPlannedFilter(rows) {
        if (!root._hidePlanned) return rows
        return rows.filter(function(r) { return !r.planned })
    }
    // Rows for one group under the active type filter.
    function _query(group) {
        var f = root._baseFilters()
        f.group = group
        return root._applyPlannedFilter(catalog.query(f, {}))
    }
    // Total rows under the active filters (drives the empty state).
    readonly property int _totalCount: root._applyPlannedFilter(catalog.query(root._baseFilters(), {})).length

    // ══ Directory (master) ════════════════════════════════════════════════════
    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentWidth: availableWidth
        visible: root._selectedKey === ""

        ColumnLayout {
            id: contentCol
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   scrollView.availableWidth - Theme.sp(64)
            spacing: Theme.sp(20)

            // ── Page header ────────────────────────────────────────────────────
            Text {
                text:                qsTr("REFERENCE")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            PpDisplayText { text: qsTr("Metric catalogue") }

            Text {
                Layout.fillWidth: true
                text: qsTr("Every metric Pinpoint can measure — what it means, how to read it, and what it needs to compute. The producers behind a metric are shown as source tags.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            // ── Filter row: type chips (left) + Hide-planned toggle (right) ─────
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.sp(7)

                    // "All" chip
                    Rectangle {
                        readonly property bool active: root._typeFilter === ""
                        height: Theme.sp(28); radius: height / 2
                        width:  allLbl.implicitWidth + Theme.sp(24)
                        color: active ? Theme.colorAccentLight
                                      : allMa.containsMouse ? Theme.colorAccentMid : "transparent"
                        border.width: 1
                        border.color: active ? Theme.colorAccentMid
                                             : allMa.containsMouse ? Theme.colorAccentMid : Theme.colorBorderMid
                        Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
                        Text {
                            id: allLbl
                            anchors.centerIn: parent
                            text: qsTr("All")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: parent.active ? Theme.colorAccent : Theme.colorText2
                        }
                        MouseArea {
                            id: allMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root._typeFilter = ""
                        }
                    }

                    // One chip per metric type
                    Repeater {
                        model: catalog.types
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: root._typeFilter === modelData
                            height: Theme.sp(28); radius: height / 2
                            width:  typeChipLbl.implicitWidth + Theme.sp(24)
                            color: active ? Theme.colorAccentLight
                                          : typeChipMa.containsMouse ? Theme.colorAccentMid : "transparent"
                            border.width: 1
                            border.color: active ? Theme.colorAccentMid
                                                 : typeChipMa.containsMouse ? Theme.colorAccentMid : Theme.colorBorderMid
                            Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
                            Text {
                                id: typeChipLbl
                                anchors.centerIn: parent
                                text: root._typeName(modelData)
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color: parent.active ? Theme.colorAccent : Theme.colorText2
                            }
                            MouseArea {
                                id: typeChipMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root._typeFilter = modelData
                            }
                        }
                    }
                }

                // Hide-planned toggle — mirrors the "All" chip. When active,
                // roadmap-placeholder (PLANNED) metrics are dropped from the list.
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    readonly property bool active: root._hidePlanned
                    implicitHeight: Theme.sp(28); radius: implicitHeight / 2
                    implicitWidth:  hidePlannedLbl.implicitWidth + Theme.sp(24)
                    color: active ? Theme.colorAccentLight
                                  : hidePlannedMa.containsMouse ? Theme.colorAccentMid : "transparent"
                    border.width: 1
                    border.color: active ? Theme.colorAccentMid
                                         : hidePlannedMa.containsMouse ? Theme.colorAccentMid : Theme.colorBorderMid
                    Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
                    Text {
                        id: hidePlannedLbl
                        anchors.centerIn: parent
                        text: qsTr("Hide planned")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color: parent.active ? Theme.colorAccent : Theme.colorText2
                    }
                    MouseArea {
                        id: hidePlannedMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        // Write through to the persisted setting; _hidePlanned is bound
                        // to it and re-evaluates via NOTIFY (keeps the binding intact).
                        onClicked: appSettings.metricsHidePlanned = !appSettings.metricsHidePlanned
                    }
                }
            }

            PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

            // ── Grouped metric list ────────────────────────────────────────────
            Repeater {
                model: catalog.groups
                delegate: ColumnLayout {
                    id: groupBlock
                    required property var modelData
                    readonly property var rows: root._query(modelData)

                    Layout.fillWidth: true
                    spacing: Theme.sp(4)
                    visible: rows.length > 0

                    Text {
                        text:                groupBlock.modelData
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                        Layout.bottomMargin: Theme.sp(2)
                    }

                    Repeater {
                        model: groupBlock.rows
                        delegate: MetricRow {
                            required property var modelData
                            Layout.fillWidth: true
                            metric: modelData
                            onClicked: root._selectedKey = modelData.key
                        }
                    }
                }
            }

            // ── Empty state (a type with no metrics yet, or all filtered out) ──
            Text {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp(8)
                visible: root._totalCount === 0
                text: root._hidePlanned ? qsTr("No live metrics of this type — all are planned.")
                                        : qsTr("No metrics of this type yet.")
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText3
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(8) }
        }
    }

    // ══ Detail ════════════════════════════════════════════════════════════════
    Loader {
        anchors.fill: parent
        active:  root._selectedKey !== ""
        visible: active
        sourceComponent: Component {
            MetricDetail {
                anchors.fill: parent   // fill the Loader — else width-dependent body text collapses
                mc:        catalog
                labels:    metricLabels
                metricKey: root._selectedKey
                onBack:    root._selectedKey = ""
                // Forwarded rather than handled here: the destination is a DIFFERENT settings panel
                // (Diagnostics), and this panel has no business knowing about its siblings. The
                // screen that hosts both performs the jump — same division as showMetricDetail().
                onOpenNorm: function(measureId) { root.openNorm(measureId) }
            }
        }
    }
}
