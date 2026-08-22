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
import Qt.labs.platform as Labs

// macOS ONLY. Loaded by Main.qml exclusively on macOS via a Loader — this file's
// Qt.labs.platform import and the native menu bar it installs must never be
// instantiated on Linux/Windows, where the header version pill is the About trigger.
//
// Provides the standard macOS application-menu items. Items carry native roles, so
// Qt relocates them into the bold application (app-name) menu automatically:
//   • AboutRole → "About PinPoint Studio" → emits aboutRequested()
//   • QuitRole  → "Quit PinPoint Studio"  → emits quitRequested() (Main.qml routes
//     it through window.close() so the session-active confirm still fires).
Labs.MenuBar {
    id: menuBar

    signal aboutRequested()
    signal quitRequested()
    signal importSessionRequested()

    Labs.Menu {
        // macOS replaces the first menu's title with the application name.
        title: qsTr("PinPoint Studio")

        Labs.MenuItem {
            text:        qsTr("About PinPoint Studio")
            role:        Labs.MenuItem.AboutRole
            onTriggered: menuBar.aboutRequested()
        }
        Labs.MenuItem {
            text:        qsTr("Quit PinPoint Studio")
            role:        Labs.MenuItem.QuitRole
            onTriggered: menuBar.quitRequested()
        }
    }

    // ⚠ THE ONLY UI H3 ASKS FOR, AND THE REASON IT IS ONLY THIS. CORE §9 and
    // plan A10: a bundle is a recorded session and a consumer gains a FILE
    // TRANSPORT, not an importer. So there is one item, it opens a file dialog,
    // and everything a wizard would ask about is either already in the bundle
    // or forbidden here — CORE 8.5b makes reconciliation a confirmation step in
    // its own right, not a checkbox on the way in.
    Labs.Menu {
        title: qsTr("File")

        Labs.MenuItem {
            text:        qsTr("Import Session…")
            onTriggered: menuBar.importSessionRequested()
        }
    }
}
