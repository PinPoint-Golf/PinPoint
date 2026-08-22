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
import QtQuick.Dialogs

// "Import session…" — a file picker and nothing else.
//
// Work package H3 is explicit that this is all the UI is: "a file transport
// that streams a PPCPBNDL through the same host-peer feed as a socket would …
// A 'Import session…' entry in the UI that DOES NOTHING MORE THAN PICK A FILE."
// Everything a wizard would offer — which streams, which shots, how to merge —
// is either the protocol's (the bundle says) or forbidden (CORE 8.5b: no
// auto-merge, and confirmation is a reconciliation UI, not an import one).
Item {
    id: root

    // Emitted whether or not the bundle landed; `ok` says which.
    signal finished(bool ok, string message)

    // ⚠ THE CONTROLLER IS A CONTEXT PROPERTY, NOT A REGISTERED QML TYPE, and
    // that is on purpose. libppcp is an OPTIONAL dependency of this application
    // (H0: a box with no sibling checkout and no fetch still builds), so a
    // `PpcpImportController {}` here would make the QML module fail to compile
    // on exactly the machines the optionality exists for. main.cpp installs
    // `ppcpImport` when the library is embedded and does not when it is not.
    readonly property bool available: typeof ppcpImport !== "undefined" && ppcpImport !== null

    // False until libppcp's peer engine lands, so a caller can say what will
    // happen before the user picks a file rather than after.
    readonly property bool ingestAvailable: available && ppcpImport.ingestAvailable
    readonly property string status: available ? ppcpImport.status : ""
    readonly property bool busy: available && ppcpImport.busy

    function open() { dialog.open() }

    Connections {
        target: root.available ? ppcpImport : null
        function onImportFinished(ok, message) { root.finished(ok, message) }
    }

    FileDialog {
        id: dialog
        title: qsTr("Import session")
        // ENC §7: one container, one extension. There is no second format to
        // offer and no "detect type" step, which is the point of the bundle
        // being the session rather than an export of it.
        nameFilters: [ qsTr("PPCP session bundles (*.ppcpbndl)"), qsTr("All files (*)") ]
        onAccepted: {
            if (root.available) ppcpImport.importSession(selectedFile)
            else root.finished(false, qsTr("PPCP is not built into this application."))
        }
    }
}
