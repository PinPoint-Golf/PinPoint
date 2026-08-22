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

// One place that says what a PinPointStudio PPCP peer IS.
//
// ⚠ IT IS ONE PLACE ON PURPOSE. The live socket path (H1/H2) and the bundle
// path (H3) must reach the SAME engine, configured the same way, or plan A10's
// "a consumer gains a file transport, not an importer" is a claim about the
// wiring diagram rather than about the code. Two constructions that happened to
// agree today is exactly how the two ingest paths ENC §7 warns about come
// about, so there is one and both call it.
//
// It deliberately does NOT depend on the transport: a bundle import must work
// on a build with no OpenSSL, because a file is not a socket and requiring TLS
// to read one would be this application inventing a requirement the
// specification does not have.

#include <functional>
#include <memory>
#include <string>

#include <ppcp/model.h>
#include <ppcp/sync.h>
#include <ppcp/time.h>

#include "ppcp_engine.h"

namespace Ppcp {

class PpcpIngestPolicy;

struct HostEngineConfig {
    // CORE 5.1a — stable across restarts, so the caller owns it.
    std::string peerId;

    // I14 / CT-I14 — the acceptance decision, as a CALLBACK and never a number.
    // May be null, which the library reads as "accept": peer.h says a peer that
    // supplies no policy accepts, "because refusing by default would make the
    // library the one with an opinion".
    const PpcpIngestPolicy *policy = nullptr;

    // CORE 5.15 — this host's own readings. Ground rule 7: the library cannot
    // take them.
    std::function<ppcp_result(ppcp_readiness *)> health;

    // CORE 7.4b / MSG 5.4 — what `heartbeat_ack` carries: thermal, storage,
    // battery. A SECOND callback and not a field of the first, because L9
    // separated them for the reason 5.15a separates readiness from a state
    // name: readiness answers "can I capture the next shot", health answers
    // "am I degrading". A host that reported one as the other would tell a
    // device its thermal state when it asked whether the session could run.
    // May be null, in which case a `heartbeat_ack` carries the minimum.
    std::function<ppcp_result(ppcp_health *)> healthReport;

    // MSG 6.1b — the timebase this peer stamps `t2`/`t3` on when it ANSWERS a
    // `sync_probe`. Empty means this host does not answer probes, and one
    // arriving is answered `error`/`profile_not_supported` rather than with a
    // fabricated instant. It is `tb:host` here because that is the only clock
    // this application reads (I1) — see hostClock().
    std::string syncTimebase;

    // ENC 2.1a — only a DIALLER mints a `link_id` and sends `link_bind`. Under
    // PPCP-RV the host is the code publisher and the TLS server, so it listens;
    // a bundle has no dial at all and the flag is moot there.
    bool listener = true;

    // CORE 5.3 / CONF 2a — the clock the engine reads for `sync_probe` and for
    // nothing else.  `hostClock()` in production, and there is no other value
    // this application ships with.
    //
    // ⚠ IT IS SETTABLE ONLY SO THAT `ppcp_sim_clock` CAN DRIVE IT IN A TEST.
    // §6.3 measures a relation between two clocks running at different rates,
    // and a test that had to WAIT for real skew to accumulate would take hours
    // and still not know the answer.  libppcp ships the simulated clock in the
    // library rather than in its tests precisely so both applications inject
    // the same one; `now` being null means hostClock().
    ppcp_clock clock{};
};

// Null with `whyNot` set when the engine cannot be built.
std::unique_ptr<PpcpEngine> makeHostEngine(HostEngineConfig cfg, std::string *whyNot = nullptr);

}  // namespace Ppcp
