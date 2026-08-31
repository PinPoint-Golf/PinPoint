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


// Link stubs for ppcp_host_service_test.
//
// `PpcpHostService` reaches eight symbols in `src/Video` — `registerPpcpPeer()`
// on `declare`, `applyTimebaseOffsets()` / `clearTimebaseMappings()` around a
// link's timebase relations, and `dispatchEvent()` / `detachAll()` /
// `reattachAll()` around a
// VideoInputPpcp a caller may have attached for preview.  Linking the real ones
// would drag
// `video_input_factory.cpp` (Aravis, Spinnaker and Qt Multimedia backends) and
// `VideoInputPpcp_inventory.cpp` (which reaches Bluetooth through
// `device_enumerator.h` -> `imu_base.h`) into a suite whose whole subject is the
// pairing code's clock.  `ppcp_video_input_test` refuses the same TU for the
// same reason and says so.
//
// ⚠ EVERY ONE OF THEM IS UNREACHABLE IN THIS SUITE.  All five are called only
// from `onDeclare()`, the event hook `configurePhonePeer()` installs, and
// `dropPhone()`, and this test never accepts a link — it publishes a code and
// watches the clock.  So these definitions satisfy the linker without standing
// in for behaviour anything here observes; if a future test in this file did
// accept a link, it would be asserting against stubs and these would have to
// go.

#include "../../Core/pp_debug.h"
#include "../../Video/VideoInputPpcp.h"
#include "../../Video/video_input_factory.h"
#include "ppcp_source_declaration.h"

int VideoInputFactory::registerPpcpPeer(const struct ppcp_peer_desc * /*peer*/)
{
    return 0;
}

int VideoInputPpcp::applyTimebaseOffsets(const QString & /*peerId*/,
                                         const TimebaseOffsetLookup & /*lookup*/)
{
    return 0;
}

int VideoInputPpcp::clearTimebaseMappings(const QString & /*peerId*/)
{
    return 0;
}

int VideoInputPpcp::dispatchEvent(const QString & /*peerId*/, const ppcp_event & /*ev*/)
{
    return 0;
}

int VideoInputPpcp::detachAll(const QString & /*peerId*/)
{
    return 0;
}

int VideoInputPpcp::reattachAll(const QString & /*peerId*/, ppcp_peer * /*peer*/,
                                const QString & /*sessionId*/)
{
    return 0;
}

int VideoInputPpcp::openPreviewStreams(ppcp_peer * /*peer*/, const QString & /*peerId*/,
                                       const QString & /*sessionId*/,
                                       const ppcp_peer_desc * /*desc*/)
{
    return 0;
}

QVariantList VideoInputPpcp::countersFor(const QString & /*peerId*/)
{
    return {};
}

// ── The logger is NOT stubbed here any more ─────────────────────────────────
// It was, because `ppWarn()` used to live in pp_debug.cpp beside the installer
// that reaches whisper and FFmpeg — so this suite dropped every line the service
// logged.  The primitive stands alone now (src/Core/pp_log_stream.cpp), which the
// target links, and this host service is loud on purpose: its log is how a
// pairing or a wired dial is followed in the field.

// ── The host's own inventory ────────────────────────────────────────────────
// ⚠ UNLIKE THE THREE ABOVE, THIS ONE IS REACHED: `start()` builds this host's
// `declare` (MSG 3.3c) from it.  The real one reads `DeviceEnumerator`, which
// reaches Bluetooth through `imu_base.h`, and an empty inventory is a state the
// real function itself produces — `ppcp_source_declaration.h:116-119` keeps it
// the only global-touching function precisely so the rest is "reproducible on a
// machine with no cameras".  That is the machine this suite pretends to be on,
// and a pairing code's clock does not depend on what is plugged into the host.
Ppcp::PpcpSourceDeclaration::Inventory Ppcp::PpcpSourceDeclaration::hostInventory()
{
    return {};
}

// ── The preview consumers (27 Aug 2026) ─────────────────────────────────────
// ⚠ Same reasoning as the block above and the same unreachability: both are
// called only from `onDeclare()` and `dropPhone()`, and this suite never
// accepts a link.  Static for exactly this reason — constructing a real
// `VideoInputPpcp` here would need its vtable and moc output, and with them the
// camera backends this file exists to keep out.
int VideoInputPpcp::startPreviewConsumers(QObject * /*parent*/, ppcp_peer * /*peer*/,
                                          const QString & /*peerId*/,
                                          const QString & /*sessionId*/,
                                          const ppcp_peer_desc * /*desc*/,
                                          std::vector<VideoInputPpcp *> * /*out*/)
{
    return 0;
}

void VideoInputPpcp::stopPreviewConsumers(std::vector<VideoInputPpcp *> * /*consumers*/)
{
}
