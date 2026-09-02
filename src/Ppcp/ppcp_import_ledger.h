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

// What this host has already taken in, so that taking it in again does nothing.
// Work package H3; invariants I34, I10 and CORE 5.14h.
//
// ── I34 / CORE 8.5c — WHAT IDENTIFIES A CAPTURE ────────────────────────────
//
// "Re-import of a session already held is a no-op, never a duplicate.  Session
// identity is `Session.id` plus the minting `Peer.id`.  CAPTURE IDENTITY IS
// `Capture.id`, SCOPED BY THOSE TWO; `Capture.digest` is a CONTENT check where
// present, NOT the identifier."
//
// The specification records why, and it is the kind of mistake that only shows
// up on the second import: "8.5c named the digest as the identifier until Draft
// 3, which left two ordinary cases with no identity at all: a Capture of
// `completeness: absent` has no payload and therefore no hash, and a
// `complete` + `pending` Capture may reach a bundle before its digest is
// computed.  Absent captures are the most important content of a partial
// session, and identifying them by a hash they cannot have would have
// duplicated them on exactly the second import the rule exists to make safe."
//
// ── I10 / ENC 7d — WHAT COMPLETENESS MEANS ─────────────────────────────────
//
// "The reader treats the Session as `completeness: partial` ONLY IF the bundle
// itself did not assert otherwise, and NEVER upgrades a partial Session to
// complete on the strength of what happened to be present."  Completeness is
// ASSERTED by the owner and never inferred by the receiver — so a truncated
// bundle that asserted `complete` stays complete, and a second import that
// happens to carry everything does not promote a session the owner called
// partial.
//
// ── CORE 5.14h / 5.14h1 — WHAT IS OWED BACK ────────────────────────────────
//
// "A receiver that durably commits a Capture obtained FROM A BUNDLE sends
// `capture_committed` for it ON ITS NEXT CONNECTION with the owning peer."
// Without it `confirmed` is unreachable on the offline path, "which is the path
// an entry-level capture device spends most of its life in" — and a device that
// can never reach `confirmed` can never evict anything under I38, so its
// storage fills across a season. 5.14h1: such a message naming a CLOSED session
// is accepted, not answered `unknown_session`, because it may arrive days after
// the bundle was imported and releasing storage stays legitimate after a
// Session closes.

#include <cstdint>
#include <string>
#include <vector>

#include <ppcp/bundle.h>

namespace Ppcp {

// I34 — the three parts of a Capture's identity, in the order they scope.
struct CaptureKey {
    std::string peerId;      // the MINTING peer, not whoever handed us the file
    std::string sessionId;
    std::string captureId;

    bool operator==(const CaptureKey &o) const
    {
        return peerId == o.peerId && sessionId == o.sessionId && captureId == o.captureId;
    }
    bool valid() const { return !peerId.empty() && !sessionId.empty() && !captureId.empty(); }
    std::string str() const { return peerId + "/" + sessionId + "/" + captureId; }
};

// CORE 5.10 / 5.14 — asserted by the owner, never inferred (I10).
enum class Completeness { Complete, Partial, Absent };
const char *completenessStr(Completeness c);

// The ledger stores a digest as hex because it round-trips through JSON; the
// wire wants bytes.  Exposed rather than left inline in `heldDigests()` because
// `capture_committed` needs the same conversion, and two hand-rolled hex
// decoders in one subsystem is one too many.  False for a string that is not
// exactly PPCP_SHA256_BYTES*2 hex digits — which includes the legitimate empty
// case, an owner that had not computed one.
bool digestFromHex(const std::string &hex, ppcp_digest *out);
// The other direction, for a digest that arrived in a message and must be kept
// in the ledger (and sent back in `capture_committed`, 8.4a).  Empty where the
// digest is not present.
std::string digestToHex(const ppcp_digest &d);

// ⭐ THE LINK, AND THE WHOLE REASON THIS CLASS EXTENDS TO THE LIVE PATH.
//
// PPCP identity is OPAQUE and minted by the device: a Capture is `Capture.id`
// scoped by `Session.id` and the minting `Peer.id` (I34, CORE 8.5c), and the
// spec is explicit that the digest is not the identifier.  PinPoint Studio's
// identity is DERIVED: `SwingPaths::allocateSwingDir()` composes a folder from
// athlete, date and a naming pattern, and it changes if the user changes a
// preference.  Neither can be expressed in the other.
//
// So they are LINKED, not merged — CORE 8.5a/8.5b: reconciliation creates links
// and "no entity is rewritten or merged".  The opaque key stays authoritative
// on the PPCP side, the derived one on the library side, and this record relates
// them.  Each side can be rebuilt from the other: a lost ledger can be walked
// back out of the swing.json files, and a lost swing.json still leaves the
// ledger able to refuse a duplicate.  That redundancy is the point.
//
// Empty for a bundle import, correctly: those captures are not in a swing.
struct SwingRef {
    std::string sessionDir;    // "2026-09-01_Mark-Liversedge_Wrist_01"
    std::string swingId;       // "swing_0007"
    std::string streamAlias;   // "phoneWide" — which streams[] element it is

    bool empty() const
    {
        return sessionDir.empty() && swingId.empty() && streamAlias.empty();
    }
};

class PpcpImportLedger {
public:
    struct CaptureRecord {
        CaptureKey  key;
        std::string digestHex;       // empty where the owner had not computed one
        Completeness completeness = Completeness::Complete;
        std::string localPath;       // where the clip landed, alongside swing.json
        SwingRef    swingRef;        // the derived identity; empty for bundles
    };

    struct SessionRecord {
        std::string peerId;
        std::string sessionId;
        Completeness completeness = Completeness::Partial;
        bool         completenessAsserted = false;   // did the bundle say?
        bool         closed = false;
        std::string  localDir;
    };

    // What is owed back to an owning peer on its next connection (5.14h).
    struct PendingCommit {
        CaptureKey  key;
        std::string digestHex;
    };

    // The outcome of offering one Capture to the ledger.
    enum class Admission {
        Recorded,        // new; the caller should write the payload
        AlreadyHeld,     // I34 — a no-op, never a duplicate
        DigestConflict,  // same identity, different content: reported, never merged
    };

    PpcpImportLedger() = default;

    bool load(const std::string &path);
    bool save() const;

    // ── The move to one ledger for both landing sites ─────────────────────
    //
    // The ledger used to live at `<library>/PPCP Imports/ppcp-import.json`,
    // which was right while bundles were the only thing it recorded and wrong
    // as soon as a live capture lands in the swing library instead.  It is now
    // `<library>/ppcp-ledger.json`, and `localPath` points wherever the bytes
    // actually went.  One ledger, two landing sites, one identity rule.
    //
    // Folds a legacy file into this one and answers how many records it added.
    // ⚠ THE LEGACY FILE IS LEFT IN PLACE.  Deleting it would make the migration
    // irreversible on a version downgrade, and it costs a few hundred bytes.
    // Records already held win: admit() never rewrites one (I9, CORE 8.5a), so
    // folding the same file in twice is a no-op and this is safe to call on
    // every launch.  Legacy records carry no `swingRef`, correctly.
    std::size_t foldIn(const std::string &legacyPath);
    const std::string &path() const { return m_path; }
    void setPath(std::string p) { m_path = std::move(p); }

    // ── Sessions ──────────────────────────────────────────────────────────
    //
    // `asserted` is what the BUNDLE said, and `bundleTruncated` is what the
    // reader observed. ENC 7d resolves the two in exactly one direction: an
    // observation may downgrade a session nobody asserted anything about, and
    // may never upgrade one.
    void noteSession(const std::string &peerId, const std::string &sessionId,
                     bool asserted, Completeness assertedValue, bool bundleTruncated,
                     const std::string &localDir = {});
    bool holdsSession(const std::string &peerId, const std::string &sessionId) const;
    const SessionRecord *session(const std::string &peerId, const std::string &sessionId) const;

    // CORE 5.10 — a Session that has closed. 5.14h1 makes this NOT a reason to
    // refuse a later capture_committed.
    void closeSession(const std::string &peerId, const std::string &sessionId);

    // ── Captures ──────────────────────────────────────────────────────────
    Admission admit(const CaptureRecord &rec);
    bool holds(const CaptureKey &k) const;

    // Where the payload landed, recorded once it IS landed. Separate from
    // admit() because admit() is the identity decision and must not be a way to
    // rewrite a held record: I9 and CORE 8.5a/8.5b say reconciliation creates
    // links and "no entity is rewritten or merged". A local path is not part of
    // the entity — it is this host's note of where it put the bytes.
    bool setLocalPath(const CaptureKey &k, const std::string &path,
                      const std::string &digestHex = {});

    // The other half of the link, and separate from setLocalPath() for the same
    // reason that one is separate from admit(): where the bytes went and which
    // swing they belong to are two different notes this host makes ABOUT a
    // Capture, neither of them part of the entity.  A live capture sets both; a
    // bundle import sets only the path.
    bool setSwingRef(const CaptureKey &k, const SwingRef &ref);

    const CaptureRecord *capture(const CaptureKey &k) const;

    // ── capture_committed, owed on the owner's next connection ────────────
    //
    // Called when the payload is DURABLY held — written and flushed, not merely
    // received (MSG 8.4a). 8.4b: an owner may not set `confirmed` on its own
    // authority, so this queue is the only route to it on the offline path.
    void queueCommitted(const CaptureKey &k, const std::string &digestHex);
    std::vector<PendingCommit> pendingCommits(const std::string &owningPeerId) const;
    std::size_t pendingCommitCount() const { return m_pending.size(); }

    // Called once the message has actually been sent AND the owner has had it —
    // not when it was queued for writing. A commit dropped by a link that died
    // mid-send must still be owed.
    void clearCommitted(const CaptureKey &k);

    // ── MSG 9.1a — what to tell a device it need not send again ──────────
    //
    // `session_accept.have_digests` is what this importer ALREADY HOLDS, and
    // the payload for such a Capture is not replayed.  Identity here is
    // `Capture.digest` — a different rule from I34's re-import identity above,
    // and deliberately so: a digest cannot be the key for an `absent` Capture,
    // and an `absent` Capture has no payload to skip.  So a record with no
    // digest contributes nothing here and that is correct, not a gap.
    //
    // Bounded by PPCP_MAX_HAVE_DIGESTS at the wire; the caller truncates and
    // says so, because a silently truncated list makes the device re-send
    // payloads rather than lose them, which is the safe direction.
    std::vector<ppcp_digest> heldDigests(const std::string &peerId,
                                         const std::string &sessionId) const;

    // I34, decided by libppcp and not by this class.
    //
    // ⚠ THE RULE IS THE LIBRARY'S AND THE MEMORY IS OURS. `ppcp_capture_index`
    // holds the identity rule — `Capture.id` scoped by session and owning peer,
    // with `digest` deliberately NOT in the key — so both applications get the
    // same answer (ground rule 1). It is not persistent, by design: "the
    // embedding owns storage and seeds this from whatever it kept." This is the
    // seeding. Overflow is PPCP_ERR_LIMIT and is reported rather than wrapped,
    // because a silently truncated index would make a held Capture look new.
    bool seedIndex(ppcp_capture_index *ix, std::size_t *outDropped = nullptr) const;

    std::size_t captureCount() const { return m_captures.size(); }
    std::size_t sessionCount() const { return m_sessions.size(); }

private:
    std::string                m_path;
    std::vector<SessionRecord> m_sessions;
    std::vector<CaptureRecord> m_captures;
    std::vector<PendingCommit> m_pending;

    SessionRecord *findSession(const std::string &peerId, const std::string &sessionId);
};

}  // namespace Ppcp
