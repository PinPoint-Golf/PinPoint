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
import PinPointStudio

Item {
    id: root

    signal addAthleteRequested()
    signal athletePickerRequested()
    signal editCurrentAthleteRequested()
    signal startSessionRequested(int sessionTypeIndex)
    // Coming-soon types skip the session wizard and jump straight to their
    // (placeholder) rail screen.
    signal openSessionScreenRequested(int sessionTypeIndex)

    property int    selectedType: 1   // default to Wrist — the only startable type today
    // Session types not yet implemented: badged "coming soon" tiles that open
    // their placeholder screen instead of starting a session.
    readonly property var comingSoonTypes: [0, 2, 3]
    // EMPTY, not a club. This is carried into SessionController.activeClub at session
    // start, and start() only seeds from the athlete's preferred club when activeClub is
    // empty — so a literal here becomes the session's club and silently outranks the
    // preference whenever onAthMapChanged hasn't resolved one (no current athlete, or an
    // empty bag, which also hides the CLUB row so it can't be corrected).
    property string selectedClub: ""

    property var athMap: {
        if (!athleteController.hasCurrentAthlete) return {}
        var list = athleteController.athletes
        for (var i = 0; i < list.length; i++) {
            if (list[i].uuid === athleteController.currentUuid) return list[i]
        }
        return {}
    }

    // The current athlete's bag (canonical ids, vocabulary order). Empty when
    // there's no athlete or an emptied bag — the CLUB row hides in that case.
    // Re-evaluates on every club commit via athletesChanged.
    readonly property var clubModel: {
        void athleteController.athletes
        if (!athleteController.hasCurrentAthlete) return []
        var bag = athleteController.clubsFor(athleteController.currentUuid)
        var vocab = athleteController.clubOptions()
        var ids = Object.keys(bag)
        ids.sort(function (a, b) { return vocab.indexOf(a) - vocab.indexOf(b) })
        return ids
    }

    // Default the club to the athlete's preferred club (resolved to a real bag
    // club) whenever the athlete or their bag changes.
    onAthMapChanged: {
        if (!athleteController.hasCurrentAthlete) return
        var pref = athleteController.effectivePrimaryClub(athleteController.currentUuid)
        if (pref !== "") root.selectedClub = pref
    }

    // The pairing panel.  A Popup parented to Overlay.overlay, so it covers the
    // whole window from here without Main.qml having to host it.
    PpcpPairDialog {
        id: pairDialog
    }

    // H10 — RV-6 guided pairing (PPCP-RV §11): a first pairing with no code
    // carried between two screens, authenticated by six digits compared on
    // both.  A separate dialogue from PpcpPairDialog rather than a mode of it,
    // deliberately: that one exists to show a code, and §11's screen must not
    // acquire a control that shows one.  The two paths meet only where 11.9d
    // sends a failed guided attempt to the code — which is an offer of a
    // DIFFERENT path and never a retry (11.9c).
    PpcpGuidedPairDialog {
        id: guidedDialog
        onPairingCodeRequested: pairDialog.open()
    }

    Component.onCompleted: resourceMonitor.refresh()

    Timer {
        interval: 2000
        running:  root.visible
        repeat:   true
        onTriggered: resourceMonitor.refresh()
    }

    Rectangle { anchors.fill: parent; color: Theme.colorBg }

    // Ambient theme-reactive contour field over the solid background. Subtle by
    // default; pauses when off-screen / backgrounded; respects reduced-motion.
    // Hover lifts the terrain under the cursor; click/tap sends a ripple.
    PpTopoBackground {
        id: topo
        anchors.fill: parent
        colorLow:  Theme.gradientWarm
        colorMid:  Theme.gradientWarmLit
        colorHigh: Theme.gradientCool
        accentColor: Theme.colorAccent
        animated:  !appSettings.reduceMotion
        hoverPoint: topoHover.hovered
            ? Qt.point(topoHover.point.position.x / width,
                       topoHover.point.position.y / height)
            : Qt.point(-1, -1)
    }

    Flickable {
        anchors.fill:  parent
        contentWidth:  width
        contentHeight: mainCol.implicitHeight + 80
        clip:          true

        // Pointer handlers live on the Flickable, not the screen root: the
        // full-bleed Flickable sits above the fixed background and intercepts
        // presses for flick-detection, so root-level handlers never see a
        // completed tap (the holding screens have no Flickable, so root works
        // there). Hosted here they share the viewport coordinate space the
        // background uses. HoverHandler never blocks; TapHandler's DragThreshold
        // keeps only a passive grab, so cards/buttons still get their clicks and
        // a vertical drag flicks the list instead of spawning a ripple.
        HoverHandler {
            id: topoHover
            enabled: topo.animated
        }
        TapHandler {
            enabled: topo.animated
            gesturePolicy: TapHandler.DragThreshold
            onTapped: (ep) => topo.ripple(ep.position.x / topo.width,
                                          ep.position.y / topo.height)
        }

        Column {
            id: mainCol
            anchors.horizontalCenter: parent.horizontalCenter
            width:   Theme.contentWidth(parent.width)
            spacing: 0

            Item { width: 1; height: Theme.sp(48) }

            // ── Section 1: Athlete identity ──────────────────────────────────

            // No athlete state
            Column {
                visible: !athleteController.hasCurrentAthlete
                height:  visible ? implicitHeight : 0
                width:   parent.width
                spacing: 0

                Text {
                    text:               qsTr("GOLF SWING ANALYSIS")
                    font.family:        Theme.fontData
                    font.pixelSize:     Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color:              Theme.colorText3
                }
                Item { width: 1; height: Theme.sp(12) }

                PpDisplayText {
                    width:          parent.width
                    text:           qsTr("Welcome to Pinpoint Studio")
                    wrapMode:       Text.WordWrap
                    lineHeight:     1.1
                }
                Item { width: 1; height: Theme.sp(10) }

                Text {
                    width:          parent.width
                    text:           qsTr("An open-source workshop for understanding the golf swing — cameras, IMUs, and ground forces working together to show you what's actually happening.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody
                    font.weight:    Theme.fontBodyWeight
                    color:          Theme.colorText2
                    wrapMode:       Text.WordWrap
                    lineHeight:     1.7
                }
                Item { width: 1; height: Theme.sp(32) }

                Text {
                    width:          parent.width
                    text:           qsTr("Start by adding an athlete")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody
                    color:          Theme.colorText
                }
                Item { width: 1; height: Theme.sp(4) }

                Text {
                    width:          parent.width
                    text:           qsTr("Every session belongs to someone. That's usually you.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    color:          Theme.colorText3
                }
                Item { width: 1; height: Theme.sp(16) }

                Rectangle {
                    width:  parent.width
                    height: Theme.sp(42)
                    radius: Theme.radius
                    color:  Theme.colorAccent

                    Text {
                        anchors.centerIn: parent
                        text:           qsTr("Add your first athlete")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.dark ? Theme.colorBg : "#FFFFFF"
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape:  Qt.PointingHandCursor
                        onClicked:    root.addAthleteRequested()
                    }
                }
                Item { width: 1; height: Theme.sp(48) }

                Row {
                    width:   parent.width
                    spacing: Theme.sp(10)

                    Repeater {
                        model: [
                            { icon: "⊞", title: qsTr("Connect a camera"), desc: qsTr("Basler or GenTL over USB3") },
                            { icon: "⌖", title: qsTr("Pair wrist IMUs"),   desc: qsTr("Lead and trail hand sensors") },
                            { icon: "↗", title: qsTr("Read the docs"),     desc: qsTr("Setup guides and hardware") }
                        ]

                        delegate: Rectangle {
                            required property var modelData
                            width:  (parent.width - 20) / 3
                            height: secCardCol.implicitHeight + 28
                            radius: Theme.radius
                            color:  "transparent"
                            border.width: 1
                            border.color: Theme.colorBorder

                            Column {
                                id: secCardCol
                                anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.sp(14) }
                                spacing: 0

                                Item { width: 1; height: Theme.sp(14) }
                                Text {
                                    text:           modelData.icon
                                    font.pixelSize: Theme.sp(16)
                                    color:          Theme.colorText2
                                }
                                Item { width: 1; height: 7 }
                                Text {
                                    width:          parent.width
                                    text:           modelData.title
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorText
                                }
                                Item { width: 1; height: 3 }
                                Text {
                                    width:          parent.width
                                    text:           modelData.desc
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    font.weight:    Theme.fontBodyWeight
                                    color:          Theme.colorText3
                                    wrapMode:       Text.WordWrap
                                    lineHeight:     1.4
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                onEntered:    parent.color = Theme.colorBg2
                                onExited:     parent.color = "transparent"
                            }
                        }
                    }
                }
                Item { width: 1; height: Theme.sp(24) }

                Text {
                    width:               parent.width
                    text:                qsTr("Nothing connects to the cloud unless you configure it.")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzBody2
                    font.weight:         Theme.fontBodyWeight
                    color:               Theme.colorText3
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            // Has athlete: compact identity row
            Item {
                visible: athleteController.hasCurrentAthlete
                height:  visible ? 52 : 0
                width:   parent.width

                Rectangle {
                    id: avatarCircle
                    width: Theme.sp(52); height: Theme.sp(52); radius: Theme.sp(26)
                    anchors.left:           parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    color:        Theme.colorAccentLight
                    border.width: 1
                    border.color: avatarPress.containsMouse ? Theme.colorAccent : Theme.colorAccentMid
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        anchors.centerIn: parent
                        text:           athleteController.currentInitials
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.sp(16)
                        color:          Theme.colorAccent
                    }

                    // Click the athlete avatar to edit the current athlete's profile.
                    PpPressable {
                        id:        avatarPress
                        onClicked: root.editCurrentAthleteRequested()
                    }
                }

                Column {
                    anchors.left:           avatarCircle.right
                    anchors.leftMargin:     16
                    anchors.right:          switchLink.left
                    anchors.rightMargin:    12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.sp(3)

                    Text {
                        width:          parent.width
                        text:           athleteController.currentName
                        font.family:    Theme.fontDisplay
                        font.italic:    Theme.fontDisplayItalic
                        font.weight: Theme.fontDisplayWeight
                        font.pixelSize: Math.min(Theme.fontSzDisplay, 24)
                        color:          Theme.colorText
                        elide:          Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: {
                            var m   = root.athMap
                            var hcp = Theme.formatHandicap(m.handicap)
                            var sc  = (m.sessionCount !== undefined)
                                          ? m.sessionCount + " sessions" : "0 sessions"
                            return athleteController.currentHandedness + " · " + hcp + " · " + sc
                        }
                        font.family:        Theme.fontData
                        font.pixelSize:     Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingData
                        color:              Theme.colorText3
                        elide:              Text.ElideRight
                    }
                }

                Text {
                    id: switchLink
                    anchors.right:          parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text:           qsTr("Switch →")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorAccent

                    MouseArea {
                        anchors.fill: parent
                        cursorShape:  Qt.PointingHandCursor
                        onClicked:    root.athletePickerRequested()
                    }
                }
            }

            // ── Section 2: Session launcher ──────────────────────────────────
            Column {
                visible: athleteController.hasCurrentAthlete
                height:  visible ? implicitHeight : 0
                width:   parent.width
                spacing: 0

                Item { width: 1; height: Theme.sp(40) }

                // Banner — title font at double the largest theme title size.
                PpDisplayText {
                    text:           qsTr("PinPoint Studio")
                    pixelSize:      Theme.fontSzDisplay * 2
                }
                Item { width: 1; height: Theme.sp(20) }

                Text {
                    text:               qsTr("NEW SESSION")
                    font.family:        Theme.fontData
                    font.pixelSize:     Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color:              Theme.colorText3
                }
                Item { width: 1; height: Theme.sp(16) }

                Row {
                    spacing: Theme.sp(20)
                    width:   parent.width

                    Repeater {
                        model: [
                            { icon: "◑", name: qsTr("Swing analysis"), desc: qsTr("Capture golf shots with IMUs on your spine and review your sequencing and key swing metrics to assess your swing"), img: "qrc:/assets/tiles/golfswing.png", reqCameras: 2, reqImus: 3, idx: 0, comingSoon: true },
                            { icon: "⌖", name: qsTr("Wrist motion"),   desc: qsTr("Hit shots with IMUs on your lead wrist and hand to assess how your wrist angles impact club delivery"),              img: "qrc:/assets/tiles/grip.png",      reqCameras: 1, reqImus: 2, idx: 1 },
                            { icon: "⇅", name: qsTr("Ground forces"),  desc: qsTr("Hit shots with IMUs on your hips to assess how you use the ground to generate power"),                              img: "qrc:/assets/tiles/feet.png",      reqCameras: 2, reqImus: 3, idx: 2, comingSoon: true },
                            { icon: "✦", name: qsTr("AI coach"),       desc: qsTr("Work with an AI coach to hit shots and get feedback on your swing and how to improve"),                             img: "qrc:/assets/tiles/coach.png",     reqCameras: 2, reqImus: 3, idx: 3, comingSoon: true }
                        ]

                        delegate: HmTypeCard {
                            required property var modelData
                            width:           (parent.width - 3 * Theme.sp(20)) / 4
                            height:          width * 1.6
                            imageSource:     modelData.img
                            iconText:        modelData.icon
                            typeName:        modelData.name
                            description:     modelData.desc
                            camerasRequired: modelData.reqCameras
                            camerasOptional: modelData.optCameras || false
                            imusRequired:    modelData.reqImus
                            camerasCount:    cameraManager.cameraList.length
                            imusCount:       imuManager.imuEnumeratedCount
                            comingSoon:      modelData.comingSoon || false
                            isSelected:      root.selectedType === modelData.idx
                            onClicked:       root.selectedType = modelData.idx
                            onDoubleClicked: {
                                root.selectedType = modelData.idx
                                if (root.comingSoonTypes.indexOf(modelData.idx) !== -1)
                                    root.openSessionScreenRequested(modelData.idx)
                                else
                                    root.startSessionRequested(modelData.idx)
                            }
                        }
                    }
                }
                Item { width: 1; height: Theme.sp(20) }

                // Club selector: the current athlete's bag. A wrapping Flow of chips
                // (not PpChipGroup, a non-wrapping Row) so a full bag reflows —
                // same chip language as the Clubs-section tab strip.
                Column {
                    width:   parent.width
                    spacing: Theme.sp(8)
                    visible: root.clubModel.length > 0

                    Text {
                        text:               qsTr("CLUB")
                        font.family:        Theme.fontData
                        font.pixelSize:     Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color:              Theme.colorText3
                    }

                    Flow {
                        width:   parent.width
                        spacing: Theme.sp(6)

                        Repeater {
                            model: root.clubModel

                            Rectangle {
                                required property string modelData

                                readonly property bool _sel: modelData === root.selectedClub

                                height:  Theme.sp(28)
                                width:   clubChipLabel.implicitWidth + Theme.sp(24)
                                radius:  Theme.radius
                                color:   _sel                    ? Theme.colorAccentLight
                                       : clubChipMa.containsMouse ? Theme.colorBg2
                                       :                            Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                                border.width: 1
                                border.color: _sel                    ? Theme.colorAccent
                                            : clubChipMa.containsMouse ? Theme.colorAccentMid
                                            :                            Theme.colorBorderStrong
                                Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                                Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                Text {
                                    id: clubChipLabel
                                    anchors.centerIn: parent
                                    text:           ClubFormat.display(modelData)
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody
                                    font.weight:    _sel ? Font.Normal : Theme.fontBodyWeight
                                    color:          _sel ? Theme.colorAccent : Theme.colorText2
                                }

                                PpPressable {
                                    id: clubChipMa
                                    onClicked: root.selectedClub = modelData
                                }
                            }
                        }
                    }
                }
                Item { width: 1; height: Theme.sp(20); visible: root.clubModel.length > 0 }

                Rectangle {
                    width:  parent.width
                    height: Theme.sp(44)
                    radius: Theme.radius
                    // Filled CTA: brighten on hover, dip on press. No hover grow —
                    // it's full-width, so scaling up would bulge past the content
                    // margins; the PpPressable press-dip (scale DOWN) stays in bounds.
                    color:  startMa.containsMouse ? Qt.lighter(Theme.colorAccent, 1.08) : Theme.colorAccent
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        anchors.centerIn: parent
                        text:           qsTr("▶  Start session")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.sp(14)
                        font.weight:    Font.Normal
                        color:          Theme.dark ? Theme.colorBg : "#FFFFFF"
                    }

                    PpPressable {
                        id: startMa
                        hoverScale: 1.0   // full-width: brighten on hover, dip on press only
                        onClicked:    if (root.comingSoonTypes.indexOf(root.selectedType) === -1)
                                          root.startSessionRequested(root.selectedType)
                                      else
                                          root.openSessionScreenRequested(root.selectedType)
                    }
                }
            }

            // ── Section 3: Device readiness ──────────────────────────────────
            Item { width: 1; height: Theme.sp(44) }

            Item {
                width:  parent.width
                height: Theme.sp(34)

                Text {
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    text:               qsTr("DEVICES")
                    font.family:        Theme.fontData
                    font.pixelSize:     Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color:              Theme.colorText3
                }

                // ── Pairing, PPCP-RV §4 ────────────────────────────────────
                //
                // The whole of it, on this screen.  What used to be here — a
                // "PAIR A DEVICE" heading, a status line, a paragraph, a raw
                // IP:port list, two buttons and a table of 16-hex pairing
                // handles — was visible on every launch whether or not anybody
                // had a phone, because the panel keyed off the listener being
                // up and the listener is always up.  It is now one button, and
                // it opens PpcpPairDialog.
                //
                // Beside DEVICES because that is where the result appears: pair
                // a phone, the phone's cameras join the list below.  Hidden
                // entirely on a build without libppcp and OpenSSL, where there
                // is no `ppcpHost` context property at all (H0 — a build with
                // no PPCP must still be a working application).
                PpButton {
                    id: pairButton
                    objectName: "pairPhoneButton"
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    visible: (typeof ppcpHost !== "undefined") && ppcpHost !== null
                             && ppcpHost.listening
                    label:   qsTr("Pair to my phone")
                    // Held while the dialog it owns is open — the toolbar-pill
                    // convention, so the button stays visibly the source of the
                    // thing covering the screen.
                    onClicked: pairDialog.open()

                    // Drawn, not a glyph: PpButton.glyph is a character in
                    // Theme.fontSymbol, and neither Apple Symbols nor Segoe UI
                    // Symbol has a QR or a phone.  PpButton's `icon` slot hands
                    // it contentColor, so it tracks the theme with the label.
                    icon: Component { PpQrGlyph {} }
                }

                // H10 — the guided path, beside the code path and never
                // instead of it.  §4's code is the REQUIRED path (2a) and
                // stays the primary button; this one appears only where the
                // platform can browse at all, because RV §3 is optional and
                // 3.6b makes its absence silent (no DNS-SD client, no windows
                // to find, and the user sees the code prompt as before).
                PpButton {
                    id: guidedPairButton
                    objectName: "guidedPairButton"
                    anchors { right: pairButton.left
                              rightMargin: Theme.sp(8)
                              verticalCenter: parent.verticalCenter }
                    visible: (typeof ppcpHost !== "undefined") && ppcpHost !== null
                             && ppcpHost.listening && ppcpHost.guidedAvailable
                    label:   qsTr("Pair without a code")
                    onClicked: guidedDialog.open()
                }
            }
            Item { width: 1; height: 12 }

            // Empty devices state
            Item {
                visible: resourceMonitor.devices.length === 0
                height:  visible ? 40 : 0
                width:   parent.width

                Text {
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    text:           qsTr("No devices detected. Connect a camera or IMU to get started.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    color:          Theme.colorText3
                }
            }

            // Device rows — inline, no card chrome
            Repeater {
                model: resourceMonitor.devices

                Item {
                    required property var modelData
                    property var d: modelData
                    width:  mainCol.width
                    height: Theme.sp(40)

                    Rectangle {
                        anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
                        height: 1
                        color:  Theme.colorBorder
                    }

                    Row {
                        anchors.fill: parent
                        spacing:      0

                        Item {
                            id: dotItem
                            property var d: parent.parent.d
                            width:  Theme.sp(20)
                            height: parent.height

                            Rectangle {
                                width: Theme.sp(6); height: Theme.sp(6); radius: Theme.sp(3)
                                anchors.centerIn: parent
                                color: {
                                    var d = dotItem.d
                                    if (d.status === "streaming" || d.status === "connected") return Theme.colorGood
                                    if (d.hasWarning || d.status === "stalled") return Theme.colorWarn
                                    return Theme.colorBorderStrong
                                }
                            }
                        }

                        Text {
                            width:             parent.width - 20 - 80
                            height:            parent.height
                            text:              parent.parent.d.name
                            font.family:       Theme.fontBody
                            font.pixelSize:    Theme.fontSzBody
                            color:             Theme.colorText
                            elide:             Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }

                        Text {
                            width:  Theme.sp(80)
                            height: parent.height
                            text: {
                                var d = parent.parent.d
                                if (d.kind === "Camera")
                                    return d.status === "streaming" ? d.dataRateHz.toFixed(0) + " fps" : qsTr("idle")
                                return d.status === "connected" ? d.dataRateHz.toFixed(0) + " Hz" : qsTr("disconnected")
                            }
                            font.family:         Theme.fontData
                            font.pixelSize:      Theme.fontSzMicro
                            font.letterSpacing:  Theme.trackingData
                            color: {
                                var d = parent.parent.d
                                return (d.status === "streaming" || d.status === "connected")
                                           ? Theme.colorGood : Theme.colorText3
                            }
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment:   Text.AlignVCenter
                        }
                    }
                }
            }

            // ── The sessions a connected capture device is offering ─────────
            //
            // In the DEVICES area on purpose: an offer is a fact about a
            // connected device, and it belongs beside the device it came from.
            // Unlike the pairing panel that used to sit above it, this one is
            // already silent when it has nothing to say — `visible: offerCount
            // > 0` — so it is not part of what the home screen was cluttered
            // with, and it is the only route to a device's recorded sessions.
            // On a build without libppcp there is no controller and it never
            // appears (H0).
            PpcpOfferList {
                width: mainCol.width
            }

            // Warning notice (first warning only)
            Item {
                visible: resourceMonitor.warnings.length > 0
                height:  visible ? (warnNotice.implicitHeight + 12) : 0
                width:   parent.width

                RmWarningNotice {
                    id: warnNotice
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    message: resourceMonitor.warnings.length > 0 ? resourceMonitor.warnings[0] : ""
                }
            }

            Item { width: 1; height: 60 }
        }
    }
}
