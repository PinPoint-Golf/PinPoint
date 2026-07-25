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

#include "reanalysis_controller.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrent>

#include "../Export/swing_doc.h"
#include "../Core/pp_debug.h"
#include "../Core/pp_os_metrics.h"

ReanalysisController::ReanalysisController(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcherBase::finished,
            this, &ReanalysisController::onWorkerFinished);
}

void ReanalysisController::reanalyse(const QVariantList &swingDirs)
{
    if (swingDirs.isEmpty()) {
        ppInfo() << "[Reanalysis] reanalyse() called with no shots — ignored";
        return;
    }

    // The caller passed already-resolved on-disk swing dirs (analysis-only shots
    // with no folder are dropped by swingDirsForIds). De-dupe against what's
    // already queued / in flight.
    QStringList dirs;
    for (const QVariant &v : swingDirs) {
        const QString d = v.toString();
        if (!d.isEmpty() && d != m_current && !m_queue.contains(d) && !dirs.contains(d))
            dirs << d;
    }
    if (dirs.isEmpty()) {
        ppInfo() << "[Reanalysis] nothing to re-analyse (no on-disk swings)";
        return;
    }

    m_queue += dirs;
    ppInfo() << "[Reanalysis] queued" << dirs.size() << "swing(s);" << m_queue.size() << "pending";
    emit reanalyseQueued(dirs.size());

    if (!m_reanalysing) {
        m_succeeded = 0;
        m_failed    = 0;
        m_lastError.clear();
        startNext();
    }
}

void ReanalysisController::startNext()
{
    if (m_queue.isEmpty()) {
        m_current.clear();
        m_startDeferred = false;
        setReanalysing(false);
        ppInfo() << "[Reanalysis] batch done —" << m_succeeded << "ok," << m_failed << "failed";
        emit reanalyseFinished(m_succeeded, m_failed, m_lastError);
        return;
    }

    // Hold the batch BETWEEN swings while a live shot is processing — never start a
    // second ViTPose alongside the live one. setLiveBusy(false) resumes it.
    if (m_liveBusy) {
        m_startDeferred = true;
        setReanalysing(true);   // batch is still in progress, just paused
        ppInfo() << "[Reanalysis] deferring next swing — live shot processing";
        return;
    }

    setReanalysing(true);
    m_current = m_queue.takeFirst();
    const QString dir = m_current;
    // One swing at a time: the analyzer's ViTPose pass runs at physical-core thread
    // count and streams a window from disk — sequential keeps memory/CPU bounded.
    // fullWindow stays OFF: it existed to compensate for the grip-speed bs0 lag
    // (span started mid-takeaway, so bounded re-analysis missed real swing) —
    // onset v2 fixed that at source, so re-analysis follows the same span-bounded
    // two-pass path as a live shot. ReanalyzeOptions::fullWindow remains as an
    // escape hatch for SwingLab/debugging only.
    m_watcher.setFuture(QtConcurrent::run([dir] {
        pinpoint::osmetrics::ThreadScope _tscope("Analysis.Worker");
        return pinpoint::analysis::reanalyzeSwingDir(dir, pinpoint::analysis::ReanalyzeOptions{});
    }));
}

void ReanalysisController::onWorkerFinished()
{
    const pinpoint::analysis::ReanalyzeResult r = m_watcher.result();
    const QString dir = m_current;
    m_current.clear();

    if (r.ok && r.analysis.detail) {
        // Write the fresh analysis back, preserving the existing manifest blocks
        // (capture / streams / review). writeSwingJson replaces only "analysis".
        QJsonObject manifest;
        QFile f(dir + QStringLiteral("/swing.json"));
        if (f.open(QIODevice::ReadOnly)) {
            manifest = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
        }
        // Guard against clobbering: an empty manifest means swing.json went missing
        // or unreadable between load and write-back (the folder was trashed mid-batch,
        // or a transient read failure). Writing then would replace the whole document
        // with only {schema, analysis}, destroying streams/capture/review. Skip it.
        QString err;
        if (manifest.isEmpty()) {
            ++m_failed;
            m_lastError = QStringLiteral("This shot's swing.json could not be read, so it was left untouched.");
            ppWarn() << "[Reanalysis] write-back skipped —" << dir
                     << "swing.json missing/unreadable (not overwriting)";
        } else if (pinpoint::SwingDocWriter::writeSwingJson(dir, manifest,
                                                            r.analysis.detail.get(), &err)) {
            ++m_succeeded;
            emit reanalysed(dir);   // host refreshes the row in the active model
            ppInfo() << "[Reanalysis] re-analysed" << dir << "— score" << r.analysis.score;
        } else {
            ++m_failed;
            m_lastError = QStringLiteral("Couldn't save the re-analysis: %1").arg(err);
            ppError() << "[Reanalysis] write-back failed for" << dir << ":" << err;
        }
    } else {
        ++m_failed;
        m_lastError = r.error.isEmpty()
                          ? QStringLiteral("This shot couldn't be re-analysed.")
                          : r.error;
        ppWarn() << "[Reanalysis] failed for" << dir << ":" << m_lastError;
    }

    startNext();
}

void ReanalysisController::setLiveBusy(bool busy)
{
    if (m_liveBusy == busy)
        return;
    m_liveBusy = busy;
    // Live shot finished and a start was held off → resume the batch now.
    if (!busy && m_startDeferred && m_current.isEmpty()) {
        m_startDeferred = false;
        startNext();
    }
}

void ReanalysisController::setReanalysing(bool on)
{
    if (m_reanalysing == on)
        return;
    m_reanalysing = on;
    emit reanalysingChanged();
}
