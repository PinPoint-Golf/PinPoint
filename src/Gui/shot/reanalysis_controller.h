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

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QVariantList>

#include "../Analysis/swing_reanalyzer.h"   // pinpoint::analysis::ReanalyzeResult

// ReanalysisController — the single funnel for "re-analyse this shot" from the
// carousel's PpShotActionBar (focused shot) and the set-scope menu ("re-analyse
// all shown"). It reloads each exported swing from disk via the STREAMING
// SwingDiskLoader (no all-frames-in-RAM rebuild), re-runs the production analyzer
// on a worker thread ONE SWING AT A TIME (each runs ViTPose at physical-core
// thread count — sequential bounds memory/CPU), and writes the fresh analysis back
// into swing.json (preserving capture/streams/review).
//
// It is MODEL-AGNOSTIC: callers pass already-resolved swing DIRS (the carousel
// resolves them from its active model — live shotModel or a loaded session's
// review model), and the controller emits reanalysed(swingDir) per success so the
// caller refreshes the right model's row. This avoids the trap of resolving ids
// against the wrong model when a past session is under review.
//
// Registered in main.cpp as the QML context property `reanalysisController`. Kept
// separate from ShotProcessor: that owns the strict live-shot state machine that
// gates `armed`; re-analysis uses its own disk-backed window and never touches the
// live ring, so the two are independent (it may run alongside live capture).
class ReanalysisController : public QObject
{
    Q_OBJECT
    // True while a re-analysis batch is in flight — the bar binds it for a spinner.
    Q_PROPERTY(bool reanalysing READ reanalysing NOTIFY reanalysingChanged)

public:
    explicit ReanalysisController(QObject *parent = nullptr);

    bool reanalysing() const { return m_reanalysing; }

    // The one entry point for both carousel paths. Takes the on-disk swing dirs
    // (the caller resolves them from its ACTIVE model via swingDirsForIds), enqueues
    // the new ones, and drains the queue sequentially. Empty list is a no-op.
    Q_INVOKABLE void reanalyse(const QVariantList &swingDirs);

    // A live shot is processing (its own ViTPose pass at physical-core thread
    // count). Wired from ShotProcessor::busyChanged in main.cpp: while true the
    // queue holds BETWEEN swings so re-analysis never starts a second ViTPose
    // alongside the live one (≈2× CPU / OOM risk). An in-flight swing finishes;
    // only the next is deferred.
    Q_INVOKABLE void setLiveBusy(bool busy);

signals:
    void reanalysingChanged();
    // Emitted once per accepted reanalyse() with the number of swings queued — the
    // host turns this into a "re-analysing N shots" toast.
    void reanalyseQueued(int count);
    // Emitted after each swing's fresh analysis is written back — the host refreshes
    // that swing's row in whichever model is showing it (live or review).
    void reanalysed(const QString &swingDir);
    // Emitted when the whole batch drains. lastError is the reason of the last
    // failure (empty if none) — the host shows it verbatim for a single-shot batch
    // and falls back to the "N re-analysed, M failed" count for a multi-shot batch.
    void reanalyseFinished(int succeeded, int failed, const QString &lastError);

private slots:
    void onWorkerFinished();

private:
    void startNext();
    void setReanalysing(bool on);

    QStringList m_queue;        // pending swing dirs
    QString     m_current;      // swing dir in flight (empty when idle)
    QString     m_lastError;    // reason of the most recent failure this batch
    int         m_succeeded = 0;
    int         m_failed    = 0;
    bool        m_reanalysing  = false;
    bool        m_liveBusy     = false;   // a live shot is processing
    bool        m_startDeferred = false;  // a startNext() was held off for m_liveBusy

    QFutureWatcher<pinpoint::analysis::ReanalyzeResult> m_watcher;
};
