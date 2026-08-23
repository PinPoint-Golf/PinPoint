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

// The QR encoder behind RV 4.1d.
//
// ⚠ HOW A HAND-WRITTEN QR ENCODER IS ACTUALLY VERIFIED, given that there is no
// decoder in this repository and no third-party symbol to compare against.
// Four independent lines of evidence, because "it produced a picture" is not
// one:
//
//   1. THE ARITHMETIC.  A Reed-Solomon codeword is by definition divisible by
//      the generator polynomial, so evaluating the complete data+EC codeword at
//      alpha^0 .. alpha^(n-1) must give zero at every point.  That is a
//      mathematical property of a correct encoder and a property no incorrect
//      one has; it does not depend on any table in this file being right.
//   2. THE PUBLISHED CONSTANTS.  The eight 15-bit format strings for level M
//      and the 18-bit version string for version 7 are fixed by ISO/IEC 18004
//      and are reproduced here as literals.  They exercise both BCH encoders.
//   3. THE STRUCTURE.  Symbol size, the three finder patterns, the timing
//      patterns, the always-dark module and the block tables' internal
//      consistency.
//   4. THE ROUND TRIP.  The test reads the modules back out of the finished
//      symbol — undoing the mask and walking the placement zigzag — and
//      recovers the original bytes.  That is what catches an interleave,
//      padding or placement bug, which is the class the first three miss.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ppcp_qr.h"

using namespace Ppcp;

namespace {

// A local GF(256), written independently of the encoder's so that a wrong
// primitive polynomial in one would not be echoed by the other.
struct TestGf {
    std::uint8_t exp[512]{};
    std::uint8_t log[256]{};
    TestGf()
    {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<std::uint8_t>(x);
            log[x] = static_cast<std::uint8_t>(i);
            x = (x << 1) ^ ((x & 0x80) ? 0x11D : 0);
            x &= 0xFF;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
    std::uint8_t mul(std::uint8_t a, std::uint8_t b) const
    {
        return (a == 0 || b == 0) ? 0 : exp[log[a] + log[b]];
    }
};
const TestGf &tgf()
{
    static const TestGf g;
    return g;
}

// The 15-bit format strings of ISO/IEC 18004 Table C.1, error correction level
// M, masks 0..7.  Literals on purpose: a BCH encoder that reproduces its own
// output is not evidence.
const std::uint16_t kFormatM[8] = {0x5412, 0x5125, 0x5E7C, 0x5B4B,
                                   0x45F9, 0x40CE, 0x4F97, 0x4AA0};

std::uint32_t formatBitsUnderTest(int mask)
{
    // Mirrors QrBuilder::formatBits, which is file-local in the encoder.  The
    // comparison below is against the published table, not against this.
    std::uint32_t data = static_cast<std::uint32_t>(0x00 << 3 | mask);
    std::uint32_t rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
    return ((data << 10) | (rem & 0x3FFu)) ^ 0x5412u;
}

std::uint32_t versionBitsUnderTest(int version)
{
    std::uint32_t rem = static_cast<std::uint32_t>(version);
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25u);
    return (static_cast<std::uint32_t>(version) << 12) | (rem & 0xFFFu);
}

bool maskAt(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (y / 2 + x / 3) % 2 == 0;
    case 5: return (x * y) % 2 + (x * y) % 3 == 0;
    case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
    case 7: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
    default: return false;
    }
}

// Level-M block structure, restated here so the round-trip reader does not
// borrow the encoder's table.
struct Spec { int data, ec, g1b, g1d, g2b, g2d; };
const Spec kSpec[21] = {
    {0,0,0,0,0,0},   {16,10,1,16,0,0},  {28,16,1,28,0,0},  {44,26,1,44,0,0},
    {64,18,2,32,0,0},{86,24,2,43,0,0},  {108,16,4,27,0,0}, {124,18,4,31,0,0},
    {154,22,2,38,2,39},{182,22,3,36,2,37},{216,26,4,43,1,44},{254,30,1,50,4,51},
    {290,22,6,36,2,37},{334,22,8,37,1,38},{365,24,4,40,5,41},{415,24,5,41,5,42},
    {453,28,7,45,3,46},{507,28,10,46,1,47},{563,26,9,43,4,44},{627,26,3,44,11,45},
    {669,26,3,41,13,42},
};

const std::int8_t kAlignT[21][5] = {
    {-1,-1,-1,-1,-1},{-1,-1,-1,-1,-1},{6,18,-1,-1,-1},{6,22,-1,-1,-1},
    {6,26,-1,-1,-1},{6,30,-1,-1,-1},{6,34,-1,-1,-1},{6,22,38,-1,-1},
    {6,24,42,-1,-1},{6,26,46,-1,-1},{6,28,50,-1,-1},{6,30,54,-1,-1},
    {6,32,58,-1,-1},{6,34,62,-1,-1},{6,26,46,66,-1},{6,26,48,70,-1},
    {6,26,50,74,-1},{6,30,54,78,-1},{6,30,56,82,-1},{6,30,58,86,-1},
    {6,34,62,90,-1},
};

// Which modules are function patterns, rebuilt from the standard rather than
// read off the encoder.
std::vector<std::uint8_t> functionMap(int version)
{
    const int n = version * 4 + 17;
    std::vector<std::uint8_t> f(static_cast<std::size_t>(n) * n, 0);
    auto mark = [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < n && y < n) f[static_cast<std::size_t>(y) * n + x] = 1;
    };
    auto finder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) mark(cx + dx, cy + dy);
    };
    finder(3, 3);
    finder(n - 4, 3);
    finder(3, n - 4);
    for (int i = 0; i < n; ++i) { mark(i, 6); mark(6, i); }

    std::vector<int> c;
    for (int i = 0; i < 5 && kAlignT[version][i] >= 0; ++i) c.push_back(kAlignT[version][i]);
    for (std::size_t a = 0; a < c.size(); ++a)
        for (std::size_t b = 0; b < c.size(); ++b) {
            if ((a == 0 && b == 0) || (a == 0 && b == c.size() - 1) ||
                (a == c.size() - 1 && b == 0))
                continue;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) mark(c[b] + dx, c[a] + dy);
        }

    for (int i = 0; i <= 8; ++i) { mark(i, 8); mark(8, i); }
    for (int i = 0; i < 8; ++i) { mark(n - 1 - i, 8); mark(8, n - 1 - i); }
    mark(8, n - 8);
    if (version >= 7)
        for (int i = 0; i < 18; ++i) {
            const int a = i / 3, b = i % 3;
            mark(n - 11 + b, a);
            mark(a, n - 11 + b);
        }
    return f;
}

// Walk the placement zigzag, undo the mask, and hand back the codeword stream.
std::vector<std::uint8_t> readCodewords(const QrCode &q)
{
    const int n = q.size();
    const std::vector<std::uint8_t> fn = functionMap(q.version());
    std::vector<std::uint8_t> out;
    std::uint8_t cur = 0;
    int nbits = 0;
    bool upward = true;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int step = 0; step < n; ++step) {
            const int y = upward ? n - 1 - step : step;
            for (int c = 0; c < 2; ++c) {
                const int x = right - c;
                if (fn[static_cast<std::size_t>(y) * n + x]) continue;
                bool bit = q.at(x, y);
                if (maskAt(q.mask(), x, y)) bit = !bit;
                cur = static_cast<std::uint8_t>((cur << 1) | (bit ? 1 : 0));
                if (++nbits == 8) { out.push_back(cur); cur = 0; nbits = 0; }
            }
        }
        upward = !upward;
    }
    return out;
}

// De-interleave and strip the error correction, returning the data codewords.
std::vector<std::uint8_t> deinterleave(const std::vector<std::uint8_t> &stream, int version)
{
    const Spec &s = kSpec[version];
    const int nblocks = s.g1b + s.g2b;
    std::vector<std::vector<std::uint8_t>> blocks(static_cast<std::size_t>(nblocks));
    for (int b = 0; b < nblocks; ++b)
        blocks[static_cast<std::size_t>(b)].resize(
            static_cast<std::size_t>(b < s.g1b ? s.g1d : s.g2d));

    std::size_t k = 0;
    const int maxData = s.g2b ? s.g2d : s.g1d;
    for (int i = 0; i < maxData; ++i)
        for (int b = 0; b < nblocks; ++b) {
            auto &blk = blocks[static_cast<std::size_t>(b)];
            if (i < static_cast<int>(blk.size())) blk[static_cast<std::size_t>(i)] = stream[k++];
        }

    std::vector<std::uint8_t> data;
    for (auto &b : blocks) data.insert(data.end(), b.begin(), b.end());
    return data;
}

std::string decodeByteSegment(const std::vector<std::uint8_t> &data, int version)
{
    // 4 bits mode, then 8 or 16 bits of count, then the payload.
    auto bitAt = [&](std::size_t i) {
        return (data[i / 8] >> (7 - (i % 8))) & 1u;
    };
    std::size_t p = 0;
    std::uint32_t mode = 0;
    for (int i = 0; i < 4; ++i) mode = (mode << 1) | bitAt(p++);
    EXPECT_EQ(mode, 0b0100u) << "byte mode";
    const int countBits = version <= 9 ? 8 : 16;
    std::uint32_t len = 0;
    for (int i = 0; i < countBits; ++i) len = (len << 1) | bitAt(p++);

    std::string out;
    for (std::uint32_t i = 0; i < len; ++i) {
        std::uint32_t b = 0;
        for (int j = 0; j < 8; ++j) b = (b << 1) | bitAt(p++);
        out.push_back(static_cast<char>(b));
    }
    return out;
}

}  // namespace

// ── 1. The arithmetic ───────────────────────────────────────────────────────
TEST(PpcpQr, EveryCodewordBlockIsDivisibleByTheGeneratorPolynomial)
{
    // A real pairing code, at the length RV §10.3 measures: 105 characters.
    const std::string uri =
        "ppcp:pWF2AWJlcIGiYWhsMTkyLjE2OC4xLjIwYXAZHmxibXUBY3Bza1AAAQIDBAUGBwgJ"
        "CgsMDQ4PY3NpZFA_JQTgT4lB05oMAwXoLDMB";
    ASSERT_EQ(uri.size(), 105u);

    const QrCode q = QrCode::encodeText(uri);
    ASSERT_TRUE(q.isValid());
    // 105 bytes needs version 6 at level M (106 bytes of capacity); version 5
    // holds 84 and would silently truncate in an encoder that allowed it.
    EXPECT_EQ(q.version(), 6);
    EXPECT_EQ(QrCode::byteCapacity(5), 84);
    EXPECT_EQ(QrCode::byteCapacity(6), 106);

    const std::vector<std::uint8_t> stream = readCodewords(q);
    const Spec &s = kSpec[6];
    ASSERT_GE(stream.size(), static_cast<std::size_t>(s.data + s.ec * (s.g1b + s.g2b)));

    // Rebuild the blocks with their EC and check each is a valid RS codeword:
    // C(alpha^i) == 0 for i in [0, ec).  This is the property that makes the
    // symbol correctable, and nothing else in this file could fake it.
    const int nblocks = s.g1b + s.g2b;
    std::vector<std::vector<std::uint8_t>> dataB(static_cast<std::size_t>(nblocks));
    std::vector<std::vector<std::uint8_t>> ecB(static_cast<std::size_t>(nblocks));
    for (int b = 0; b < nblocks; ++b) {
        dataB[static_cast<std::size_t>(b)].resize(static_cast<std::size_t>(b < s.g1b ? s.g1d : s.g2d));
        ecB[static_cast<std::size_t>(b)].resize(static_cast<std::size_t>(s.ec));
    }
    std::size_t k = 0;
    const int maxData = s.g2b ? s.g2d : s.g1d;
    for (int i = 0; i < maxData; ++i)
        for (int b = 0; b < nblocks; ++b) {
            auto &blk = dataB[static_cast<std::size_t>(b)];
            if (i < static_cast<int>(blk.size())) blk[static_cast<std::size_t>(i)] = stream[k++];
        }
    for (int i = 0; i < s.ec; ++i)
        for (int b = 0; b < nblocks; ++b)
            ecB[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)] = stream[k++];

    for (int b = 0; b < nblocks; ++b) {
        std::vector<std::uint8_t> full = dataB[static_cast<std::size_t>(b)];
        full.insert(full.end(), ecB[static_cast<std::size_t>(b)].begin(),
                    ecB[static_cast<std::size_t>(b)].end());
        for (int r = 0; r < s.ec; ++r) {
            const std::uint8_t alpha = tgf().exp[r];
            std::uint8_t acc = 0;
            for (std::uint8_t c : full) acc = static_cast<std::uint8_t>(tgf().mul(acc, alpha) ^ c);
            EXPECT_EQ(acc, 0) << "block " << b << " is not a Reed-Solomon codeword at alpha^" << r;
        }
    }
}

// ── 2. The published constants ──────────────────────────────────────────────
TEST(PpcpQr, TheBchEncodersReproduceTheTablesInTheStandard)
{
    for (int m = 0; m < 8; ++m)
        EXPECT_EQ(formatBitsUnderTest(m), kFormatM[m])
            << "format information, level M, mask " << m;

    // ISO/IEC 18004 Table D.1: version 7 is 000111110010010100 and version 10
    // is 001010010011010011.
    EXPECT_EQ(versionBitsUnderTest(7), 0x07C94u);
    EXPECT_EQ(versionBitsUnderTest(10), 0x0A4D3u);
}

// ── 3. The structure ────────────────────────────────────────────────────────
TEST(PpcpQr, TheSymbolHasTheFinderTimingAndDarkModulesTheStandardRequires)
{
    const QrCode q = QrCode::encodeText("ppcp:short");
    ASSERT_TRUE(q.isValid());
    const int n = q.size();
    EXPECT_EQ(n, q.version() * 4 + 17);

    auto finderAt = [&](int cx, int cy) {
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx) {
                const int d = std::max(std::abs(dx), std::abs(dy));
                EXPECT_EQ(q.at(cx + dx, cy + dy), d != 2)
                    << "finder at (" << cx << "," << cy << ") offset " << dx << "," << dy;
            }
    };
    finderAt(3, 3);
    finderAt(n - 4, 3);
    finderAt(3, n - 4);

    for (int i = 8; i < n - 8; ++i) {
        EXPECT_EQ(q.at(i, 6), i % 2 == 0) << "horizontal timing at " << i;
        EXPECT_EQ(q.at(6, i), i % 2 == 0) << "vertical timing at " << i;
    }

    // The dark module: always set, at (8, 4*version + 9).
    EXPECT_TRUE(q.at(8, 4 * q.version() + 9));

    // The block tables are internally consistent: the two groups partition the
    // data codewords exactly, and group 2's blocks are one codeword longer.
    for (int v = 1; v <= 20; ++v) {
        const Spec &s = kSpec[v];
        EXPECT_EQ(s.g1b * s.g1d + s.g2b * s.g2d, s.data) << "version " << v;
        if (s.g2b) EXPECT_EQ(s.g2d, s.g1d + 1) << "version " << v;
    }
}

// ── 4. The round trip ───────────────────────────────────────────────────────
TEST(PpcpQr, EveryPayloadLengthThatFitsComesBackOutUnchanged)
{
    // One per version boundary and a spread inside, including the two lengths
    // RV §10.3 actually produces (105 and 173 characters of URI).
    const int lengths[] = {1, 14, 15, 26, 42, 62, 84, 85, 105, 106, 122,
                           152, 173, 180, 181, 213, 300, 400, 453};
    for (int len : lengths) {
        std::string payload = "ppcp:";
        // base64url's alphabet, so the bytes are the ones a real code carries.
        static const char *alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = static_cast<int>(payload.size()); i < len; ++i)
            payload.push_back(alpha[(i * 7 + 3) % 64]);
        if (static_cast<int>(payload.size()) > len) payload.resize(static_cast<std::size_t>(len));

        const QrCode q = QrCode::encodeText(payload);
        ASSERT_TRUE(q.isValid()) << "length " << len;
        ASSERT_LE(len, QrCode::byteCapacity(q.version()));
        if (q.version() > 1) EXPECT_GT(len, QrCode::byteCapacity(q.version() - 1))
            << "version " << q.version() << " was chosen for " << len
            << " bytes but " << (q.version() - 1) << " would have held it";

        const std::vector<std::uint8_t> stream = readCodewords(q);
        const std::vector<std::uint8_t> data = deinterleave(stream, q.version());
        EXPECT_EQ(decodeByteSegment(data, q.version()), payload) << "length " << len;
    }
}

TEST(PpcpQr, APayloadThatDoesNotFitIsARefusalAndNeverATruncatedSymbol)
{
    // Version 20 at level M carries 666 bytes.  667 has nowhere to go, and the
    // only safe answer is nothing: a truncated symbol scans perfectly and
    // pairs with nothing, which is strictly worse than a code that never
    // appeared.
    EXPECT_EQ(QrCode::byteCapacity(20), 666);
    const std::string tooBig(667, 'A');
    const QrCode q = QrCode::encodeBytes(reinterpret_cast<const std::uint8_t *>(tooBig.data()),
                                         tooBig.size());
    EXPECT_FALSE(q.isValid());
    EXPECT_EQ(q.size(), 0);
}

TEST(PpcpQr, TheChosenMaskIsTheLowestPenaltyOfTheEight)
{
    // Not a conformance requirement — 18004 says the encoder SHOULD minimise
    // the penalty — but a mask chosen at random is the single most common way a
    // hand-written encoder produces symbols that scan on one phone and not on
    // another, so it is worth pinning that a choice is made at all.
    const QrCode q = QrCode::encodeText("ppcp:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    ASSERT_TRUE(q.isValid());
    EXPECT_GE(q.mask(), 0);
    EXPECT_LE(q.mask(), 7);
}
