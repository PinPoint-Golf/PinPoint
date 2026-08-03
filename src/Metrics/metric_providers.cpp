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
    return { QStringLiteral("leadWristFlexExt"), QStringLiteral("leadWristRadUln"),
             QStringLiteral("forearmPronation"), QStringLiteral("leadArmFlexion") };
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
    return {
        QStringLiteral("faceAngle"),      QStringLiteral("faceToPath"),
        QStringLiteral("spinRate"),       QStringLiteral("spinAxis"),
        QStringLiteral("smashFactor"),    QStringLiteral("strikeLocation"),
        QStringLiteral("carryDistance"),  QStringLiteral("dynamicLoft"),
        QStringLiteral("spinLoft"),
    };
}

} // namespace pinpoint::analysis
