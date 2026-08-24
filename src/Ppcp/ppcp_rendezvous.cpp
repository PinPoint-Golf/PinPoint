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

// Deliberately Qt-free (see the header), so both platform branches below reach
// straight for the OS rather than QNetworkInterface/QRandomGenerator.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

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
#if defined(_WIN32)
bool csprngBytes(void *out, std::size_t len)
{
    // BCryptGenRandom with the system-preferred generator is Windows' analogue
    // of getentropy(): CNG's own pool, no seeding, and it either fills the
    // buffer or returns a non-zero NTSTATUS — no silent degrade, same as the
    // POSIX branch below. No per-call cap to chunk against, unlike getentropy().
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out),
                           static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 /* STATUS_SUCCESS */;
}
#else
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
#endif

// ── RV 4.3d ─────────────────────────────────────────────────────────────────
std::vector<RvEndpoint> reachableEndpoints(std::uint16_t port)
{
    std::vector<RvEndpoint> routable, loopback;

#if defined(_WIN32)
    // GetAdaptersAddresses is this platform's getifaddrs: no /proc, no netlink,
    // just a buffer Microsoft's own docs say to size at 15000 bytes and retry
    // once on ERROR_BUFFER_OVERFLOW with the size it reports back.
    ULONG bufLen = 15000;
    std::vector<unsigned char> raw(bufLen);
    auto *addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(raw.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        raw.resize(bufLen);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(raw.data());
        ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &bufLen);
    }
    if (ret != NO_ERROR) return {};

    for (auto *a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        // A tunnel is not a bay — same intent as the POSIX branch's
        // IFF_POINTOPOINT check, read here off the adapter's own type.
        if (a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_TUNNEL ||
            a->IfType == IF_TYPE_SLIP) continue;
        const bool lo = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);

        for (auto *ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            const SOCKADDR *sa = ua->Address.lpSockaddr;
            char buf[INET6_ADDRSTRLEN] = {0};

            if (sa->sa_family == AF_INET) {
                auto *sin = reinterpret_cast<const sockaddr_in *>(sa);
                if (!InetNtopA(AF_INET, &sin->sin_addr, buf, sizeof buf)) continue;
            } else if (sa->sa_family == AF_INET6) {
                auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(sa);
                // fe80::/10 by hand: the IN6_IS_ADDR_LINKLOCAL macro the POSIX
                // branch gets from netinet/in.h isn't available here. Same
                // reason to skip it — a link-local address needs a scope id
                // the scanner cannot use (4.3c tries every entry in turn, so
                // an unusable one is a wasted dial).
                const unsigned char *b = sin6->sin6_addr.s6_addr;
                if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) continue;
                if (!InetNtopA(AF_INET6, &sin6->sin6_addr, buf, sizeof buf)) continue;
            } else {
                continue;
            }

            RvEndpoint e;
            e.host = buf;
            e.port = port;
            (lo ? loopback : routable).push_back(e);
        }
    }
#else
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
#endif

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
        // 3.4d1's SHOULD — "advertise a recently used pairing first, because
        // the counterpart a user is standing in front of is usually the one
        // they used last".  Set when a link actually establishes on this
        // pairing, so it is a record of a device having been here rather than
        // of a code having been printed.  0 = never, this run.
        std::uint64_t lastUsedUnixS = 0;
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
    e->lastUsedUnixS = m_impl->now();   // 3.4d1 — recently used first
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

bool PpcpRendezvous::adoptGuidedPairing(const std::uint8_t sid[PPCP_RV_SID_BYTES],
                                       const ppcp_rv_keys &keys,
                                       const std::string &displayName,
                                       std::string *out, std::string *err)
{
    if (!out) return false;
    std::uint8_t handle[8];
    if (!csprngBytes(handle, sizeof handle)) {
        if (err) *err = "the platform CSPRNG failed";
        return false;
    }

    char sessionId[PPCP_RV_SESSION_ID_CHARS] = {0};
    // 11.6d — `sid` is DERIVED rather than exchanged, with the version and
    // variant bits set before it is used for anything.  libppcp did that; this
    // only renders it, and a `sid` that does not render is one this host would
    // not be able to name a session with.
    if (ppcp_rv_sid_to_session_id(sid, sessionId) != PPCP_OK) {
        if (err) *err = "the derived sid does not render as a Session.id";
        return false;
    }

    Impl::Entry e;
    e.pairingId = toHex(handle, sizeof handle);
    e.sessionId = sessionId;
    // ⛔ 4.4d / 3.3g — the display name here came off a bootstrap TXT record as
    // `dl` and is UNTRUSTED: it was shown before anything was authenticated, so
    // it is whatever a stranger put on the wire.  It is stored as a LABEL and
    // never as an identifier, a trust signal or a storage key — `pairingId`
    // above is drawn from the CSPRNG and owes nothing to it.
    e.displayName = displayName;
    std::memcpy(e.prk, keys.prk, PPCP_RV_KEY_BYTES);
    std::memcpy(e.kId, keys.k_id, PPCP_RV_KEY_BYTES);
    std::memcpy(e.kTls, keys.k_tls, PPCP_RV_KEY_BYTES);
    // 7.4a — no expiry and no use limit; see the header.
    e.maxUses = 0;
    e.usesRemaining = UINT64_MAX;
    e.expUnixS = 0;
    e.persisted = false;         // 7.4b — the user's action, not this one
    e.mayPersist = true;

    *out = e.pairingId;
    {
        std::lock_guard<std::mutex> g(m_impl->mu);
        m_impl->entries.push_back(std::move(e));
    }
    return true;
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

// ── 3.4a / 3.4d1 — the advertisement side of the same identifiers (H9) ──────

std::vector<std::string> PpcpRendezvous::advertisablePairings() const
{
    std::lock_guard<std::mutex> g(m_impl->mu);

    // Persisted and not revoked.  `revoke()` erases the key material and drops
    // the row, so a revoked pairing cannot appear here at all; `invalidated`
    // is the belt to that brace and costs one comparison.
    std::vector<std::pair<std::uint64_t, std::size_t>> rows;
    for (std::size_t i = 0; i < m_impl->entries.size(); ++i) {
        const auto &e = m_impl->entries[i];
        if (!e.persisted || e.invalidated) continue;
        rows.push_back({e.lastUsedUnixS, i});
    }

    // Most recently used first; never-used last in ledger order.  `stable_sort`
    // rather than `sort` so that the order of two pairings with the same stamp
    // — every pairing that has not been used this run shares the stamp 0 — is
    // the ledger's and does not vary between calls.  3.4d1 asks for "a stable
    // order", and a comparator that ties arbitrarily would not give one.
    std::stable_sort(rows.begin(), rows.end(),
                     [](const std::pair<std::uint64_t, std::size_t> &a,
                        const std::pair<std::uint64_t, std::size_t> &b) {
                         return a.first > b.first;
                     });

    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const auto &r : rows) out.push_back(m_impl->entries[r.second].pairingId);
    return out;
}

bool PpcpRendezvous::mintRid(const std::string &pairingId,
                             std::uint8_t rn[PPCP_RV_RN_BYTES],
                             std::uint8_t rid[PPCP_RV_RID_BYTES]) const
{
    if (!rn || !rid) return false;

    std::uint8_t kId[PPCP_RV_KEY_BYTES];
    {
        std::lock_guard<std::mutex> g(m_impl->mu);
        const Impl::Entry *e = m_impl->find(pairingId);
        if (!e || e->invalidated) return false;
        std::memcpy(kId, e->kId, sizeof kId);
    }

    // 7.2a — the CSPRNG, at full width, with no fallback.  A failure here
    // skips one rotation; the previous `rn` stays on the wire, which is
    // conformant for up to 3.4a's fifteen minutes and is strictly better than
    // publishing a nonce from a degraded source.
    const bool drew = csprngBytes(rn, PPCP_RV_RN_BYTES);
    const bool ok = drew && ppcp_rv_rid(kId, rn, rid) == PPCP_OK;
    wipe(kId, sizeof kId);
    if (!ok) {
        wipe(rn, PPCP_RV_RN_BYTES);
        wipe(rid, PPCP_RV_RID_BYTES);
        return false;
    }
    return true;
}

// ── RV 5.3a1, erratum E21 — DRAWING AN IDENTITY FOR A LIVE CONNECTION ──────
//
// ⚠ `ppcp_rv_psk_identity()` IS THE WRONG ENTRY POINT FOR A SOCKET, and it does
// not say so at the call site — which is exactly how this cost a session.  The
// identity is `0x01 || rn2 || tag`, 17 octets; several widely-used TLS stacks
// carry a PSK identity as a C string and take its length with `strlen`, so an
// embedded `0x00` truncates it, the server resolves nothing, and the handshake
// fails **intermittently** — one connection in sixteen, because 17 octets each
// have a 1-in-256 chance of being zero.  At a driving range that is diagnosed
// as a network fault.  `ppcp_transport_test` carries the demonstration against
// OpenSSL's own TLS 1.2 PSK interface.
//
// So the draw entry point is the one this class offers: it redraws `rn2` until
// neither it nor the resulting tag carries a zero (1.07 draws on average,
// leaving `rn2` better than 63 bits of entropy) and nothing at the server
// changes — 5.3b recomputes the tag from the `rn2` it received exactly as
// before.  `ppcp_rv_psk_identity()` stays reachable only where §10.2's vector
// must reproduce byte for byte, which is a test and never a connection.
bool PpcpRendezvous::drawPskIdentity(const std::string &pairingId, PskIdentity *out,
                                     std::string *whyNot) const
{
    auto fail = [&](const char *why) { if (whyNot) *whyNot = why; return false; };
    if (!out) return fail("no output buffer");

    std::lock_guard<std::mutex> g(m_impl->mu);
    for (const auto &e : m_impl->entries) {
        if (e.pairingId != pairingId) continue;
        if (e.invalidated) return fail("that pairing has been invalidated");
        std::uint8_t rn2[PPCP_RV_RN_BYTES];
        std::uint8_t id[PPCP_RV_PSK_IDENTITY_BYTES];
        const ppcp_result r = ppcp_rv_psk_identity_draw(
            e.kId,
            [](void * /*ctx*/, std::uint8_t *buf, std::size_t len) {
                return csprngBytes(buf, len);
            },
            nullptr, rn2, id);
        if (r != PPCP_OK) return fail("the CSPRNG failed, so no identity was drawn");
        out->assign(id, id + sizeof id);
        return true;
    }
    return fail("no such pairing");
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
