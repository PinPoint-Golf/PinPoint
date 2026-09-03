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

#include "ppcp_mdns_wire.h"

#include <cstring>
#include <sstream>

namespace Ppcp {
namespace Mdns {
namespace {

void putU16(std::vector<std::uint8_t> &buf, std::uint16_t v)
{
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    buf.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void putU32(std::vector<std::uint8_t> &buf, std::uint32_t v)
{
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    buf.push_back(static_cast<std::uint8_t>(v & 0xff));
}

std::uint16_t getU16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t getU32(const std::uint8_t *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

// Splits "a.b.local" into labels ["a","b","local"] on '.'. mDNS service
// names never contain an escaped dot (RFC 6763's instance-name escaping
// rules exist for arbitrary human-readable names; PPCP's own names are
// "PPCP-XXXXXXXX", "_ppcp", "_tcp" and "local" — none of which can contain
// one), so a bare split is exact for every name this engine ever writes.
std::vector<std::string> splitLabels(const std::string &name)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : name) {
        if (c == '.') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

void MessageWriter::writeHeader(bool response, bool authoritative, std::uint16_t qdcount,
                                std::uint16_t ancount)
{
    putU16(m_buf, 0);   // ID — RFC 6762 §18.1: SHOULD be zero on transmission
    std::uint16_t flags = 0;
    if (response) flags |= 0x8000;        // QR
    if (authoritative) flags |= 0x0400;   // AA
    putU16(m_buf, flags);
    putU16(m_buf, qdcount);
    putU16(m_buf, ancount);
    putU16(m_buf, 0);   // NSCOUNT — unused, see the header comment
    putU16(m_buf, 0);   // ARCOUNT — unused, see the header comment
}

void MessageWriter::writeName(const std::string &name)
{
    for (const std::string &label : splitLabels(name)) {
        // 63 bytes is RFC 1035's hard per-label ceiling (the top two bits of
        // the length byte are reserved to distinguish a length from a
        // compression pointer) — every label this engine ever writes is a
        // handful of characters, so this is a defensive truncation that
        // should never fire, not a real limit anyone hits.
        const std::size_t n = label.size() > 63 ? 63 : label.size();
        m_buf.push_back(static_cast<std::uint8_t>(n));
        m_buf.insert(m_buf.end(), label.begin(), label.begin() + static_cast<long>(n));
    }
    m_buf.push_back(0);   // the terminating zero-length label
}

void MessageWriter::writeQuestion(const std::string &name, std::uint16_t qtype)
{
    writeName(name);
    putU16(m_buf, qtype);
    putU16(m_buf, kClassIN);   // no QU bit — see the header comment
}

void MessageWriter::writeRecord(const std::string &name, std::uint16_t rtype, bool cacheFlush,
                                std::uint32_t ttlSeconds, const std::vector<std::uint8_t> &rdata)
{
    writeName(name);
    putU16(m_buf, rtype);
    putU16(m_buf, static_cast<std::uint16_t>(kClassIN | (cacheFlush ? 0x8000u : 0u)));
    putU32(m_buf, ttlSeconds);
    putU16(m_buf, static_cast<std::uint16_t>(rdata.size()));
    m_buf.insert(m_buf.end(), rdata.begin(), rdata.end());
}

std::vector<std::uint8_t> MessageWriter::rdataName(const std::string &name)
{
    MessageWriter w;
    w.writeName(name);
    return w.m_buf;
}

std::vector<std::uint8_t> MessageWriter::rdataSrv(std::uint16_t priority, std::uint16_t weight,
                                                    std::uint16_t port, const std::string &target)
{
    std::vector<std::uint8_t> out;
    putU16(out, priority);
    putU16(out, weight);
    putU16(out, port);
    MessageWriter w;
    w.writeName(target);
    out.insert(out.end(), w.m_buf.begin(), w.m_buf.end());
    return out;
}

std::vector<std::uint8_t> MessageWriter::rdataA(const std::string &ipv4Literal)
{
    // A dotted-quad parsed by hand rather than via inet_pton: this file has
    // no socket dependency and no platform #ifdef, and four small integers
    // are cheaper to parse here than to justify pulling Winsock into a file
    // that is otherwise pure wire format.
    std::vector<std::uint8_t> out(4, 0);
    unsigned parts[4] = {0, 0, 0, 0};
    int idx = 0;
    unsigned cur = 0;
    bool any = false;
    for (char c : ipv4Literal) {
        if (c == '.') {
            if (idx >= 4) return std::vector<std::uint8_t>(4, 0);
            parts[idx++] = cur;
            cur = 0;
            any = false;
        } else if (c >= '0' && c <= '9') {
            cur = cur * 10 + static_cast<unsigned>(c - '0');
            any = true;
        } else {
            return std::vector<std::uint8_t>(4, 0);   // not a dotted-quad; caller's problem
        }
    }
    if (idx == 3 && any) parts[3] = cur;
    for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(parts[i] & 0xff);
    return out;
}

namespace {

// Reads one (possibly compressed) name starting at `pos`. `*consumed` is set
// to the number of bytes AT `pos` the name occupies at the OUTER level —
// which, when the name is or ends in a compression pointer, is everything up
// to and including that 2-byte pointer, and NOT the bytes at the pointer's
// target (RFC 1035 §4.1.4: a pointer always terminates the name at the level
// where it appears; what a followed pointer finds belongs to a DIFFERENT
// occurrence of the name elsewhere in the packet, not to this one's length).
//
// Jump-guarded rather than trusting the input: a malformed or hostile packet
// on a shared multicast segment is an ordinary event (this file's header
// comment), never a crash. `kMaxLabels` bounds total work regardless of how
// the pointers are arranged, which is simpler and just as safe as tracking
// visited offsets.
bool readName(const std::uint8_t *data, std::size_t len, std::size_t pos, std::string *out,
              std::size_t *consumed)
{
    out->clear();
    constexpr int kMaxLabels = 128;
    std::size_t cur = pos;
    std::size_t outerConsumed = 0;
    bool consumedSet = false;
    bool first = true;

    for (int labels = 0; labels < kMaxLabels; ++labels) {
        if (cur >= len) return false;
        const std::uint8_t b = data[cur];

        if ((b & 0xc0) == 0xc0) {   // a compression pointer
            if (cur + 1 >= len) return false;
            if (!consumedSet) {
                outerConsumed = cur + 2 - pos;
                consumedSet = true;
            }
            const std::size_t target =
                (static_cast<std::size_t>(b & 0x3f) << 8) | data[cur + 1];
            if (target >= len) return false;
            cur = target;
            continue;
        }
        if ((b & 0xc0) != 0) return false;   // the two reserved label-length prefixes

        const std::size_t labelLen = b;
        if (labelLen == 0) {
            if (!consumedSet) outerConsumed = cur + 1 - pos;
            *consumed = outerConsumed;
            return true;
        }
        if (cur + 1 + labelLen > len) return false;
        if (!first) out->push_back('.');
        out->append(reinterpret_cast<const char *>(data + cur + 1), labelLen);
        first = false;
        cur += 1 + labelLen;
    }
    return false;   // more labels than any name this engine writes or expects could have
}

bool readQuestion(const std::uint8_t *data, std::size_t len, std::size_t *pos, Question *out)
{
    std::size_t nameLen = 0;
    if (!readName(data, len, *pos, &out->name, &nameLen)) return false;
    std::size_t p = *pos + nameLen;
    if (p + 4 > len) return false;
    out->qtype = getU16(data + p);
    const std::uint16_t rawClass = getU16(data + p + 2);
    out->unicastRequested = (rawClass & 0x8000) != 0;
    p += 4;
    *pos = p;
    return true;
}

bool readRecord(const std::uint8_t *data, std::size_t len, std::size_t *pos, Record *out)
{
    std::size_t nameLen = 0;
    if (!readName(data, len, *pos, &out->name, &nameLen)) return false;
    std::size_t p = *pos + nameLen;
    if (p + 10 > len) return false;
    out->rtype = getU16(data + p);
    const std::uint16_t rawClass = getU16(data + p + 2);
    out->cacheFlush = (rawClass & 0x8000) != 0;
    out->ttl = getU32(data + p + 4);
    const std::uint16_t rdlength = getU16(data + p + 8);
    p += 10;
    if (p + rdlength > len) return false;
    const std::size_t rdataStart = p;

    if (out->rtype == kTypePTR) {
        std::size_t nl = 0;
        if (!readName(data, len, rdataStart, &out->targetName, &nl)) return false;
        // A name inside RDATA is not itself bounded by RDLENGTH the way a
        // fixed-width field is — RFC 1035 explicitly allows it to point
        // outside the record via compression — so `nl` is not checked
        // against `rdlength` here; only the record's own p+rdlength bound
        // (already checked above) protects the outer cursor.
    } else if (out->rtype == kTypeSRV) {
        if (rdlength < 6) return false;
        out->srvPort = getU16(data + rdataStart + 4);
        std::size_t nl = 0;
        if (!readName(data, len, rdataStart + 6, &out->targetName, &nl)) return false;
    } else {
        out->rawRdata.assign(data + rdataStart, data + rdataStart + rdlength);
    }

    p = rdataStart + rdlength;
    *pos = p;
    return true;
}

}  // namespace

bool parseMessage(const std::uint8_t *data, std::size_t len, Message *out)
{
    if (!data || len < 12) return false;
    *out = Message{};

    const std::uint16_t flags = getU16(data + 2);
    out->response = (flags & 0x8000) != 0;
    const std::uint16_t qdcount = getU16(data + 4);
    const std::uint16_t ancount = getU16(data + 6);
    const std::uint16_t nscount = getU16(data + 8);
    const std::uint16_t arcount = getU16(data + 10);

    std::size_t pos = 12;
    out->questions.reserve(qdcount);
    for (std::uint16_t i = 0; i < qdcount; ++i) {
        Question q;
        if (!readQuestion(data, len, &pos, &q)) return false;
        out->questions.push_back(std::move(q));
    }

    const std::uint32_t totalRecords =
        static_cast<std::uint32_t>(ancount) + nscount + arcount;
    out->records.reserve(totalRecords);
    for (std::uint32_t i = 0; i < totalRecords; ++i) {
        Record r;
        if (!readRecord(data, len, &pos, &r)) return false;
        // TXT/A payloads are already exactly what the caller needs
        // (rawRdata); anything else (AAAA, NSEC, …) is read structurally —
        // name, type, class, ttl — and its RDATA is simply skipped, not
        // treated as a parse failure. `readRecord` already advanced `pos`
        // past it via rdlength regardless of rtype.
        out->records.push_back(std::move(r));
    }

    return true;
}

}  // namespace Mdns
}  // namespace Ppcp
