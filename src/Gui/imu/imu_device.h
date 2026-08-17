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

#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>

#include "types.h"

// The device-kind-agnostic slice of an IMU device, as ImuManager uses it.
//
// WHY THIS EXISTS. ImuManager owned a bare `ImuInstance*` per selected device
// and touched it at ~14 sites. The HackMotion wG3 is a second device KIND — not
// a second Witmotion — and it breaks the one assumption every one of those sites
// made: that a device is one instance, one EventBuffer source, one anatomical
// segment. The wG3 is one BLE peripheral presenting TWO sensor units (lower arm
// and palm, fixed by the wiring), so it registers TWO sources and fills TWO
// wrist slots.
//
// ⚠ WHAT DELIBERATELY IS *NOT* HERE. Only what the manager calls generically.
// Anything device-specific — output-rate selection, orientation-filter choice,
// host-side zeroing, the anatomical A/M solve — stays on the concrete class and
// the caller qobject_casts for it. That is not squeamishness: the wG3 has no
// settable output rate (it is adaptive), no host-side fusion (it fuses on-device)
// and no host-side zero, so hoisting those here would create a base class whose
// contract two implementations answer in contradictory ways, which is the exact
// shape of bug this split exists to prevent.
//
// The QML-facing Q_PROPERTYs stay on the concrete classes. QML resolves by NAME
// through the metaobject, so a property need not be declared here to be readable
// from QML — and the two kinds expose genuinely different property sets.
class ImuDeviceBase : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~ImuDeviceBase() override = default;

    // Identification. `deviceId` is the enumerator's device id (BT address, or
    // the device UUID on macOS where addresses are null) and is the key for
    // AppSettings::imuExcluded and the manager's own map.
    virtual QString     deviceId()          const = 0;
    virtual QString     deviceDescription() const = 0;
    virtual QStringList logEntries()        const = 0;

    // Live state the manager and the resource monitor surface.
    virtual QString stateLabel()      const = 0;
    virtual bool    imuConnected()    const = 0;
    virtual bool    busy()            const = 0;
    virtual int     batteryPercent()  const = 0;
    virtual double  dataRateHz()      const = 0;

    // ⚠ PLURAL, AND THAT IS THE POINT. A Witmotion returns exactly one id; a
    // HackMotion returns two (lower arm, palm). Every consumer that resolves
    // device metadata by source id — the exporter, the resource monitor, the
    // analysis job's binding list — has to be able to ask for all of them.
    // Returning a single scalar here is what made one-device-one-role
    // structural in the first place.
    virtual std::vector<pinpoint::SourceId> sourceIds() const = 0;

    // Parallel to sourceIds() when non-empty — same length, same order, one
    // short human label per registered source ("Lower arm" / "Palm" on the
    // wG3). Defaults to {} rather than being derived from the buffer's
    // SourceInfo::name: that string is a display value this layer does not
    // own or parse, and deriving from it would make the label depend on
    // registration-time formatting instead of device semantics. A Witmotion
    // has one unnamed source and returns {}; the resource monitor treats an
    // empty list as "no per-source labels", NOT as "no sources" — sourceIds()
    // is still the count. Without this, a multi-source device's rows fall
    // back to "#1"/"#2", which tells a coach nothing about which strap moved.
    virtual QStringList sourceLabels() const { return {}; }

    // Lifecycle, called by ImuManager.
    //
    // ⚠ THE ORDER IS A CONTRACT, not a convention: stop() is the producer stop
    // barrier and must return only once no ring write can be in flight or can
    // start. deregisterFromBuffer() may then be called, and ONLY while the
    // EventBuffer is paused (EventBuffer::deregisterSource asserts it). Getting
    // this pair the wrong way round races the merger against a live producer.
    virtual void start()               = 0;
    virtual void stop()                = 0;
    virtual void deregisterFromBuffer() = 0;

    // Dump this device's log ring to a file and return the path (or an error
    // string). ImuManager::saveLog() calls this generically across every
    // active instance — one call site, either device kind.
    virtual QString saveLog() = 0;

signals:
    void stateLabelChanged();
    void imuConnectedChanged();
    void busyChanged();
    void batteryPercentChanged();
    void dataRateHzChanged();
    void logEntryAdded(const QString &line);
};
