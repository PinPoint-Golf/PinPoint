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

#include "ppcp_clip_filer.h"

#include "../../Core/pp_debug.h"
#include "../../Export/swing_doc.h"
#include "../../Ppcp/ppcp_import_ledger.h"
#include "../../Ppcp/ppcp_import_sink.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

using pinpoint::SwingDocWriter;

namespace {

QString completenessName(ppcp_completeness c)
{
    switch (c) {
    case PPCP_COMPLETE: return QStringLiteral("complete");
    case PPCP_PARTIAL:  return QStringLiteral("partial");
    case PPCP_ABSENT:   return QStringLiteral("absent");
    }
    return {};
}

Ppcp::Completeness ledgerCompleteness(ppcp_completeness c)
{
    switch (c) {
    case PPCP_COMPLETE: return Ppcp::Completeness::Complete;
    case PPCP_PARTIAL:  return Ppcp::Completeness::Partial;
    case PPCP_ABSENT:   return Ppcp::Completeness::Absent;
    }
    return Ppcp::Completeness::Complete;
}

}  // namespace

PpcpClipFiler::PpcpClipFiler(QObject *parent) : QObject(parent) {}

PpcpClipFiler::Shot *PpcpClipFiler::find(const QString &shotId)
{
    for (Shot &s : m_shots)
        if (s.shotId == shotId) return &s;
    return nullptr;
}

PpcpClipFiler::Shot *PpcpClipFiler::awaiting()
{
    // The one shot in flight: the most recent that has neither a folder nor a
    // verdict.  armed() guarantees there is at most one.
    for (auto it = m_shots.rbegin(); it != m_shots.rend(); ++it)
        if (it->swingDir.isEmpty() && !it->abandoned) return &*it;
    return nullptr;
}

void PpcpClipFiler::onCaptureAsked(const QString &shotId, const QString &peerId,
                                   const QString &sourceId, const QString &streamId,
                                   const QString &alias)
{
    Shot *s = find(shotId);
    if (!s) {
        m_shots.push_back(Shot{ shotId, {}, {}, {}, false });
        while (m_shots.size() > kMaxTrackedShots) m_shots.pop_front();
        s = find(shotId);
    }
    if (!s) return;
    s->asked.push_back(Asked{ peerId, sourceId, streamId, alias });
}

void PpcpClipFiler::onSwingReady(const QString &swingDir)
{
    Shot *s = awaiting();
    if (!s || swingDir.isEmpty()) return;
    s->swingDir = swingDir;

    // Record what is OWED before anything has arrived, so a swing whose clip
    // never turns up still says a clip was asked for.  ⚠ These elements carry
    // no `file`, so no reader treats them as playable video.
    for (const Asked &a : s->asked) {
        SwingDocWriter::StreamOrigin o;
        o.transport = QStringLiteral("ppcp");
        o.peerId    = a.peerId;
        o.captureId = QString();          // not minted until the phone answers
        o.streamId  = a.streamId;
        o.transfer  = QStringLiteral("requested");
        QString err;
        if (!SwingDocWriter::updateStreamOrigin(swingDir, a.alias, o, &err))
            ppWarn() << "[ppcp] could not record a pending clip on" << swingDir << "—" << err;
    }

    // Anything that beat the folder here.
    std::vector<PpcpClip> parked;
    parked.swap(s->parked);
    for (const PpcpClip &c : parked) file(*s, c);
}

void PpcpClipFiler::onSwingFailed()
{
    Shot *s = awaiting();
    if (!s) return;
    s->abandoned = true;
    if (!s->parked.empty())
        ppWarn() << "[ppcp]" << s->parked.size()
                 << "clip(s) arrived for a shot that produced no swing folder — discarded";
    s->parked.clear();
}

QString PpcpClipFiler::aliasFor(const Shot &s, const PpcpClip &clip) const
{
    // Matched on the Stream the announce named, falling back to the Source: a
    // device may legally open its own capture Stream, in which case the id is
    // not one we asked for but the Source still is.
    for (const Asked &a : s.asked)
        if (!clip.streamId.isEmpty() && a.streamId == clip.streamId) return a.alias;
    for (const Asked &a : s.asked)
        if (a.peerId == clip.peerId && a.sourceId == clip.sourceId) return a.alias;
    return {};
}

void PpcpClipFiler::onClipReady(const PpcpClip &clip)
{
    if (clip.preview) return;              // never, ever the ring or the library

    // I27 — the Capture's own anchor says which Shot this answers.  Without one
    // there is no swing it can belong to, and guessing from arrival order is
    // exactly what a second phone or a re-request breaks.
    if (clip.shotId.isEmpty()) {
        ppWarn() << "[ppcp] clip" << clip.captureId
                 << "is anchored to no Shot — not filed against any swing";
        return;
    }
    Shot *s = find(clip.shotId);
    if (!s) {
        ppWarn() << "[ppcp] clip" << clip.captureId << "for unknown shot" << clip.shotId
                 << "— nothing here asked for it";
        return;
    }
    if (s->abandoned) {
        ppWarn() << "[ppcp] clip" << clip.captureId << "for shot" << clip.shotId
                 << "— that shot produced no swing folder, so there is nowhere to put it";
        return;
    }
    if (s->swingDir.isEmpty()) {
        // The folder is still 4-11 s away. PARKED, not dropped: dropping here
        // is the whole defect this work exists to remove.
        s->parked.push_back(clip);
        return;
    }
    file(*s, clip);
}

bool PpcpClipFiler::file(Shot &s, const PpcpClip &clip)
{
    const QString alias = aliasFor(s, clip);
    if (alias.isEmpty()) {
        ppWarn() << "[ppcp] clip" << clip.captureId << "matches no Stream asked for on shot"
                 << s.shotId;
        return false;
    }

    SwingDocWriter::StreamOrigin o;
    o.transport    = QStringLiteral("ppcp");
    o.peerId       = clip.peerId;
    o.captureId    = clip.captureId;
    o.streamId     = clip.streamId;
    // ⚠ THE OWNER'S WORD, CARRIED AND NEVER INFERRED (CORE 5.14, I10).
    o.completeness = completenessName(clip.completeness);

    // ⭐ `absent` IS AN ANSWER, NOT A FAILURE.  The golfer hit twice in three
    // seconds, or the ring had rolled: 7.3b makes that a Capture of
    // completeness `absent` with a reason, and the receiver records it rather
    // than retrying or reporting an error.
    if (clip.completeness == PPCP_ABSENT || clip.payload.isEmpty()) {
        o.transfer     = QStringLiteral("absent");
        o.absentReason = clip.absentReason.isEmpty() ? QStringLiteral("outside_buffer")
                                                     : clip.absentReason;
        QString err;
        if (!SwingDocWriter::updateStreamOrigin(s.swingDir, alias, o, &err))
            ppWarn() << "[ppcp] could not record an absent clip —" << err;
        ppInfo() << "[ppcp] no phone video for shot" << s.shotId << "—" << o.absentReason;
        return true;
    }

    if (!m_ledger) return false;

    // I34 — the identity decision, and the ONLY thing that stops a re-arrival
    // duplicating the clip.  Two VideoInputPpcp instances may assemble the same
    // Capture (dispatchEvent broadcasts to every instance bound to the peer), so
    // this is load-bearing on the ordinary path and not only on a replay.
    Ppcp::PpcpImportLedger::CaptureRecord rec;
    rec.key.peerId    = clip.peerId.toStdString();
    rec.key.sessionId = clip.sessionId.toStdString();
    rec.key.captureId = clip.captureId.toStdString();
    rec.completeness  = ledgerCompleteness(clip.completeness);

    const auto admission = m_ledger->admit(rec);
    if (admission == Ppcp::PpcpImportLedger::Admission::AlreadyHeld) {
        ppDebug() << "[ppcp] clip" << clip.captureId << "already held — nothing written";
        return true;
    }
    if (admission == Ppcp::PpcpImportLedger::Admission::DigestConflict) {
        // Same identity, different content. Reported, never merged, never
        // overwritten — the one case that is genuinely an error.
        ppError() << "[ppcp] DIGEST CONFLICT for capture" << clip.captureId
                  << "— the same identity with different content; nothing written";
        return false;
    }

    // ENC 6g / 6h — the extension comes from the CONTAINER and from nothing
    // else.  A clip with no container is refused a name rather than given a
    // guessed one: 6h forbids inferring it from the codec, the Stream kind or
    // the bytes.
    const std::string ext =
        Ppcp::PpcpImportSink::extensionForPayload(clip.container.toStdString(), "video");
    const QString path = s.swingDir + QLatin1Char('/') + alias + QString::fromStdString(ext);

    // QSaveFile: a clip torn in half by a crash would be a file the document
    // names and the replay cannot open.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        ppWarn() << "[ppcp] cannot write clip to" << path << "—" << f.errorString();
        o.transfer = QStringLiteral("failed");
        QString err;
        SwingDocWriter::updateStreamOrigin(s.swingDir, alias, o, &err);
        return false;
    }
    f.write(clip.payload);
    if (!f.commit()) {
        ppWarn() << "[ppcp] cannot commit clip" << path << "—" << f.errorString();
        o.transfer = QStringLiteral("failed");
        QString err;
        SwingDocWriter::updateStreamOrigin(s.swingDir, alias, o, &err);
        return false;
    }

    // ⭐ ONLY NOW.  MSG 8.4a makes `capture_committed` the receiver's statement
    // that it holds the bytes DURABLY — written and flushed, not merely
    // received.  It is queued rather than sent, so a commit lost with a dying
    // link is still owed; and it is not a nicety, because under I38 a device may
    // not evict a Capture that never reached `confirmed`, so a phone used all
    // season fills up and cannot clear itself without it.
    m_ledger->setLocalPath(rec.key, path.toStdString());
    m_ledger->setSwingRef(rec.key,
        Ppcp::SwingRef{ QFileInfo(s.swingDir).dir().dirName().toStdString(),
                        QFileInfo(s.swingDir).fileName().toStdString(),
                        alias.toStdString() });
    m_ledger->queueCommitted(rec.key, {});
    m_ledger->save();
    if (m_pumpCommits) m_pumpCommits();

    o.transfer    = QStringLiteral("complete");
    o.committedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QString err;
    // ⚠ AND THE FILE, or the element says `complete` while naming nothing and no
    // reader can open it.  Relative to the swing folder, as every other stream's
    // `file` is.
    if (!SwingDocWriter::updateStreamOrigin(s.swingDir, alias, o, &err,
                                            QFileInfo(path).fileName()))
        ppWarn() << "[ppcp] clip landed but swing.json was not updated —" << err;

    ppInfo() << "[ppcp] phone video landed:" << path << clip.payload.size() << "bytes, shot"
             << s.shotId;
    emit clipFiled(s.swingDir, alias);
    return true;
}
