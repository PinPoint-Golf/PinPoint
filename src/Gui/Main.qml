import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Basic

ApplicationWindow {
    id: root
    width: 800
    height: 700
    visible: true
    title: qsTr("PinPoint – Speech to Text")
    color: "#1e1e2e"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // ── Live transcript ──────────────────────────────────────────────────
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

        // ── Divider ─────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#45475a"
        }

        // ── Text-to-speech panel ─────────────────────────────────────────────
        Label {
            text: "Text to Speech"
            color: "#cdd6f4"
            font.pixelSize: 16
            font.bold: true
        }

        TextArea {
            id: ttsInput
            Layout.fillWidth: true
            implicitHeight: 80
            placeholderText: qsTr("Type text to speak…")
            placeholderTextColor: "#6c7086"
            wrapMode: TextArea.Wrap
            color: "#cdd6f4"
            font.pixelSize: 14
            padding: 10
            background: Rectangle {
                color: "#313244"
                radius: 6
                border.color: ttsInput.activeFocus ? "#89b4fa" : "transparent"
                border.width: 1
            }
            Keys.onPressed: function(event) {
                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        && (event.modifiers & Qt.ControlModifier)) {
                    ttsController.speak(ttsInput.text)
                    event.accepted = true
                }
            }
        }

        RowLayout {
            spacing: 8

            Button {
                id: speakButton
                text: ttsController.ttsActive ? qsTr("Speaking…") : qsTr("Speak ▶")
                enabled: ttsController.ttsReady && !ttsController.ttsActive
                         && ttsInput.text.trim().length > 0
                onClicked: ttsController.speak(ttsInput.text)
                contentItem: Text {
                    text: speakButton.text
                    color: speakButton.enabled ? "#1e1e2e" : "#6c7086"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: speakButton.enabled
                           ? (speakButton.pressed ? "#74c7ec" : "#89b4fa")
                           : "#313244"
                    radius: 6
                }
            }

            Button {
                id: stopButton
                text: qsTr("Stop ■")
                enabled: ttsController.ttsActive
                onClicked: ttsController.stopSpeaking()
                contentItem: Text {
                    text: stopButton.text
                    color: stopButton.enabled ? "#1e1e2e" : "#6c7086"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: stopButton.enabled
                           ? (stopButton.pressed ? "#f38ba8" : "#fab387")
                           : "#313244"
                    radius: 6
                }
            }

            Label {
                text: qsTr("Voice:")
                color: "#cdd6f4"
                font.pixelSize: 13
            }

            ComboBox {
                id: voiceSelector
                model: ttsController.voices
                currentIndex: ttsController.voices.indexOf(ttsController.voice)
                onActivated: ttsController.voice = currentText
                implicitWidth: 130
                contentItem: Text {
                    leftPadding: 8
                    text: voiceSelector.displayText
                    color: "#cdd6f4"
                    font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: "#313244"
                    radius: 6
                    border.color: voiceSelector.activeFocus ? "#89b4fa" : "transparent"
                    border.width: 1
                }
                popup.background: Rectangle {
                    color: "#313244"
                    radius: 6
                }
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                width: 8; height: 8; radius: 4
                color: ttsController.ttsReady ? "#a6e3a1" : "#f38ba8"
                ToolTip.visible: ttsStatusHover.containsMouse
                ToolTip.text: ttsController.ttsReady ? qsTr("TTS model ready")
                                                      : qsTr("TTS model not loaded")
                HoverHandler { id: ttsStatusHover }
            }

            Label {
                text: ttsController.ttsReady    ? qsTr("Ready")
                    : ttsController.downloading ? qsTr("Downloading…")
                    :                             qsTr("Loading…")
                color: ttsController.ttsReady ? "#a6e3a1" : "#f9e2af"
                font.pixelSize: 12
            }
        }

        // Download progress — only visible while fetching model files
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: ttsController.downloading

            ProgressBar {
                Layout.fillWidth: true
                value: ttsController.downloadProgress
                background: Rectangle { color: "#313244"; radius: 3 }
                contentItem: Item {
                    Rectangle {
                        width: parent.width * ttsController.downloadProgress
                        height: parent.height
                        radius: 3
                        color: "#89b4fa"
                        Behavior on width { NumberAnimation { duration: 150 } }
                    }
                }
            }

            Label {
                text: ttsController.downloadStatus
                color: "#a6adc8"
                font.pixelSize: 11
            }
        }
    }
}
