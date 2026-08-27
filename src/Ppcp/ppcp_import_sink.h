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

// What the host DOES with the frames a bundle fed it.  Work package H3.
//
// The transport moves bytes and the engine decodes them; this is the third
// thing, and it is the only one of the three that is PinPointStudio's opinion:
// which of the decoded messages become records on this host's disk.  It reads
// `ppcp_peer_next_event()`, which is the same queue the live socket fills — so
// a Capture that arrived in a file and a Capture that arrived on a wire reach
// this class through one code path and there is no branch in here that could
// tell them apart (plan A10, CORE §9).
//
// ⚠ WHY IT DRAINS AFTER EVERY FRAME.  The engine's event ring is four deep and
// DROPS THE OLDEST when it overflows, and `payload_chunk.data` points into the
// buffer the transport fed — so the bytes of a clip are only valid until the
// next frame is presented.  PpcpBundleTransport calls back per frame for
// exactly this reason.
//
// ⚠ WHAT IS NOT HERE.  The imported Session is NOT yet written into the swing
// library as a `swing.json` beside the live captures.  It cannot be: this
// application's `Session.id` is a filesystem directory path and its `Shot.id`
// an `int` ordinal (src/Export/swing_paths.h), and CORE 8.5c keys idempotent
// re-import on OPAQUE ids.  Bolting an opaque PPCP identity onto a path-derived
// one is host work item 2 in the PPS review, and doing it here would either
// duplicate sessions on the second import or throw the PPCP identity away.  So
// the clips and the ledger land under an import root with their real ids, and
// the join to the swing library waits for the identity change rather than
// pre-empting it.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <ppcp/bundle.h>
#include <ppcp/peer.h>

#include "ppcp_bundle_transport.h"
#include "ppcp_import_ledger.h"

namespace Ppcp {

class PpcpImportSink {
public:
    struct Config {
        // Where a Session's clips land.  One directory per import root; the
        // sink makes `<root>/<peer>/<session>/` under it.
        std::string importRoot;
        // False writes no payload bytes at all, which is what a conformance run
        // that only cares about identity wants.
        bool writeClips = true;
    };

    struct Stats {
        std::string ownerPeerId;      // the MINTING peer (I34's second scope)
        std::string sessionId;
        std::size_t streams = 0;
        std::size_t captures = 0;         // announced
        std::size_t capturesNew = 0;      // admitted
        std::size_t capturesAlreadyHeld = 0;   // I34 — the no-op
        std::size_t digestConflicts = 0;
        // A Capture announced before this walk knew who minted it. MSG 3.3c
        // ("a peer declares BEFORE it originates any message referencing a
        // Source, Stream or Candidate") makes that non-conformant, but ENC §7
        // never says a bundle MUST carry a `declare` at all — so it is counted
        // rather than assumed away. I34's identity is unresolvable without it.
        std::size_t capturesUnattributable = 0;
        // ENC 6g / E7 — how many payloads named their container.  A zero
        // against a non-zero `clipsWritten` means a sender that has not taken
        // E7 yet, and the extension came off the Stream-kind fallback.
        std::size_t payloadsWithContainer = 0;
        std::size_t clipsWritten = 0;
        std::size_t clipBytes = 0;
        std::size_t commitsQueued = 0;
        std::size_t unknownEvents = 0;    // MSG 1b / I13 — carried, not fatal
        std::string sessionDir;
    };

    PpcpImportSink(PpcpImportLedger &ledger, ppcp_peer *peer, Config cfg);
    ~PpcpImportSink();

    // Seeded from the ledger, handed to the reader, and the authority on
    // whether a Capture is new (I34).
    ppcp_capture_index *index() { return &m_index; }

    // Everything the engine has queued since the last call.  THE OFFLINE PATH:
    // this class is the only drainer of a bundle's own engine.
    void drainEvents();

    // ⚠ THE LIVE PATH.  A socket's event ring has one drainer (`PpcpHostPeer`),
    // so the embedding registers a hook and feeds each event here instead.
    // Consumes nothing.
    //
    // ⚠ AND THE CALLER MUST FILTER ON `ev.imported`.  A live link carries BOTH
    // the running Session's own captures — which belong to `VideoInputPpcp` —
    // and the frames of a stored Session being REPLAYED under MSG §9.1, which
    // are what this class is for.  Feeding it the live ones would file a
    // capture the user is making right now as an import of somebody's archive.
    void observeEvent(const ppcp_event &ev);

    // CORE 4.4a / ENC 7d — the Session's completeness, and the close.  Called
    // once the walk is over, with what the reader decided.
    void finish(const PpcpBundleTransport::Result &r);

    // The same, for a Session replayed onto a live link, where there is no
    // bundle and therefore no bundle-level completeness assertion to resolve
    // against (ENC 7d has nothing to say here).  A replay cut short by a dying
    // link simply never delivers `session_close`, so the Session stays open in
    // the ledger — which is the honest record, and is what lets a later
    // reconnection finish it.
    void finishLive();

    const Stats &stats() const { return m_stats; }

private:
    void onCapture(const ppcp_msg *m);
    void onPayloadBegin(const ppcp_msg *m);
    void onPayloadChunk(const ppcp_msg *m);
    void onPayloadEnd(const ppcp_msg *m);
    std::string sessionDir();
    std::string clipPath(const std::string &captureId, const std::string &container) const;

    struct OpenPayload {
        std::string captureId;
        std::string path;
        std::uint64_t declaredBytes = 0;
        std::string container;     // ENC 6g / E7 — the IANA media type, or empty
        std::uint64_t written = 0;
        void *file = nullptr;      // FILE*, opaque so the header stays clean
    };

    PpcpImportLedger  &m_ledger;
    ppcp_peer         *m_peer = nullptr;
    Config             m_cfg;
    Stats              m_stats;
    ppcp_capture_index m_index{};
    // stream id -> kind, so a clip gets a plausible extension.  ENC §6 carries
    // no container type at all — see the note in the .cpp.
    std::map<std::string, std::string> m_streamKind;
    std::map<std::string, std::string> m_captureStream;
    OpenPayload m_open;
    bool        m_sawClose = false;
};

}  // namespace Ppcp
