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

#include "gcquad_monitor.h"

#include "gcquad_csv_parser.h"

#include "pp_debug.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace pinpoint::lm {

GcQuadMonitor::GcQuadMonitor(QObject *parent) : LaunchMonitorBase(parent)
{
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &GcQuadMonitor::pollNow);
}

QString GcQuadMonitor::sourceDescription() const
{
    if (m_dir.isEmpty())
        return QString();
    const QString resolved = resolveFilePath();
    return resolved.isEmpty() ? QDir(m_dir).filePath(fileName()) : resolved;
}

void GcQuadMonitor::setSourcePath(const QString &path)
{
    if (m_dir == path)
        return;
    m_dir = path;
    // A new directory is a new baseline: whatever is sitting in it belongs to
    // whoever was using it before us.
    m_primed = false;
    m_seenBytes.clear();
    m_seenShotId.clear();
    if (m_running)
        start();
}

void GcQuadMonitor::setPollIntervalMs(int ms)
{
    m_intervalMs = qBound(50, ms, 10'000);
    if (m_timer.isActive())
        m_timer.start(m_intervalMs);
}

void GcQuadMonitor::start()
{
    m_running = true;

    if (m_dir.isEmpty()) {
        m_timer.stop();
        setState(State::Disabled);
        return;
    }
    if (!QDir(m_dir).exists()) {
        m_timer.stop();
        setState(State::Error, tr("Folder not found: %1").arg(m_dir));
        return;
    }

    primeWatermark();
    m_timer.start(m_intervalMs);
}

void GcQuadMonitor::stop()
{
    m_running = false;
    m_timer.stop();
    setState(State::Disabled);
}

QString GcQuadMonitor::resolveFilePath() const
{
    if (m_dir.isEmpty())
        return QString();

    const QDir dir(m_dir);
    const QString direct = dir.filePath(fileName());
    if (QFileInfo::exists(direct))
        return direct;

    // Case-sensitive filesystem: FSX2020 writes "LastShot.CSV", our default spelling
    // is "LastShot.csv", and a share may present either. Scan rather than guess.
    const QString wanted = fileName().toLower();
    const QStringList entries = dir.entryList(QDir::Files | QDir::Readable);
    for (const QString &e : entries)
        if (e.toLower() == wanted)
            return dir.filePath(e);

    return QString();
}

void GcQuadMonitor::primeWatermark()
{
    const QString path = resolveFilePath();
    if (path.isEmpty()) {
        m_primed = true;                    // nothing to baseline against; the next write is new
        setState(State::Waiting, QString());
        return;
    }

    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = f.readAll();
        f.close();
        if (const auto r = parseLastShotCsv(bytes)) {
            m_seenBytes  = bytes;
            m_seenShotId = r->deviceShotId;
        }
    }

    m_primed = true;
    // Reaching a parseable file proves the configured path is right, which is the
    // question the settings panel is actually asking. It says nothing about whether
    // this shot is ours — and it will not be claimed.
    setState(m_seenShotId.isEmpty() ? State::Waiting : State::Ready, QString());
}

void GcQuadMonitor::pollNow()
{
    if (!m_running || m_dir.isEmpty())
        return;

    const QString path = resolveFilePath();
    if (path.isEmpty()) {
        if (!QDir(m_dir).exists())
            setState(State::Error, tr("Folder not found: %1").arg(m_dir));
        else if (state() != State::Ready)
            setState(State::Waiting, QString());
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setState(State::Error, tr("Cannot read %1").arg(path));
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    // Byte-identical to what we last accepted: nothing has happened. A `touch` lands
    // here — the mtime moved and the shot did not — and so does FSX2020 rewriting a row
    // it has already reported. Said out loud because a tester touching the file to
    // simulate a shot would otherwise see nothing at all and have no way to know why.
    if (!bytes.isEmpty() && bytes == m_seenBytes) {
        ppDebug() << "LaunchMonitor: file touched but its contents are identical — same shot"
                  << m_seenShotId
                  << "(to simulate a new one, change a VALUE; the Shot ID alone decides nothing)";
        return;
    }

    QString parseError;
    const auto reading = parseLastShotCsv(bytes, &parseError);
    if (!reading) {
        // Almost always a torn read — we caught FSX2020 mid-rewrite. DO NOT advance
        // the watermark: the next tick must look at this same file again, or the
        // shot is lost for good, the file having been rewritten in place.
        return;
    }

    // THE CONTENT IS THE IDENTITY, NOT THE SHOT ID. Reaching here already means the
    // bytes differ from the last reading we accepted, and different numbers mean a
    // different strike.
    //
    // Keying on the Shot ID instead loses shots, because FSX2020's counter is PER
    // SESSION and restarts: close it, reopen it, and the first shot of the new session
    // can carry an id we have already seen. Same id, different numbers — and an
    // id-keyed rule drops it silently, which is the worst way to lose a shot. The id
    // stays as provenance and as the thing the raw block records; it decides nothing.
    //
    // The case this gives up is FSX2020 restating one shot with ADJUSTED values, which
    // would read as two. Nothing in the file distinguishes that from two real shots, and
    // between inventing a duplicate and dropping a real strike, the duplicate is the one
    // a user can see and delete.
    const bool isNewShot = true;

    m_seenBytes  = bytes;
    m_seenShotId = reading->deviceShotId;
    setState(State::Ready, QString());

    if (!m_primed || !isNewShot)
        return;   // isNewShot is structural — see above; kept so the guard reads whole

    LaunchMonitorReading out = *reading;
    out.deviceKind = kindKey(kind());
    out.sourcePath = path;
    out.readAtMs   = QDateTime::currentMSecsSinceEpoch();
    emit readingAvailable(out);
}

} // namespace pinpoint::lm
