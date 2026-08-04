/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "launch_monitor_base.h"

#include <QByteArray>
#include <QString>
#include <QTimer>

namespace pinpoint::lm {

// Reads Foresight's LastShot.CSV out of a configured directory.
//
// The file is written by FSX2020 — the legacy Foresight application, which runs only
// on Windows — shortly after each shot, and is REWRITTEN IN PLACE rather than
// appended to or replaced. This class is deliberately platform-agnostic all the
// same: the directory can be an SMB share from the Windows box, and being able to
// point it at a folder of captured samples is what makes the whole path testable
// off-Windows.
//
// WHY POLLING AND NOT QFileSystemWatcher. Three reasons, any one sufficient. The
// file is rewritten in place, which some platforms report as delete-then-create and
// which loses a watcher's handle. The directory is usually a network share, where
// change notification is unreliable or absent. And a watcher gives no natural place
// to re-read a file that was caught half-written. Re-reading two lines every ~250 ms
// costs nothing next to the analysis running beside it.
class GcQuadMonitor : public LaunchMonitorBase
{
    Q_OBJECT

public:
    explicit GcQuadMonitor(QObject *parent = nullptr);

    Kind    kind() const override { return Kind::GcQuad; }
    QString sourceDescription() const override;

    void start() override;
    void stop() override;
    void setSourcePath(const QString &path) override;
    void setPollIntervalMs(int ms) override;

    // One poll, run directly. The timer calls this; tests call it themselves rather
    // than spinning an event loop.
    void pollNow();

    // The name looked for inside the configured directory, matched case-insensitively
    // (FSX2020 writes "LastShot.CSV"; the docs say "LastShot.csv").
    static QString fileName() { return QStringLiteral("LastShot.csv"); }

private:
    // Locate the file inside m_dir, tolerating case. Empty when it is not there.
    QString resolveFilePath() const;
    // Take the current file as the baseline WITHOUT emitting. Called once on start:
    // FSX2020 leaves its last shot on disk between runs, so a monitor that claimed
    // what it found would hand the first swing of the day yesterday's numbers.
    void primeWatermark();

    QTimer  m_timer;
    QString m_dir;
    int     m_intervalMs = 250;
    bool    m_running    = false;

    // The bytes we last successfully read. Compared whole, as the cheap "nothing has
    // changed at all" early-out.
    //
    // NOT size-and-mtime, which is the obvious gate and is WRONG here. FSX2020
    // rewrites the same columns every shot, so consecutive rows routinely come out
    // byte-identical in length — 38.978607 and 40.000000 occupy the same space — and
    // mtime granularity is a whole second on some filesystems and coarser again over
    // SMB. A shot arriving inside that window would be silently skipped, which is the
    // one failure mode with no visible symptom. The file is two lines; reading it
    // outright four times a second costs nothing worth protecting.
    QByteArray m_seenBytes;
    // The Shot ID we last read. PROVENANCE ONLY — it decides nothing, because FSX2020's
    // counter is per session and restarts, so a genuinely new shot can carry an id we
    // have already seen. Kept for the log line and for the raw block.
    QString    m_seenShotId;
    // True once the baseline has been taken. Until then nothing is ever claimed.
    bool       m_primed = false;
};

} // namespace pinpoint::lm
