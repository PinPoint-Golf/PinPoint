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

#include "metric_descriptor.h"  // MetricDescriptor, MetricRoute, MetricRequirement
#include "reference_bands.h"    // BandContext
#include "swing_analysis.h"     // SegmentRole, ReconstructionTier

#include <QString>
#include <QStringList>

#include <vector>

// The provider seam (design §5). A producer declares which descriptor keys it can satisfy and, per
// shot, at what quality — generalising swing_data_source's lane resolution to every metric. It does
// NOT compute the metric: production stays in the analysis stages. The catalogue is assembled from a
// fixed provider set on demand (makeMetricCatalogue), mirroring makeReferenceBandProvider — there is
// deliberately no startup registration / self-registering static (analysis_stage.h anti-goals).

namespace pinpoint::analysis {

// The per-shot facts the resolver needs (design §3.5). Built once per shot from swing.json (tier,
// analysis.bindings[].role, pose/shaft/ball stream presence, session type) — the same fields
// swing_data_source already reads — or from the studio's configured capability when the directory
// is browsed with no shot loaded ("available with my setup").
struct ShotContext {
    ReconstructionTier       tier         = ReconstructionTier::Angles2D;
    std::vector<SegmentRole> imuRoles;      // roles actually bound & calibrated this shot
    bool                     hasFaceOn    = false;
    // A second camera down the target line. False on every shot this build can record — there is no
    // DTL capture path yet — and present anyway, for the same reason hasLaunchMonitor is: the day
    // one lands, the metrics stated along the depth axis resolve through the route ladder with no
    // catalogue change, and until then they say "needs a down-the-line camera" instead of going
    // quiet or pretending to be work we owe.
    bool                     hasDtl       = false;
    bool                     hasClubTrack = false;   // ShaftTrack2D valid
    bool                     hasBallTrack = false;   // BallTrack2D present
    bool                     hasLaunchMonitor = false;   // a launch monitor supplied readings for this shot
    int                      sessionType  = -1;      // SessionController::Type; -1 = none
    BandContext              band;                   // archetype/club/shape for normative resolution

    bool hasRole(SegmentRole r) const
    {
        for (SegmentRole x : imuRoles)
            if (x == r) return true;
        return false;
    }
};

// The result of resolving one metric against one shot (design §3.5). Bridged = produced but at
// reduced fidelity / a lower tier than ideal (graceful degradation), distinct from a hard miss.
struct MetricAvailability {
    enum State { Measured, Bridged, Unavailable };
    State              state  = Unavailable;
    QString            reason;                             // "needs face-on camera", "partial · needs body IMUs"
    ReconstructionTier tier   = ReconstructionTier::Angles2D;
    // Which MetricRoute produced this answer (MetricRoute::id), empty when nothing did.
    QString            routeId;
    // What better kit would buy on THIS shot: "a pelvis IMU would measure it directly". Empty when
    // the best route already fired, and — unlike MetricDescriptor::upgradeDevices() — never
    // advertises a route no producer implements. A golfer reading a real swing is being told what to
    // go and buy, and pointing at a pipeline we have not written would be a lie with a price on it.
    QString            upgrade;
};

// Best-state ordering used by the resolver: Measured > Bridged > Unavailable.
inline int metricStateRank(MetricAvailability::State s)
{
    switch (s) {
    case MetricAvailability::Measured:    return 2;
    case MetricAvailability::Bridged:     return 1;
    case MetricAvailability::Unavailable: return 0;
    }
    return 0;
}

// ── Reading a requirement against a shot ────────────────────────────────────────────────────────

// The pieces of `req` this shot does not have, in the words the golfer would use. Empty ⇒ satisfied.
inline QStringList missingForRequirement(const MetricRequirement &req, const ShotContext &ctx)
{
    QStringList missing;

    if (req.faceOnCamera && !ctx.hasFaceOn)
        missing << QStringLiteral("a face-on camera");
    if (req.dtlCamera && !ctx.hasDtl)
        missing << QStringLiteral("a down-the-line camera");
    if (req.clubTrack && !ctx.hasClubTrack)
        missing << QStringLiteral("club tracking");
    if (req.ballTrack && !ctx.hasBallTrack)
        missing << QStringLiteral("ball tracking");
    if (req.launchMonitor && !ctx.hasLaunchMonitor)
        missing << QStringLiteral("a launch monitor");

    QStringList roles;
    for (SegmentRole r : req.imuRoles)
        if (!ctx.hasRole(r))
            roles << segmentRoleName(r);
    if (!roles.isEmpty())
        missing << (roles.join(QStringLiteral(" + ")) + QStringLiteral(" IMU"));

    if (static_cast<int>(ctx.tier) < static_cast<int>(req.minTier))
        missing << QStringLiteral("a higher reconstruction tier");

    return missing;
}

// Human-readable "why this can't be produced" for one unmet requirement. Empty when nothing is
// missing (the caller should not reach here in that case).
inline QString describeRequirement(const MetricRequirement &req, const ShotContext &ctx)
{
    const QStringList missing = missingForRequirement(req, ctx);
    if (missing.isEmpty())
        return QString();
    return QStringLiteral("needs ") + missing.join(QStringLiteral(", "));
}

// ── The route walk ──────────────────────────────────────────────────────────────────────────────
//
// The whole of the availability policy, in one place, driven by manifest data rather than by
// per-provider C++. Walk the ladder best-first; the first route we have BUILT and this shot can
// SATISFY wins, and its declared quality is the answer. This is why the providers below mostly have
// nothing left to say — see IMetricProvider::availability.
inline MetricAvailability resolveRoutes(const MetricDescriptor &desc, const ShotContext &ctx)
{
    MetricAvailability a;
    a.tier = ctx.tier;

    if (desc.routes.empty()) {
        a.state  = MetricAvailability::Unavailable;
        a.reason = QStringLiteral("no acquisition route declared");
        return a;
    }

    const MetricRoute *winner = nullptr;
    for (const MetricRoute &r : desc.routes) {
        if (r.planned) continue;
        if (missingForRequirement(r.requirement, ctx).isEmpty()) { winner = &r; break; }
    }

    if (winner) {
        a.state   = winner->quality == RouteQuality::Direct ? MetricAvailability::Measured
                                                            : MetricAvailability::Bridged;
        a.routeId = winner->id;

        // What a better rung would buy, generated rather than authored — the missing kit plus what
        // reaching that rung does. Only LIVE routes, and only rungs above the one that fired.
        //
        // THE BEST RUNG, NOT THE NEAREST. The walk is best-first and stops at the first hit, so a
        // metric with two rungs above the one that fired names the top one. That is the whole point:
        // face-on `xFactor` has both a stereo rung and an IMU rung above it, and stereo is the wrong
        // thing to send someone to buy — an IMU pair is cheaper, measures the turn outright, and
        // works on one camera. Naming the next step up the ladder would recommend the second-best
        // purchase to every owner of the weakest setup.
        for (const MetricRoute &r : desc.routes) {
            if (&r == winner) break;
            if (r.planned) continue;
            const QStringList gap = missingForRequirement(r.requirement, ctx);
            if (gap.isEmpty()) continue;                     // cannot happen — it would have won
            a.upgrade = gap.join(QStringLiteral(" + "))
                      + (r.quality == RouteQuality::Direct
                             ? QStringLiteral(" would measure it directly")
                             : QStringLiteral(" would improve it"));
            break;
        }

        // A Measured reading needs no explanation; an estimated one always does, and says which
        // METHOD produced it rather than which device is absent.
        if (a.state == MetricAvailability::Bridged) {
            a.reason = winner->summary;
            if (!a.upgrade.isEmpty())
                a.reason += QStringLiteral(" — ") + a.upgrade;
        }
        return a;
    }

    a.state = MetricAvailability::Unavailable;

    // Nothing is built: the metric is planned, and says so in the route's own words about why.
    const MetricRoute *best = desc.bestLiveRoute();
    if (!best) {
        a.reason = QStringLiteral("planned — ")
                 + (desc.routes.front().summary.isEmpty()
                        ? QStringLiteral("not yet produced in this build")
                        : desc.routes.front().summary);
        return a;
    }

    // Built, but this shot cannot feed any route. Every live route's gap, joined with "or" — a
    // metric two kits can produce must not name only one of them.
    QStringList alternatives;
    for (const MetricRoute &r : desc.routes) {
        if (r.planned) continue;
        const QString gap = missingForRequirement(r.requirement, ctx).join(QStringLiteral(", "));
        if (!gap.isEmpty() && !alternatives.contains(gap)) alternatives << gap;
    }
    a.reason = QStringLiteral("needs ") + alternatives.join(QStringLiteral(", or "));
    return a;
}

// A producer's capability declaration. Stateless; one shared const instance per producer lives for
// the process lifetime (makeMetricCatalogue holds them as function-local statics), so the catalogue
// keeps only non-owning pointers.
class IMetricProvider {
public:
    virtual ~IMetricProvider() = default;

    // The descriptor keys this producer can satisfy (context-independent).
    virtual std::vector<QString> provides() const = 0;

    // Per-shot quality for one of its keys. Called only for keys in provides(), and `desc` is that
    // key's descriptor — so the default answer is simply to read the metric's own route ladder,
    // which is what every producer we ship wants.
    //
    // OVERRIDE ONLY FOR SOMETHING THE LADDER CANNOT SAY. Two providers used to hand-write their
    // ladders here (body rotation's IMU-vs-camera split, the scores' missing scorer) and both were
    // invisible to the directory, which could show only the descriptor's single flat requirement.
    // A per-key `if` in this file is now a signal that a route is missing from the manifest.
    virtual MetricAvailability availability(const MetricDescriptor &desc, const ShotContext &ctx) const
    {
        return resolveRoutes(desc, ctx);
    }

    // Lower value wins when two providers claim the same key at equal state. Default 0.
    virtual int priority() const { return 0; }
};

} // namespace pinpoint::analysis
