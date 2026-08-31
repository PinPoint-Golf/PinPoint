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

#include "ppcp_dnssd_runtime.h"

// Qt-free, matching every other file in src/Ppcp/ (design: "Deliberately
// Qt-free so the conformance harness can drive it headless").  Silent by
// design too — RV 3.6a says absence is not an error, and the one log line
// belongs to the Qt-based caller (ppcp_host_service.cpp), not to this probe.

#if defined(PP_HAVE_DNS_SD) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pinpoint::ppcp {
namespace {

bool probeAndLoad()
{
    // dnssd.dll is a shared system-service DLL (installed to System32 by
    // iTunes / "Apple Devices" / Bonjour Print Services), not a private SDK
    // artifact the way Spinnaker's core DLL is — so the standard search
    // order (System32 first) is exactly right and no SDK-root probing is
    // needed. ⚠ This is an EXPLICIT, EARLY probe, not a delay-load gate —
    // dnssd.lib's own stub functions do this same LoadLibrary internally on
    // first call regardless (see ppcp_dnssd_runtime.h), so this call mainly
    // buys an early, own diagnostic rather than new protection.
    return ::LoadLibraryW(L"dnssd.dll") != nullptr;
}

}  // namespace

bool dnssdRuntimeAvailable()
{
    // Thread-safe, run-once initialisation (C++11 magic statics).
    static const bool available = probeAndLoad();
    return available;
}

}  // namespace pinpoint::ppcp

#else  // !(PP_HAVE_DNS_SD && _WIN32)

namespace pinpoint::ppcp {
// Off Windows, DNS-SD is never delay-loaded — macOS gets it from libSystem
// and Linux links Avahi's compat shim normally — so there is no runtime
// probe to gate on. When PP_HAVE_DNS_SD is unset the compile-time guard in
// ppcp_discovery.cpp already returns null factories; this is never reached.
bool dnssdRuntimeAvailable() { return true; }
}  // namespace pinpoint::ppcp

#endif
