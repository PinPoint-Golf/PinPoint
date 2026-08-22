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

// Annotations, host side.  Work package H7.  CORE §5.18; MSG §9.0.
//
// ── I37 / 5.18c — THE WHOLE POINT OF THIS FILE IS WHAT IT DOES NOT DO ──────
//
// "There is NO path from anything here to a Shot, a Candidate, a Calibration or
// any computed quantity.  Not a weak one, not a convenience one."  An
// Annotation is a USER ARTEFACT, not an observation: everything else in PPCP
// that carries payload is produced by a Source, which has a clock, a
// calibration and an owning peer.  A person has none of those.
//
// So there is no function on this class that takes an Annotation and returns an
// instant, a phase, a metric or anything an analysis could consume; `kind:
// nav_anchor` in particular is stored and returned and never interpreted as
// phase data.  CONF §3 says CT-I37 is asserted "BY API SURFACE, NOT
// BEHAVIOUR" — so the evidence is (a) this class's surface and (b) a test that
// greps `src/Analysis` for any include of a markup header and fails if one
// appears.  A comment cannot be that evidence; the test is.
//
// ⚠ AND `body` IS OPAQUE.  5.18a makes lossless round-tripping the requirement
// and interpreting the format explicitly NOT one.  The bytes are written to
// their own file and read back byte for byte precisely so that a `format` this
// host has never heard of survives a save, a restart and a re-send.  Nothing
// here parses a body, and a JSON sidecar would have been the wrong container
// for exactly that reason — an 8 KiB blob with an embedded NUL is legal
// (5.18f, ENC §8) and is not text.
//
// ── 5.18e / 9.0c — SUPERSESSION IS THE LIBRARY'S ──────────────────────────
//
// `ppcp_annotation_supersedes()` orders by `id`, then `revision`, then
// `author_peer_id` BYTEWISE, and the tiebreak is not decoration: without it a
// coach at the host and a golfer at the device both hold revision 1, both
// produce revision 2, each ignores the other's equal revision, and the two ends
// diverge permanently while each believes it converged.  This class holds a
// `ppcp_annotation_store` and adds persistence; it does not re-implement the
// comparison, because two implementations of one total order is how the two
// ends stop agreeing.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <ppcp/markup.h>
#include <ppcp/model.h>

struct ppcp_peer;
struct ppcp_event;
struct ppcp_stream;

namespace Ppcp {

class PpcpAnnotationStore {
public:
    PpcpAnnotationStore();
    ~PpcpAnnotationStore();
    PpcpAnnotationStore(const PpcpAnnotationStore &) = delete;
    PpcpAnnotationStore &operator=(const PpcpAnnotationStore &) = delete;

    // Where the persisted annotations live.  One directory per Session and Shot
    // beneath it, so the on-disk layout carries the SCOPE 8.5c scopes identity
    // by and a re-import can find what it already had.  Empty means "hold in
    // memory only", which is what a test wants and what a host with no library
    // configured has.
    bool setRoot(const std::string &root, std::string *err = nullptr);
    const std::string &root() const { return m_root; }

    void attach(ppcp_peer *peer, const std::string &selfPeerId);
    void detach();

    // ── Receiving (MSG 9.0a) ──────────────────────────────────────────────
    //
    // `annotation` travels EITHER direction — the only content in PPCP that
    // does — so this is not a device-only path.  Applies 5.18e and persists
    // where the incoming revision won.  `outReplaced`, where non-null, says
    // whether the store changed: false means what arrived was superseded by
    // what was already held and was IGNORED (9.0c's "lower is ignored"), which
    // is a normal outcome and not a failure.
    bool observe(const ppcp_annotation &a, bool *outReplaced = nullptr,
                 std::string *err = nullptr);

    // Every event the peer raised; the `annotation` arm calls observe().
    void observeEvent(const ppcp_event &ev);

    // ── Authoring (CORE 5.18, MSG 9.0a) ───────────────────────────────────
    //
    // A host-authored line or plane, with `stream_id` NAMING THE VIEW it was
    // drawn on.  5.18j: a view-specific `kind` carries `stream_id` and a
    // non-view-specific one does not, and 5.18g then fixes which timebase `at`
    // is in — the STREAM'S where there is one, `Session.timebase_ref` where
    // there is not.  Both are checked through
    // ppcp_annotation_validate_placement() before a wire sees the message, so a
    // line anchored on the host's clock but claiming a device's view is refused
    // here rather than rendered in the wrong place at the far end.
    //
    // `stream` is the Stream named by `streamId`, or null for a
    // non-view-specific kind.  `atNs` is in that Stream's timebase (5.18g).
    // `revision` starts at 1 and the caller increments it; the store will not
    // do it, because a revision the author did not choose is a revision that
    // races the far end (5.18e's tiebreak exists for the case where they tie).
    bool author(const std::string &annotationId, const std::string &sessionId,
                const std::string &shotId, const std::string &kind,
                const std::string &format, const std::vector<std::uint8_t> &body,
                const std::string &streamId, const ppcp_stream *stream,
                const std::string &timebaseRef, std::int64_t atNs,
                std::int64_t createdAtNs, std::uint64_t revision,
                std::string *err = nullptr);

    // 5.18 — deletion is a REVISION, not a removal.  There is no erase() here:
    // an annotation the far end has not yet seen deleted must still converge,
    // and a store that forgot it would accept the old revision back.
    bool markDeleted(const std::string &annotationId, std::uint64_t revision,
                     std::int64_t createdAtNs, std::string *err = nullptr);

    // ── What is held ──────────────────────────────────────────────────────
    std::size_t count() const;
    const ppcp_annotation *at(std::size_t index) const;
    const ppcp_annotation *find(const std::string &id) const;

    // Every annotation held against one Shot, in `id` order.  A pure filter on
    // the store; it returns Annotations and nothing derived from them (I37).
    std::vector<const ppcp_annotation *> forShot(const std::string &sessionId,
                                                 const std::string &shotId) const;

    // Reads back what was persisted, byte for byte.  Used on start-up and by
    // the round-trip evidence for 5.18a.
    bool loadFromRoot(std::string *err = nullptr);

    struct Stats {
        std::size_t received   = 0;
        std::size_t superseded = 0;   // 9.0c — arrived lower and was ignored
        std::size_t authored   = 0;
        std::size_t sent       = 0;
        std::size_t persisted  = 0;
        std::size_t placementRefused = 0;   // 5.18g / 5.18j
    };
    const Stats &stats() const { return m_stats; }

private:
    bool persist(const ppcp_annotation &a, std::string *err);
    std::string dirFor(const ppcp_annotation &a) const;

    std::vector<std::uint8_t> m_storage;
    ppcp_annotation_store    *m_store = nullptr;
    // The bodies, owned here.  `ppcp_annotation.body` is a borrowed pointer and
    // the library's store copies the bytes into its own slot — but a caller
    // reading one back out gets a pointer into that slot, and an annotation
    // built here for sending needs its bytes to outlive the call.  Keyed by id,
    // one entry per current revision.
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> m_bodies;
    std::string  m_root;
    ppcp_peer   *m_peer = nullptr;
    std::string  m_selfPeerId;
    Stats        m_stats;

    std::vector<std::uint8_t> *bodySlot(const std::string &id);
};

}  // namespace Ppcp
