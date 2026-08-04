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
import QtQuick.Controls.Basic
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    property int    cameraCount:    cameraManager.cameraList.length
    property int    imuCount:       imuManager.imuEnumeratedCount
    property int    activeNavIndex: 0
    property string searchQuery:    ""

    // Emitted by the pinned bottom action; Main.qml jumps to the System Info
    // screen (its own screen — back returns here).
    signal resourceMonitorRequested()

    // ── Full-text search ──────────────────────────────────────────────────────

    readonly property bool isSearching: root.searchQuery.trim().length >= 2

    readonly property var searchResults: {
        var q = root.searchQuery.trim().toLowerCase()
        if (q.length < 2) return []
        var results = []
        var entries = SettingsIndex.entries
        for (var i = 0; i < entries.length; i++) {
            var e = entries[i]
            var haystack = (e.label + " " + e.subtitle + " " + (e.actions || "") + " " + e.groupLabel + " " + e.panelLabel).toLowerCase()
            if (haystack.indexOf(q) >= 0)
                results.push(e)
        }
        return results
    }

    function navigateToResult(entry) {
        searchInput.text = ""
        root.searchQuery  = ""
        root.activeNavIndex = entry.panelIndex
        Qt.callLater(function() {
            var panels = [
                generalPanel, appearancePanel, displaysPanel,
                camerasPanel, imusPanel, microphonesPanel,
                launchMonitorPanel, storagePanel, null, diagnosticModelPanel
            ]
            var panel = panels[entry.panelIndex]
            if (panel) scrollWithRetry(panel, entry.itemId, 0)
        })
    }

    // Deep link straight to one metric (dashboard tile click-through, routed via
    // MetricRoute). Same callLater shape as navigateToResult: the panel Loader must
    // have instantiated before we can address it.
    function showMetricDetail(key) {
        searchInput.text = ""
        root.searchQuery = ""
        root.activeNavIndex = 9                       // Diagnostic Model
        Qt.callLater(function() { diagnosticModelPanel.showMetric(key) })
    }

    // Deep link straight to one characteristic's detail page. Same callLater shape as
    // showMetricDetail: the panel Loader must have instantiated before we can address it.
    function showCharacteristicDetail(conditionId) {
        searchInput.text = ""
        root.searchQuery = ""
        root.activeNavIndex = 9                       // Diagnostic Model
        Qt.callLater(function() { diagnosticModelPanel.showCharacteristic(conditionId) })
    }

    // Deep link to the MEASURE that defines a corridor. Reached from a metric's detail page: the
    // metric catalogue says what good looks like, and this is the way to the place that decides it.
    // Same callLater shape as its two siblings above.
    function showMeasureDetail(measureId) {
        searchInput.text = ""
        root.searchQuery = ""
        root.activeNavIndex = 9                       // Diagnostic Model
        Qt.callLater(function() { diagnosticModelPanel.showMeasure(measureId) })
    }

    function scrollWithRetry(panel, itemId, retries) {
        if (!panel || retries > 3) return
        var ok = panel.scrollToItem(itemId)
        if (!ok && retries < 3)
            Qt.callLater(function() { scrollWithRetry(panel, itemId, retries + 1) })
    }

    Shortcut {
        sequences: [ StandardKey.Find ]
        onActivated: searchInput.forceActiveFocus()
    }

    // ── The fold ──────────────────────────────────────────────────────────────
    //
    // The sidenav costs 275px on every settings panel, and on the widest of them — the Diagnostic
    // Model, three panes and a facet rail — that is the width the name column starves for. Folding it
    // to an icon strip gives the panel back a quarter of the room it was short of.
    //
    // Deliberately MANUAL. A width-driven auto-fold was the alternative and it is the wrong trade: a
    // sidenav that vanishes on its own teaches an author that the app moves things when they are not
    // looking. The state persists in AppSettings and applies to every settings panel, because a fold
    // that resets on every visit is a fold you re-do forever.
    readonly property bool navCollapsed: appSettings.settingsNavCollapsed

    Shortcut {
        // ⌘\ on macOS, Ctrl+\ elsewhere — Qt maps the modifier per platform.
        sequence: "Ctrl+\\"
        onActivated: appSettings.settingsNavCollapsed = !appSettings.settingsNavCollapsed
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── Sidenav ──────────────────────────────────────────────────────────
        Item {
            id: sidenav
            Layout.preferredWidth: root.navCollapsed ? Theme.sp(46) : Theme.sidenavWidth
            Layout.fillHeight: true

            Behavior on Layout.preferredWidth {
                NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic }
            }

            Rectangle {
                anchors.fill: parent
                color: Theme.colorSurface
            }

            Rectangle {
                anchors.right:  parent.right
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width:   1
                color:   Theme.colorBorderMid
                opacity: Theme.borderOpacityNormal
            }

            // A layout rather than a plain Column since the fold gave the strip a foot: the nav list
            // takes what is left instead of every item computing its own height off the parent's.
            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // ── Search field ──────────────────────────────────────────────
                // Gone when folded. A 46px strip cannot hold a text field, and half of one that
                // could not be typed into would be chrome pretending to be a control.
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.navCollapsed ? 0 : Theme.sp(46)
                    visible: !root.navCollapsed
                    clip:    true

                    Row {
                        id: searchRow
                        anchors.left:            parent.left
                        anchors.right:           parent.right
                        anchors.verticalCenter:  parent.verticalCenter
                        anchors.leftMargin:  Theme.sp(10)
                        anchors.rightMargin: Theme.sp(10)
                        spacing: Theme.sp(6)

                        Text {
                            id: searchIcon
                            text: "⌕"
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzHeading
                            color: searchInput.activeFocus ? Theme.colorText2 : Theme.colorText3
                            anchors.verticalCenter: parent.verticalCenter
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        TextField {
                            id: searchInput
                            width:         searchRow.width - searchIcon.width - searchRow.spacing
                            topPadding:    0
                            bottomPadding: 0
                            leftPadding:   0
                            rightPadding:  0
                            placeholderText:      qsTr("Search settings…")
                            placeholderTextColor: Theme.colorText3
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                            color:          Theme.colorText
                            background:     null
                            onTextChanged: root.searchQuery = text.toLowerCase()
                        }
                    }

                    Rectangle {
                        anchors.bottom:       parent.bottom
                        anchors.bottomMargin: Theme.sp(6)
                        anchors.left:         parent.left
                        anchors.right:        parent.right
                        anchors.leftMargin:   Theme.sp(10)
                        anchors.rightMargin:  Theme.sp(10)
                        height: 1
                        color: searchInput.activeFocus ? Theme.colorAccent : Theme.colorBorderMid
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }
                }

                // ── Nav list — hidden while searching ────────────────────────
                Item {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    clip:    true
                    visible: !root.isSearching

                    Column {
                        width:   parent.width
                        spacing: 0

                        Repeater {
                            model: [
                                { navIdx: 0, icon: "⊞", label: qsTr("General"),       sectionHead: qsTr("General"),  hasBadge: false },
                                { navIdx: 1, icon: "◐", label: qsTr("Appearance"),     sectionHead: "",               hasBadge: false },
                                { navIdx: 2, icon: "▭", label: qsTr("Displays"),       sectionHead: "",               hasBadge: false },
                                { navIdx: 3, icon: "⊙", label: qsTr("Cameras"),        sectionHead: qsTr("Hardware"), hasBadge: true  },
                                { navIdx: 4, icon: "⌖", label: qsTr("IMUs"),           sectionHead: "",               hasBadge: true  },
                                { navIdx: 5, icon: "♪", label: qsTr("Microphone"),     sectionHead: "",               hasBadge: false },
                                { navIdx: 6, icon: "◎", label: qsTr("Launch Monitor"), sectionHead: "",               hasBadge: false },
                                { navIdx: 7, icon: "▥", label: qsTr("Storage"),        sectionHead: qsTr("Data"),     hasBadge: false },
                                { navIdx: 8, icon: "▤", label: qsTr("Archiving"),      sectionHead: "",               hasBadge: false },
                                // Metrics and Diagnostics were both rows here once. A metric, the
                                // measures that read it and the corridors that judge them are one
                                // chain, and following it meant leaving the panel — so they became
                                // views inside Diagnostic Model, which now answers every deep link
                                // the two of them used to. The old panel and its hidden row are
                                // gone, and the indices below are contiguous again.
                                { navIdx: 9, icon: "❖", label: qsTr("Diagnostic Model"), sectionHead: qsTr("Reference"), hasBadge: false },
                                // Not a panel: emits resourceMonitorRequested() (its own screen)
                                // rather than switching activeNavIndex. It is an ACTION row, so its
                                // navIdx never indexes the StackLayout — but it must not collide
                                // with a panel index either, which is why it sits one past the last.
                                { navIdx: 10, icon: "◈", label: qsTr("System"),        sectionHead: "",               hasBadge: false, action: "system" }
                            ]

                            delegate: Column {
                                required property var modelData

                                width:   sidenav.width
                                spacing: 0

                                // Section eyebrow. Folded, the word will not fit and is drawn as the
                                // rule it always was — the grouping survives, the label waits.
                                Item {
                                    width:   parent.width
                                    height:  modelData.sectionHead === "" ? 0
                                           : root.navCollapsed ? Theme.sp(13)
                                                               : Theme.sp(14) + eyebrow.implicitHeight + Theme.sp(4)
                                    visible: modelData.sectionHead !== ""

                                    Text {
                                        id: eyebrow
                                        anchors.left:         parent.left
                                        anchors.leftMargin:   Theme.sp(16)
                                        anchors.bottom:       parent.bottom
                                        anchors.bottomMargin: Theme.sp(4)
                                        visible:              !root.navCollapsed
                                        text:                 modelData.sectionHead
                                        font.family:          Theme.fontBody
                                        font.pixelSize:       Theme.fontSzMicro
                                        font.weight:          Theme.fontBodyWeight
                                        font.letterSpacing:   Theme.trackingMicro
                                        font.capitalization:  Font.AllUppercase
                                        color:                Theme.colorText3
                                    }

                                    Rectangle {
                                        anchors.left:           parent.left
                                        anchors.right:          parent.right
                                        anchors.leftMargin:     Theme.sp(12)
                                        anchors.rightMargin:    Theme.sp(12)
                                        anchors.verticalCenter: parent.verticalCenter
                                        height:  1
                                        visible: root.navCollapsed
                                        color:   Theme.colorBorderMid
                                        opacity: Theme.borderOpacityNormal
                                    }
                                }

                                // Nav item row
                                Item {
                                    id: navItem
                                    width:  parent.width
                                    height: Theme.sp(38)

                                    readonly property bool isActive: root.activeNavIndex === modelData.navIdx
                                    readonly property bool hovered:  navArea.containsMouse
                                    readonly property int  actualBadge: !modelData.hasBadge ? -1
                                                                        : (modelData.navIdx === 3 ? root.cameraCount
                                                                        : (modelData.navIdx === 4 ? root.imuCount : -1))

                                    opacity: root.searchQuery.length === 0
                                             || modelData.label.toLowerCase().indexOf(root.searchQuery) >= 0
                                             ? 1.0 : 0.3
                                    Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

                                    Rectangle {
                                        anchors.fill: parent
                                        color: navItem.isActive ? Theme.colorAccentLight
                                                                : (navItem.hovered ? Theme.colorBg2 : "transparent")
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    }

                                    Rectangle {
                                        anchors.left:   parent.left
                                        anchors.top:    parent.top
                                        anchors.bottom: parent.bottom
                                        width: 2
                                        color: navItem.isActive ? Theme.colorAccent : "transparent"
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    }

                                    // Folded, the icon takes the middle of the strip and the row keeps
                                    // its full height — a fold that also shrank the rows would be a
                                    // different nav, not the same one narrower.
                                    Text {
                                        id: navItemIcon
                                        anchors.left:           parent.left
                                        anchors.leftMargin:     root.navCollapsed
                                                                    ? (parent.width - navItemIcon.width) / 2
                                                                    : Theme.sp(16)
                                        anchors.verticalCenter: parent.verticalCenter
                                        text:           modelData.icon
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzBody
                                        color: navItem.isActive ? Theme.colorAccent
                                                                : (navItem.hovered ? Theme.colorText2 : Theme.colorText3)
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    }

                                    Text {
                                        anchors.left:           navItemIcon.right
                                        anchors.leftMargin:     Theme.sp(10)
                                        anchors.right:          navItemBadge.visible ? navItemBadge.left : parent.right
                                        anchors.rightMargin:    Theme.sp(10)
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible:        !root.navCollapsed
                                        text:           modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    navItem.isActive ? Font.Normal : Theme.fontBodyWeight
                                        color: navItem.isActive ? Theme.colorAccent
                                                                : (navItem.hovered ? Theme.colorText : Theme.colorText2)
                                        elide: Text.ElideRight
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    }

                                    Text {
                                        id: navItemBadge
                                        anchors.right:          parent.right
                                        anchors.rightMargin:    Theme.sp(12)
                                        anchors.verticalCenter: parent.verticalCenter
                                        text:           navItem.actualBadge >= 0 ? navItem.actualBadge.toString() : ""
                                        visible:        navItem.actualBadge >= 0 && !root.navCollapsed
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color: navItem.isActive ? Theme.colorAccent : Theme.colorText3
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    }

                                    // Folded, hovering the strip peeks the label. The icons alone are
                                    // learnable but not self-describing, and a strip you have to
                                    // unfold to read is a strip nobody folds twice.
                                    ToolTip.visible: root.navCollapsed && navItem.hovered
                                    ToolTip.text:    modelData.label
                                    ToolTip.delay:   300

                                    PpPressable {
                                        id: navArea
                                        hoverScale: 1.0
                                        onClicked: {
                                            if (modelData.action === "system")
                                                root.resourceMonitorRequested()
                                            else
                                                root.activeNavIndex = modelData.navIdx
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ── Search results — shown while searching ────────────────────
                Item {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    clip:    true
                    visible: root.isSearching

                    ListView {
                        id: resultsList
                        anchors.fill:    parent
                        anchors.margins: Theme.sp(8)
                        spacing:         Theme.sp(2)
                        clip:            true
                        model:           root.searchResults

                        Text {
                            anchors.centerIn:    parent
                            visible:             resultsList.count === 0
                            text:                qsTr("No settings match \"%1\"").arg(root.searchQuery)
                            font.family:         Theme.fontData
                            font.pixelSize:      Theme.fontSzMicro
                            color:               Theme.colorText3
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode:            Text.WordWrap
                            width:               parent.width
                        }

                        delegate: Rectangle {
                            required property var modelData
                            required property int index

                            width:         resultsList.width
                            implicitHeight: resultContent.implicitHeight + Theme.sp(16)
                            color:          resultMouse.containsMouse ? Theme.colorAccentLight : "transparent"
                            radius:         Theme.radius
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                            ColumnLayout {
                                id: resultContent
                                anchors {
                                    left:  parent.left;  right: parent.right
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: Theme.sp(10); rightMargin: Theme.sp(10)
                                }
                                spacing: Theme.sp(2)

                                Text {
                                    text:                modelData.panelLabel + "  →  " + modelData.groupLabel
                                    font.family:         Theme.fontData
                                    font.pixelSize:      Theme.fontSzMicro
                                    font.letterSpacing:  Theme.trackingMicro
                                    color:               Theme.colorText3
                                    elide:               Text.ElideRight
                                    Layout.fillWidth:    true
                                }

                                Text {
                                    text:             modelData.label
                                    font.family:      Theme.fontBody
                                    font.pixelSize:   Theme.fontSzBody2
                                    color:            Theme.colorText
                                    elide:            Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                Text {
                                    text:             modelData.subtitle
                                    font.family:      Theme.fontData
                                    font.pixelSize:   Theme.fontSzMicro
                                    color:            Theme.colorText3
                                    elide:            Text.ElideRight
                                    Layout.fillWidth: true
                                    visible:          modelData.subtitle.length > 0
                                }
                            }

                            PpPressable {
                                id:         resultMouse
                                hoverScale: 1.0
                                onClicked:  root.navigateToResult(modelData)
                            }
                        }
                    }
                }

                // ── The fold control ─────────────────────────────────────────
                // At the sidenav's own foot, because it is the sidenav's state. Putting it on the
                // panel would make every panel carry a control for somebody else's chrome.
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.sp(34)

                    Rectangle {
                        anchors { left: parent.left; right: parent.right; top: parent.top }
                        height:  1
                        color:   Theme.colorBorderMid
                        opacity: Theme.borderOpacityNormal
                    }

                    Rectangle {
                        id: foldButton
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left:           parent.left
                        anchors.leftMargin:     root.navCollapsed
                                                    ? (parent.width - width) / 2 : Theme.sp(10)
                        width:  Theme.sp(26)
                        height: Theme.sp(22)
                        radius: Theme.radius
                        color:  foldMa.containsMouse ? Theme.colorBg2 : "transparent"
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                        Text {
                            anchors.centerIn: parent
                            text: root.navCollapsed ? "››" : "‹‹"
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzBody2
                            color: foldMa.containsMouse ? Theme.colorText2 : Theme.colorText3
                        }

                        ToolTip.visible: foldMa.containsMouse
                        ToolTip.text: (root.navCollapsed ? qsTr("Show the settings list")
                                                         : qsTr("Collapse the settings list"))
                                      + (Qt.platform.os === "osx" ? "  ⌘\\" : "  Ctrl+\\")
                        ToolTip.delay: 400

                        PpPressable {
                            id: foldMa
                            hoverScale: 1.0
                            onClicked: appSettings.settingsNavCollapsed = !appSettings.settingsNavCollapsed
                        }
                    }
                }
            }
        }

        // ── Content area ─────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            color: Theme.colorBg

            StackLayout {
                anchors.fill:  parent
                currentIndex:  root.activeNavIndex

                GeneralPanel {    id: generalPanel;    hostVisible: root.visible; Layout.fillWidth: true; Layout.fillHeight: true }  // 0
                AppearancePanel { id: appearancePanel; Layout.fillWidth: true; Layout.fillHeight: true }  // 1
                DisplaysPanel {   id: displaysPanel;   Layout.fillWidth: true; Layout.fillHeight: true }  // 2
                CamerasPanel {    id: camerasPanel;    Layout.fillWidth: true; Layout.fillHeight: true }  // 3
                ImusPanel {       id: imusPanel;       Layout.fillWidth: true; Layout.fillHeight: true }  // 4
                MicrophonesPanel { id: microphonesPanel; hostVisible: root.visible; Layout.fillWidth: true; Layout.fillHeight: true }  // 5
                LaunchMonitorPanel { id: launchMonitorPanel; Layout.fillWidth: true; Layout.fillHeight: true } // 6
                StoragePanel {    id: storagePanel;    Layout.fillWidth: true; Layout.fillHeight: true }  // 7
                ScreenPlaceholder { titleText: "Archiving" }                                               // 8
                DiagnosticModel { id: diagnosticModelPanel; Layout.fillWidth: true; Layout.fillHeight: true }       // 9
            }
        }
    }
}
