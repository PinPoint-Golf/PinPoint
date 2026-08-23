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

// The one place VideoInputPpcp touches the running application's device
// registry.
//
// Separated from VideoInputPpcp.cpp for exactly the reason
// ppcp_host_inventory.cpp is separated from ppcp_source_declaration.cpp (H2):
// device_enumerator.h reaches Bluetooth through imu_base.h, and a camera
// backend that had to link the IMU stack to be tested would not be testable at
// all.  Everything in VideoInputPpcp.cpp is a pure function of a declaration;
// this is the impure edge, and it is one function long.

#include "VideoInputPpcp.h"

#include "../Core/device_enumerator.h"

int VideoInputPpcp::registerSources(const ppcp_peer_desc *peer)
{
    if (!peer) return 0;
    DeviceEnumerator *e = DeviceEnumerator::instance();
    if (!e) return 0;

    int n = 0;
    for (std::size_t i = 0; i < peer->source_count; ++i) {
        const ppcp_source &s = peer->sources[i];
        // 5.6 — a Peer declares every Source it owns, of every kind.  Only the
        // cameras belong behind a camera factory; a microphone or an IMU Source
        // reaches this host through its own abstraction, or through none.
        if (!ppcp_source_kind_is_camera(&s)) continue;

        const QString peerId   = QString::fromUtf8(peer->id.v);
        const QString sourceId = QString::fromUtf8(s.id.v);
        QString label = s.has_label ? QString::fromUtf8(s.label.v) : sourceId;
        if (peer->product.present && !QString::fromUtf8(peer->product.model.v).isEmpty())
            // ⚠ QStringLiteral AND NOT QLatin1String.  This file is UTF-8, so
            // the em dash in that literal is the three bytes E2 80 94.
            // QLatin1String declares them Latin-1, which turns one dash into
            // three characters — "â" and two control codes — and every phone
            // camera in the DEVICES list read "iPhone 16 â.. Wide".
            label = QString::fromUtf8(peer->product.model.v) + QStringLiteral(" — ") + label;

        e->registerDevice(DeviceType::VideoInput,
                          VideoInputFactory::Backend::Ppcp,
                          VideoInputPpcp::deviceIdFor(peerId, sourceId),
                          label,
                          VideoInputPpcp::capabilitiesFor(peer, &s));
        ++n;
    }
    return n;
}
