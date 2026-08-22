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

// A PPCPBNDL file, streamed into the SAME sink a socket feeds.  Work package H3.
//
// ⚠ THERE IS NO IMPORTER, AND THAT IS THE WHOLE DESIGN.  Plan decision A10 and
// CORE §9: "An exported offline session is a recorded PPCP message stream
// replayed from a file.  A CONSUMER GAINS A FILE TRANSPORT, NOT AN IMPORTER."
// 9a: "a bundle contains the same messages, in the same encoding and framing, as
// the live path.  An implementation that can parse a live session can parse a
// bundle."  ENC §7's closing line says what the alternative costs: "three
// artefacts collapse into this one format — the export bundle, the regression
// fixture, and the store-and-forward path.  One parser, one schema, one set of
// conformance tests.  A separate 'import' feature is how two ingest paths and a
// drifting schema come about."
//
// ⚠ WHAT THIS CLASS IS, NOW THAT L8 HAS LANDED.  It is a FILE READER and a
// CHUNKING POLICY, and nothing else.  Every rule of ENC §7 — the container
// header, the frame walk, ENC 7d's truncation, 7f's tolerated MINOR, 7c's
// manifest ordering, I34's capture index — is `ppcp_bundle_reader`, in libppcp,
// where both applications get the same answer (ground rule 1).  The first draft
// of this file walked the frames itself and got the container wrong: it took
// PPCP_BUNDLE_MAGIC_BYTES + PPCP_BUNDLE_HEADER_BYTES = 24 as the header size
// when the 16-byte header INCLUDES the magic, and the test that checked it
// built its bundles the same wrong way, so the two agreed with each other and
// not with the specification.  That is the argument for ground rule 1 in one
// paragraph.
//
// ⚠ WHY IT FEEDS ONE FRAME AT A TIME.  ppcp_bundle_reader_feed() consumes as
// many whole frames as the buffer holds, and the peer's event ring is four
// deep and DROPS THE OLDEST when it overflows (ppcp_peer.c).  A whole-file feed
// would therefore deliver every frame and let the embedding see the last four.
// So the transport presents exactly one frame per call and the caller drains
// events between them — which is a chunking policy a socket also has, not a
// second ingest path.  `payload_chunk.data` points into the fed buffer as well,
// so the caller must write the clip bytes out before the next frame arrives.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <ppcp/bundle.h>

namespace Ppcp {

class PpcpBundleTransport {
public:
    struct Result {
        bool        ok = false;
        std::string error;         // empty when ok

        std::uint16_t minor = 0;   // ENC 7f — what the writer said it was
        std::size_t frames = 0;
        std::size_t bytesConsumed = 0;

        // ENC 7d — a truncated FINAL frame means the bundle is incomplete.  It
        // is NOT an error (ENC 3c: truncation at the end of a byte stream is
        // fatal on a live transport and is not one in a bundle); it is a fact
        // the completeness rule of I10 consumes.
        bool truncated = false;

        // I10 / ENC 7d, kept apart because CT-I36 (c) and (d) turn on telling
        // them apart: a truncated tail in a Session asserted `partial` is the
        // declared incompleteness, and the same truncation in a Session
        // asserted `complete` is a defect.
        bool              assertedCompleteness = false;
        ppcp_completeness asserted = PPCP_UNKNOWN;
        // What ppcp_bundle_reader_finish() decided.  Three states, not two:
        // `unknown` is a whole bundle that never said, and it is not `complete`
        // because completeness is asserted by the owner and never inferred.
        ppcp_completeness completeness = PPCP_UNKNOWN;

        // ENC 7c — a MUST on the writer, so false means a non-conformant
        // bundle.  Read on anyway: one misordered frame is not a session.
        bool manifestOrdered = true;
    };

    struct Options {
        // The peer the frames are fed to — the same engine a socket feeds.
        // NULL parses and accounts without delivering, which is what a fixture
        // validator wants (L8's own words).
        ppcp_peer *sink = nullptr;

        // I34 — what this host already holds, and where the walk writes what it
        // met.  Seeded from the ledger so a re-import is a no-op rather than a
        // duplicate; NULL means the reader starts empty.
        ppcp_capture_index *index = nullptr;

        // Called after every frame has reached the sink, so the embedding can
        // drain the peer's events before the next frame overwrites them.
        std::function<void(std::uint8_t channel)> onFrame;
    };

    static Result streamFile(const std::string &path, const Options &opt);
    static Result streamBytes(const std::uint8_t *bytes, std::size_t len, const Options &opt);
};

}  // namespace Ppcp
