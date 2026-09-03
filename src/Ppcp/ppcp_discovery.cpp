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

#ifdef _WIN32
// W4, second cut — the native engine, no Bonjour SDK dependency (see its own
// header for why). PP_DNS_SD_AVAILABLE below is never set on Windows any
// more, so BonjourBrowser/BonjourAdvertiser compile out there entirely; this
// factory pair is what actually answers makePlatformBrowser()/Advertiser().
#include "ppcp_mdns_native.h"
#endif

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

// ntohs() below is used only by BonjourBrowser, so arpa/inet.h — POSIX-only,
// absent on Windows — travels with dns_sd.h under the same guard rather than
// sitting unconditionally at the top of a file every platform compiles.
// ── Where the DNSService* symbols come from ─────────────────────────────────
// Apple ships them in libSystem, so `__APPLE__` alone has always been enough
// there.  Elsewhere they come from a library CMake has to find — on Linux,
// Avahi's Bonjour compatibility layer, which exposes this same `dns_sd.h` —
// and CMake sets `PP_HAVE_DNS_SD` when it did.  ⚠ ONE derived macro rather
// than repeating the disjunction at five sites: the two backends below are
// either both compiled or both absent, and there is no configuration in which
// that is untrue.
#if defined(__APPLE__) || defined(PP_HAVE_DNS_SD)
#define PP_DNS_SD_AVAILABLE 1
#endif

// ── Which DNSService* implementation answers a resolve, and how fast ───────
// macOS and Windows both talk to Apple's OWN mDNSResponder — natively on
// macOS, and via the Bonjour SDK's `mDNSResponder.exe` service on Windows,
// the SAME upstream codebase cross-compiled rather than an independent
// reimplementation. Both answer a local resolve inline, in well under a
// millisecond, and are safe to call from inside a browse callback — the
// behaviour this file has always assumed for `__APPLE__`.
//
// Linux's Avahi compat shim is independently implemented and measured
// otherwise (29 Aug 2026): 650 ms, and it deadlocks if resolved from inside
// the browse callback. Only Avahi gets the deferred/async epoll path below.
//
// ⚠ Windows joining the fast path is REASONED FROM THE SHARED CODEBASE, not
// yet measured on Windows hardware the way the Linux number was — the
// Bonjour SDK is not installed on this box. Confirm on first bring-up (a
// standalone resolve-from-inside-a-browse-callback probe, mirroring the one
// that caught Avahi's deadlock) before relying on it under load.
#if defined(__APPLE__) || defined(_WIN32)
#define PP_DNS_SD_INLINE_RESOLVE 1
#endif

#if defined(PP_DNS_SD_AVAILABLE)
#if defined(_WIN32)
// ⛔ ORDER IS LOAD-BEARING. The Bonjour SDK's `dns_sd.h` (line ~123) does a
// bare `#include <windows.h>` with no `WIN32_LEAN_AND_MEAN` guard of its
// own — last touched circa 2007, well before that became the convention.
// Unguarded, `<windows.h>` pulls in the LEGACY WinSock 1.1 `<winsock.h>`,
// and a later `<winsock2.h>` then collides with it: `struct sockaddr`
// redefinition, `accept`/`bind`/`connect`/… "redefinition; different
// linkage", cascading into hundreds of errors in `ws2tcpip.h`. Defining
// `WIN32_LEAN_AND_MEAN` and including `<winsock2.h>` BEFORE `<dns_sd.h>`
// is the standard fix: winsock2.h's own header guard then stops the
// legacy header from redefining anything when dns_sd.h's windows.h
// include runs.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <poll.h>
#endif
#include <dns_sd.h>
#include <chrono>
#if !defined(PP_DNS_SD_INLINE_RESOLVE)
#include <sys/epoll.h>
#include <unistd.h>
#endif
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

#if defined(PP_DNS_SD_AVAILABLE)
namespace {

// ⚠ TWO RESOLVE STRATEGIES, AND THE SPLIT IS MEASURED RATHER THAN STYLISTIC.
//
// macOS: mDNSResponder answers a local resolve in well under a millisecond, so
// it is done inline with this as a backstop against a responder that will never
// answer.  That is the behaviour this file has always had.
//
// Linux/Avahi: a resolve takes **650 ms** (measured 29 Aug 2026) and will not
// complete AT ALL if called from inside a browse callback.  Inline is therefore
// impossible twice over — it deadlocks, and even deferred it would block the
// GUI thread, which is where `startDiscovery()` watches `fd()`.  So off Apple
// the resolves are genuinely asynchronous and `fd()` returns an epoll set
// holding the browse socket and every pending resolve socket.  One fd out, N
// fds in — which is what lets the `RvBrowser` interface stay exactly as it was.
constexpr int kResolveBudgetMs = 250;

// How long an asynchronous resolve may stay armed before it is abandoned.
// Generous against the measured 650 ms because the cost of waiting is nil —
// nothing blocks on it — while giving up too early loses a real phone.
constexpr int kResolveGiveUpS = 10;

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
#if !defined(PP_DNS_SD_INLINE_RESOLVE)
        m_epoll = ::epoll_create1(0);
        if (m_epoll < 0) {                  // 3.6a again: degrade, never fault
            DNSServiceRefDeallocate(m_browse);
            m_browse = nullptr;
            return false;
        }
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = DNSServiceRefSockFD(m_browse);
        ::epoll_ctl(m_epoll, EPOLL_CTL_ADD, ev.data.fd, &ev);
#endif
        return true;
    }

    void stop() override
    {
        for (auto &kv : m_resolves)
            if (kv.second) DNSServiceRefDeallocate(kv.second);
        m_resolves.clear();
        m_discovered.clear();
#if !defined(PP_DNS_SD_INLINE_RESOLVE)
        for (auto &kv : m_byFd) {
            if (kv.second.first) DNSServiceRefDeallocate(kv.second.first);
            delete kv.second.second;
        }
        m_byFd.clear();
        if (m_epoll >= 0) { ::close(m_epoll); m_epoll = -1; }
#endif
        if (m_browse) {
            DNSServiceRefDeallocate(m_browse);
            m_browse = nullptr;
        }
    }

    int fd() const override
    {
#if defined(PP_DNS_SD_INLINE_RESOLVE)
        return m_browse ? DNSServiceRefSockFD(m_browse) : -1;
#else
        // The epoll set, not the browse socket: it goes readable when the
        // browse OR any pending resolve has something, which is what makes the
        // owner's single QSocketNotifier drive both.
        return m_epoll;
#endif
    }

    bool process() override
    {
        if (!m_browse) return false;
#if defined(PP_DNS_SD_INLINE_RESOLVE)
        const bool ok = DNSServiceProcessResult(m_browse) == kDNSServiceErr_NoError;
        drainDiscovered();
        return ok;
#else
        // Non-blocking: whatever is ready right now and nothing else.  The
        // owner is a QSocketNotifier on the GUI thread and must never wait here.
        struct epoll_event evs[8];
        const int n = ::epoll_wait(m_epoll, evs, 8, 0);
        if (n < 0) return false;
        bool ok = true;
        const int bfd = DNSServiceRefSockFD(m_browse);
        for (int i = 0; i < n; ++i) {
            const int rfd = evs[i].data.fd;
            if (rfd == bfd) {
                ok = DNSServiceProcessResult(m_browse) == kDNSServiceErr_NoError;
                continue;
            }
            auto it = m_byFd.find(rfd);
            if (it == m_byFd.end()) continue;
            DNSServiceProcessResult(it->second.first);
            // ⛔ RETIRE ONLY ONCE IT HAS ANSWERED.  See Pending::done.
            if (it->second.second->done)
                retire(it);
        }
        // New instances from this pass become new pending resolves.  ⛔ AFTER
        // the loop, so DNSServiceResolve is never called on the callback stack.
        drainDiscovered();
        sweepStaleResolves();
        return ok;
#endif
    }

    // A resolve carries its own socket; the owner polls them through here too.
    // Kept simple deliberately: a handful of instances on a range network is
    // the whole population, and 3.6a means being slow to discover costs
    // nothing that matters.
    // ⚠ "browse only" is asserted by ppcp_rendezvous_test and
    // ppcp_host_service_test — it says this object cannot publish, which is
    // RV 2's querier-only role.  Keep the substring.
    std::string describe() const override
    {
#if defined(__APPLE__)
        return "DNS-SD via mDNSResponder (browse only)";
#elif defined(_WIN32)
        return "DNS-SD via Bonjour for Windows (browse only)";
#else
        return "DNS-SD via Avahi compat (browse only)";
#endif
    }

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
        // ⛔ RECORD, DO NOT RESOLVE — WE ARE INSIDE A CALLBACK.
        // Resolving here is what the first Linux port did and it DEADLOCKED:
        // measured 29 Aug 2026, `ppcp_advertise_test` hung for its full 300 s
        // with the main thread in `unix_stream_data_wait`.  A standalone probe
        // isolated the cause — the identical resolve, deferred until after
        // `DNSServiceProcessResult` returns, succeeds in milliseconds
        // (`err=0 host=… port=…`), so Avahi's compat shim simply will not
        // service a resolve re-entrantly.  macOS never showed it because
        // mDNSResponder answers a local resolve immediately.
        self->m_discovered.push_back(Discovered{name, type ? type : "",
                                                domain ? domain : "", ifIndex});
    }

    // An instance the browse reported, awaiting a resolve that must not run on
    // the callback stack.  Owned by value: `name`/`type`/`domain` are borrowed
    // pointers inside the callback and do not outlive it.
    struct Pending {
        BonjourBrowser *self;
        std::string     name;
        // ⛔ A RESOLVE IS NOT DONE AFTER ONE ProcessResult, AND ASSUMING IT WAS
        // COST AN AFTERNOON.  Avahi's compat shim wakes its socket several
        // times before it delivers — the answer took 650 ms and a handful of
        // wakeups when measured standalone.  Retiring the ref on the first
        // wakeup destroys the resolve before it can answer, and the symptom is
        // a browse that finds instances and resolves none of them.
        bool            done = false;
        std::chrono::steady_clock::time_point armed{};
    };

    struct Discovered {
        std::string name;
        std::string type;
        std::string domain;
        std::uint32_t ifIndex;
    };

#if !defined(PP_DNS_SD_INLINE_RESOLVE)
    // Close one pending resolve and free everything it owns.
    void retire(std::map<int, std::pair<DNSServiceRef, Pending *>>::iterator it)
    {
        ::epoll_ctl(m_epoll, EPOLL_CTL_DEL, it->first, nullptr);
        DNSServiceRefDeallocate(it->second.first);
        delete it->second.second;
        m_byFd.erase(it);
    }

    // A resolve that will never answer must not sit in the set for ever — an
    // instance that keeps being announced would otherwise arm a new one on
    // every pass.  3.6a: dropping it is not an error, the next announcement
    // simply tries again.
    void sweepStaleResolves()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = m_byFd.begin(); it != m_byFd.end(); ) {
            const auto age = now - it->second.second->armed;
            if (age > std::chrono::seconds(kResolveGiveUpS)) {
                auto doomed = it++;
                retire(doomed);
            } else {
                ++it;
            }
        }
    }
#endif

    // Resolve everything the last browse callback told us about.  Called from
    // process(), i.e. off the callback stack.
    void drainDiscovered()
    {
        std::vector<Discovered> batch;
        batch.swap(m_discovered);
        for (const Discovered &d : batch)
            resolveOne(d);
    }

    void resolveOne(const Discovered &d)
    {
        DNSServiceRef ref = nullptr;
        auto *pending = new Pending{this, d.name};
        if (DNSServiceResolve(&ref, 0, d.ifIndex, d.name.c_str(), d.type.c_str(),
                              d.domain.c_str(), &BonjourBrowser::onResolve,
                              pending) != kDNSServiceErr_NoError) {
            delete pending;
            return;
        }
        // ⛔ BOUNDED, NOT BLOCKING — AND THE COMMENT THIS REPLACES WAS WRONG.
        // It read "one blocking read on a local IPC socket, off the main thread
        // by construction because process() is called from wherever the owner
        // watches fd()".  ⚠ THE OWNER IS THE GUI THREAD: `startDiscovery()`
        // watches fd() with a QSocketNotifier on it, so an unbounded read here
        // freezes the application.
        //
        // On macOS mDNSResponder answers a local resolve immediately and this
        // was never observed.  On Linux, through Avahi's compat shim, it does
        // NOT return — `ppcp_advertise_test` hung for the full 300 s timeout
        // with the main thread in `unix_stream_data_wait` (measured 29 Aug
        // 2026).  Plausibly re-entrancy: this runs INSIDE a browse callback,
        // and the shim services each DNSServiceRef from its own thread.
        //
        // So: poll the resolve's own fd and give up on a deadline.  Losing a
        // resolve is not an error — 3.4c drops an instance that cannot be
        // resolved anyway, and 3.6a forbids treating discovery failure as a
        // fault; the instance is announced again and the next pass retries.
        // ⚠ A freeze, by contrast, is unrecoverable and blames the wrong
        // subsystem when someone finally looks.
        const int rfd = DNSServiceRefSockFD(ref);
#if defined(PP_DNS_SD_INLINE_RESOLVE)
        if (rfd >= 0) {
#if defined(_WIN32)
            WSAPOLLFD pfd{};
            pfd.fd = static_cast<SOCKET>(rfd);
            pfd.events = POLLIN;
            if (::WSAPoll(&pfd, 1, kResolveBudgetMs) > 0 && (pfd.revents & POLLIN))
                DNSServiceProcessResult(ref);
#else
            struct pollfd pfd;
            pfd.fd = rfd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (::poll(&pfd, 1, kResolveBudgetMs) > 0 && (pfd.revents & POLLIN))
                DNSServiceProcessResult(ref);
#endif
        }
        DNSServiceRefDeallocate(ref);
        delete pending;
#else
        // ⚠ `pending` deliberately OUTLIVES this function off the inline-resolve
        // platforms — the resolve is now asynchronous, so onResolve frees it.
        if (rfd < 0 || m_epoll < 0) {
            DNSServiceRefDeallocate(ref);
            delete pending;
            return;
        }
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = rfd;
        if (::epoll_ctl(m_epoll, EPOLL_CTL_ADD, rfd, &ev) != 0) {
            DNSServiceRefDeallocate(ref);
            delete pending;
            return;
        }
        pending->armed = std::chrono::steady_clock::now();
        m_byFd[rfd] = { ref, pending };
#endif
    }


    static void DNSSD_API onResolve(DNSServiceRef, DNSServiceFlags, uint32_t,
                                    DNSServiceErrorType err, const char *,
                                    const char *hostTarget, uint16_t port,
                                    uint16_t txtLen, const unsigned char *txt, void *ctx)
    {
        auto *p = static_cast<Pending *>(ctx);
        if (!p) return;
        p->done = true;            // answered — the ref may now be retired
        if (err != kDNSServiceErr_NoError) return;

        RvAdvertisement ad;
        ad.instanceName = p->name;
        ad.host = hostTarget ? hostTarget : "";
        ad.port = ntohs(port);
        if (txt && txtLen) parseTxtRecord(txt, txtLen, &ad);
        if (p->self->m_found) p->self->m_found(ad);
    }

    DNSServiceRef m_browse = nullptr;
    std::map<std::string, DNSServiceRef> m_resolves;
    // Instances the browse callback reported, waiting to be resolved off the
    // callback stack — see drainDiscovered().
    std::vector<Discovered> m_discovered;
#if !defined(PP_DNS_SD_INLINE_RESOLVE)
    int m_epoll = -1;                          // the one fd the owner watches
    // resolve socket -> {ref, its callback context}.  ⚠ BOTH are owned here:
    // off the inline-resolve platforms the resolve is asynchronous, so
    // `Pending` outlives resolveOne() and this map is the only thing that can
    // free it.
    std::map<int, std::pair<DNSServiceRef, Pending *>> m_byFd;
#endif
    FoundFn m_found;
    LostFn  m_lost;
};

}  // namespace
#endif  // PP_DNS_SD_AVAILABLE

std::unique_ptr<RvBrowser> makePlatformBrowser()
{
#if defined(_WIN32)
    // W4, second cut — a native engine (ppcp_mdns_native.h) rather than the
    // Bonjour SDK for Windows: Apple's runtime registers a Winsock namespace
    // provider that fails modern Windows Code Integrity / LSA-protection
    // signing checks, on every machine that has it installed, whether or not
    // this application is even running. Unlike the Bonjour SDK path, this
    // never returns null for want of an installed dependency — there is
    // none. A `start()` failure later (no usable interface — 3.6a) is the
    // only way discovery ends up disabled on this platform now.
    return Ppcp::Mdns::makeNativeBrowser();
#elif defined(PP_DNS_SD_AVAILABLE)
    return std::unique_ptr<RvBrowser>(new BonjourBrowser);
#else
    // No DNS-SD client on this platform.  3.6b: failure to discover falls
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

#if defined(PP_DNS_SD_AVAILABLE)
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
#endif  // PP_DNS_SD_AVAILABLE

std::unique_ptr<RvAdvertiser> makePlatformAdvertiser()
{
#if defined(_WIN32)
    // W4, second cut — see makePlatformBrowser()'s comment above; the same
    // native engine, no Bonjour SDK dependency, no delay-load probe needed
    // because there is no external DLL to probe for.
    return Ppcp::Mdns::makeNativeAdvertiser();
#elif defined(PP_DNS_SD_AVAILABLE)
    return std::unique_ptr<RvAdvertiser>(new BonjourAdvertiser);
#else
    // No DNS-SD responder on this platform (CA5's original concern, but no
    // longer Windows' reason for landing here — see above). 3.6b makes the
    // consequence silent: no advertisement, so such a host is reached by
    // pairing code every session until somebody decides.
    //
    // ⚠ LINUX IS NOT IN THIS BRANCH EITHER.  Avahi's compat shim supplies the
    // same API, so a Linux box with it installed advertises exactly as macOS
    // does; a Linux box WITHOUT it lands here, which is why this stays a
    // silent null rather than becoming an error.
    return nullptr;
#endif
}

}  // namespace Ppcp
