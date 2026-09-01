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
import PinPointStudio

// One PinPointCapture phone on the resource monitor's PINPOINTCAPTURE tab: the
// link's readings across the top, then one row per camera it offers.
//
// `rowData` is the merge ScreenResourceMonitor builds every refresh —
// `{ phone: <a ppcpHost.phones row>, stat: <ppcpStats().perPhone entry|null>,
//    cameras: [<one entry per source_id>] }`.  A remembered-but-absent phone
// has a `phone` and no `stat`, which is why every read below is guarded and
// every missing reading renders "—".
//
// ⚠ -1 AND "" ARE READINGS, NOT STUBS.  `batteryPct: -1` and `thermal: ""` mean
// "no `heartbeat_ack` has arrived yet", which is a different answer from 0% and
// from "nominal".  Rendering the sentinel itself would put a -1 in front of an
// operator, so every formatter below maps it to an em dash.
Rectangle {
    id: root

    property var rowData

    readonly property var phone:   rowData && rowData.phone ? rowData.phone : null
    readonly property var stat:    rowData && rowData.stat  ? rowData.stat  : null
    readonly property var cameras: rowData && rowData.cameras ? rowData.cameras : []
    // MSG 5.6 — one entry per Stream that has reported a margin.  Empty until
    // one does, which is the normal state of a device that has not evicted
    // anything: 5.6c is push-on-change, not a poll.
    readonly property var buffers: stat && stat.bufferStatus ? stat.bufferStatus : []
    readonly property bool live:   stat !== null

    radius: Theme.radiusLg
    border.width: 1
    border.color: Theme.colorBorderMid
    color: Theme.colorSurface
    clip: true
    visible: phone !== null
    implicitHeight: header.height + body.implicitHeight + Theme.sp(20)

    // ── Formatters — every one of them maps a sentinel to an em dash ────────
    function fmtBattery() {
        if (!phone || phone.batteryPct === undefined || phone.batteryPct < 0) return "—"
        var s = phone.batteryPct + "%"
        if (phone.charging === 1) s += " ⚡"
        return s
    }
    function fmtThermal() {
        if (!phone || !phone.thermal) return "—"
        return phone.thermal
    }
    function fmtSigma() {
        if (!phone || phone.syncSigmaMs === undefined || phone.syncSigmaMs < 0) return "—"
        return phone.syncSigmaMs.toFixed(2) + " ms"
    }
    function fmtStorage() {
        if (!phone || phone.storageFreeBytes === undefined || phone.storageFreeBytes < 0) return "—"
        var gb = phone.storageFreeBytes / (1024 * 1024 * 1024)
        return gb >= 1 ? gb.toFixed(1) + " GB"
                       : (phone.storageFreeBytes / (1024 * 1024)).toFixed(0) + " MB"
    }
    function fmtArm() {
        if (!phone || !phone.armState) return "—"
        if (phone.armState === "blocked" && phone.armBlockedReason)
            return phone.armState + " · " + phone.armBlockedReason
        if (phone.armState === "arming" && phone.armReadyMs >= 0)
            return phone.armState + " · " + phone.armReadyMs + " ms"
        return phone.armState
    }
    function fmtMs(ms) {
        if (ms === undefined || ms < 0) return "—"
        if (ms < 1000) return ms + " ms"
        return (ms / 1000).toFixed(ms < 10000 ? 1 : 0) + " s"
    }
    function fmtSessionFor() {
        if (!live || stat.sessionForMs === undefined || stat.sessionForMs < 0) return "—"
        var secs = Math.floor(stat.sessionForMs / 1000)
        var h = Math.floor(secs / 3600)
        var m = Math.floor((secs % 3600) / 60)
        var s = secs % 60
        function pad(n) { return n < 10 ? "0" + n : String(n) }
        return h > 0 ? h + ":" + pad(m) + ":" + pad(s) : m + ":" + pad(s)
    }
    function fmtTransport() {
        if (!phone || !phone.transport) return "—"
        return phone.transport === "cable" ? qsTr("cable") : qsTr("wifi")
    }
    function thermalTint() {
        if (!phone || !phone.thermal) return Theme.colorText3
        if (phone.thermal === "nominal") return Theme.colorGood
        if (phone.thermal === "fair")    return Theme.colorText2
        return Theme.colorWarn
    }
    function batteryTint() {
        if (!phone || phone.batteryPct === undefined || phone.batteryPct < 0) return Theme.colorText3
        if (phone.batteryPct <= 15) return Theme.colorError
        if (phone.batteryPct <= 30) return Theme.colorWarn
        return Theme.colorText2
    }

    // One reading, label above value.
    component Reading: Column {
        id: rd
        property string label: ""
        property string value: "—"
        property color  tint: Theme.colorText2
        width: Theme.sp(96)
        spacing: 2
        Text {
            text: rd.label
            font.family: Theme.fontData; font.pixelSize: Theme.sp(8)
            font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
        }
        Text {
            text: rd.value
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
            color: rd.tint
            elide: Text.ElideRight
            width: parent ? parent.width : 0
        }
    }

    // ── Header strip ────────────────────────────────────────────────────────
    Rectangle {
        id: header
        height: Theme.sp(40)
        anchors { top: parent.top; left: parent.left; right: parent.right }
        color: Theme.colorBg2
        radius: Theme.radiusLg

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
            anchors { fill: parent; leftMargin: Theme.sp(12); rightMargin: Theme.sp(10) }
            spacing: Theme.sp(8)

            Rectangle {
                width: Theme.sp(7)
                height: Theme.sp(7)
                radius: Theme.sp(4)
                anchors.verticalCenter: parent.verticalCenter
                color: {
                    if (!root.phone) return Theme.colorBorderStrong
                    if (root.phone.status === "connected") return Theme.colorGood
                    if (root.phone.status === "revoked")   return Theme.colorError
                    if (root.phone.status === "available") return Theme.colorWarn
                    return Theme.colorBorderStrong
                }
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                width: parent.width - Theme.sp(7) - Theme.sp(16) - statusPill.implicitWidth

                Text {
                    text: root.phone ? root.phone.name : ""
                    font.family: Theme.fontBody
                    font.pixelSize: Theme.fontSzBody
                    color: Theme.colorText
                    elide: Text.ElideRight
                    width: parent.width
                }
                Text {
                    // The phone's own declared name under the alias, then the
                    // peer id the camera rows hang off.
                    text: {
                        if (!root.phone) return ""
                        var parts = []
                        if (root.phone.declaredName && root.phone.declaredName !== root.phone.name)
                            parts.push(root.phone.declaredName)
                        if (root.phone.model && root.phone.model !== root.phone.declaredName)
                            parts.push(root.phone.model)
                        if (root.phone.counterpartId) parts.push(root.phone.counterpartId)
                        else parts.push(root.phone.pairingId)
                        return parts.join(" · ")
                    }
                    font.family: Theme.fontData
                    font.pixelSize: Theme.sp(9)
                    color: Theme.colorText3
                    elide: Text.ElideRight
                    width: parent.width
                }
            }

            Item {
                id: statusPill
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: pillLbl.implicitWidth + Theme.sp(14)
                implicitHeight: pillLbl.implicitHeight + Theme.sp(6)

                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: root.phone && root.phone.status === "connected" ? Theme.colorGoodLight
                         : root.phone && root.phone.status === "available" ? Theme.colorWarnLight
                                                                           : Theme.colorBg3
                    border.width: 1
                    border.color: root.phone && root.phone.status === "connected" ? Theme.colorGood
                                : root.phone && root.phone.status === "available" ? Theme.colorWarn
                                                                                  : Theme.colorBorderMid
                }
                Text {
                    id: pillLbl
                    anchors.centerIn: parent
                    text: root.phone ? root.phone.status : ""
                    font.family: Theme.fontData
                    font.pixelSize: Theme.sp(9)
                    font.letterSpacing: Theme.trackingMicro
                    color: root.phone && root.phone.status === "connected" ? Theme.colorGood
                         : root.phone && root.phone.status === "available" ? Theme.colorWarn
                                                                           : Theme.colorText3
                }
            }
        }
    }

    // ── Body ────────────────────────────────────────────────────────────────
    Column {
        id: body
        anchors { top: header.bottom; left: parent.left; right: parent.right
                  topMargin: Theme.sp(10); leftMargin: Theme.sp(12); rightMargin: Theme.sp(12) }
        spacing: Theme.sp(10)

        // The link's readings.
        Flow {
            width: parent.width
            spacing: Theme.sp(10)

            Reading { label: qsTr("BATTERY");  value: root.fmtBattery();  tint: root.batteryTint() }
            Reading { label: qsTr("THERMAL");  value: root.fmtThermal();  tint: root.thermalTint() }
            Reading { label: qsTr("STORAGE");  value: root.fmtStorage() }
            Reading { label: qsTr("TRANSPORT"); value: root.fmtTransport() }
            Reading {
                label: qsTr("SYNC σ")
                value: root.fmtSigma()
                tint: root.phone && root.phone.syncSigmaMs >= 0 && root.phone.syncSigmaMs > 2.0
                      ? Theme.colorWarn : Theme.colorText2
            }
            Reading {
                label: qsTr("ARM")
                value: root.fmtArm()
                width: Theme.sp(140)
                tint: root.phone && root.phone.armState === "armed"   ? Theme.colorGood
                    : root.phone && (root.phone.armState === "blocked"
                                  || root.phone.armState === "stalled") ? Theme.colorWarn
                                                                        : Theme.colorText2
            }
            Reading {
                label: qsTr("SESSION")
                value: !root.live ? "—" : (root.stat.sessionOpen ? qsTr("open") : qsTr("closed"))
                tint: root.live && root.stat.sessionOpen ? Theme.colorGood : Theme.colorText2
            }
            // ⭐ CORE 5.10h / erratum E61 — how long this Session has been
            // open, from `Session.opened_at`.
            //
            // ⚠ COMPUTABLE ONLY BECAUSE WE ARE THE HOST.  `opened_at` has no
            // wire carrier (plan §10 item 3), so this host knows it by having
            // set it, and both ends of the subtraction are readings of
            // `tb:host` — one clock, no relation, no mixed epoch.  Before the
            // field existed a consumer had to fabricate a start time from the
            // first message it happened to see, which is precisely what 5.10h
            // exists to prevent.
            Reading { label: qsTr("SESSION FOR"); value: root.fmtSessionFor() }
            Reading {
                label: qsTr("ARBITER")
                value: !root.live ? "—" : (root.stat.arbiter ? qsTr("yes") : qsTr("no"))
                tint: root.live && root.stat.arbiter ? Theme.colorAccent : Theme.colorText2
            }
            Reading { label: qsTr("CHANNELS");  value: root.live ? String(root.stat.channels) : "—" }
            Reading { label: qsTr("SHOTS HELD"); value: root.live ? String(root.stat.retained) : "—" }
            Reading { label: qsTr("SHOT GROUPS"); value: root.live ? String(root.stat.groups) : "—" }
        }

        // ── MSG 5.6 / CORE 5.21 — the ring buffer's standing margin ─────────
        //
        // One strip per `shot_windowed` Stream that has reported (5.21c
        // confines the message to those).  Nothing at all until one arrives:
        // 5.6c is push-on-change like `readiness`, so silence is "this device
        // has not spoken about its buffer", not "the buffer is fine".
        //
        // ⛔ NO RETAINED WINDOW IS COMPUTED HERE.  `retained_from` is stamped in
        // the DEVICE's timebase and this host's clock shares no epoch with it;
        // subtracting one from the other is the exact defect found live on
        // 27 August, where a mixed-domain elapsed turned a real 17 ms sigma into
        // a fabricated 460 ms one.  The wire carries no device "now", so what is
        // shown is what was actually said: the target, the discard count and the
        // last discard's span.
        //
        // ⚠ AND `discarded_since_open` IS PER STREAM OPEN, not per arm (trap 7)
        // — the label says so, because the two epochs look identical in a
        // number and only one of them resets when an operator disarms.
        Rectangle {
            width: parent.width
            height: Theme.sp(34)
            radius: Theme.radius
            color: Theme.colorBg
            border.width: 1
            border.color: Theme.colorBorderMid
            visible: root.buffers.length === 0

            Text {
                anchors { left: parent.left; leftMargin: Theme.sp(10); verticalCenter: parent.verticalCenter }
                text: qsTr("RING BUFFER")
                font.family: Theme.fontData
                font.pixelSize: Theme.sp(8)
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }
            Text {
                anchors { right: parent.right; rightMargin: Theme.sp(10); verticalCenter: parent.verticalCenter }
                text: root.live ? qsTr("no buffer_status reported yet")
                                : qsTr("not connected")
                font.family: Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color: Theme.colorText3
                elide: Text.ElideRight
            }
        }

        // ⚠ MODELLED ON THE COUNT for the same reason the camera rows are: the
        // array behind it is rebuilt every 500 ms refresh.
        Column {
            width: parent.width
            spacing: Theme.sp(4)
            Repeater {
                model: root.buffers.length
                Rectangle {
                    required property int index
                    readonly property var buf: root.buffers[index]
                    width: parent.width
                    height: Theme.sp(34)
                    radius: Theme.radius
                    color: Theme.colorBg
                    border.width: 1
                    border.color: Theme.colorBorderMid

                    Column {
                        anchors { left: parent.left; leftMargin: Theme.sp(10)
                                  verticalCenter: parent.verticalCenter }
                        spacing: 1
                        Text {
                            text: qsTr("RING BUFFER")
                            font.family: Theme.fontData
                            font.pixelSize: Theme.sp(8)
                            font.letterSpacing: Theme.trackingMicro
                            color: Theme.colorText3
                        }
                        Text {
                            text: buf ? buf.streamId : ""
                            font.family: Theme.fontData
                            font.pixelSize: Theme.sp(9)
                            color: Theme.colorText3
                        }
                    }

                    Row {
                        anchors { right: parent.right; rightMargin: Theme.sp(10)
                                  verticalCenter: parent.verticalCenter }
                        spacing: Theme.sp(10)

                        Reading {
                            label: qsTr("TARGET")
                            width: Theme.sp(70)
                            value: !buf || buf.retentionTargetMs < 0 ? "—"
                                 : root.fmtMs(buf.retentionTargetMs)
                        }
                        Reading {
                            label: qsTr("DISCARDED / OPEN")
                            width: Theme.sp(110)
                            value: buf ? String(buf.discardedSinceOpen) : "—"
                            tint: buf && buf.discardedSinceOpen > 0 ? Theme.colorWarn
                                                                    : Theme.colorText2
                        }
                        Reading {
                            label: qsTr("LAST DISCARD")
                            width: Theme.sp(90)
                            value: !buf || buf.lastDiscardDurationMs < 0 ? "—"
                                 : root.fmtMs(buf.lastDiscardDurationMs)
                            tint: buf && buf.lastDiscardDurationMs >= 0 ? Theme.colorWarn
                                                                        : Theme.colorText2
                        }
                    }
                }
            }
        }

        // ── Cameras ─────────────────────────────────────────────────────────
        Item {
            width: parent.width
            height: Theme.sp(20)

            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                text: qsTr("CAMERAS (%1)").arg(root.cameras.length)
                font.family: Theme.fontData
                font.pixelSize: Theme.sp(9)
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }
        }

        Rectangle {
            width: parent.width
            height: Theme.sp(30)
            visible: root.cameras.length === 0
            radius: Theme.radius
            color: Theme.colorBg
            border.width: 1
            border.color: Theme.colorBorderMid
            Text {
                anchors.centerIn: parent
                text: root.live ? qsTr("No camera Sources attached")
                                : qsTr("Not connected — no Sources to report")
                font.family: Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color: Theme.colorText3
            }
        }

        // ⚠ MODELLED ON THE COUNT, NOT THE ARRAY.  The array is rebuilt every
        // 500 ms refresh; binding the Repeater to it would destroy and recreate
        // every row twice a second.  Bound to `length`, the delegates survive
        // and only their readings change — the same discipline the phone rows
        // in Settings learned when a heartbeat rebuild ate an operator's typing.
        Column {
            width: parent.width
            spacing: 0
            Repeater {
                model: root.cameras.length
                RmPpcpCameraRow {
                    width: parent.width
                    camData: root.cameras[index]
                    isAlternate: index % 2 === 1
                }
            }
        }
    }
}
