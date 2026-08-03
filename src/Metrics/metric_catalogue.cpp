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

#include "metric_catalogue.h"

#include "metric_providers.h"
#include "metric_resolver.h"

namespace pinpoint::analysis {

void MetricCatalogue::addDescriptor(const MetricDescriptor &d)
{
    if (d.key.isEmpty() || m_index.contains(d.key))
        return;                                   // ignore blanks / duplicates (first wins)
    m_index.insert(d.key, static_cast<int>(m_descriptors.size()));
    m_descriptors.push_back(d);
}

void MetricCatalogue::addProvider(const IMetricProvider *p)
{
    if (p)
        m_providers.push_back(p);
}

const MetricDescriptor *MetricCatalogue::descriptor(const QString &key) const
{
    const auto it = m_index.constFind(key);
    if (it == m_index.constEnd())
        return nullptr;
    return &m_descriptors[*it];
}

std::vector<const MetricDescriptor *> MetricCatalogue::all() const
{
    std::vector<const MetricDescriptor *> out;
    out.reserve(m_descriptors.size());
    for (const MetricDescriptor &d : m_descriptors)
        out.push_back(&d);
    return out;
}

std::vector<const MetricDescriptor *> MetricCatalogue::query(const MetricQuery &q,
                                                             const ShotContext *ctx) const
{
    std::vector<const MetricDescriptor *> out;
    for (const MetricDescriptor &d : m_descriptors) {
        if (q.type && d.type != *q.type)
            continue;
        if (!q.group.isEmpty() && d.group != q.group)
            continue;
        if (q.scored && d.scored != *q.scored)
            continue;
        if (q.availableOnly) {
            if (!ctx)
                continue;                         // availableOnly without a context yields nothing
            if (resolve(d.key, *ctx).state == MetricAvailability::Unavailable)
                continue;
        }
        out.push_back(&d);
    }
    return out;
}

MetricAvailability MetricCatalogue::resolve(const QString &key, const ShotContext &ctx) const
{
    return resolveAvailability(m_providers, descriptor(key), key, ctx);
}

MetricCatalogue makeMetricCatalogue()
{
    MetricCatalogue cat;
    installMetricManifest(cat);

    // Stateless, process-lifetime provider instances — the catalogue keeps non-owning pointers.
    static const WristMetricProvider     wristProvider;
    static const KinematicSeriesProvider kinematicProvider;
    static const FootMetricProvider      footProvider;
    static const LowerBodyMetricProvider lowerBodyProvider;
    static const UpperBodyMetricProvider upperBodyProvider;
    static const TrailWristProvider      trailWristProvider;
    static const BodyRotationProvider    bodyRotationProvider;
    static const ClubDeliveryProvider    clubDeliveryProvider;
    static const TempoProvider           tempoProvider;
    static const HeadMetricProvider      headProvider;
    static const ShaftLeanProvider       shaftLeanProvider;
    static const ScoreProvider           scoreProvider;
    static const LaunchMonitorProvider   launchMonitorProvider;
    cat.addProvider(&wristProvider);
    cat.addProvider(&kinematicProvider);
    cat.addProvider(&footProvider);
    cat.addProvider(&lowerBodyProvider);
    cat.addProvider(&upperBodyProvider);
    cat.addProvider(&trailWristProvider);
    cat.addProvider(&bodyRotationProvider);
    cat.addProvider(&clubDeliveryProvider);
    cat.addProvider(&tempoProvider);
    cat.addProvider(&headProvider);
    cat.addProvider(&shaftLeanProvider);
    cat.addProvider(&scoreProvider);
    cat.addProvider(&launchMonitorProvider);

    return cat;
}

} // namespace pinpoint::analysis
