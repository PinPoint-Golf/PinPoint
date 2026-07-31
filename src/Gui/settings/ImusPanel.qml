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

Item {
    id: root

    // Id of the IMU device whose test panel is currently open ("" = none).
    property string openTestId: ""

    // ─────────────────────────────────────────────────────────────────────────
    // Inline component — reusable toggle pill (copied verbatim from CamerasPanel)
    // ─────────────────────────────────────────────────────────────────────────
    component TogglePill: Rectangle {
        id: pill
        property bool checked: false
        signal toggled(bool value)

        width:  Theme.sp(34)
        height: Theme.sp(18)
        radius: Theme.sp(9)
        color:  pill.checked ? Theme.colorAccent : Theme.colorBg3
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Rectangle {
            width:  Theme.sp(12)
            height: Theme.sp(12)
            radius: Theme.sp(6)
            color:  "white"
            anchors.verticalCenter: parent.verticalCenter
            x: pill.checked ? parent.width - width - Theme.sp(3) : Theme.sp(3)
            Behavior on x { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape:  Qt.PointingHandCursor
            onClicked:    pill.toggled(!pill.checked)
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Inline component — IMU device row
    // ─────────────────────────────────────────────────────────────────────────
    component ImuDeviceRow: Item {
        id: imuRow

        property var imuData: ({})

        // Live ImuInstance for this device; null when not selected/connected.
        // Rebinds whenever instances change (selection or connection-state change).
        readonly property QtObject inst: {
            // Read as a VALUE, not as a bare statement. `imuManager.instances` on a line of its own
            // is dead code: the compiler drops it and takes the dependency with it, so this would
            // resolve once and never rebind — exactly the bug that froze the diagnostic model's
            // inspector (see the note on `_revision` in DiagnosticModel.qml).
            if (imuManager.instances.length < 0) return null
            return imuManager.instanceFor(imuData.id)
        }

        readonly property bool isConnected: inst !== null && inst.imuConnected
        readonly property bool isExcluded:  appSettings.imuExcluded.indexOf(imuData.id) >= 0
        readonly property bool testOpen:    root.openTestId === imuData.id

        // Capabilities strip cell model — reactive to live battery data.
        readonly property var capsCells: {
            var sensors = []
            if (imuData.hasAccelerometer) sensors.push(qsTr("Accel"))
            if (imuData.hasGyroscope)     sensors.push(qsTr("Gyro"))
            if (imuData.hasMagnetometer)  sensors.push(qsTr("Mag"))
            var rates = imuData.supportedRatesHz || []
            var maxRate = rates.length > 0 ? Math.max.apply(null, rates) : 0
            var batPct = (imuRow.isConnected && imuRow.inst && imuRow.inst.batteryPercent >= 0)
                             ? imuRow.inst.batteryPercent : -1
            return [
                { key: qsTr("Transport"),   val: imuData.transport || "—",    isAccent: false, batPct: -1 },
                { key: qsTr("Sensors"),     val: sensors.length > 0 ? sensors.join(" · ") : "—",
                                                                               isAccent: false, batPct: -1 },
                { key: qsTr("Accel range"), val: (imuData.accelRangeMax > 0)
                                                    ? ("±" + imuData.accelRangeMax.toFixed(0) + " g") : "—",
                                                                               isAccent: false, batPct: -1 },
                { key: qsTr("Gyro range"),  val: (imuData.gyroRangeMax > 0)
                                                    ? ("±" + imuData.gyroRangeMax.toFixed(0) + " °/s") : "—",
                                                                               isAccent: false, batPct: -1 },
                { key: qsTr("Max rate"),    val: maxRate > 0 ? (maxRate + " Hz") : "—",
                                                                               isAccent: true,  batPct: -1 },
                { key: qsTr("Battery"),     val: batPct >= 0 ? (batPct + "%") : "—",
                                                                               isAccent: false, batPct: batPct }
            ]
        }

        implicitHeight: testOpen && !isExcluded
            ? headerRow.height + bodyWrap.height + testPanel.height
            : headerRow.height + (isExcluded ? excludedNote.height : bodyWrap.height)

        Behavior on implicitHeight { NumberAnimation { duration: Theme.durationFast } }

        opacity: isExcluded ? 0.5 : 1.0
        Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

        // Background fill
        Rectangle {
            anchors.fill: parent
            color:        Theme.colorSurface
            radius:       Theme.radius
        }

        clip: true

        // Border overlay — z:100 so content never occludes it
        Rectangle {
            anchors.fill: parent
            color:        "transparent"
            border.width: 1
            border.color: imuRow.testOpen    ? Theme.colorAccent
                        : imuRow.isConnected ? Theme.colorGood
                        : imuRow.isExcluded  ? Theme.colorBorderMid
                        :                      Theme.colorBorderStrong
            radius: Theme.radius
            z:      100
            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
        }

        // ── Header row ───────────────────────────────────────────────────────
        RowLayout {
            id: headerRow
            anchors.top:     parent.top
            anchors.left:    parent.left
            anchors.right:   parent.right
            anchors.margins: Theme.sp(14)
            height:   Theme.sp(54)
            spacing:  Theme.sp(10)

            // Status dot
            Rectangle {
                width:  Theme.sp(6)
                height: Theme.sp(6)
                radius: Theme.sp(3)
                color: imuRow.isConnected ? Theme.colorGood
                     : !imuRow.isExcluded  ? Theme.colorWarn
                     :                       Theme.colorText3
                Layout.alignment: Qt.AlignVCenter
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
            }

            // Alias (editable) + meta
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(2)

                PpTextField {
                    Layout.fillWidth: true
                    placeholderText:  qsTr("Device alias…")
                    text:             imuData.alias
                    onEditingFinished: imuManager.setImuAlias(imuData.imuKey, text)
                }

                Row {
                    spacing: Theme.sp(10)
                    Text { text: imuData.description || "";    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro; color: Theme.colorText3 }
                    Text { text: imuData.transport || "";      font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro; color: Theme.colorText3 }
                    Text { text: imuData.id || "";             font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro; color: Theme.colorText3 }
                    Text { text: imuData.firmwareVersion || ""; font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro; color: Theme.colorText3 }
                }
            }

            // Enable toggle
            Row {
                spacing: Theme.sp(6)
                Layout.alignment: Qt.AlignVCenter

                Text {
                    text:           qsTr("Enable")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    anchors.verticalCenter: parent.verticalCenter
                }

                TogglePill {
                    checked: !imuRow.isExcluded
                    onToggled: (v) => {
                        var list = appSettings.imuExcluded
                        var idx  = list.indexOf(imuData.id)
                        if (!v && idx < 0)  list.push(imuData.id)
                        if ( v && idx >= 0) list.splice(idx, 1)
                        appSettings.imuExcluded = list
                        if (!v && imuRow.testOpen) root.openTestId = ""
                    }
                }
            }
        }

        // ── Excluded note ────────────────────────────────────────────────────
        Item {
            id: excludedNote
            anchors.top:   headerRow.bottom
            anchors.left:  parent.left
            anchors.right: parent.right
            height:  imuRow.isExcluded ? (excText.implicitHeight + Theme.sp(18)) : 0
            visible: imuRow.isExcluded
            clip:    true

            Text {
                id: excText
                anchors {
                    left:        parent.left
                    right:       parent.right
                    top:         parent.top
                    leftMargin:  Theme.sp(14)
                    rightMargin: Theme.sp(14)
                    topMargin:   Theme.sp(4)
                }
                text:        qsTr("Excluded — will not be connected at session start.")
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                font.italic: true
                color:       Theme.colorText3
                wrapMode:    Text.WordWrap
            }
        }

        // ── Body wrap — chips + capabilities strip ───────────────────────────
        Item {
            id: bodyWrap
            anchors.top:   headerRow.bottom
            anchors.left:  parent.left
            anchors.right: parent.right
            height:  !imuRow.isExcluded ? (bodyCol.implicitHeight + Theme.sp(20)) : 0
            visible: !imuRow.isExcluded
            clip:    true

            ColumnLayout {
                id: bodyCol
                anchors.left:  parent.left
                anchors.right: parent.right
                anchors.top:   parent.top
                spacing: 0

                // Config chips row
                RowLayout {
                    Layout.fillWidth:    true
                    Layout.leftMargin:   Theme.sp(14)
                    Layout.rightMargin:  Theme.sp(14)
                    Layout.topMargin:    Theme.sp(4)
                    Layout.bottomMargin: Theme.sp(12)
                    spacing: Theme.sp(16)

                    // ── Placement selector ───────────────────────────────────
                    ColumnLayout {
                        spacing: Theme.sp(4)
                        Layout.alignment: Qt.AlignTop

                        Text {
                            text:                qsTr("PLACEMENT")
                            font.family:         Theme.fontData
                            font.pixelSize:      Theme.fontSzMicro
                            font.letterSpacing:  Theme.trackingMicro
                            font.capitalization: Font.AllUppercase
                            color:               Theme.colorText3
                        }

                        PpComboBox {
                            id: placementCombo
                            implicitWidth: Theme.sp(200)

                            readonly property var placementOptions: [
                                { label: qsTr("— Unassigned —"),                  value: ""      },
                                { label: qsTr("A — Thorax / Lead Wrist"),         value: "A"     },
                                { label: qsTr("B — Lumbar Spine / Lead Hand"),    value: "B"     },
                                { label: qsTr("C — T12 Junction / Shoulder"),     value: "C"     },
                                { label: qsTr("D — Other"),                       value: "D"     }
                            ]

                            model: placementOptions.map(function(o) { return o.label })

                            function placementTakenBy(value) {
                                if (!value || value === "" || value === "other") return false
                                var map     = appSettings.imuPlacement
                                var devList = imuManager.imuDeviceList
                                for (var i = 0; i < devList.length; ++i) {
                                    var d = devList[i]
                                    if (d.id !== imuData.id
                                            && appSettings.imuExcluded.indexOf(d.id) < 0
                                            && map[d.id] === value)
                                        return true
                                }
                                return false
                            }

                            // Disable placements already assigned to another connected IMU.
                            itemEnabledFn: function(index) {
                                return !placementCombo.placementTakenBy(
                                            placementCombo.placementOptions[index].value)
                            }

                            Component.onCompleted: {
                                var saved = appSettings.imuPlacement[imuData.id] || ""
                                for (var i = 0; i < placementOptions.length; i++) {
                                    if (placementOptions[i].value === saved) { currentIndex = i; break }
                                }
                            }

                            Connections {
                                target: appSettings
                                function onImuPlacementChanged() {
                                    var saved = appSettings.imuPlacement[imuData.id] || ""
                                    for (var i = 0; i < placementCombo.placementOptions.length; i++) {
                                        if (placementCombo.placementOptions[i].value === saved) {
                                            placementCombo.currentIndex = i; break
                                        }
                                    }
                                }
                            }

                            onActivated: (idx) => {
                                var map = appSettings.imuPlacement
                                map[imuData.id] = placementOptions[idx].value
                                appSettings.imuPlacement = map
                            }
                        }
                    }

                    // ── Output rate chips ────────────────────────────────────
                    ColumnLayout {
                        spacing: Theme.sp(4)
                        Layout.alignment: Qt.AlignTop

                        Text {
                            text:                qsTr("OUTPUT RATE")
                            font.family:         Theme.fontData
                            font.pixelSize:      Theme.fontSzMicro
                            font.letterSpacing:  Theme.trackingMicro
                            font.capitalization: Font.AllUppercase
                            color:               Theme.colorText3
                        }

                        Row {
                            id: rateChipRow
                            spacing: Theme.sp(4)

                            readonly property var rateOptions: {
                                var rates = imuData.supportedRatesHz
                                return (rates && rates.length > 0) ? rates : [50, 100, 200, 500]
                            }

                            readonly property int selectedRate: {
                                var v = appSettings.imuOutputRateHz[imuData.id]
                                return (v !== undefined && v > 0) ? v : (imuData.defaultRateHz || 100)
                            }

                            Repeater {
                                model: rateChipRow.rateOptions

                                delegate: Rectangle {
                                    id: rateChip
                                    required property var modelData

                                    readonly property bool isSelected: rateChipRow.selectedRate === modelData

                                    width:  rateChipLabel.implicitWidth + Theme.sp(20)
                                    height: Theme.sp(24)
                                    radius: Theme.radius
                                    color:  isSelected ? Theme.colorAccentLight
                                          : rateArea.containsMouse
                                              ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                              : "transparent"
                                    border.width: 1
                                    border.color: isSelected ? Theme.colorAccent
                                                : rateArea.containsMouse ? Theme.colorAccentMid
                                                : Theme.colorBorderStrong
                                    Behavior on color       { ColorAnimation { duration: Theme.durationFast } }
                                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                    Text {
                                        id: rateChipLabel
                                        anchors.centerIn: parent
                                        text:           modelData + qsTr(" Hz")
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          rateChip.isSelected ? Theme.colorAccent : Theme.colorText2
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    }

                                    MouseArea {
                                        id: rateArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape:  Qt.PointingHandCursor
                                        onClicked: {
                                            var map = appSettings.imuOutputRateHz
                                            map[imuData.id] = modelData
                                            appSettings.imuOutputRateHz = map
                                            if (imuRow.isConnected && imuRow.inst)
                                                imuRow.inst.setOutputRateHz(modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    // ── Test button ──────────────────────────────────────────
                    Rectangle {
                        id: testBtn
                        readonly property bool active: imuRow.testOpen
                        width:  testBtnLabel.implicitWidth + Theme.sp(24)
                        height: Theme.sp(26)
                        radius: Theme.radius
                        color:  active ? Theme.colorAccentLight
                              : testPress.containsMouse
                                  ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                  : "transparent"
                        border.width: 1
                        border.color: active ? Theme.colorAccent
                                    : testPress.containsMouse ? Theme.colorAccentMid
                                    : Theme.colorBorderStrong
                        Layout.alignment: Qt.AlignVCenter
                        Behavior on color       { ColorAnimation { duration: Theme.durationFast } }
                        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                        Text {
                            id: testBtnLabel
                            anchors.centerIn: parent
                            text:           qsTr("Test")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                            color:          testBtn.active ? Theme.colorAccent : Theme.colorText2
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        PpPressable {
                            id: testPress
                            onClicked: {
                                if (imuRow.testOpen) {
                                    root.openTestId = ""
                                } else {
                                    root.openTestId = imuData.id
                                    if (!imuRow.isConnected)
                                        imuManager.setSelected(imuData.index, true)
                                }
                            }
                        }
                    }
                }

                // ── Capabilities strip ───────────────────────────────────────
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Repeater {
                        model: imuRow.capsCells

                        delegate: Rectangle {
                            required property var modelData
                            required property int index

                            Layout.fillWidth: true
                            implicitHeight: capsCol.implicitHeight + Theme.sp(18)
                            color: Theme.colorBg2

                            // Vertical separator (except last)
                            Rectangle {
                                anchors.right:  parent.right
                                anchors.top:    parent.top
                                anchors.bottom: parent.bottom
                                width:   1
                                color:   Theme.colorBorderMid
                                opacity: Theme.borderOpacityNormal
                                visible: index < 5
                            }

                            ColumnLayout {
                                id: capsCol
                                anchors {
                                    left:   parent.left
                                    right:  parent.right
                                    top:    parent.top
                                    leftMargin:  Theme.sp(14)
                                    rightMargin: Theme.sp(14)
                                    topMargin:   Theme.sp(9)
                                }
                                spacing: Theme.sp(3)

                                Text {
                                    text:                modelData.key
                                    font.family:         Theme.fontData
                                    font.pixelSize:      Theme.fontSzMicro
                                    font.letterSpacing:  Theme.trackingMicro
                                    font.capitalization: Font.AllUppercase
                                    color:               Theme.colorText3
                                }
                                Text {
                                    text:  modelData.val
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzBody2
                                    color: modelData.isAccent ? Theme.colorAccent
                                         : (modelData.batPct >= 0)
                                               ? (modelData.batPct > 60 ? Theme.colorGood
                                                 : modelData.batPct > 20 ? Theme.colorWarn
                                                 :                         Theme.colorError)
                                         : Theme.colorText
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Test panel ───────────────────────────────────────────────────────
        Item {
            id: testPanel
            anchors.top:   bodyWrap.bottom
            anchors.left:  parent.left
            anchors.right: parent.right
            height: imuRow.testOpen && !imuRow.isExcluded ? testPanelContent.implicitHeight : 0
            clip:   true

            Behavior on height { NumberAnimation { duration: Theme.durationFast } }

            ColumnLayout {
                id: testPanelContent
                anchors.left:  parent.left
                anchors.right: parent.right
                spacing: 0

                // Top separator
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color:  Theme.colorBorderMid
                    opacity: Theme.borderOpacityNormal
                }

                // ── Panel header ──────────────────────────────────────────────
                RowLayout {
                    Layout.fillWidth:    true
                    Layout.leftMargin:   Theme.sp(14)
                    Layout.rightMargin:  Theme.sp(14)
                    Layout.topMargin:    Theme.sp(12)
                    Layout.bottomMargin: Theme.sp(12)
                    spacing: Theme.sp(8)

                    Text {
                        text: {
                            var p = appSettings.imuPlacement[imuData.id]
                            var label = (p && p !== "") ? (p + " — " + imuData.description)
                                                        : imuData.description
                            return qsTr("Live test — ") + label
                        }
                        font.family:         Theme.fontData
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                        Layout.fillWidth:    true
                    }

                    // Connect / Disconnect button — fixed width sized for the longer label.
                    Rectangle {
                        id: connBtn
                        readonly property bool connected: imuRow.isConnected
                        width:  disconnectMeasure.implicitWidth + Theme.sp(24)
                        height: Theme.sp(26)
                        radius: Theme.radius
                        color:  connPress.containsMouse
                                    ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                    : "transparent"
                        border.width: 1
                        border.color: connected ? Qt.rgba(Theme.colorWarn.r, Theme.colorWarn.g, Theme.colorWarn.b, 0.5)
                                    : connPress.containsMouse ? Theme.colorAccentMid
                                    : Theme.colorBorderStrong
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                        // Invisible sizer — always measures the longer string so the button
                        // never shrinks when switching between "Connect" and "Disconnect".
                        Text {
                            id: disconnectMeasure
                            visible:        false
                            text:           qsTr("Disconnect")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                        }

                        Text {
                            id: connBtnLabel
                            anchors.centerIn: parent
                            text:           connBtn.connected ? qsTr("Disconnect") : qsTr("Connect")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                            color:          connBtn.connected ? Theme.colorWarn : Theme.colorAccent
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        PpPressable {
                            id: connPress
                            onClicked: {
                                if (imuRow.isConnected)
                                    imuManager.setSelected(imuData.index, false)
                                else
                                    imuManager.setSelected(imuData.index, true)
                            }
                        }

                        // Traveling-light frame while this device's BLE connect
                        // is in flight (busy spans the retry/backoff/watchdog).
                        PpConnectingFrame {
                            anchors.fill: parent
                            radius:  parent.radius
                            running: imuRow.inst !== null && imuRow.inst.busy && !connBtn.connected
                        }
                    }

                    // Done button — closes the test panel; does not disconnect
                    Rectangle {
                        id: testDoneBtn
                        width:  doneBtnLabel.implicitWidth + Theme.sp(24)
                        height: Theme.sp(26)
                        radius: Theme.radius
                        color:  testDonePress.containsMouse ? Qt.lighter(Theme.colorAccentLight, 1.08) : Theme.colorAccentLight
                        border.width: 1
                        border.color: Theme.colorAccent
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                        Text {
                            id: doneBtnLabel
                            anchors.centerIn: parent
                            text:           qsTr("Done")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight:    Font.Normal
                            color:          Theme.colorAccent
                        }

                        PpPressable {
                            id: testDonePress
                            onClicked:    root.openTestId = ""
                        }
                    }
                }

                // ── Body — viz left, live data right ─────────────────────────
                RowLayout {
                    Layout.fillWidth:    true
                    Layout.leftMargin:   Theme.sp(14)
                    Layout.rightMargin:  Theme.sp(14)
                    Layout.bottomMargin: Theme.sp(24)
                    spacing: Theme.sp(16)

                    // ImuVizView — loaded only when test panel is open to avoid
                    // creating GPU contexts for every row at startup.
                    Item {
                        width:  Theme.sp(220)
                        height: Theme.sp(220)
                        Layout.alignment: Qt.AlignTop

                        Loader {
                            anchors.fill: parent
                            active:  imuRow.testOpen && imuRow.inst !== null
                            visible: imuRow.isConnected
                            sourceComponent: Component {
                                ImuVizView {
                                    anchors.fill: parent
                                    controller:   imuRow.inst
                                }
                            }
                        }

                        // Placeholder when not connected
                        Rectangle {
                            anchors.fill:  parent
                            color:         Theme.colorBg
                            border.width:  1
                            border.color:  Theme.colorBorderMid
                            radius:        Theme.radius
                            visible:       !imuRow.isConnected

                            Text {
                                anchors.centerIn: parent
                                text:           qsTr("Not connected")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }
                    }

                    // ── Live data ─────────────────────────────────────────────
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Theme.sp(12)

                        // Connection status + battery
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(8)

                            // Status dot
                            Rectangle {
                                width:  Theme.sp(6)
                                height: Theme.sp(6)
                                radius: Theme.sp(3)
                                color: imuRow.isConnected                     ? Theme.colorGood
                                     : (imuRow.inst && imuRow.inst.busy)      ? Theme.colorAccent
                                     :                                           Theme.colorText3
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            }

                            // State label + data rate
                            Text {
                                text: {
                                    var s = imuRow.inst ? imuRow.inst.stateLabel : qsTr("Disconnected")
                                    if (imuRow.isConnected && imuRow.inst && imuRow.inst.dataRateHz > 0)
                                        return s + qsTr(" · ") + imuRow.inst.dataRateHz.toFixed(1) + qsTr(" Hz")
                                    return s
                                }
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzBody2
                                color: imuRow.isConnected                     ? Theme.colorGood
                                     : (imuRow.inst && imuRow.inst.busy)      ? Theme.colorWarn
                                     :                                           Theme.colorText3
                                Layout.fillWidth: true
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            }

                            // Battery indicator
                            RowLayout {
                                visible: imuRow.isConnected
                                         && imuRow.inst !== null
                                         && imuRow.inst.batteryPercent >= 0
                                spacing: Theme.sp(4)

                                Text {
                                    text:           qsTr("BAT")
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }

                                Rectangle {
                                    width:  Theme.sp(40)
                                    height: Theme.sp(8)
                                    radius: Theme.sp(2)
                                    color:  Theme.colorBg3
                                    border.width: 1
                                    border.color: Theme.colorBorderStrong

                                    Rectangle {
                                        property int pct: imuRow.inst ? imuRow.inst.batteryPercent : 0
                                        width:  parent.width * Math.max(0, Math.min(1, pct / 100.0))
                                        height: parent.height
                                        radius: parent.radius
                                        color:  pct > 60 ? Theme.colorGood
                                              : pct > 20 ? Theme.colorWarn
                                              :             Theme.colorError
                                        Behavior on width { NumberAnimation { duration: Theme.durationFast } }
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    }
                                }

                                Text {
                                    property int pct: imuRow.inst ? imuRow.inst.batteryPercent : 0
                                    text:  pct + qsTr("%")
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color: pct > 60 ? Theme.colorGood
                                         : pct > 20 ? Theme.colorWarn
                                         :             Theme.colorError
                                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                }
                            }
                        }

                        // ── Gimbal drop counter ───────────────────────────────
                        RowLayout {
                            Layout.fillWidth: true
                            visible: imuRow.isConnected && imuRow.inst !== null
                                     && imuRow.inst.gimbalDropCount > 0
                            spacing: Theme.sp(6)

                            Text {
                                text:           qsTr("GIMBAL DROPS")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                            Text {
                                text:           imuRow.inst ? imuRow.inst.gimbalDropCount : 0
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorWarn
                            }
                            Text {
                                text:           qsTr("packets dropped this session")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                Layout.fillWidth: true
                            }
                        }

                        // ── Euler angles ──────────────────────────────────────
                        ColumnLayout {
                            Layout.fillWidth:    true
                            Layout.bottomMargin: Theme.sp(8)
                            spacing: Theme.sp(4)

                            Text {
                                text:                qsTr("QUATERNION")
                                font.family:         Theme.fontData
                                font.pixelSize:      Theme.fontSzMicro
                                font.letterSpacing:  Theme.trackingMicro
                                font.capitalization: Font.AllUppercase
                                color:               Theme.colorText3
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.sp(6)

                                Repeater {
                                    model: 4

                                    delegate: Rectangle {
                                        required property int index

                                        Layout.fillWidth: true
                                        height: Theme.sp(60)
                                        color:  Theme.colorBg2
                                        radius: Theme.radius
                                        border.width: 1
                                        border.color: Theme.colorBorderMid

                                        ColumnLayout {
                                            anchors.fill:          parent
                                            anchors.topMargin:     Theme.sp(8)
                                            anchors.bottomMargin:  Theme.sp(12)
                                            anchors.leftMargin:    Theme.sp(8)
                                            anchors.rightMargin:   Theme.sp(8)
                                            spacing: Theme.sp(2)

                                            Text {
                                                text: ["W", "X", "Y", "Z"][index]
                                                font.family:         Theme.fontData
                                                font.pixelSize:      Theme.fontSzMicro
                                                font.letterSpacing:  Theme.trackingMicro
                                                font.capitalization: Font.AllUppercase
                                                color:               Theme.colorText3
                                            }
                                            Text {
                                                text: [
                                                    (imuRow.inst ? imuRow.inst.quatW.toFixed(4) : "1.0000"),
                                                    (imuRow.inst ? imuRow.inst.quatX.toFixed(4) : "0.0000"),
                                                    (imuRow.inst ? imuRow.inst.quatY.toFixed(4) : "0.0000"),
                                                    (imuRow.inst ? imuRow.inst.quatZ.toFixed(4) : "0.0000")
                                                ][index]
                                                font.family:    Theme.fontData
                                                font.pixelSize: Theme.fontSzHeading
                                                color:          Theme.colorText
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // ── Calibration tools ─────────────────────────────────
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(4)
                            enabled: imuRow.isConnected && imuRow.inst !== null

                            Text {
                                text:                qsTr("CALIBRATION")
                                font.family:         Theme.fontData
                                font.pixelSize:      Theme.fontSzMicro
                                font.letterSpacing:  Theme.trackingMicro
                                font.capitalization: Font.AllUppercase
                                color:               Theme.colorText3
                            }

                            Row {
                                spacing: Theme.sp(6)

                                // Zero orientation
                                Rectangle {
                                    id: zeroBtn
                                    width:  zeroLabel.implicitWidth + Theme.sp(20)
                                    height: Theme.sp(24)
                                    radius: Theme.radius
                                    color:  (parent.parent.enabled && zeroPress.containsMouse)
                                                ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                                : "transparent"
                                    border.width: 1
                                    border.color: !parent.parent.enabled ? Theme.colorBorderMid
                                                : zeroPress.containsMouse ? Theme.colorAccentMid
                                                : Theme.colorBorderStrong
                                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                    Text {
                                        id: zeroLabel
                                        anchors.centerIn: parent
                                        text:           qsTr("Zero orientation")
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    Theme.fontBodyWeight
                                        color:          parent.parent.parent.enabled
                                                            ? Theme.colorText2 : Theme.colorText3
                                    }

                                    PpPressable {
                                        id: zeroPress
                                        onClicked:    if (imuRow.inst) imuRow.inst.zeroOrientation()
                                    }
                                }

                                // Calibrate magnetometer (placeholder — no API yet)
                                Rectangle {
                                    visible: imuData.supportsMagCalibration
                                    width:   magCalLabel.implicitWidth + Theme.sp(20)
                                    height:  Theme.sp(24)
                                    radius:  Theme.radius
                                    color:   "transparent"
                                    border.width: 1
                                    border.color: Theme.colorBorderMid
                                    opacity: 0.5

                                    Text {
                                        id: magCalLabel
                                        anchors.centerIn: parent
                                        text:           qsTr("Calibrate mag")
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    Theme.fontBodyWeight
                                        color:          Theme.colorText3
                                    }
                                    // TODO: imuManager.calibrateMagnetometer() — not implemented yet
                                }

                                // Save to flash (placeholder — no API yet)
                                Rectangle {
                                    visible: imuData.supportsConfigPersistence
                                    width:   saveFlashLabel.implicitWidth + Theme.sp(20)
                                    height:  Theme.sp(24)
                                    radius:  Theme.radius
                                    color:   "transparent"
                                    border.width: 1
                                    border.color: Theme.colorBorderMid
                                    opacity: 0.5

                                    Text {
                                        id: saveFlashLabel
                                        anchors.centerIn: parent
                                        text:           qsTr("Save to flash")
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    Theme.fontBodyWeight
                                        color:          Theme.colorText3
                                    }
                                    // TODO: imuManager.saveConfigToFlash() — not implemented yet
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Search scroll-to support ──────────────────────────────────────────────

    property string lastHighlightId: ""

    function findChild(parent, name) {
        for (var i = 0; i < parent.children.length; i++) {
            var child = parent.children[i]
            if (child.objectName === name) return child
            var found = findChild(child, name)
            if (found) return found
        }
        return null
    }

    function scrollToItem(itemId) {
        if (!itemId) return true
        var target = findChild(contentCol, itemId)
        if (!target) return false
        var mapped = target.mapToItem(contentCol, 0, 0)
        scrollView.contentItem.contentY = Math.max(0, Math.min(
            mapped.y - Theme.sp(24),
            scrollView.contentItem.contentHeight - scrollView.height
        ))
        target.searchHighlight = true
        lastHighlightId = itemId
        highlightTimer.restart()
        return true
    }

    Timer {
        id: highlightTimer
        interval: 1800
        onTriggered: {
            var target = findChild(contentCol, lastHighlightId)
            if (target) target.searchHighlight = false
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Main scroll view
    // ─────────────────────────────────────────────────────────────────────────
    ScrollView {
        id: scrollView
        anchors.fill:  parent
        contentWidth:  availableWidth
        contentHeight: contentCol.y + contentCol.implicitHeight + Theme.sp(28)

        ColumnLayout {
            id: contentCol
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(16)

            // ── Page header ────────────────────────────────────────────────
            Text {
                text:                qsTr("HARDWARE")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            PpDisplayText {
                text: qsTr("IMUs")
            }

            Text {
                text: qsTr("All enumerated IMU devices are listed below. Assign each to a body placement, configure the output rate, and use the Test panel to verify orientation before a session.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                Layout.fillWidth: true
            }

            // ── Enumerated devices header ──────────────────────────────────
            RowLayout {
                Layout.fillWidth: true

                Text {
                    text:                qsTr("ENUMERATED DEVICES")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                    Layout.fillWidth:    true
                }

                // Scan button — fixed width sized for the longer "Scanning…" label.
                Rectangle {
                    id: scanBtn
                    property bool scanning: false

                    width:  scanningMeasure.implicitWidth + Theme.sp(24)
                    height: Theme.sp(26)
                    radius: Theme.radius
                    color:  scanning ? Theme.colorAccentLight
                          : scanPress.containsMouse
                              ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                              : "transparent"
                    border.width: 1
                    border.color: scanning ? Theme.colorAccent
                                : scanPress.containsMouse ? Theme.colorAccentMid
                                : Theme.colorBorderStrong
                    Behavior on color       { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                    // Invisible sizer — always measures the longer string.
                    Text {
                        id: scanningMeasure
                        visible:        false
                        text:           qsTr("Scanning…")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                    }

                    Text {
                        id: scanBtnLabel
                        anchors.centerIn: parent
                        text:           scanBtn.scanning ? qsTr("Scanning…") : qsTr("Scan")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color:          scanBtn.scanning ? Theme.colorAccent : Theme.colorText2
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }

                    Timer {
                        id: scanTimer
                        interval: 30000
                        onTriggered: scanBtn.scanning = false
                    }

                    Connections {
                        target: imuManager
                        function onImuEnumeratedCountChanged() { scanTimer.stop(); scanBtn.scanning = false }
                    }

                    PpPressable {
                        id: scanPress
                        onClicked: {
                            scanBtn.scanning = true
                            imuManager.rescanImu()
                            scanTimer.restart()
                        }
                    }
                }
            }

            // ── Device rows ────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.sp(26)
                spacing: Theme.sp(8)

                Repeater {
                    model: imuManager.imuDeviceList

                    delegate: ImuDeviceRow {
                        required property var modelData
                        imuData:          modelData
                        Layout.fillWidth: true
                        // Dim (but keep listed + interactive) when absent from the
                        // latest scan, so a known device can still be renamed/forgotten.
                        opacity: modelData.present ? 1.0 : 0.45
                        Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
                    }
                }

                // Empty state — distinguishes "discovery couldn't run" (e.g.
                // Bluetooth off / no adapter, surfaced via imuScanError) from a
                // genuine "no devices in range".
                Text {
                    visible:        imuManager.imuDeviceList.length === 0
                    text:           imuManager.imuScanError !== ""
                                        ? qsTr("Bluetooth discovery error: %1").arg(imuManager.imuScanError)
                                        : qsTr("No IMU devices found. Click Scan to search for BLE devices.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    font.italic:    true
                    color:          imuManager.imuScanError !== "" ? Theme.colorWarn : Theme.colorText3
                    wrapMode:       Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            // ── Status summary ─────────────────────────────────────────────
            Rectangle {
                id: summaryRect
                Layout.fillWidth: true
                Layout.leftMargin: Theme.sp(26)
                height:  Theme.sp(40)
                color:   Theme.colorBg2
                radius:  Theme.radius
                border.width: 1
                border.color: Theme.colorBorderMid

                readonly property int enabledCount: {
                    var list = imuManager.imuDeviceList
                    var ex   = appSettings.imuExcluded
                    return list.filter(function(d) { return ex.indexOf(d.id) < 0 }).length
                }
                readonly property int connectedCount: imuManager.imuCount

                readonly property var assignedPlacements: {
                    var map = appSettings.imuPlacement
                    return Object.values(map).filter(function(v) { return v && v !== "" && v !== "other" })
                }
                readonly property var missingPlacements: {
                    var all = ["A", "B", "C", "D"]
                    return all.filter(function(p) { return summaryRect.assignedPlacements.indexOf(p) < 0 })
                }

                RowLayout {
                    anchors.fill:    parent
                    anchors.margins: Theme.sp(12)
                    spacing:         Theme.sp(16)

                    Text {
                        text:  summaryRect.connectedCount + qsTr(" of ") + summaryRect.enabledCount + qsTr(" connected")
                        color: (summaryRect.connectedCount === summaryRect.enabledCount && summaryRect.enabledCount > 0)
                                    ? Theme.colorGood : Theme.colorWarn
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }

                    Rectangle { width: 1; height: Theme.sp(14); color: Theme.colorBorderStrong; opacity: 0.4 }

                    Text {
                        text:  summaryRect.assignedPlacements.length > 0
                                   ? qsTr("Assigned: ") + summaryRect.assignedPlacements.join(qsTr(" · "))
                                   : qsTr("No placements assigned")
                        color:          Theme.colorText2
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        Layout.fillWidth: true
                    }

                    Text {
                        visible:        summaryRect.missingPlacements.length > 0
                        text:           qsTr("Missing: ") + summaryRect.missingPlacements.join(qsTr(" · "))
                        color:          Theme.colorWarn
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                    }
                }
            }

            PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

            // ── Global IMU settings ────────────────────────────────────────
            Text {
                text:                qsTr("GLOBAL IMU SETTINGS")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            // Auto-connect on session start
            RowLayout {
                objectName: "setting_imuAutoConnect"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)

                    Text {
                        text:           qsTr("Auto-connect on session start")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("Connects all enabled devices before recording begins")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }

                TogglePill {
                    checked: appSettings.imuAutoConnect
                    onToggled: (v) => appSettings.imuAutoConnect = v
                    Layout.alignment: Qt.AlignVCenter
                }
            }

            // Auto-reconnect on signal loss
            RowLayout {
                objectName: "setting_imuAutoReconnect"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)

                    Text {
                        text:           qsTr("Auto-reconnect on signal loss")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("Attempts reconnect if the BLE link drops during a session")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }

                TogglePill {
                    checked: appSettings.imuAutoReconnect
                    onToggled: (v) => appSettings.imuAutoReconnect = v
                    Layout.alignment: Qt.AlignVCenter
                }
            }

            // Save calibration to device flash
            RowLayout {
                objectName: "setting_imuFlash"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)

                    Text {
                        text:           qsTr("Save calibration to device flash")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("Persists zero-orientation and mag calibration across power cycles")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }

                TogglePill {
                    checked: appSettings.imuSaveCalibrationToFlash
                    onToggled: (v) => appSettings.imuSaveCalibrationToFlash = v
                    Layout.alignment: Qt.AlignVCenter
                }
            }

            PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

            // ── Orientation fusion (global) ────────────────────────────────
            Text {
                text:                qsTr("ORIENTATION FUSION")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            ColumnLayout {
                Layout.fillWidth:  true
                Layout.leftMargin: Theme.sp(26)
                spacing:           Theme.sp(6)

                RowLayout {
                    spacing: Theme.sp(8)

                    Repeater {
                        model: [ { key: "Madgwick", label: qsTr("Madgwick Filter") },
                                 { key: "ESKF",     label: qsTr("Error State Kalman Filter (ESKF)") } ]

                        delegate: Rectangle {
                            id: fusionSeg
                            required property var modelData
                            readonly property bool active:
                                appSettings.imuOrientationFilter === modelData.key

                            implicitWidth: segLabel.implicitWidth + Theme.sp(28)
                            height:        Theme.sp(30)
                            radius:        Theme.radius
                            color:         active ? Theme.colorAccentLight
                                         : fusionArea.containsMouse
                                             ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                             : "transparent"
                            border.width:  1
                            border.color:  active ? Theme.colorAccent
                                         : fusionArea.containsMouse ? Theme.colorAccentMid
                                         : Theme.colorBorderStrong
                            Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                            Text {
                                id: segLabel
                                anchors.centerIn: parent
                                text:           fusionSeg.modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                font.weight:    Theme.fontBodyWeight
                                color:          fusionSeg.active ? Theme.colorAccent : Theme.colorText2
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            }

                            MouseArea {
                                id: fusionArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                onClicked:    imuManager.setOrientationFilter(fusionSeg.modelData.key)
                            }
                        }
                    }
                }

                Text {
                    text: qsTr("Software sensor-fusion algorithm used to derive orientation from the raw gyroscope + accelerometer. Applies to all IMUs and takes effect immediately.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }
    }
}
