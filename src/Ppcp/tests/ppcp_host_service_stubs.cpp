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
// `PpcpHostService` reaches three symbols in `src/Video` — `registerPpcpPeer()`
// on `declare`, and `applyTimebaseOffsets()` / `clearTimebaseMappings()` around
// a link's timebase relations.  Linking the real ones would drag
// `video_input_factory.cpp` (Aravis, Spinnaker and Qt Multimedia backends) and
// `VideoInputPpcp_inventory.cpp` (which reaches Bluetooth through
// `device_enumerator.h` -> `imu_base.h`) into a suite whose whole subject is the
// pairing code's clock.  `ppcp_video_input_test` refuses the same TU for the
// same reason and says so.
//
// ⚠ EVERY ONE OF THEM IS UNREACHABLE IN THIS SUITE.  All three are called only
// from `onDeclare()` and `dropLink()`, and this test never accepts a link — it
// publishes a code and watches the clock.  So these definitions satisfy the
// linker without standing in for behaviour anything here observes; if a future
// test in this file did accept a link, it would be asserting against stubs and
// these would have to go.

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

// ── The logger ──────────────────────────────────────────────────────────────
// `ppWarn()` is a `PpLogStream`, and `pp_debug.cpp` reaches whisper and FFmpeg
// to install their log captures.  A collecting sink that drops what it collects
// is the whole of what this suite needs from it — the assertions are on the
// service's state, never on what it printed.
PpLogStream::PpLogStream(QtMsgType t) : m_type(t), m_dbg(QDebug(&m_buf)) {}

PpLogStream::~PpLogStream() = default;

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
