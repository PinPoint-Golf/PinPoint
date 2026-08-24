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

#include "ppcp_discovery.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>

// ntohs() below is used only by BonjourBrowser, so arpa/inet.h — POSIX-only,
// absent on Windows — travels with dns_sd.h under the same guard rather than
// sitting unconditionally at the top of a file every platform compiles.
#if defined(__APPLE__)
#include <arpa/inet.h>
#include <dns_sd.h>
#endif

namespace Ppcp {
namespace {

bool hexToBytes(const std::string &s, std::uint8_t *out, std::size_t n)
{
    if (s.size() != n * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

bool parseTxtRecord(const std::uint8_t *txt, std::size_t len, RvAdvertisement *out)
{
    if (!out) return false;
    std::size_t i = 0;
    while (i < len) {
        const std::size_t n = txt[i];
        if (i + 1 + n > len) return false;   // malformed AS A TXT RECORD
        const std::string entry(reinterpret_cast<const char *>(txt + i + 1), n);
        i += 1 + n;

        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;   // a bare key; not ours, ignore
        const std::string key = entry.substr(0, eq);
        const std::string val = entry.substr(eq + 1);

        // 3.3a — exactly these keys are recognised, and any other is ignored
        // rather than rejected.  A value of the wrong shape leaves the field
        // unset for the same reason: the browser's job is to be unsurprised.
        if (key == "txtvers") out->txtvers = val;
        else if (key == "pv") out->pv = val;
        else if (key == "role") out->role = val;
        else if (key == "rn") out->hasRn = hexToBytes(val, out->rn, PPCP_RV_RN_BYTES);
        else if (key == "rid") out->hasRid = hexToBytes(val, out->rid, PPCP_RV_RID_BYTES);
    }
    return true;
}

// ── `RV` 3.3d / 3.3e — THE ONE RANGE SYNTAX (erratum E25) ──────────────────
//
// A version range is `LOW` or `LOW-HIGH`, each endpoint a `MAJOR.MINOR` as
// `CORE` 10.1b defines it.  Both endpoints are INCLUSIVE and SHARE a MAJOR; a
// bare `LOW` is the range `LOW-LOW`.  Support across two MAJORs is several
// ranges separated by a comma, most preferred first — `2.0-2.1,1.4-1.6`.
//
// ⚠ THE OLD PARSER READ BARE MAJORS AND GOT THE COMMA CASE WRONG.  `1-2` meant
// majors 1 through 2, so `1.0-1.2` accidentally worked (both endpoints parse to
// 1) and `2.0-2.1,1.4-1.6` did not: the second endpoint parsed as `strtol`
// stopped at the comma, giving the range 2..2, and a peer supporting major 1
// was refused with nothing to say why.  E25 settled the syntax and this is it.
//
// 3.3d's last sentence is the failure mode: "a reader that cannot parse a range
// IGNORES that advertisement rather than guessing."  So anything malformed
// anywhere in the list makes the whole record unusable and this returns false —
// it does not skip the bad component and accept on the others.
bool parseEndpoint(const std::string &s, int *major, int *minor)
{
    if (s.empty()) return false;
    const std::size_t dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size()) return false;
    auto num = [](const std::string &t, int *out) {
        if (t.empty()) return false;
        for (char ch : t) if (ch < '0' || ch > '9') return false;
        errno = 0;
        const long v = std::strtol(t.c_str(), nullptr, 10);
        if (errno != 0 || v < 0 || v > 100000) return false;
        *out = static_cast<int>(v);
        return true;
    };
    return num(s.substr(0, dot), major) && num(s.substr(dot + 1), minor);
}

bool pvAcceptsMajor(const std::string &pv, int major)
{
    if (pv.empty()) return false;

    bool hit = false;
    std::size_t at = 0;
    while (at <= pv.size()) {
        const std::size_t comma = pv.find(',', at);
        const std::string range =
            pv.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (range.empty()) return false;         // "1.0,,2.0" is not parseable

        const std::size_t dash = range.find('-');
        int loMajor = 0, loMinor = 0, hiMajor = 0, hiMinor = 0;
        if (dash == std::string::npos) {
            if (!parseEndpoint(range, &loMajor, &loMinor)) return false;
            hiMajor = loMajor;                    // a bare LOW is LOW-LOW
            hiMinor = loMinor;
        } else {
            if (!parseEndpoint(range.substr(0, dash), &loMajor, &loMinor)) return false;
            if (!parseEndpoint(range.substr(dash + 1), &hiMajor, &hiMinor)) return false;
        }
        // 3.3d — the endpoints share a MAJOR, and HIGH is not below LOW.  Both
        // are parse failures rather than empty ranges: an advertisement saying
        // `1.4-1.2` is one this reader cannot make sense of, and 3.3d says
        // ignore rather than guess which end was meant.
        if (loMajor != hiMajor) return false;
        if (hiMinor < loMinor) return false;

        if (loMajor == major) hit = true;
        if (comma == std::string::npos) break;
        at = comma + 1;
        if (at == pv.size()) return false;        // a trailing comma
    }
    return hit;
}

bool instanceNameMatchesRid(const std::string &instanceName,
                            const std::uint8_t rid[PPCP_RV_RID_BYTES])
{
    char expected[PPCP_RV_INSTANCE_NAME_MAX] = {0};
    if (ppcp_rv_instance_name(rid, expected) != PPCP_OK) return false;
    return instanceName == expected;
}

DialDecision decideDial(const RvAdvertisement &ad, int wireMajor, const RidResolver &resolve)
{
    DialDecision d;

    // 3.3a — the browser filters on MAJOR *before* connecting.  Doing it first
    // also means an incompatible peer never reaches the resolver, so no key
    // material is exercised on its behalf.
    if (!pvAcceptsMajor(ad.pv, wireMajor)) {
        d.why = "pv does not carry our wire major";
        return d;
    }
    // 3.4b needs both halves; an advertisement missing either cannot be
    // resolved, and 3.4c then forbids the connection.
    if (!ad.hasRn || !ad.hasRid) {
        d.why = "no rn/rid in the TXT record";
        return d;
    }
    if (!instanceNameMatchesRid(ad.instanceName, ad.rid)) {
        // 3.2a is a MUST and the check is one HMAC-free string compare.  A
        // mismatch is a peer that is not following §3, so nothing here can be
        // trusted to name a pairing.
        d.why = "instance name does not derive from rid (3.2a)";
        return d;
    }
    if (!resolve) {
        d.why = "no pairings held";
        return d;
    }

    const std::string pairing = resolve(ad.rn, ad.rid);
    if (pairing.empty()) {
        // 3.4c.  There is no branch below this one that dials anyway, and that
        // is the whole point of the function: an unresolvable `rid` is a
        // stranger, and a stranger is not dialled.
        d.why = "rid resolves to no held pairing (3.4c)";
        return d;
    }

    d.dial = true;
    d.pairingId = pairing;
    d.why = "rid resolved";
    return d;
}

// ── The platform browser ────────────────────────────────────────────────────

#if defined(__APPLE__)
namespace {

class BonjourBrowser final : public RvBrowser {
public:
    ~BonjourBrowser() override { stop(); }

    bool start(FoundFn onFound, LostFn onLost) override
    {
        stop();
        m_found = std::move(onFound);
        m_lost = std::move(onLost);
        // ⚠ kDNSServiceFlagsDefault and NOT a "register" call anywhere in this
        // file.  DNSServiceBrowse asks mDNSResponder — which already owns UDP
        // 5353 — to query on our behalf; this process opens only the local IPC
        // socket returned by DNSServiceRefSockFD.  RV 2's "browsing needs only
        // the querier role" is satisfied by construction rather than by policy.
        const DNSServiceErrorType e =
            DNSServiceBrowse(&m_browse, 0, kDNSServiceInterfaceIndexAny,
                             "_ppcp._tcp", nullptr, &BonjourBrowser::onBrowse, this);
        if (e != kDNSServiceErr_NoError) {
            m_browse = nullptr;
            return false;   // 3.6a — not an error state, just no discovery
        }
        return true;
    }

    void stop() override
    {
        for (auto &kv : m_resolves)
            if (kv.second) DNSServiceRefDeallocate(kv.second);
        m_resolves.clear();
        if (m_browse) {
            DNSServiceRefDeallocate(m_browse);
            m_browse = nullptr;
        }
    }

    int fd() const override
    {
        return m_browse ? DNSServiceRefSockFD(m_browse) : -1;
    }

    bool process() override
    {
        if (!m_browse) return false;
        return DNSServiceProcessResult(m_browse) == kDNSServiceErr_NoError;
    }

    // A resolve carries its own socket; the owner polls them through here too.
    // Kept simple deliberately: a handful of instances on a range network is
    // the whole population, and 3.6a means being slow to discover costs
    // nothing that matters.
    std::string describe() const override { return "DNS-SD via mDNSResponder (browse only)"; }

private:
    static void DNSSD_API onBrowse(DNSServiceRef, DNSServiceFlags flags,
                                   uint32_t ifIndex, DNSServiceErrorType err,
                                   const char *name, const char *type,
                                   const char *domain, void *ctx)
    {
        auto *self = static_cast<BonjourBrowser *>(ctx);
        if (err != kDNSServiceErr_NoError || !name) return;
        if (!(flags & kDNSServiceFlagsAdd)) {
            if (self->m_lost) self->m_lost(name);
            return;
        }
        DNSServiceRef ref = nullptr;
        auto *pending = new Pending{self, name};
        if (DNSServiceResolve(&ref, 0, ifIndex, name, type, domain,
                              &BonjourBrowser::onResolve, pending) != kDNSServiceErr_NoError) {
            delete pending;
            return;
        }
        // Resolve synchronously: one blocking read on a local IPC socket, off
        // the main thread by construction because process() is called from
        // wherever the owner watches fd().
        DNSServiceProcessResult(ref);
        DNSServiceRefDeallocate(ref);
        delete pending;
    }

    struct Pending {
        BonjourBrowser *self;
        std::string     name;
    };

    static void DNSSD_API onResolve(DNSServiceRef, DNSServiceFlags, uint32_t,
                                    DNSServiceErrorType err, const char *,
                                    const char *hostTarget, uint16_t port,
                                    uint16_t txtLen, const unsigned char *txt, void *ctx)
    {
        auto *p = static_cast<Pending *>(ctx);
        if (!p || err != kDNSServiceErr_NoError) return;

        RvAdvertisement ad;
        ad.instanceName = p->name;
        ad.host = hostTarget ? hostTarget : "";
        ad.port = ntohs(port);
        if (txt && txtLen) parseTxtRecord(txt, txtLen, &ad);
        if (p->self->m_found) p->self->m_found(ad);
    }

    DNSServiceRef m_browse = nullptr;
    std::map<std::string, DNSServiceRef> m_resolves;
    FoundFn m_found;
    LostFn  m_lost;
};

}  // namespace
#endif  // __APPLE__

std::unique_ptr<RvBrowser> makePlatformBrowser()
{
#if defined(__APPLE__)
    return std::unique_ptr<RvBrowser>(new BonjourBrowser);
#else
    // No DNS-SD client on this platform yet.  3.6b: failure to discover falls
    // back to the pairing code, without user-visible failure — so returning
    // null is the whole of the handling.
    return nullptr;
#endif
}

}  // namespace Ppcp
