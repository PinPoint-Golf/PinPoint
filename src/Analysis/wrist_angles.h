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

#include <QMatrix3x3>
#include <QQuaternion>
#include <QString>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <vector>

// Pure lead-arm joint-angle extraction from per-segment ANATOMICAL quaternions
// (q_anat = A·q_raw·M; design: docs/implementation/shot_analyzer_m1_wrist.md §4; frame & joint-DOF
// axis tables: docs/design/imu_frame_contract.md §4-5). Header-only and Qt-Gui-only so it is
// unit-testable without the app / SwingWindow.
//
// ANATOMICAL BODY FRAME (from imu_calibration::solveSegment): per segment,
//   +Y = long axis (distal / elbow→wrist),  +X = flexion axis (medio-lateral),
//   +Z = e_x × e_y.  These are SEGMENT-frame axis NAMES.
//
// ⚠ Do NOT conflate those segment-axis names with the axes the wrist DOFs are READ
// ON. The joint angles are extracted from the forearm→hand RELATIVE-rotation matrix
// (see wristFlexExtDeviation below, lines ~117-119), and in THAT decomposition:
// flexion/extension is read about Z, radial/ulnar deviation about X, and axial
// (hand-vs-forearm roll) drops out about Y. The flexion/deviation Z↔X assignment vs.
// the segment-axis names is deliberate and hardware-locked (2026-06; see the SIGN/AXIS
// STATUS note below and line 118) — an earlier "flexion about X" form had the two
// swapped, so do not "restore" it.
//
// SIGN/AXIS STATUS — hardware-locked 2026-06 for the lead (left) arm against
// ~/pinpoint_wrist_log.csv (captured live on the wizard "check your sensors" page). In
// the imu_calibration anatomical frame: flexion/extension = rotation about Z, radial/ulnar
// deviation = rotation about X, forearm pronation = axial twist (Y). The earlier X/Z swap
// is fixed below; signs verified WITHOUT a left/right mirror. NOT yet verified: the
// right-lead (left-handed golfer) case. Known limitation: ~10–15° of FE↔RUD cross-talk
// remains because the no-magnetometer heading (yaw) is unobservable between two sensors
// (see imu_base.h) — primary channels are correct, the secondary leak is the heading work.
//
// CHOSEN CONVENTION (PinPoint = ISB + golf coaching; docs/reference/wristmetrics.md):
//   +flexion  = "bowed"   (lead wrist bowed toward the ground — the impact goal)
//   +deviation= "ulnar"   (wrist "hinge"/cock)
//   +pronation= "roll"    (forearm pronation)
// User-facing names: bow/cup, hinge, roll.

namespace pinpoint::analysis {

// q = swing * twist, where `twist` is the rotation about `axis` and `swing` is the
// remainder. Robust at the 180°-about-⟂-axis singularity (axial part vanishes →
// identity twist, so the caller gets the full rotation as swing).
struct SwingTwist { QQuaternion swing; QQuaternion twist; };

inline SwingTwist swingTwistDecompose(const QQuaternion &q, const QVector3D &axis)
{
    const QVector3D a = axis.normalized();
    const QVector3D v(q.x(), q.y(), q.z());
    const QVector3D proj = QVector3D::dotProduct(v, a) * a;
    QQuaternion twist(q.scalar(), proj.x(), proj.y(), proj.z());
    if (twist.lengthSquared() < 1e-10f)
        twist = QQuaternion();                 // identity (singular: q ≈ 180° ⟂ axis)
    else
        twist.normalize();
    const QQuaternion swing = (q * twist.conjugated()).normalized();
    return { swing, twist };
}

// Signed angle (rad) of a twist quaternion about +axis.
//
// ⚠⚠ THE RANGE IS (−2π, 2π], NOT (−π, π], AND THIS COMMENT USED TO SAY OTHERWISE.
// The body is 2·atan2(…), and atan2 alone is what returns (−π, π]; doubling it doubles
// the range. That mis-stated range is most of the reason the 180° flip keeps being
// rediscovered: a reader checking whether a caller needs unwrapping reads "(−π, π]",
// concludes the value is already principal, and moves on.
//
// ⚠ A SINGLE SAMPLE FROM THIS FUNCTION IS NOT CONTINUOUS WITH ITS NEIGHBOURS. A unit
// quaternion double-covers SO(3) — q and −q are the same rotation — so as the physical
// twist passes ±180° the scalar changes sign and this result jumps by exactly 2π. On a
// still frame that is invisible; on a plotted TIME SERIES it draws a vertical cliff, and
// any peak/rate/Δ reduced from the series is then nonsense (a 28,758 °/100 ms "peak rate"
// is the signature).
//
// ⚠ SO EVERY CALLER THAT BUILDS A SERIES MUST RUN unwrapAngleSeries() OVER IT. Callers
// that read ONE instant (the live check-sensors readout) cannot unwrap — they have no
// neighbours — and should present the principal value instead.
inline double twistAngleRad(const QQuaternion &twist, const QVector3D &axis)
{
    const QVector3D v(twist.x(), twist.y(), twist.z());
    const double s = QVector3D::dotProduct(v, axis.normalized());
    return 2.0 * std::atan2(std::copysign(static_cast<double>(v.length()), s),
                            static_cast<double>(twist.scalar()));
}

// Make a sampled angle sequence CONTINUOUS across the branch cut, in place (np.unwrap):
// wrap each inter-sample DELTA into (−π, π] and accumulate, rather than wrapping every
// sample independently. This is the same treatment buildShaftLeanSeries applies to the
// shaft's atan2 angle, for the same reason and against the same failure.
//
// `anchor` is the index whose value is held canonical — normalised into (−π, π] and then
// unwrapped away from in BOTH directions. For an address-referenced metric pass the
// Address index: `forearmRotation` is defined as travel FROM address and is 0 there by
// construction, and unwrapping from sample 0 instead would leave that defining zero
// sitting at some ±2πk. For an absolute posture pass 0.
//
// ⚠ Correct only while the true angle moves < 180° BETWEEN SAMPLES, which is what makes
// the deltas unambiguous. At the rates these lanes run — ~800 Hz from a wG3, ~200 Hz
// fused — a 2,000 °/s forearm covers 2.5°/sample and 10°/sample respectively, so the
// margin is more than an order of magnitude. A sparse or gapped lane would NOT be safe.
inline void unwrapAngleSeries(std::vector<double> &rad, int anchor = 0)
{
    const int n = static_cast<int>(rad.size());
    if (n == 0) return;
    if (anchor < 0 || anchor >= n) anchor = 0;

    constexpr double kTwoPi = 2.0 * M_PI;
    const std::vector<double> raw = rad;

    rad[anchor] = std::remainder(raw[anchor], kTwoPi);
    for (int i = anchor + 1; i < n; ++i)
        rad[i] = rad[i - 1] + std::remainder(raw[i] - raw[i - 1], kTwoPi);
    for (int i = anchor - 1; i >= 0; --i)
        rad[i] = rad[i + 1] + std::remainder(raw[i] - raw[i + 1], kTwoPi);
}

// --- relative-quaternion builders (address-referenced in quaternion space) ----

// Hand relative to forearm (the wrist), referenced to the Address pose.
inline QQuaternion wristRel(const QQuaternion &qForeAnat, const QQuaternion &qHandAnat,
                            const QQuaternion &qWristAddr)
{
    const QQuaternion qWrist = (qForeAnat.conjugated() * qHandAnat).normalized();
    return (qWristAddr.conjugated() * qWrist).normalized();
}

// Forearm relative to upper arm (the elbow), referenced to the Address pose.
inline QQuaternion elbowRel(const QQuaternion &qUpperAnat, const QQuaternion &qForeAnat,
                            const QQuaternion &qElbowAddr)
{
    const QQuaternion qElbow = (qUpperAnat.conjugated() * qForeAnat).normalized();
    return (qElbowAddr.conjugated() * qElbow).normalized();
}

// The forearm relative to its OWN Address pose — the input to `forearmRotation`.
//
// ⚠ ONE SEGMENT, NOT A JOINT, and that is the whole distinction. Its two siblings
// above take a rotation BETWEEN two segments and re-reference it; this takes one
// segment's own orientation. There is no second triad because there is no second
// sensor: this is what a lone forearm unit can answer, and it is a segment axial
// rotation rather than an ISB joint angle. Fed through forearmPronElbowFlex() the
// twist component is the quantity; the swing component is the forearm's change of
// POINTING, which is not what this metric is about.
//
// ⚠ THE ADDRESS REFERENCE IS LOAD-BEARING AND IS WHAT MAKES THE NUMBER MEAN
// ANYTHING. Each vendor zeroes at its own calibration pose — a wG3 at
// forearm-across-chest palm-down, which is already near full pronation; a Witmotion
// at ours, arm hanging — so the ABSOLUTE angle compares across neither two
// instruments nor two calibrations of the same one. Referenced to Address that
// constant offset cancels and what remains is TRAVEL, which is comparable and is
// the quantity actually wanted: how far the forearm turned between address and
// impact is the direct indicator of flipping. Publishing the un-referenced absolute
// would be publishing the calibration pose. See docs/design/pinpoint_sign_conventions.md,
// "Segment axial rotations", and hm_frame.h's note on why the RATE needs no such
// reference but an ANGLE does.
inline QQuaternion forearmRel(const QQuaternion &qForeAnat, const QQuaternion &qForeAddr)
{
    return (qForeAddr.conjugated() * qForeAnat).normalized();
}

// --- wrist flex/ext + radial/ulnar (cross-talk-safe: swing-twist then Cardan) ---

struct WristAngles {
    double feRad  = 0.0;   // lead-wrist flexion(+) / extension(−)
    double rudRad = 0.0;   // radial(−) / ulnar(+) deviation
};

inline WristAngles wristFlexExtDeviation(const QQuaternion &qWristRel, bool leftArm = false)
{
    // Hardware-locked 2026-06 against ~/pinpoint_wrist_log.csv (lead/left arm). In the
    // imu_calibration anatomical frame the wrist's FLEXION/EXTENSION is rotation about Z
    // and RADIAL/ULNAR deviation is rotation about X — the original spec was right; an
    // earlier "XZY" form had the two axes swapped (so bow/cup read on hinge and vice
    // versa). ZXY Tait-Bryan: flexion (Z) and deviation (X) are extracted and the forearm
    // axial twist (Y) drops out, so pronation never leaks into FE/RUD. FE uses atan2 (no
    // gimbal across wrist ROM); RUD is the asin term but deviation stays far from ±90°.
    //   +flexion = bowed,  +deviation = ulnar (radial negative).
    const QMatrix3x3 R = qWristRel.toRotationMatrix();
    const double feRad  = std::atan2(-R(0, 1), R(1, 1));                 // flexion about Z (+ = bowed)
    const double rudRad = std::asin(std::clamp(R(2, 1), -1.0f, 1.0f));   // deviation about X (+ = ulnar)

    // The lead arm (left, right-handed golfer) reads correct signs with NO mirror —
    // verified on hardware. The right-lead case (left-handed golfer) is not yet verified.
    Q_UNUSED(leftArm);
    return { feRad, rudRad };
}

// --- forearm pronation + elbow flexion (one swing-twist of the elbow rel) -------

struct ForearmElbow {
    double pronRad = 0.0;   // pronation(+) / supination(−), forearm long-axis twist
    double flexRad = 0.0;   // elbow flexion magnitude [0, π]
};

inline ForearmElbow forearmPronElbowFlex(const QQuaternion &qElbowRel, bool leftArm = false)
{
    const QVector3D yLong(0.0f, 1.0f, 0.0f);          // forearm long axis (elbow→wrist)
    const SwingTwist st = swingTwistDecompose(qElbowRel, yLong);

    // pronation = signed twist about the long axis; elbow flexion = swing magnitude.
    const double pronRad = twistAngleRad(st.twist, yLong);
    const QVector3D sv(st.swing.x(), st.swing.y(), st.swing.z());
    const double flexRad = 2.0 * std::atan2(static_cast<double>(sv.length()),
                                            static_cast<double>(st.swing.scalar()));

    // +pronation verified for the lead/left arm with NO mirror (hardware 2026-06).
    Q_UNUSED(leftArm);
    return { pronRad, std::abs(flexRad) };
}

inline double radToDeg(double r) { return r * 180.0 / M_PI; }

// User-facing value string per metric, in PinPoint's coaching convention
// (docs/reference/wristmetrics.md): bow/cup, hinge (ulnar/radial), roll (pronated/supinated);
// other keys fall back to a signed magnitude in degrees. Shared by the offline analyzer
// (wrist_analyzer) and the live check-sensors readout (live_wrist_angles).
inline QString wristMetricLabel(const QString &key, double deg)
{
    const long r = std::lround(deg);
    const long a = r < 0 ? -r : r;
    const QString d = QString::number(a) + QStringLiteral("°");
    if (key == QLatin1String("leadWristFlexExt"))
        return r > 1 ? d + QStringLiteral(" bowed") : r < -1 ? d + QStringLiteral(" cupped") : QStringLiteral("flat");
    if (key == QLatin1String("leadWristRadUln"))
        return r > 1 ? d + QStringLiteral(" ulnar") : r < -1 ? d + QStringLiteral(" radial") : QStringLiteral("neutral");
    if (key == QLatin1String("forearmPronation"))
        return r > 1 ? d + QStringLiteral(" pronated") : r < -1 ? d + QStringLiteral(" supinated") : QStringLiteral("square");
    if (key == QLatin1String("impactShaftLean"))
        return r > 1 ? d + QStringLiteral(" forward") : r < -1 ? d + QStringLiteral(" back") : QStringLiteral("neutral");
    return d;
}

} // namespace pinpoint::analysis
