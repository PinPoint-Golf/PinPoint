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

// Session-screen MODE — the layout/activity axis (Capture | Replay | Analyse).
// Effectively global: only one session screen is visible at a time, so a single
// singleton holds the active mode and the focused swing that Replay/Analyse show.
//
// Mode is ORTHOGONAL to the data-source axis (Live vs Loaded, owned by
// SessionReviewController.reviewActive — kept under its legacy "review" name for
// now). Replay/Analyse work on either data-source; only Capture forces Live, so
// entering Capture returns to the live current session. The two compose — do not
// fuse them.
//
// Enum values are plain ints (a QML singleton can't host a Q_ENUM cleanly):
//   capture = 0, replay = 1, analyse = 2. Compare with `=== SessionMode.replay`.
//
// shotReplay (a context property) is reachable from this singleton's root context.

pragma Singleton
import QtQuick
import PinPointStudio

QtObject {
    id: sm

    readonly property int capture: 0
    readonly property int replay:  1
    readonly property int analyse: 2

    property int    mode: capture

    // The swing currently promoted onto the stage (Replay/Analyse). -1 / "" = none.
    property int    focusedShotId:   -1
    property string focusedSwingDir: ""

    // The live session diagnostics ledger, if a panel is up — the carousel's shot cards read
    // their pip row off it. NULLABLE AND NULL BY DEFAULT: the panel is off unless the user
    // turns it on in View, and a card must cost nothing when it is (see PpShotCard).
    property var    sessionDiagnostics: null

    // True only while the active Replay was auto-promoted from a just-captured shot
    // (post-shot instant playback). When that replay reaches its natural end the
    // host returns to Capture; EVERY other transition below clears it, so a
    // user-initiated Replay/Analyse — or a carousel pick — never auto-exits. The
    // return itself is wired in Main.qml (shotReplay.onPlaybackEnded).
    property bool   autoReturnToCapture: false

    // Capture IS the live current session: it shows live tiles, never the disk
    // replay. Returning to Capture exits any loaded past session (resumeLive is
    // idempotent) and drops that session's focused swing; the live-capture record
    // state is remembered/restored around that excursion in main.cpp. When toggling
    // from Replay/Analyse within the LIVE session, the focused swing is kept so
    // re-entering Replay/Analyse re-shows it.
    function enterCapture() {
        autoReturnToCapture = false
        mode = capture
        shotReplay.stop()
        if (sessionReviewController.reviewActive) {
            focusedShotId   = -1
            focusedSwingDir = ""
            sessionReviewController.resumeLive()
        }
    }

    // Analyse shows the focused swing's charts/table (its analysisDetail), so the
    // replay must stay loaded — ensure it is.
    function enterAnalyse() { autoReturnToCapture = false; mode = analyse; _loadFocused() }

    // Show Replay for the currently-focused swing (the mode-switch path). With no
    // focused swing the stage shows its empty-state — Replay is never blocked.
    function showReplay() {
        autoReturnToCapture = false
        mode = replay
        _loadFocused()
    }

    // Focus a swing and replay it. `fromCapture` is true ONLY on the post-shot
    // instant-playback path (Main.qml.onShotProcessed) — that replay auto-returns
    // to Capture when it ends. The carousel-click path passes nothing → false, so
    // a user-picked replay stays on the stage. Re-clicking the same card in Replay
    // just keeps it on the stage (no reload — see _loadFocused).
    function enterReplay(shotId, swingDir, fromCapture) {
        focusedShotId   = shotId
        focusedSwingDir = swingDir
        showReplay()                              // clears autoReturnToCapture
        autoReturnToCapture = (fromCapture === true)
    }

    // Deselect the focused swing — re-clicking the on-stage card in the carousel
    // clears the stage. Replay and Analyse fall back to their empty state (nothing
    // focused, replay stopped); the active mode is left unchanged so the user stays
    // on the same view, just emptied. Mirrors the "fresh, nothing focused" half of
    // enterLoadedSession without forcing a mode switch.
    function clearFocus() {
        autoReturnToCapture = false
        focusedShotId   = -1
        focusedSwingDir = ""
        shotReplay.stop()
    }

    // A past session was just loaded (data-source → Loaded): show it in Replay,
    // fresh — its own carousel supplies the swings, nothing focused yet. Capture is
    // live-only, so a loaded session never lands there.
    function enterLoadedSession() {
        autoReturnToCapture = false
        focusedShotId   = -1
        focusedSwingDir = ""
        mode = replay
        shotReplay.stop()
    }

    // Load the focused swing onto the stage, unless it is already the loaded one
    // (so toggling Replay↔Analyse, or re-clicking the focused card, never restarts
    // playback). One replay surface now (the session stage) — no target arg.
    function _loadFocused() {
        if (focusedShotId >= 0 && focusedSwingDir !== ""
                && shotReplay.shotId !== focusedShotId)
            shotReplay.start(focusedShotId, focusedSwingDir, 0.25)
    }
}
