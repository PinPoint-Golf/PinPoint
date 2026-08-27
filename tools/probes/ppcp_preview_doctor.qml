// Preview doctor — "where does the preview stop?", answered in order.
//
//   PinPointStudio --probe-qml /abs/path/tools/probes/ppcp_preview_doctor.qml
//
// Run it over the REAL UI (not offscreen) so Settings -> Cameras is still
// usable underneath. Open "Set crop" on the phone's camera row and read the
// panel: the first field that is zero when it should not be is the break.
//
//   channels  2 = ENC 2.1d's preview channel never arrived. Preview payload has
//               no route, whatever else is right. 3 = it is open.
//   preview   the Stream id this instance is reading. Empty = never opened, or
//               opened and then refused/closed — lastStreamError says which.
//   capt      preview capture_announce received. 0 with channels=3 means the
//               phone is not producing, or is not announcing to us.
//   frames    videoFrameReady emitted. capt>0 and frames=0 is payload or decode.
//   decodeF   the payload arrived and would not decode — a codec mismatch, not
//               a plumbing one.
import QtQuick

Item {
    anchors.fill: parent

    Rectangle {
        anchors { right: parent.right; top: parent.top; margins: 8 }
        width: 520; height: txt.implicitHeight + 18
        color: "#ee0f1216"; border.color: "#3aa0ff"; radius: 6
        Text {
            id: txt; anchors.centerIn: parent; width: 500
            wrapMode: Text.WordWrap; color: "#e6f0ff"
            font.family: "Menlo"; font.pixelSize: 11
            text: "PPCP DOCTOR — waiting for a phone…"
        }
    }

    function describe() {
        var s = ppcpHost.ppcpStats()
        if (!s.phones) return "no phone connected (listening=" + s.listening
                             + " port=" + s.port + ")"
        var p = s.perPhone[0]
        var head = p.name + "  session=" + p.sessionOpen
                 + "  channels=" + p.channels + (p.channels < 3 ? "  ⛔ NO PREVIEW CHANNEL" : "")
                 + "\narbiter=" + p.arbiter
                 + "  observed=" + s.observedForeign
                 + "  unarbitrated=" + s.unarbitrated

        if (!p.inputs || p.inputs.length === 0)
            return head + "\n⛔ no live VideoInputPpcp — nothing is reading this "
                        + "phone's camera.\n   Open \"Set crop\" on its row."

        var body = ""
        for (var i = 0; i < p.inputs.length; ++i) {
            var v = p.inputs[i]
            body += "\n" + v.sourceId
                 + (v.attached ? "" : "  ⛔ DETACHED")
                 + "\n  preview=" + (v.previewStream === "" ? "⛔ none" : "open")
                 + "  capt=" + v.previewCaptures
                 + "  frames=" + v.previewFrames
                 + "  foreign=" + v.foreignStream
                 + "\n  opened=" + v.streamsOpened
                 + "  refused=" + v.streamsRefused
                 + "  closedByOwner=" + v.closedByOwner
                 + "  decodeF=" + v.decodeFailures
                 + "  absent=" + v.absentSegments
            if (v.lastStreamError !== "") body += "\n  last error: " + v.lastStreamError
        }
        return head + body
    }

    Timer {
        interval: 1000; running: true; repeat: true
        onTriggered: {
            var d = "PPCP DOCTOR\n" + describe()
            txt.text = d
            console.warn(d.replace(/\n/g, " | "))
        }
    }
    Component.onCompleted: console.warn("PPCP DOCTOR started")
}
