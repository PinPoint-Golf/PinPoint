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

// The one place the declaration touches the running application.
//
// Separated from ppcp_source_declaration.cpp so that the builder is a pure
// function of an Inventory and can be tested with no camera, no microphone and
// no DeviceEnumerator — which is most of CT-S3, since the assertion that
// matters most (MSG 3.3d: a host with no Sources still sends `declare`) is
// exactly the case a developer machine cannot produce by having hardware.

#include "ppcp_source_declaration.h"

#include "../Core/device_enumerator.h"
#include "../Core/pp_settings.h"

namespace Ppcp {

PpcpSourceDeclaration::Inventory PpcpSourceDeclaration::hostInventory()
{
    Inventory inv;
    DeviceEnumerator *e = DeviceEnumerator::instance();
    if (!e) return inv;

    for (const Device &d : e->devices(DeviceType::VideoInput)) {
        Camera c;
        c.backend = d.backend;
        c.id = d.id.toStdString();
        c.label = d.description.toStdString();
        c.caps = d.capabilities;
        inv.cameras.push_back(std::move(c));
    }

    // One microphone Source, because one is what the host nominates from: the
    // acoustic detector of src/Audio runs on the active input, not on all of
    // them.
    //
    // ⚠ IT MUST BE THE INPUT THE APPLICATION ACTUALLY OPENS, AND FOR A WHILE IT
    // WAS NOT.  This took `audio.front()` — first-REGISTERED order — while
    // `AudioInput::start()` opens the device whose id matches the persisted
    // `General/audioInputDevice`, falling back to the system default.  With one
    // input those agree and nothing showed; with two they need not, and the
    // declared Source then names a microphone that is not listening.  That id
    // rides on every Candidate this host nominates (5.12a), so the error is not
    // cosmetic: it attributes an acoustic detection to the wrong instrument,
    // which is the one thing I19/I26 exist to prevent.
    //
    // The setting is read straight out of the shared INI rather than through
    // AppSettings, because nothing in src/Ppcp may depend on src/Gui — the same
    // reason the phone alias below is read this way.
    const QList<Device> audio = e->devices(DeviceType::AudioInput);
    if (!audio.isEmpty()) {
        const QString preferred =
            ppSettings().value(QStringLiteral("General/audioInputDevice")).toString();
        const Device *chosen = &audio.front();
        if (!preferred.isEmpty()) {
            for (const Device &d : audio) {
                if (d.id == preferred) { chosen = &d; break; }
            }
        }
        inv.hasMicrophone = true;
        inv.microphone.id = chosen->id.toStdString();
        inv.microphone.label = chosen->description.toStdString();
    }
    return inv;
}

}  // namespace Ppcp
