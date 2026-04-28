import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 800
    height: 600
    visible: true
    title: qsTr("PinPoint – Speech to Text")
    color: "#1e1e2e"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: "Live Transcript"
            color: "#cdd6f4"
            font.pixelSize: 20
            font.bold: true
        }

        ScrollView {
            id: scrollView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: transcriptArea
                readOnly: true
                text: controller.transcript
                wrapMode: TextArea.Wrap
                color: "#cdd6f4"
                font.pixelSize: 14
                font.family: "Menlo, Monaco, monospace"
                padding: 12
                background: Rectangle {
                    color: "#313244"
                    radius: 6
                }
                onTextChanged: scrollView.ScrollBar.vertical.position =
                    Math.max(0, 1.0 - scrollView.ScrollBar.vertical.size)
            }
        }

        RowLayout {
            spacing: 8

            Rectangle {
                width: 10; height: 10; radius: 5
                color: "#a6e3a1"
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.2; duration: 900; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1.0; duration: 900; easing.type: Easing.InOutSine }
                }
            }

            Label {
                text: "Listening…"
                color: "#a6e3a1"
                font.pixelSize: 13
            }
        }
    }
}
