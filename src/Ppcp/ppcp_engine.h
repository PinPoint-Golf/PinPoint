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

// The one thing both transports hand bytes to.
//
// In its own header, and not beside the socket transport, because plan A10 is
// a claim about exactly this: "PinPointStudio's bundle import is a FILE
// TRANSPORT for the same peer engine, landing in the same ingest path as a live
// socket.  THERE IS NO IMPORTER."  A file transport that had to include an
// OpenSSL-bearing header to reach the sink would not be the same ingest path;
// it would be the socket path with a file bolted to it.

#include <cstddef>
#include <cstdint>

#include <ppcp/common.h>

struct ppcp_peer;

namespace Ppcp {

// What the pump moves bytes into and out of.
//
// In production this is `ppcp_peer` and nothing else. It is an interface
// because the pump is THIS APPLICATION'S code — the part ground rule 7 says the
// embedding owes the library — and it has to be testable without a socket at
// one end.
class PpcpEngine {
public:
    virtual ~PpcpEngine() = default;

    // Bytes in, on the channel they arrived on. ENC 2c's header-matches-stream
    // check is the engine's, which is why the channel travels with the bytes.
    //
    // ⚠ `*out_consumed` IS THE WHOLE REASON THE PUMP HAS A TAIL BUFFER, and it
    // is L6's shape rather than ours. peer.h: "An engine that buffered a whole
    // frame internally would carry nine megabytes of storage per link for the
    // benefit of a caller that already has the bytes in a socket buffer. So the
    // engine buffers NOTHING: it consumes whole frames from the caller's buffer
    // and reports how many bytes it took." A socket hands over arbitrary
    // chunks, so the trailing partial frame is THE EMBEDDING'S to keep and
    // re-present — which is what pump() below does and what it did not have to
    // do before L6 landed.
    virtual ppcp_result feed(std::uint8_t channel, const std::uint8_t *bytes,
                             std::size_t len, std::size_t *out_consumed) = 0;

    // Bytes out, per channel. PPCP_ERR_NOT_FOUND (or *out_len == 0) means the
    // engine has nothing to send on that channel right now.
    virtual ppcp_result drain(std::uint8_t channel, std::uint8_t *out, std::size_t cap,
                              std::size_t *out_len) = 0;

    // The engine underneath, where there is one.
    //
    // ⚠ NOT A LEAK OF THE ABSTRACTION — it is L8's interface showing through.
    // ppcp_bundle_reader_new() takes a `ppcp_peer *` and nothing else, because
    // CORE 9a's "a consumer gains a file transport, not an importer" is only
    // true if the file path reaches the SAME function a socket does. A bundle
    // fed to a C++ interface that merely resembles a peer would be a second
    // ingest path wearing the first one's name.
    virtual ppcp_peer *peer() const { return nullptr; }
};

}  // namespace Ppcp
