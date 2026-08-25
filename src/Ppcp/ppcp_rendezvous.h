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

// PPCP-RV rendezvous, host side — work package H6.
//
// This host is the CODE PUBLISHER (RV §2): it displays a pairing code, it
// listens, and the peer that scans dials it.  That direction settles three
// things at once — the host is the TLS server (5.2g), the host sends
// `hello_accept` rather than `hello` (2d), and the host holds the authoritative
// clock for expiry (7.3e).
//
// ⚠ WHAT IS HERE AND WHAT IS libppcp'S.  Plan A7/A8: the library owns the code
// payload, the derivation, the identifiers and the resolvers, and owns no
// socket, no storage and NO RANDOM NUMBER GENERATOR.  Every random value in RV
// is a parameter to a libppcp function, which is exactly why RT-12 is a review
// row: the one requirement the whole model rests on is the one nothing on the
// wire can check.  So this file holds the CSPRNG, the endpoint enumeration, the
// outstanding-code table, the persistence decision and the diagnostic export —
// and re-derives NOTHING.  Every byte of key material below came out of
// ppcp_rv_derive(), ppcp_rv_rid() or ppcp_rv_psk_identity().
//
// Deliberately Qt-free, like ppcp_transport.h, so the H8 harness can drive it
// headless and so the tests need no event loop.  The QML face of it is
// ppcp_rendezvous_controller.h.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ppcp/rv.h>

#include "ppcp_transport.h"

namespace Ppcp {

// ── RV 7.2a — the CSPRNG, at full width ─────────────────────────────────────
// "A predictable secret defeats the entire model, and it is the single easiest
// thing to get wrong."  getentropy(2) is the kernel's own pool on macOS and
// Linux alike, it takes no seeding and it CANNOT silently return weak bytes —
// it either fills the buffer or fails.  Returns false on failure, and every
// caller below treats that as fatal to the operation rather than falling back
// to anything.  There is no fallback: a code minted from a degraded source is
// worse than no code, because it looks identical.
//
// RT-12 reads this function and the four call sites of it.
bool csprngBytes(void *out, std::size_t len);

// ── RV 4.3d — every address the publisher is reachable at ───────────────────
// "wired, wireless, and its hotspot address where it provides one.  This is
// what makes the code work when discovery does not."  Enumerated from the OS
// rather than from configuration, most preferred first (4.3), so a machine that
// gains an interface gains an endpoint with no edit here.
struct RvEndpoint {
    std::string   host;
    std::uint16_t port = 0;
};

// Ordering is 4.3's "most preferred first" made concrete: wired before
// wireless before anything else, IPv4 before IPv6, and loopback LAST because a
// code that leads with 127.0.0.1 is a code that only works on this machine.
// Link-local IPv6 is omitted entirely: it needs a scope id the other end
// cannot use.
std::vector<RvEndpoint> reachableEndpoints(std::uint16_t port);

// ── RV §6 — network join credentials, where the host provides the network ───
struct RvWifi {
    std::string ssid;
    bool        hasPassphrase = false;
    std::string passphrase;
    bool        hidden = false;
};

// ── One published code ──────────────────────────────────────────────────────
// ⚠ `uri` IS THE SECRET.  It is the whole payload — psk, sid and, where one is
// set, the network passphrase (6c).  4.4c and 7.2b forbid it reaching a log, a
// crash report, an analytics event or a diagnostic export.  It exists to be
// rendered as a QR code and for nothing else, which is why it is returned once
// from publish() and never stored on the outstanding-code row.
struct PublishedCode {
    std::string   pairingId;    // opaque local handle; never sent anywhere
    std::string   uri;          // ⚠ SECRET — ppcp:<base64url(payload)>
    std::string   sessionId;    // 4.3e — canonical lowercase UUID text of sid
    std::uint64_t maxUses = 1;
    std::uint64_t expUnixS = 0;
};

// A row of the outstanding-code table, as it may be DISPLAYED and EXPORTED.
// Nothing here is a secret and nothing here is a payload field: that is the
// property RT-9 asserts, and keeping the secret off this struct is how it is
// made true by construction rather than by a filter somebody has to maintain.
struct CodeStatus {
    std::string   pairingId;
    std::string   sessionId;
    std::string   displayName;      // our OWN dn — we printed it, so it is ours
    std::uint64_t maxUses = 1;
    std::uint64_t usesRemaining = 1;
    std::uint64_t expUnixS = 0;
    bool          invalidated = false;
    bool          persisted = false;   // 7.4a — a pairing, no longer just a code
    std::vector<RvEndpoint> endpoints;
};

// ── RV 7.2c / 7.4 — where a persisted PRK lives ─────────────────────────────
// 7.2c is a SHOULD (libppcp erratum E56, 25 August 2026), and this application
// does not use protected storage: the PRK lives in the app's own settings, on
// every platform.  The interface is here so the rendezvous logic never sees a
// storage API and so a test can substitute a store that holds nothing at all.
// See ppcp_pairing_store.cpp for what that trades away and why it was judged
// worth it — the short version is that the keychain path existed on macOS only,
// so Windows and Linux persisted nothing and could never reconnect.
//
// The unit of storage is PRK and only PRK (5.1c): a peer that persists a
// pairing persists PRK and derives from it, never the original psk.  psk never
// reaches this interface, which is why it cannot be persisted by accident.
class PairingSecretStore {
public:
    virtual ~PairingSecretStore() = default;
    virtual bool put(const std::string &pairingId,
                     const std::uint8_t prk[PPCP_RV_KEY_BYTES]) = 0;
    virtual bool get(const std::string &pairingId,
                     std::uint8_t prk[PPCP_RV_KEY_BYTES]) const = 0;
    virtual bool erase(const std::string &pairingId) = 0;
    virtual std::vector<std::string> list() const = 0;
    // What the diagnostic export is allowed to say about it (RT-9): a name and
    // a count, never a byte of a key.
    virtual std::string describe() const = 0;
};

// The application's own settings store, on every platform.  ⚠ Never null: it
// returned null off Apple until erratum E56, which is precisely why Windows and
// Linux could persist no pairing and reconnection could never work there.
std::unique_ptr<PairingSecretStore> makePlatformPairingStore(const std::string &service);

// An in-memory store for tests and for a run that declines persistence.  It is
// NOT protected storage and says so in describe().
std::unique_ptr<PairingSecretStore> makeEphemeralPairingStore();

// ── The publisher ───────────────────────────────────────────────────────────
class PpcpRendezvous {
public:
    struct Config {
        // 7.3c — "the shortest expiry the workflow tolerates".  Five minutes is
        // long enough to walk to the bay and short enough that a photograph of
        // the screen is worth little.  7.3a and 7.3b are the primary defence
        // and are clock-free; this is secondary and says so.
        std::uint64_t codeLifetimeS = 300;
        // 7.3a.  Default 1, and 7.4f is the price of anything above it.
        std::uint64_t maxUses = 1;
        // 4.3 / 4.4d — at most 64 bytes.  Ours to print; UNTRUSTED at the far
        // end, which is the far end's problem and is documented there.
        std::string   displayName;
    };

    PpcpRendezvous();
    ~PpcpRendezvous();
    PpcpRendezvous(const PpcpRendezvous &) = delete;
    PpcpRendezvous &operator=(const PpcpRendezvous &) = delete;

    // Persistence is OFF until an owner installs a store: 7.4b makes it opt-in
    // and individually revocable, and a default-on protected store would be
    // neither.  Passing null returns to no persistence and erases nothing —
    // revoke() is how a stored pairing goes away.
    void setSecretStore(std::unique_ptr<PairingSecretStore> s);
    const PairingSecretStore *secretStore() const { return m_store.get(); }

    // The wall clock, seconds since the Unix epoch.  Injectable because 7.3e
    // makes the PUBLISHER the party holding the authoritative clock, so every
    // expiry assertion in the suite has to be able to move it.
    using NowFn = std::function<std::uint64_t()>;
    void setClock(NowFn f);

    // ── 7.3d — a fresh psk and sid for EVERY code ──────────────────────────
    // "A code is never regenerated with the same secret."  Both come from
    // csprngBytes() here, at full width, and the previous code's material is
    // not consulted.  Fails — returning false and publishing nothing — if the
    // CSPRNG fails or if there is no endpoint to advertise, because a code
    // with an empty `ep` is a code nothing can dial (4.3c).
    bool publish(const Config &cfg, const std::vector<RvEndpoint> &endpoints,
                 const RvWifi *wifi, PublishedCode *out, std::string *err = nullptr);

    // ── RV 5.3b — the server side of the handshake ─────────────────────────
    // Hand this to Listener::setIdentityResolver().  It recomputes the tag with
    // the K_id of EVERY pairing it holds — outstanding codes and persisted
    // pairings alike — with no early exit and a constant-time comparison, both
    // of which are libppcp's (ppcp_rv_resolve_psk_identity).
    //
    // ⚠ POLICY IS APPLIED AFTER THE RESOLVE AND NEVER BEFORE IT.  Filtering
    // expired or exhausted entries out of the candidate set would make the cost
    // of the resolve a function of how many codes are live, which is exactly
    // the timing signal 5.3d asks to be absent.  So every held entry is offered
    // to the resolver, and a match that is expired (7.3e), exhausted (7.3a) or
    // invalidated (7.3b) returns false — the same false, on the same path, as
    // an identity nothing resolves (5.3c, 7.7c).
    IdentityResolver identityResolver();

    // 7.3a — "invalidates a pairing code once `mu` handshakes have COMPLETED".
    //
    // ⚠ ONE CALL PER *LINK*, NOT PER TLS HANDSHAKE, AND THAT IS A DEFECT
    // REPORT AGAINST THE SPECIFICATION rather than a liberty taken here.  RV
    // 7.3a counts HANDSHAKES; `PPCP-CORE` T1/T2 and `PPCP-ENC` §2.1 require two
    // (optionally three) TCP connections per link, each its own TLS session
    // keyed by the same K_tls.  So a `mu: 1` code — the DEFAULT, and the
    // pairwise case the whole model is built around — would be invalidated by
    // the control channel's handshake and the bulk channel of the same link
    // would then be refused, leaving every conformant PPCP-RV pairing dead on
    // its second channel.  `mu` can only mean PAIRINGS.  See finding F-H6-1.
    //
    // Completed, not attempted: the resolver runs mid-handshake and a link that
    // then fails to bind must not consume a use.  So the count moves here,
    // called once by whoever accepted the link, and never in the resolver.
    void noteLinkEstablished(const std::string &pairingId);

    // 7.3b — "invalidates the code when the session it belongs to closes,
    // whether or not it was used".  Erases key material with it (7.2d) unless
    // the pairing was persisted under 7.4.
    void closeSession(const std::string &sessionId);

    // 7.4b/7.4d — individually revocable, honoured immediately by this side.
    // Erases the key material and the stored PRK, so the next handshake for
    // that pairing resolves nothing and fails like any stranger (7.7c).
    void revoke(const std::string &pairingId);

    // ── RV 11.5g / 11.1a — a guided pairing becomes an ORDINARY pairing ────
    //
    // 11.1a is the point of `ppcp_bs_pairing`'s shape: "from here the pairing
    // is INDISTINGUISHABLE from one established by a scanned code, so §5, §7.4
    // and §7.5 apply verbatim and are unchanged by §11".  So this is the only
    // function §11 needs from this class, and it produces an entry the resolver,
    // the advertiser and the Phones panel cannot tell from a code's.
    //
    // ⛔ CALL ONLY AFTER 11.5g.  "The pairing exists only when a peer has BOTH
    // affirmed at its own end and verified the counterpart's MAC.  Until then it
    // holds nothing and MUST NOT persist, advertise, or offer anything derived
    // from the exchange."  `GuidedAttempt::takePairing()` refuses in any other
    // phase, which is what makes that a property rather than a comment.
    //
    // ⚠ NO EXPIRY AND NO USE LIMIT, AND THAT IS NOT A LIBERTY.  Those are
    // properties of a CODE (7.4a) and a guided pairing is not one: there is no
    // `exp` to honour and no `mu` to count down, because nothing was printed.
    // 7.4f does not bite either — the material was never held by anyone who
    // scanned anything — so `mayPersist` is true and 7.4b's opt-in is the only
    // gate on persistence, exactly as it is for a `mu: 1` code.
    //
    // ⚠ AND IT IS NOT PERSISTED HERE.  7.4b makes persistence opt-in, visible
    // and individually revocable; `persist()` is still the user's action.
    //
    // False if the CSPRNG cannot draw a handle.  `out` receives the pairingId.
    bool adoptGuidedPairing(const std::uint8_t sid[PPCP_RV_SID_BYTES],
                            const ppcp_rv_keys &keys,
                            const std::string &displayName,
                            std::string *out, std::string *err = nullptr);

    // 7.4a — persist this pairing so a later session needs no new code.
    // Refuses, returning false, when the code's `mu` exceeded 1 (7.4f: the key
    // material is held by every peer that scanned it) or when no protected
    // store is installed (7.2c).  ppcp_rv_may_persist() is the predicate and
    // this function does not second-guess it.
    bool persist(const std::string &pairingId, std::string *whyNot = nullptr);
    bool isPersisted(const std::string &pairingId) const;

    // Reload persisted pairings from the store at start-up.  Each becomes a
    // resolvable entry with no `exp` and no use limit — those are properties of
    // a CODE, and a persisted pairing is not one (7.4a).
    std::size_t loadPersisted();

    // ── The advertisement side of the resolver (3.4b) ──────────────────────
    // Resolve a discovered `rid` against every pairing held.  Returns the
    // pairingId on a match and an empty string otherwise, which 3.4c makes a
    // refusal to connect.  The caller never gets to see "close enough".
    // RV 5.3a1 (erratum E21) — the PSK identity to offer when THIS peer dials
    // (the discovery path; on the pairing-code path the scanner dials and draws
    // its own).  Draws `rn2` until neither it nor the tag carries a `0x00`
    // octet, because a TLS stack that lengths the identity with `strlen`
    // truncates it and the handshake fails one connection in sixteen.
    //
    // ⚠ NOTHING ELSE IN THIS APPLICATION MAY CALL `ppcp_rv_psk_identity()` FOR
    // A CONNECTION.  That entry point does not reject a zero-bearing draw, on
    // purpose, so §10.2's test vector still reproduces byte for byte — which
    // makes it correct for a vector and wrong for a socket.
    bool drawPskIdentity(const std::string &pairingId, PskIdentity *out,
                         std::string *whyNot = nullptr) const;

    std::string resolveRid(const std::uint8_t rn[PPCP_RV_RN_BYTES],
                           const std::uint8_t rid[PPCP_RV_RID_BYTES]) const;

    // ── The ADVERTISEMENT side of the same identifiers (3.4a) — H9 ─────────
    //
    // 3.5e makes this host the peer that advertises, because its counterpart
    // is barred from doing so by 3.5d.  These two functions are the whole of
    // what the advertiser needs from the ledger, and between them they are the
    // reason `K_id` never leaves this class: the advertiser receives eight
    // bytes of `rn` and eight of `rid`, both of which 3.4e publishes in the
    // clear on purpose, and nothing else.
    //
    // 3.4d1 — "a peer holding several pairings advertises exactly one service
    // instance at a time, and rotates which pairing it advertises", SHOULD
    // "advertise a recently used pairing first".  This returns the candidates
    // in exactly that order: most recently used first, and pairings never used
    // this run last, in ledger order.
    //
    // ⚠ ONLY PERSISTED PAIRINGS (7.4a).  An outstanding CODE is dialled by the
    // peer that scanned it, at the endpoint printed in the code itself (4.3d),
    // so advertising its `rid` would offer a reconnection to a device that has
    // never connected once.  §3 is "reconnection convenience", and a pairing
    // that has not happened yet has nothing to reconnect to.
    std::vector<std::string> advertisablePairings() const;

    // Draws a fresh `rn` from the CSPRNG and computes `rid` with the `K_id` of
    // one held pairing.  False if the pairing is not held, has been revoked or
    // invalidated, or the CSPRNG failed — and a false is one rotation skipped,
    // never an error state (3.6a).
    //
    // ⚠ THIS IS `ppcp_rv_rid()` AND NOTHING ELSE.  It re-derives no key: `K_id`
    // came out of `ppcp_rv_derive()` when the pairing was made and is copied,
    // never recomputed, here.
    bool mintRid(const std::string &pairingId,
                 std::uint8_t rn[PPCP_RV_RN_BYTES],
                 std::uint8_t rid[PPCP_RV_RID_BYTES]) const;

    // ── The table ──────────────────────────────────────────────────────────
    std::vector<CodeStatus> codes() const;
    bool status(const std::string &pairingId, CodeStatus *out) const;

    // Purge rows that can never authenticate again — expired, exhausted,
    // invalidated and not persisted — and erase their key material (7.2d).
    // Separate from the checks in the resolver because the resolver MUST NOT
    // change the size of the candidate set as a side effect of being called.
    std::size_t reap();

    // ── RT-9 — the diagnostic export ───────────────────────────────────────
    // 7.2b: no pairing secret, no derived key and no decoded payload in a log,
    // a crash report, an analytics event or a diagnostic export.  4.4c adds the
    // payload itself.  This function is the whole of what this subsystem
    // contributes to a diagnostic bundle, and the assertion is that it is
    // constructed from CodeStatus — a struct that HOLDS none of those — rather
    // than filtered afterwards.
    std::string diagnosticExport() const;

    // What the resolver has seen, for the export.  Counters only.
    struct Counters {
        std::uint64_t resolveCalls = 0;
        std::uint64_t resolved = 0;
        std::uint64_t refusedNoPairing = 0;
        std::uint64_t refusedExpired = 0;
        std::uint64_t refusedExhausted = 0;
        std::uint64_t refusedInvalidated = 0;
    };
    Counters counters() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<PairingSecretStore> m_store;
};

}  // namespace Ppcp
