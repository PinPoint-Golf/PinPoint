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

MetricAvailability resolveAvailability(const std::vector<const IMetricProvider *> &providers,
                                       const MetricDescriptor *desc, const QString &key,
                                       const ShotContext &ctx)
{
    MetricAvailability best;
    best.tier = ctx.tier;
    bool claimed = false;
    int  bestPriority = 0;

    // An unknown key has no ladder to walk and no provider worth asking.
    if (!desc) {
        best.state  = MetricAvailability::Unavailable;
        best.reason = QStringLiteral("unknown metric");
        return best;
    }

    for (const IMetricProvider *p : providers) {
        if (!p)
            continue;
        bool provides = false;
        for (const QString &k : p->provides())
            if (k == key) { provides = true; break; }
        if (!provides)
            continue;

        const MetricAvailability a = p->availability(*desc, ctx);
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

    // No provider claims it. A metric whose every route is planned is SUPPOSED to land here — the
    // ladder says "planned" for itself, and a placeholder provider that only ever repeated that
    // sentence was a second list to keep in step. A metric with a live route landing here is the
    // stanceWidthMm bug (declared, produced, claimed by nobody, reported Unavailable on every shot
    // ever taken), which the catalogue test sweeps for by name.
    if (!claimed)
        best = resolveRoutes(*desc, ctx);
    return best;
}

} // namespace pinpoint::analysis
