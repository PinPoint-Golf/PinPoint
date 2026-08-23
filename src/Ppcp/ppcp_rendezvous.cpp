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

#include "ppcp_rendezvous.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>

namespace Ppcp {
namespace {

// Zeroing that a compiler may not elide.  7.2d asks for key material to be
// erased; a memset the optimiser removes is not an erasure.
void wipe(void *p, std::size_t n)
{
    volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
    while (n--) *v++ = 0;
}

std::string toHex(const std::uint8_t *b, std::size_t n)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[b[i] >> 4]);
        s.push_back(d[b[i] & 0x0f]);
    }
    return s;
}

std::uint64_t wallClockNow()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace

// ── RV 7.2a ─────────────────────────────────────────────────────────────────
bool csprngBytes(void *out, std::size_t len)
{
    // getentropy() is capped at 256 bytes per call on both platforms that have
    // it, so the loop is not optional.  There is deliberately no fallback: on a
    // failure the caller abandons the operation.  An RNG that quietly degrades
    // is the single failure mode RT-12 exists to catch and the one the wire
    // cannot show.
    unsigned char *p = static_cast<unsigned char *>(out);
    while (len > 0) {
        const std::size_t chunk = len > 256 ? 256 : len;
        if (::getentropy(p, chunk) != 0) return false;
        p += chunk;
        len -= chunk;
    }
    return true;
}

// ── RV 4.3d ─────────────────────────────────────────────────────────────────
std::vector<RvEndpoint> reachableEndpoints(std::uint16_t port)
{
    std::vector<RvEndpoint> routable, loopback;

    struct ifaddrs *ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return {};

    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
        if (!(p->ifa_flags & IFF_UP)) continue;
        if (p->ifa_flags & IFF_POINTOPOINT) continue;   // a tunnel is not a bay

        char buf[INET6_ADDRSTRLEN] = {0};
        const bool lo = (p->ifa_flags & IFF_LOOPBACK) != 0;

        if (p->ifa_addr->sa_family == AF_INET) {
            auto *sin = reinterpret_cast<struct sockaddr_in *>(p->ifa_addr);
            if (!::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) continue;
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            auto *sin6 = reinterpret_cast<struct sockaddr_in6 *>(p->ifa_addr);
            // A link-local address needs a scope id the scanner cannot use, and
            // 4.3c has it try every entry in turn — so an unusable entry is a
            // wasted dial and a longer code for nothing.
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
            if (!::inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof buf)) continue;
        } else {
            continue;
        }

        RvEndpoint e;
        e.host = buf;
        e.port = port;
        (lo ? loopback : routable).push_back(e);
    }
    ::freeifaddrs(ifa);

    // 4.3 "most preferred first".  IPv4 before IPv6 within each group, because
    // a range network that has IPv6 at all usually has IPv4 too and the
    // reverse is not true; loopback last, because a code that leads with
    // 127.0.0.1 works only on the machine that displayed it.
    auto v4first = [](const RvEndpoint &a, const RvEndpoint &b) {
        const bool av4 = a.host.find(':') == std::string::npos;
        const bool bv4 = b.host.find(':') == std::string::npos;
        return av4 && !bv4;
    };
    std::stable_sort(routable.begin(), routable.end(), v4first);
    std::stable_sort(loopback.begin(), loopback.end(), v4first);
    routable.insert(routable.end(), loopback.begin(), loopback.end());

    // 4.5a wants the payload under 400 bytes and libppcp caps the array at
    // PPCP_RV_MAX_ENDPOINTS anyway.  Truncating the LEAST preferred is the only
    // truncation that keeps 4.3c meaningful.
    if (routable.size() > PPCP_RV_MAX_ENDPOINTS) routable.resize(PPCP_RV_MAX_ENDPOINTS);
    return routable;
}

// ── The publisher ───────────────────────────────────────────────────────────

struct PpcpRendezvous::Impl {
    // One held pairing.  A CODE and a PAIRING are the same row at different
    // points in its life (5.3b resolves both against the same identity), which
    // is why there is one struct and a `usesRemaining` rather than two tables.
    struct Entry {
        std::string   pairingId;
        std::string   sessionId;
        std::string   displayName;
        std::uint8_t  prk[PPCP_RV_KEY_BYTES]{};
        std::uint8_t  kId[PPCP_RV_KEY_BYTES]{};
        std::uint8_t  kTls[PPCP_RV_KEY_BYTES]{};
        std::uint64_t maxUses = 1;
        std::uint64_t usesRemaining = 1;
        std::uint64_t expUnixS = 0;      // 0 = no expiry (a persisted pairing)
        bool          invalidated = false;
        bool          persisted = false;
        bool          mayPersist = true;  // 7.4f, decided at publish time
        std::vector<RvEndpoint> endpoints;

        void wipeKeys()
        {
            wipe(prk, sizeof prk);
            wipe(kId, sizeof kId);
            wipe(kTls, sizeof kTls);
        }
    };

    mutable std::mutex mu;
    std::vector<Entry> entries;
    NowFn              now = wallClockNow;
    Counters           counters;

    Entry *find(const std::string &id)
    {
        for (auto &e : entries)
            if (e.pairingId == id) return &e;
        return nullptr;
    }
    const Entry *find(const std::string &id) const
    {
        for (auto &e : entries)
            if (e.pairingId == id) return &e;
        return nullptr;
    }
};

PpcpRendezvous::PpcpRendezvous() : m_impl(new Impl) {}

PpcpRendezvous::~PpcpRendezvous()
{
    // 7.2d — every key this object still holds dies with it.  A pairing that
    // was persisted lives on in the store as PRK and nowhere else.
    std::lock_guard<std::mutex> g(m_impl->mu);
    for (auto &e : m_impl->entries) e.wipeKeys();
}

void PpcpRendezvous::setSecretStore(std::unique_ptr<PairingSecretStore> s)
{
    m_store = std::move(s);
}

void PpcpRendezvous::setClock(NowFn f)
{
    std::lock_guard<std::mutex> g(m_impl->mu);
    m_impl->now = f ? std::move(f) : NowFn(wallClockNow);
}

bool PpcpRendezvous::publish(const Config &cfg, const std::vector<RvEndpoint> &endpoints,
                             const RvWifi *wifi, PublishedCode *out, std::string *err)
{
    auto fail = [&](const char *m) {
        if (err) *err = m;
        return false;
    };
    if (!out) return fail("no output");
    if (endpoints.empty())
        // 4.3 makes `ep` cardinality 1..n and 4.3c has the scanner walk it.  A
        // code with nothing to walk is a code that cannot work, and publishing
        // one would be a QR the user photographs and blames.
        return fail("no reachable address to advertise");

    // 7.3d — fresh psk and sid for EVERY code, at full width (4.3g, 7.2a).
    // 32 bytes rather than the 16-byte floor: the payload is 16 characters
    // longer and the code is still comfortably scannable (4.5a).
    std::uint8_t psk[PPCP_RV_PSK_MAX];
    std::uint8_t sid[PPCP_RV_SID_BYTES];
    std::uint8_t handle[8];
    if (!csprngBytes(psk, sizeof psk) || !csprngBytes(sid, sizeof sid) ||
        !csprngBytes(handle, sizeof handle)) {
        wipe(psk, sizeof psk);
        wipe(sid, sizeof sid);
        return fail("the platform CSPRNG failed");
    }

    ppcp_rv_payload p;
    ppcp_rv_payload_init(&p);
    p.v = 1;
    for (const auto &e : endpoints) {
        if (ppcp_rv_payload_add_endpoint(&p, e.host.c_str(), e.host.size(), e.port) != PPCP_OK)
            break;   // PPCP_RV_MAX_ENDPOINTS; the preferred ones are already in
    }
    if (ppcp_rv_payload_set_secret(&p, psk, sizeof psk, sid) != PPCP_OK) {
        wipe(psk, sizeof psk);
        return fail("libppcp refused the secret");
    }
    if (ppcp_rv_payload_set_max_uses(&p, cfg.maxUses ? cfg.maxUses : 1) != PPCP_OK) {
        wipe(psk, sizeof psk);
        return fail("libppcp refused mu");
    }

    const std::uint64_t nowS = m_impl->now();
    std::uint64_t expS = 0;
    if (cfg.codeLifetimeS > 0) {
        expS = nowS + cfg.codeLifetimeS;
        if (ppcp_rv_payload_set_expiry(&p, expS) != PPCP_OK) {
            wipe(psk, sizeof psk);
            return fail("libppcp refused exp");
        }
    }
    if (!cfg.displayName.empty() &&
        ppcp_rv_payload_set_display_name(&p, cfg.displayName.c_str(),
                                         cfg.displayName.size()) != PPCP_OK) {
        wipe(psk, sizeof psk);
        return fail("the display name is longer than the 64 bytes 4.3 allows");
    }
    if (wifi) {
        ppcp_rv_wifi w{};
        w.s = wifi->ssid.c_str();
        w.s_len = wifi->ssid.size();
        w.has_k = wifi->hasPassphrase;
        w.k = wifi->passphrase.c_str();
        w.k_len = wifi->passphrase.size();
        w.has_h = true;
        w.h = wifi->hidden;
        if (ppcp_rv_payload_set_wifi(&p, &w) != PPCP_OK) {
            wipe(psk, sizeof psk);
            return fail("libppcp refused the wifi block");
        }
    }
    if (ppcp_rv_payload_validate(&p) != PPCP_OK) {
        wipe(psk, sizeof psk);
        return fail("the payload libppcp built does not validate");
    }

    char uri[PPCP_RV_MAX_URI];
    std::size_t uriLen = 0;
    if (ppcp_rv_uri_encode(&p, uri, sizeof uri, &uriLen) != PPCP_OK) {
        wipe(psk, sizeof psk);
        return fail("the payload does not fit a pairing code");
    }

    // 5.1 — derived HERE, once, from the bytes we just minted.  Nothing in this
    // repository re-implements HKDF (plan A7); this is the only place a key
    // comes into existence and it comes out of libppcp.
    ppcp_rv_keys keys;
    const ppcp_result dr = ppcp_rv_derive(sid, sizeof sid, psk, sizeof psk, &keys);
    // 7.2a's companion obligation: the psk has done its whole job by this line.
    // Everything after this point uses PRK, K_tls and K_id, so the pairing
    // secret ceases to exist in this process the moment it is no longer needed
    // — and in particular it never reaches the entry below.
    wipe(psk, sizeof psk);
    if (dr != PPCP_OK) {
        wipe(uri, sizeof uri);
        return fail("libppcp refused the derivation");
    }

    char sessionId[PPCP_RV_SESSION_ID_CHARS] = {0};
    if (ppcp_rv_sid_to_session_id(sid, sessionId) != PPCP_OK) {
        wipe(&keys, sizeof keys);
        wipe(uri, sizeof uri);
        return fail("sid does not render as a Session.id");
    }

    Impl::Entry e;
    e.pairingId = toHex(handle, sizeof handle);
    e.sessionId = sessionId;
    e.displayName = cfg.displayName;
    std::memcpy(e.prk, keys.prk, PPCP_RV_KEY_BYTES);
    std::memcpy(e.kId, keys.k_id, PPCP_RV_KEY_BYTES);
    std::memcpy(e.kTls, keys.k_tls, PPCP_RV_KEY_BYTES);
    e.maxUses = cfg.maxUses ? cfg.maxUses : 1;
    e.usesRemaining = e.maxUses;
    e.expUnixS = expS;
    e.endpoints = endpoints;
    // 7.4f, decided ONCE at publish and stored, so persist() can never be
    // talked into it later by a caller holding a stale idea of `mu`.
    e.mayPersist = ppcp_rv_may_persist(&p);
    wipe(&keys, sizeof keys);
    wipe(sid, sizeof sid);

    out->pairingId = e.pairingId;
    out->uri.assign(uri, uriLen);
    out->sessionId = e.sessionId;
    out->maxUses = e.maxUses;
    out->expUnixS = e.expUnixS;
    wipe(uri, sizeof uri);

    {
        std::lock_guard<std::mutex> g(m_impl->mu);
        m_impl->entries.push_back(std::move(e));
    }
    return true;
}

// ── RV 5.3b ─────────────────────────────────────────────────────────────────
IdentityResolver PpcpRendezvous::identityResolver()
{
    Impl *impl = m_impl.get();
    return [impl](const unsigned char *identity, std::size_t len,
                  ResolvedPairing &out) -> bool {
        std::lock_guard<std::mutex> g(impl->mu);
        impl->counters.resolveCalls++;

        // EVERY entry is offered, with no pre-filter.  See the header: a
        // candidate set whose size depends on which codes are still live is a
        // timing signal, and 5.3d asks for there not to be one.
        std::vector<ppcp_rv_pairing> pairings;
        pairings.reserve(impl->entries.size());
        for (std::size_t i = 0; i < impl->entries.size(); ++i)
            pairings.push_back(ppcp_rv_pairing{impl->entries[i].kId,
                                               reinterpret_cast<void *>(i)});

        std::size_t idx = 0;
        const ppcp_result r = ppcp_rv_resolve_psk_identity(
            pairings.empty() ? nullptr : pairings.data(), pairings.size(),
            identity, len, &idx);
        if (r != PPCP_OK || idx >= impl->entries.size()) {
            impl->counters.refusedNoPairing++;
            return false;
        }

        Impl::Entry &e = impl->entries[idx];

        // 7.3b — invalidated with its session, whether or not it was used.
        if (e.invalidated) {
            impl->counters.refusedInvalidated++;
            return false;
        }
        // 7.3e — the PUBLISHER refuses a handshake for a code past its `exp`.
        // This is the authoritative clock, which is exactly why 4.4a1 lets a
        // scanner with a bad one attempt rather than be locked out.
        if (e.expUnixS != 0 && impl->now() > e.expUnixS) {
            impl->counters.refusedExpired++;
            return false;
        }
        // 7.3a — spent.
        if (e.usesRemaining == 0) {
            impl->counters.refusedExhausted++;
            return false;
        }

        std::memcpy(out.kTls.data(), e.kTls, PPCP_RV_KEY_BYTES);
        out.pairingId = e.pairingId;
        impl->counters.resolved++;
        return true;
    };
}

void PpcpRendezvous::noteLinkEstablished(const std::string &pairingId)
{
    std::lock_guard<std::mutex> g(m_impl->mu);
    Impl::Entry *e = m_impl->find(pairingId);
    if (!e) return;
    if (e->usesRemaining > 0) e->usesRemaining--;
    if (e->usesRemaining == 0 && !e->persisted) {
        // The CODE is spent.  The key material stays until 7.3b or 7.4 disposes
        // of it, because the link this handshake produced is still carrying a
        // session and RV 7.5a reconnects it on the same K_tls.
        //
        // ⚠ THE ROW IS NOT MARKED INVALIDATED HERE.  7.3a is "invalidate the
        // CODE"; 7.3b is "invalidate the pairing when the session closes".
        // Conflating them would end the session at its first reconnection.
    }
}

void PpcpRendezvous::closeSession(const std::string &sessionId)
{
    std::lock_guard<std::mutex> g(m_impl->mu);
    for (auto &e : m_impl->entries) {
        if (e.sessionId != sessionId) continue;
        e.invalidated = true;
        e.usesRemaining = 0;
        // 7.2d — erased on session close UNLESS the pairing was persisted under
        // 7.4, in which case PRK is the point and it lives in the store.
        if (!e.persisted) e.wipeKeys();
    }
}

void PpcpRendezvous::revoke(const std::string &pairingId)
{
    std::unique_ptr<PairingSecretStore> *store = &m_store;
    std::lock_guard<std::mutex> g(m_impl->mu);
    Impl::Entry *e = m_impl->find(pairingId);
    if (!e) return;
    // 7.4d — honoured immediately by this side, which means the NEXT handshake
    // resolves nothing.  Erasing K_id is what makes that true: the identity
    // stops being resolvable at all rather than being resolved and then
    // refused, so a revoked peer is indistinguishable from a stranger (7.7c).
    e->invalidated = true;
    e->usesRemaining = 0;
    e->persisted = false;
    e->wipeKeys();
    if (*store) (*store)->erase(pairingId);
}

bool PpcpRendezvous::persist(const std::string &pairingId, std::string *whyNot)
{
    auto no = [&](const char *m) {
        if (whyNot) *whyNot = m;
        return false;
    };
    if (!m_store)
        // 7.2c — "where one exists".  With no protected storage the conformant
        // answer is no persistence, not a key in a preferences file.
        return no("no protected storage on this platform");

    std::lock_guard<std::mutex> g(m_impl->mu);
    Impl::Entry *e = m_impl->find(pairingId);
    if (!e) return no("no such pairing");
    if (e->invalidated) return no("that pairing has been revoked");
    if (!e->mayPersist)
        // 7.4f — the key material is held by every peer that scanned the code.
        return no("the code was multi-use, so the pairing is session-scoped (RV 7.4f)");

    if (!m_store->put(pairingId, e->prk)) return no("the platform store refused the key");
    e->persisted = true;
    return true;
}

bool PpcpRendezvous::isPersisted(const std::string &pairingId) const
{
    std::lock_guard<std::mutex> g(m_impl->mu);
    const Impl::Entry *e = m_impl->find(pairingId);
    return e && e->persisted;
}

std::size_t PpcpRendezvous::loadPersisted()
{
    if (!m_store) return 0;
    const std::vector<std::string> ids = m_store->list();
    std::size_t n = 0;
    for (const auto &id : ids) {
        std::uint8_t prk[PPCP_RV_KEY_BYTES];
        if (!m_store->get(id, prk)) continue;
        ppcp_rv_keys keys;
        // 5.1c — derived FROM PRK.  The original psk was destroyed at publish
        // and is not recoverable from anything here, which is the point.
        const bool ok = ppcp_rv_derive_from_prk(prk, &keys) == PPCP_OK;
        wipe(prk, sizeof prk);
        if (!ok) continue;

        std::lock_guard<std::mutex> g(m_impl->mu);
        if (m_impl->find(id)) {
            wipe(&keys, sizeof keys);
            continue;
        }
        Impl::Entry e;
        e.pairingId = id;
        std::memcpy(e.prk, keys.prk, PPCP_RV_KEY_BYTES);
        std::memcpy(e.kId, keys.k_id, PPCP_RV_KEY_BYTES);
        std::memcpy(e.kTls, keys.k_tls, PPCP_RV_KEY_BYTES);
        wipe(&keys, sizeof keys);
        // A persisted pairing is not a code: it has no expiry and no use limit
        // (7.4a), and 7.4e derives a fresh `sid` inside the authenticated
        // channel, so it carries no Session.id here either.
        e.maxUses = 0;
        e.usesRemaining = UINT64_MAX;
        e.expUnixS = 0;
        e.persisted = true;
        e.mayPersist = true;
        m_impl->entries.push_back(std::move(e));
        n++;
    }
    return n;
}

std::string PpcpRendezvous::resolveRid(const std::uint8_t rn[PPCP_RV_RN_BYTES],
                                       const std::uint8_t rid[PPCP_RV_RID_BYTES]) const
{
    std::lock_guard<std::mutex> g(m_impl->mu);
    std::vector<ppcp_rv_pairing> pairings;
    pairings.reserve(m_impl->entries.size());
    std::vector<std::size_t> map;
    for (std::size_t i = 0; i < m_impl->entries.size(); ++i) {
        const auto &e = m_impl->entries[i];
        if (e.invalidated) continue;
        pairings.push_back(ppcp_rv_pairing{e.kId, nullptr});
        map.push_back(i);
    }
    std::size_t idx = 0;
    if (ppcp_rv_resolve_rid(pairings.empty() ? nullptr : pairings.data(), pairings.size(),
                            rn, rid, &idx) != PPCP_OK)
        return {};   // 3.4c — and the caller does not get to connect anyway
    if (idx >= map.size()) return {};
    return m_impl->entries[map[idx]].pairingId;
}

std::vector<CodeStatus> PpcpRendezvous::codes() const
{
    std::lock_guard<std::mutex> g(m_impl->mu);
    std::vector<CodeStatus> out;
    out.reserve(m_impl->entries.size());
    for (const auto &e : m_impl->entries) {
        CodeStatus s;
        s.pairingId = e.pairingId;
        s.sessionId = e.sessionId;
        s.displayName = e.displayName;
        s.maxUses = e.maxUses;
        s.usesRemaining = e.usesRemaining;
        s.expUnixS = e.expUnixS;
        s.invalidated = e.invalidated;
        s.persisted = e.persisted;
        s.endpoints = e.endpoints;
        out.push_back(std::move(s));
    }
    return out;
}

bool PpcpRendezvous::status(const std::string &pairingId, CodeStatus *out) const
{
    if (!out) return false;
    std::lock_guard<std::mutex> g(m_impl->mu);
    const Impl::Entry *e = m_impl->find(pairingId);
    if (!e) return false;
    out->pairingId = e->pairingId;
    out->sessionId = e->sessionId;
    out->displayName = e->displayName;
    out->maxUses = e->maxUses;
    out->usesRemaining = e->usesRemaining;
    out->expUnixS = e->expUnixS;
    out->invalidated = e->invalidated;
    out->persisted = e->persisted;
    out->endpoints = e->endpoints;
    return true;
}

std::size_t PpcpRendezvous::reap()
{
    std::lock_guard<std::mutex> g(m_impl->mu);
    const std::uint64_t nowS = m_impl->now();
    std::size_t n = 0;
    auto dead = [&](Impl::Entry &e) {
        if (e.persisted) return false;
        if (e.invalidated) return true;
        if (e.expUnixS != 0 && nowS > e.expUnixS) return true;
        return false;
    };
    for (auto &e : m_impl->entries)
        if (dead(e)) { e.wipeKeys(); n++; }
    m_impl->entries.erase(
        std::remove_if(m_impl->entries.begin(), m_impl->entries.end(), dead),
        m_impl->entries.end());
    return n;
}

PpcpRendezvous::Counters PpcpRendezvous::counters() const
{
    std::lock_guard<std::mutex> g(m_impl->mu);
    return m_impl->counters;
}

// ── RT-9 ────────────────────────────────────────────────────────────────────
std::string PpcpRendezvous::diagnosticExport() const
{
    // Built from CodeStatus and Counters, and those two structs hold no secret
    // and no payload field.  That is the whole design: 7.2b is met because
    // there is nothing here to leak, not because a filter removed it.
    const std::vector<CodeStatus> rows = codes();
    const Counters c = counters();
    const std::uint64_t nowS = [&] {
        std::lock_guard<std::mutex> g(m_impl->mu);
        return m_impl->now();
    }();

    std::ostringstream o;
    o << "[ppcp-rv] rendezvous, host side\n";
    o << "[ppcp-rv] pairings held: " << rows.size() << "\n";
    for (const auto &r : rows) {
        o << "[ppcp-rv]   pairing=" << r.pairingId
          << " session=" << (r.sessionId.empty() ? "-" : r.sessionId)
          << " uses=" << (r.usesRemaining == UINT64_MAX
                              ? std::string("n/a")
                              : std::to_string(r.usesRemaining))
          << "/" << (r.maxUses ? std::to_string(r.maxUses) : std::string("n/a"));
        if (r.expUnixS == 0) o << " exp=none";
        else if (r.expUnixS > nowS) o << " exp=+" << (r.expUnixS - nowS) << "s";
        else o << " exp=-" << (nowS - r.expUnixS) << "s";
        o << " invalidated=" << (r.invalidated ? "yes" : "no")
          << " persisted=" << (r.persisted ? "yes" : "no")
          << " endpoints=" << r.endpoints.size() << "\n";
    }
    o << "[ppcp-rv] store: " << (m_store ? m_store->describe() : std::string("none")) << "\n";
    o << "[ppcp-rv] resolver: calls=" << c.resolveCalls
      << " resolved=" << c.resolved
      << " no-pairing=" << c.refusedNoPairing
      << " expired=" << c.refusedExpired
      << " exhausted=" << c.refusedExhausted
      << " invalidated=" << c.refusedInvalidated << "\n";
    return o.str();
}

}  // namespace Ppcp
