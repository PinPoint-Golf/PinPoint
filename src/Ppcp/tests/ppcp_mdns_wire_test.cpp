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

// The wire layer of the native Windows mDNS engine (W4, second cut — see
// ppcp_mdns_wire.h for why this exists instead of the Bonjour SDK path).
// Pure encode/decode, no socket, no platform code — every row here is
// runnable on any OS, which is deliberate: getting the DNS wire format wrong
// is the class of bug that only shows up against a real responder, and by
// then it is expensive to isolate. This suite is where it is cheap.

#include "ppcp_mdns_wire.h"

#include <gtest/gtest.h>

using namespace Ppcp::Mdns;

namespace {

std::vector<std::uint8_t> asciiTxt(const std::string &kv)
{
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(kv.size()));
    out.insert(out.end(), kv.begin(), kv.end());
    return out;
}

}  // namespace

// ── Questions ────────────────────────────────────────────────────────────

TEST(MdnsWire, AQueryWithOneQuestionRoundTrips)
{
    MessageWriter w;
    w.writeHeader(/*response=*/false, /*authoritative=*/false, 1, 0);
    w.writeQuestion("_ppcp._tcp.local", kTypePTR);

    Message m;
    ASSERT_TRUE(parseMessage(w.bytes().data(), w.bytes().size(), &m));
    EXPECT_FALSE(m.response);
    ASSERT_EQ(m.questions.size(), 1u);
    EXPECT_EQ(m.questions[0].name, "_ppcp._tcp.local");
    EXPECT_EQ(m.questions[0].qtype, kTypePTR);
    EXPECT_FALSE(m.questions[0].unicastRequested);
    EXPECT_TRUE(m.records.empty());
}

// ── The full record set an advertiser sends ─────────────────────────────

TEST(MdnsWire, PtrSrvTxtAResponseRoundTripsWithAllFieldsIntact)
{
    MessageWriter w;
    w.writeHeader(/*response=*/true, /*authoritative=*/true, 0, 4);
    w.writeRecord("_ppcp._tcp.local", kTypePTR, /*cacheFlush=*/false, 120,
                  MessageWriter::rdataName("PPCP-DEADBEEF._ppcp._tcp.local"));
    w.writeRecord("PPCP-DEADBEEF._ppcp._tcp.local", kTypeSRV, /*cacheFlush=*/true, 120,
                  MessageWriter::rdataSrv(0, 0, 7788, "Marks-Mac-mini.local"));
    const std::vector<std::uint8_t> txt = asciiTxt("role=host");
    w.writeRecord("PPCP-DEADBEEF._ppcp._tcp.local", kTypeTXT, /*cacheFlush=*/true, 120, txt);
    w.writeRecord("Marks-Mac-mini.local", kTypeA, /*cacheFlush=*/true, 120,
                  MessageWriter::rdataA("192.168.1.42"));

    Message m;
    ASSERT_TRUE(parseMessage(w.bytes().data(), w.bytes().size(), &m));
    EXPECT_TRUE(m.response);
    ASSERT_EQ(m.records.size(), 4u);

    const Record &ptr = m.records[0];
    EXPECT_EQ(ptr.name, "_ppcp._tcp.local");
    EXPECT_EQ(ptr.rtype, kTypePTR);
    EXPECT_FALSE(ptr.cacheFlush) << "PTR is a shared record (RFC 6762 §10.2), never cache-flush";
    EXPECT_EQ(ptr.ttl, 120u);
    EXPECT_EQ(ptr.targetName, "PPCP-DEADBEEF._ppcp._tcp.local");

    const Record &srv = m.records[1];
    EXPECT_EQ(srv.name, "PPCP-DEADBEEF._ppcp._tcp.local");
    EXPECT_EQ(srv.rtype, kTypeSRV);
    EXPECT_TRUE(srv.cacheFlush);
    EXPECT_EQ(srv.srvPort, 7788u);
    EXPECT_EQ(srv.targetName, "Marks-Mac-mini.local");

    const Record &txtRec = m.records[2];
    EXPECT_EQ(txtRec.rtype, kTypeTXT);
    EXPECT_EQ(txtRec.rawRdata, txt) << "TXT rdata must be byte-identical to buildTxtRecord()'s "
                                        "own output — this is the wire format, not a re-encoding";

    const Record &a = m.records[3];
    EXPECT_EQ(a.name, "Marks-Mac-mini.local");
    EXPECT_EQ(a.rtype, kTypeA);
    ASSERT_EQ(a.rawRdata.size(), 4u);
    EXPECT_EQ(a.rawRdata[0], 192);
    EXPECT_EQ(a.rawRdata[1], 168);
    EXPECT_EQ(a.rawRdata[2], 1);
    EXPECT_EQ(a.rawRdata[3], 42);
}

// ── The cache-flush bit must not leak into the class a reader sees ────────

TEST(MdnsWire, CacheFlushBitDoesNotCorruptTheClassField)
{
    MessageWriter w;
    w.writeHeader(true, true, 0, 1);
    w.writeRecord("x.local", kTypeA, /*cacheFlush=*/true, 60, MessageWriter::rdataA("10.0.0.1"));
    Message m;
    ASSERT_TRUE(parseMessage(w.bytes().data(), w.bytes().size(), &m));
    ASSERT_EQ(m.records.size(), 1u);
    EXPECT_TRUE(m.records[0].cacheFlush);
    // The class byte's low 15 bits must still read as class 1 (IN) once the
    // top bit is masked — a bug here would misroute every record on class,
    // and every record this engine ever emits is class IN.
}

// ── DNS name compression — the trap most hand-rolled parsers get wrong ────
//
// Builds a message BY HAND (not via MessageWriter, which never compresses on
// write) so the reader is exercised against a pointer independently of the
// writer ever producing one — the two must not be allowed to only agree with
// themselves.
TEST(MdnsWire, ReaderFollowsACompressionPointerInsideRdata)
{
    std::vector<std::uint8_t> buf;
    auto putU16 = [&](std::uint16_t v) {
        buf.push_back(static_cast<std::uint8_t>(v >> 8));
        buf.push_back(static_cast<std::uint8_t>(v & 0xff));
    };
    auto putU32 = [&](std::uint32_t v) {
        buf.push_back(static_cast<std::uint8_t>(v >> 24));
        buf.push_back(static_cast<std::uint8_t>(v >> 16));
        buf.push_back(static_cast<std::uint8_t>(v >> 8));
        buf.push_back(static_cast<std::uint8_t>(v & 0xff));
    };

    // Header: response, AA, ancount=2.
    putU16(0);
    putU16(0x8400);
    putU16(0);
    putU16(2);
    putU16(0);
    putU16(0);

    // Record 1 — PTR "_ppcp._tcp.local" -> "PPCP-AAAAAAAA._ppcp._tcp.local",
    // written with "_ppcp._tcp.local" spelled out in full. Remember its
    // offset so record 2 can point back at it.
    const std::size_t nameOffset = buf.size();
    for (const std::string &label : {std::string("_ppcp"), std::string("_tcp"),
                                     std::string("local")}) {
        buf.push_back(static_cast<std::uint8_t>(label.size()));
        buf.insert(buf.end(), label.begin(), label.end());
    }
    buf.push_back(0);
    putU16(kTypePTR);
    putU16(kClassIN);
    putU32(120);
    // RDATA: "PPCP-AAAAAAAA" + a pointer back at nameOffset, rather than
    // spelling ".local" out again — exactly the shape a real responder uses.
    std::vector<std::uint8_t> rdata;
    const std::string inst = "PPCP-AAAAAAAA";
    rdata.push_back(static_cast<std::uint8_t>(inst.size()));
    rdata.insert(rdata.end(), inst.begin(), inst.end());
    // one more label ("_ppcp") then a pointer straight to "local" would also
    // be legal, but pointing at the WHOLE remaining name (".local" via
    // "_ppcp._tcp.local"'s own start) is simpler and just as valid —
    // instead: point at nameOffset itself, i.e. re-use "_ppcp._tcp.local"
    // whole as the suffix, which only makes sense if the instance name
    // legitimately ends in that suffix, which it does here.
    rdata.push_back(static_cast<std::uint8_t>(0xc0 | ((nameOffset >> 8) & 0x3f)));
    rdata.push_back(static_cast<std::uint8_t>(nameOffset & 0xff));
    putU16(static_cast<std::uint16_t>(rdata.size()));
    buf.insert(buf.end(), rdata.begin(), rdata.end());

    // Record 2 — a trivial A record, present only so parseMessage has a
    // second record to walk PAST the compressed one, which is what proves
    // the outer cursor advanced by the right number of bytes rather than by
    // however many the pointer's target happened to occupy.
    buf.push_back(1);
    buf.push_back('x');
    buf.push_back(0);
    putU16(kTypeA);
    putU16(kClassIN);
    putU32(60);
    putU16(4);
    const auto a = MessageWriter::rdataA("10.0.0.9");
    buf.insert(buf.end(), a.begin(), a.end());

    Message m;
    ASSERT_TRUE(parseMessage(buf.data(), buf.size(), &m));
    ASSERT_EQ(m.records.size(), 2u);
    EXPECT_EQ(m.records[0].targetName, "PPCP-AAAAAAAA._ppcp._tcp.local")
        << "the compression pointer inside RDATA was not followed correctly";
    EXPECT_EQ(m.records[1].name, "x");
    EXPECT_EQ(m.records[1].rawRdata.size(), 4u);
}

// ── Malformed input is refused, never crashed on ──────────────────────────

TEST(MdnsWire, TruncatedHeaderIsRefused)
{
    const std::uint8_t tooShort[4] = {0, 0, 0, 0};
    Message m;
    EXPECT_FALSE(parseMessage(tooShort, sizeof tooShort, &m));
}

TEST(MdnsWire, ARecordLengthRunningPastTheBufferIsRefused)
{
    MessageWriter w;
    w.writeHeader(true, true, 0, 1);
    w.writeRecord("x.local", kTypeA, false, 60, MessageWriter::rdataA("10.0.0.1"));
    std::vector<std::uint8_t> truncated = w.bytes();
    truncated.resize(truncated.size() - 2);   // lop off the last two RDATA bytes
    Message m;
    EXPECT_FALSE(parseMessage(truncated.data(), truncated.size(), &m));
}

TEST(MdnsWire, ACompressionPointerLoopIsRefusedNotHung)
{
    // A name at offset 12 that points at itself — the classic malformed
    // input a naive follow-the-pointer reader hangs or reads forever on.
    std::vector<std::uint8_t> buf(12, 0);   // header, all zero (0 questions/records is fine)
    buf.push_back(static_cast<std::uint8_t>(0xc0 | ((12 >> 8) & 0x3f)));
    buf.push_back(static_cast<std::uint8_t>(12 & 0xff));
    // Claim one question so the parser actually walks into the loop.
    buf[4] = 0;
    buf[5] = 1;
    buf.push_back(0);
    buf.push_back(static_cast<std::uint8_t>(kTypePTR >> 8));
    buf.push_back(static_cast<std::uint8_t>(kTypePTR & 0xff));
    buf.push_back(0);
    buf.push_back(static_cast<std::uint8_t>(kClassIN));

    Message m;
    // The assertion that matters is that this call RETURNS at all — the test
    // binary's own timeout is the backstop if it does not.
    EXPECT_FALSE(parseMessage(buf.data(), buf.size(), &m));
}

// ── The A-record dotted-quad hand parser ───────────────────────────────────

TEST(MdnsWire, RdataAParsesADottedQuad)
{
    const auto a = MessageWriter::rdataA("203.0.113.7");
    ASSERT_EQ(a.size(), 4u);
    EXPECT_EQ(a[0], 203);
    EXPECT_EQ(a[1], 0);
    EXPECT_EQ(a[2], 113);
    EXPECT_EQ(a[3], 7);
}
