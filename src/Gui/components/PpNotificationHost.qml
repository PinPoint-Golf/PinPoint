// The ONE window-level notification surface, rendering NotificationCenter.
//
// ⛔ WHY THIS IS A MODEL AND NOT FOUR PpToasts.  Until this component the four
// shot-pipeline notices were four PpToast instances, each wired to its own
// signal and each positioned by hand relative to `saveErrorToast.y` — three of
// them computing the SAME y, so any two visible at once overlapped. Worse,
// PpToast.show() sets the text and restarts a hide timer, so ten consecutive
// failures were one toast showing the tenth message. Identity, counting,
// cause-suppression and latching all live in C++ now (see notification_center.h);
// this file only draws what the model says is live.
//
// PpToast itself is untouched and still serves the five panel-local imperative
// confirmations (copy-to-clipboard, an export result) — those are not this
// problem, per design §7.4.

import QtQuick
import PinPointStudio

ListView {
    id: root

    // Severity enum, mirrored from NotificationCenter::Severity.
    readonly property int sevInfo:     0
    readonly property int sevWarn:     1
    readonly property int sevError:    2
    readonly property int sevProgress: 3
    // Kind enum, mirrored from NotificationCenter::Kind.
    readonly property int kindCondition: 1

    model: notifications

    // Index 0 — the first thing raised — keeps the bottom anchor position the
    // old saveErrorToast had, and later news piles above it.
    verticalLayoutDirection: ListView.BottomToTop
    spacing: Theme.sp(10)
    interactive: false
    height: Math.min(contentHeight, parent ? parent.height * 0.6 : contentHeight)
    implicitWidth: Theme.sp(560)

    delegate: Item {
        id: row
        width: root.width
        height: card.implicitHeight

        required property string notificationId
        required property int    severity
        required property int    kind
        required property string title
        required property string detail
        required property int    count
        required property string glyph
        required property string actionLabel

        readonly property color _severityColor:
              severity === root.sevError ? Theme.colorError
            : severity === root.sevWarn  ? Theme.colorWarn
            :                              Theme.colorBorderStrong

        readonly property string _glyph:
            glyph.length > 0 ? glyph
          : severity === root.sevError    ? "⚠"
          : severity === root.sevWarn     ? "⚠"
          : severity === root.sevProgress ? "⋯"
          :                                 "◎"

        Rectangle {
            id: card
            anchors.horizontalCenter: parent.horizontalCenter
            width: content.implicitWidth + Theme.sp(30)
            implicitHeight: Math.max(Theme.sp(40), content.implicitHeight + Theme.sp(16))
            radius: Theme.radiusLg
            color:  Theme.colorBg3
            border.width: 1
            border.color: row._severityColor

            Row {
                id: content
                anchors.centerIn: parent
                spacing: Theme.sp(14)

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text:           row._glyph
                    font.family:    Theme.fontSymbol
                    font.pixelSize: Theme.fontSzBody
                    color:          row.severity === root.sevInfo ? Theme.colorText2
                                                                  : row._severityColor
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.sp(2)
                    Text {
                        // ⭐ The count is the diagnostically valuable part, and
                        // the old shape lost it entirely.
                        text: row.count > 1 ? qsTr("%1 (×%2)").arg(row.title).arg(row.count)
                                            : row.title
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        visible:        row.detail.length > 0
                        text:           row.detail
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color:          Theme.colorText2
                        elide:          Text.ElideMiddle
                        width:          Math.min(implicitWidth, Theme.sp(420))
                    }
                }

                Rectangle {   // hairline before the action
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1; height: Theme.sp(18)
                    color: Theme.colorBorderStrong
                    visible: row.actionLabel.length > 0
                }

                Text {        // the fix-this affordance — in-app navigation only
                    anchors.verticalCenter: parent.verticalCenter
                    visible:        row.actionLabel.length > 0
                    text:           row.actionLabel
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzBody2
                    font.letterSpacing: Theme.trackingLabel
                    color:          Theme.colorAccent
                    PpPressable {
                        anchors.margins: -Theme.sp(6)
                        onClicked: notifications.invokeAction(row.notificationId)
                    }
                }

                Rectangle {   // hairline before dismiss
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1; height: Theme.sp(18)
                    color: Theme.colorBorderStrong
                    // A condition does not auto-hide, so it needs a way out.
                    visible: row.kind === root.kindCondition
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible:        row.kind === root.kindCondition
                    text:           "✕"
                    font.family:    Theme.fontSymbol
                    font.pixelSize: Theme.fontSzBody
                    color:          Theme.colorText2
                    PpPressable {
                        anchors.margins: -Theme.sp(6)
                        onClicked: notifications.dismiss(row.notificationId)
                    }
                }
            }
        }
    }
}
