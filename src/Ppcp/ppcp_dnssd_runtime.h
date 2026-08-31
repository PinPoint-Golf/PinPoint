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

// W4 — an EARLY, EXPLICIT version of a check the Bonjour SDK's own
// `dnssd.lib` already performs internally. ⚠ Unlike src/Video/spinnaker_runtime.h's
// PE `/DELAYLOAD` thunk, `dnssd.lib` is not a conventional import library at
// all: `dumpbin /archivemembers` shows one object, `DLLStub.obj`, and every
// `DNSService*` entry point is a small stub that does its own
// `LoadLibrary`/`GetProcAddress` against dnssd.dll at call time — Apple's
// own hand-rolled delay-loading, predating the `/DELAYLOAD` convention.
// dnssd.dll never appears in this binary's import table at all (confirmed:
// neither the regular nor the delay-load list in `dumpbin /dependents`), so
// there is no PE-level thunk to gate. This probe exists anyway: it is an
// explicit, OWN diagnostic point ahead of the SDK's, consistent with how
// every other optional native dependency here is gated (AMDS, Spinnaker),
// rather than trusting all of a 2007-era stub library's error paths.
// dnssd.dll ships only with iTunes / the Microsoft Store "Apple Devices" app
// / the Bonjour Print Services installer, and we may not redistribute it
// (design §4.3). Every DNSService* entry point (BonjourBrowser::start(),
// BonjourAdvertiser::start()) checks this first, on Windows only — see
// ppcp_discovery.cpp's makePlatformBrowser()/makePlatformAdvertiser().

namespace pinpoint::ppcp {

// True once dnssd.dll has been found and loaded. False elsewhere on Windows
// (the DLL is absent — an ordinary state, RV 3.6a: no banner, one log line).
// Always true off Windows, where DNS-SD is never delay-loaded.
bool dnssdRuntimeAvailable();

}  // namespace pinpoint::ppcp
