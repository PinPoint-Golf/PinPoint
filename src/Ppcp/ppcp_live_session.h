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

// The live Session, host side: open, synchronise, keep alive, arm.  Work
// package H5.  CORE §6.3, §7.3, §7.4, §8.3g; MSG §4, §5.4, §6.
//
// ⚠ QT-FREE AND SOCKET-FREE, FOR THE REASON PpcpHostPeer IS.  This class owns
// no thread, no timer and no clock of its own: it is handed a `ppcp_peer` and a
// nanosecond reading, and the embedding calls pump() from wherever its loop
// lives.  That is ground rule 7 applied to our own side of the boundary, and it
// is what lets the whole of H5's evidence be produced by two peers in one
// process with no socket between them.
//
// ── WHAT `timebase_ref` IS, AND WHY IT IS NOT NEGOTIABLE ───────────────────
//
// CORE 5.10b / I16: the Session's `timebase_ref` is immutable, and every
// arbitrated `t0` is expressed in it.  This host opens the Session with
// `tb:host` — the clock it actually reads (hostClock(), which answers for no
// other timebase, I1).  A host that named a device's clock would be asserting a
// reading it cannot take.
//
// ── I21 — ONE PROBE SEQUENCE PER LOCAL TIMEBASE, AND WHY THE LOOP MATTERS ──
//
// CORE 6.1d and 5.4.1a: relations are MEASURED per timebase and declared
// directly; there is no composition (I18, 5.4c).  So the prober is registered
// once per timebase THIS HOST DECLARES, by walking the declaration — not once,
// hardcoded, on `tb:host`.  Today PinPointStudio samples every Source against
// one host clock and the loop runs once; the moment a camera arrives with a
// free-running clock of its own, the loop runs twice and nothing else changes.
// A hardcoded single registration would pass every test on this hardware and
// fail silently on the hardware the invariant exists for.
//
// ⚠ AND THE REMOTE HALF OF I21 IS NOT REACHABLE THROUGH THIS API.  A responder
// answers `sync_probe` by stamping its ONE `sync_timebase` (peer.h,
// `ppcp_peer_config.sync_timebase`), and `ppcp_peer_sync_add_timebase()` keys
// its estimators on the LOCAL timebase — so a host cannot run two probe
// sequences against two clocks of the same device.  A device with a camera
// clock and an audio clock therefore yields one measured relation and one
// unrelated one, which 5.4b makes a legal and honest outcome and 8.2d then
// excludes.  Recorded as a finding rather than worked around; synthesising the
// second relation is exactly 8.1e.
//
// ── 6.3d — THE HEARTBEAT RATE IS NOT THE SYNC RATE ─────────────────────────
//
// Two cadences, two pumps, and this class calls both from one entry point
// because the embedding has one loop — not because they are one thing.
// `ppcp_peer_sync_pump()` runs the burst and maintenance schedule of 6.3c/6.3g;
// `ppcp_peer_liveness_pump()` runs `Session.heartbeat_interval_ms`.  Nothing
// here derives one from the other.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <ppcp/model.h>
#include <ppcp/peer.h>
#include <ppcp/sync.h>

#include "ppcp_source_declaration.h"

namespace Ppcp {

class PpcpLiveSession {
public:
    struct Config {
        // CORE 5.10 — the Session id is opaque and this host's to mint.  Given
        // by the caller because it must be stable for the Session's life and
        // nothing here can see what the application already wrote down.
        std::string sessionId;

        // 5.10b / I16 — immutable, and the clock this host actually reads.
        std::string timebaseRef = kHostTimebaseId;

        // 8.2c / 8.2g — DECLARED Session parameters, never constants, and both
        // present if and only if the Session has a host (5.10e).  The defaults
        // are CORE §5.10's own proposals and CORE B8 records that neither has
        // been measured; they are here so that a rig measurement changes one
        // number in one place rather than a constant in three.
        std::int64_t coincidenceWindowNs = PPCP_DEFAULT_COINCIDENCE_WINDOW_NS;
        std::int64_t issueHoldNs         = PPCP_DEFAULT_ISSUE_HOLD_NS;

        // CORE 7.4a — the heartbeat interval, and 7.4c makes three consecutive
        // missed ones a lost link.
        std::uint32_t heartbeatIntervalMs = PPCP_DEFAULT_HEARTBEAT_MS;
    };

    PpcpLiveSession();
    ~PpcpLiveSession();
    PpcpLiveSession(const PpcpLiveSession &) = delete;
    PpcpLiveSession &operator=(const PpcpLiveSession &) = delete;

    // `declaration` is this host's own, and is read ONLY for its Timebase list
    // (I21).  Null is legal: the prober then registers `cfg.timebaseRef` alone,
    // which is what a host that has not declared yet has to offer.
    void attach(ppcp_peer *peer, const PpcpSourceDeclaration *declaration);
    void detach();
    ppcp_peer *peer() const { return m_peer; }

    // MSG 4.1 — `session_open` with `tb:host` and BOTH arbitration parameters,
    // because 5.10e makes their presence the structural statement that this
    // Session has a host.  Registers the sync estimators and fires 6.3c's
    // on-connect burst.  Refused unless the peer is `role: host`.
    bool open(const Config &cfg, std::string *err = nullptr);
    bool isOpen() const { return m_open; }
    const Config &config() const { return m_cfg; }

    // MSG 4.4 — `session_close`.
    bool close(const char *reason, std::string *err = nullptr);

    // ── The pump ───────────────────────────────────────────────────────────
    //
    // `nowNs` is any monotonic nanosecond count of the embedding's choosing —
    // hostNowNs() in production.  It SCHEDULES and never enters a measurement:
    // the numbers that enter the estimate are `t1`..`t4`, and the library reads
    // t1/t4 from the injected clock itself.
    //
    // Runs, in order: the sync schedule (6.3c burst spacing, 6.3g maintenance),
    // the liveness schedule (7.4a heartbeats out, 7.4c misses counted), and —
    // where an estimate moved — 6.1f's `relation_update`.
    void pump(std::int64_t nowNs);

    // 6.3c — the three events that trigger a burst.  The library has no network
    // stack and no thermometer, so the embedding delivers each one.
    void triggerSync(ppcp_sync_trigger why);

    // ── Events ─────────────────────────────────────────────────────────────
    //
    // Handed every event the peer raised, so the class can follow link state,
    // learn the counterpart's timebases and notice relations arriving.  It
    // consumes nothing: the caller keeps the event and passes it on.
    void observe(const ppcp_event &ev);

    // ── Arming (CORE 7.3a) ─────────────────────────────────────────────────
    //
    // Host-controlled, and MSG 5.2's empty list means EVERY open capture
    // Stream.  That is the shipping case: this application's `armed` is a
    // property of the whole capture path, not of one camera.
    bool arm(const std::vector<std::string> &streamIds = {}, std::string *err = nullptr);
    bool disarm(const std::vector<std::string> &streamIds = {}, std::string *err = nullptr);

    // ⚠ WHAT THIS ANSWERS, AND WHAT IT DOES NOT.  `ppcp_peer_arm()` sets the
    // peer's `armed` flag when the message is successfully QUEUED — before any
    // byte leaves, and with nothing acknowledging it.  So this is "I sent arm",
    // not "the device is armed", and it must never be shown to a person as the
    // second.  `armState()` below is the one a screen may read.
    bool isArmed() const;

    // ── 5.2a — the other half of arming, which is the device's ─────────────
    //
    // MSG 5.2a makes the answer to `arm` a `readiness`, and libppcp does not
    // send one for the device: the embedding must.  Until this existed nothing
    // in this application consumed `readiness` at all, so arming was a message
    // leaving the machine and a green light with no evidence behind it.
    enum class ArmState {
        Disarmed,   // nothing sent, or `disarm` sent
        Arming,     // `arm` queued; no readiness yet, or `settled: false`
        Armed,      // readiness said `settled: true`
        Blocked,    // readiness carried a `blocked_reason` (7.3c)
        // ⚠ THIS HOST'S CONCLUSION, NOT THE DEVICE'S STATEMENT.  Asked, and
        // nothing terminal came back inside the time the device itself
        // predicted.  Kept apart from `Blocked` on purpose: a `blocked_reason`
        // is a word the DEVICE chose and this application renders verbatim, and
        // manufacturing one here to describe our own silence would be putting
        // our conclusion in the counterpart's mouth.
        Stalled,
    };
    ArmState armState() const;
    // How long this host waits for a terminal answer before concluding the arm
    // has stalled.  Generous on purpose: a device's own estimate has been
    // measured at ~8.85 s where a format change was needed, and a false
    // "stalled" is a worse lie than a slow spinner.  The device's
    // `estimated_ready_ms`, doubled, wins where it is larger.
    static constexpr std::int64_t kArmStallFloorNs = 20LL * 1000 * 1000 * 1000;
    // Why, where the device said.  Empty unless `armState() == Blocked`.
    const std::string &blockedReason() const { return m_readiness.blockedReason; }
    // 5.2a's `estimated_ready_ms`, MANDATORY when not settled.  Zero and
    // `hasEstimate == false` are different answers and the second is a device
    // that owes one.
    struct PeerReadiness {
        bool          valid = false;      // one has arrived at all
        bool          settled = false;
        bool          hasEstimate = false;
        std::uint32_t estimatedReadyMs = 0;
        std::string   blockedReason;      // empty where none
    };
    const PeerReadiness &peerReadiness() const { return m_readiness; }
    // Raised when a `readiness` moved any of the above.
    using ReadinessFn = std::function<void(const PeerReadiness &)>;
    void setReadinessCallback(ReadinessFn f) { m_onReadiness = std::move(f); }

    // ── Liveness, as the UI needs to see it ────────────────────────────────
    ppcp_link_state linkState() const;
    std::uint32_t   missedHeartbeats() const;
    // 8.3g — "a Session with an unreachable host is not a Session with no
    // host."  On the HOST this is only ever true of a Session with no host in
    // its roster, which cannot happen here; it is exposed because the
    // difference is the whole content of the clause.
    bool zeroHost() const;

    // 7.4b — the counterpart's degradation, as its last `heartbeat_ack`
    // reported it.  `valid` is false until one has arrived: "no reading" is a
    // different answer from "nominal" and is not shown as one.
    struct PeerHealth {
        bool               valid = false;
        ppcp_thermal_level thermal = PPCP_THERMAL_NOMINAL;
        bool               hasVendorLabel = false;
        std::string        vendorThermalLabel;
        std::uint64_t      storageFreeBytes = 0;
        bool               hasBatteryPct = false;
        std::uint32_t      batteryPct = 0;
        bool               hasCharging = false;
        bool               charging = false;
    };
    const PeerHealth &peerHealth() const { return m_peerHealth; }

    // Raised when the link is lost (7.4c) or restored, and when a
    // `heartbeat_ack` moved any of the numbers above.  A callback and not a
    // signal because this class is Qt-free; the Qt controller adapts.
    using LinkStateFn = std::function<void(ppcp_link_state)>;
    using HealthFn    = std::function<void(const PeerHealth &)>;
    void setLinkStateCallback(LinkStateFn f) { m_onLinkState = std::move(f); }
    void setHealthCallback(HealthFn f) { m_onHealth = std::move(f); }

    // ── The relations, and the one scalar the camera seam can take ─────────
    //
    // CORE 5.4 — the relations this peer holds, measured and received.  Read
    // through libppcp so that nothing here composes (I18).
    const ppcp_relation_set *relations() const;

    // `host_ns - source_ns` EVALUATED AT `atNs`, which is what
    // VideoInputPpcp::setTimebaseOffsetNs() can accept.
    //
    // ⚠ THE SEAM IS A SCALAR AND A RELATION IS AFFINE, so the skew term is
    // folded in at one instant and goes stale at the rate the skew was
    // measured.  That is why it is re-evaluated on every publish rather than
    // set once, and why `outSigmaNs` is handed back beside it: a consumer that
    // wants the uncertainty has it, and one that ignores it is visibly
    // ignoring something.  Returns false — and writes nothing — where there is
    // no direct relation or where it is `unrelated` (5.4b, 8.2i1).  It never
    // falls back to zero, because a fabricated mapping is indistinguishable
    // downstream from a measured one and shaped exactly like drift.
    bool offsetToRefNs(const std::string &sourceTimebase, std::int64_t atNs,
                       std::int64_t *outOffsetNs, double *outSigmaNs = nullptr) const;

    // ⚠ FOUND LIVE 27 AUG AGAINST A REAL PHONE, NOT A SYNTHETIC PEER.  A
    // relation's `observed_at` (5.4) is stamped in the SOURCE's own since-boot
    // clock — CORE 5.4: "expressed in `from`" — and this host's `hostNowNs()`
    // is a reading of ITS OWN since-boot clock. The two counters share no
    // epoch; PPC's own code found this exact mismatch worth a comment ("can
    // read minus several million milliseconds"). Every existing caller of
    // `offsetToRefNs()` was passing `hostNowNs()` as `atNs` — mixing the two —
    // which `ppcp_relations_sigma_ns()`/`ppcp_relation_apply()` then subtract
    // straight from `observed_at.ns` (`elapsed = atNs - observed_at.ns`,
    // `ppcp_sync.c:420`, `ppcp_time.c:308`) to grow the skew term. A bogus
    // multi-year `elapsed` turns a real ~17ms sigma into a fabricated ~460ms
    // one, and the exact same corruption reaches the OFFSET this feeds
    // `VideoInputPpcp::setTimebaseOffsetNs()`, not merely a diagnostic number.
    //
    // The fix is to evaluate a relation only at an instant genuinely IN its
    // own domain, and the one such instant this host can ALWAYS name without
    // guessing is the relation's own `observed_at` — elapsed 0 by
    // construction, so `outSigmaNs` reads exactly `offset_sigma_ns`, no
    // projection, no domain mixing. PPC now republishes every ~5s (27 Aug),
    // so a caller re-fetching this on every `relation_update` stays within
    // seconds of fresh regardless — the growth term `offsetToRefNs()` exists
    // to add was only ever going to be sub-millisecond at that cadence.
    // Returns false under the same conditions `offsetToRefNs()` does.
    bool observedAtNs(const std::string &sourceTimebase, std::int64_t *outObservedAtNs) const;

    // Every timebase for which a direct relation into `timebase_ref` exists.
    std::vector<std::string> relatedTimebases() const;

    // Called after each `relation_update` this host PUBLISHES (6.1f), with the
    // count.  The embedding uses it to re-feed VideoInputPpcp's offset seam.
    using RelationsFn = std::function<void()>;
    void setRelationsCallback(RelationsFn f) { m_onRelations = std::move(f); }

    struct Stats {
        std::size_t   probesQueued      = 0;
        std::size_t   relationsPublished = 0;
        std::size_t   heartbeatAcks     = 0;
        std::size_t   linkLosses        = 0;
        std::size_t   linkRestores      = 0;
        std::size_t   syncEstimators    = 0;
        // 6.3a — an estimator that has not yet produced a rate has no relation
        // to publish, and saying so is the point of the counter.
        std::size_t   estimatorsWithoutEstimate = 0;
    };
    const Stats &stats() const { return m_stats; }

private:
    bool registerEstimators(std::string *err);
    void publishRelations();

    ppcp_peer                   *m_peer = nullptr;
    const PpcpSourceDeclaration *m_declaration = nullptr;
    Config                       m_cfg;
    bool                         m_open = false;
    // The timebases the prober was registered on, in declaration order.  Kept
    // so that a test can assert I21 by count rather than by inspection.
    std::vector<std::string>     m_localTimebases;
    PeerHealth                   m_peerHealth;
    PeerReadiness                m_readiness;
    ReadinessFn                  m_onReadiness;
    // Whether THIS host has asked.  Kept beside the device's answer because the
    // two together are the state, and either alone is a half-truth: an `arm`
    // nobody answered and a stale `settled` from before a `disarm` look
    // identical if only one of them is remembered.
    bool                         m_armRequested = false;
    // Stamped by pump() on the first tick after an `arm`, because this class
    // owns no clock (ground rule 7) and arm() has no reading to hand it.
    std::int64_t                 m_armAskedAtNs = 0;
    bool                         m_armStalled = false;
    ppcp_link_state              m_lastLinkState = PPCP_LINK_LIVE;
    LinkStateFn                  m_onLinkState;
    HealthFn                     m_onHealth;
    RelationsFn                  m_onRelations;
    Stats                        m_stats;
    // 6.1f is "publish the current estimate"; publishing one that has not moved
    // is bytes on a control channel for no information.  This is the count of
    // estimators that had an estimate at the last publish, which is the cheapest
    // honest change detector: it never suppresses a NEW relation, and a moving
    // offset is republished on the maintenance cadence anyway.
    std::size_t                  m_publishedWith = 0;
};

}  // namespace Ppcp
