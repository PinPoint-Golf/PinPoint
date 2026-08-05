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

#include "launch_monitor_reading.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace pinpoint::analysis { struct SwingAnalysis; }

namespace pinpoint {

// The single, unified per-shot document. Raw capture manifest and derived analysis
// live in ONE swing.json — no separate analysis.json. Written once, on the GUI thread,
// at the analyzer∥exporter join (ShotProcessor::maybeJoin), so the two concurrent
// workers never write the same file.
class SwingDocWriter {
public:
    // Compose `rawManifest` (the exporter's returned pinpoint.swing tree) with the
    // analyzer's `analysis` (inline, additive "analysis" object) and write atomically
    // to <swingDir>/swing.json. Bumps schema to pinpoint.swing/2. analysis==nullptr →
    // raw only (no "analysis"). Returns false (and sets *error) on write failure.
    static bool writeSwingJson(const QString &swingDir, const QJsonObject &rawManifest,
                               const analysis::SwingAnalysis *analysis,
                               QString *error = nullptr);

    // Write-through of the user's review (rating 0–5, free-text note, club) into
    // an existing <swingDir>/swing.json: reads the doc, replaces the additive
    // "review" block, atomic rewrite (QSaveFile). Called from the shot model's
    // setRating/setNote/setClub so edits survive a restart. Returns false (and
    // sets *error) if the doc can't be read or rewritten — a shot whose swing.json
    // was never written (e.g. export failed) simply fails here harmlessly.
    static bool updateReview(const QString &swingDir, int rating, const QString &note,
                             const QString &club, QString *error = nullptr);

    // Fold a launch monitor reading into an existing <swingDir>/swing.json.
    //
    // LATE BY DESIGN. The monitor writes its file after the shot, and swing.json is
    // written only when the analyzer and exporter have both finished (12-37 s), so a
    // reading normally lands during analysis and sometimes during the first replay.
    // Rather than hold the document open or re-run analysis, this is the same
    // read-modify-atomic-rewrite updateReview() performs, and it writes two things:
    //
    //   · a top-level "launchMonitor" block — every value as the device reported it,
    //     including the columns that map to no metric. Additive, so older readers
    //     ignore it (swing_json_schema.md).
    //   · one entry per reading in analysis.metrics[], keyed `lm.*`, each an EMPTY
    //     CURVE carrying a single phaseSample at Impact. That is the shape every
    //     point-in-time metric already uses, and it is what lets the phase grid,
    //     the measures, the corridors, the charts and the data viewer pick these up
    //     with no new machinery anywhere.
    //
    // The metric entries are only written when the document already has an "analysis"
    // block; the raw block is written regardless, so a shot whose analysis failed
    // still keeps what the device said.
    //
    // Idempotent: re-applying replaces any existing "launchMonitor" block and any
    // existing `lm.` metric entries rather than appending duplicates. Refreshes the
    // summary sidecar for the same reason updateReview does — this rewrite moves
    // swing.json's size and mtime, which invalidates both sidecar guards.
    static bool updateLaunchMonitor(const QString &swingDir,
                                    const lm::LaunchMonitorReading &reading,
                                    QString *error = nullptr);

    // Who the shot belongs to and where it sits — everything a device-only document
    // needs that the reading itself cannot supply.
    struct DeviceOnlyMeta {
        QString swingId;              // "swing_0007"
        int     swingIndex = 0;
        QString sessionId;            // the session FOLDER name, for the draw-from filter
        QString athleteName;
        QString athleteUuid;          // what a norm COHORT is resolved through
        QString club;                 // the app's selected club, not the device's code
        int     sessionType = -1;
        qint64  wallclockMs = 0;      // when the reading was taken
    };

    // Write a complete swing.json for a shot that ONLY a launch monitor saw.
    //
    // The capture pipeline produced nothing: no camera, no IMU, no buffer window, so no
    // analysis and no export. This is not a degraded version of writeSwingJson — it is a
    // different document, and it says so rather than leaving blanks that read as failure:
    // `streams` is empty, there is no thumbnail, and `analysis` carries metrics and one
    // phase and nothing else.
    //
    // THE IMPACT EVENT IS MANUFACTURED, AND IT HAS TO BE. Every lm.* measure reduces
    // "at p7", and buildPhaseGrid returns an EMPTY grid when `analysis.phases[]` is empty
    // — "unsegmented: no phase to read anything at, so nothing is producible" — so
    // without a phase entry the readings would persist and then resolve to nothing at
    // all. A ball WAS struck, which is why `conf` is 1.0: what is unknown is the
    // instant, not the event. That unknown is carried honestly by `capture.impactUs`,
    // which stays at its -1 "unknown" sentinel while the phase sits at t_us 0.
    //
    // Returns false (and sets *error) if the directory cannot be written.
    static bool writeDeviceOnlySwing(const QString &swingDir,
                                     const lm::LaunchMonitorReading &reading,
                                     const DeviceOnlyMeta &meta,
                                     QString *error = nullptr);
};

// A reloaded shot — everything ShotListModel::addPersistedShot needs to rebuild a
// carousel row from a swing.json on disk. The flat metrics + analysisDetail are
// reconstructed into the same shapes ShotProcessor produces live.
struct PersistedShot {
    bool        ok = false;
    QString     swingDir;
    int         ordinal = 0;
    QString     timestampLabel;     // hh:mm:ss from clock.wallclock
    qint64      wallclockMs = 0;    // absolute instant from clock.wallclock (epoch ms; 0 = unknown)
    // WHO swung it. swing.json has carried these since the exporter's first version and the reader
    // dropped them, so a reloaded shot knew when it happened and not who it belonged to.
    //
    // The uuid is what a norm COHORT is resolved through: the athlete record holds the date of
    // birth, `wallclockMs` holds the day, and the band is derived from the two at read time — never
    // stored, because an athlete ages across their own history. Without the uuid here, an offline
    // re-analysis and the live path would resolve different cohorts for the same swing and grade it
    // two ways, with nothing anywhere reporting a disagreement.
    QString     athleteUuid;
    QString     athleteName;        // display only; the uuid is the key
    QString     club;
    bool        hasVideo = false;
    QString     thumbnailPath;      // absolute, empty if none
    int         score = 0;
    int         rating = 0;         // 0–5 user stars (from the "review" block)
    QString     note;               // free-text user note (from the "review" block)
    QVariantMap metrics;            // key -> { label, value } at Impact
    QVariantMap analysisDetail;     // { tier, overall, series, phases } for the graph
    bool        dataWarning = false;// IMU re-fusion parity failed (imuIntegrity block) → not re-analysable
    // WHICH DEVICE MEASURED IT — the launchMonitor.kind token ("gcquad"), empty when the
    // shot has no device block. The readings themselves come back through analysisDetail's
    // `lm.` series; this is the only place their provenance survives a reload, and without
    // it a reviewed session could only be captioned with whatever is plugged in today.
    QString     lmDeviceKind;
};

// The handful of fields a session-list row needs — everything pinpoint::ShotSummaryInput
// (session_summary.h) consumes, plus ordering/display scalars. Deliberately NOT a subset
// view of PersistedShot: that struct carries analysisDetail (incl. the multi-MB pose2d
// keypoint track needed for replay overlays), and the whole point here is to summarise a
// session WITHOUT touching it.
struct SwingSummary {
    bool    ok = false;
    QString swingDir;
    int     ordinal = 0;
    QString timestampLabel;         // hh:mm:ss from clock.wallclock
    qint64  wallclockMs = 0;        // absolute instant (epoch ms); 0 = unknown
    QString club;
    bool    hasVideo = false;
    QString thumbnailPath;          // absolute, empty if none
    int     score = 0;
    // Provenance, never persisted: true when this came from the sidecar, false when the
    // full swing.json had to be parsed. Lets the parity test prove it actually exercised
    // the cheap path — a bug that always fell back would otherwise pass silently while
    // the stall crept back.
    bool    fromSidecar = false;
};

// Reads the single unified swing.json back into reloadable shots.
class SwingDocReader {
public:
    static PersistedShot readSwingJson(const QString &swingDir);

    // Cheap per-swing summary for the session picker. Prefers <swingDir>/swing_summary.json
    // (a few hundred bytes), validating its source{size,mtime_ms} against the real
    // swing.json; on a miss or a stale guard it falls back to the full readSwingJson()
    // parse and — when writeSidecar is true — writes the sidecar so the next read is cheap.
    //
    // Pass writeSidecar=false on any GUI-thread path that must never fat-parse: the caller
    // then gets ok=false for an un-indexed swing and renders it without detail, rather than
    // stalling. The sidecar is pure cache — always safe to delete, always regenerable.
    static SwingSummary readSwingSummary(const QString &swingDir, bool writeSidecar = true);

    // Derive a summary from an already-parsed shot and write its sidecar. Called where a
    // fat parse has happened anyway (session load), so indexing costs nothing extra.
    static bool writeSwingSummary(const PersistedShot &shot, QString *error = nullptr);
    // swing_*/ directories under sessionDir, ascending (swing_0001 .. swing_NNNN).
    static QStringList findSwingDirs(const QString &sessionDir);
    // Most recent session dir for an athlete, by directory modification time
    // (folder names embed the naming pattern, so a name sort isn't reliable);
    // empty if the library/athlete has none.
    static QString latestSessionDir(const QString &libraryRoot, const QString &athleteName);
    // All session dirs for an athlete, most-recently-modified first (same recency
    // basis as latestSessionDir). Empty list if the library/athlete has none.
    static QStringList sessionDirs(const QString &libraryRoot, const QString &athleteName);
};

} // namespace pinpoint
