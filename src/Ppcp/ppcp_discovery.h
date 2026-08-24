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

// PPCP-RV §3 service discovery — the browser half, and since H9 the
// advertisement half as well.
//
// RV 3.5b: "a capture peer advertises and a host browses.  Browsing needs only
// the querier role; advertising needs a responder, which a mobile platform
// supplies and several desktop platforms do not."  That was the whole of this
// file until 24 August 2026.
//
// ⚠ 3.5e (erratum E32) REVERSES IT FOR THIS DEPLOYMENT, and the reversal is
// an obligation rather than a permission.  Our counterpart is bound by 3.5d
// — `Network.framework`'s listener has no server-side PSK resolver, so the
// capture peer MUST NOT advertise for reconnection — and 3.5e then says the
// peer that CAN advertise does.  If neither did, §7.4's persisted pairing
// would buy nothing: both peers would hold valid key material with no path by
// which either finds the other, and the user would see a protocol that
// remembers them and still asks for a code every session.
//
// ⚠ THIS PROCESS STILL BINDS NO MULTICAST SOCKET.  Both halves ask the
// platform's existing responder (mDNSResponder on macOS) over a local IPC
// socket — `DNSServiceBrowse` to query on our behalf, `DNSServiceRegister` to
// publish on our behalf.  There is no QUdpSocket anywhere in this file and
// there must never be one: binding 5353 in this process would conflict with
// the responder that already owns it.
//
// ⚠ AND THE WHOLE THING IS A CONVENIENCE.  RV §3 is "optional.  Reconnection
// convenience only — a first pairing always uses §4", and 3.6a MUST NOT treat
// discovery failure as an error state: "multicast is rate-limited or dropped by
// many consumer access points, blocked by client isolation on guest networks,
// and does not cross VLAN boundaries.  IT WILL NOT WORK AT A RANGE."  So this
// class has no error signal, only results, and its absence is silent (3.6b).
//
// ⚠ 3.4c IS THE ONE HARD RULE HERE: a browsing peer MUST NOT connect to an
// instance whose `rid` it cannot resolve.  `decideDial()` below is the only
// function that answers the question, it takes the resolver as its argument,
// and it has no "unknown" branch that connects anyway.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ppcp/rv.h>

namespace Ppcp {

// One discovered `_ppcp._tcp` instance, as far as it has been resolved.
struct RvAdvertisement {
    std::string   instanceName;    // 3.2a — "PPCP-" + 8 uppercase hex
    std::string   host;            // resolved target, or an address literal
    std::uint16_t port = 0;

    // 3.3a's TXT keys.  Anything else in the record is IGNORED and is not an
    // error — the same rule as 4.2c, one layer out.
    std::string   txtvers;
    std::string   pv;              // e.g. "1.0" or "1.0-1.2"
    std::string   role;            // host | capture | observer
    bool          hasRn = false;
    std::uint8_t  rn[PPCP_RV_RN_BYTES]{};
    bool          hasRid = false;
    std::uint8_t  rid[PPCP_RV_RID_BYTES]{};
};

// DNS-SD TXT is a sequence of length-prefixed "key=value" strings.  Returns
// false only when the buffer is malformed as a TXT record; an unknown key, a
// missing key or a value of the wrong length leaves the corresponding field
// unset and is NOT a parse failure (3.3a: "a receiver ignores any key it does
// not recognise").
bool parseTxtRecord(const std::uint8_t *txt, std::size_t len, RvAdvertisement *out);

// 3.3a — "a browser filters on MAJOR before connecting".  `pv` is either a
// single version or an inclusive range `lo-hi`; this answers whether `major`
// falls inside it.  An unparseable or absent `pv` is NOT acceptable: it is
// exactly the case where connecting would waste a dial on a peer that cannot
// speak to us.
bool pvAcceptsMajor(const std::string &pv, int major);

// 3.2a — the instance name a given `rid` must produce.  A name that disagrees
// with the TXT record's own `rid` is a malformed advertisement, and saying so
// is free.
bool instanceNameMatchesRid(const std::string &instanceName,
                            const std::uint8_t rid[PPCP_RV_RID_BYTES]);

// The decision of 3.4b/3.4c.  `resolve` is PpcpRendezvous::resolveRid — it
// returns the pairing handle on a match and an empty string otherwise.
struct DialDecision {
    bool        dial = false;
    std::string pairingId;      // empty unless `dial`
    // Diagnostic only.  RV 7.2b is satisfied because none of these name a key,
    // a secret or a payload; `rid` is published in the clear by design (3.4e).
    const char *why = "";
};
using RidResolver =
    std::function<std::string(const std::uint8_t rn[PPCP_RV_RN_BYTES],
                              const std::uint8_t rid[PPCP_RV_RID_BYTES])>;

DialDecision decideDial(const RvAdvertisement &ad, int wireMajor, const RidResolver &resolve);

// ── The platform browser ────────────────────────────────────────────────────
// Wraps the system DNS-SD client.  Owns no thread: it exposes the client
// socket so a Qt event loop (or a headless harness) can watch it and call
// process() when it is readable.  Returns null where the platform has no
// DNS-SD client, and that is not an error either (3.6a).
class RvBrowser {
public:
    virtual ~RvBrowser() = default;

    // Called for each instance that resolves far enough to carry rn and rid.
    // Called on whichever thread invoked process().
    using FoundFn = std::function<void(const RvAdvertisement &)>;
    using LostFn = std::function<void(const std::string &instanceName)>;

    virtual bool start(FoundFn onFound, LostFn onLost) = 0;
    virtual void stop() = 0;
    // The client socket to watch for readability, or -1.
    virtual int  fd() const = 0;
    // Handle whatever is ready.  Returns false once the browse has died, which
    // 3.6a makes a reason to stop watching and NOT a reason to report a fault.
    virtual bool process() = 0;
    // Whether this build has a browser at all, for the diagnostic export.
    virtual std::string describe() const = 0;
};

std::unique_ptr<RvBrowser> makePlatformBrowser();

// ── The advertisement half (RV 3.5e, CR-01 CA5) — H9 ────────────────────────
//
// ⚠ THE OBJECTION AT THE TOP OF THIS FILE DOES NOT REACH `DNSServiceRegister`,
// AND THAT IS WHY THIS IS ADDITIVE RATHER THAN A REVERSAL.  What 3.5b calls
// "a responder, which a mobile platform supplies and several desktop platforms
// do not" is the thing that OWNS UDP 5353 and answers queries for the whole
// machine.  On macOS that is mDNSResponder, it is already running, and
// `DNSServiceRegister` asks it — over the same local IPC socket
// `DNSServiceBrowse` above already uses — to publish a record on our behalf.
// This process still binds no multicast socket and still answers no query, so
// the "no QUdpSocket anywhere in this file" rule stands unchanged.
//
// ⚠ AND 3.5d — the clause that FORBIDS most desktops from advertising — is
// satisfied here rather than assumed.  It bars a peer whose platform cannot
// resolve a PSK identity server-side, because 5.3b needs the listener to
// recompute `tag` with the `K_id` of every pairing held.  This host has that
// hook: `PpcpRendezvous::identityResolver()` is installed on the listener at
// start-up.  A peer without it would be discoverable and unable to complete
// the handshake it advertised for, which is why the clause exists.
//
// ⚠ WINDOWS IS DEFERRED, AND IT IS A DEPENDENCY DECISION AND NOT A PROTOCOL
// ONE (CA5).  There is no `dns_sd.h` on Windows without Apple's Bonjour SDK,
// which is an installer and a service rather than a header.  So
// `makePlatformAdvertiser()` returns null there, exactly as
// `makePlatformBrowser()` does, and 3.6b makes that silent: the pairing code
// path is unaffected and the user sees a code prompt rather than a failure.
// Recorded here rather than left as an `#ifdef` nobody reads.

// 3.2d — a registration nonce is four CSPRNG bytes, drawn fresh for EACH
// REGISTRATION, and the instance name is derived from it and from nothing
// else.  3.2b is satisfied by construction: a value drawn per registration
// persists across nothing, so there is no device name, no user name and no
// value that outlives the registration anywhere in it.
inline constexpr std::size_t PPCP_RV_REG_NONCE_BYTES = 4;

// `PPCP-` followed by eight uppercase hexadecimal characters, which is the
// same FORM as 3.2a's rid-derived name and deliberately indistinguishable from
// it.  Nothing reads the derivation: 3.4b resolves `rid` from the TXT record,
// never from the name.
bool registrationInstanceName(const std::uint8_t nonce[PPCP_RV_REG_NONCE_BYTES],
                              std::string *out);

// The 3.3a TXT record of a RECONNECTION instance.  A bootstrap instance
// (3.3f/3.7) is a different set of keys and is NOT built here — H10 owns it.
struct RvTxtFields {
    std::string  pv = "1.0";      // 3.3d's range syntax; ours is a bare LOW
    std::string  role = "host";   // 3.5e — this peer advertises as the host
    std::uint8_t rn[PPCP_RV_RN_BYTES]{};
    std::uint8_t rid[PPCP_RV_RID_BYTES]{};
};

// Serialises to DNS-SD's length-prefixed form.  Returns false if the result
// would breach 3.3c's 200 bytes, which for the fixed key set of 3.3a it never
// does — the check is there so that a future key cannot quietly grow the
// record past a single response.
//
// ⚠ 3.3b IS ENFORCED BY THE SHAPE OF `RvTxtFields`, not by a filter.  There is
// no field on it for a device name, a `Peer.id`, a serial number or a count of
// sessions, so no call site can put one in the record by accident.  That is
// what RT-7 reads.
bool buildTxtRecord(const RvTxtFields &f, std::vector<std::uint8_t> *out);

// ── 3.4a / 3.4d1 / 3.4d3 as amended by E55 — how fast to rotate ─────────────
//
// 3.4d1 advertises ONE pairing at a time and rotates which one, so a device
// waits up to `period x pairings held` before it sees an advertisement it can
// resolve.  3.4d2's mitigation — browse as well as advertise — is unavailable
// to this peer: its counterparts cannot advertise (3.5d), so there is nothing
// to find.  That leaves the rotation period as the only control.
//
// ⚠ SIZED ON PAIRINGS *HELD*, NOT ON DEVICES *PRESENT* (E55).  7.4a gives a
// persisted pairing no expiry — it ends on revocation (7.4d) and on nothing
// else — so this host accumulates every device it has ever paired with until
// somebody prunes them by hand.  The count that sets the wait is that
// accumulated total, which is why this function takes `pairingsHeld` and not
// a count of anything currently connected.
//
//   0 pairings  — nothing to advertise; the caller does not register at all.
//   1 pairing   — there is no wrong pairing to be on, so the wait is zero and
//                 the only constraint is 3.4a's MUST that `rn` rotate at least
//                 every 15 minutes.  That is the period.
//   n >= 2      — aim at a full cycle of `kAdvertisementCycleTargetS`, floored
//                 so that a host with a season of pairings does not rotate
//                 every second.  Three pairings gives twenty seconds, which is
//                 3.4d3's own worked example for a studio.
//
// Rotation is affordable only because of 3.2d: the instance name is stable for
// the registration, so a rotation is ONE TXT update rather than a deregister /
// probe / announce cycle.  E49 corrected the clause on exactly this point, and
// `RvReconnectionAdvertisement` below is built to keep it true.
inline constexpr unsigned kAdvertisementCycleTargetS = 60;
inline constexpr unsigned kAdvertisementMinPeriodS   = 10;
inline constexpr unsigned kAdvertisementMaxPeriodS   = 900;   // 3.4a's MUST

unsigned rotationPeriodSeconds(std::size_t pairingsHeld);

// ── The platform advertiser ─────────────────────────────────────────────────
// The mirror of `RvBrowser`: owns no thread and no clock, exposes the client
// socket for the owner's event loop, and returns null where the platform has
// no DNS-SD responder to ask.
class RvAdvertiser {
public:
    virtual ~RvAdvertiser() = default;

    // Registers `_ppcp._tcp` under `instanceName` at `port`.  `port` is in
    // host byte order.  Returns false when the responder refuses, which 3.6a
    // makes a reason to advertise nothing rather than a reason to report a
    // fault.
    virtual bool start(const std::string &instanceName, std::uint16_t port,
                       const std::vector<std::uint8_t> &txt) = 0;

    // 3.2d's whole point: a rotation replaces the TXT record of an existing
    // registration and does NOT re-register.  A call that re-registered would
    // rename the service and trigger the probe/announce storm E49 describes.
    virtual bool updateTxt(const std::vector<std::uint8_t> &txt) = 0;

    // Withdraws the instance.  3.7b withdraws a bootstrap instance on close;
    // a reconnection instance is withdrawn when the host stops listening.
    virtual void stop() = 0;

    virtual int  fd() const = 0;
    virtual bool process() = 0;
    virtual std::string describe() const = 0;

    // The name the responder settled on.  It is not necessarily the one asked
    // for: a conflict makes mDNSResponder rename, and the renamed instance is
    // still conformant because 3.2b binds what the name MAY NOT contain and a
    // suffix contains none of it.  Empty until the registration is confirmed.
    virtual std::string registeredName() const = 0;
};

std::unique_ptr<RvAdvertiser> makePlatformAdvertiser();

// ── The rotation driver ─────────────────────────────────────────────────────
//
// Qt-free, clock-injected and socket-free, like everything else in this file,
// so the suite can run a hundred rotations without a responder or an event
// loop.  It holds NO key material: `RidMinter` is the seam, and the only thing
// that crosses it is the eight bytes of `rn` and the eight of `rid` that 3.4e
// publishes in the clear by design.  `K_id` stays in the rendezvous ledger.
class RvReconnectionAdvertisement {
public:
    // Draws `rn` and computes `rid` for one held pairing (3.4a/3.4b).  Returns
    // false if the pairing is gone or the CSPRNG failed, and a false is a
    // rotation skipped rather than an error — 3.6a again.
    using RidMinter = std::function<bool(const std::string &pairingId,
                                         std::uint8_t rn[PPCP_RV_RN_BYTES],
                                         std::uint8_t rid[PPCP_RV_RID_BYTES])>;
    // The registration nonce of 3.2d comes from here, so a test can pin the
    // instance name and the production path can use the same CSPRNG every
    // other secret in this application comes from.
    using RandomFn = std::function<bool(void *out, std::size_t len)>;

    RvReconnectionAdvertisement(RvAdvertiser *advertiser, RidMinter mint, RandomFn rng);

    // `pairings` is most-recently-used first — 3.4d1's SHOULD, "because the
    // counterpart a user is standing in front of is usually the one they used
    // last".  An empty list registers nothing.
    bool start(std::uint16_t port, const std::vector<std::string> &pairings,
               std::uint64_t nowS);

    // The ledger changed.  Keeps the registration and its instance name — a
    // pairing being added or revoked is not a new REGISTRATION under 3.2d —
    // unless the list has become empty, which withdraws the instance.
    void setPairings(const std::vector<std::string> &pairings, std::uint64_t nowS);

    void stop();

    // Call as often as convenient.  Rotates when one is due and returns true
    // when it did; everything else is an integer comparison.
    bool tick(std::uint64_t nowS);

    bool          active() const;
    unsigned      periodSeconds() const;
    std::uint64_t nextRotationAtS() const;
    std::string   instanceName() const;      // what we asked to be called
    std::string   currentPairingId() const;  // whose `rid` is on the wire now
    std::size_t   rotations() const;
    // For the RT-9 diagnostic export.  Names no pairing and no key: a count, a
    // period and the service type, which is all 7.2b permits.
    std::string   describe() const;

private:
    bool rotate(std::uint64_t nowS, bool firstRegistration);

    RvAdvertiser *m_adv = nullptr;
    RidMinter     m_mint;
    RandomFn      m_rng;
    std::vector<std::string> m_pairings;
    std::size_t   m_at = 0;
    std::string   m_instanceName;
    std::uint16_t m_port = 0;
    bool          m_active = false;
    std::uint64_t m_nextS = 0;
    std::size_t   m_rotations = 0;
    std::string   m_current;
};

}  // namespace Ppcp
