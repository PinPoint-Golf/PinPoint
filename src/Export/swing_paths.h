/*
 * Copyright (C) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <QString>

namespace pinpoint {

// Resolves and creates the on-disk layout for swing exports:
//
//   <athleteLibraryPath>/
//     <athlete-name-sanitised>/          athlete level
//       <session-folder>_NN/             session level — name per naming pattern
//         swing_0001/                    one folder per swing
//
// The session-folder leaf is composed from the AppSettings sessionNamingPattern
// (date / athlete / session-type tokens) plus a per-day uniqueness counter.
// It is allocated lazily on the first save and cached for the lifetime of this
// object (i.e. per app run): all swings of one run share one session folder.
// The cache is invalidated when the athlete, date, library root, or composed
// session-base (pattern / session type) changes.
class SwingPaths {
public:
    struct Allocation {
        QString swingDir;     // absolute path, created; empty on failure
        QString sessionId;    // e.g. "2026-06-05_Mark-Liversedge_Swing_01"
        QString swingId;      // e.g. "swing_0007"
        int     swingIndex = 0;
    };

    // ⭐ DELETE = TRASH, AND THE VOLUME MAY NOT HAVE ONE (2 Sept 2026).  Every
    // delete in this application is a move to the OS trash, so it is
    // recoverable.  A network share has no trash — macOS answers
    // NSCocoaErrorDomain 3328, "the volume doesn't have one" — and until now
    // every delete of a session or shot on the SwingData share failed, with a
    // log line and nothing on screen.  This tries the OS trash and, where the
    // volume refuses, MOVES the folder into a library-local trash on the same
    // volume: `<libraryRoot>/.pinpoint-trash/<athlete>/<stamp>_[<session>_]<name>`,
    // hidden from every listing (dot-prefixed; the enumerators do not pass
    // QDir::Hidden) and as recoverable as the OS trash would have been.  `where`
    // names the destination for the log.
    static bool trashPath(const QString& path, QString* where = nullptr);

    // Allocates (and creates) the next swing directory.  libraryRoot may be
    // empty — falls back to <AppDataLocation>/swings with a warning.
    // namingPattern is the AppSettings sessionNamingPattern key
    // ("date-name-type" | "date-type-name" | "name-date-type" | "date-only");
    // sessionTypeLabel is a human label ("Swing"/"Wrist"/...), empty when none.
    Allocation allocateSwingDir(const QString& libraryRoot,
                                const QString& athleteName,
                                const QString& athleteUuid,
                                const QString& namingPattern = QStringLiteral("date-name-type"),
                                const QString& sessionTypeLabel = QString());

    // ── Explicit per-session lifecycle ──────────────────────────────────────
    // The session folder is normally allocated lazily on the first save (above).
    // These let the folder be decided up front at session START instead, so the
    // UI can offer "extend today's session vs start a new one" and discard a
    // session that captured nothing.

    // Most-recent existing session folder for TODAY matching this athlete +
    // naming pattern + session type, or "" when none exists today. (The composed
    // base embeds today's ISO date, so every "<base>_NN" match is a today folder.)
    QString findTodaySessionDir(const QString& libraryRoot,
                                const QString& athleteName,
                                const QString& athleteUuid,
                                const QString& namingPattern = QStringLiteral("date-name-type"),
                                const QString& sessionTypeLabel = QString());

    // Begin an explicit session. extendExisting reuses today's most-recent folder
    // (no counter bump); otherwise a fresh "_NN" is allocated and created. Primes
    // the allocation cache so every swing this session lands in it, and records a
    // swing-count baseline for endSession(). Returns the folder path (empty on
    // failure).
    QString beginSession(const QString& libraryRoot,
                         const QString& athleteName,
                         const QString& athleteUuid,
                         const QString& namingPattern = QStringLiteral("date-name-type"),
                         const QString& sessionTypeLabel = QString(),
                         bool extendExisting = false);

    // The current (primed/cached) session folder, or "".
    QString currentSessionDir() const { return m_cachedSessionDir; }

    // End the session. When discardIfNoNewSwings and the folder gained no swings
    // since beginSession() (swing_* count <= baseline), move it to the OS trash
    // (recoverable). Always clears the cache so the next session re-resolves.
    void endSession(bool discardIfNoNewSwings);

    // Filesystem-safe token: trim, whitespace/path-hostile chars -> '-',
    // collapse separators, truncate; "unknown" when nothing survives.
    static QString sanitise(const QString& raw);

private:
    // Resolved, pattern-composed session identity (shared by allocate/find/begin).
    struct Resolved {
        QString root;         // library root, with AppData fallback applied
        QString athleteDir;   // <root>/<athlete-token>
        QString today;        // ISO yyyy-MM-dd
        QString base;         // composed session-folder base (pre-"_NN" counter)
    };
    static Resolved resolveSession(const QString& libraryRoot, const QString& athleteName,
                                   const QString& athleteUuid, const QString& namingPattern,
                                   const QString& sessionTypeLabel);

    // Session cache key — a new session folder is allocated when any part changes.
    QString m_cachedRoot;
    QString m_cachedAthleteUuid;
    QString m_cachedDate;        // ISO yyyy-MM-dd
    QString m_cachedBase;        // composed session-folder base (pre-counter)
    QString m_cachedSessionDir;  // absolute path
    QString m_cachedSessionId;   // folder name
    // Swing_* count in the folder when beginSession() primed it (-1 = no explicit
    // session in progress). endSession() discards when the count hasn't grown.
    int     m_sessionBaselineSwings = -1;
};

} // namespace pinpoint
