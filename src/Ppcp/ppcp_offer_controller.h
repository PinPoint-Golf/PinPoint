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

// The sessions a CONNECTED capture device is offering, and the one control that
// accepts one.  Work package H5 (the offer list); MSG §9.1–9.2, CORE §9.
//
// ⚠ SESSIONS ARE NEVER IMPORTED FROM FILES, and this class is why H3's file
// picker was deleted the day it landed.  The user's flow is: a capture device
// connects, says what it holds, and the host picks from that list in-screen.
// There is no menu, no `FileDialog`, and no "import" verb anywhere in the user
// interface — plan §9, 22 August 2026.
//
// ⚠ AND IT IS STILL NOT AN IMPORTER.  Accepting an offer sends
// `session_accept`; the device then REPLAYS its stored bundle onto the live
// link (`ppcp_bundle_replay`, MSG 9.1), and those frames arrive through the
// ordinary ingest path — `ppcp_peer_feed()`, the same function a live session
// drives.  So this class originates one message and reads three; it parses no
// bundle and knows no file format.  ENC 7a is what makes that true rather than
// merely tidy: a bundle is a recorded message stream with a sixteen-byte header
// in front of it.
//
// ── 9.1a — WHY THE ACCEPT CARRIES DIGESTS ─────────────────────────────────
//
// `have_digests` is what this host ALREADY HOLDS, so the device skips those
// payloads on replay.  Identity here is `Capture.digest` — CONTENT — and that
// is a DIFFERENT rule from I34's re-import identity, which is `Capture.id`
// scoped by session and peer with the digest deliberately not in the key.  Both
// rules are right and they answer different questions: "must you send me these
// bytes again" and "have I taken this Capture in before".  The ledger answers
// both and this class asks it the first one.
//
// ⚠ THE `capture_announce` AND THE MANIFEST ENTRY ARE STILL SENT for a Capture
// whose payload is skipped.  The importer needs the Capture RECORD; it is the
// payload that is redundant, not the fact.  Nothing here has to do anything
// about that — it is the device's obligation and libppcp's replay implements
// it — but a reader of this file who assumes otherwise will misread the row
// counts.

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <string>

// The whole message vocabulary, because `ppcp_offer_verdict` lives beside the
// body it belongs to and the moc-generated translation unit needs it too.
#include <ppcp/message.h>
#include <ppcp/model.h>

struct ppcp_peer;
struct ppcp_event;

namespace Ppcp { class PpcpImportLedger; }

class PpcpOfferController : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    enum Roles {
        SessionIdRole = Qt::UserRole + 1,
        PeerIdRole,          // the peer the offer arrived from
        MintingPeerIdRole,   // 8.5c — the peer whose namespace the ids are in
        EpochLabelRole,      // I15: a LABEL, never used to compute an interval
        HasEpochRole,
        CompletenessRole,    // "complete" | "partial" | "unknown" — ASSERTED (I10)
        BytesEstimateRole,
        BytesLabelRole,
        HasBytesRole,
        AlreadyHeldRole,     // this host's ledger says it has this Session
        AcceptedRole         // an accept has been sent for this row
    };
    Q_ENUM(Roles)

    explicit PpcpOfferController(QObject *parent = nullptr);
    ~PpcpOfferController() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString status() const { return m_status; }

    // The link an offer arrived on and an accept goes back out on.  `peerId` is
    // the counterpart's declared Id — the offer body carries a `minting_peer_id`
    // which may differ, because a device may hold a Session another peer minted
    // and 8.5c scopes identity by the MINTER, not by whoever hands over the
    // bytes.
    void attach(ppcp_peer *peer, const QString &peerId);
    void detach();

    // The ledger this host keeps, so `have_digests` is real rather than empty.
    // Null is legal and means "I hold nothing", which makes the device replay
    // everything — correct, and merely slower.
    void setLedger(const Ppcp::PpcpImportLedger *ledger) { m_ledger = ledger; }

    // Every event the peer raised.  `session_offer` adds a row; `session_accept`
    // and `session_manifest` are noted for the status line.  Consumes nothing.
    void observe(const ppcp_event &ev);

    // ── The one control on each row ────────────────────────────────────────
    //
    // MSG 9.2 — `session_accept` with `verdict: accept` and the digests this
    // host holds.  Returns false and sets `status` if the row is gone or the
    // link is down; it never silently does nothing.
    Q_INVOKABLE bool acceptOffer(int row);
    // 9.2 — the other two verdicts.  `already_held` is not a refusal: it is the
    // honest answer when I34 says a re-import would be a no-op, and it saves
    // the device a replay it would have thrown away.
    Q_INVOKABLE bool refuseOffer(int row, const QString &reason = {});

    // How many offers arrived, ever — including ones already replaced by a
    // second offer of the same Session.  A test's observable.
    std::size_t offersSeen() const { return m_offersSeen; }
    std::size_t acceptsSent() const { return m_acceptsSent; }
    // 9.1a — how many digests the last accept carried, and how many the ledger
    // held but the wire could not (PPCP_MAX_HAVE_DIGESTS).  The second number
    // is not a failure: the device re-sends those payloads.
    std::size_t lastAcceptDigests() const { return m_lastAcceptDigests; }
    std::size_t lastAcceptDigestsDropped() const { return m_lastAcceptDropped; }

signals:
    void countChanged();
    void statusChanged();
    // The host accepted; the replayed frames now arrive through the ordinary
    // ingest path.  The session layer uses this to open a progress affordance.
    void offerAccepted(const QString &peerId, const QString &sessionId);

private:
    struct Row {
        QString       sessionId;
        QString       peerId;
        QString       mintingPeerId;
        bool          hasEpoch = false;
        std::int64_t  epochWallUtcNs = 0;
        ppcp_completeness completeness = PPCP_UNKNOWN;
        bool          hasBytes = false;
        std::uint64_t bytesEstimate = 0;
        std::uint64_t msgId = 0;      // ENC 5b — the accept's `reply_to`
        bool          alreadyHeld = false;
        bool          accepted = false;
    };

    void setStatus(const QString &s);
    int indexOf(const QString &peerId, const QString &sessionId) const;
    bool sendAccept(int row, ppcp_offer_verdict verdict, const QString &reason);

    QVector<Row>  m_rows;
    ppcp_peer    *m_peer = nullptr;
    QString       m_peerId;
    const Ppcp::PpcpImportLedger *m_ledger = nullptr;
    QString       m_status;
    std::size_t   m_offersSeen = 0;
    std::size_t   m_acceptsSent = 0;
    std::size_t   m_lastAcceptDigests = 0;
    std::size_t   m_lastAcceptDropped = 0;
};
