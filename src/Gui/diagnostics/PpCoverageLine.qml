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

// The coverage line — the quietest sentence on the panel and, per brief §1, the most
// load-bearing: how few of the pack's characteristics this capture can measure at all.
// It is drawn at the bottom in colorText3 because it must never compete with a finding,
// and it is never omitted, because a panel that hid it would be implying the nine
// conditions it can speak to are the whole of the golfer's swing.

import QtQuick
import PinPointStudio

Text {
    id: root

    // SessionDiagnosticsModel::coverageLine()
    property string line: ""
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0

    objectName: "sdCoverageLine"

    visible: line !== ""

    text: line
    elide: Text.ElideRight
    font.family: Theme.fontData
    font.pixelSize: Math.max(1, Math.round(Theme.sp(8) * fit))
    color: Theme.colorText3
}
