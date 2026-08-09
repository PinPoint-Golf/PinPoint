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

pragma Singleton

// MetricRoute — the one-line router that links a metric ANYWHERE in the app to its
// catalogue detail page. A metric tile is typically instantiated several layers
// inside a stage delegate (or a top-level cast Window), so it has no path to the
// settings screen that hosts MetricLibrary/MetricDetail. Rather than thread a
// signal up through every host, tiles call MetricRoute.open(key) and Main.qml — the
// one place that owns navigation — performs the jump.
//
// CURRENTLY NO PRODUCER: the per-shot dashboard's tiles were its only callers and
// went with that surface. The receiving side (Main.qml → ScreenSettings) stays
// wired for the next surface that wants a metric deep-link.
//
// Deliberately state-light: `requestedKey` is a record of the last request for a
// late-attaching listener, not a queue. The signal is the contract.

import QtQuick

QtObject {
    // Last requested metric key ("" = none requested yet).
    property string requestedKey: ""

    // Emitted on every open() — including a repeat of the same key, which is a real
    // user action (they clicked it again) and must still navigate.
    signal openRequested(string key)

    function open(key) {
        if (!key || key.length === 0)
            return
        requestedKey = key
        openRequested(key)
    }
}
