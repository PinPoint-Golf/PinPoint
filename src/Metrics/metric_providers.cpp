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

#include "metric_providers.h"

// CLAIM LISTS ONLY. Not one class here answers availability: the seam's default walks the metric's
// route ladder from the manifest (metric_provider.h `resolveRoutes`), which is the single statement
// of what a metric needs and how well each route gets it.
//
// Every list below must stay in step with what its producer stage actually emits, in BOTH
// directions. Omitting a key does not make it planned or hidden — it makes it belong to no provider,
// and the metric then falls back to its own ladder with nothing having claimed it, which the
// catalogue test treats as a defect for any metric with a live route. `stanceWidthMm` shipped that
// way: produced on every ruler-resolved swing, claimed by nobody, reported Unavailable on all of
// them, and indistinguishable from an honest answer in the directory.

namespace pinpoint::analysis {

// ---------------------------------------------------------------------------- WristMetricProvider

std::vector<QString> WristMetricProvider::provides() const
{
    // ONE PROVIDER FOR BOTH INSTRUMENTS, because there is one producer: MetricExtractor runs the
    // same arithmetic over the same fused streams whichever device filled them, and only the key it
    // writes differs. A second provider for the `hm.` keys would say in the architecture that there
    // are two producers, which is the exact thing the identical-maths rule exists to prevent.
    return { QStringLiteral("leadWristFlexExt"), QStringLiteral("leadWristRadUln"),
             QStringLiteral("forearmPronation"), QStringLiteral("leadArmFlexion"),
             // A segment axial rotation from the forearm alone — produced for either vendor, and
             // for a three-sensor rig BESIDE forearmPronation rather than instead of it.
             QStringLiteral("forearmRotation"),
             // The HackMotion rungs. Claimed here so a wG3 swing's metrics resolve Measured rather
             // than reporting Unavailable while sitting in the document — the failure mode
             // stanceWidthMm shipped with, and the reason this list is checked at all.
             QStringLiteral("hm.leadWristFlexExt"), QStringLiteral("hm.leadWristRadUln"),
             QStringLiteral("hm.forearmRotation") };
}

// ------------------------------------------------------------------------- KinematicSeriesProvider

std::vector<QString> KinematicSeriesProvider::provides() const
{
    return { QStringLiteral("clubheadSpeed"), QStringLiteral("handSpeed"),
             QStringLiteral("lagAngle") };
}

// ----------------------------------------------------------------------------- FootMetricProvider

std::vector<QString> FootMetricProvider::provides() const
{
    return { QStringLiteral("stanceWidth"), QStringLiteral("stanceWidthMm"),
             QStringLiteral("leadFootFlare"),
             QStringLiteral("trailFootFlare"), QStringLiteral("toeLineAngle"),
             QStringLiteral("leadHeelLift"), QStringLiteral("ballPosition") };
}

// ------------------------------------------------------------------- LowerBodyMetricProvider

std::vector<QString> LowerBodyMetricProvider::provides() const
{
    return { QStringLiteral("leadKneeDrift"),  QStringLiteral("pelvisSway"),
             QStringLiteral("pelvisLift"),     QStringLiteral("hipLineTilt"),
             QStringLiteral("feetAlignment"),  QStringLiteral("comOverLeadFoot") };
}

// ------------------------------------------------------------------- UpperBodyMetricProvider

std::vector<QString> UpperBodyMetricProvider::provides() const
{
    return { QStringLiteral("secondaryAxisTilt"),   QStringLiteral("spineSideBend"),
             QStringLiteral("thoraxLateralDrift"),  QStringLiteral("shoulderPlaneAngle"),
             QStringLiteral("elbowAlignment"),      QStringLiteral("trailElbowHeight"),
             QStringLiteral("leadHandWidth"),       QStringLiteral("leadUpperArmToChest"),
             QStringLiteral("leadArmToTorso") };
}

// ------------------------------------------------------------------------- TrailWristProvider

std::vector<QString> TrailWristProvider::provides() const
{
    return { QStringLiteral("trailWristFlexExt") };
}

// ----------------------------------------------------------------------- BodyRotationProvider

std::vector<QString> BodyRotationProvider::provides() const
{
    return { QStringLiteral("pelvisRotation"), QStringLiteral("thoraxRotation"),
             QStringLiteral("xFactor"),        QStringLiteral("xFactorStretch") };
}

// ----------------------------------------------------------------------- ClubDeliveryProvider

std::vector<QString> ClubDeliveryProvider::provides() const
{
    return { QStringLiteral("shaftAngleVsHorizontal"), QStringLiteral("attackAngle"),
             QStringLiteral("lowPointAhead") };
}

// ------------------------------------------------------------------------------------ TempoProvider

std::vector<QString> TempoProvider::provides() const
{
    return { QStringLiteral("tempoBackswing"), QStringLiteral("tempoRatio") };
}

// ------------------------------------------------------------------------------ HeadMetricProvider

std::vector<QString> HeadMetricProvider::provides() const
{
    return { QStringLiteral("headSway"), QStringLiteral("headLift"), QStringLiteral("headTilt") };
}

// ------------------------------------------------------------------------------- ShaftLeanProvider

std::vector<QString> ShaftLeanProvider::provides() const
{
    return { QStringLiteral("impactShaftLean") };
}

// ------------------------------------------------------------------------------ ShaftPlaneProvider

std::vector<QString> ShaftPlaneProvider::provides() const
{
    return { QStringLiteral("transitionPlaneDelta") };
}

// ---------------------------------------------------------------------------------- ScoreProvider

std::vector<QString> ScoreProvider::provides() const
{
    // swingScore is claimed even though its only route is planned, because a claim is a statement
    // about WHOSE metric this is, not about whether it resolves today. When the adherence scorer is
    // wired, dropping `.planned` from the route is the whole change.
    return { QStringLiteral("wristScore"), QStringLiteral("wristResemblance"),
             QStringLiteral("swingScore") };
}

// -------------------------------------------------------------------------- LaunchMonitorProvider

std::vector<QString> LaunchMonitorProvider::provides() const
{
    // Every key here is `lm.`-prefixed, and the seven that duplicate a quantity we
    // estimate ourselves are NOT claimed under their bare key. That is the whole
    // point: the bare `clubheadSpeed` stays with the camera producer so the two can
    // be compared on the same shot. Claiming both here would make the resolver pick
    // one, which is exactly what must not happen.
    return {
        // Measured where we also estimate — the validation pairs.
        QStringLiteral("lm.clubheadSpeed"),  QStringLiteral("lm.ballSpeed"),
        QStringLiteral("lm.attackAngle"),    QStringLiteral("lm.clubPath"),
        QStringLiteral("lm.launchAngle"),    QStringLiteral("lm.launchDirection"),
        QStringLiteral("lm.lowPointAhead"),
        // Club delivery no camera of ours can resolve.
        QStringLiteral("lm.faceAngle"),      QStringLiteral("lm.faceToPath"),
        QStringLiteral("lm.dynamicLoft"),    QStringLiteral("lm.spinLoft"),
        QStringLiteral("lm.lieAngle"),       QStringLiteral("lm.closureRate"),
        // Strike.
        QStringLiteral("lm.smashFactor"),    QStringLiteral("lm.strikeLocation"),
        QStringLiteral("lm.strikeHeight"),
        // Spin.
        QStringLiteral("lm.spinRate"),       QStringLiteral("lm.backSpin"),
        QStringLiteral("lm.sideSpin"),       QStringLiteral("lm.spinAxis"),
        // Flight-model outputs.
        QStringLiteral("lm.carryDistance"),  QStringLiteral("lm.totalDistance"),
        QStringLiteral("lm.offline"),        QStringLiteral("lm.peakHeight"),
        QStringLiteral("lm.descentAngle"),   QStringLiteral("lm.distanceToPin"),
    };
}

// ------------------------------------------------------------- LaunchMonitorDerivedProvider

std::vector<QString> LaunchMonitorDerivedProvider::provides() const
{
    // Bare keys, on purpose — see the class comment and the manifest block above `compoundMiss`.
    // Produced in swing_doc.cpp beside the readings rather than in an analysis stage, because a
    // reading is paired to a swing after the stages have run.
    return { QStringLiteral("compoundMiss") };
}

} // namespace pinpoint::analysis
