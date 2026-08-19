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

#include <cmath>

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

    // A HackMotion answers both of these for real: Phase D reconciled the device's own
    // anatomical convention with ours, and HmInstance's display tick sets anatQuat from
    // hm_frame::toAnatomical() with the selected candidate whenever the device reports itself
    // calibrated. So a calibrated wG3 produces a lead-wrist readout here exactly as a Witmotion
    // pair does, and this wrapper needs nothing but the two accessors it already forwards.
    //
    // ⚠ Throughout Phase C this said a HackMotion was uncalibrated here and the "—" was the
    // honest outcome. That was true then and became false the day Phase D shipped, which is worth
    // remembering: a comment pinned to a phase is a claim with an expiry date on it.
    //
    // The "—" is still what an UNCALIBRATED unit yields, and that is still deliberate. Do not
    // "fix" it by feeding the raw quaternion through, and do not invent a transform: the wrist
    // angle 2·acos|q_a·q_b| is convention-blind, so a wrong frame yields a perfectly plausible
    // number with every decomposed sign free to be inverted, and the display is the one place
    // nothing checks it.
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
        m_rollTitle = tr("Roll");
        const QQuaternion rel = (upper.anatQuat().conjugated() * fore.anatQuat()).normalized();
        const ForearmElbow ef = forearmPronElbowFlex(rel, leftArm);
        // ⚠ PRINCIPAL VALUE, because this readout holds ONE instant. twistAngleRad returns
        // (−2π, 2π] and jumps a full turn as the quaternion changes sign, so the raw number
        // can read −282° for a forearm sitting at +78°. The series path answers this by
        // unwrapping against neighbouring samples; a live tick has no neighbours, so the
        // only defensible answer is the principal one. See wrist_angles.h.
        m_roll      = radToDeg(principalAngleRad(ef.pronRad));
        m_rollLabel = wristMetricLabel(QStringLiteral("forearmPronation"), m_roll);
    } else if (fore.anatCalibrated()) {
        // ⚠ THE NO-UPPER-ARM FALLBACK IS CALLED "Rotation", NEVER "Roll" AND NEVER
        // forearmPronation — and it is VENDOR-AGNOSTIC on purpose. Our roll is an ISB
        // radioulnar joint angle read against the UPPER ARM; a wG3 has no unit for one and a
        // Witmotion worn A+B has none either, so both rigs land in the same place. What the
        // slot-A sensor alone CAN answer — identically for both vendors — is the forearm's
        // rotation about its own long axis versus the calibration pose: the same swing-twist
        // this file already runs, with the upper arm as identity, and the quantity the wG3's
        // vendor reports as its third wrist metric under the same name. ONE definition for
        // both instruments is deliberate: defining Witmotion's rotation against the upper arm
        // while the wG3's is forearm-alone would publish two different quantities under one
        // name, and a cross-lane comparison (P1→P7 deltas, Phase G grading) would read the
        // shoulder's contribution as sensor error. hm_frame.h's PRONATION RATE note is the
        // authority; pinpoint_sign_conventions.md forbids implying ISB conformance we lack.
        //
        // ⚠ ZERO IS THE CALIBRATION POSE, NOT ANATOMICAL SQUARE — and the two vendors calibrate
        // in DIFFERENT poses (wG3 palm-down across the chest, Witmotion arm-down), so absolute
        // values are not comparable across vendors; deltas between swing positions are. This
        // number says how far the forearm has TURNED since calibration, in the pronation(+)/
        // supination(−) sense, not where it sits anatomically. That is why the label is a
        // signed angle with no anatomical words, and why this value is display-only and must
        // never feed a metric.
        m_rollTitle = tr("Rotation");
        m_rollValid = true;
        //
        // ⚠ AND IT IS THE PRINCIPAL VALUE — see the Roll branch above. This one matters more
        // than its neighbour rather than less: a wG3 calibrates palm-down across the chest,
        // already near full pronation, so a forearm in a normal address posture sits a long
        // way from zero and lands near the ±180° cut where the raw value flips sign. Reading
        // −282° on the check page for +78° of turn is exactly the confusion this page exists
        // to prevent.
        const ForearmElbow ef = forearmPronElbowFlex(fore.anatQuat().normalized(), leftArm);
        m_roll      = radToDeg(principalAngleRad(ef.pronRad));
        const long r = std::lround(m_roll);
        m_rollLabel = (r > 0 ? QStringLiteral("+") : QString())
                      + QString::number(r) + QStringLiteral("°");
    } else {
        m_rollTitle = tr("Roll");
        m_rollLabel = QStringLiteral("—");
    }

    emit changed();
}
