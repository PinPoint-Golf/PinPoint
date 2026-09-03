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

#include "ppcp_mdns_native.h"

#include "ppcp_mdns_wire.h"
#include "ppcp_rendezvous.h"   // reachableEndpoints() — the same address set the QR code uses

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>

namespace Ppcp {
namespace Mdns {
namespace {

// Long-lived: PTR/SRV/A never change for the life of one registration — only
// `updateTxt()` ever changes anything, and only the TXT record. RFC 6762
// §10's own recommendation for this category of "unlikely to change" record.
constexpr std::uint32_t kLongLivedTtlS = 4500;
// Short-lived: this is the record that DOES rotate (3.4a/3.4d1), and a
// shorter TTL bounds how long a listener that missed our update announce
// keeps trusting a stale `rn`/`rid` before it re-asks.
constexpr std::uint32_t kTxtTtlS = 120;

void ensureWinsock()
{
    struct Once {
        Once() { WSADATA d; ::WSAStartup(MAKEWORD(2, 2), &d); }
    };
    static Once once;
    (void)once;
}

// A non-blocking UDP socket bound to 0.0.0.0:5353, joined to the mDNS
// multicast group. SO_REUSEADDR lets the advertiser and browser — or a
// still-running Bonjour Service, if the operator has not uninstalled it —
// share the port: mDNS is explicitly a multi-listener protocol on a shared
// segment, so this is how every mDNS stack there already behaves, not a
// workaround.
SOCKET openMulticastSocket()
{
    ensureWinsock();

    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    BOOL reuse = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof reuse);

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(kPort);
    if (::bind(s, reinterpret_cast<sockaddr *>(&bindAddr), sizeof bindAddr) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(kMulticastGroupV4);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char *>(&mreq),
                     sizeof mreq) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }

    // RFC 6762 §11: the source IP TTL on every mDNS packet MUST be 255 — a
    // receiver that checks it can reject anything a router forwarded onto
    // the segment from elsewhere. (This engine does not check it on RECEIVE
    // — that needs WSARecvMsg and ancillary-data parsing, real additional
    // complexity for a hardening measure our actual security boundary,
    // TLS-PSK, does not depend on: RV 3.6a already treats every mDNS field
    // as untrusted, and an off-link spoof here costs a wasted dial attempt
    // that fails at the handshake, not a compromised pairing. Scoped out,
    // not overlooked.)
    int ttl = 255;
    ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char *>(&ttl), sizeof ttl);

    u_long nonBlocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonBlocking);

    return s;
}

void leaveAndClose(SOCKET s)
{
    if (s == INVALID_SOCKET) return;
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(kMulticastGroupV4);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    ::setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char *>(&mreq),
                sizeof mreq);
    ::closesocket(s);
}

void sendMulticast(SOCKET s, const std::vector<std::uint8_t> &bytes)
{
    if (s == INVALID_SOCKET) return;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ::inet_addr(kMulticastGroupV4);
    dst.sin_port = htons(kPort);
    ::sendto(s, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<sockaddr *>(&dst), sizeof dst);
}

// This machine's own reachable IPv4 addresses — the SAME enumeration RV
// 4.3d's QR code already uses (ppcp_rendezvous.cpp's reachableEndpoints()),
// so the address set a phone gets over mDNS and the address set it gets from
// a scanned code are never two independently-maintained answers to the same
// question. Loopback is dropped: nothing on the LAN can usefully dial it.
std::vector<std::string> ownIPv4Addresses()
{
    std::vector<std::string> out;
    for (const RvEndpoint &e : reachableEndpoints(0)) {
        if (e.host == "127.0.0.1" || e.host == "::1") continue;
        if (e.host.find(':') != std::string::npos) continue;   // IPv6 — out of scope, see the header
        out.push_back(e.host);
    }
    return out;
}

// "<computer name>.local" — the SRV target and the name our A record(s) are
// published under. Sanitised to a valid DNS label: GetComputerNameExW can
// return characters (spaces, in particular, on a machine named with one)
// that RFC 1035 labels do not allow; anything outside [A-Za-z0-9-] becomes
// '-', and a run of them collapses to one, matching what real mDNS stacks
// do to a hostname for exactly this reason.
std::string localMdnsHostname()
{
    wchar_t buf[256];
    DWORD n = sizeof buf / sizeof buf[0];
    std::string name = "pinpointstudio";
    if (::GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) {
        int needed = ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0,
                                           nullptr, nullptr);
        if (needed > 0) {
            std::string utf8(static_cast<std::size_t>(needed), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), utf8.data(), needed,
                                  nullptr, nullptr);
            name = utf8;
        }
    }
    std::string label;
    label.reserve(name.size());
    bool lastWasDash = false;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-';
        if (ok) {
            label.push_back(c);
            lastWasDash = (c == '-');
        } else if (!lastWasDash && !label.empty()) {
            label.push_back('-');
            lastWasDash = true;
        }
    }
    while (!label.empty() && label.back() == '-') label.pop_back();
    if (label.empty()) label = "pinpointstudio";
    if (label.size() > 63) label.resize(63);
    return label + ".local";
}

std::uint64_t nowS()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count());
}

bool iequalsSuffix(const std::string &s, const std::string &suffixLower)
{
    if (s.size() < suffixLower.size()) return false;
    const std::string tail = s.substr(s.size() - suffixLower.size());
    return std::equal(tail.begin(), tail.end(), suffixLower.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    });
}

// "PPCP-XXXXXXXX._ppcp._tcp.local" -> "PPCP-XXXXXXXX". The suffix compares
// case-insensitively (RFC 1035 §3.1's rule for domain-name comparison); the
// instance label itself is returned VERBATIM — 3.2a's "eight UPPERCASE hex
// characters" and instanceNameMatchesRid()'s exact string comparison both
// depend on it never being case-folded.
std::string stripInstanceLabel(const std::string &fullName)
{
    static const std::string kSuffix = "._ppcp._tcp.local";
    if (!iequalsSuffix(fullName, kSuffix)) return {};
    return fullName.substr(0, fullName.size() - kSuffix.size());
}

bool iequals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

// ── The advertiser ───────────────────────────────────────────────────────
class NativeAdvertiser final : public RvAdvertiser {
public:
    ~NativeAdvertiser() override { stop(); }

    bool start(const std::string &instanceName, std::uint16_t port,
              const std::vector<std::uint8_t> &txt) override
    {
        stop();
        if (instanceName.empty() || port == 0 || txt.empty() || txt.size() > 0xffff) return false;

        m_addresses = ownIPv4Addresses();
        if (m_addresses.empty()) return false;   // 3.6a — nothing reachable, nothing to advertise

        m_sock = openMulticastSocket();
        if (m_sock == INVALID_SOCKET) return false;

        m_instanceName = instanceName;
        m_port = port;
        m_txt = txt;
        m_hostname = localMdnsHostname();

        announce();
        m_registered = true;
        return true;
    }

    bool updateTxt(const std::vector<std::uint8_t> &txt) override
    {
        if (m_sock == INVALID_SOCKET || !m_registered) return false;
        if (txt.empty() || txt.size() > 0xffff) return false;
        m_txt = txt;
        // 3.2d's whole point: ONE record changes.  PTR and SRV are neither
        // re-sent nor re-derived here — sending them again would be
        // harmless on THIS wire (identical content), but it is the shape
        // the contract explicitly asks an implementation not to reason its
        // way back into.
        MessageWriter w;
        w.writeHeader(true, true, 0, 1);
        const std::string full = m_instanceName + "._ppcp._tcp.local";
        w.writeRecord(full, kTypeTXT, true, kTxtTtlS, m_txt);
        sendMulticast(m_sock, w.bytes());
        return true;
    }

    void stop() override
    {
        if (m_sock != INVALID_SOCKET) {
            if (m_registered) goodbye();
            leaveAndClose(m_sock);
            m_sock = INVALID_SOCKET;
        }
        m_registered = false;
        m_instanceName.clear();
    }

    int fd() const override { return m_sock == INVALID_SOCKET ? -1 : static_cast<int>(m_sock); }

    bool process() override
    {
        if (m_sock == INVALID_SOCKET) return false;
        unsigned char buf[2048];
        for (;;) {
            sockaddr_in from{};
            int fromLen = sizeof from;
            const int n = ::recvfrom(m_sock, reinterpret_cast<char *>(buf), sizeof buf, 0,
                                     reinterpret_cast<sockaddr *>(&from), &fromLen);
            if (n <= 0) {
                if (n == 0) continue;   // a zero-length datagram is not EOF on UDP
                return true;            // WSAEWOULDBLOCK (drained) or a transient error either way
            }
            handleIncoming(buf, static_cast<std::size_t>(n));
        }
    }

    std::string describe() const override
    {
        return "native mDNS, no Bonjour dependency (register)";
    }

    std::string registeredName() const override
    {
        return m_registered ? m_instanceName : std::string();
    }

private:
    void appendAnswerSet(MessageWriter &w, std::uint32_t ttl) const
    {
        const std::string full = m_instanceName + "._ppcp._tcp.local";
        // PTR is a SHARED record (many hosts' PTRs coexist under the same
        // service-type name) — never cache-flush, per RFC 6762 §10.2.
        w.writeRecord("_ppcp._tcp.local", kTypePTR, /*cacheFlush=*/false, kLongLivedTtlS,
                      MessageWriter::rdataName(full));
        w.writeRecord(full, kTypeSRV, /*cacheFlush=*/true, ttl,
                      MessageWriter::rdataSrv(0, 0, m_port, m_hostname));
        w.writeRecord(full, kTypeTXT, /*cacheFlush=*/true, kTxtTtlS, m_txt);
        for (const std::string &ip : m_addresses)
            w.writeRecord(m_hostname, kTypeA, /*cacheFlush=*/true, ttl,
                          MessageWriter::rdataA(ip));
    }

    void announce()
    {
        MessageWriter w;
        const std::uint16_t ancount = static_cast<std::uint16_t>(3 + m_addresses.size());
        w.writeHeader(true, true, 0, ancount);
        appendAnswerSet(w, kLongLivedTtlS);
        sendMulticast(m_sock, w.bytes());
    }

    void goodbye()
    {
        // RFC 6762 §10.1: every record we asserted, TTL=0 — tells anyone
        // caching us to drop it now rather than waiting out the TTL.
        MessageWriter w;
        const std::uint16_t ancount = static_cast<std::uint16_t>(3 + m_addresses.size());
        w.writeHeader(true, true, 0, ancount);
        appendAnswerSet(w, 0);
        sendMulticast(m_sock, w.bytes());
    }

    void handleIncoming(const unsigned char *data, std::size_t len)
    {
        Message m;
        if (!parseMessage(data, len, &m)) return;   // malformed — RV 3.6a, drop and keep listening
        if (m.response) return;                     // we answer queries, not other responses

        const std::string full = m_instanceName + "._ppcp._tcp.local";
        for (const Question &q : m.questions) {
            if (iequals(q.name, "_ppcp._tcp.local") || iequals(q.name, full) ||
                iequals(q.name, m_hostname)) {
                announce();   // the whole bundle, every time — see ppcp_mdns_wire.h's header
                return;
            }
        }
    }

    SOCKET m_sock = INVALID_SOCKET;
    bool m_registered = false;
    std::string m_instanceName;
    std::uint16_t m_port = 0;
    std::vector<std::uint8_t> m_txt;
    std::string m_hostname;
    std::vector<std::string> m_addresses;
};

// ── The browser ──────────────────────────────────────────────────────────
class NativeBrowser final : public RvBrowser {
public:
    ~NativeBrowser() override { stop(); }

    bool start(FoundFn onFound, LostFn onLost) override
    {
        stop();
        m_sock = openMulticastSocket();
        if (m_sock == INVALID_SOCKET) return false;
        m_onFound = std::move(onFound);
        m_onLost = std::move(onLost);
        sendQuery();
        m_queryIntervalS = 1;
        m_nextQueryS = nowS() + m_queryIntervalS;
        return true;
    }

    void stop() override
    {
        if (m_sock != INVALID_SOCKET) {
            leaveAndClose(m_sock);
            m_sock = INVALID_SOCKET;
        }
        m_instances.clear();
        m_onFound = nullptr;
        m_onLost = nullptr;
    }

    int fd() const override { return m_sock == INVALID_SOCKET ? -1 : static_cast<int>(m_sock); }

    bool process() override
    {
        if (m_sock == INVALID_SOCKET) return false;
        unsigned char buf[2048];
        for (;;) {
            sockaddr_in from{};
            int fromLen = sizeof from;
            const int n = ::recvfrom(m_sock, reinterpret_cast<char *>(buf), sizeof buf, 0,
                                     reinterpret_cast<sockaddr *>(&from), &fromLen);
            if (n <= 0) {
                if (n == 0) continue;
                return true;
            }
            handleIncoming(buf, static_cast<std::size_t>(n));
        }
    }

    std::string describe() const override
    {
        return "native mDNS, no Bonjour dependency (browse only)";
    }

    void tick(std::uint64_t t) override
    {
        if (m_sock == INVALID_SOCKET) return;

        // RFC 6762 §5.2's continuous-querying schedule: repeat at increasing
        // intervals starting at ~1s, capped, rather than a fixed cadence —
        // fast when a browse is fresh, quiet once it has settled.
        if (t >= m_nextQueryS) {
            sendQuery();
            m_queryIntervalS = std::min<std::uint64_t>(m_queryIntervalS * 2, kMaxQueryIntervalS);
            m_nextQueryS = t + m_queryIntervalS;
        }

        // Expire anything not refreshed inside its own TTL. Whether or not a
        // goodbye packet ever arrives, a record we have not heard about
        // again inside its TTL is gone in every sense this caller acts on —
        // packet loss on a lossy multicast segment is ordinary (3.6a).
        for (auto it = m_instances.begin(); it != m_instances.end();) {
            if (t >= it->second.expiresAtS) {
                const std::string name = it->first;
                it = m_instances.erase(it);
                if (m_onLost) m_onLost(name);
            } else {
                ++it;
            }
        }
    }

private:
    static constexpr std::uint64_t kMaxQueryIntervalS = 60;

    struct Pending {
        bool hasSrv = false, hasTxt = false, hasA = false;
        std::string hostTarget;
        std::uint16_t port = 0;
        std::vector<std::uint8_t> txt;
        std::string ipv4;
        std::uint64_t expiresAtS = 0;
    };

    void sendQuery()
    {
        MessageWriter w;
        w.writeHeader(false, false, 1, 0);
        w.writeQuestion("_ppcp._tcp.local", kTypePTR);
        sendMulticast(m_sock, w.bytes());
    }

    void handleIncoming(const unsigned char *data, std::size_t len)
    {
        Message m;
        if (!parseMessage(data, len, &m)) return;   // malformed — RV 3.6a, drop and keep listening
        if (!m.response) return;   // we act on answers; queries are the advertiser's job

        const std::uint64_t t = nowS();
        std::vector<std::string> touched;

        for (const Record &r : m.records) {
            if (r.rtype == kTypePTR) {
                if (!iequals(r.name, "_ppcp._tcp.local")) continue;
                if (r.ttl == 0) {
                    // A goodbye for the PTR itself — the instance withdrew.
                    // tick()'s expiry sweep would also catch this once its
                    // TTL lapses, but honouring it immediately is what a
                    // goodbye packet exists for (RFC 6762 §10.1).
                    const std::string inst = stripInstanceLabel(r.targetName);
                    if (!inst.empty() && m_instances.erase(inst) > 0 && m_onLost) m_onLost(inst);
                    continue;
                }
                const std::string inst = stripInstanceLabel(r.targetName);
                if (inst.empty()) continue;
                Pending &p = m_instances[inst];
                p.expiresAtS = t + r.ttl;
                touched.push_back(inst);
            } else if (r.rtype == kTypeSRV) {
                const std::string inst = stripInstanceLabel(r.name);
                if (inst.empty()) continue;
                Pending &p = m_instances[inst];
                p.hasSrv = true;
                p.hostTarget = r.targetName;
                p.port = r.srvPort;
                p.expiresAtS = t + r.ttl;
                touched.push_back(inst);
            } else if (r.rtype == kTypeTXT) {
                const std::string inst = stripInstanceLabel(r.name);
                if (inst.empty()) continue;
                Pending &p = m_instances[inst];
                p.hasTxt = true;
                p.txt = r.rawRdata;
                p.expiresAtS = t + r.ttl;
                touched.push_back(inst);
            } else if (r.rtype == kTypeA) {
                if (r.rawRdata.size() != 4) continue;
                char buf[INET_ADDRSTRLEN] = {0};
                in_addr addr{};
                std::memcpy(&addr, r.rawRdata.data(), 4);
                if (!::InetNtopA(AF_INET, &addr, buf, sizeof buf)) continue;
                // An A record is keyed by HOSTNAME, not by instance — walk
                // every pending instance whose SRV target names this host.
                // Normally exactly one; nothing here assumes it is.
                for (auto &kv : m_instances) {
                    if (kv.second.hasSrv && iequals(kv.second.hostTarget, r.name)) {
                        kv.second.hasA = true;
                        kv.second.ipv4 = buf;
                        touched.push_back(kv.first);
                    }
                }
            }
        }

        for (const std::string &inst : touched) tryEmit(inst);
    }

    // Fires FoundFn once PTR (already implied by the instance existing in
    // the map), SRV, TXT and A are all known — the same bar RvBrowser's own
    // FoundFn contract sets ("resolves far enough to carry rn and rid").
    // Fires again on every subsequent update (a TXT rotation, most often);
    // the caller (PpcpHostService::startDiscovery()'s FoundFn) already
    // de-duplicates on (instance, resolved pairing) before acting on it, so
    // firing more than once per instance is not a defect here — see its own
    // `m_seenInstances.value(inst) == pid` check.
    void tryEmit(const std::string &instanceName)
    {
        auto it = m_instances.find(instanceName);
        if (it == m_instances.end()) return;
        const Pending &p = it->second;
        if (!p.hasSrv || !p.hasTxt || !p.hasA) return;

        RvAdvertisement ad;
        ad.instanceName = instanceName;
        ad.host = p.ipv4;
        ad.port = p.port;
        // parseTxtRecord() only ever touches the TXT-derived fields
        // (txtvers/pv/role/rn/rid/bs/dl) — instanceName/host/port above are
        // untouched by it, by inspection of its own implementation.
        if (!parseTxtRecord(p.txt.data(), p.txt.size(), &ad)) return;

        if (m_onFound) m_onFound(ad);
    }

    SOCKET m_sock = INVALID_SOCKET;
    FoundFn m_onFound;
    LostFn m_onLost;
    std::map<std::string, Pending> m_instances;
    std::uint64_t m_queryIntervalS = 1;
    std::uint64_t m_nextQueryS = 0;
};

}  // namespace

std::unique_ptr<RvBrowser> makeNativeBrowser()
{
    return std::unique_ptr<RvBrowser>(new NativeBrowser);
}

std::unique_ptr<RvAdvertiser> makeNativeAdvertiser()
{
    return std::unique_ptr<RvAdvertiser>(new NativeAdvertiser);
}

}  // namespace Mdns
}  // namespace Ppcp

#else  // !_WIN32

namespace Ppcp {
namespace Mdns {
std::unique_ptr<RvBrowser> makeNativeBrowser() { return nullptr; }
std::unique_ptr<RvAdvertiser> makeNativeAdvertiser() { return nullptr; }
}  // namespace Mdns
}  // namespace Ppcp

#endif
