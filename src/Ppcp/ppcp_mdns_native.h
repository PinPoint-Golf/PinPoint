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

#pragma once

// Windows only. The socket/responder/querier half of the native mDNS engine
// — see ppcp_mdns_wire.h for the wire codec and why this exists instead of
// the Bonjour SDK for Windows path: Apple's Bonjour runtime registers a
// Winsock namespace provider (mdnsNSP.dll, last touched 2015) that fails
// modern Windows Code Integrity / LSA-protection signing checks, visibly —
// a "blocked from loading into the Local Security Authority" popup on every
// boot, plus constant background Code Integrity errors — on every machine
// that has it installed, whether or not PinPointStudio is even running.
//
// makeNativeBrowser()/makeNativeAdvertiser() implement RvBrowser/RvAdvertiser
// (ppcp_discovery.h) directly against a raw UDP 5353 multicast socket this
// process owns. Nothing here calls into Bonjour, registers a Winsock
// namespace provider, or touches any Apple binary — this machine's only
// mDNS presence is this process's own socket, for as long as it is running.

#include <memory>

#include "ppcp_discovery.h"

namespace Ppcp {
namespace Mdns {

std::unique_ptr<RvBrowser> makeNativeBrowser();
std::unique_ptr<RvAdvertiser> makeNativeAdvertiser();

}  // namespace Mdns
}  // namespace Ppcp
