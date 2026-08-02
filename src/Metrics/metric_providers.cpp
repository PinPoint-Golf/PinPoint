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

} // namespace

// ---------------------------------------------------------------------------- WristMetricProvider

std::vector<QString> WristMetricProvider::provides() const
{
    return { QStringLiteral("leadWristFlexExt"), QStringLiteral("leadWristRadUln"),
             QStringLiteral("forearmPronation"), QStringLiteral("leadArmFlexion") };
}

MetricAvailability WristMetricProvider::availability(const QString &key, const ShotContext &ctx) const
{
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
    return { QStringLiteral("stanceWidth"), QStringLiteral("stanceWidthMm"),
             QStringLiteral("leadFootFlare"),
             QStringLiteral("trailFootFlare"), QStringLiteral("toeLineAngle"),
             QStringLiteral("leadHeelLift"), QStringLiteral("ballPosition") };
}

MetricAvailability FootMetricProvider::availability(const QString &key, const ShotContext &ctx) const
{
    MetricRequirement req;
    req.faceOnCamera = true;                       // whole-body pose feet keypoints
    // Three keys here need more than the feet, and all three need it for the same reason: a
    // ball. ballPosition has nothing to locate along the stance without one; stanceWidthMm and
    // leadHeelLift are the two readings in real-world units, and the ball diameter IS the ruler
    // that gets them there (foot_metrics.cpp emits both only when mmPerPx resolved).
    //
    // Omitting a key from this list does not make it planned or hidden — it makes it belong to
    // NO provider, and MetricCatalogue::resolve() then answers Unavailable on every shot however
    // capable. stanceWidthMm shipped that way: produced on every ruler-resolved swing and
    // reported as unavailable on all of them. Keep this list and provides() in step.
    if (key == QStringLiteral("ballPosition") || key == QStringLiteral("stanceWidthMm")
        || key == QStringLiteral("leadHeelLift"))
        req.ballTrack = true;
    return fromRequirement(req, ctx);
}

// ------------------------------------------------------------------- LowerBodyMetricProvider

std::vector<QString> LowerBodyMetricProvider::provides() const
{
    return { QStringLiteral("leadKneeDrift"),  QStringLiteral("pelvisSway"),
             QStringLiteral("pelvisLift"),     QStringLiteral("hipLineTilt"),
             QStringLiteral("feetAlignment"),  QStringLiteral("comOverLeadFoot") };
}

MetricAvailability LowerBodyMetricProvider::availability(const QString &key,
                                                         const ShotContext &ctx) const
{
    Q_UNUSED(key)
    MetricRequirement req;
    req.faceOnCamera = true;      // frontal-plane pose: hips, knees, ankles
    return fromRequirement(req, ctx);
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

MetricAvailability UpperBodyMetricProvider::availability(const QString &key,
                                                         const ShotContext &ctx) const
{
    Q_UNUSED(key)
    MetricRequirement req;
    req.faceOnCamera = true;      // frontal-plane pose: shoulders, elbows, wrists, hips, ankles
    return fromRequirement(req, ctx);
}

// ------------------------------------------------------------------------- TrailWristProvider

std::vector<QString> TrailWristProvider::provides() const
{
    return { QStringLiteral("trailWristFlexExt") };
}

MetricAvailability TrailWristProvider::availability(const QString &key, const ShotContext &ctx) const
{
    Q_UNUSED(key)
    MetricRequirement req;
    req.faceOnCamera = true;      // and the WholeBody hand keypoints — see the header
    return fromRequirement(req, ctx);
}

// ----------------------------------------------------------------------- BodyRotationProvider

std::vector<QString> BodyRotationProvider::provides() const
{
    return { QStringLiteral("pelvisRotation"), QStringLiteral("thoraxRotation"),
             QStringLiteral("xFactor"),        QStringLiteral("xFactorStretch") };
}

MetricAvailability BodyRotationProvider::availability(const QString &key, const ShotContext &ctx) const
{
    // Per-segment: which roles does THIS key need, and does the shot have them?
    const bool needsPelvis = key != QLatin1String("thoraxRotation");
    const bool needsThorax = key != QLatin1String("pelvisRotation");
    const bool hasPelvis   = !needsPelvis || ctx.hasRole(SegmentRole::Pelvis);
    const bool hasThorax   = !needsThorax || ctx.hasRole(SegmentRole::Thorax);

    MetricAvailability a;
    a.tier = ctx.tier;

    // Every segment this key needs is instrumented — the turn is measured, not inferred.
    if (hasPelvis && hasThorax) {
        a.state = MetricAvailability::Measured;
        return a;
    }

    // Otherwise the camera estimates it. BRIDGED, not Measured and not Unavailable: the number is
    // real and the method is weaker, which is the exact distinction the state exists to carry. The
    // reason names the method rather than the missing device, because "needs a pelvis IMU" would
    // read as a refusal when a value is in fact produced.
    if (ctx.hasFaceOn) {
        a.state  = MetricAvailability::Bridged;
        a.reason = QStringLiteral("estimated from the face-on camera — a pelvis / thorax IMU "
                                  "would measure it directly");
        return a;
    }

    a.state  = MetricAvailability::Unavailable;
    a.reason = QStringLiteral("needs a face-on camera, or a pelvis / thorax IMU");
    return a;
}

// ----------------------------------------------------------------------- ClubDeliveryProvider

std::vector<QString> ClubDeliveryProvider::provides() const
{
    return { QStringLiteral("shaftAngleVsHorizontal"), QStringLiteral("attackAngle"),
             QStringLiteral("lowPointAhead") };
}

MetricAvailability ClubDeliveryProvider::availability(const QString &key, const ShotContext &ctx) const
{
    MetricRequirement req;
    req.faceOnCamera = true;
    req.clubTrack    = true;                       // every reading comes off the measured clubhead
    if (key == QLatin1String("lowPointAhead"))
        req.ballTrack = true;                      // the reference it is stated against, and its ruler
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

    // wristScore / wristResemblance: from the lead-wrist series, so the wrist IMUs are the gate.
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
        // ── Depth. A face-on camera's blind axis, and no amount of pipeline work changes that ──
        // pelvisThrust is toward and away from the camera. clubPath's discriminating axis is the
        // same one. swingPlane needs the plane's azimuth and shaftDirection its bearing on the
        // target line. launchDirection is where the ball left relative to the target line, which is
        // also depth — its descriptor used to claim a face-on camera could read it, and could not.
        QStringLiteral("pelvisThrust"),
        QStringLiteral("swingPlane"),       QStringLiteral("clubPath"),
        QStringLiteral("shaftDirection"),   QStringLiteral("launchDirection"),
        QStringLiteral("ballBodyDistance"),

        // ── Sagittal. Present in the image, foreshortened to noise by the frontal projection ──
        // The knee angles are the strongest argument for a down-the-line pipeline: four
        // characteristics sit over them and would be graded almost entirely off projection error.
        QStringLiteral("spineForwardBend"),
        QStringLiteral("leadKneeFlexion"),  QStringLiteral("trailKneeFlexion"),

        // ── No keypoint exists, in either layout ────────────────────────────────────────────────
        // Nothing sits between the shoulders and the hips, so the spinal regions cannot come from
        // the skeleton at all. They are still roadmap items rather than capture gaps because the
        // back CONTOUR of a down-the-line silhouette shows both plainly.
        QStringLiteral("thoracicFlexion"),  QStringLiteral("lumbarExtension"),

        // ── Needs sensors we do not place ──────────────────────────────────────────────────────
        QStringLiteral("hipInternalRotation"),   // a pelvis IMU plus thigh IMUs

        // ── Needs a ball-FLIGHT track, which is not what the ball detector is ───────────────────
        // The detector is an at-spot presence tracker: it locks the stationary ball, reports
        // whether it is still there, and records the instant it vanishes. `BallSample2D::center`
        // is always the locked spot and never a ball in the air, so there is no trajectory to take
        // a launch angle or a speed from. Their descriptors said "needs the ball track", which we
        // have; what they need is a tracker that follows the ball after it leaves.
        QStringLiteral("launchAngle"),      QStringLiteral("ballSpeed"),

        // ── Has a reduction, has no series to reduce ────────────────────────────────────────────
        // kinematic_sequence.h already computes the ordered peak-speed nodes and the dashboard
        // already consumes them; what is missing is angular-SPEED series for the pelvis and thorax.
        // The angle series now exist (body_rotation.cpp), so this is a short follow-on — but it
        // carries no measure and no corridor, so promoting it today would unblock nothing.
        QStringLiteral("kinematicSequence"),
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

MetricAvailability LaunchMonitorProvider::availability(const QString &key, const ShotContext &ctx) const
{
    Q_UNUSED(key)
    // No session gate and no camera requirement: a launch monitor sees what it sees whatever else
    // is connected. The single requirement is the device itself, and routing it through
    // fromRequirement() means the "needs a launch monitor" sentence is rendered by the same code
    // that renders "needs a face-on camera" — one voice for every absent input.
    MetricRequirement req;
    req.launchMonitor = true;
    return fromRequirement(req, ctx);
}

} // namespace pinpoint::analysis
