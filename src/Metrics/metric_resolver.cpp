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

#include "metric_resolver.h"

#include <QStringList>

namespace pinpoint::analysis {

QString describeRequirement(const MetricRequirement &req, const ShotContext &ctx)
{
    QStringList missing;

    if (req.faceOnCamera && !ctx.hasFaceOn)
        missing << QStringLiteral("a face-on camera");
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

    if (missing.isEmpty())
        return QString();
    return QStringLiteral("needs ") + missing.join(QStringLiteral(", "));
}

MetricAvailability resolveAvailability(const std::vector<const IMetricProvider *> &providers,
                                       const MetricDescriptor *desc, const QString &key,
                                       const ShotContext &ctx)
{
    MetricAvailability best;
    best.tier = ctx.tier;
    bool claimed = false;
    int  bestPriority = 0;

    for (const IMetricProvider *p : providers) {
        if (!p)
            continue;
        bool provides = false;
        for (const QString &k : p->provides())
            if (k == key) { provides = true; break; }
        if (!provides)
            continue;

        const MetricAvailability a = p->availability(key, ctx);
        const bool better = !claimed
                          || metricStateRank(a.state) > metricStateRank(best.state)
                          || (metricStateRank(a.state) == metricStateRank(best.state)
                              && p->priority() < bestPriority);
        if (better) {
            best         = a;
            bestPriority = p->priority();
            claimed      = true;
        }
    }

    if (!claimed) {
        best.state = MetricAvailability::Unavailable;
        if (!desc)
            best.reason = QStringLiteral("unknown metric");
        else {
            best.reason = describeRequirement(desc->requirement, ctx);
            if (best.reason.isEmpty())
                best.reason = QStringLiteral("no producer available");
        }
    }
    return best;
}

} // namespace pinpoint::analysis
