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

#include "ppcp_bundle_transport.h"

#include <fstream>
#include <vector>

#include <ppcp/frame.h>

namespace Ppcp {
namespace {

// One reader, in caller-owned storage.  libppcp allocates nothing (ground rule
// 8), so the embedding owns every byte of it — including the decode arena, which
// is why this is a vector and not a stack buffer.
struct Reader {
    std::vector<std::uint8_t> storage;
    ppcp_bundle_reader       *r = nullptr;

    ppcp_result open(ppcp_peer *sink)
    {
        storage.resize(ppcp_bundle_reader_sizeof());
        return ppcp_bundle_reader_new(storage.data(), storage.size(), sink, &r);
    }
};

}  // namespace

PpcpBundleTransport::Result PpcpBundleTransport::streamFile(const std::string &path,
                                                            const Options &opt)
{
    Result r;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        r.error = "cannot open " + path;
        return r;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    return streamBytes(bytes.data(), bytes.size(), opt);
}

PpcpBundleTransport::Result PpcpBundleTransport::streamBytes(const std::uint8_t *bytes,
                                                             std::size_t len,
                                                             const Options &opt)
{
    Result res;

    Reader rd;
    const ppcp_result orc = rd.open(opt.sink);
    if (orc != PPCP_OK) {
        res.error = std::string("bundle reader: ") + ppcp_result_str(orc);
        return res;
    }

    // I34 — the index is seeded with what this host already holds, so a Capture
    // met for the second time is reported as already held rather than imported
    // twice.  CORE 8.5c puts the rule in the library because both applications
    // need the same one; the PERSISTENCE of it is the embedding's, which is the
    // ledger.
    if (opt.index != nullptr)
        *ppcp_bundle_reader_index(rd.r) = *opt.index;

    // ENC §7 — the 16-byte container header.  It CONTAINS the magic; the two
    // are not consecutive fields.  Fed on its own so the reader answers for it.
    {
        std::size_t took = 0;
        const std::size_t n = (len < PPCP_BUNDLE_HEADER_BYTES) ? len : PPCP_BUNDLE_HEADER_BYTES;
        const ppcp_result rc = ppcp_bundle_reader_feed(rd.r, bytes, n, &took);
        if (rc != PPCP_OK) {
            // A wrong magic, or a MAJOR this reader cannot read (ENC 7f).  The
            // MINOR half of 7f never lands here: a higher one is accepted.
            res.error = std::string("bundle header refused: ") + ppcp_result_str(rc);
            return res;
        }
        res.bytesConsumed += took;
    }

    // One frame per feed.  See the header: the peer's event ring is four deep
    // and drops the oldest, and `payload_chunk.data` points into the buffer we
    // hand over, so the caller has to be given the wheel between frames.
    std::size_t pos = res.bytesConsumed;
    while (pos < len) {
        std::size_t slice = len - pos;
        if (slice >= PPCP_FRAME_HEADER_BYTES) {
            ppcp_frame_header fh{};
            if (ppcp_frame_header_parse(bytes + pos, &fh) == PPCP_OK) {
                const std::size_t whole =
                    static_cast<std::size_t>(PPCP_FRAME_HEADER_BYTES) + fh.payload_len;
                if (whole < slice) slice = whole;
            }
            // A header the parser refuses is handed over unshortened: ENC 8a is
            // the READER's refusal to make, and a transport that pre-judged it
            // would be deciding what the library is for.
        }

        std::size_t took = 0;
        const ppcp_result rc = ppcp_bundle_reader_feed(rd.r, bytes + pos, slice, &took);
        if (rc != PPCP_OK) {
            // ENC 8a — a `payload_len` past the channel limit means the stream
            // has desynchronised and cannot be resynchronised.  Everything read
            // so far still stands: what was fed was fed, and the Session it
            // describes is partial.
            res.bytesConsumed += took;
            res.error = std::string("malformed frame at offset ") + std::to_string(pos)
                        + ": " + ppcp_result_str(rc);
            res.truncated = true;
            res.frames = ppcp_bundle_reader_frame_count(rd.r);
            res.minor = ppcp_bundle_reader_minor(rd.r);
            (void)ppcp_bundle_reader_finish(rd.r, &res.completeness);
            if (opt.index != nullptr) *opt.index = *ppcp_bundle_reader_index(rd.r);
            return res;
        }
        if (took == 0) break;   // ENC 7d — a trailing partial frame, and not an error

        res.bytesConsumed += took;
        if (opt.onFrame) {
            ppcp_frame_header fh{};
            const std::uint8_t ch =
                (ppcp_frame_header_parse(bytes + pos, &fh) == PPCP_OK) ? fh.channel : 0u;
            opt.onFrame(ch);
        }
        pos += took;
    }

    res.frames = ppcp_bundle_reader_frame_count(rd.r);
    res.minor = ppcp_bundle_reader_minor(rd.r);
    res.truncated = ppcp_bundle_reader_truncated(rd.r);
    res.manifestOrdered = ppcp_bundle_reader_manifest_ordered(rd.r);
    res.assertedCompleteness = ppcp_bundle_reader_asserted(rd.r, &res.asserted);
    (void)ppcp_bundle_reader_finish(rd.r, &res.completeness);
    if (opt.index != nullptr) *opt.index = *ppcp_bundle_reader_index(rd.r);
    res.ok = true;
    return res;
}

}  // namespace Ppcp
