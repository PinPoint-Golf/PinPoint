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
#include <QTimer>

class ImuManager;
class AppSettings;
class AthleteController;

// LiveWristAngles — real-time lead-wrist metrics measured against the calibration
// NEUTRAL, for the session-wizard "Check your sensor" overlay (QML context property
// `liveWrist`). It resolves the same A/B/C IMU slots ArmVizView uses — through
// ImuManager::instanceForSlot(), which resolves either device kind — reads each
// sensor's anatQuat (identity at the neutral reference) on a timer, and runs the
// tested wrist_angles.h math:
//   bow/cup + hinge = wristFlexExtDeviation(forearmAnat⁻¹ · handAnat)   [slots A + B]
//   roll            = forearmPronElbowFlex(upperAnat⁻¹ · forearmAnat)   [slots C + A]
//
// Display/verification ONLY — it does NOT feed shot analysis (the post-shot analyzer
// computes its own values, and also relative to address). It never modifies ArmVizView.
class LiveWristAngles : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    active     READ active     WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool    bowValid   READ bowValid   NOTIFY changed)   // forearm + hand calibrated
    Q_PROPERTY(bool    rollValid  READ rollValid  NOTIFY changed)   // upper arm + forearm calibrated
    Q_PROPERTY(double  bowValue   READ bowValue   NOTIFY changed)
    Q_PROPERTY(QString bowLabel   READ bowLabel   NOTIFY changed)
    Q_PROPERTY(double  hingeValue READ hingeValue NOTIFY changed)
    Q_PROPERTY(QString hingeLabel READ hingeLabel NOTIFY changed)
    Q_PROPERTY(double  rollValue  READ rollValue  NOTIFY changed)
    Q_PROPERTY(QString rollLabel  READ rollLabel  NOTIFY changed)
    // "Roll" when the value is ISB pronation (upper arm + forearm, Witmotion path);
    // "Rotation" when it is the wG3 fallback below — the vendor's own name for its
    // third wrist metric, chosen because it makes no ISB claim (see the .cpp note).
    Q_PROPERTY(QString rollTitle  READ rollTitle  NOTIFY changed)

public:
    LiveWristAngles(ImuManager *imu, AppSettings *settings, AthleteController *athlete,
                    QObject *parent = nullptr);

    bool active() const { return m_active; }
    void setActive(bool on);

    bool    bowValid()   const { return m_bowValid; }
    bool    rollValid()  const { return m_rollValid; }
    double  bowValue()   const { return m_bow; }
    QString bowLabel()   const { return m_bowLabel; }
    double  hingeValue() const { return m_hinge; }
    QString hingeLabel() const { return m_hingeLabel; }
    double  rollValue()  const { return m_roll; }
    QString rollLabel()  const { return m_rollLabel; }
    QString rollTitle()  const { return m_rollTitle; }

signals:
    void activeChanged();
    void changed();

private:
    void tick();

    ImuManager        *m_imu;
    // ⚠ No AppSettings member. Slot→sensor resolution is ImuManager::
    // instanceForSlot()'s job as of Phase C — it is the single place that knows
    // placement is keyed by device id for a Witmotion and by UNIT for a HackMotion
    // ("<deviceId>#lowerArm" / "#palm", one wG3 filling A and B). Reading
    // imuPlacement here again would be a seventh copy of that rule, and the copies
    // are what a two-unit device breaks silently. The constructor still takes the
    // settings pointer so main.cpp is unchanged.
    AthleteController *m_athlete;
    QTimer             m_timer;

    bool    m_active    = false;
    bool    m_bowValid  = false;
    bool    m_rollValid = false;
    double  m_bow = 0.0, m_hinge = 0.0, m_roll = 0.0;
    QString m_bowLabel  = QStringLiteral("—");
    QString m_hingeLabel = QStringLiteral("—");
    QString m_rollLabel  = QStringLiteral("—");
    QString m_rollTitle  = QStringLiteral("Roll");
};
