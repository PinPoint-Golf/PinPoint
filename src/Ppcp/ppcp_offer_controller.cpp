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

#include "ppcp_offer_controller.h"

#include "ppcp_import_ledger.h"

#include <QDateTime>
#include <QLocale>
#include <QTimeZone>

#include <ppcp/peer.h>

namespace {

QString idToString(const ppcp_id &id) { return QString::fromUtf8(id.v, id.len); }

const char *completenessName(ppcp_completeness c)
{
    // I10 — ASSERTED by the owner, never inferred by the receiver.  `unknown`
    // is a real answer and is shown as one: it means the device did not say,
    // not that the session is fine.
    switch (c) {
    case PPCP_COMPLETE: return "complete";
    case PPCP_PARTIAL:  return "partial";
    case PPCP_ABSENT:   return "absent";
    default:            return "unknown";
    }
}

}  // namespace

PpcpOfferController::PpcpOfferController(QObject *parent) : QAbstractListModel(parent) {}
PpcpOfferController::~PpcpOfferController() = default;

int PpcpOfferController::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QHash<int, QByteArray> PpcpOfferController::roleNames() const
{
    return {
        { SessionIdRole,      "sessionId" },
        { PeerIdRole,         "peerId" },
        { MintingPeerIdRole,  "mintingPeerId" },
        { EpochLabelRole,     "epochLabel" },
        { HasEpochRole,       "hasEpoch" },
        { CompletenessRole,   "completeness" },
        { BytesEstimateRole,  "bytesEstimate" },
        { BytesLabelRole,     "bytesLabel" },
        { HasBytesRole,       "hasBytes" },
        { AlreadyHeldRole,    "alreadyHeld" },
        { AcceptedRole,       "accepted" },
    };
}

QVariant PpcpOfferController::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const Row &r = m_rows.at(index.row());
    switch (role) {
    case SessionIdRole:     return r.sessionId;
    case PeerIdRole:        return r.peerId;
    case MintingPeerIdRole: return r.mintingPeerId;
    case HasEpochRole:      return r.hasEpoch;
    case EpochLabelRole:
        // I15 / CORE 5.3b — `wall_utc` is a LABEL.  It is formatted for a human
        // and never subtracted from anything; this application computes every
        // interval from a monotonic timebase, and the H3 ingest path is
        // asserted to contain no reference to `.epoch` at all.
        if (!r.hasEpoch) return QString();
        return QDateTime::fromMSecsSinceEpoch(r.epochWallUtcNs / 1000000, QTimeZone::UTC)
                   .toLocalTime()
                   .toString(QStringLiteral("d MMM yyyy HH:mm"));
    case CompletenessRole:  return QString::fromLatin1(completenessName(r.completeness));
    case BytesEstimateRole: return static_cast<qulonglong>(r.bytesEstimate);
    case HasBytesRole:      return r.hasBytes;
    case BytesLabelRole:
        if (!r.hasBytes) return QString();
        return QLocale().formattedDataSize(static_cast<qint64>(r.bytesEstimate));
    case AlreadyHeldRole:   return r.alreadyHeld;
    case AcceptedRole:      return r.accepted;
    default: return {};
    }
}

void PpcpOfferController::attach(ppcp_peer *peer, const QString &peerId)
{
    if (peerId.isEmpty()) return;
    m_peers.insert(peerId, peer);
}

void PpcpOfferController::detach(const QString &peerId)
{
    if (m_peers.remove(peerId) == 0 && peerId.isEmpty()) return;

    // The offers were facts about a link that no longer exists.  Keeping them
    // would present a control that cannot work, which is worse than an empty
    // list: the user would press it and nothing would happen.
    //
    // ⚠ ONLY THIS PEER'S ROWS.  The other phone is still connected and its
    // offers are still true; clearing the whole model on one disconnect —
    // which is what detach() used to do — would take them with it.
    bool removedAny = false;
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (m_rows.at(i).peerId != peerId) continue;
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        endRemoveRows();
        removedAny = true;
    }
    if (removedAny) emit countChanged();
}

void PpcpOfferController::detachAll()
{
    m_peers.clear();
    if (m_rows.isEmpty()) return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit countChanged();
}

void PpcpOfferController::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

int PpcpOfferController::indexOf(const QString &peerId, const QString &sessionId) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).peerId == peerId && m_rows.at(i).sessionId == sessionId) return i;
    return -1;
}

void PpcpOfferController::observe(const QString &peerId, const ppcp_event &ev)
{
    if (!ev.msg) return;
    switch (ev.kind) {
    case PPCP_EVENT_SESSION_OFFER: {
        const ppcp_body_session_offer &o = ev.msg->body.session_offer;
        ++m_offersSeen;

        Row r;
        r.sessionId     = idToString(o.session_id);
        r.peerId        = peerId;
        r.mintingPeerId = idToString(o.minting_peer_id);
        r.hasEpoch      = o.epoch.present;
        r.epochWallUtcNs = o.epoch.present ? o.epoch.wall_utc_ns : 0;
        r.completeness  = o.completeness;
        r.hasBytes      = o.has_bytes_estimate;
        r.bytesEstimate = o.bytes_estimate;
        r.msgId         = ev.msg->env.msg_id;
        if (m_ledger)
            // I34 — "re-import of a session already held is a no-op, never a
            // duplicate".  Shown on the row so the user is told before they
            // press, rather than after a replay that changed nothing.
            r.alreadyHeld = m_ledger->holdsSession(r.mintingPeerId.toStdString(),
                                                   r.sessionId.toStdString());

        const int at = indexOf(r.peerId, r.sessionId);
        if (at >= 0) {
            // A second offer for the same Session REPLACES the first.  MSG 3.3a
            // makes a `declare` a complete snapshot and 9.1 follows the same
            // shape: the device is stating what it holds now, and holding both
            // would present two rows for one Session with different sizes.
            m_rows[at] = r;
            const QModelIndex ix = index(at);
            emit dataChanged(ix, ix);
        } else {
            beginInsertRows({}, m_rows.size(), m_rows.size());
            m_rows.push_back(r);
            endInsertRows();
            emit countChanged();
        }
        setStatus(tr("%1 offered %n session(s)", "", static_cast<int>(m_rows.size()))
                      .arg(peerId));
        break;
    }
    case PPCP_EVENT_SESSION_MANIFEST: {
        // ENC 7c — the manifest is the frame a bundle MUST carry, and its
        // arrival is how the host knows the replay has actually begun.  Nothing
        // is stored: the ledger is filled by the ingest path, not from here.
        const ppcp_body_session_manifest &m = ev.msg->body.session_manifest;
        setStatus(tr("Receiving %1 — %n capture(s)", "",
                     static_cast<int>(m.capture_count))
                      .arg(idToString(m.session_id)));
        break;
    }
    default:
        break;
    }
}

bool PpcpOfferController::sendAccept(int row, ppcp_offer_verdict verdict, const QString &reason)
{
    if (row < 0 || row >= m_rows.size()) { setStatus(tr("That session is no longer offered.")); return false; }
    Row &r = m_rows[row];
    // The link this offer ARRIVED on, which with several phones connected is
    // not necessarily the one that spoke most recently.
    ppcp_peer *peer = m_peers.value(r.peerId, nullptr);
    if (!peer) { setStatus(tr("The device is no longer connected.")); return false; }

    ppcp_body_session_accept acc{};
    if (ppcp_id_set(&acc.session_id, r.sessionId.toUtf8().constData(),
                    static_cast<std::size_t>(r.sessionId.toUtf8().size())) != PPCP_OK) {
        setStatus(tr("That session id cannot be expressed on the wire."));
        return false;
    }
    acc.verdict = verdict;
    if (!reason.isEmpty()) {
        const QByteArray rb = reason.toUtf8();
        if (ppcp_id_set(&acc.reason, rb.constData(), static_cast<std::size_t>(rb.size()))
            == PPCP_OK)
            acc.has_reason = true;
    }

    m_lastAcceptDigests = 0;
    m_lastAcceptDropped = 0;
    if (verdict == PPCP_OFFER_ACCEPT && m_ledger) {
        // 9.1a — keyed on the MINTING peer, because 8.5c scopes Capture
        // identity by the peer that minted it and not by whoever is handing
        // over the bytes.  A device relaying another peer's Session would
        // otherwise be told we hold nothing.
        const std::vector<ppcp_digest> held =
            m_ledger->heldDigests(r.mintingPeerId.toStdString(), r.sessionId.toStdString());
        for (const ppcp_digest &d : held) {
            if (acc.have_digest_count >= PPCP_MAX_HAVE_DIGESTS) { ++m_lastAcceptDropped; continue; }
            acc.have_digests[acc.have_digest_count++] = d;
        }
        m_lastAcceptDigests = acc.have_digest_count;
    }

    // ENC 5b — every response carries `reply_to`, and this is the offer's own
    // `msg_id`.  Without it a device offering several Sessions cannot tell
    // which offer was answered.
    const ppcp_result res = ppcp_peer_session_accept(peer, &acc, r.msgId);
    if (res != PPCP_OK) {
        setStatus(tr("The device refused the request (%1).")
                      .arg(QString::fromLatin1(ppcp_result_str(res))));
        return false;
    }
    ++m_acceptsSent;
    r.accepted = (verdict == PPCP_OFFER_ACCEPT);
    const QModelIndex ix = index(row);
    emit dataChanged(ix, ix);
    if (verdict == PPCP_OFFER_ACCEPT) {
        setStatus(tr("Receiving %1…").arg(r.sessionId));
        emit offerAccepted(r.peerId, r.sessionId);
    }
    return true;
}

bool PpcpOfferController::acceptOffer(int row)
{
    return sendAccept(row, PPCP_OFFER_ACCEPT, {});
}

bool PpcpOfferController::refuseOffer(int row, const QString &reason)
{
    if (row < 0 || row >= m_rows.size()) return false;
    // I34 again: where the ledger already holds it, `already_held` is the
    // truthful verdict and saves a replay that would have been a no-op.
    const ppcp_offer_verdict v = m_rows.at(row).alreadyHeld ? PPCP_OFFER_ALREADY_HELD
                                                            : PPCP_OFFER_REFUSE;
    return sendAccept(row, v, reason);
}
