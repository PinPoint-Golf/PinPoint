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

#include "metric_resolver.h"   // describeRequirement — shared "why unavailable" renderer

namespace pinpoint::analysis {

namespace {

// Wrist Motion (sessionType 1) profile gate. -1 (no shot / directory browse) is session-agnostic.
bool wristSessionOk(int sessionType) { return sessionType == -1 || sessionType == 1; }

// Turn a requirement into a Measured/Unavailable verdict for one shot, reusing the shared reason
// renderer so provider reasons and the no-provider fallback read identically.
MetricAvailability fromRequirement(const MetricRequirement &req, const ShotContext &ctx)
{
    MetricAvailability a;
    a.tier = ctx.tier;
    const QString missing = describeRequirement(req, ctx);
    if (missing.isEmpty()) {
        a.state = MetricAvailability::Measured;
    } else {
        a.state  = MetricAvailability::Unavailable;
        a.reason = missing;
    }
    return a;
}

MetricAvailability wristSessionOnly()
{
    MetricAvailability a;
    a.state  = MetricAvailability::Unavailable;
    a.reason = QStringLiteral("produced in Wrist Motion sessions only");
    return a;
}

} // namespace

// ---------------------------------------------------------------------------- WristMetricProvider

std::vector<QString> WristMetricProvider::provides() const
{
    return { QStringLiteral("leadWristFlexExt"), QStringLiteral("leadWristRadUln"),
             QStringLiteral("forearmPronation"), QStringLiteral("leadArmFlexion") };
}

MetricAvailability WristMetricProvider::availability(const QString &key, const ShotContext &ctx) const
{
    if (!wristSessionOk(ctx.sessionType))
        return wristSessionOnly();

    MetricRequirement req;
    req.imuRoles = { SegmentRole::LeadForearm, SegmentRole::LeadHand };
    if (key == QLatin1String("forearmPronation") || key == QLatin1String("leadArmFlexion"))
        req.imuRoles.push_back(SegmentRole::LeadUpperArm);   // needs the upper-arm binding too
    return fromRequirement(req, ctx);
}

// ------------------------------------------------------------------------- KinematicSeriesProvider

std::vector<QString> KinematicSeriesProvider::provides() const
{
    return { QStringLiteral("clubheadSpeed"), QStringLiteral("handSpeed"),
             QStringLiteral("lagAngle") };
}

MetricAvailability KinematicSeriesProvider::availability(const QString &key, const ShotContext &ctx) const
{
    MetricRequirement req;
    req.clubTrack = true;                         // speeds + lag all read the shaft/club track
    if (key == QLatin1String("lagAngle"))
        req.faceOnCamera = true;                  // lag additionally reads lead-forearm pose
    return fromRequirement(req, ctx);
}

// ----------------------------------------------------------------------------- FootMetricProvider

std::vector<QString> FootMetricProvider::provides() const
{
    return { QStringLiteral("stanceWidth"), QStringLiteral("leadFootFlare"),
             QStringLiteral("trailFootFlare"), QStringLiteral("toeLineAngle"),
             QStringLiteral("leadHeelLift"), QStringLiteral("ballPosition") };
}

MetricAvailability FootMetricProvider::availability(const QString &key, const ShotContext &ctx) const
{
    if (!wristSessionOk(ctx.sessionType))
        return wristSessionOnly();

    MetricRequirement req;
    req.faceOnCamera = true;                       // whole-body pose feet keypoints
    // ballPosition is the one key here that needs more than the feet: without a
    // detected ball there is nothing to locate along the stance. (This provider
    // used to be key-agnostic; it no longer can be.)
    if (key == QStringLiteral("ballPosition"))
        req.ballTrack = true;
    return fromRequirement(req, ctx);
}

// ------------------------------------------------------------------------------------ TempoProvider

std::vector<QString> TempoProvider::provides() const
{
    return { QStringLiteral("tempoBackswing"), QStringLiteral("tempoRatio") };
}

MetricAvailability TempoProvider::availability(const QString &key, const ShotContext &ctx) const
{
    Q_UNUSED(key)
    if (!wristSessionOk(ctx.sessionType))
        return wristSessionOnly();
    // Deliberately EMPTY: tempo needs only a phase ladder, and either an IMU or a
    // face-on camera can produce one. Requiring both would be wrong and requiring
    // either is not expressible here — so the capability answer is "yes", and the
    // per-shot honesty lives in the producer, which refuses an unreliable ladder
    // outright rather than emitting a plausible-looking wrong number.
    return fromRequirement(MetricRequirement{}, ctx);
}

// ------------------------------------------------------------------------------ HeadMetricProvider

std::vector<QString> HeadMetricProvider::provides() const
{
    return { QStringLiteral("headSway"), QStringLiteral("headLift"), QStringLiteral("headTilt") };
}

MetricAvailability HeadMetricProvider::availability(const QString &key, const ShotContext &ctx) const
{
    Q_UNUSED(key)
    if (!wristSessionOk(ctx.sessionType))
        return wristSessionOnly();
    MetricRequirement req;
    req.faceOnCamera = true;                       // head keypoints from the face-on pose
    return fromRequirement(req, ctx);
}

// ------------------------------------------------------------------------------- ShaftLeanProvider

std::vector<QString> ShaftLeanProvider::provides() const
{
    return { QStringLiteral("impactShaftLean") };
}

MetricAvailability ShaftLeanProvider::availability(const QString &key, const ShotContext &ctx) const
{
    Q_UNUSED(key)
    if (!wristSessionOk(ctx.sessionType))
        return wristSessionOnly();
    MetricRequirement req;
    req.faceOnCamera = true;
    req.clubTrack    = true;                        // shaft lean from the club/shaft track
    return fromRequirement(req, ctx);
}

// ---------------------------------------------------------------------------------- ScoreProvider

std::vector<QString> ScoreProvider::provides() const
{
    return { QStringLiteral("wristScore"), QStringLiteral("wristResemblance"),
             QStringLiteral("swingScore") };
}

MetricAvailability ScoreProvider::availability(const QString &key, const ShotContext &ctx) const
{
    // Swing/GRF/Coach adherence score: no live producer yet (SwingScorer is dead; the analyzer
    // stubs it). Aspirational — declared so the directory documents it, always Unavailable.
    if (key == QLatin1String("swingScore")) {
        MetricAvailability a;
        a.tier   = ctx.tier;
        a.state  = MetricAvailability::Unavailable;
        a.reason = QStringLiteral("no live scorer yet — swing adherence scorer not wired");
        return a;
    }

    // wristScore / wristResemblance: live for the Wrist session, from the lead-wrist series.
    if (!wristSessionOk(ctx.sessionType))
        return wristSessionOnly();
    MetricRequirement req;
    req.imuRoles = { SegmentRole::LeadForearm, SegmentRole::LeadHand };
    return fromRequirement(req, ctx);
}

// -------------------------------------------------------------------------- PlannedMetricProvider

std::vector<QString> PlannedMetricProvider::provides() const
{
    // The design-catalogue metrics with no producer in this build. Keep in sync with the manifest's
    // `.planned = true` descriptors (the metric_catalogue_test asserts they resolve Unavailable).
    return {
        QStringLiteral("pelvisRotation"),   QStringLiteral("thoraxRotation"),
        QStringLiteral("xFactor"),          QStringLiteral("xFactorStretch"),
        QStringLiteral("hipInternalRotation"),
        QStringLiteral("spineForwardBend"), QStringLiteral("spineSideBend"),
        QStringLiteral("secondaryAxisTilt"),
        QStringLiteral("pelvisSway"),       QStringLiteral("pelvisThrust"),
        QStringLiteral("pelvisLift"),
        QStringLiteral("swingPlane"),       QStringLiteral("clubPath"),
        QStringLiteral("attackAngle"),      QStringLiteral("faceAngle"),
        QStringLiteral("lowPointAhead"),
        QStringLiteral("kinematicSequence"),
        QStringLiteral("shoulderAlignment"), QStringLiteral("elbowAlignment"),
        QStringLiteral("hipAlignment"),      QStringLiteral("feetAlignment"),
    };
}

MetricAvailability PlannedMetricProvider::availability(const QString &key, const ShotContext &ctx) const
{
    Q_UNUSED(key)
    MetricAvailability a;
    a.tier   = ctx.tier;
    a.state  = MetricAvailability::Unavailable;
    a.reason = QStringLiteral("planned — not yet produced in this build");
    return a;
}

} // namespace pinpoint::analysis
