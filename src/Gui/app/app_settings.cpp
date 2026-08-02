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

#include "app_settings.h"

#include <QDirIterator>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>

StorageInfo AppSettings::queryStorageInfo() const
{
    StorageInfo info;
    const QString path = athleteLibraryPath();
    if (path.isEmpty()) return info;

    QStorageInfo si(path);
    if (si.isValid()) {
        info.totalBytes = si.bytesTotal();
        info.freeBytes  = si.bytesAvailable();
        info.volumeName = si.displayName();
    }

    // Whatever the last completed scan found. -1 until one has. NOT recomputed here — see the
    // header: this function is called from a QML binding at component completion, and the walk it
    // used to do turned every launch into a five-to-ten-second black window on a network library.
    info.sessionBytes = m_sessionBytes;

    return info;
}

void AppSettings::refreshSessionBytes()
{
    if (m_sessionBytesScanning)
        return;                          // already measuring; the answer would be the same
    const QString path = athleteLibraryPath();
    if (path.isEmpty())
        return;

    m_sessionBytesScanning = true;
    emit sessionBytesChanged();

    auto abort = std::make_shared<std::atomic_bool>(false);
    m_sessionBytesAbort = abort;

    // Disconnect-then-connect: the watcher is reused across scans (library path changes, a manual
    // refresh), and a stale connection would deliver an old finish into the new one.
    m_sessionBytesWatcher.disconnect();
    connect(&m_sessionBytesWatcher, &QFutureWatcher<qint64>::finished, this, [this] {
        // A cancelled scan reports nothing rather than a partial sum: half a library measured is
        // not a smaller library, and showing it as one would be a wrong number where the honest
        // answer is the previous one.
        const qint64 v = m_sessionBytesWatcher.result();
        m_sessionBytesScanning = false;
        if (v >= 0)
            m_sessionBytes = v;
        emit sessionBytesChanged();
    });

    m_sessionBytesWatcher.setFuture(QtConcurrent::run([path, abort]() -> qint64 {
        QDirIterator it(path, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        qint64 total = 0;
        int    n     = 0;
        while (it.hasNext()) {
            it.next();
            total += it.fileInfo().size();
            // Polled rather than checked every entry: on a network share the abort flag is far
            // cheaper than the stat beside it, but the loop runs tens of thousands of times and
            // there is no reason to make quitting more responsive than one batch of files.
            if ((++n & 0xFF) == 0 && abort->load())
                return -1;
        }
        return total;
    }));
}

AppSettings::~AppSettings()
{
    if (m_sessionBytesAbort)
        m_sessionBytesAbort->store(true);
    // The lambda captures `path` and `abort` BY VALUE and touches no member, so it is safe on its
    // own — but the watcher is a member and would be destroyed under a running future. Waiting is
    // bounded by one poll interval because of the abort above, not by the length of the walk.
    m_sessionBytesWatcher.disconnect();
    if (m_sessionBytesWatcher.isRunning())
        m_sessionBytesWatcher.waitForFinished();
}
