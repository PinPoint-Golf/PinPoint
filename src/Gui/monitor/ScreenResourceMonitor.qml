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
import QtQuick.Controls
import PinPointStudio

Item {
    id: root

    signal navigateToSettings(int panelIndex)

    // Bottom log area is tabbed: 0 = Message Log, 1 = Stats History,
    // 2 = Analysis Runs, 3 = PinPointCapture (CR-02 goal (a)).
    // Always reset to Message Log when the screen is (re)entered.
    property int logTab: 0
    onVisibleChanged: if (visible) logTab = 0
    // Nothing polls the phones until their tab is actually in front.
    onLogTabChanged: if (logTab === 3) refreshPpcp()

    // ── The PinPointCapture tab's data ───────────────────────────────────────
    //
    // ⛔ READ IN QML, STRAIGHT OFF `ppcpHost`.  ResourceMonitorController must
    // not gain a PPCP include — `HAVE_PPCP_TRANSPORT` is defined for some
    // translation units and not others, which is why it already reaches phones
    // through a duck-typed property read rather than a header.  This tab needs
    // no C++ seam at all: `ppcpStats()` is Q_INVOKABLE and `phones` is a
    // Q_PROPERTY, so the whole tab is a QML read on the existing 500 ms timer
    // and adds no signal (a *reading* must never emit `phonesChanged()`).
    //
    // `ppcpHost` exists only where libppcp AND OpenSSL are both present (H0) —
    // the same guard ScreenSettings.qml and PhonesPanel.qml use.
    readonly property bool ppcpAvailable: (typeof ppcpHost !== "undefined")
    property var ppcpStatsData: ({})
    property var ppcpRows: []

    // One row per camera SOURCE for a phone, folded from `perPhone[].inputs`.
    //
    // ⛔ KEYED ON `source_id`, NEVER ON `serialNumber`.  A PPCP camera's
    // `serialNumber` IS the owning peer's id, so a two-camera phone collapses
    // onto ONE identity anywhere that is used as a key — the known defect in
    // the folded device stats, which CR-02 CB5 forbids repeating.  The row also
    // carries the full device id VideoInputPpcp::deviceIdFor() builds.
    //
    // ⚠ `inputs` IS PER CONSUMER, NOT PER CAMERA.  A host-owned preview
    // consumer (alive from `declare`) and a session tile are two live
    // VideoInputPpcp objects for the SAME Source, so the entries are summed
    // into one row per Source — otherwise one camera would show up twice while
    // the second camera of a two-camera phone still looked like a duplicate.
    function ppcpCamerasFor(peerId, stat) {
        var inputs = (stat && stat.inputs) ? stat.inputs : []
        var order  = []
        var bySource = ({})
        for (var i = 0; i < inputs.length; i++) {
            var e   = inputs[i]
            var sid = e.sourceId
            var row = bySource[sid]
            if (row === undefined) {
                row = { sourceId: sid,
                        deviceId: "ppcp:" + peerId + "/" + sid,
                        consumers: 0, attached: false, previewOnly: true,
                        previewFrames: 0, streamsOpened: 0, streamsRefused: 0,
                        decodeFailures: 0, absentSegments: 0, lastStreamError: "" }
                bySource[sid] = row
                order.push(row)
            }
            row.consumers      += 1
            row.attached        = row.attached || e.attached
            if (!e.previewOnly) row.previewOnly = false
            row.previewFrames  += e.previewFrames
            row.streamsOpened  += e.streamsOpened
            row.streamsRefused += e.streamsRefused
            row.decodeFailures += e.decodeFailures
            row.absentSegments += e.absentSegments
            if (row.lastStreamError === "" && e.lastStreamError)
                row.lastStreamError = e.lastStreamError
        }

        // ── MSG 5.5 — per-Source availability, merged in on `source_id` ─────
        //
        // ⛔ THE SAME KEY, AND A ROW MAY BE CREATED HERE.  A `device_status`
        // can name a Source with NO live consumer — a camera another
        // application took before anybody opened a Stream on it is exactly what
        // `in_use` is for — so the merge adds a row rather than only decorating
        // one.  Anything else would drop the reading that matters most.
        //
        // ⚠ `avail: ""` IS "NOTHING HAS BEEN SAID", not "available". 5.5a is
        // push-on-change, so a Source that has never had a problem has never
        // been reported on, and rendering that as a green "yes" would be
        // inventing a measurement.
        var ds = (stat && stat.deviceStatus) ? stat.deviceStatus : []
        for (var k = 0; k < ds.length; k++) {
            var d    = ds[k]
            var drow = bySource[d.sourceId]
            if (drow === undefined) {
                drow = { sourceId: d.sourceId,
                         deviceId: "ppcp:" + peerId + "/" + d.sourceId,
                         consumers: 0, attached: false, previewOnly: true,
                         previewFrames: 0, streamsOpened: 0, streamsRefused: 0,
                         decodeFailures: 0, absentSegments: 0, lastStreamError: "",
                         avail: "", availReason: "" }
                bySource[d.sourceId] = drow
                order.push(drow)
            }
            drow.avail       = d.available ? "yes" : "no"
            // 5.5b — a reason iff unavailable, from an open registry, carried
            // verbatim and never mapped onto a word this host already knows.
            drow.availReason = d.available ? "" : (d.reason ? d.reason : "")
        }
        for (var m = 0; m < order.length; m++) {
            if (order[m].avail === undefined) { order[m].avail = ""; order[m].availReason = "" }
        }
        return order
    }

    // Merge the two reads into one row per phone.  `phones` is the outer list
    // because a remembered-but-disconnected phone has NO `perPhone` entry and
    // must still appear — with every reading as an em dash, which is the honest
    // answer and not a fault.
    function refreshPpcp() {
        if (!ppcpAvailable) { ppcpStatsData = ({}); ppcpRows = []; return }
        var stats  = ppcpHost.ppcpStats()
        var per    = stats.perPhone ? stats.perPhone : []
        var byPeer = ({})
        for (var i = 0; i < per.length; i++) byPeer[per[i].peerId] = per[i]

        var phones = ppcpHost.phones
        var rows   = []
        for (var j = 0; j < phones.length; j++) {
            var ph = phones[j]
            var st = (ph.counterpartId && byPeer[ph.counterpartId] !== undefined)
                     ? byPeer[ph.counterpartId] : null
            rows.push({ phone: ph, stat: st,
                        cameras: ppcpCamerasFor(ph.counterpartId, st) })
        }
        ppcpStatsData = stats
        ppcpRows      = rows
    }

    // Refresh data every 500ms, but only while this screen is visible.
    // StackLayout sets non-active children to visible: false, so running: visible
    // gates the timer automatically without any extra wiring.
    Timer {
        interval: 500
        running: visible
        repeat: true
        onTriggered: {
            resourceMonitor.refresh()
            if (profiler.available) profiler.refresh()
            // Only while its own tab is in front: two QVariant reads a second
            // for a tab nobody is looking at is work for nothing.
            if (root.logTab === 3) root.refreshPpcp()
        }
    }

    // ── Left column: Devices ──────────────────────────────────────────────────
    ScrollView {
        id: leftScroll
        width: Theme.sp(320)
        anchors { top: parent.top; bottom: parent.bottom; left: parent.left }
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Column {
            width: Theme.sp(320)
            padding: Theme.sp(20)
            spacing: Theme.sp(12)

            // Section label
            Text {
                text: qsTr("DEVICES")
                font.family: Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
                bottomPadding: 8
            }

            Repeater {
                model: resourceMonitor.devices

                RmDeviceCard {
                    deviceData: modelData
                    width: Theme.sp(280)
                    onOpenSettingsRequested: root.navigateToSettings(modelData.kind === "Camera" ? 3 : 4)
                }
            }

            // Scan controls at bottom of devices column
            Item {
                width: Theme.sp(280)
                height: Theme.sp(28)

                Text {
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    text: resourceMonitor.scanStatus
                    font.family: Theme.fontData
                    font.pixelSize: Theme.sp(9)
                    color: Theme.colorText3
                }

                Rectangle {
                    id: scanBtn
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    width: scanLbl.implicitWidth + Theme.sp(16)
                    height: Theme.sp(18)
                    radius: Theme.radius
                    color: scanBtnHover.hovered
                           ? Theme.colorBg2
                           : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                    border.width: 1
                    border.color: resourceMonitor.scanning ? Theme.colorBorderMid
                                : scanBtnHover.hovered    ? Theme.colorAccentMid
                                :                           Theme.colorBorderStrong
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    opacity: resourceMonitor.scanning ? 0.5 : 1.0
                    scale: resourceMonitor.scanning ? 1.0 : (scanBtnTap.pressed ? 0.97 : scanBtnHover.hovered ? 1.02 : 1.0)
                    Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                    Text {
                        id: scanLbl
                        anchors.centerIn: parent
                        text: qsTr("Scan")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.sp(10)
                        color: resourceMonitor.scanning ? Theme.colorText3 : Theme.colorText2
                    }

                    TapHandler  { id: scanBtnTap; enabled: !resourceMonitor.scanning; onTapped: resourceMonitor.scanDevices() }
                    HoverHandler { id: scanBtnHover; cursorShape: resourceMonitor.scanning ? Qt.ArrowCursor : Qt.PointingHandCursor }
                }
            }
        }
    }

    // Vertical divider
    Rectangle {
        width: 1
        anchors { top: parent.top; bottom: parent.bottom; left: leftScroll.right }
        color: Theme.colorBorderMid
        opacity: 0.6
    }

    // ── Right column: Event buffer ────────────────────────────────────────────
    ScrollView {
        id: rightScroll
        anchors {
            top: parent.top; bottom: parent.bottom
            left: leftScroll.right; leftMargin: 1
            right: parent.right
        }
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Column {
            width: rightScroll.availableWidth
            padding: Theme.sp(20)
            spacing: Theme.sp(16)

            // Section label
            Text {
                text: qsTr("EVENT BUFFER")
                font.family: Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
                bottomPadding: 8
            }

            // Buffer state bar
            Rectangle {
                width: parent.width - 40
                height: Theme.sp(72)
                radius: Theme.radiusLg
                border.width: 1
                border.color: Theme.colorBorderMid
                color: Theme.colorSurface

                Row {
                    anchors { fill: parent; leftMargin: Theme.sp(16); rightMargin: Theme.sp(16) }
                    spacing: Theme.sp(12)

                    // Status dot
                    Rectangle {
                        width: Theme.sp(7)
                        height: Theme.sp(7)
                        radius: Theme.sp(4)
                        anchors.verticalCenter: parent.verticalCenter
                        color: {
                            var s = resourceMonitor.bufferState
                            if (s === "capturing") return Theme.colorGood
                            if (s === "paused")    return Theme.colorWarn
                            return Theme.colorBorderStrong
                        }
                    }

                    // State label
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: resourceMonitor.bufferState.toUpperCase()
                        font.family: Theme.fontData
                        font.pixelSize: Theme.fontSzDataSm
                        font.letterSpacing: Theme.trackingMicro
                        color: {
                            var s = resourceMonitor.bufferState
                            if (s === "capturing") return Theme.colorGood
                            if (s === "paused")    return Theme.colorWarn
                            return Theme.colorText3
                        }
                        width: Theme.sp(80)
                    }

                    // Divider
                    Rectangle {
                        width: 1
                        height: Theme.sp(40)
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.colorBorderMid
                    }

                    // KV pairs
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.sp(28)

                        // Total events
                        Column {
                            spacing: Theme.sp(2)
                            Text {
                                text: qsTr("EVENTS")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                            Text {
                                text: resourceMonitor.totalEventsStr
                                font.family: Theme.fontData
                                font.pixelSize: Theme.fontSzData
                                font.weight: Font.Light
                                color: Theme.colorText
                            }
                            Text {
                                text: qsTr("written")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                color: Theme.colorText3
                            }
                        }

                        // Timeline entries
                        Column {
                            spacing: Theme.sp(2)
                            Text {
                                text: qsTr("TIMELINE")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                            Text {
                                text: resourceMonitor.timelineEntriesStr
                                font.family: Theme.fontData
                                font.pixelSize: Theme.fontSzData
                                font.weight: Font.Light
                                color: Theme.colorText
                            }
                            Text {
                                text: qsTr("entries")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                color: Theme.colorText3
                            }
                        }

                        // Source count
                        Column {
                            spacing: Theme.sp(2)
                            Text {
                                text: qsTr("SOURCES")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                            Text {
                                text: resourceMonitor.sourceCount.toString()
                                font.family: Theme.fontData
                                font.pixelSize: Theme.fontSzData
                                font.weight: Font.Light
                                color: Theme.colorText
                            }
                            Text {
                                text: qsTr("active")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                color: Theme.colorText3
                            }
                        }
                    }
                }
            }

            // Sources section
            Column {
                width: parent.width - 40
                spacing: 0

                // Section label
                Text {
                    text: qsTr("REGISTERED SOURCES")
                    font.family: Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText3
                    bottomPadding: 8
                }

                // Header row
                Rectangle {
                    width: parent.width
                    height: Theme.sp(30)
                    color: Theme.colorBg2
                    radius: Theme.radiusLg

                    // Bottom square to remove bottom radius
                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: Theme.radiusLg
                        color: Theme.colorBg2
                    }

                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: 1
                        color: Theme.colorBorderMid
                    }

                    Row {
                        anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }

                        property int nameW: parent.width - Theme.sp(10) - Theme.sp(10) - Theme.sp(80) - Theme.sp(72) - Theme.sp(80) - Theme.sp(90) - Theme.sp(60)

                        Item {
                            width: parent.nameW
                            height: parent.height
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("SOURCE")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                        }

                        Item {
                            width: Theme.sp(80); height: parent.height
                            Text {
                                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                text: qsTr("WRITTEN")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                        }

                        Item {
                            width: Theme.sp(72); height: parent.height
                            Text {
                                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                text: qsTr("WRAPS")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                        }

                        Item {
                            width: Theme.sp(80); height: parent.height
                            Text {
                                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                text: qsTr("BYTES")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                        }

                        Item {
                            width: Theme.sp(90); height: parent.height
                            Text {
                                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                text: qsTr("MAX INTER")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                        }

                        Item {
                            width: Theme.sp(60); height: parent.height
                            Text {
                                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                text: qsTr("FILL")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: Theme.colorText3
                            }
                        }
                    }
                }

                // Source rows
                Repeater {
                    model: resourceMonitor.sources

                    RmSourceRow {
                        sourceData: modelData
                        isAlternate: index % 2 === 1
                        width: parent.width
                    }
                }

                // Empty state
                Rectangle {
                    visible: resourceMonitor.sources.length === 0
                    width: parent.width
                    height: Theme.sp(40)
                    color: Theme.colorSurface

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("No sources registered")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color: Theme.colorText3
                    }
                }
            }

            // Timeline chart
            RmTimelineChart {
                width: parent.width - 40
                historyData: resourceMonitor.timelineHistory
                bufferState: resourceMonitor.bufferState
            }

            // ── Resource profiler ─────────────────────────────────────────────
            // Hidden when the feature is compiled out (GA builds). All values are
            // pre-formatted in ProfilerController — pure bindings here.
            Column {
                id: profilerSection
                visible: profiler.available
                width: parent.width - 40
                spacing: Theme.sp(12)

                // Header: section label + deep toggle + Reset + Dump-to-log
                Item {
                    width: parent.width
                    height: Theme.sp(28)

                    Text {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        text: qsTr("PROFILER")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                    }

                    Row {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        spacing: Theme.sp(8)

                        // Deep toggle — greyed when the deep tier isn't compiled in
                        Rectangle {
                            id: deepChip
                            readonly property bool on: profiler.deepEnabled
                            width: deepLbl.implicitWidth + Theme.sp(16)
                            height: Theme.sp(18)
                            radius: Theme.sp(9)
                            opacity: profiler.deepAvailable ? 1.0 : 0.4
                            color: on ? Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b, 0.15)
                                      : deepHover.hovered ? Theme.colorBg2
                                      : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                            border.width: 1
                            border.color: on ? Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b, 0.4)
                                             : deepHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            scale: profiler.deepAvailable ? (deepTap.pressed ? 0.97 : deepHover.hovered ? 1.02 : 1.0) : 1.0
                            Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                            Text {
                                id: deepLbl
                                anchors.centerIn: parent
                                text: qsTr("DEEP")
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: parent.on ? Theme.colorAccent : Theme.colorText3
                            }

                            TapHandler  { id: deepTap; enabled: profiler.deepAvailable; onTapped: profiler.deepEnabled = !profiler.deepEnabled }
                            HoverHandler { id: deepHover; cursorShape: profiler.deepAvailable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                        }

                        // Reset
                        Rectangle {
                            width: resetLbl.implicitWidth + Theme.sp(16)
                            height: Theme.sp(18)
                            radius: Theme.radius
                            color: resetHover.hovered
                                   ? Theme.colorBg2
                                   : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                            border.width: 1
                            border.color: resetHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            scale: resetTap.pressed ? 0.97 : resetHover.hovered ? 1.02 : 1.0
                            Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }
                            Text {
                                id: resetLbl
                                anchors.centerIn: parent
                                text: qsTr("Reset")
                                font.family: Theme.fontBody
                                font.pixelSize: Theme.sp(10)
                                color: Theme.colorText3
                            }
                            TapHandler  { id: resetTap; onTapped: profiler.reset() }
                            HoverHandler { id: resetHover; cursorShape: Qt.PointingHandCursor }
                        }

                        // Dump to log
                        Rectangle {
                            width: dumpLbl.implicitWidth + Theme.sp(16)
                            height: Theme.sp(18)
                            radius: Theme.radius
                            color: dumpHover.hovered
                                   ? Theme.colorBg2
                                   : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                            border.width: 1
                            border.color: dumpHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            scale: dumpTap.pressed ? 0.97 : dumpHover.hovered ? 1.02 : 1.0
                            Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }
                            Text {
                                id: dumpLbl
                                anchors.centerIn: parent
                                text: qsTr("Dump to log")
                                font.family: Theme.fontBody
                                font.pixelSize: Theme.sp(10)
                                color: Theme.colorText3
                            }
                            TapHandler  { id: dumpTap; onTapped: profiler.dumpToLog() }
                            HoverHandler { id: dumpHover; cursorShape: Qt.PointingHandCursor }
                        }
                    }
                }

                // Gauge strip — process CPU% and RSS, each with its peak watermark
                Rectangle {
                    width: parent.width
                    height: Theme.sp(64)
                    radius: Theme.radiusLg
                    border.width: 1
                    border.color: Theme.colorBorderMid
                    color: Theme.colorSurface

                    Row {
                        anchors { fill: parent; leftMargin: Theme.sp(16); rightMargin: Theme.sp(16) }
                        spacing: Theme.sp(28)

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.sp(2)
                            Text {
                                text: qsTr("PROCESS CPU")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                            Text {
                                text: profiler.cpuPercentStr
                                font.family: Theme.fontData; font.pixelSize: Theme.fontSzData
                                font.weight: Font.Light; color: Theme.colorText
                            }
                            Text {
                                text: qsTr("peak %1").arg(profiler.peakCpuPercentStr)
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9); color: Theme.colorText3
                            }
                        }

                        Rectangle {
                            width: 1; height: Theme.sp(40)
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.colorBorderMid
                        }

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.sp(2)
                            Text {
                                text: qsTr("RSS")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                            Text {
                                text: profiler.rssBytesStr
                                font.family: Theme.fontData; font.pixelSize: Theme.fontSzData
                                font.weight: Font.Light; color: Theme.colorText
                            }
                            Text {
                                text: qsTr("peak %1").arg(profiler.peakRssBytesStr)
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9); color: Theme.colorText3
                            }
                        }
                    }
                }

                // GPU gauge — device + this-process VRAM. On a CPU/no-GPU host
                // (no supported GPU memory source) it collapses to a muted line.
                Rectangle {
                    width: parent.width
                    height: Theme.sp(64)
                    radius: Theme.radiusLg
                    border.width: 1
                    border.color: Theme.colorBorderMid
                    color: Theme.colorSurface

                    Text {
                        visible: !profiler.gpuAvailable
                        anchors { left: parent.left; leftMargin: Theme.sp(16); verticalCenter: parent.verticalCenter }
                        text: qsTr("GPU  —  none (CPU inference)")
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
                        color: Theme.colorText3
                    }

                    Row {
                        visible: profiler.gpuAvailable
                        anchors { fill: parent; leftMargin: Theme.sp(16); rightMargin: Theme.sp(16) }
                        spacing: Theme.sp(28)

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.sp(2)
                            Text {
                                text: qsTr("GPU (PROCESS)")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                            Text {
                                text: profiler.gpuProcessBytesStr
                                font.family: Theme.fontData; font.pixelSize: Theme.fontSzData
                                font.weight: Font.Light; color: Theme.colorText
                            }
                            Text {
                                text: qsTr("peak %1").arg(profiler.gpuPeakProcessBytesStr)
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9); color: Theme.colorText3
                            }
                        }

                        Rectangle {
                            width: 1; height: Theme.sp(40)
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.colorBorderMid
                        }

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.sp(2)
                            Text {
                                text: qsTr("GPU (DEVICE)")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                            Text {
                                // used/total when the backend reports device-wide used (NVML);
                                // total/budget only for DXGI/Metal.
                                text: profiler.gpuDeviceUsedBytes > 0
                                      ? qsTr("%1 / %2").arg(profiler.gpuDeviceUsedBytesStr).arg(profiler.gpuDeviceTotalBytesStr)
                                      : profiler.gpuDeviceTotalBytesStr
                                font.family: Theme.fontData; font.pixelSize: Theme.fontSzData
                                font.weight: Font.Light; color: Theme.colorText
                            }
                            Text {
                                width: Theme.sp(190)
                                text: profiler.gpuBackend
                                      + " · " + profiler.gpuDeviceName
                                      + (profiler.gpuUnified ? qsTr("  (unified)") : "")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                color: Theme.colorText3; elide: Text.ElideRight
                            }
                        }
                    }
                }

                // Per-thread CPU
                Column {
                    width: parent.width
                    spacing: 0

                    Text {
                        text: qsTr("THREADS")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                        bottomPadding: 8
                    }

                    Repeater {
                        model: profiler.threads

                        Item {
                            width: parent.width
                            height: Theme.sp(22)

                            Text {
                                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                                text: modelData.name
                                font.family: Theme.fontData
                                font.pixelSize: Theme.fontSzDataSm
                                color: Theme.colorText2
                            }

                            Rectangle {
                                anchors { right: thVal.left; rightMargin: Theme.sp(10); verticalCenter: parent.verticalCenter }
                                width: Theme.sp(90); height: Theme.sp(4); radius: Theme.sp(2)
                                color: Theme.colorBg3
                                Rectangle {
                                    height: Theme.sp(4); radius: Theme.sp(2)
                                    width: parent.width * Math.min(1.0, modelData.cpuPercent / 100.0)
                                    color: modelData.cpuPercent > 85 ? Theme.colorWarn : Theme.colorGood
                                }
                            }

                            Text {
                                id: thVal
                                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                width: Theme.sp(56)
                                horizontalAlignment: Text.AlignRight
                                text: modelData.cpuPercentStr
                                font.family: Theme.fontData
                                font.pixelSize: Theme.fontSzDataSm
                                color: Theme.colorText2
                            }
                        }
                    }

                    Rectangle {
                        visible: profiler.threads.length === 0
                        width: parent.width
                        height: Theme.sp(28)
                        color: Theme.colorSurface
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("No threads registered")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText3
                        }
                    }
                }

                // Scopes table — NAME / CALLS / TOTAL / AVG / MAX / CPU
                Column {
                    width: parent.width
                    spacing: 0

                    Text {
                        text: qsTr("SCOPES")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                        bottomPadding: 8
                    }

                    Rectangle {
                        width: parent.width
                        height: Theme.sp(30)
                        color: Theme.colorBg2
                        radius: Theme.radiusLg
                        Rectangle {
                            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                            height: Theme.radiusLg; color: Theme.colorBg2
                        }
                        Rectangle {
                            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                            height: 1; color: Theme.colorBorderMid
                        }

                        Row {
                            anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }
                            property int nameW: parent.width - Theme.sp(10) - Theme.sp(10) - Theme.sp(64) - Theme.sp(76) - Theme.sp(76) - Theme.sp(76) - Theme.sp(76)

                            Item {
                                width: parent.nameW; height: parent.height
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("SCOPE")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(64); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("CALLS")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(76); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("TOTAL")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(76); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("AVG")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(76); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("MAX")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(76); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("CPU")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro
                                    color: profiler.deepEnabled ? Theme.colorText3 : Theme.colorBorderStrong
                                }
                            }
                        }
                    }

                    Repeater {
                        model: profiler.scopes
                        RmProfilerRow {
                            scopeData: modelData
                            isAlternate: index % 2 === 1
                            deepOn: profiler.deepEnabled
                            width: parent.width
                        }
                    }

                    Rectangle {
                        visible: profiler.scopes.length === 0
                        width: parent.width
                        height: Theme.sp(28)
                        color: Theme.colorSurface
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("No scopes recorded")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText3
                        }
                    }
                }

                // Analysis stages table — per-pipeline-stage wall time (STAGE / CALLS / TOTAL / AVG / MAX)
                Column {
                    width: parent.width
                    spacing: 0

                    Text {
                        text: qsTr("ANALYSIS STAGES")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                        bottomPadding: 2
                    }
                    Text {
                        text: qsTr("per shot-analysis pipeline stage, averaged/peaked across this session — CPU is not attributed per stage (see the process/thread gauge)")
                        width: parent.width
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color: Theme.colorText3
                        bottomPadding: 8
                    }

                    Rectangle {
                        width: parent.width
                        height: Theme.sp(30)
                        color: Theme.colorBg2
                        radius: Theme.radiusLg
                        Rectangle {
                            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                            height: Theme.radiusLg; color: Theme.colorBg2
                        }
                        Rectangle {
                            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                            height: 1; color: Theme.colorBorderMid
                        }

                        Row {
                            anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }
                            property int nameW: parent.width - Theme.sp(10) - Theme.sp(10) - Theme.sp(64) - Theme.sp(76) - Theme.sp(76) - Theme.sp(76) - Theme.sp(76)

                            Item {
                                width: parent.nameW; height: parent.height
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("STAGE")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(64); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("CALLS")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(76); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("TOTAL")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(76); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("AVG")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(76); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("MAX")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(76); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("CPU")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorBorderStrong
                                }
                            }
                        }
                    }

                    Repeater {
                        model: profiler.analysisStages
                        RmProfilerRow {
                            scopeData: modelData
                            isAlternate: index % 2 === 1
                            deepOn: profiler.deepEnabled
                            width: parent.width
                        }
                    }

                    Rectangle {
                        visible: profiler.analysisStages.length === 0
                        width: parent.width
                        height: Theme.sp(28)
                        color: Theme.colorSurface
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("No analyses recorded — run a shot or re-analyse a swing")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText3
                        }
                    }
                }

                // Memory table — CATEGORY / CURRENT / PEAK
                Column {
                    width: parent.width
                    spacing: 0

                    Text {
                        text: qsTr("MEMORY")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                        bottomPadding: 8
                    }

                    Rectangle {
                        width: parent.width
                        height: Theme.sp(30)
                        color: Theme.colorBg2
                        radius: Theme.radiusLg
                        Rectangle {
                            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                            height: Theme.radiusLg; color: Theme.colorBg2
                        }
                        Rectangle {
                            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                            height: 1; color: Theme.colorBorderMid
                        }

                        Row {
                            anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }

                            Item {
                                width: parent.width - Theme.sp(100) - Theme.sp(100); height: parent.height
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("CATEGORY")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(100); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("CURRENT")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(100); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("PEAK")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                        }
                    }

                    Repeater {
                        model: profiler.memory

                        Rectangle {
                            width: parent.width
                            height: Theme.sp(28)
                            color: index % 2 === 1 ? Theme.colorBg : Theme.colorSurface

                            Row {
                                anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }

                                Item {
                                    width: parent.width - Theme.sp(100) - Theme.sp(100); height: parent.height
                                    Text {
                                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                                        width: parent.width
                                        text: modelData.name
                                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
                                        color: Theme.colorText2; elide: Text.ElideRight
                                    }
                                }
                                Item {
                                    width: Theme.sp(100); height: parent.height
                                    Text {
                                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                        text: modelData.currentStr
                                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
                                        color: Theme.colorText2
                                    }
                                }
                                Item {
                                    width: Theme.sp(100); height: parent.height
                                    Text {
                                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                        text: modelData.peakStr
                                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
                                        color: Theme.colorText2
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        visible: profiler.memory.length === 0
                        width: parent.width
                        height: Theme.sp(28)
                        color: Theme.colorSurface
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("No memory categories")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText3
                        }
                    }
                }

                // GPU MEMORY table — per-subsystem VRAM attribution (GPU.* categories)
                Column {
                    width: parent.width
                    spacing: 0

                    Text {
                        text: qsTr("GPU MEMORY")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                        bottomPadding: 2
                    }
                    Text {
                        text: qsTr("measured process-VRAM delta per model — the first model loaded also carries the shared GPU runtime/context")
                        width: parent.width
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color: Theme.colorText3
                        bottomPadding: 8
                    }

                    Rectangle {
                        width: parent.width
                        height: Theme.sp(30)
                        color: Theme.colorBg2
                        radius: Theme.radiusLg
                        Rectangle {
                            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                            height: Theme.radiusLg; color: Theme.colorBg2
                        }
                        Rectangle {
                            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                            height: 1; color: Theme.colorBorderMid
                        }

                        Row {
                            anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }

                            Item {
                                width: parent.width - Theme.sp(100) - Theme.sp(100); height: parent.height
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("CATEGORY")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(100); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("CURRENT")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                            Item {
                                width: Theme.sp(100); height: parent.height
                                Text {
                                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: qsTr("PEAK")
                                    font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                                }
                            }
                        }
                    }

                    Repeater {
                        model: profiler.gpuMemory

                        Rectangle {
                            width: parent.width
                            height: Theme.sp(28)
                            color: index % 2 === 1 ? Theme.colorBg : Theme.colorSurface

                            Row {
                                anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }

                                Item {
                                    width: parent.width - Theme.sp(100) - Theme.sp(100); height: parent.height
                                    Text {
                                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                                        width: parent.width
                                        text: modelData.name
                                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
                                        color: Theme.colorText2; elide: Text.ElideRight
                                    }
                                }
                                Item {
                                    width: Theme.sp(100); height: parent.height
                                    Text {
                                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                        text: modelData.currentStr
                                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
                                        color: Theme.colorText2
                                    }
                                }
                                Item {
                                    width: Theme.sp(100); height: parent.height
                                    Text {
                                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                        text: modelData.peakStr
                                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
                                        color: Theme.colorText2
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        visible: profiler.gpuMemory.length === 0
                        width: parent.width
                        height: Theme.sp(28)
                        color: Theme.colorSurface
                        Text {
                            anchors.centerIn: parent
                            text: profiler.gpuAvailable ? qsTr("No GPU allocations attributed yet")
                                                        : qsTr("No GPU categories (CPU inference)")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText3
                        }
                    }
                }
            }

            // ── Tab bar: Message Log / Stats History ─────────────────────────
            Row {
                id: logTabBar
                width: parent.width - 40
                spacing: Theme.sp(20)

                // Message Log tab
                Item {
                    width: msgTabLbl.implicitWidth
                    height: Theme.sp(24)
                    transformOrigin: Item.Left
                    scale: msgTabTap.pressed ? 0.97 : msgTabHover.hovered ? 1.02 : 1.0
                    Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                    Text {
                        id: msgTabLbl
                        anchors { left: parent.left; top: parent.top; topMargin: Theme.sp(3) }
                        text: qsTr("MESSAGE LOG")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: root.logTab === 0 ? Theme.colorText2
                             : msgTabHover.hovered ? Theme.colorText2 : Theme.colorText3
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: Theme.sp(2)
                        radius: 1
                        color: Theme.colorAccent
                        visible: root.logTab === 0
                    }
                    TapHandler   { id: msgTabTap; onTapped: root.logTab = 0 }
                    HoverHandler { id: msgTabHover; cursorShape: Qt.PointingHandCursor }
                }

                // Stats History tab — only when the profiler is compiled in
                Item {
                    visible: profiler.available
                    width: statsTabLbl.implicitWidth
                    height: Theme.sp(24)
                    transformOrigin: Item.Left
                    scale: statsTabTap.pressed ? 0.97 : statsTabHover.hovered ? 1.02 : 1.0
                    Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                    Text {
                        id: statsTabLbl
                        anchors { left: parent.left; top: parent.top; topMargin: Theme.sp(3) }
                        text: qsTr("STATS HISTORY")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: root.logTab === 1 ? Theme.colorText2
                             : statsTabHover.hovered ? Theme.colorText2 : Theme.colorText3
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: Theme.sp(2)
                        radius: 1
                        color: Theme.colorAccent
                        visible: root.logTab === 1
                    }
                    TapHandler   { id: statsTabTap; onTapped: root.logTab = 1 }
                    HoverHandler { id: statsTabHover; cursorShape: Qt.PointingHandCursor }
                }

                // Analysis Runs tab — the per-analysis drill-down (profiler compiled in)
                Item {
                    visible: profiler.available
                    width: anTabLbl.implicitWidth
                    height: Theme.sp(24)
                    transformOrigin: Item.Left
                    scale: anTabTap.pressed ? 0.97 : anTabHover.hovered ? 1.02 : 1.0
                    Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                    Text {
                        id: anTabLbl
                        anchors { left: parent.left; top: parent.top; topMargin: Theme.sp(3) }
                        text: qsTr("ANALYSIS RUNS")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: root.logTab === 2 ? Theme.colorText2
                             : anTabHover.hovered ? Theme.colorText2 : Theme.colorText3
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: Theme.sp(2)
                        radius: 1
                        color: Theme.colorAccent
                        visible: root.logTab === 2
                    }
                    TapHandler   { id: anTabTap; onTapped: root.logTab = 2 }
                    HoverHandler { id: anTabHover; cursorShape: Qt.PointingHandCursor }
                }

                // PinPointCapture tab — the phones, their links and their
                // cameras.  Gated on `ppcpHost` existing the way the two tabs
                // above are gated on the profiler being compiled in: a build
                // without libppcp/OpenSSL has no phones to report and shows
                // three tabs, exactly as it does today.
                Item {
                    visible: root.ppcpAvailable
                    width: ppcpTabLbl.implicitWidth
                    height: Theme.sp(24)
                    transformOrigin: Item.Left
                    scale: ppcpTabTap.pressed ? 0.97 : ppcpTabHover.hovered ? 1.02 : 1.0
                    Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                    Text {
                        id: ppcpTabLbl
                        anchors { left: parent.left; top: parent.top; topMargin: Theme.sp(3) }
                        text: qsTr("PINPOINTCAPTURE")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: root.logTab === 3 ? Theme.colorText2
                             : ppcpTabHover.hovered ? Theme.colorText2 : Theme.colorText3
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: Theme.sp(2)
                        radius: 1
                        color: Theme.colorAccent
                        visible: root.logTab === 3
                    }
                    TapHandler   { id: ppcpTabTap; onTapped: root.logTab = 3 }
                    HoverHandler { id: ppcpTabHover; cursorShape: Qt.PointingHandCursor }
                }
            }

            // ── Message log (grows downward; lives at bottom) ─────────────────
            Column {
                id: msgLogColumn
                visible: root.logTab === 0
                width: parent.width - 40
                spacing: 0

                // Active severity filters — reassign (don't mutate) to trigger reactivity.
                // INFO starts off: FFmpeg capture makes it noisy; opt in via the chip.
                property var    activeFilters: ({"INFO": false, "WARN": true, "ERROR": true, "FATAL": true})
                property string textFilter:    ""

                function toggleFilter(sev) {
                    var f = Object.assign({}, activeFilters)
                    f[sev] = !f[sev]
                    activeFilters = f
                }

                function severityColor(sev) {
                    if (sev === "WARN")                    return Theme.colorWarn
                    if (sev === "ERROR" || sev === "FATAL") return Theme.colorError
                    if (sev === "INFO")                    return Theme.colorText2
                    return Theme.colorText3  // DEBUG
                }

                property var filteredLog: {
                    var filters = activeFilters
                    var needle  = textFilter.toLowerCase()
                    var log = resourceMonitor.messageLog
                    var result = []
                    for (var i = 0; i < log.length; i++) {
                        var e = log[i]
                        if (filters[e.severity] === false) continue
                        if (needle !== "" && e.message.toLowerCase().indexOf(needle) === -1) continue
                        result.push(e)
                    }
                    return result
                }

                // Header
                Item {
                    width: parent.width
                    height: Theme.sp(32)

                    // Entry count
                    Text {
                        visible: resourceMonitor.messageLog.length > 0
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        text: qsTr("%1 entries").arg(msgLogColumn.filteredLog.length)
                        font.family: Theme.fontData
                        font.pixelSize: Theme.sp(9)
                        color: Theme.colorText3
                    }

                    // Clear button
                    Rectangle {
                        id: clearBtn
                        visible: resourceMonitor.messageLog.length > 0
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        width: clearLbl.implicitWidth + Theme.sp(16)
                        height: Theme.sp(18)
                        radius: Theme.radius
                        color: clearHover.hovered
                               ? Theme.colorBg2
                               : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                        border.width: 1
                        border.color: clearHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        scale: clearTap.pressed ? 0.97 : clearHover.hovered ? 1.02 : 1.0
                        Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                        Text {
                            id: clearLbl
                            anchors.centerIn: parent
                            text: qsTr("Clear")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                        }

                        TapHandler  { id: clearTap; onTapped: resourceMonitor.clearLog() }
                        HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                    }

                    // Export button — writes the log to a text file in the home directory
                    Rectangle {
                        id: exportBtn
                        visible: resourceMonitor.messageLog.length > 0
                        anchors { right: clearBtn.left; rightMargin: Theme.sp(8); verticalCenter: parent.verticalCenter }
                        width: exportLbl.implicitWidth + Theme.sp(16)
                        height: Theme.sp(18)
                        radius: Theme.radius
                        color: exportHover.hovered
                               ? Theme.colorBg2
                               : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                        border.width: 1
                        border.color: exportHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        scale: exportTap.pressed ? 0.97 : exportHover.hovered ? 1.02 : 1.0
                        Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                        Text {
                            id: exportLbl
                            anchors.centerIn: parent
                            text: qsTr("Export")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                        }

                        TapHandler {
                            id: exportTap
                            onTapped: {
                                var path = resourceMonitor.exportLog()
                                exportToast.copyText = path   // empty on failure → copy icon hidden
                                if (path.length > 0) exportToast.show(qsTr("Log exported to %1").arg(path))
                                else                 exportToast.show(qsTr("Log export failed"))
                            }
                        }
                        HoverHandler { id: exportHover; cursorShape: Qt.PointingHandCursor }
                    }

                    // Inline text filter box
                    Rectangle {
                        id: filterBox
                        visible: resourceMonitor.messageLog.length > 0
                        anchors { right: filterChips.left; rightMargin: Theme.sp(8); verticalCenter: parent.verticalCenter }
                        width: Theme.sp(120)
                        height: Theme.sp(18)
                        radius: Theme.radius
                        color: Theme.colorBg2
                        border.width: 1
                        border.color: filterInput.activeFocus ? Theme.colorAccent : Theme.colorBorderMid

                        Text {
                            anchors { left: parent.left; leftMargin: Theme.sp(7); verticalCenter: parent.verticalCenter }
                            visible: filterInput.text.length === 0
                            text: qsTr("Filter…")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                        }

                        TextInput {
                            id: filterInput
                            anchors { left: parent.left; right: clearFilter.left; leftMargin: Theme.sp(7); rightMargin: Theme.sp(4); verticalCenter: parent.verticalCenter }
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText
                            selectionColor: Theme.colorAccentMid
                            selectedTextColor: Theme.colorText
                            clip: true
                            onTextChanged: msgLogColumn.textFilter = text
                        }

                        Text {
                            id: clearFilter
                            anchors { right: parent.right; rightMargin: Theme.sp(5); verticalCenter: parent.verticalCenter }
                            visible: filterInput.text.length > 0
                            text: "×"
                            font.pixelSize: Theme.sp(12)
                            color: Theme.colorText3
                            TapHandler   { onTapped: { filterInput.text = ""; filterInput.forceActiveFocus() } }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                        }
                    }

                    // Severity filter chips
                    Row {
                        id: filterChips
                        visible: resourceMonitor.messageLog.length > 0
                        anchors { right: exportBtn.left; rightMargin: Theme.sp(8); verticalCenter: parent.verticalCenter }
                        spacing: Theme.sp(4)

                        Repeater {
                            model: ["INFO", "WARN", "ERROR"]

                            Rectangle {
                                required property string modelData

                                readonly property bool   active:   msgLogColumn.activeFilters[modelData] !== false
                                readonly property color  sevColor: msgLogColumn.severityColor(modelData)

                                width:  chipLbl.implicitWidth + Theme.sp(10)
                                height: Theme.sp(18)
                                radius: Theme.sp(9)
                                color: active
                                    ? Qt.rgba(sevColor.r, sevColor.g, sevColor.b, sevChipHover.hovered ? 0.22 : 0.15)
                                    : sevChipHover.hovered
                                      ? Qt.rgba(sevColor.r, sevColor.g, sevColor.b, 0.08)
                                      : "transparent"
                                border.width: 1
                                border.color: active
                                    ? Qt.rgba(sevColor.r, sevColor.g, sevColor.b, sevChipHover.hovered ? 0.55 : 0.4)
                                    : sevChipHover.hovered
                                      ? Qt.rgba(sevColor.r, sevColor.g, sevColor.b, 0.4)
                                      : Theme.colorBorderMid
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                                Text {
                                    id: chipLbl
                                    anchors.centerIn: parent
                                    text: parent.modelData
                                    font.family: Theme.fontData
                                    font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro
                                    color: parent.active ? parent.sevColor : Theme.colorText3
                                }

                                TapHandler  { onTapped: msgLogColumn.toggleFilter(parent.modelData) }
                                HoverHandler { id: sevChipHover; cursorShape: Qt.PointingHandCursor }
                            }
                        }
                    }
                }

                // Column header for entries
                Rectangle {
                    visible: resourceMonitor.messageLog.length > 0
                    width: parent.width
                    height: Theme.sp(24)
                    color: Theme.colorBg2
                    radius: Theme.radiusLg
                    // Square bottom corners
                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: Theme.radiusLg; color: Theme.colorBg2
                    }
                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: 1; color: Theme.colorBorderMid
                    }

                    Row {
                        anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }

                        Item {
                            width: Theme.sp(62); height: parent.height
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("TIME")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                        }

                        Item {
                            width: Theme.sp(52); height: parent.height
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("SEVERITY")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("MESSAGE")
                            font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                            font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                        }
                    }
                }

                // Empty state
                Rectangle {
                    visible: msgLogColumn.filteredLog.length === 0
                    width: parent.width
                    height: Theme.sp(40)
                    color: Theme.colorSurface
                    radius: Theme.radius
                    border.width: 1
                    border.color: Theme.colorBorderMid

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("No messages logged")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color: Theme.colorText3
                    }
                }

                // Log rows — newest first
                Repeater {
                    model: msgLogColumn.filteredLog

                    Rectangle {
                        required property var  modelData
                        required property int  index

                        width: parent.width
                        height: logMsg.implicitHeight + Theme.sp(12)
                        color: index % 2 === 0 ? Theme.colorSurface : Theme.colorBg
                        radius: 0
                        bottomLeftRadius:  index === msgLogColumn.filteredLog.length - 1 ? Theme.radius : 0
                        bottomRightRadius: index === msgLogColumn.filteredLog.length - 1 ? Theme.radius : 0

                        // Helpers: severity → colour and abbreviation
                        readonly property color  sevColor: {
                            var s = modelData.severity
                            if (s === "WARN")            return Theme.colorWarn
                            if (s === "ERROR" || s === "FATAL") return Theme.colorError
                            if (s === "INFO")            return Theme.colorText2
                            return Theme.colorText3   // DEBUG
                        }

                        // Timestamp
                        TextEdit {
                            id: logTs
                            anchors { left: parent.left; top: parent.top
                                      leftMargin: Theme.sp(10); topMargin: Theme.sp(6) }
                            width: Theme.sp(52)
                            text: modelData.timestamp
                            font.family: Theme.fontData
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                            readOnly: true
                            selectByMouse: true
                            selectionColor: Theme.colorAccentMid
                            selectedTextColor: Theme.colorText
                        }

                        // Severity badge
                        Rectangle {
                            id: sevBadge
                            anchors { left: logTs.right; top: parent.top
                                      leftMargin: Theme.sp(4); topMargin: Theme.sp(5) }
                            width: Theme.sp(44)
                            height: Theme.sp(16)
                            radius: Theme.sp(3)
                            color: Qt.rgba(parent.sevColor.r, parent.sevColor.g,
                                           parent.sevColor.b, 0.12)
                            border.width: 1
                            border.color: Qt.rgba(parent.sevColor.r, parent.sevColor.g,
                                                  parent.sevColor.b, 0.35)

                            Text {
                                anchors.centerIn: parent
                                text: modelData.severity
                                font.family: Theme.fontData
                                font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro
                                color: parent.parent.sevColor
                            }
                        }

                        // Message text
                        TextEdit {
                            id: logMsg
                            anchors {
                                left: sevBadge.right; right: parent.right; top: parent.top
                                leftMargin: Theme.sp(8); rightMargin: Theme.sp(10)
                                topMargin: Theme.sp(6)
                            }
                            text: modelData.message
                            wrapMode: TextEdit.WordWrap
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight: Theme.fontBodyWeight
                            color: Theme.colorText2
                            readOnly: true
                            selectByMouse: true
                            selectionColor: Theme.colorAccentMid
                            selectedTextColor: Theme.colorText
                        }
                    }
                }
            }

            // ── Stats history — the dedicated PpStatsLog ring (own filter) ──
            Column {
                id: statsCol
                visible: root.logTab === 1 && profiler.available
                width: parent.width - 40
                spacing: 0

                // Header: category chips + text filter + Clear/Export
                Item {
                    width: parent.width
                    height: Theme.sp(32)

                    // Clear
                    Rectangle {
                        id: statsClearBtn
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        width: statsClearLbl.implicitWidth + Theme.sp(16)
                        height: Theme.sp(18)
                        radius: Theme.radius
                        color: statsClearHover.hovered
                               ? Theme.colorBg2
                               : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                        border.width: 1
                        border.color: statsClearHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        scale: statsClearTap.pressed ? 0.97 : statsClearHover.hovered ? 1.02 : 1.0
                        Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }
                        Text {
                            id: statsClearLbl
                            anchors.centerIn: parent
                            text: qsTr("Clear")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                        }
                        TapHandler  { id: statsClearTap; onTapped: profiler.clearStats() }
                        HoverHandler { id: statsClearHover; cursorShape: Qt.PointingHandCursor }
                    }

                    // Export
                    Rectangle {
                        id: statsExportBtn
                        anchors { right: statsClearBtn.left; rightMargin: Theme.sp(8); verticalCenter: parent.verticalCenter }
                        width: statsExportLbl.implicitWidth + Theme.sp(16)
                        height: Theme.sp(18)
                        radius: Theme.radius
                        color: statsExportHover.hovered
                               ? Theme.colorBg2
                               : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                        border.width: 1
                        border.color: statsExportHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        scale: statsExportTap.pressed ? 0.97 : statsExportHover.hovered ? 1.02 : 1.0
                        Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }
                        Text {
                            id: statsExportLbl
                            anchors.centerIn: parent
                            text: qsTr("Export")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                        }
                        TapHandler {
                            id: statsExportTap
                            onTapped: {
                                var path = profiler.exportStats()
                                exportToast.copyText = path
                                if (path.length > 0) exportToast.show(qsTr("Stats exported to %1").arg(path))
                                else                 exportToast.show(qsTr("Stats export failed"))
                            }
                        }
                        HoverHandler { id: statsExportHover; cursorShape: Qt.PointingHandCursor }
                    }

                    // Category filter chips
                    Row {
                        id: statsChips
                        anchors { right: statsExportBtn.left; rightMargin: Theme.sp(8); verticalCenter: parent.verticalCenter }
                        spacing: Theme.sp(4)

                        Repeater {
                            model: profiler.statsCategories

                            Rectangle {
                                readonly property bool active: modelData.active

                                width: statsChipLbl.implicitWidth + Theme.sp(10)
                                height: Theme.sp(18)
                                radius: Theme.sp(9)
                                color: active
                                    ? Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b, statsChipHover.hovered ? 0.22 : 0.15)
                                    : statsChipHover.hovered
                                      ? Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b, 0.08)
                                      : "transparent"
                                border.width: 1
                                border.color: active
                                    ? Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b, statsChipHover.hovered ? 0.55 : 0.4)
                                    : statsChipHover.hovered
                                      ? Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b, 0.4)
                                      : Theme.colorBorderMid
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                                Text {
                                    id: statsChipLbl
                                    anchors.centerIn: parent
                                    text: modelData.name
                                    font.family: Theme.fontData
                                    font.pixelSize: Theme.sp(9)
                                    font.letterSpacing: Theme.trackingMicro
                                    color: parent.active ? Theme.colorAccent : Theme.colorText3
                                }

                                TapHandler  { onTapped: profiler.toggleStatsCategory(modelData.name) }
                                HoverHandler { id: statsChipHover; cursorShape: Qt.PointingHandCursor }
                            }
                        }
                    }

                    // Text filter
                    Rectangle {
                        anchors { right: statsChips.left; rightMargin: Theme.sp(8); verticalCenter: parent.verticalCenter }
                        width: Theme.sp(120)
                        height: Theme.sp(18)
                        radius: Theme.radius
                        color: Theme.colorBg2
                        border.width: 1
                        border.color: statsFilterInput.activeFocus ? Theme.colorAccent : Theme.colorBorderMid

                        Text {
                            anchors { left: parent.left; leftMargin: Theme.sp(7); verticalCenter: parent.verticalCenter }
                            visible: statsFilterInput.text.length === 0
                            text: qsTr("Filter…")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                        }

                        TextInput {
                            id: statsFilterInput
                            anchors { left: parent.left; right: statsClearFilter.left; leftMargin: Theme.sp(7); rightMargin: Theme.sp(4); verticalCenter: parent.verticalCenter }
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText
                            selectionColor: Theme.colorAccentMid
                            selectedTextColor: Theme.colorText
                            clip: true
                            onTextChanged: profiler.setStatsTextFilter(text)
                        }

                        Text {
                            id: statsClearFilter
                            anchors { right: parent.right; rightMargin: Theme.sp(5); verticalCenter: parent.verticalCenter }
                            visible: statsFilterInput.text.length > 0
                            text: "×"
                            font.pixelSize: Theme.sp(12)
                            color: Theme.colorText3
                            TapHandler   { onTapped: { statsFilterInput.text = ""; statsFilterInput.forceActiveFocus() } }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                        }
                    }
                }

                // Column header
                Rectangle {
                    width: parent.width
                    height: Theme.sp(24)
                    color: Theme.colorBg2
                    radius: Theme.radiusLg
                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: Theme.radiusLg; color: Theme.colorBg2
                    }
                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: 1; color: Theme.colorBorderMid
                    }

                    Row {
                        anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }

                        Item {
                            width: Theme.sp(52); height: parent.height
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("TIME")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                        }
                        Item {
                            width: Theme.sp(60); height: parent.height
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("CATEGORY")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("MESSAGE")
                            font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                            font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                        }
                    }
                }

                // Empty state
                Rectangle {
                    visible: profiler.statsHistory.length === 0
                    width: parent.width
                    height: Theme.sp(40)
                    color: Theme.colorSurface
                    radius: Theme.radius
                    border.width: 1
                    border.color: Theme.colorBorderMid
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("No stats recorded — dumped every 60 s and at session end")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color: Theme.colorText3
                    }
                }

                // Rows — newest first
                Repeater {
                    model: profiler.statsHistory
                    RmStatRow {
                        statData: modelData
                        isAlternate: index % 2 === 1
                        width: parent.width
                    }
                }
            }

            // ── Analysis runs (per-analysis drill-down) ───────────────────────
            Column {
                id: analysisRunsCol
                visible: root.logTab === 2 && profiler.available
                width: parent.width - 40
                spacing: 0

                // Header: hint + Clear/Export
                Item {
                    width: parent.width
                    height: Theme.sp(32)

                    Text {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        text: qsTr("tap a run to expand its per-stage breakdown")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.sp(10)
                        color: Theme.colorText3
                    }

                    // Clear
                    Rectangle {
                        id: anClearBtn
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        width: anClearLbl.implicitWidth + Theme.sp(16)
                        height: Theme.sp(18)
                        radius: Theme.radius
                        color: anClearHover.hovered
                               ? Theme.colorBg2
                               : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                        border.width: 1
                        border.color: anClearHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        scale: anClearTap.pressed ? 0.97 : anClearHover.hovered ? 1.02 : 1.0
                        Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }
                        Text {
                            id: anClearLbl
                            anchors.centerIn: parent
                            text: qsTr("Clear")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                        }
                        TapHandler   { id: anClearTap; onTapped: profiler.clearAnalysisRuns() }
                        HoverHandler { id: anClearHover; cursorShape: Qt.PointingHandCursor }
                    }

                    // Export
                    Rectangle {
                        id: anExportBtn
                        anchors { right: anClearBtn.left; rightMargin: Theme.sp(8); verticalCenter: parent.verticalCenter }
                        width: anExportLbl.implicitWidth + Theme.sp(16)
                        height: Theme.sp(18)
                        radius: Theme.radius
                        color: anExportHover.hovered
                               ? Theme.colorBg2
                               : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                        border.width: 1
                        border.color: anExportHover.hovered ? Theme.colorAccentMid : Theme.colorBorderMid
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        scale: anExportTap.pressed ? 0.97 : anExportHover.hovered ? 1.02 : 1.0
                        Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }
                        Text {
                            id: anExportLbl
                            anchors.centerIn: parent
                            text: qsTr("Export")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.sp(10)
                            color: Theme.colorText3
                        }
                        TapHandler {
                            id: anExportTap
                            onTapped: {
                                var path = profiler.exportAnalysisRuns()
                                exportToast.copyText = path
                                if (path.length > 0) exportToast.show(qsTr("Analysis runs exported to %1").arg(path))
                                else                 exportToast.show(qsTr("Analysis export failed"))
                            }
                        }
                        HoverHandler { id: anExportHover; cursorShape: Qt.PointingHandCursor }
                    }
                }

                // Column header
                Rectangle {
                    width: parent.width
                    height: Theme.sp(24)
                    color: Theme.colorBg2
                    radius: Theme.radiusLg
                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: Theme.radiusLg; color: Theme.colorBg2
                    }
                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: 1; color: Theme.colorBorderMid
                    }

                    Row {
                        anchors { fill: parent; leftMargin: Theme.sp(24); rightMargin: Theme.sp(10) }
                        spacing: Theme.sp(8)

                        Item {
                            width: Theme.sp(52); height: parent.height
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("TIME")
                                font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("SESSION")
                            font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                            font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                        }
                    }
                    Text {
                        anchors { right: parent.right; rightMargin: Theme.sp(10); verticalCenter: parent.verticalCenter }
                        text: qsTr("FRAMES · TOTAL · SCORE")
                        font.family: Theme.fontData; font.pixelSize: Theme.sp(9)
                        font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                    }
                }

                // Empty state
                Rectangle {
                    visible: profiler.analysisRuns.length === 0
                    width: parent.width
                    height: Theme.sp(40)
                    color: Theme.colorSurface
                    radius: Theme.radius
                    border.width: 1
                    border.color: Theme.colorBorderMid
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("No analyses yet — run a shot or re-analyse a swing")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color: Theme.colorText3
                    }
                }

                // Rows — newest first, tap to expand the per-stage breakdown
                Repeater {
                    model: profiler.analysisRuns
                    RmAnalysisRunRow {
                        runData: modelData
                        isAlternate: index % 2 === 1
                        width: parent.width
                    }
                }
            }

            // ── PinPointCapture (CR-02 goal (a)) ──────────────────────────
            //
            // Fed by ScreenResourceMonitor's own `refreshPpcp()`, which merges
            // `ppcpHost.ppcpStats()` — a large already-built dataset whose only
            // consumer until now was the automated harness — with the per-phone
            // readings `ppcpHost.phones` publishes.
            Column {
                id: ppcpCol
                visible: root.logTab === 3 && root.ppcpAvailable
                width: parent.width - 40
                spacing: Theme.sp(10)

                // Header: the listener, and the arbitration counters summed
                // across every phone.
                Item {
                    width: parent.width
                    height: Theme.sp(32)

                    Text {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        text: {
                            var st = root.ppcpStatsData
                            if (!st || st.listening === undefined) return qsTr("not listening")
                            return st.listening
                                 ? qsTr("listening on port %1 · %2 phone(s)").arg(st.port).arg(st.phones)
                                 : qsTr("not listening")
                        }
                        font.family: Theme.fontData
                        font.pixelSize: Theme.sp(10)
                        color: Theme.colorText3
                    }

                    Text {
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        text: {
                            var st = root.ppcpStatsData
                            if (!st || st.nominated === undefined) return ""
                            // ⚠ `unarbitrated` is the one that says a device
                            // nominated a shot and nobody was listening.
                            return qsTr("shots  nominated %1 · issued %2 · adopted %3 · excluded %4 · unarbitrated %5")
                                   .arg(st.nominated).arg(st.issued).arg(st.adopted)
                                   .arg(st.excluded).arg(st.unarbitrated)
                        }
                        font.family: Theme.fontData
                        font.pixelSize: Theme.sp(9)
                        color: (root.ppcpStatsData && root.ppcpStatsData.unarbitrated > 0)
                               ? Theme.colorWarn : Theme.colorText3
                    }
                }

                // Empty state — a build WITH ppcp that has never been paired.
                Rectangle {
                    visible: root.ppcpRows.length === 0
                    width: parent.width
                    height: Theme.sp(40)
                    color: Theme.colorSurface
                    radius: Theme.radius
                    border.width: 1
                    border.color: Theme.colorBorderMid
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("No phones — pair one from Settings → Phones")
                        font.family: Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color: Theme.colorText3
                    }
                }

                // ⚠ MODELLED ON THE COUNT, NOT THE ARRAY.  `ppcpRows` is rebuilt
                // every 500 ms; a Repeater bound to the array itself would
                // destroy and recreate every card twice a second.  Bound to the
                // length, the cards stand and only their readings move — the
                // same discipline `phoneHealthChanged()` exists to enforce on
                // the C++ side.
                Repeater {
                    model: root.ppcpRows.length
                    RmPpcpPhoneCard {
                        width: parent.width
                        rowData: root.ppcpRows[index]
                    }
                }

                // What the import ledger owes — MSG 9.1, already counted.
                Item {
                    width: parent.width
                    height: Theme.sp(24)
                    visible: root.ppcpStatsData && root.ppcpStatsData.importCaptures !== undefined

                    Text {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        text: {
                            var st = root.ppcpStatsData
                            if (!st || st.importCaptures === undefined) return ""
                            return qsTr("imported  %1 capture(s) over %2 session(s) · %3 commit(s) owed")
                                   .arg(st.importCaptures).arg(st.importSessions).arg(st.commitsOwed)
                        }
                        font.family: Theme.fontData
                        font.pixelSize: Theme.sp(9)
                        color: Theme.colorText3
                    }
                }
            }

            Item { height: Theme.sp(16) }
        }
    }

    // Informational toast for log export — no UNDO action.
    PpToast {
        id: exportToast
        showUndo: false
        glyph: "💾"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.sp(24)
    }
}
