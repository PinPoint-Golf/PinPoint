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

#include <QObject>
#include <QString>
#include <deque>
#include <vector>

#include "../../Video/VideoInputPpcp.h"

namespace Ppcp { class PpcpImportLedger; }

// PpcpClipFiler — where a phone's swing video becomes part of a swing.
//
// ⛔ THE JOIN THAT WAS DELIBERATELY LEFT OPEN, AND WHY IT CAN BE MADE NOW.
// `clipReady()` was never connected, and the comment saying so was right: a
// PpcpClip carries OPAQUE identities the device mints, this application's swing
// identity is a DERIVED folder name, and writing a clip under the derived one
// throws away the identity CORE 8.5c keys idempotent re-import on.  H3 made the
// same call for imported bundles and refused to bodge it.
//
// What changed is that the ledger now holds both: `SwingRef` on a CaptureRecord
// links the opaque key to the swing without either scheme giving ground (CORE
// 8.5a/8.5b — reconciliation creates links, "no entity is rewritten or merged"),
// and swing.json's `origin` block is the same link written from the other end.
// So this class does not merge the two identities; it records the relation.
//
// ── THE ORDERING PROBLEM THIS EXISTS TO SOLVE ─────────────────────────────
//
// The request goes out at the shot.  The swing folder is allocated 4-11 s later,
// behind the post-roll and the history gather.  The clip may arrive on either
// side of that, and a shot may produce no folder at all.  So:
//
//   · a clip that arrives BEFORE the folder is PARKED, never dropped;
//   · the folder, when it lands, files everything parked for that shot;
//   · a shot that produced no folder discards what it parked, and says so.
//
// ── WHY ONE SHOT AT A TIME IS SOUND ───────────────────────────────────────
//
// `ShotController::armed()` includes `!m_processorBusy`, so a second shot is
// refused while the first is in the pipeline — exactly one shot is ever
// in flight.  The correlation is still keyed on the PPCP shot id rather than on
// arrival order, because a clip for shot N can legitimately arrive while shot
// N+1 is being processed, and I27 makes the Capture's own anchor the honest
// answer to "which shot is this".
class PpcpClipFiler : public QObject
{
    Q_OBJECT

public:
    explicit PpcpClipFiler(QObject *parent = nullptr);

    // Borrowed, never owned — the one ledger PpcpHostService loads.
    void setLedger(Ppcp::PpcpImportLedger *ledger) { m_ledger = ledger; }

    // Sent when the payload is DURABLY held (MSG 8.4a).  Supplied by main.cpp
    // because only PpcpHostService knows which link owns which peer, and the
    // ledger's owed-commit queue is what survives a link dying mid-send.
    using CommitFn = std::function<void()>;
    void setCommitPump(CommitFn f) { m_pumpCommits = std::move(f); }

    // ⭐ THE CHAIN, COUNTED, so a rig can assert on it instead of grepping four
    // logs.  Every one of these had to be reconstructed by hand from log lines
    // on 1 September, and two of them (`asked`, `filed`) were the difference
    // between "the leg works" and "no frame has ever landed".
    struct Stats {
        std::size_t asked     = 0;   // Streams a capture_request named
        std::size_t arrived   = 0;   // clipReady reached this class
        std::size_t parked    = 0;   // arrived before the swing folder existed
        std::size_t filed     = 0;   // bytes written into a swing, ledger updated
        std::size_t absent    = 0;   // the owner answered `absent` (I10 — an ANSWER)
        std::size_t duplicate = 0;   // I34 — AlreadyHeld, correctly written once
        std::size_t orphaned  = 0;   // anchored to no Shot, or to one we never asked about
        std::size_t failed    = 0;   // could not be written
    };
    const Stats &stats() const { return m_stats; }

public slots:
    // One Stream was asked for a clip around `shotId`.
    void onCaptureAsked(const QString &shotId, const QString &peerId,
                        const QString &sourceId, const QString &streamId,
                        const QString &alias);

    // The swing folder for the shot in flight now exists.
    void onSwingReady(const QString &swingDir);

    // The shot produced no folder — nothing can be filed against it.
    void onSwingFailed();

    // A Capture finished arriving.  `absent` is an ANSWER, not a failure (I10).
    void onClipReady(const PpcpClip &clip);

signals:
    // A clip landed in a swing and the document names it.  Whoever wants the
    // swing re-analysed listens here; this class does not queue analysis, so a
    // clip landing can never steal the stage from a golfer still hitting.
    void clipFiled(const QString &swingDir, const QString &alias);

private:
    struct Asked {
        QString peerId, sourceId, streamId, alias;
    };
    struct Shot {
        QString            shotId;
        QString            swingDir;      // empty until the folder exists
        std::vector<Asked> asked;
        std::vector<PpcpClip> parked;     // arrived before the folder did
        bool               abandoned = false;
    };

    Shot *find(const QString &shotId);
    Shot *awaiting();
    QString aliasFor(const Shot &s, const PpcpClip &clip) const;
    bool file(Shot &s, const PpcpClip &clip);

    Stats                   m_stats;
    Ppcp::PpcpImportLedger *m_ledger = nullptr;
    CommitFn                m_pumpCommits;
    // Bounded: a clip that turns up long after its shot has scrolled off is a
    // clip with nowhere to go, and an unbounded map would be a slow leak across
    // a season.  Eight is several shots' grace at 15-40 s per shot.
    std::deque<Shot>        m_shots;
    static constexpr std::size_t kMaxTrackedShots = 8;
};
