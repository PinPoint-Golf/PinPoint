import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Basic

Item {

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: "Camera"
            color: "#cdd6f4"
            font.pixelSize: 20
            font.bold: true
        }

        // ── Camera frame ─────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#181825"
            radius: 6

            Image {
                id: cameraFrame
                anchors.fill: parent
                anchors.margins: 2
                // Appending frameId to the URL forces QML to re-request the
                // image from the provider on every new frame.
                source: videoController.isRecording
                        ? "image://videoframe/frame?t=" + videoController.frameId
                        : ""
                cache: false
                asynchronous: true
                fillMode: Image.PreserveAspectFit
            }

            Label {
                anchors.centerIn: parent
                visible: !videoController.isRecording
                text: qsTr("No camera feed")
                color: "#6c7086"
                font.pixelSize: 14
            }
        }

        // ── Controls ─────────────────────────────────────────────────────────
        RowLayout {
            spacing: 8

            Button {
                id: startButton
                text: qsTr("Start ●")
                enabled: !videoController.isRecording
                onClicked: videoController.startRecording()
                contentItem: Text {
                    text: startButton.text
                    color: startButton.enabled ? "#1e1e2e" : "#6c7086"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: startButton.enabled
                           ? (startButton.pressed ? "#a6e3a1" : "#40a02b")
                           : "#313244"
                    radius: 6
                }
            }

            Button {
                id: stopButton
                text: qsTr("Stop ■")
                enabled: videoController.isRecording
                onClicked: videoController.stopRecording()
                contentItem: Text {
                    text: stopButton.text
                    color: stopButton.enabled ? "#1e1e2e" : "#6c7086"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: stopButton.enabled
                           ? (stopButton.pressed ? "#f38ba8" : "#e64553")
                           : "#313244"
                    radius: 6
                }
            }

            Rectangle {
                visible: videoController.isRecording
                width: 10; height: 10; radius: 5
                color: "#f38ba8"
                SequentialAnimation on opacity {
                    running: videoController.isRecording
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.2; duration: 900; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1.0; duration: 900; easing.type: Easing.InOutSine }
                }
            }

            Label {
                visible: videoController.isRecording
                text: qsTr("Recording…")
                color: "#f38ba8"
                font.pixelSize: 13
            }
        }
    }
}
