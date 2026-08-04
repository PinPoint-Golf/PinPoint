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

#include <QString>

#include <optional>

namespace pinpoint::lm {

// Decides WHICH swing a launch monitor reading belongs to.
//
// The problem: LastShot.CSV is one row rewritten in place. It carries no timestamp
// we can trust, and its Shot ID is FSX2020's own counter with no relationship to
// ours. Nothing in the file says which of our swings it describes.
//
// The rule, which is a policy choice and not a deduction:
//
//     shot detected ──► arm for this swing (displacing whatever was armed)
//     reading arrives ─► armed?  yes ─► it is this swing's; disarm
//                                no  ─► discard it
//
// A swing waits INDEFINITELY for its row — there is no timeout. What ends the wait
// is the next shot, because once another ball has been struck any subsequent write
// describes that one. So a swing whose row never arrives simply has no launch
// monitor data, which is the honest outcome; guessing would be worse.
//
// The second problem is that the row usually arrives before there is anywhere to
// put it. ShotProcessor writes swing.json only after both its workers finish, and
// analysis alone runs 12-37 s, so the reading is normally in hand long before a
// swingDir exists. A claimed reading with no destination is therefore PARKED and
// flushed by noteSwingDir(). It can equally arrive after — during first replay —
// in which case the destination is already known and it is claimed outright.
//
// WHY A SINGLE SLOT IS ENOUGH. ShotProcessor handles exactly one shot at a time,
// and ShotController::armed requires the processor to be idle, so no new shot can
// be detected while one is processing or replaying. Every shotProcessed therefore
// refers to the most recent shotDetected, and there can never be two swings
// waiting at once. If that invariant is ever relaxed, this class needs a map and
// this comment needs deleting.
//
// Pure logic, no Qt objects and no I/O, so all of it is exercised in a unit test.
class ShotPairing
{
public:
    enum class Disposition {
        Claimed,    // belongs to `swingDir`, write it now
        Parked,     // belongs to the armed swing, held until noteSwingDir()
        Discarded,  // nothing was armed — not ours to attribute
    };

    struct Offer {
        Disposition disposition = Disposition::Discarded;
        QString     swingDir;   // set only for Claimed
    };

    // A shot was committed. Arms for it, displacing any swing still waiting — that
    // swing gets no launch monitor data, deliberately.
    void noteShotDetected()
    {
        m_armed = true;
        m_swingDir.clear();
        // A parked reading here means the previous swing never reached a swingDir
        // (its analysis or export failed). It has nowhere to go.
        m_parked.reset();
    }

    // A reading turned up. Returns what should happen to it.
    Offer offerReading(const LaunchMonitorReading &reading)
    {
        if (!m_armed)
            return {};                          // Discarded
        m_armed = false;

        if (!m_swingDir.isEmpty())
            return { Disposition::Claimed, m_swingDir };

        m_parked = reading;
        return { Disposition::Parked, QString() };
    }

    // The armed swing reached disk. Returns a reading if one was parked for it.
    std::optional<LaunchMonitorReading> noteSwingDir(const QString &swingDir)
    {
        m_swingDir = swingDir;
        if (!m_parked)
            return std::nullopt;
        auto out = m_parked;
        m_parked.reset();
        return out;
    }

    // The shot failed before producing a swingDir. Anything held for it is dropped.
    void noteShotFailed()
    {
        m_armed = false;
        m_parked.reset();
        m_swingDir.clear();
    }

    bool armed()      const { return m_armed; }
    bool hasParked()  const { return m_parked.has_value(); }

private:
    bool    m_armed = false;
    QString m_swingDir;                             // "" until shotProcessed
    std::optional<LaunchMonitorReading> m_parked;   // claimed, awaiting a destination
};

} // namespace pinpoint::lm
