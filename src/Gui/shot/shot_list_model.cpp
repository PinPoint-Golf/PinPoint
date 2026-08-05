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

#include "shot_list_model.h"

#include "../Export/swing_doc.h"
#include "../Core/club_vocabulary.h"
#include "../Core/pp_debug.h"

#include <QFile>
#include <QStringList>

ShotListModel::ShotListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ShotListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_shots.size();
}

QVariant ShotListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_shots.size())
        return {};
    const Shot &s = m_shots.at(index.row());
    switch (role) {
    case ShotIdRole:          return s.id;
    case OrdinalRole:         return s.ordinal;
    case TimestampLabelRole:  return s.timestampLabel;
    case ClubRole:            return s.club;
    case HasVideoRole:        return s.hasVideo;
    case ThumbnailSourceRole: return s.thumbnailSource;
    case TracePointsRole:     return s.tracePoints;
    case ScoreRole:           return s.score;
    case RatingRole:          return s.rating;
    case NoteRole:            return s.note;
    case MetricsRole:         return s.metrics;
    case AnalysisDetailRole:  return s.analysisDetail;
    case SwingDirRole:        return s.swingDir;
    case DataWarningRole:     return s.dataWarning;
    case LmDeviceKindRole:    return s.lmDeviceKind;
    default:                  return {};
    }
}

QHash<int, QByteArray> ShotListModel::roleNames() const
{
    return {
        { ShotIdRole,          "shotId"          },
        { OrdinalRole,         "ordinal"         },
        { TimestampLabelRole,  "timestampLabel"  },
        { ClubRole,            "club"            },
        { HasVideoRole,        "hasVideo"        },
        { ThumbnailSourceRole, "thumbnailSource" },
        { TracePointsRole,     "tracePoints"     },
        { ScoreRole,           "score"           },
        { RatingRole,          "rating"          },
        { NoteRole,            "note"            },
        { MetricsRole,         "metrics"         },
        { AnalysisDetailRole,  "analysisDetail"  },
        { SwingDirRole,        "swingDir"        },
        { DataWarningRole,     "dataWarning"     },
        { LmDeviceKindRole,    "lmDeviceKind"    },
    };
}

int ShotListModel::activeCount() const
{
    return m_shots.size();
}

int ShotListModel::rowForId(int id) const
{
    for (int i = 0; i < m_shots.size(); ++i)
        if (m_shots.at(i).id == id)
            return i;
    return -1;
}

int ShotListModel::addShot(const QString &swingDir, const QString &timestampLabel,
                           const QString &club, bool hasVideo, const QUrl &thumbnailSource,
                           const QVariantList &tracePoints, int score,
                           const QVariantMap &metrics,
                           const QVariantMap &analysisDetail, bool dataWarning)
{
    int maxOrdinal = 0;
    for (const Shot &s : m_shots)
        maxOrdinal = std::max(maxOrdinal, s.ordinal);

    Shot shot;
    shot.id              = m_nextId++;
    shot.ordinal         = maxOrdinal + 1;
    shot.swingDir        = swingDir;
    shot.timestampLabel  = timestampLabel;
    shot.club            = club;
    shot.hasVideo        = hasVideo;
    shot.thumbnailSource = thumbnailSource;
    shot.tracePoints     = tracePoints;
    shot.score           = score;
    shot.metrics         = metrics;
    shot.analysisDetail  = analysisDetail;
    shot.dataWarning     = dataWarning;

    // Newest first, matching the mockup's right-of-cap ordering.
    beginInsertRows(QModelIndex(), 0, 0);
    m_shots.prepend(shot);
    endInsertRows();
    emit activeCountChanged();
    return shot.id;
}

void ShotListModel::addPersistedShot(const QString &swingDir, int ordinal,
                                     const QString &timestampLabel, const QString &club,
                                     bool hasVideo, const QUrl &thumbnailSource, int score,
                                     int rating, const QString &note,
                                     const QVariantMap &metrics, const QVariantMap &analysisDetail,
                                     bool dataWarning, const QString &lmDeviceKind)
{
    Shot shot;
    shot.id              = m_nextId++;
    shot.ordinal         = ordinal;          // preserve the on-disk swing index
    shot.swingDir        = swingDir;
    shot.timestampLabel  = timestampLabel;
    shot.club            = club;
    shot.hasVideo        = hasVideo;
    shot.thumbnailSource = thumbnailSource;
    shot.score           = score;
    shot.rating          = rating;           // restored from the "review" block
    shot.note            = note;
    shot.metrics         = metrics;
    shot.analysisDetail  = analysisDetail;
    shot.dataWarning     = dataWarning;
    shot.lmDeviceKind    = lmDeviceKind;

    // Prepend (newest first); callers reload ascending so the highest ordinal lands first.
    beginInsertRows(QModelIndex(), 0, 0);
    m_shots.prepend(shot);
    endInsertRows();
    emit activeCountChanged();
}

void ShotListModel::loadSessionDir(const QString &dir)
{
    // Point the live carousel at a specific session folder — today's, on wrist
    // entry and at session start/end. An empty dir just clears (empty carousel).
    // Never called during live capture; clears then re-seeds from disk truth.
    clear();
    if (dir.isEmpty())
        return;
    for (const QString &sd : pinpoint::SwingDocReader::findSwingDirs(dir)) {
        const pinpoint::PersistedShot ps = pinpoint::SwingDocReader::readSwingJson(sd);
        if (!ps.ok)
            continue;
        // Index while the document is in hand — this is the live session, the one the
        // picker is most likely to be asked about first.
        pinpoint::SwingDocReader::writeSwingSummary(ps);
        addPersistedShot(ps.swingDir, ps.ordinal, ps.timestampLabel, ps.club, ps.hasVideo,
                         ps.thumbnailPath.isEmpty() ? QUrl()
                                                    : QUrl::fromLocalFile(ps.thumbnailPath),
                         ps.score, ps.rating, ps.note, ps.metrics, ps.analysisDetail,
                         ps.dataWarning, ps.lmDeviceKind);
    }
}

void ShotListModel::refreshShot(const QString &swingDir)
{
    if (swingDir.isEmpty())
        return;
    int row = -1;
    for (int i = 0; i < m_shots.size(); ++i) {
        if (m_shots.at(i).swingDir == swingDir) { row = i; break; }
    }
    if (row < 0)
        return;

    const pinpoint::PersistedShot p = pinpoint::SwingDocReader::readSwingJson(swingDir);
    if (!p.ok) {
        ppWarn() << "[ShotListModel] refreshShot: could not read" << swingDir;
        return;
    }

    Shot &s = m_shots[row];
    s.score          = p.score;
    s.metrics        = p.metrics;
    s.analysisDetail = p.analysisDetail;
    // Re-read alongside the readings it belongs to: this is the path a live row takes when
    // a monitor's numbers are folded in, so it is where the row learns which device sent them.
    s.lmDeviceKind   = p.lmDeviceKind;
    emit dataChanged(index(row), index(row),
                     { ScoreRole, MetricsRole, AnalysisDetailRole, LmDeviceKindRole });
}

void ShotListModel::attachSwingDir(int id, const QString &swingDir)
{
    if (swingDir.isEmpty())
        return;
    int row = -1;
    for (int i = 0; i < m_shots.size(); ++i)
        if (m_shots.at(i).id == id) { row = i; break; }
    if (row < 0)
        return;

    Shot &s = m_shots[row];
    if (!s.swingDir.isEmpty() && s.swingDir != swingDir) {
        ppWarn() << "[ShotListModel] attachSwingDir: row" << id
                 << "already points at" << s.swingDir << "— refusing to repoint it at" << swingDir;
        return;
    }

    const pinpoint::PersistedShot p = pinpoint::SwingDocReader::readSwingJson(swingDir);
    if (!p.ok) {
        ppWarn() << "[ShotListModel] attachSwingDir: could not read" << swingDir;
        return;
    }

    s.swingDir       = swingDir;
    s.score          = p.score;
    s.metrics        = p.metrics;
    s.analysisDetail = p.analysisDetail;
    s.club           = p.club;
    s.timestampLabel = p.timestampLabel;
    s.lmDeviceKind   = p.lmDeviceKind;
    emit dataChanged(index(row), index(row),
                     { SwingDirRole, ScoreRole, MetricsRole, AnalysisDetailRole,
                       ClubRole, TimestampLabelRole, LmDeviceKindRole });
}

void ShotListModel::clear()
{
    if (m_shots.isEmpty())
        return;
    beginResetModel();
    m_shots.clear();
    endResetModel();
    emit activeCountChanged();
}

// Persist the row's review (rating + note + club) to its swing.json, if it has
// one on disk. A row with no swingDir (export produced no directory) is in-memory
// only and silently skips the write-through.
void ShotListModel::persistReview(int row)
{
    const Shot &s = m_shots.at(row);
    if (s.swingDir.isEmpty())
        return;
    QString err;
    if (!pinpoint::SwingDocWriter::updateReview(s.swingDir, s.rating, s.note, s.club, &err))
        ppWarn() << "[ShotListModel] review write-through failed:" << err;
}

void ShotListModel::setRating(int id, int n)
{
    const int row = rowForId(id);
    if (row < 0)
        return;
    n = std::clamp(n, 0, 5);
    if (m_shots[row].rating == n)
        return;
    m_shots[row].rating = n;
    emit dataChanged(index(row), index(row), { RatingRole });
    persistReview(row);
}

void ShotListModel::setNote(int id, const QString &text)
{
    const int row = rowForId(id);
    if (row < 0 || m_shots.at(row).note == text)
        return;
    m_shots[row].note = text;
    emit dataChanged(index(row), index(row), { NoteRole });
    persistReview(row);
}

void ShotListModel::setClub(int id, const QString &club)
{
    const int row = rowForId(id);
    if (row < 0 || m_shots.at(row).club == club)
        return;
    m_shots[row].club = club;
    emit dataChanged(index(row), index(row), { ClubRole });
    persistReview(row);
}

QStringList ShotListModel::clubOptions()
{
    // The one canonical bag — shared with the Markup Lab picker (see club_vocabulary.h).
    return pinpoint::clubVocabulary();
}

bool ShotListModel::moveToTrash(int id)
{
    const int row = rowForId(id);
    if (row < 0)
        return false;
    // Move the on-disk folder to the OS trash (recoverable there). An
    // analysis-only shot has no folder and simply drops from the model.
    const QString dir = m_shots.at(row).swingDir;
    if (!dir.isEmpty() && !QFile::moveToTrash(dir)) {
        ppWarn() << "[ShotListModel] could not move to trash:" << dir;
        return false;                 // keep the row — its files are still here
    }
    beginRemoveRows(QModelIndex(), row, row);
    m_shots.removeAt(row);
    endRemoveRows();
    emit activeCountChanged();
    return true;
}

int ShotListModel::moveAllToTrash(const QVariantList &ids)
{
    // Delegate per id — moveToTrash() re-resolves the row each time, so the
    // index shift from each removeAt() can't stale a cached row.
    int moved = 0;
    for (const QVariant &v : ids)
        if (moveToTrash(v.toInt()))
            ++moved;
    return moved;
}

QVariantList ShotListModel::swingDirsForIds(const QVariantList &ids) const
{
    QVariantList dirs;
    for (const QVariant &v : ids) {
        const int row = rowForId(v.toInt());
        if (row < 0)
            continue;
        const QString &dir = m_shots.at(row).swingDir;
        if (!dir.isEmpty())
            dirs.append(dir);
    }
    return dirs;
}

QVariantMap ShotListModel::shotSummary(int id) const
{
    const int row = rowForId(id);
    if (row < 0)
        return { { QStringLiteral("valid"), false } };
    const Shot &s = m_shots.at(row);
    return {
        { QStringLiteral("valid"),          true },
        { QStringLiteral("ordinal"),        s.ordinal },
        { QStringLiteral("club"),           s.club },
        { QStringLiteral("timestampLabel"), s.timestampLabel },
        { QStringLiteral("score"),          s.score },
        { QStringLiteral("rating"),         s.rating },
        { QStringLiteral("note"),           s.note },
        { QStringLiteral("hasVideo"),       s.hasVideo },
        { QStringLiteral("swingDir"),       s.swingDir },
    };
}

QVariantMap ShotListModel::previousAnalysisDetail(const QString &swingDir) const
{
    if (swingDir.isEmpty())
        return {};
    for (int i = 0; i < m_shots.size(); ++i) {
        if (m_shots.at(i).swingDir != swingDir)
            continue;
        const int older = i + 1;               // newest-first → the next row is the previous swing
        return (older < m_shots.size()) ? m_shots.at(older).analysisDetail : QVariantMap{};
    }
    return {};
}

QVariantMap ShotListModel::analysisDetailForSwingDir(const QString &swingDir) const
{
    if (swingDir.isEmpty())
        return {};
    for (const Shot &s : m_shots)
        if (s.swingDir == swingDir)
            return s.analysisDetail;
    // Not in this list is not "does not exist" — see the header. The row is the fast path
    // (it is already parsed); the document is the truth.
    const pinpoint::PersistedShot ps = pinpoint::SwingDocReader::readSwingJson(swingDir);
    return ps.ok ? ps.analysisDetail : QVariantMap{};
}

