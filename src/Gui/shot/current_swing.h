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

#include <QObject>
#include <QString>

// WHICH SWING IS ON SCREEN, app-wide — one string, written by whichever session screen has one
// loaded and readable by anything that is not that screen.
//
// It exists because "the focused swing" was a QML expression rather than a fact. ScreenWrist
// computes `shotReplay.swingDir !== "" ? … : carousel.selectedCard.swingDir` — the active replay,
// else the carousel's selection — and that expression is reachable only from inside that screen's
// own scope. Everything on another screen (the Diagnostic Model panel in Settings, which the
// session lock deliberately leaves reachable) could see the replay half through the `shotReplay`
// context property and none of the selection half, so it saw nothing whenever a swing was merely
// SELECTED rather than playing, which is most of the time.
//
// Deliberately NOT folded into ShotReplayController. That object's contract is the QML-shot
// identity of an ACTIVE replay — `swingDir()` returns empty when `!m_active`, and every consumer
// depends on that emptiness to fall back. A second, stickier dir on the same object would have two
// meanings of "the shot this controller is about", and the fallback expressions would have to know
// which one they wanted.
//
// The writer is the screen, not this object: it holds no policy about what "focused" means, because
// that answer is per screen (Wrist reads a carousel; another screen may not have one). A screen that
// stops showing a swing clears it — an empty dir means nothing is loaded, and every reader treats it
// that way.
class CurrentSwing : public QObject
{
    Q_OBJECT

    // Absolute path of the swing_NNNN/ folder, or empty when no screen has one loaded.
    Q_PROPERTY(QString swingDir READ swingDir WRITE setSwingDir NOTIFY swingDirChanged)

public:
    using QObject::QObject;

    QString swingDir() const { return m_swingDir; }

    void setSwingDir(const QString &dir)
    {
        // Guarded, because the writer is a QML binding that re-evaluates on every carousel and
        // replay change: re-announcing the same swing would make every reader re-do its work
        // (the model panel re-reads a phase grid off disk) for no change at all.
        if (m_swingDir == dir) return;
        m_swingDir = dir;
        emit swingDirChanged();
    }

signals:
    void swingDirChanged();

private:
    QString m_swingDir;
};
