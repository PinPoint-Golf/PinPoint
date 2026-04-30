import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 12

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Capture")
            color: "#cdd6f4"
            font.pixelSize: 20
            font.bold: true
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Coming soon")
            color: "#6c7086"
            font.pixelSize: 14
        }
    }
}
