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

// The launch monitor panel's ball flight — INTEGRATED, NOT DRAWN. Pure, header-only,
// no Qt-GUI. Sibling of lm_session_reductions.h and governed by the same rule (analysis
// pipeline guide §6.2): QML positions and paints, C++ decides the numbers.
//
// WHY A MODEL AND NOT AN ARCH. A bezier through launch, apex and landing gets all three
// of those points right and every point between them wrong, in a way that is visible:
// it is symmetric. A real ball spends the back half of its flight slower and steeper
// than the front half, so its apex sits past the midpoint (near 63% of carry here) and
// it comes down at an angle visibly steeper than it left at. THAT ASYMMETRY IS THE ONLY
// REASON THE CURVE IS WORTH DRAWING — a symmetric arch tells the golfer nothing their
// carry number did not already say. So the shape comes from integrating the ball.
//
// WHAT IS MODELLED AND WHAT IS MEASURED. The integration produces a whole trajectory
// from the launch conditions the device reported; the device also reports where that
// trajectory ENDED. Those two disagree slightly, and we trust the device on endpoints
// and the model on shape: lmNormalisedPath() scales the curve so carry, apex and
// offline land exactly on the reported values while every intermediate point keeps the
// model's proportions. The alternative — drawing the raw model — would put the ball
// down somewhere other than where the card says it landed, on the same card.
//
// THE OFFLINE RESIDUAL IS KEPT, NOT SWALLOWED. For the reference shot the spin axis
// accounts for 1.6 yd of a reported 5.1 yd finish. Normalisation stretches the curve to
// cover the difference, which is the right thing for the drawing and the wrong thing to
// forget: either the axis reading is soft on that strike or there was wind, and both are
// worth knowing. residualOfflineYd carries it out so a caller can say so. Nothing in the
// panel renders it yet — that needs a handful of shots in known conditions before the
// card claims a cause — but the number costs nothing to keep and everything to re-derive.
//
// Unit-tested standalone in src/Analysis/tests/lm_flight_path_test.cpp against the
// reference row in the design brief.

#include <algorithm>
#include <cmath>
#include <vector>

namespace pinpoint::analysis {

// Unit conversions, named once. Every public number in and out of this header is in the
// catalogue's units (mph, °, rpm, yd, ft); the integration is metric internally because
// the aerodynamics are.
inline constexpr double kMphToMs   = 0.44704;
inline constexpr double kMToYd     = 1.0 / 0.9144;
inline constexpr double kMToFt     = 1.0 / 0.3048;
inline constexpr double kRpmToRads = 2.0 * M_PI / 60.0;
inline constexpr double kDegToRad  = M_PI / 180.0;
inline constexpr double kRadToDeg  = 180.0 / M_PI;

// Standard sphere aerodynamics for a golf ball. A DEFAULTED STRUCT RATHER THAN LITERALS
// because these are configuration: the lift curve in particular is an empirical fit that
// a later ball model, or a wind-tunnel number someone trusts more, should be able to
// replace without touching the integrator or hunting constants through view code.
//
// Cd and Cl are functions of the spin factor S = r·omega/|v| — the surface speed of the
// ball's skin as a fraction of its speed through the air, which is the quantity both
// coefficients actually depend on. Cl saturates: past roughly S = 0.29 a golf ball stops
// generating more lift, and without the clamp a wedge's 11,000 rpm would fly it upwards.
struct LmFlightModel {
    double massKg   = 0.04593;    // R&A / USGA maximum
    double radiusM  = 0.02134;    // R&A / USGA minimum diameter, halved
    double rho      = 1.225;      // ISA sea level, 15 °C
    double g        = 9.80665;
    double dtS      = 0.0005;     // forward Euler; see the note on step size below

    double cd0      = 0.255;      // Cd = cd0 + cdS·S
    double cdS      = 0.16;
    double clS      = 1.90;       // Cl = clamp(clS·S − clS2·S², 0, clMax)
    double clS2     = 3.25;
    double clMax    = 0.45;

    double areaM2() const { return M_PI * radiusM * radiusM; }
};

// What the device measured about the strike, in catalogue units. Everything the
// integration needs and nothing it does not.
struct LmLaunch {
    double ballSpeedMph   = 0.0;
    double launchAngleDeg = 0.0;   // above horizontal
    double startDirDeg    = 0.0;   // + = right of target for a right-hander
    double spinRpm        = 0.0;   // total spin
    double spinAxisDeg    = 0.0;   // + = tilted right for a right-hander
};

// A point on the flight. Which units these carry depends on which function produced it —
// lmIntegrateFlight() returns yards and feet, lmNormalisedPath() returns fractions — and
// each says so. Kept as one struct because the geometry is identical either way.
struct LmPoint {
    double x = 0.0;   // downrange
    double y = 0.0;   // height
    double z = 0.0;   // lateral, + = right for a right-hander
};

// The raw integration: real units, tee at the origin, ending the instant the ball
// touches down.
struct LmFlightIntegration {
    bool has = false;
    std::vector<LmPoint> points;      // x, z in yd; y in ft
    double carryYd     = 0.0;
    double apexFt      = 0.0;
    double descentDeg  = 0.0;         // below horizontal at touchdown, positive
    double offlineYd   = 0.0;         // at touchdown
    double launchDeg   = 0.0;         // echoed back, so the caller can compare the two
    double apexFraction = 0.0;        // apex's x as a fraction of carry
    // The ball's ground heading as it lands, which is where any roll continues. Not the
    // start direction: the curve has been bending the whole way down.
    double landingHeadingDeg = 0.0;
};

// Integrate the ball from the measured launch conditions.
//
// FORWARD EULER AT HALF A MILLISECOND. Euler is the crudest integrator there is and it
// is the right one here: the step is small enough that its error is far below the
// device's own resolution (halving it again moves carry by centimetres), the whole
// flight is ~13,000 steps of a dozen flops, and the alternative — RK4 — would buy
// accuracy this drawing cannot express against a curve that gets rescaled to the
// device's endpoints anyway. Simplicity that can be read and checked wins.
inline LmFlightIntegration lmIntegrateFlight(const LmLaunch &in,
                                             const LmFlightModel &m = LmFlightModel())
{
    LmFlightIntegration out;
    if (!std::isfinite(in.ballSpeedMph) || in.ballSpeedMph <= 0.0
        || !std::isfinite(in.launchAngleDeg) || in.launchAngleDeg <= 0.0
        || !std::isfinite(in.spinRpm) || in.spinRpm < 0.0)
        return out;                       // no launch, no flight — and no invented one

    const double startDir = std::isfinite(in.startDirDeg) ? in.startDirDeg : 0.0;
    const double axis     = std::isfinite(in.spinAxisDeg) ? in.spinAxisDeg : 0.0;

    // Right-handed frame: x downrange, y up, z right. (x cross y = z, so z really is
    // the golfer's right when x points at the target.)
    const double v0    = in.ballSpeedMph * kMphToMs;
    const double la    = in.launchAngleDeg * kDegToRad;
    const double sd    = startDir * kDegToRad;
    const double axisR = axis * kDegToRad;

    double px = 0.0, py = 0.0, pz = 0.0;
    double vx = v0 * std::cos(la) * std::cos(sd);
    double vy = v0 * std::sin(la);
    double vz = v0 * std::cos(la) * std::sin(sd);

    const double omega = in.spinRpm * kRpmToRads;
    const double half  = 0.5 * m.rho * m.areaM2() / m.massKg;   // per-unit-mass constant
    const double dt    = m.dtS > 0.0 ? m.dtS : 0.0005;

    out.points.reserve(16384);
    out.points.push_back({ 0.0, 0.0, 0.0 });

    double apexM = 0.0, apexXm = 0.0;
    // A hard step ceiling so a pathological input (a Cl that keeps it climbing) cannot
    // spin here forever. 120 s of flight is four times the longest real golf shot.
    const int maxSteps = int(120.0 / dt);

    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;
    for (int i = 0; i < maxSteps; ++i) {
        const double sp = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (!(sp > 0.1))
            break;

        // Spin factor, and the two coefficients it drives.
        const double S  = m.radiusM * omega / sp;
        const double Cd = m.cd0 + m.cdS * S;
        const double Cl = std::clamp(m.clS * S - m.clS2 * S * S, 0.0, m.clMax);

        // Lift acts perpendicular to velocity. Build that perpendicular explicitly: the
        // horizontal right-hand normal to the flight path, then the "up" normal from it,
        // then tilt the pair about the velocity by the spin axis. Doing it as a basis
        // rather than as two independent components is what keeps the lift genuinely
        // perpendicular once the ball is climbing steeply — a small-angle shortcut here
        // quietly adds drag or thrust on a wedge.
        double sx = -vz, sy = 0.0, sz = vx;              // v cross up
        double sn = std::sqrt(sx * sx + sz * sz);
        if (sn < 1e-9) { sx = 0.0; sy = 0.0; sz = 1.0; sn = 1.0; }
        sx /= sn; sy /= sn; sz /= sn;

        const double ivx = vx / sp, ivy = vy / sp, ivz = vz / sp;
        // up-normal = side cross velocity
        const double ux = sy * ivz - sz * ivy;
        const double uy = sz * ivx - sx * ivz;
        const double uz = sx * ivy - sy * ivx;

        const double ca = std::cos(axisR), sa = std::sin(axisR);
        const double lx = ux * ca + sx * sa;
        const double ly = uy * ca + sy * sa;
        const double lz = uz * ca + sz * sa;

        const double drag = half * Cd * sp;              // × velocity vector below
        const double lift = half * Cl * sp * sp;

        const double ax = -drag * vx + lift * lx;
        const double ay = -drag * vy + lift * ly - m.g;
        const double az = -drag * vz + lift * lz;

        prevX = px; prevY = py; prevZ = pz;
        vx += ax * dt; vy += ay * dt; vz += az * dt;
        px += vx * dt; py += vy * dt; pz += vz * dt;

        if (py > apexM) { apexM = py; apexXm = px; }

        if (py <= 0.0 && i > 0) {
            // Land ON the ground rather than below it: interpolate the crossing, so
            // carry does not depend on where the step boundary happened to fall.
            const double t = (prevY - py) > 1e-12 ? prevY / (prevY - py) : 1.0;
            px = prevX + (px - prevX) * t;
            pz = prevZ + (pz - prevZ) * t;
            py = 0.0;
            out.points.push_back({ px * kMToYd, 0.0, pz * kMToYd });

            const double horiz = std::sqrt(vx * vx + vz * vz);
            out.descentDeg = horiz > 1e-9 ? std::atan2(-vy, horiz) * kRadToDeg : 90.0;
            out.landingHeadingDeg = std::atan2(vz, vx) * kRadToDeg;
            out.has = true;
            break;
        }

        // One sample per millisecond is more than the drawing can resolve and keeps the
        // vector small enough to hand across to QML without thinning it afterwards.
        if ((i % 2) == 0)
            out.points.push_back({ px * kMToYd, py * kMToFt, pz * kMToYd });
    }

    if (!out.has)
        return LmFlightIntegration{};

    out.carryYd    = px * kMToYd;
    out.apexFt     = apexM * kMToFt;
    out.offlineYd  = pz * kMToYd;
    out.launchDeg  = in.launchAngleDeg;
    out.apexFraction = out.carryYd > 1e-9 ? (apexXm * kMToYd) / out.carryYd : 0.0;
    return out;
}

// The flight as the card draws it: normalised, with the device's endpoints.
//
// x runs 0 at the tee to 1 at TOTAL distance (so the roll fits on the same axis as the
// trajectory, which is what makes one shared distance ruler honest). y runs 0 at the
// ground to 1 at apex. z runs −1…1 across the lateral extent the shot actually used,
// with lateralExtentYd saying what 1 means — the card cannot label an axis it does not
// know the scale of, and a fixed lateral scale would flatten every straight shot to a
// line and clip every big slice.
struct LmFlightPath {
    bool has = false;

    std::vector<LmPoint> points;   // the flight, normalised as above
    LmPoint landing;               // touchdown = end of the trajectory
    LmPoint finish;                // end of the roll (= total distance)

    double carryFraction   = 1.0;  // normalised x at touchdown; 1.0 when total == carry
    double apexAtX         = 0.0;  // normalised x of the apex
    double apexFractionOfCarry = 0.0;
    double lateralExtentYd = 0.0;  // what |z| = 1 means

    // Tangent directions at both ends, in normalised units, for the short launch/landing
    // guide strokes on the card. Held as unit vectors rather than angles because the two
    // axes are scaled differently and an angle would be wrong in the drawn space.
    LmPoint launchTangent;
    LmPoint landingTangent;

    // What is drawn (the device's) beside what was modelled (ours). Both, because the
    // gap between them is a fact about the shot and not an embarrassment to hide.
    double carryYd = 0.0, apexFt = 0.0, descentDeg = 0.0, offlineYd = 0.0, totalYd = 0.0;
    double modelCarryYd = 0.0, modelApexFt = 0.0, modelDescentDeg = 0.0, modelOfflineYd = 0.0;
    double residualOfflineYd = 0.0;   // reported − modelled, signed
};

// Scale the integrated shape onto the device's reported endpoints.
//
// Each axis is scaled independently and each by a single factor, which is what preserves
// the shape: the apex still sits at the same FRACTION of carry, the descent is still
// steeper than the launch by the same ratio, and only the units change. A per-point
// correction would land the endpoints just as well and would no longer be the model's
// curve — it would be an arch again, drawn the long way round.
//
// A reported value that is absent or nonsensical falls back to the model's own, so a
// device that reports carry but not apex still draws a correctly-shaped flight.
inline LmFlightPath lmNormalisedPath(const LmFlightIntegration &raw,
                                     double reportedCarryYd,
                                     double reportedApexFt,
                                     double reportedOfflineYd,
                                     double reportedTotalYd)
{
    LmFlightPath out;
    if (!raw.has || raw.points.size() < 3 || raw.carryYd <= 0.0 || raw.apexFt <= 0.0)
        return out;

    const bool haveCarry   = std::isfinite(reportedCarryYd)   && reportedCarryYd > 0.0;
    const bool haveApex    = std::isfinite(reportedApexFt)    && reportedApexFt > 0.0;
    const bool haveOffline = std::isfinite(reportedOfflineYd);

    out.carryYd   = haveCarry   ? reportedCarryYd   : raw.carryYd;
    out.apexFt    = haveApex    ? reportedApexFt    : raw.apexFt;
    out.offlineYd = haveOffline ? reportedOfflineYd : raw.offlineYd;
    out.descentDeg = raw.descentDeg;
    // Total below carry is a device quirk or a plugged lie, not a shot that rolled
    // backwards; clamp rather than draw a roll pointing at the golfer.
    out.totalYd = (std::isfinite(reportedTotalYd) && reportedTotalYd > out.carryYd)
                      ? reportedTotalYd : out.carryYd;

    out.modelCarryYd   = raw.carryYd;
    out.modelApexFt    = raw.apexFt;
    out.modelDescentDeg = raw.descentDeg;
    out.modelOfflineYd = raw.offlineYd;
    out.residualOfflineYd = out.offlineYd - raw.offlineYd;

    const double sx = out.carryYd / raw.carryYd;
    const double sy = out.apexFt  / raw.apexFt;
    // A dead-straight model shot has no lateral shape to scale. Fall back to shifting
    // the finish linearly with distance, which is the only defensible curve through
    // "started straight, finished 5 yd right" when the model says it never moved.
    const bool lateralFromModel = std::abs(raw.offlineYd) > 1e-6;
    const double sz = lateralFromModel ? (out.offlineYd / raw.offlineYd) : 0.0;

    // The roll runs on from touchdown, drifting sideways along the ball's landing
    // heading. Its DOWNRANGE extent is the device's total distance, not carry plus the
    // heading's cosine: total distance is the number the card's ruler is labelled with,
    // and an axis that ended a yard short of its own label would be wrong in the one
    // place the reader is looking. The heading decides only how far right or left the
    // ball ran, which is the part the device did not report.
    const double rollYd = out.totalYd - out.carryYd;
    const double head   = raw.landingHeadingDeg * kDegToRad;
    const double rollZ  = rollYd * std::sin(head);

    const double spanX = out.totalYd;
    if (!(spanX > 1e-9))
        return out;

    // The lateral extent every z is a fraction of: the widest the ball got, or where it
    // finished, whichever is further out — and never zero, so a straight shot draws a
    // straight line rather than dividing by nothing.
    double maxAbsZ = std::abs(out.offlineYd + rollZ);
    for (const LmPoint &p : raw.points) {
        const double z = lateralFromModel ? p.z * sz : out.offlineYd * (p.x / raw.carryYd);
        maxAbsZ = std::max(maxAbsZ, std::abs(z));
    }
    out.lateralExtentYd = std::max(maxAbsZ, 1e-6);

    out.points.reserve(raw.points.size());
    for (const LmPoint &p : raw.points) {
        const double zy = lateralFromModel ? p.z * sz : out.offlineYd * (p.x / raw.carryYd);
        out.points.push_back({ (p.x * sx) / spanX,
                               (p.y * sy) / out.apexFt,
                               zy / out.lateralExtentYd });
    }

    out.landing = out.points.back();
    out.finish  = { 1.0, 0.0, (out.offlineYd + rollZ) / out.lateralExtentYd };
    out.carryFraction = out.landing.x;
    out.apexFractionOfCarry = raw.apexFraction;
    out.apexAtX = out.carryFraction * raw.apexFraction;

    // Tangents in the DRAWN space, taken across a few samples so a single Euler step's
    // noise cannot tip them.
    const auto unit = [](LmPoint v) {
        const double n = std::sqrt(v.x * v.x + v.y * v.y);
        return n > 1e-12 ? LmPoint{ v.x / n, v.y / n, 0.0 } : LmPoint{ 1.0, 0.0, 0.0 };
    };
    const size_t k = std::min<size_t>(4, out.points.size() - 1);
    const LmPoint &a0 = out.points.front(), &a1 = out.points[k];
    const LmPoint &b0 = out.points[out.points.size() - 1 - k], &b1 = out.points.back();
    out.launchTangent  = unit({ a1.x - a0.x, a1.y - a0.y, 0.0 });
    out.landingTangent = unit({ b1.x - b0.x, b1.y - b0.y, 0.0 });

    out.has = true;
    return out;
}

} // namespace pinpoint::analysis
