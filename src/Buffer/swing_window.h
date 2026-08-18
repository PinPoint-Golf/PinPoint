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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pinpoint {

// Frozen, bounded view of a time range, served by a pluggable SwingPayloadSource.
// Backed either by a paused EventBuffer's ring (live capture/export — valid only
// while Paused; destroy the window before resume()) or by a disk streamer (offline
// re-analysis). The window owns its source for its whole lifetime.
class SwingWindow {
public:
    // Construct from a payload source + the frozen index slice. Public so both the
    // live factory (EventBuffer::captureSwingWindow) and the offline loader
    // (SwingDiskLoader) can build one; each supplies the matching backing.
    SwingWindow(std::unique_ptr<const SwingPayloadSource> source,
                std::vector<IndexEntry> entries,
                int64_t start_us,
                int64_t end_us);

    SwingWindow(const SwingWindow&)            = delete;
    SwingWindow& operator=(const SwingWindow&) = delete;

    SwingWindow(SwingWindow&&) noexcept;
    SwingWindow& operator=(SwingWindow&&) noexcept;

    ~SwingWindow();

    int64_t startTimestampUs() const noexcept { return start_us_; }
    int64_t endTimestampUs()   const noexcept { return end_us_; }
    std::chrono::microseconds duration() const noexcept {
        return std::chrono::microseconds(end_us_ - start_us_);
    }

    std::span<const IndexEntry> entries() const noexcept {
        return {entries_.data(), entries_.size()};
    }

    std::vector<IndexEntry> entriesFor(SourceId id) const;

    size_t frameCount(SourceId camera_id) const noexcept;
    size_t imuSampleCount(SourceId imu_id) const noexcept;

    // Zero-copy payload access. Valid for the window's lifetime (buffer is frozen).
    // Returns a handle with data=nullptr if the entry is not in this window.
    SourceRing::ReadHandle payloadOf(const IndexEntry& e) const noexcept;

    const FormatDescriptor& formatOf(SourceId id) const noexcept;

    // Interpolate between the two nearest ImuSample records for imu_id at target_us.
    // Lerps accel/gyro linearly; slerps the quaternion along the shortest arc.
    // out_bytes must equal sizeof(ImuSample). Returns false if data is insufficient.
    bool interpolateImu(SourceId imu_id, int64_t target_us,
                        std::byte* out, size_t out_bytes) const noexcept;

private:
    // Per-source lookup index, built once at construction — deferred_sources_
    // design.md §4.2. entriesFor(), frameCount() and interpolateImu() used to
    // scan every entry in the window on every call, and ImuVisionFuser calls
    // interpolateImu once per grid point per binding, so the cost was
    // gridPoints × bindings × totalEntries. A deferred high-rate source inflates
    // both of the terms that matter.
    //
    // Each lane holds this source's entries in the window's own order, which is
    // ascending by timestamp — so a bracketing pair is a binary search rather
    // than a scan. Held by value rather than as indices into entries_ so that
    // entriesFor() is one copy and the search touches one contiguous run.
    struct Lane {
        SourceId                id;
        std::vector<IndexEntry> entries;
    };

    // The lane for a source, or nullptr if it contributed nothing to this window.
    const std::vector<IndexEntry>* laneFor(SourceId id) const noexcept;

    std::unique_ptr<const SwingPayloadSource> source_;
    std::vector<IndexEntry>                   entries_;
    std::vector<Lane>                         lanes_;
    int64_t                                   start_us_;
    int64_t                                   end_us_;
};

} // namespace pinpoint
