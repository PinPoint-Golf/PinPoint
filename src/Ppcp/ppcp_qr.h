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

// A QR encoder, byte mode, error correction level M — RV 4.1d.
//
// ⚠ WHY THIS EXISTS RATHER THAN A DEPENDENCY.  The repository has no QR code
// anywhere: no qrencode, no ZXing, no QZXing, nothing vendored in
// `third_party/`.  RV 4.1d is a SHOULD for level M or higher and the primary
// pairing path does not work without a rendered code, so the choice was a new
// third-party dependency in an application that already fetches ten of them, or
// ~450 lines of well-specified arithmetic.  ISO/IEC 18004's encoding is fixed
// and finished; a library would buy nothing but a build-system entry and a
// licence to audit.
//
// ⚠ ENCODE ONLY, AND DELIBERATELY.  This host DISPLAYS codes; the peer that
// SCANS them is PinPointCapture, on a phone, using the platform camera.  A
// decoder here would be code with no caller — and a decoder is where the
// interesting failure modes live, so having none is worth stating.
//
// Level M and not L: a code on a screen is photographed at an angle, in a
// driving bay, by a phone that is also being held.  M recovers 15% and costs
// one version step at these payload sizes.
//
// Byte mode and not alphanumeric: the payload is `ppcp:` followed by unpadded
// base64url (RV 4.1a), whose alphabet includes lowercase letters, `-` and `_`.
// QR's alphanumeric mode has 45 characters and none of those three, so byte
// mode is not a simplification — it is the only mode that can carry the URI.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Ppcp {

// A square matrix of modules.  `at(x, y)` is true where the module is dark.
// No quiet zone is included: ISO 18004 requires four modules of it and that is
// the RENDERER's decision, because it depends on what the code is drawn onto.
// The QML side adds it; the tests assert on the symbol itself.
class QrCode {
public:
    int  size() const { return m_size; }
    int  version() const { return m_version; }
    bool at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= m_size || y >= m_size) return false;
        return m_modules[static_cast<std::size_t>(y) * m_size + x] != 0;
    }
    bool isValid() const { return m_size > 0; }

    // The chosen mask (0..7), for diagnostics.  RV asks nothing about it; it is
    // here because a symbol that scans badly is nearly always a mask bug and
    // being able to name the one that was chosen is the difference between a
    // ten-minute diagnosis and a day of it.
    int mask() const { return m_mask; }

    // Encodes `data` at ECC level M in the smallest version that fits, up to
    // version 20 (669 data codewords, 213..666 payload bytes depending on
    // version).  Returns an invalid QrCode when the payload does not fit — a
    // pairing code that overflows is a construction failure, never a truncated
    // symbol, because a truncated symbol scans perfectly and pairs with nothing.
    //
    // RV 4.5a keeps a payload under 400 bytes, which is version 16-M with room
    // to spare, so 20 is generous rather than arbitrary.
    static QrCode encodeBytes(const std::uint8_t *data, std::size_t len);
    static QrCode encodeText(const std::string &s)
    {
        return encodeBytes(reinterpret_cast<const std::uint8_t *>(s.data()), s.size());
    }

    // How many payload bytes fit at a given version, level M.  Public because
    // the panel has to say "this code is too long" before it tries.
    static int byteCapacity(int version);

private:
    int m_size = 0;
    int m_version = 0;
    int m_mask = -1;
    std::vector<std::uint8_t> m_modules;

    friend struct QrBuilder;
};

}  // namespace Ppcp
