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

#include "ppcp_qr.h"

#include <algorithm>
#include <cstring>

namespace Ppcp {
namespace {

// ── ISO/IEC 18004 block structure, error correction level M, versions 1..20 ──
// { total data codewords, EC codewords per block, blocks in group 1, data
//   codewords per group-1 block, blocks in group 2, data codewords per group-2
//   block }.  Group 2's blocks each carry exactly one more data codeword than
//   group 1's, which is the invariant the interleaver below relies on.
struct EccSpec {
    int dataCodewords;
    int ecPerBlock;
    int g1Blocks;
    int g1Data;
    int g2Blocks;
    int g2Data;
};

const EccSpec kEccM[21] = {
    {0, 0, 0, 0, 0, 0},          // version 0 does not exist
    {16, 10, 1, 16, 0, 0},       // 1
    {28, 16, 1, 28, 0, 0},       // 2
    {44, 26, 1, 44, 0, 0},       // 3
    {64, 18, 2, 32, 0, 0},       // 4
    {86, 24, 2, 43, 0, 0},       // 5
    {108, 16, 4, 27, 0, 0},      // 6
    {124, 18, 4, 31, 0, 0},      // 7
    {154, 22, 2, 38, 2, 39},     // 8
    {182, 22, 3, 36, 2, 37},     // 9
    {216, 26, 4, 43, 1, 44},     // 10
    {254, 30, 1, 50, 4, 51},     // 11
    {290, 22, 6, 36, 2, 37},     // 12
    {334, 22, 8, 37, 1, 38},     // 13
    {365, 24, 4, 40, 5, 41},     // 14
    {415, 24, 5, 41, 5, 42},     // 15
    {453, 28, 7, 45, 3, 46},     // 16
    {507, 28, 10, 46, 1, 47},    // 17
    {563, 26, 9, 43, 4, 44},     // 18
    {627, 26, 3, 44, 11, 45},    // 19
    {669, 26, 3, 41, 13, 42},    // 20
};
const int kMaxVersion = 20;

// Alignment pattern centre coordinates per version (ISO 18004 Annex E).
const std::int8_t kAlign[21][5] = {
    {-1, -1, -1, -1, -1},   // 0
    {-1, -1, -1, -1, -1},   // 1 — none
    {6, 18, -1, -1, -1},
    {6, 22, -1, -1, -1},
    {6, 26, -1, -1, -1},
    {6, 30, -1, -1, -1},
    {6, 34, -1, -1, -1},
    {6, 22, 38, -1, -1},
    {6, 24, 42, -1, -1},
    {6, 26, 46, -1, -1},
    {6, 28, 50, -1, -1},
    {6, 30, 54, -1, -1},
    {6, 32, 58, -1, -1},
    {6, 34, 62, -1, -1},
    {6, 26, 46, 66, -1},
    {6, 26, 48, 70, -1},
    {6, 26, 50, 74, -1},
    {6, 30, 54, 78, -1},
    {6, 30, 56, 82, -1},
    {6, 30, 58, 86, -1},
    {6, 34, 62, 90, -1},
};

// ── GF(256), primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D) ─────────
struct Gf {
    std::uint8_t exp[512];
    std::uint8_t log[256];
    Gf()
    {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<std::uint8_t>(x);
            log[x] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
        log[0] = 0;   // never read: mul() short-circuits on a zero operand
    }
    std::uint8_t mul(std::uint8_t a, std::uint8_t b) const
    {
        if (a == 0 || b == 0) return 0;
        return exp[log[a] + log[b]];
    }
};
const Gf &gf()
{
    static const Gf g;
    return g;
}

// The RS generator polynomial of degree `n`, coefficients high-order first.
std::vector<std::uint8_t> rsGenerator(int n)
{
    std::vector<std::uint8_t> g{1};
    for (int i = 0; i < n; ++i) {
        // multiply g(x) by (x - alpha^i)
        std::vector<std::uint8_t> next(g.size() + 1, 0);
        for (std::size_t j = 0; j < g.size(); ++j) {
            next[j] ^= g[j];
            next[j + 1] ^= gf().mul(g[j], gf().exp[i]);
        }
        g.swap(next);
    }
    return g;
}

std::vector<std::uint8_t> rsRemainder(const std::uint8_t *data, std::size_t len, int ecLen)
{
    const std::vector<std::uint8_t> gen = rsGenerator(ecLen);
    std::vector<std::uint8_t> rem(static_cast<std::size_t>(ecLen), 0);
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t factor = data[i] ^ rem[0];
        rem.erase(rem.begin());
        rem.push_back(0);
        for (std::size_t j = 0; j < rem.size(); ++j)
            rem[j] ^= gf().mul(gen[j + 1], factor);
    }
    return rem;
}

// ── Bit buffer ──────────────────────────────────────────────────────────────
struct BitBuf {
    std::vector<std::uint8_t> bytes;
    std::size_t bits = 0;
    void push(std::uint32_t value, int n)
    {
        for (int i = n - 1; i >= 0; --i) {
            if (bits % 8 == 0) bytes.push_back(0);
            if ((value >> i) & 1u) bytes[bits / 8] |= static_cast<std::uint8_t>(0x80 >> (bits % 8));
            bits++;
        }
    }
};

}  // namespace

int QrCode::byteCapacity(int version)
{
    if (version < 1 || version > kMaxVersion) return 0;
    const int countBits = version <= 9 ? 8 : 16;
    const int usable = kEccM[version].dataCodewords * 8 - 4 - countBits;
    return usable <= 0 ? 0 : usable / 8;
}

// ── The builder ─────────────────────────────────────────────────────────────
struct QrBuilder {
    int size = 0;
    int version = 0;
    std::vector<std::uint8_t> mods;      // 1 = dark
    std::vector<std::uint8_t> reserved;  // 1 = function pattern, never masked

    void set(int x, int y, bool dark, bool fn)
    {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        mods[static_cast<std::size_t>(y) * size + x] = dark ? 1 : 0;
        if (fn) reserved[static_cast<std::size_t>(y) * size + x] = 1;
    }
    bool get(int x, int y) const
    {
        return mods[static_cast<std::size_t>(y) * size + x] != 0;
    }
    bool isReserved(int x, int y) const
    {
        return reserved[static_cast<std::size_t>(y) * size + x] != 0;
    }

    void drawFinder(int cx, int cy)
    {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                const int d = std::max(std::abs(dx), std::abs(dy));
                set(x, y, d != 2 && d <= 3, true);
            }
        }
    }

    void drawAlignment(int cx, int cy)
    {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1, true);
    }

    void drawFunctionPatterns()
    {
        drawFinder(3, 3);
        drawFinder(size - 4, 3);
        drawFinder(3, size - 4);

        // Timing patterns.
        for (int i = 8; i < size - 8; ++i) {
            set(i, 6, i % 2 == 0, true);
            set(6, i, i % 2 == 0, true);
        }

        // Alignment patterns, skipping the three that would collide with a
        // finder.
        std::vector<int> centres;
        for (int i = 0; i < 5 && kAlign[version][i] >= 0; ++i)
            centres.push_back(kAlign[version][i]);
        for (std::size_t a = 0; a < centres.size(); ++a) {
            for (std::size_t b = 0; b < centres.size(); ++b) {
                const bool topLeft = a == 0 && b == 0;
                const bool topRight = a == 0 && b == centres.size() - 1;
                const bool bottomLeft = a == centres.size() - 1 && b == 0;
                if (topLeft || topRight || bottomLeft) continue;
                drawAlignment(centres[b], centres[a]);
            }
        }

        // Reserve the format-information modules and the always-dark module.
        //
        // ⚠ SKIP INDEX 6.  Row 6 and column 6 are the TIMING patterns, already
        // drawn above; the format information runs (8,0..5), (8,7), (8,8),
        // (7,8) and (5..0,8) and steps over them.  Clearing them here as part
        // of "reserving the format area" blanks two timing modules — the
        // symbol still looks right to the eye and no scanner will read it.
        for (int i = 0; i <= 8; ++i) {
            if (i == 6) continue;
            set(i, 8, false, true);
            set(8, i, false, true);
        }
        reserved[static_cast<std::size_t>(8) * size + 6] = 1;
        reserved[static_cast<std::size_t>(6) * size + 8] = 1;
        for (int i = 0; i < 8; ++i) {
            set(size - 1 - i, 8, false, true);
            set(8, size - 1 - i, false, true);
        }
        set(8, size - 8, true, true);   // the dark module, always set

        if (version >= 7) {
            for (int i = 0; i < 18; ++i) {
                const int a = i / 3, b = i % 3;
                set(size - 11 + b, a, false, true);
                set(a, size - 11 + b, false, true);
            }
        }
    }

    // 15-bit format information: 2 bits ECC level (M = 0b00), 3 bits mask,
    // BCH(15,5) with generator 0x537, XOR-masked with 0x5412.
    static std::uint32_t formatBits(int mask)
    {
        std::uint32_t data = static_cast<std::uint32_t>(0x00 << 3 | mask);   // M
        std::uint32_t rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
        return ((data << 10) | (rem & 0x3FFu)) ^ 0x5412u;
    }

    // 18-bit version information: 6 data bits, BCH(18,6), generator 0x1F25.
    static std::uint32_t versionBits(int version)
    {
        std::uint32_t rem = static_cast<std::uint32_t>(version);
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25u);
        return (static_cast<std::uint32_t>(version) << 12) | (rem & 0xFFFu);
    }

    void drawFormat(int mask)
    {
        const std::uint32_t f = formatBits(mask);
        // Bit 0 is the least significant; the two copies run in opposite
        // directions, which is the part everybody gets wrong once.
        for (int i = 0; i <= 5; ++i) set(8, i, ((f >> i) & 1u) != 0, true);
        set(8, 7, ((f >> 6) & 1u) != 0, true);
        set(8, 8, ((f >> 7) & 1u) != 0, true);
        set(7, 8, ((f >> 8) & 1u) != 0, true);
        for (int i = 9; i < 15; ++i) set(14 - i, 8, ((f >> i) & 1u) != 0, true);

        for (int i = 0; i < 8; ++i) set(size - 1 - i, 8, ((f >> i) & 1u) != 0, true);
        for (int i = 8; i < 15; ++i) set(8, size - 15 + i, ((f >> i) & 1u) != 0, true);
        set(8, size - 8, true, true);
    }

    void drawVersion()
    {
        if (version < 7) return;
        const std::uint32_t v = versionBits(version);
        for (int i = 0; i < 18; ++i) {
            const bool bit = ((v >> i) & 1u) != 0;
            const int a = i / 3, b = i % 3;
            set(size - 11 + b, a, bit, true);
            set(a, size - 11 + b, bit, true);
        }
    }

    // The zigzag: two-module-wide columns from the right, upward then downward,
    // skipping the vertical timing column at x = 6.
    void placeCodewords(const std::vector<std::uint8_t> &data)
    {
        std::size_t bit = 0;
        const std::size_t total = data.size() * 8;
        bool upward = true;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int step = 0; step < size; ++step) {
                const int y = upward ? size - 1 - step : step;
                for (int c = 0; c < 2; ++c) {
                    const int x = right - c;
                    if (isReserved(x, y)) continue;
                    bool dark = false;
                    if (bit < total)
                        dark = (data[bit / 8] >> (7 - (bit % 8)) & 1u) != 0;
                    bit++;
                    mods[static_cast<std::size_t>(y) * size + x] = dark ? 1 : 0;
                }
            }
            upward = !upward;
        }
    }

    static bool maskAt(int mask, int x, int y)
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

    void applyMask(int mask)
    {
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (!isReserved(x, y) && maskAt(mask, x, y))
                    mods[static_cast<std::size_t>(y) * size + x] ^= 1;
    }

    // ISO 18004 §8.8.2 — the four penalty rules.
    int penalty() const
    {
        int score = 0;

        // Rule 1: runs of five or more identical modules in a row or column.
        for (int y = 0; y < size; ++y) {
            int run = 1;
            for (int x = 1; x < size; ++x) {
                if (get(x, y) == get(x - 1, y)) {
                    run++;
                    if (run == 5) score += 3;
                    else if (run > 5) score += 1;
                } else {
                    run = 1;
                }
            }
        }
        for (int x = 0; x < size; ++x) {
            int run = 1;
            for (int y = 1; y < size; ++y) {
                if (get(x, y) == get(x, y - 1)) {
                    run++;
                    if (run == 5) score += 3;
                    else if (run > 5) score += 1;
                } else {
                    run = 1;
                }
            }
        }

        // Rule 2: 2x2 blocks of one colour.
        for (int y = 0; y + 1 < size; ++y)
            for (int x = 0; x + 1 < size; ++x)
                if (get(x, y) == get(x + 1, y) && get(x, y) == get(x, y + 1) &&
                    get(x, y) == get(x + 1, y + 1))
                    score += 3;

        // Rule 3: the 1:1:3:1:1 finder-like pattern with four light modules on
        // either side, in either orientation.
        static const bool pat[7] = {true, false, true, true, true, false, true};
        auto matches = [&](int x, int y, int dx, int dy) {
            for (int i = 0; i < 7; ++i)
                if (get(x + dx * i, y + dy * i) != pat[i]) return false;
            auto light = [&](int j) {
                const int px = x + dx * j, py = y + dy * j;
                if (px < 0 || py < 0 || px >= size || py >= size) return true;  // quiet zone
                return !get(px, py);
            };
            bool before = true, after = true;
            for (int j = 1; j <= 4; ++j) {
                if (!light(-j)) before = false;
                if (!light(6 + j)) after = false;
            }
            return before || after;
        };
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                if (x + 6 < size && matches(x, y, 1, 0)) score += 40;
                if (y + 6 < size && matches(x, y, 0, 1)) score += 40;
            }

        // Rule 4: deviation of the dark-module proportion from 50%.
        int dark = 0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (get(x, y)) dark++;
        const int total = size * size;
        const int percent = dark * 100 / total;
        const int k = (std::abs(percent - 50) + 4) / 5;
        score += k * 10;

        return score;
    }
};

QrCode QrCode::encodeBytes(const std::uint8_t *data, std::size_t len)
{
    QrCode out;

    int version = 0;
    for (int v = 1; v <= kMaxVersion; ++v) {
        if (static_cast<int>(len) <= byteCapacity(v)) { version = v; break; }
    }
    // A payload that does not fit is a FAILED CONSTRUCTION and never a
    // truncated symbol: a truncated code scans perfectly and pairs with
    // nothing, which is the worst failure available here.
    if (version == 0) return out;

    const EccSpec &spec = kEccM[version];
    const int countBits = version <= 9 ? 8 : 16;

    // ── The bit stream (ISO 18004 §8.4) ────────────────────────────────────
    BitBuf bb;
    bb.push(0b0100, 4);                                    // byte mode
    bb.push(static_cast<std::uint32_t>(len), countBits);
    for (std::size_t i = 0; i < len; ++i) bb.push(data[i], 8);

    const std::size_t capacityBits = static_cast<std::size_t>(spec.dataCodewords) * 8;
    // Terminator: up to four zero bits, fewer if the capacity is nearly full.
    const std::size_t term = std::min<std::size_t>(4, capacityBits - bb.bits);
    bb.push(0, static_cast<int>(term));
    // Pad to a byte boundary, then alternate 0xEC / 0x11 to the end.
    if (bb.bits % 8) bb.push(0, static_cast<int>(8 - bb.bits % 8));
    for (std::uint8_t pad = 0xEC; bb.bits < capacityBits; pad = pad == 0xEC ? 0x11 : 0xEC)
        bb.push(pad, 8);

    // ── Blocks, error correction, and the interleave (§8.6) ────────────────
    std::vector<std::vector<std::uint8_t>> dataBlocks, ecBlocks;
    std::size_t off = 0;
    for (int b = 0; b < spec.g1Blocks + spec.g2Blocks; ++b) {
        const int n = b < spec.g1Blocks ? spec.g1Data : spec.g2Data;
        std::vector<std::uint8_t> blk(bb.bytes.begin() + off, bb.bytes.begin() + off + n);
        off += static_cast<std::size_t>(n);
        ecBlocks.push_back(rsRemainder(blk.data(), blk.size(), spec.ecPerBlock));
        dataBlocks.push_back(std::move(blk));
    }

    std::vector<std::uint8_t> stream;
    const int maxData = spec.g2Blocks ? spec.g2Data : spec.g1Data;
    for (int i = 0; i < maxData; ++i)
        for (auto &blk : dataBlocks)
            if (i < static_cast<int>(blk.size())) stream.push_back(blk[i]);
    for (int i = 0; i < spec.ecPerBlock; ++i)
        for (auto &blk : ecBlocks) stream.push_back(blk[static_cast<std::size_t>(i)]);

    // ── The matrix ─────────────────────────────────────────────────────────
    const int size = version * 4 + 17;

    int bestMask = 0, bestScore = -1;
    std::vector<std::uint8_t> bestModules;
    for (int mask = 0; mask < 8; ++mask) {
        QrBuilder b;
        b.size = size;
        b.version = version;
        b.mods.assign(static_cast<std::size_t>(size) * size, 0);
        b.reserved.assign(static_cast<std::size_t>(size) * size, 0);
        b.drawFunctionPatterns();
        b.drawVersion();
        b.placeCodewords(stream);
        b.applyMask(mask);
        b.drawFormat(mask);
        const int s = b.penalty();
        if (bestScore < 0 || s < bestScore) {
            bestScore = s;
            bestMask = mask;
            bestModules = b.mods;
        }
    }

    out.m_size = size;
    out.m_version = version;
    out.m_mask = bestMask;
    out.m_modules = std::move(bestModules);
    return out;
}

}  // namespace Ppcp
