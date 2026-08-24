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
#include <utility>

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

// 3.3d — derived from libppcp's own version macros rather than written out.
// See the note in the header: a literal here fails silently under 3.6a.
std::string wirePvRange()
{
    return std::to_string(PPCP_WIRE_VERSION_MAJOR) + "." +
           std::to_string(PPCP_WIRE_VERSION_MINOR);
}

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
        // 3.3f (H10) — the bootstrap form.  `bs` is what identifies the
        // instance as one, and `dl` is UNTRUSTED display text (3.3g -> 4.4d).
        // Both are captured RAW here and sanitised at the point of display:
        // this function's contract is to be unsurprised by the wire, and a
        // parser that silently repaired a hostile `dl` would hide from
        // `classifyInstance()` the fact that it was hostile.
        else if (key == "bs") { out->hasBs = true; out->bs = val; }
        else if (key == "dl") { out->hasDl = true; out->dl = val; }
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

// ── The advertisement half (RV 3.5e / CA5) — H9 ─────────────────────────────

namespace {

// Lowercase, which is what §10's own worked example uses for `rn` and `rid` in
// a TXT record.  The instance name is uppercase (3.2a) and that asymmetry is
// the document's, not ours; `hexToBytes()` above reads either, so a peer that
// chose the other case still resolves.
std::string bytesToHexLower(const std::uint8_t *b, std::size_t n)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[(b[i] >> 4) & 0x0f]);
        s.push_back(d[b[i] & 0x0f]);
    }
    return s;
}

// One length-prefixed "key=value".  DNS-SD gives each string a single length
// byte, so 255 is the hard limit per entry; 3.3c's 200 bytes for the whole
// record is far stricter and is checked by the caller.
bool appendTxtEntry(std::vector<std::uint8_t> *out, const std::string &kv)
{
    if (kv.empty() || kv.size() > 255) return false;
    out->push_back(static_cast<std::uint8_t>(kv.size()));
    out->insert(out->end(), kv.begin(), kv.end());
    return true;
}

}  // namespace

bool registrationInstanceName(const std::uint8_t nonce[PPCP_RV_REG_NONCE_BYTES],
                              std::string *out)
{
    if (!nonce || !out) return false;
    static const char *d = "0123456789ABCDEF";
    std::string s = "PPCP-";
    for (std::size_t i = 0; i < PPCP_RV_REG_NONCE_BYTES; ++i) {
        s.push_back(d[(nonce[i] >> 4) & 0x0f]);
        s.push_back(d[nonce[i] & 0x0f]);
    }
    *out = s;
    return true;
}

bool buildTxtRecord(const RvTxtFields &f, std::vector<std::uint8_t> *out)
{
    if (!out) return false;
    if (f.pv.empty() || f.role.empty()) return false;
    // 3.3a's `role` is a closed set.  A value outside it is a record no
    // browser can act on, and refusing to build one is cheaper than shipping
    // it and wondering why nothing dials.
    if (f.role != "host" && f.role != "capture" && f.role != "observer") return false;

    std::vector<std::uint8_t> txt;
    // The order is the order of 3.3a's table.  Nothing depends on it — a TXT
    // record is a set — but a record that reads like the specification is one
    // a reader can check against it.
    const bool ok =
        appendTxtEntry(&txt, "txtvers=1") &&
        appendTxtEntry(&txt, "pv=" + f.pv) &&
        appendTxtEntry(&txt, "role=" + f.role) &&
        appendTxtEntry(&txt, "rn=" + bytesToHexLower(f.rn, PPCP_RV_RN_BYTES)) &&
        appendTxtEntry(&txt, "rid=" + bytesToHexLower(f.rid, PPCP_RV_RID_BYTES));
    if (!ok) return false;
    if (txt.size() > 200) return false;   // 3.3c
    out->swap(txt);
    return true;
}

unsigned rotationPeriodSeconds(std::size_t pairingsHeld)
{
    if (pairingsHeld == 0) return 0;              // nothing to advertise
    if (pairingsHeld == 1) return kAdvertisementMaxPeriodS;   // 3.4a and no more

    unsigned p = static_cast<unsigned>(kAdvertisementCycleTargetS / pairingsHeld);
    if (p < kAdvertisementMinPeriodS) p = kAdvertisementMinPeriodS;
    if (p > kAdvertisementMaxPeriodS) p = kAdvertisementMaxPeriodS;
    return p;
}

// ── The rotation driver ─────────────────────────────────────────────────────

RvReconnectionAdvertisement::RvReconnectionAdvertisement(RvAdvertiser *advertiser,
                                                         RidMinter mint, RandomFn rng)
    : m_adv(advertiser), m_mint(std::move(mint)), m_rng(std::move(rng))
{
}

bool RvReconnectionAdvertisement::start(std::uint16_t port,
                                        const std::vector<std::string> &pairings,
                                        std::uint64_t nowS)
{
    stop();
    if (!m_adv || !m_mint || !m_rng) return false;
    if (pairings.empty() || port == 0) return false;

    // 3.2d — four CSPRNG bytes, drawn fresh for THIS registration.  A failure
    // to draw them advertises nothing: an instance name from a degraded source
    // is not a privacy failure the way a key would be, but this application
    // has one CSPRNG and no fallback anywhere, and inventing one here would be
    // the first.
    std::uint8_t nonce[PPCP_RV_REG_NONCE_BYTES]{};
    if (!m_rng(nonce, sizeof nonce)) return false;
    if (!registrationInstanceName(nonce, &m_instanceName)) return false;

    m_pairings = pairings;
    m_at = 0;
    m_port = port;
    m_rotations = 0;
    return rotate(nowS, true);
}

void RvReconnectionAdvertisement::setPairings(const std::vector<std::string> &pairings,
                                              std::uint64_t nowS)
{
    if (pairings.empty()) { stop(); return; }
    if (!m_active) { start(m_port, pairings, nowS); return; }

    // The registration survives.  3.2d draws a name per REGISTRATION, and a
    // pairing arriving or being revoked is not one — re-drawing here would
    // rename the service for a reason 3.2d does not recognise, which is the
    // deregister/probe/announce cycle E49 says makes the rotation unaffordable.
    const std::string wasOn = m_current;
    m_pairings = pairings;

    // Stay on the pairing already advertised where it is still held, so a
    // device that is midway through resolving this instance is not moved out
    // from under it by an unrelated change to the ledger.
    m_at = 0;
    for (std::size_t i = 0; i < m_pairings.size(); ++i) {
        if (m_pairings[i] == wasOn) { m_at = i; break; }
    }
    if (m_current != m_pairings[m_at]) rotate(nowS, false);
    else m_nextS = nowS + periodSeconds();
}

void RvReconnectionAdvertisement::stop()
{
    if (m_adv) m_adv->stop();
    m_active = false;
    m_pairings.clear();
    m_current.clear();
    m_instanceName.clear();
    m_at = 0;
    m_nextS = 0;
}

bool RvReconnectionAdvertisement::tick(std::uint64_t nowS)
{
    if (!m_active || m_pairings.empty()) return false;
    if (nowS < m_nextS) return false;
    // 3.4d1 — "selecting the next pairing in a stable order".  The order is
    // the caller's, which is most-recently-used first, and it wraps.
    m_at = (m_at + 1) % m_pairings.size();
    return rotate(nowS, false);
}

bool RvReconnectionAdvertisement::rotate(std::uint64_t nowS, bool firstRegistration)
{
    if (m_pairings.empty()) return false;
    const std::string &id = m_pairings[m_at];

    // 3.4a — a fresh `rn` on every registration and on every rotation, and
    // `rid` recomputed with it.  The minter is the only thing that touches
    // `K_id`, and it lives in the rendezvous ledger.
    RvTxtFields f;
    if (!m_mint(id, f.rn, f.rid)) return false;

    std::vector<std::uint8_t> txt;
    if (!buildTxtRecord(f, &txt)) return false;

    const bool ok = firstRegistration ? m_adv->start(m_instanceName, m_port, txt)
                                      : m_adv->updateTxt(txt);
    if (!ok) {
        // 3.6a — a responder that will not publish is not an error state.  We
        // simply are not discoverable, and 3.6b's fallback is the pairing code
        // the user can already reach.
        if (firstRegistration) m_active = false;
        return false;
    }

    m_active = true;
    m_current = id;
    m_nextS = nowS + periodSeconds();
    ++m_rotations;
    return true;
}

bool          RvReconnectionAdvertisement::active() const { return m_active; }
unsigned      RvReconnectionAdvertisement::periodSeconds() const
{
    return rotationPeriodSeconds(m_pairings.size());
}
std::uint64_t RvReconnectionAdvertisement::nextRotationAtS() const { return m_nextS; }
std::string   RvReconnectionAdvertisement::instanceName() const { return m_instanceName; }
std::string   RvReconnectionAdvertisement::currentPairingId() const { return m_current; }
std::size_t   RvReconnectionAdvertisement::rotations() const { return m_rotations; }

std::string RvReconnectionAdvertisement::describe() const
{
    // 7.2b — a count, a period and the service type.  No pairing id, because
    // although a local handle is not key material, naming which pairing is on
    // the wire in a diagnostic bundle is exactly the correlation 3.4e's
    // unlinkability argument is about.
    if (!m_active) return "not advertising";
    std::string s = "advertising _ppcp._tcp as ";
    s += m_instanceName;
    s += " (role host), rotating ";
    s += std::to_string(m_pairings.size());
    s += " pairing(s) every ";
    s += std::to_string(periodSeconds());
    s += "s, ";
    s += std::to_string(m_rotations);
    s += " so far";
    return s;
}

// ── The platform advertiser ─────────────────────────────────────────────────

#if defined(__APPLE__)
namespace {

class BonjourAdvertiser final : public RvAdvertiser {
public:
    ~BonjourAdvertiser() override { stop(); }

    bool start(const std::string &instanceName, std::uint16_t port,
               const std::vector<std::uint8_t> &txt) override
    {
        stop();
        if (instanceName.empty() || port == 0 || txt.empty()) return false;
        if (txt.size() > 0xffff) return false;

        // ⚠ NO `kDNSServiceFlagsNoAutoRename`.  If another instance on the
        // link already holds this name, letting mDNSResponder rename ours
        // keeps us discoverable; refusing would leave §7.4's persistence dead
        // for the sake of eight hex characters that name nothing.  3.2b binds
        // what the name may not CONTAIN, and a numeric suffix contains none of
        // it.  The name we end up with is reported by onRegister below.
        //
        // ⚠ AND NO CALLBACK-FREE FORM.  Passing a null callback would make the
        // registration fire-and-forget and leave `registeredName()` empty, so
        // there would be no way to observe that the responder took it.
        const DNSServiceErrorType e =
            DNSServiceRegister(&m_reg, 0, kDNSServiceInterfaceIndexAny,
                               instanceName.c_str(), "_ppcp._tcp",
                               nullptr,   // default domain (.local)
                               nullptr,   // this host
                               htons(port),
                               static_cast<uint16_t>(txt.size()), txt.data(),
                               &BonjourAdvertiser::onRegister, this);
        if (e != kDNSServiceErr_NoError) {
            m_reg = nullptr;
            return false;   // 3.6a — not an error state, just not discoverable
        }
        m_asked = instanceName;
        return true;
    }

    bool updateTxt(const std::vector<std::uint8_t> &txt) override
    {
        if (!m_reg || txt.empty() || txt.size() > 0xffff) return false;
        // A null DNSRecordRef means "the primary TXT record of this
        // registration", and a TTL of 0 means the responder's default.  This
        // is the one call 3.2d exists to make possible: the service keeps its
        // name and one record changes, so a rotation is a single announcement
        // rather than a deregister, probe and announce.
        return DNSServiceUpdateRecord(m_reg, nullptr, 0,
                                      static_cast<uint16_t>(txt.size()),
                                      txt.data(), 0) == kDNSServiceErr_NoError;
    }

    void stop() override
    {
        if (m_reg) {
            // Deallocating the ref is what sends the goodbye packet and
            // withdraws the instance.
            DNSServiceRefDeallocate(m_reg);
            m_reg = nullptr;
        }
        m_asked.clear();
        m_registered.clear();
    }

    int fd() const override { return m_reg ? DNSServiceRefSockFD(m_reg) : -1; }

    bool process() override
    {
        if (!m_reg) return false;
        return DNSServiceProcessResult(m_reg) == kDNSServiceErr_NoError;
    }

    std::string describe() const override
    {
        return "DNS-SD via mDNSResponder (register)";
    }

    std::string registeredName() const override { return m_registered; }

private:
    static void DNSSD_API onRegister(DNSServiceRef, DNSServiceFlags,
                                     DNSServiceErrorType err, const char *name,
                                     const char *, const char *, void *ctx)
    {
        auto *self = static_cast<BonjourAdvertiser *>(ctx);
        if (!self) return;
        if (err != kDNSServiceErr_NoError) {
            // 3.6a.  A name conflict the responder could not resolve, or a
            // responder that went away: we are not discoverable and there is
            // nothing to report to a user about it.
            self->m_registered.clear();
            return;
        }
        if (name) self->m_registered = name;
    }

    DNSServiceRef m_reg = nullptr;
    std::string   m_asked;
    std::string   m_registered;
};

}  // namespace
#endif  // __APPLE__

std::unique_ptr<RvAdvertiser> makePlatformAdvertiser()
{
#if defined(__APPLE__)
    return std::unique_ptr<RvAdvertiser>(new BonjourAdvertiser);
#else
    // CA5 — Windows is DEFERRED and this is where it is recorded.  There is no
    // `dns_sd.h` outside Apple's Bonjour SDK, which is an installer and a
    // system service rather than a header, and taking that dependency is a
    // decision about what PinPointStudio ships rather than about the protocol.
    // 3.6b makes the consequence silent: no advertisement, so a Windows host
    // is reached by pairing code every session until somebody decides.
    return nullptr;
#endif
}

}  // namespace Ppcp
