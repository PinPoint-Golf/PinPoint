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

// The host peer: libppcp's engine in `role: host`, fed from H1's transport.
// Work package H2, second half.
//
// GROUND RULE 7 IS THE SHAPE OF THIS FILE.  "The library owns no I/O.  libppcp
// is sans-I/O: it owns no socket, thread, timer, clock or file.  The embedding
// application supplies bytes, timestamps and storage and drains the library."
// So everything the engine cannot do for itself is here: the sockets (H1), the
// clock (`tb:host`), the acceptance decision, the health readings, and the loop
// that moves bytes between the two.
//
// ⚠ DEFERRAL, RECORDED HONESTLY.  `ppcp_peer` is work package L6 and had not
// landed when this file was written: `include/ppcp/peer.h` was still a
// placeholder and every ppcp_peer_* symbol was a declaration in planned.h with
// no definition in libppcp.a.  Rather than fake an engine, the binding to it is
// behind PPCP_HAVE_PEER (set by CMake when the real header appears) and
// everything on THIS side of the boundary — the pump, the policy, the clock,
// the health callbacks — is real, built and tested against a sink interface.
// The day L6 lands, LibppcpEngine below is the only thing that has to compile.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ppcp/model.h>
#include <ppcp/time.h>

#include "ppcp_ingest_policy.h"
#include "ppcp_source_declaration.h"
#include "ppcp_transport.h"

namespace Ppcp {

// What the pump moves bytes into and out of.
//
// In production this is `ppcp_peer` and nothing else. It is an interface
// because the pump is THIS APPLICATION'S code — the part ground rule 7 says the
// embedding owes the library — and it has to be testable without a socket at
// one end and without L6 at the other.
class PpcpEngine {
public:
    virtual ~PpcpEngine() = default;

    // Bytes in, on the channel they arrived on. ENC 2c's header-matches-stream
    // check is the engine's, which is why the channel travels with the bytes.
    virtual ppcp_result feed(std::uint8_t channel, const std::uint8_t *bytes,
                             std::size_t len) = 0;

    // Bytes out, per channel. PPCP_ERR_NOT_FOUND (or *out_len == 0) means the
    // engine has nothing to send on that channel right now.
    virtual ppcp_result drain(std::uint8_t channel, std::uint8_t *out, std::size_t cap,
                              std::size_t *out_len) = 0;
};

class PpcpHostPeer {
public:
    struct Config {
        // CORE 5.1a — stable, and not derived from mutable local state. The
        // caller supplies it because it must survive a restart, which nothing
        // this class can see does.
        std::string peerId;

        PpcpIngestPolicy::Limits ingest;

        // ⚠ APPLICATION POLICY, LIKE THE INGEST FLOOR, AND FOR THE SAME REASON.
        // CORE §9 and 5.14g1: "a peer under storage pressure REFUSES TO ARM
        // rather than dropping swings a consumer has not received". How much
        // headroom counts as pressure is this host's decision — the protocol
        // says nothing about it and must not — and 5.15's `blocked_reason` is
        // where the decision surfaces on the wire.
        std::uint64_t storageFloorBytes = 2ull * 1024 * 1024 * 1024;   // 2 GiB
    };

    // The host's own readings, supplied by the embedding because the library
    // cannot take them (ground rule 7). Both return false for "cannot tell",
    // which is a different answer from "fine" and is treated as one.
    using ThermalFn = std::function<bool(ppcp_thermal_level *out)>;
    using StorageFn = std::function<bool(std::uint64_t *freeBytes)>;

    explicit PpcpHostPeer(Config cfg);
    ~PpcpHostPeer();
    PpcpHostPeer(const PpcpHostPeer &) = delete;
    PpcpHostPeer &operator=(const PpcpHostPeer &) = delete;

    // Builds this host's `declare` from its own hardware (H2, first half).
    // MSG 3.3c: a peer declares BEFORE it originates any message referencing a
    // Source, Stream or Candidate, so this happens before anything is pumped.
    bool declareSelf(const PpcpSourceDeclaration::Inventory &inv, std::string *err = nullptr);
    const PpcpSourceDeclaration &declaration() const { return m_declaration; }

    const PpcpIngestPolicy &ingestPolicy() const { return m_policy; }
    void setThermal(ThermalFn f) { m_thermal = std::move(f); }
    void setStorage(StorageFn f) { m_storage = std::move(f); }

    // CORE 5.15 — Readiness, as a MEASUREMENT and never a state-machine name
    // (5.15a: "a device state-machine name — `cold`, `warm`, `armed` or any
    // equivalent — MUST NOT cross the wire").
    ppcp_result readiness(ppcp_readiness *out) const;

    // The acceptance decision, exposed so the same policy object answers both
    // the engine's callback and a diagnostic view.
    IngestVerdict evaluate(const ppcp_peer_desc &counterpart) const;

    // The clock the engine is handed. `tb:host`, monotonic, and it answers for
    // no other timebase (I1).
    static ppcp_clock clock() { return hostClock(); }

    // ── The pump ───────────────────────────────────────────────────────────
    // One link, its channels, and the engine at the other end. Nothing here
    // blocks: TransportChannel is non-blocking throughout, which is what makes
    // the backpressure of CORE T2 visible — a bulk channel that has filled its
    // window says so and the control channel carries on regardless (ENC 2d).
    void attach(PeerConnection *link, PpcpEngine *engine);

    struct PumpStats {
        std::size_t bytesIn = 0;
        std::size_t bytesOut = 0;
        std::size_t wouldBlockOnWrite = 0;   // CORE T2 backpressure, observed
        bool        closed = false;
    };

    // Moves whatever is ready in both directions on every bound channel.
    // Returns false once the link is dead. Call it from wherever the
    // application's loop lives; this class owns no thread (ground rule 7 again,
    // applied to ourselves so H8's headless harness can drive it).
    bool pump();

    const PumpStats &stats() const { return m_stats; }

    // Builds the engine over libppcp. Null until L6 lands — see the deferral
    // note at the top of this header — and the caller is told which symbol is
    // missing rather than getting a stub that lies.
    std::unique_ptr<PpcpEngine> makeLibppcpEngine(std::string *whyNot = nullptr);

private:
    Config                 m_cfg;
    PpcpIngestPolicy       m_policy;
    PpcpSourceDeclaration  m_declaration;
    ThermalFn              m_thermal;
    StorageFn              m_storage;
    PeerConnection        *m_link = nullptr;
    PpcpEngine            *m_engine = nullptr;
    PumpStats              m_stats;
    std::vector<std::uint8_t> m_scratch;
};

}  // namespace Ppcp
