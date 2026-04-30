import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Basic

ApplicationWindow {
    id: root
    width: 800
    height: 700
    visible: true
    title: qsTr("PinPoint")
    color: "#1e1e2e"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabBar
            Layout.fillWidth: true
            background: Rectangle { color: "#181825" }

            TabButton {
                text: qsTr("Capture")
                contentItem: Text {
                    text: parent.text
                    color: parent.checked ? "#cdd6f4" : "#6c7086"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.checked ? "#1e1e2e" : "#181825"
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 2
                        color: parent.parent.checked ? "#89b4fa" : "transparent"
                    }
                }
            }

            TabButton {
                text: qsTr("Audio")
                contentItem: Text {
                    text: parent.text
                    color: parent.checked ? "#cdd6f4" : "#6c7086"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.checked ? "#1e1e2e" : "#181825"
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 2
                        color: parent.parent.checked ? "#89b4fa" : "transparent"
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            CapturePage {}
            AudioPage {}
        }
    }
}
