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

#include "ppcp_import_ledger.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

namespace Ppcp {
namespace {

QString q(const std::string &s) { return QString::fromStdString(s); }
std::string s(const QString &v) { return v.toStdString(); }

Completeness completenessFrom(const QString &v)
{
    if (v == "complete") return Completeness::Complete;
    if (v == "absent")   return Completeness::Absent;
    return Completeness::Partial;
}

}  // namespace

const char *completenessStr(Completeness c)
{
    switch (c) {
    case Completeness::Complete: return "complete";
    case Completeness::Partial:  return "partial";
    case Completeness::Absent:   return "absent";
    }
    return "partial";
}

PpcpImportLedger::SessionRecord *PpcpImportLedger::findSession(const std::string &peerId,
                                                               const std::string &sessionId)
{
    for (SessionRecord &r : m_sessions)
        if (r.peerId == peerId && r.sessionId == sessionId) return &r;
    return nullptr;
}

const PpcpImportLedger::SessionRecord *PpcpImportLedger::session(
    const std::string &peerId, const std::string &sessionId) const
{
    for (const SessionRecord &r : m_sessions)
        if (r.peerId == peerId && r.sessionId == sessionId) return &r;
    return nullptr;
}

bool PpcpImportLedger::holdsSession(const std::string &peerId,
                                    const std::string &sessionId) const
{
    return session(peerId, sessionId) != nullptr;
}

void PpcpImportLedger::noteSession(const std::string &peerId, const std::string &sessionId,
                                   bool asserted, Completeness assertedValue,
                                   bool bundleTruncated, const std::string &localDir)
{
    SessionRecord *r = findSession(peerId, sessionId);
    if (!r) {
        m_sessions.push_back(SessionRecord{ peerId, sessionId, Completeness::Partial,
                                            false, false, localDir });
        r = &m_sessions.back();
    }
    if (!localDir.empty()) r->localDir = localDir;

    // ENC 7d and I10, in the one direction they run.
    //
    // An ASSERTION by the owner is authoritative and is taken, once. A second
    // bundle for the same Session may assert again; a `partial` never becomes
    // `complete`, because "never upgrades a partial Session to complete on the
    // strength of what happened to be present" is about the receiver's evidence
    // and the receiver has no other kind.
    if (asserted) {
        if (!r->completenessAsserted) {
            r->completeness = assertedValue;
            r->completenessAsserted = true;
        } else if (r->completeness == Completeness::Complete
                   && assertedValue != Completeness::Complete) {
            // A later assertion may DOWNGRADE: the owner learning it lost
            // something is new information about what it holds.
            r->completeness = assertedValue;
        }
        // and an assertion that would upgrade is ignored, deliberately.
        return;
    }

    // Nothing asserted. A truncated final frame is then the only evidence there
    // is, and ENC 7d says what it means: partial — "ONLY IF the bundle itself
    // did not assert otherwise", which is this branch and no other.
    if (bundleTruncated && !r->completenessAsserted) r->completeness = Completeness::Partial;
}

void PpcpImportLedger::closeSession(const std::string &peerId, const std::string &sessionId)
{
    if (SessionRecord *r = findSession(peerId, sessionId)) r->closed = true;
}

const PpcpImportLedger::CaptureRecord *PpcpImportLedger::capture(const CaptureKey &k) const
{
    for (const CaptureRecord &r : m_captures)
        if (r.key == k) return &r;
    return nullptr;
}

bool PpcpImportLedger::holds(const CaptureKey &k) const { return capture(k) != nullptr; }

PpcpImportLedger::Admission PpcpImportLedger::admit(const CaptureRecord &rec)
{
    // I34 — the key is the three ids and NOTHING ELSE. A Capture with no digest
    // (a `complete` + `pending` clip whose hash was not computed before the
    // bundle was written, which MSG 8.1e deliberately permits) and a Capture
    // with no payload at all (`completeness: absent`) both have a full identity
    // here, which is the entire point of the Draft 3 correction.
    for (CaptureRecord &held : m_captures) {
        if (!(held.key == rec.key)) continue;

        // `digest` is a CONTENT check where present, not the key. Two records
        // with one identity and two digests is a genuine conflict — one of the
        // two files is not what it says it is — and 8.5a/8.5b forbid resolving
        // it here: "reconciliation creates LINKS.  No entity is rewritten or
        // merged" (I9), and "an implementation MUST NOT auto-merge".
        if (!rec.digestHex.empty() && !held.digestHex.empty()
            && rec.digestHex != held.digestHex)
            return Admission::DigestConflict;

        // A second import may carry a digest the first did not have. Filling in
        // a field that was absent is not a merge and not an upgrade: it is the
        // content check becoming possible.
        if (held.digestHex.empty() && !rec.digestHex.empty()) held.digestHex = rec.digestHex;

        // Completeness is the OWNER's assertion and is not re-derived from what
        // arrived (I10). It is taken only when it downgrades, for the same
        // reason as a Session's.
        if (held.completeness == Completeness::Complete
            && rec.completeness != Completeness::Complete)
            held.completeness = rec.completeness;

        return Admission::AlreadyHeld;
    }

    m_captures.push_back(rec);
    return Admission::Recorded;
}

bool PpcpImportLedger::setLocalPath(const CaptureKey &k, const std::string &path,
                                    const std::string &digestHex)
{
    for (CaptureRecord &held : m_captures) {
        if (!(held.key == k)) continue;
        held.localPath = path;
        // A digest that was absent when the Capture was announced and present
        // by `payload_end` (MSG 8.1e permits exactly that) fills in; one that
        // DISAGREES is left alone, because the content check is admit()'s to
        // make and silently overwriting it would erase the conflict.
        if (held.digestHex.empty() && !digestHex.empty()) held.digestHex = digestHex;
        return true;
    }
    return false;
}

void PpcpImportLedger::queueCommitted(const CaptureKey &k, const std::string &digestHex)
{
    // MSG 8.4a — the receiver says this only when it holds the payload DURABLY,
    // "written and flushed, not merely received". Whether that is true is the
    // caller's to know; the ledger's job is that the message is not forgotten
    // between now and the owner's next connection.
    for (const PendingCommit &p : m_pending)
        if (p.key == k) return;   // owed once, not once per import
    m_pending.push_back(PendingCommit{ k, digestHex });
}

std::vector<PpcpImportLedger::PendingCommit> PpcpImportLedger::pendingCommits(
    const std::string &owningPeerId) const
{
    std::vector<PendingCommit> out;
    for (const PendingCommit &p : m_pending)
        if (p.key.peerId == owningPeerId) out.push_back(p);

    // ⚠ 5.14h1 — NOTHING HERE FILTERS ON SESSION STATE. "A `capture_committed`
    // naming a Session whose `state` is `closed` is ACCEPTED, not answered
    // `unknown_session`.  It may arrive days after the bundle was imported, and
    // releasing storage is the one operation that stays legitimate after a
    // Session closes."  A ledger that quietly dropped commits for closed
    // sessions would leave the owner unable to evict for exactly the sessions
    // most likely to be finished with.
    return out;
}

void PpcpImportLedger::clearCommitted(const CaptureKey &k)
{
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it->key == k) {
            m_pending.erase(it);
            return;
        }
    }
}

// ── Persistence ─────────────────────────────────────────────────────────────
// A sidecar beside the athlete library, in the same JSON idiom as swing.json.
// It is NOT a second schema for the session data — the session data is the
// bundle and the swing folders — it is only the record of what has been taken
// in, which is the one thing neither of those can carry.

bool PpcpImportLedger::seedIndex(ppcp_capture_index *ix, std::size_t *outDropped) const
{
    if (!ix) return false;
    ppcp_capture_index_init(ix);
    std::size_t dropped = 0;
    for (const CaptureRecord &r : m_captures) {
        ppcp_capture_key k{};
        bool isNew = false;
        if (ppcp_id_set_z(&k.session_id, r.key.sessionId.c_str()) != PPCP_OK
            || ppcp_id_set_z(&k.peer_id, r.key.peerId.c_str()) != PPCP_OK
            || ppcp_id_set_z(&k.capture_id, r.key.captureId.c_str()) != PPCP_OK) {
            ++dropped;
            continue;
        }
        if (ppcp_capture_index_observe(ix, &k, &isNew) != PPCP_OK) ++dropped;
    }
    if (outDropped) *outDropped = dropped;
    return dropped == 0;
}

bool PpcpImportLedger::load(const std::string &path)
{
    m_path = path;
    m_sessions.clear();
    m_captures.clear();
    m_pending.clear();

    QFile f(q(path));
    if (!f.exists()) return true;     // an empty ledger is a valid ledger
    if (!f.open(QIODevice::ReadOnly)) return false;

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();

    for (const QJsonValue &v : root.value("sessions").toArray()) {
        const QJsonObject o = v.toObject();
        SessionRecord r;
        r.peerId = s(o.value("peer_id").toString());
        r.sessionId = s(o.value("session_id").toString());
        r.completeness = completenessFrom(o.value("completeness").toString());
        r.completenessAsserted = o.value("asserted").toBool();
        r.closed = o.value("closed").toBool();
        r.localDir = s(o.value("dir").toString());
        m_sessions.push_back(r);
    }
    for (const QJsonValue &v : root.value("captures").toArray()) {
        const QJsonObject o = v.toObject();
        CaptureRecord r;
        r.key.peerId = s(o.value("peer_id").toString());
        r.key.sessionId = s(o.value("session_id").toString());
        r.key.captureId = s(o.value("capture_id").toString());
        r.digestHex = s(o.value("digest").toString());
        r.completeness = completenessFrom(o.value("completeness").toString());
        r.localPath = s(o.value("path").toString());
        m_captures.push_back(r);
    }
    for (const QJsonValue &v : root.value("pending_commits").toArray()) {
        const QJsonObject o = v.toObject();
        PendingCommit p;
        p.key.peerId = s(o.value("peer_id").toString());
        p.key.sessionId = s(o.value("session_id").toString());
        p.key.captureId = s(o.value("capture_id").toString());
        p.digestHex = s(o.value("digest").toString());
        m_pending.push_back(p);
    }
    return true;
}

bool PpcpImportLedger::save() const
{
    if (m_path.empty()) return false;

    QJsonArray sessions;
    for (const SessionRecord &r : m_sessions) {
        QJsonObject o;
        o["peer_id"] = q(r.peerId);
        o["session_id"] = q(r.sessionId);
        o["completeness"] = completenessStr(r.completeness);
        o["asserted"] = r.completenessAsserted;
        o["closed"] = r.closed;
        o["dir"] = q(r.localDir);
        sessions.append(o);
    }
    QJsonArray captures;
    for (const CaptureRecord &r : m_captures) {
        QJsonObject o;
        o["peer_id"] = q(r.key.peerId);
        o["session_id"] = q(r.key.sessionId);
        o["capture_id"] = q(r.key.captureId);
        o["digest"] = q(r.digestHex);
        o["completeness"] = completenessStr(r.completeness);
        o["path"] = q(r.localPath);
        captures.append(o);
    }
    QJsonArray pending;
    for (const PendingCommit &p : m_pending) {
        QJsonObject o;
        o["peer_id"] = q(p.key.peerId);
        o["session_id"] = q(p.key.sessionId);
        o["capture_id"] = q(p.key.captureId);
        o["digest"] = q(p.digestHex);
        pending.append(o);
    }

    QJsonObject root;
    root["version"] = 1;
    root["sessions"] = sessions;
    root["captures"] = captures;
    root["pending_commits"] = pending;

    // QSaveFile, not QFile: a ledger torn in half by a crash mid-write would
    // make the next import duplicate everything it could not read, which is the
    // one failure I34 exists to prevent.
    QSaveFile f(q(m_path));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

}  // namespace Ppcp
