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

#include "metric_descriptor.h"
#include "metric_provider.h"

#include <QString>

#include <vector>

// The resolution rule (design §5): over all providers that list `key`, take the one returning the
// highest-quality state (Measured > Bridged > Unavailable), ties broken by declared priority. If no
// provider claims the key, the metric falls back to its own route ladder. Kept separate from the
// registry so the fusion policy has one testable home.
//
// The rung-picking itself is NOT here — `resolveRoutes()` in metric_provider.h is the route walk,
// because that is where the value types it reads live and it has to be callable from the provider
// seam's default implementation. This file fuses PROVIDERS; that one reads one metric's ladder.
// `describeRequirement()` moved there with it and is still visible through this header.

namespace pinpoint::analysis {

// Fuse the providers that claim `key`. `desc` may be null (unknown key) — then the result is
// Unavailable with an "unknown metric" reason. When providers claim the key but all return
// Unavailable, the best (least-bad) provider reason is kept.
MetricAvailability resolveAvailability(const std::vector<const IMetricProvider *> &providers,
                                       const MetricDescriptor *desc, const QString &key,
                                       const ShotContext &ctx);

} // namespace pinpoint::analysis
