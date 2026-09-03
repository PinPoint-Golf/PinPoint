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

// A Multicast DNS / DNS-SD wire codec, written from scratch against RFC 6762
// (Multicast DNS) and RFC 6763 (DNS-Based Service Discovery) — no code,
// algorithm or structure drawn from any existing mDNS implementation (Apple's
// mDNSResponder, Avahi, or otherwise) and no third-party library. Everything
// below is either this project's own writing against the public RFC text, or
// a direct call into a Windows OS API (Winsock, in ppcp_mdns_native.cpp) —
// nothing here can raise a GPL-compatibility question.
//
// WHY THIS EXISTS. The Bonjour SDK for Windows path (W4, wired transport
// design doc) works, but it depends on Apple's Bonjour runtime, whose Winsock
// namespace provider (mdnsNSP.dll, last touched 2015) fails modern Windows
// Code Integrity / LSA-protection signing checks. Measured on this box:
// lsass.exe hits the block once per boot (the user-visible "blocked from
// loading into the Local Security Authority" popup), and svchost.exe /
// Windows Defender's own service hit it every few minutes in the background
// — regardless of whether PinPointStudio is even running, for as long as
// Bonjour is installed. Since every Windows operator who wants working
// WiFi reconnection (the "done when" bar) would need to install Bonjour,
// this dependency would put that popup on every one of their machines. This
// engine removes the dependency instead of working around its symptom.
//
// SCOPE, DELIBERATELY NARROW:
//   - One service type only: `_ppcp._tcp.local`. This is not a general-
//     purpose DNS-SD library.
//   - IPv4 only. IPv6 mDNS uses a different multicast group (ff02::fb) and
//     its own address family handling; a real gap, not an oversight, left
//     for a day IPv6-only Windows LANs are a case that actually arises.
//   - No DNS name compression ON WRITE. Our record set (PTR+SRV+TXT+A, one
//     service) is a few hundred bytes at most, nowhere near needing it.
//     Compression on READ (following a pointer) IS implemented — real
//     responders use it, and a reader that cannot follow one silently drops
//     a valid answer.
//   - No known-answer suppression (RFC 6762 §7.1). An optimisation, not a
//     correctness requirement; skipping it means a few extra packets on a
//     network RV 3.6a already tolerates being lossy on.
//   - Every incoming resource-record section (Answer, Authority, Additional)
//     is read into ONE flat list. The three-way split exists in the RFC to
//     hint a resolver's trust/caching policy; this engine trusts nothing it
//     did not assert itself and caches nothing beyond one browse session, so
//     the distinction carries no information it acts on.

#include <cstdint>
#include <string>
#include <vector>

namespace Ppcp {
namespace Mdns {

constexpr std::uint16_t kClassIN = 1;
constexpr std::uint16_t kTypeA   = 1;
constexpr std::uint16_t kTypePTR = 12;
constexpr std::uint16_t kTypeTXT = 16;
constexpr std::uint16_t kTypeSRV = 33;

constexpr std::uint16_t kPort = 5353;
// RFC 6762 §3: the mDNS IPv4 link-local multicast address.
constexpr const char *kMulticastGroupV4 = "224.0.0.251";

// ── Writing ──────────────────────────────────────────────────────────────
//
// One-shot builder: writeHeader() first (it needs the final question/answer
// counts, which the caller already knows — it built the list), then that
// many writeQuestion()/writeRecord() calls, in order. No random access, no
// two-phase counting: correct by construction, because the caller cannot get
// the count wrong without ALSO getting the call count wrong, and those are
// the same variable at the call site.
class MessageWriter {
public:
    // response=false is a QUERY (QR=0, RFC 6762 §18). response=true is what
    // every message we send in reply to one is — authoritative=true always
    // accompanies it in this engine, because we ARE the origin of every
    // record we ever answer with (RFC 6762 §18.4: "In response messages,
    // the Authoritative Answer bit MUST be set to one").
    void writeHeader(bool response, bool authoritative, std::uint16_t qdcount,
                     std::uint16_t ancount);

    // QCLASS is always written as plain IN (no QU bit): this engine always
    // accepts and always sends multicast responses (RFC 6762 §5.4's
    // unicast-reply path is a traffic optimisation, not a requirement — a
    // querier MUST accept a multicast reply regardless of what it asked).
    void writeQuestion(const std::string &name, std::uint16_t qtype);

    // cacheFlush sets the top bit of RRCLASS per RFC 6762 §10.2 — "this is
    // the entire, exclusive RRSet as of now" — true for every record this
    // engine asserts, since it is the sole, authoritative source of each one.
    void writeRecord(const std::string &name, std::uint16_t rtype, bool cacheFlush,
                     std::uint32_t ttlSeconds, const std::vector<std::uint8_t> &rdata);

    // RDATA builders. PTR/CNAME-shaped (a bare name) and SRV (RFC 2782)
    // are the two name-bearing types this engine ever writes.
    static std::vector<std::uint8_t> rdataName(const std::string &name);
    static std::vector<std::uint8_t> rdataSrv(std::uint16_t priority, std::uint16_t weight,
                                                std::uint16_t port, const std::string &target);
    static std::vector<std::uint8_t> rdataA(const std::string &ipv4Literal);
    // TXT rdata is NOT built here. DNS-SD's TXT record (RFC 6763 §6.1) and
    // PPCP-RV's TXT record are the same length-prefixed-strings format by
    // construction — `buildTxtRecord()`'s output (ppcp_discovery.h) IS valid
    // TXT rdata verbatim. A second encoder for the identical wire format is
    // a second place for the two to quietly disagree; there isn't one.

    const std::vector<std::uint8_t> &bytes() const { return m_buf; }

private:
    void writeName(const std::string &name);
    std::vector<std::uint8_t> m_buf;
};

// ── Reading ──────────────────────────────────────────────────────────────

struct Question {
    std::string name;
    std::uint16_t qtype = 0;
    // The QU bit (RFC 6762 §5.4) — informational only. This engine always
    // answers via multicast regardless, so nothing reads this field yet;
    // kept because dropping information a caller might reasonably want costs
    // one bool and silently discarding it costs a future debugging session.
    bool unicastRequested = false;
};

// One resource record. A NAME-bearing type's RDATA is decoded eagerly
// against the FULL packet buffer at parse time, into `targetName` — RFC 6762
// permits a compression pointer INSIDE rdata that references earlier in the
// SAME packet (e.g. an SRV target commonly points back at a name the PTR
// record already spelled out), and decoding against an isolated copy of the
// rdata bytes breaks exactly there. TXT/A rdata carry no names and are kept
// as raw bytes — for TXT, in the exact form `parseTxtRecord()` already
// expects.
struct Record {
    std::string name;
    std::uint16_t rtype = 0;
    bool cacheFlush = false;
    std::uint32_t ttl = 0;

    std::string targetName;              // PTR: the instance name. SRV: the host target.
    std::uint16_t srvPort = 0;           // SRV only.
    std::vector<std::uint8_t> rawRdata;  // TXT: the record verbatim. A: 4 bytes, network order.
};

struct Message {
    bool response = false;
    std::vector<Question> questions;
    std::vector<Record> records;   // answer + authority + additional, flattened — see above
};

// Parses the header, every question and every resource record this engine
// knows how to decode (PTR, SRV, TXT, A — anything else is a record this
// engine skips over using RDLENGTH, not a parse failure). Returns false only
// when the MESSAGE ITSELF is malformed — a truncated header, a length that
// runs past the buffer, a compression-pointer loop. On a shared multicast
// segment a malformed packet is an ordinary event (RV 3.6a's discipline,
// inherited here even though this file sits below that layer): the caller
// drops it and keeps listening, never treats it as a fault.
bool parseMessage(const std::uint8_t *data, std::size_t len, Message *out);

}  // namespace Mdns
}  // namespace Ppcp
