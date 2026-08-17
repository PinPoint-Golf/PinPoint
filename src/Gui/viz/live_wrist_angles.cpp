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

#include "live_wrist_angles.h"

#include <QQuaternion>

#include "athlete_controller.h"
#include "hm_instance.h"
#include "imu_instance.h"
#include "imu_manager.h"
#include "../Analysis/wrist_angles.h"

using namespace pinpoint::analysis;

LiveWristAngles::LiveWristAngles(ImuManager *imu, AppSettings *settings,
                                 AthleteController *athlete, QObject *parent)
    : QObject(parent), m_imu(imu), m_athlete(athlete)
{
    // `settings` is no longer read here: slot→sensor resolution moved to
    // ImuManager::instanceForSlot(), which owns the unit-keyed placement rule for
    // both device kinds (a bare device id for a Witmotion, HmUnit::unitId() for a
    // HackMotion). The parameter stays so main.cpp's construction is untouched.
    Q_UNUSED(settings)

    m_timer.setInterval(33);   // ~30 Hz — plenty for a live readout
    connect(&m_timer, &QTimer::timeout, this, &LiveWristAngles::tick);
}

void LiveWristAngles::setActive(bool on)
{
    if (m_active == on)
        return;
    m_active = on;
    if (on) { m_timer.start(); tick(); }
    else      m_timer.stop();
    emit activeChanged();
}

// ---------------------------------------------------------------------------
// Slot → sensor, across two unrelated concrete types
// ---------------------------------------------------------------------------
//
// ImuManager::instanceForSlot() owns the placement rule — a bare device id keys a
// Witmotion, HmUnit::unitId() ("<deviceId>#lowerArm" / "#palm") keys a HackMotion,
// and one wG3 fills slots A and B — and hands back the object the viz layer binds
// to. That object is an ImuInstance for a Witmotion and an HmUnit for a
// HackMotion.
//
// ⚠ AN HmUnit IS NOT AN ImuInstance AND NEVER WILL BE. HmInstance is a PEER of
// ImuInstance rather than a subclass (hm_instance.h:184-194), and HmUnit is a child
// QObject that merely duck-types the same property NAMES so ImuVizView/ArmVizView
// resolve them through the metaobject. So the `qobject_cast<ImuInstance *>` this
// helper used to end with silently returns nullptr for a HackMotion slot — which
// this readout would display as "no sensor assigned" rather than as the assigned
// sensor it could not describe. Both kinds are therefore cast for explicitly.
//
// Deliberately NOT a shared base class or an interface header: the duck-typing is
// the design (imu_device.h sets out why the device-agnostic base carries only what
// ImuManager calls generically), and hoisting anatomical accessors into it is a
// bigger refactor than this phase authorises.
namespace {

struct SlotSensor {
    ImuInstance *wt = nullptr;   // Witmotion — anatomical frame solved on the host
    HmUnit      *hm = nullptr;   // HackMotion unit — see anatCalibrated() below

    // ⚠ A HACKMOTION IS UNCALIBRATED HERE THROUGHOUT PHASE C, AND THE "—" THAT
    // RESULTS IS THE HONEST OUTCOME, NOT A BUG TO WORK AROUND. HmUnit::
    // anatCalibrated() is false and anatQuat() is identity until Phase D solves the
    // constant per-unit rotation from the device's own anatomical convention to ours
    // (hm_instance.h:127-134). Phase C's device-native calibration makes the
    // device's quaternions meaningful in ITS frame; it does not reconcile that frame
    // with PinPoint's, so there is no lead-wrist angle to display yet.
    //
    // Do not "fix" this by feeding the raw quaternion through, and do not invent a
    // transform: the wrist angle 2·acos|q_a·q_b| is convention-blind, so a wrong
    // frame yields a perfectly plausible number with every decomposed sign free to
    // be inverted, and the display is the one place nothing checks it. PHASE D is
    // what unblocks this readout.
    bool anatCalibrated() const
    {
        if (wt) return wt->anatCalibrated();
        if (hm) return hm->anatCalibrated();
        return false;
    }
    QQuaternion anatQuat() const
    {
        if (wt) return wt->anatQuat();
        if (hm) return hm->anatQuat();
        return QQuaternion();
    }
    // (angularVelocityDps() is the third property both kinds answer by the same
    // name; nothing in this readout needs it, so it is not wrapped here.)
};

SlotSensor slotSensor(ImuManager *imu, const QString &slot)
{
    SlotSensor s;
    if (!imu) return s;
    QObject *o = imu->instanceForSlot(slot);
    if (!o) return s;
    s.wt = qobject_cast<ImuInstance *>(o);
    s.hm = qobject_cast<HmUnit *>(o);
    return s;
}

}   // namespace

void LiveWristAngles::tick()
{
    const SlotSensor fore  = slotSensor(m_imu, QStringLiteral("A"));   // forearm (at wrist)
    const SlotSensor hand  = slotSensor(m_imu, QStringLiteral("B"));   // back of hand
    const SlotSensor upper = slotSensor(m_imu, QStringLiteral("C"));   // upper arm (optional)

    // Lead arm is the LEFT for a right-handed golfer (matches ArmVizView). anatQuat is
    // identity at the calibration neutral, so the relative quaternion IS the posture
    // vs neutral — no address reference here (that lives in the post-shot analyzer).
    const bool leftArm = m_athlete && m_athlete->currentHandedness() != QLatin1String("Left");

    // An unassigned or not-yet-live slot answers false here (SlotSensor holds two
    // null pointers), so the "is there a sensor at all" test is folded into the
    // calibration test exactly as before.
    m_bowValid = fore.anatCalibrated() && hand.anatCalibrated();
    if (m_bowValid) {
        const QQuaternion rel = (fore.anatQuat().conjugated() * hand.anatQuat()).normalized();
        const WristAngles w = wristFlexExtDeviation(rel, leftArm);
        m_bow        = radToDeg(w.feRad);
        m_hinge      = radToDeg(w.rudRad);
        m_bowLabel   = wristMetricLabel(QStringLiteral("leadWristFlexExt"), m_bow);
        m_hingeLabel = wristMetricLabel(QStringLiteral("leadWristRadUln"),  m_hinge);
    } else {
        m_bowLabel = m_hingeLabel = QStringLiteral("—");
    }

    m_rollValid = upper.anatCalibrated() && fore.anatCalibrated();
    if (m_rollValid) {
        const QQuaternion rel = (upper.anatQuat().conjugated() * fore.anatQuat()).normalized();
        const ForearmElbow ef = forearmPronElbowFlex(rel, leftArm);
        m_roll      = radToDeg(ef.pronRad);
        m_rollLabel = wristMetricLabel(QStringLiteral("forearmPronation"), m_roll);
    } else {
        m_rollLabel = QStringLiteral("—");
    }

    emit changed();
}
