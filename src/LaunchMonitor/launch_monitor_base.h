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

#include "launch_monitor_reading.h"

#include <QObject>
#include <QString>

namespace pinpoint::lm {

// Which connector. `None` is a real, constructible choice — see makeLaunchMonitor.
enum class Kind {
    None,
    GcQuad,     // Foresight GC Quad via FSX2020's LastShot.CSV
};

QString kindKey(Kind k);            // the settings token: "none", "gcquad"
Kind    kindFromKey(const QString &key);

// The state vocabulary, owned here so the base and every connector share one enum.
// LaunchMonitorController maps these to the strings QML switches on — that mapping
// is the single source of truth, so the QML contract cannot drift. Deliberately NOT
// exposed to QML, for the same reason pp::update::State and BleImuTransport::State
// are not.
enum class State {
    Disabled,   // no connector configured — the ordinary state on most machines
    Waiting,    // configured and watching, nothing seen yet
    Ready,      // a reading has been read from this source at least once
    Error,      // the configured source cannot be read; errorText() says why
};

// Polymorphic source of per-shot launch monitor readings. One concrete subclass per
// device family, plus an inert fallback, chosen by makeLaunchMonitor().
//
// The base deliberately knows nothing about shots, swings or attribution: a
// connector's whole job is to notice a new reading and say so. Deciding WHICH swing
// a reading belongs to is ShotPairing's, and writing it is the controller's — so a
// second connector inherits both without restating either.
class LaunchMonitorBase : public QObject
{
    Q_OBJECT

public:
    explicit LaunchMonitorBase(QObject *parent = nullptr) : QObject(parent) {}
    ~LaunchMonitorBase() override = default;

    virtual Kind kind() const = 0;

    // A short description of where readings come from, for the settings panel and
    // the log ("…/FSX2020/LastShot.CSV"). Empty when nothing is configured.
    virtual QString sourceDescription() const { return QString(); }

    // Begin watching. Must be idempotent: the controller calls it again whenever
    // the configured source changes.
    //
    // A connector MUST NOT emit a reading for data that already existed when it
    // started. FSX2020 leaves its last shot on disk between runs, so a source that
    // claimed what it found at startup would give the first swing of the day
    // yesterday's numbers. Establish the baseline silently instead.
    virtual void start() = 0;
    virtual void stop()  = 0;

    // Where to look and how often. Generic enough to live on the base: every
    // connector we can foresee is either a watched path or a polled endpoint, and
    // putting them here means the controller can re-apply settings without knowing
    // which connector it holds. Defaults ignore both.
    virtual void setSourcePath(const QString &path) { Q_UNUSED(path) }
    virtual void setPollIntervalMs(int ms)          { Q_UNUSED(ms) }

    State   state()     const { return m_state; }
    QString errorText() const { return m_errorText; }

signals:
    // A reading the connector had not seen before. Emitted on the GUI thread.
    void readingAvailable(const pinpoint::lm::LaunchMonitorReading &reading);
    void stateChanged();

protected:
    void setState(State s, const QString &errorText = QString())
    {
        if (m_state == s && m_errorText == errorText)
            return;
        m_state     = s;
        m_errorText = errorText;
        emit stateChanged();
    }

private:
    State   m_state = State::Disabled;
    QString m_errorText;
};

// The null object for "no launch monitor configured". Never emits, always Disabled.
//
// This exists so that neither the controller nor QML ever holds a null pointer and
// has to remember to check it — the same reason InertUpdateBackend exists. Every
// caller can bind to a monitor unconditionally.
class InertLaunchMonitor : public LaunchMonitorBase
{
    Q_OBJECT

public:
    explicit InertLaunchMonitor(QObject *parent = nullptr) : LaunchMonitorBase(parent) {}

    Kind kind() const override { return Kind::None; }
    void start() override { setState(State::Disabled); }
    void stop()  override {}
};

} // namespace pinpoint::lm
