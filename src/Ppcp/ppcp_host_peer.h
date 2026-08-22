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
// ⚠ THE DEFERRAL IS OVER, AND WHAT IT COST IS WORTH RECORDING.  When this file
// was written `ppcp_peer` was work package L6, `include/ppcp/peer.h` was a
// placeholder, and the binding sat behind PPCP_HAVE_PEER so that everything on
// THIS side of the boundary — the pump, the policy, the clock, the health
// callbacks — was real and tested against a sink interface rather than faked.
// L6 has now landed and the binding compiles.  It cost two changes and no
// rewrites: ppcp_peer_feed() gained an `out_consumed` (so the pump keeps the
// tail of a partial frame, which is m_tails below), and the ingest callback
// gained an `out_reason`.  Everything else stood.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ppcp/model.h>
#include <ppcp/time.h>

#include "ppcp_annotation_store.h"
#include "ppcp_engine.h"
#include "ppcp_live_session.h"
#include "ppcp_shot_bridge.h"
#include "ppcp_ingest_policy.h"
#include "ppcp_source_declaration.h"
#include "ppcp_transport.h"

namespace Ppcp {

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

    // Moves whatever is ready in both directions on every bound channel, and
    // drains the engine's event ring.  Returns false once the link is dead.
    // Call it from wherever the application's loop lives; this class owns no
    // thread (ground rule 7 again, applied to ourselves so H8's headless
    // harness can drive it).
    bool pump();

    // pump(), then the two schedules that need a clock: §6.3's sync cadence and
    // §7.4's heartbeat cadence, and then 8.2h's issue hold.  `nowNs` is a
    // reading of `tb:host` — hostNowNs() in production.
    //
    // ⚠ SEPARATE FROM pump() ON PURPOSE.  pump() is driven by the socket being
    // readable; these three are driven by TIME PASSING, and a link with no
    // traffic on it still has heartbeats due.  A loop that only pumped when
    // bytes arrived would never notice a dead peer, which is the one thing
    // §7.4 exists to notice.
    bool tick(std::int64_t nowNs);

    const PumpStats &stats() const { return m_stats; }

    // ── What H5 and H7 hang off this peer ──────────────────────────────────
    //
    // Owned here because this is the object that has the link, and every one of
    // them needs the `ppcp_peer` behind it.  attach() binds all three.
    PpcpLiveSession       &liveSession()  { return m_live; }
    PpcpShotBridge        &shotBridge()   { return m_shots; }
    PpcpAnnotationStore   &annotations()  { return m_annotations; }

    // ⚠ THE EVENT RING HAS EXACTLY ONE DRAINER, AND IT IS THIS CLASS.
    // `ppcp_peer_next_event()` REMOVES an event, and an event's `msg` is valid
    // only until PPCP_PEER_EVENT_QUEUE further events have been queued.  Two
    // consumers calling it against one peer would each see roughly half the
    // conversation, silently and non-deterministically.
    //
    // So anything else that needs to see events — the offer list, and
    // `VideoInputPpcp` when it is bound to a peer THIS class pumps — registers
    // a hook here instead of draining for itself.  `VideoInputPpcp::drainEvents()`
    // MUST NOT be called on such a peer; it exists for the standalone paths (the
    // bundle transport, and the H4 suite) where nothing else is draining.
    //
    // Hooks are called in registration order, once per event, while the event's
    // bytes are still the ones the transport fed.
    using EventFn = std::function<void(const ppcp_event &)>;
    void addEventHook(EventFn f) { m_hooks.push_back(std::move(f)); }

    // MSG 3.3 — a counterpart declared.  PPCP cameras are not discovered by
    // scanning: they appear when a capture peer connects and says what it owns,
    // so this is where `video_input_factory::registerPpcpPeer()` is called from.
    //
    // A hook and not a direct call, because `device_enumerator.h` reaches
    // Bluetooth through `imu_base.h` and this translation unit is deliberately
    // free of both — the same reason `VideoInputPpcp_inventory.cpp` and
    // `ppcp_host_inventory.cpp` are their own files.
    using DeclarationFn = std::function<void(const ppcp_peer_desc *)>;
    void setDeclarationHook(DeclarationFn f) { m_onDeclare = std::move(f); }

    // 6.1f — a `relation_update` was published or received, so every
    // `VideoInputPpcp` bound to this peer can be re-fed its offset.  Again a
    // hook: the conversion is this class's, the camera registry is not.
    using RelationsFn = std::function<void(const PpcpLiveSession &)>;
    void setRelationsHook(RelationsFn f);

    // Builds the engine over libppcp — through ppcp_host_engine.h, which is the
    // ONE place that says what a PinPointStudio peer is, so the bundle
    // transport of H3 gets the identical engine without going near a socket.
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
    // The unconsumed tail of each channel's stream. L6's ppcp_peer_feed()
    // consumes whole frames and hands the remainder back, so somebody has to
    // hold a partial frame between two socket reads; ground rule 7 says it is
    // this side of the boundary.
    std::map<std::uint8_t, std::vector<std::uint8_t>> m_tails;

    PpcpLiveSession        m_live;
    PpcpShotBridge         m_shots;
    PpcpAnnotationStore    m_annotations;
    std::vector<EventFn>   m_hooks;
    DeclarationFn          m_onDeclare;
    RelationsFn            m_onRelations;

    void drainEvents();
};

}  // namespace Ppcp
