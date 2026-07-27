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

#include <QHash>
#include <QString>

#include <optional>
#include <vector>

// MetricCatalogue — the registry (design §4). Populated once by the manifest (descriptors) and the
// fixed provider set, then read-only. It is a plain value object, NOT a singleton: assemble it on
// demand with makeMetricCatalogue() (mirrors makeReferenceBandProvider), honouring the Analysis
// module's ban on startup registration / self-registering statics. Pure; no Qt-GUI.

namespace pinpoint::analysis {

// Directory query (design §4). Any std::nullopt / empty field means "don't filter on it".
struct MetricQuery {
    std::optional<MetricType> type;
    QString                   group;                  // empty = any
    std::optional<bool>       scored;
    std::optional<int>        sessionType;
    bool                      availableOnly = false;  // needs a ShotContext to evaluate
};

class MetricCatalogue {
public:
    MetricCatalogue() = default;

    // --- population (used only by makeMetricCatalogue / the manifest) ---
    void addDescriptor(const MetricDescriptor &d);
    void addProvider(const IMetricProvider *p);       // non-owning; provider outlives the catalogue

    // --- lookup ---
    const MetricDescriptor *descriptor(const QString &key) const;
    std::vector<const MetricDescriptor *> all() const;

    // Filtered directory list. With availableOnly + a ctx, drops metrics that resolve to
    // Unavailable. Without a ctx, availability is not evaluated (directory "all" mode).
    std::vector<const MetricDescriptor *> query(const MetricQuery &q,
                                                const ShotContext *ctx = nullptr) const;

    // Per-shot resolution across the registered providers (best available wins).
    MetricAvailability resolve(const QString &key, const ShotContext &ctx) const;

    // There is no corridor() here. The catalogue describes metrics; it does not judge them. A
    // corridor is (metric, phase) → measure → norm, resolved in the shot's context —
    // `Diagnostics/metric_corridor.h`.

private:
    std::vector<MetricDescriptor>        m_descriptors;   // owns; never mutated after build
    QHash<QString, int>                  m_index;         // key -> index into m_descriptors
    std::vector<const IMetricProvider *> m_providers;     // non-owning
};

// The one place the catalogue is assembled: manifest descriptors + the fixed provider set. Cheap to
// call; providers are process-lifetime statics, so the returned value keeps only non-owning pointers.
MetricCatalogue makeMetricCatalogue();

// Declared by the manifest translation unit (metric_catalogue_manifest.cpp): pushes every live
// descriptor into `cat`. Kept separate so "what metrics exist" is one readable list.
void installMetricManifest(MetricCatalogue &cat);

} // namespace pinpoint::analysis
