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

#include "session_review_controller.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include "../../Export/swing_paths.h"
#include <QTime>
#include <QTimer>
#include <QUrl>

#include "app_settings.h"
#include "athlete_controller.h"
#include "session_summary.h"
#include "../Core/pp_debug.h"
#include "../Export/swing_doc.h"

namespace {

// thumbnail absolute paths → file:// URL strings for QML Image sources.
QStringList toThumbUrls(const QStringList &paths)
{
    QStringList urls;
    urls.reserve(paths.size());
    for (const QString &p : paths)
        urls.append(p.isEmpty() ? QString() : QUrl::fromLocalFile(p).toString());
    return urls;
}

} // namespace

SessionReviewController::SessionReviewController(ShotListModel     *liveModel,
                                                 AppSettings       *settings,
                                                 AthleteController *athlete,
                                                 QObject           *parent)
    : QObject(parent)
    , m_liveModel(liveModel)
    , m_settings(settings)
    , m_athlete(athlete)
{
    // The loaded session's shot count tracks trashing in the review model.
    connect(&m_reviewModel, &ShotListModel::activeCountChanged,
            this, &SessionReviewController::activeShotCountChanged);
    refresh();
}

QString SessionReviewController::liveSessionDir() const
{
    if (!m_liveModel)
        return {};
    const int n = m_liveModel->rowCount();
    for (int r = 0; r < n; ++r) {
        const QString sd = m_liveModel->data(m_liveModel->index(r),
                                             ShotListModel::SwingDirRole).toString();
        if (!sd.isEmpty())
            return QFileInfo(sd).absolutePath();
    }
    return {};
}

SessionListModel::Row SessionReviewController::buildLiveRow(qint64 nowMs) const
{
    QVector<pinpoint::ShotSummaryInput> ins;
    if (m_liveModel) {
        const QDate today = QDateTime::fromMSecsSinceEpoch(nowMs).date();
        const int n = m_liveModel->rowCount();
        for (int r = 0; r < n; ++r) {
            const QModelIndex idx = m_liveModel->index(r);
            pinpoint::ShotSummaryInput in;
            in.score    = m_liveModel->data(idx, ShotListModel::ScoreRole).toInt();
            in.hasVideo = m_liveModel->data(idx, ShotListModel::HasVideoRole).toBool();
            in.club     = m_liveModel->data(idx, ShotListModel::ClubRole).toString();
            const QUrl thumb = m_liveModel->data(idx, ShotListModel::ThumbnailSourceRole).toUrl();
            in.thumbnailPath = thumb.isLocalFile() ? thumb.toLocalFile() : thumb.toString();
            // Live shots carry only hh:mm:ss — combine with today for an instant.
            const QTime t = QTime::fromString(
                m_liveModel->data(idx, ShotListModel::TimestampLabelRole).toString(),
                QStringLiteral("hh:mm:ss"));
            if (t.isValid())
                in.wallclockMs = QDateTime(today, t).toMSecsSinceEpoch();
            ins.append(in);
        }
    }
    const pinpoint::SessionSummary sum = pinpoint::summarizeSession(ins, nowMs);

    SessionListModel::Row row;
    row.sessionId     = QString();           // sentinel — selecting the live row resumes live
    row.isLive        = true;
    row.dayLabel      = QStringLiteral("Today");
    row.timeLabel     = sum.timeLabel;
    row.clubMix       = sum.clubMix;
    row.shotCount     = sum.shotCount;
    row.lengthLabel   = sum.lengthLabel;
    row.avgQuality    = sum.avgQuality;
    row.previewThumbs = toThumbUrls(sum.previewThumbs);
    return row;
}

SessionListModel::Row SessionReviewController::buildDiskRow(const QString &sessionDir,
                                                            qint64 nowMs) const
{
    const QStringList swingDirs = pinpoint::SwingDocReader::findSwingDirs(sessionDir);

    // Summary sidecars ONLY — writeSidecar=false forbids the fat swing.json parse. This
    // runs on the GUI thread every time the picker opens, and the documents behind a
    // session run to hundreds of MB; parsing them here is what used to freeze the app long
    // enough for the compositor to offer to kill it.
    QVector<pinpoint::ShotSummaryInput> ins;
    bool indexed = true;
    for (const QString &sd : swingDirs) {
        const pinpoint::SwingSummary s =
            pinpoint::SwingDocReader::readSwingSummary(sd, /*writeSidecar=*/false);
        if (!s.ok) { indexed = false; continue; }
        ins.append({ s.score, s.hasVideo, s.wallclockMs, s.club, s.thumbnailPath });
    }
    const pinpoint::SessionSummary sum = pinpoint::summarizeSession(ins, nowMs);

    SessionListModel::Row row;
    row.sessionId     = QFileInfo(sessionDir).absoluteFilePath();
    row.isLive        = false;
    row.indexed       = indexed;
    row.dayLabel      = sum.dayLabel;
    row.timeLabel     = sum.timeLabel;
    row.clubMix       = sum.clubMix;
    row.shotCount     = sum.shotCount;
    row.lengthLabel   = sum.lengthLabel;
    row.avgQuality    = sum.avgQuality;
    row.previewThumbs = toThumbUrls(sum.previewThumbs);

    if (!indexed) {
        // Nothing (or not everything) indexed yet. Fall back to facts a directory listing
        // and a stat() can supply, so the row stays recognisable and clickable; opening it
        // indexes the session and the next refresh fills in the rest.
        row.shotCount = swingDirs.size();
        if (row.dayLabel.isEmpty()) {
            // Prefer the date the session folder is named for. The dir's mtime is its LAST
            // write (the final swing), while an indexed row's dayLabel comes from the
            // EARLIEST shot — those disagree across midnight, and the label would then
            // change as the session finished indexing. The naming pattern is
            // user-configurable, so fall back to the mtime when there is no leading date.
            const QDate named =
                QDate::fromString(QFileInfo(sessionDir).fileName().left(10),
                                  QStringLiteral("yyyy-MM-dd"));
            const qint64 anchorMs =
                named.isValid() ? QDateTime(named, QTime(0, 0)).toMSecsSinceEpoch()
                                : QFileInfo(sessionDir).lastModified().toMSecsSinceEpoch();
            row.dayLabel = pinpoint::relativeDayLabel(anchorMs, nowMs);
            // Leave timeLabel blank: the drawer renders "day · time" only when time is
            // non-empty, so an unknown fact reads honestly instead of showing a dir mtime
            // that is not the session's start.
        }
    }
    return row;
}

void SessionReviewController::refresh()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    QVector<SessionListModel::Row> rows;
    rows.append(buildLiveRow(nowMs));        // live session pinned first

    const QString liveDir = liveSessionDir();
    const QString root    = m_settings ? m_settings->athleteLibraryPath() : QString();
    const QString athlete = m_athlete  ? m_athlete->currentName()         : QString();
    for (const QString &sessionDir : pinpoint::SwingDocReader::sessionDirs(root, athlete)) {
        // Skip the live session's own dir — it's already the synthesized row.
        if (!liveDir.isEmpty() && QFileInfo(sessionDir).absoluteFilePath() == liveDir)
            continue;
        rows.append(buildDiskRow(sessionDir, nowMs));
    }
    m_sessionsModel.setRows(rows);
}

void SessionReviewController::loadSession(const QString &sessionId)
{
    if (sessionId.isEmpty()) {            // the live row's sentinel id
        resumeLive();
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_reviewModel.clear();

    QVector<pinpoint::ShotSummaryInput> ins;
    for (const QString &sd : pinpoint::SwingDocReader::findSwingDirs(sessionId)) {
        // The full parse is unavoidable here: analysisDetail carries the pose2d keypoint
        // track the replay overlays read. Since the document is in hand anyway, index it —
        // the sidecar costs one small write and is what keeps the picker cheap from now on.
        const pinpoint::PersistedShot ps = pinpoint::SwingDocReader::readSwingJson(sd);
        if (!ps.ok)
            continue;
        pinpoint::SwingDocReader::writeSwingSummary(ps);
        m_reviewModel.addPersistedShot(
            ps.swingDir, ps.ordinal, ps.timestampLabel, ps.club, ps.hasVideo,
            ps.thumbnailPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(ps.thumbnailPath),
            ps.score, ps.rating, ps.note, ps.metrics, ps.analysisDetail, ps.dataWarning,
            ps.lmDeviceKind);
        ins.append({ ps.score, ps.hasVideo, ps.wallclockMs, ps.club, ps.thumbnailPath });
    }

    const pinpoint::SessionSummary sum = pinpoint::summarizeSession(ins, nowMs);
    m_activeDayLabel   = sum.dayLabel;
    m_activeTimeLabel  = sum.timeLabel;
    m_activeClubMix    = sum.clubMix;
    m_loadedSessionDir = QFileInfo(sessionId).absoluteFilePath();

    m_reviewActive = true;
    emit reviewActiveChanged();          // also re-evaluates the activeDay/Time/ClubMix bindings
    emit activeShotCountChanged();

    // The loop above just indexed this session, so its row can now show full detail
    // instead of the shot count alone. Cheap — refresh() reads sidecars only — but it
    // resets the sessions model, which destroys the drawer's delegates. loadSession() is
    // called FROM one of those delegates' click handlers (PpSessionDrawer.qml), so run it
    // on the next event-loop turn rather than pulling the delegate out from under itself.
    QTimer::singleShot(0, this, [this] { refresh(); });
}

void SessionReviewController::onAthleteChanged()
{
    if (m_reviewActive)
        resumeLive();                    // the loaded session belongs to the previous athlete
    refresh();                           // sidecar-only, so this is safe to run inline
}

void SessionReviewController::resumeLive()
{
    if (!m_reviewActive)
        return;
    m_reviewActive = false;
    m_activeDayLabel.clear();
    m_activeTimeLabel.clear();
    m_activeClubMix.clear();
    m_loadedSessionDir.clear();
    m_reviewModel.clear();
    emit reviewActiveChanged();
    emit activeShotCountChanged();
}

QVariantList SessionReviewController::swingDirsForSession(const QString &sessionId) const
{
    QVariantList dirs;
    if (sessionId.isEmpty())             // the live row has no on-disk session
        return dirs;
    for (const QString &sd : pinpoint::SwingDocReader::findSwingDirs(sessionId))
        dirs.append(sd);
    return dirs;
}

bool SessionReviewController::trashSession(const QString &sessionId)
{
    if (sessionId.isEmpty())             // the live row has no on-disk session
        return false;
    // Move the whole session folder to the OS trash (recoverable there). On
    // failure keep everything in place — the files are still on disk.
    // ⚠ SwingPaths::trashPath, not QFile::moveToTrash — see its note: a
    // network share has no OS trash, and this is where every session delete
    // on the SwingData share was failing (2 Sept 2026).
    if (!pinpoint::SwingPaths::trashPath(sessionId)) {
        ppWarn() << "[SessionReviewController] could not move session to trash:" << sessionId;
        return false;
    }
    // If we just trashed the session being reviewed, drop back to live so the
    // carousel isn't left pointing at files that are no longer there.
    if (m_reviewActive && QFileInfo(sessionId).absoluteFilePath() == m_loadedSessionDir)
        resumeLive();
    refresh();                           // the trashed row drops out of the list
    return true;
}
