/*
 * Copyright (C) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "types.h"
#include "source_ring.h"
#include "format_descriptor.h"
#include "swing_payload_source.h"

#include <memory>
#include <utility>
#include <vector>

namespace pinpoint {

// Routes payload access to one of several backings by SourceId —
// deferred_sources_design.md §3.2.
//
// This is the one genuinely missing piece the design named: today a window's
// backing is EITHER the live ring OR disk, and a deferred source needs BOTH AT
// ONCE — the cameras and any live-only IMU lanes read from the frozen ring,
// while a lane whose high-rate samples arrived after the freeze reads from RAM.
//
// ⚠ ROUTING IS BY SOURCE, AND A SOURCE BELONGS TO EXACTLY ONE BACKING. A
// stitched lane is therefore served WHOLLY from RAM — its live prefix and suffix
// are merged into the RAM block rather than left behind in the ring — because
// one id cannot have its sequence space split across two backings without the
// sequence numbers colliding. That is why the stitch produces one ascending
// variable-rate trace with fresh sequences rather than a patch over the ring.
//
// Routes are resolved in registration order and the first claim wins, so a
// deferred lane must be registered BEFORE the ring fallback that would also
// answer for it.
class CompositePayloadSource final : public SwingPayloadSource {
public:
    // Add a backing that answers for exactly the listed sources. An empty id
    // list means "everything not already claimed" — the ring fallback.
    void add(std::unique_ptr<const SwingPayloadSource> source,
             std::vector<SourceId> ids)
    {
        routes_.push_back(Route{ std::move(source), std::move(ids) });
    }

    SourceRing::ReadHandle payloadOf(SourceId id, uint64_t sequence) const noexcept override {
        const SwingPayloadSource* s = routeFor(id);
        return s ? s->payloadOf(id, sequence) : SourceRing::ReadHandle{};
    }

    const FormatDescriptor& formatOf(SourceId id) const noexcept override {
        if (const SwingPayloadSource* s = routeFor(id))
            return s->formatOf(id);
        static const FormatDescriptor kEmpty{};
        return kEmpty;
    }

    bool validate(SourceId id, const SourceRing::ReadHandle& h) const noexcept override {
        const SwingPayloadSource* s = routeFor(id);
        // ⚠ No route means the handle came from nowhere this composite serves.
        // Refusing is the only safe answer: a `true` here would let a stale or
        // foreign handle through the one check that exists to catch it.
        return s ? s->validate(id, h) : false;
    }

private:
    struct Route {
        std::unique_ptr<const SwingPayloadSource> source;
        std::vector<SourceId>                     ids;   // empty = catch-all
    };

    const SwingPayloadSource* routeFor(SourceId id) const noexcept {
        for (const Route& r : routes_) {
            if (r.ids.empty())
                return r.source.get();                  // catch-all
            for (SourceId claimed : r.ids)
                if (claimed == id) return r.source.get();
        }
        return nullptr;
    }

    // A handful of entries at most (one deferred lane set + the ring), so a
    // linear walk beats any map and keeps the hot payloadOf path allocation-free.
    std::vector<Route> routes_;
};

} // namespace pinpoint
