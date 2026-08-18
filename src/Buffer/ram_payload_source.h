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
#include "imu_sample.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pinpoint {

// An in-RAM SwingPayloadSource for IMU lanes — deferred_sources_design.md §3.2.
//
// ⚠ THIS IS NOT A NEW MECHANISM. It is lifted verbatim out of the offline
// re-analysis path's SwingDiskSource (swing_reanalyzer.cpp), which has been
// serving IMU samples from owned RAM in production for months. Promoting it here
// so the LIVE path and the OFFLINE path share ONE implementation is most of the
// point: re-analysis determinism depends on the two agreeing, and two copies of
// this that drift would break it silently, in the direction of "the corpus
// re-analyses differently than it captured".
//
// It backs two callers:
//   - SwingDiskLoader, for a swing streamed off disk;
//   - a deferred source's retrieved samples, folded into a live window through
//     CompositePayloadSource.
//
// The bytes are owned and stable for the source's whole lifetime, so validate()
// is unconditionally true — there is no ring to race with and no seqlock
// generation to check.
class RamPayloadSource final : public SwingPayloadSource {
public:
    // Sequence is the index into `samples`, matching how both construction paths
    // synthesise their IndexEntry rows (IndexEntry{ t_us[i], id, i }).
    void addImu(SourceId id, FormatDescriptor fd, std::vector<ImuSample> samples) {
        imu_[id] = Stream{ std::move(fd), std::move(samples) };
    }

    bool has(SourceId id) const noexcept { return imu_.find(id) != imu_.end(); }

    size_t sampleCount(SourceId id) const noexcept {
        const auto it = imu_.find(id);
        return it == imu_.end() ? 0 : it->second.samples.size();
    }

    SourceRing::ReadHandle payloadOf(SourceId id, uint64_t sequence) const noexcept override {
        const auto it = imu_.find(id);
        if (it == imu_.end() || sequence >= it->second.samples.size())
            return {};
        SourceRing::ReadHandle h;
        h.data  = reinterpret_cast<const std::byte*>(&it->second.samples[size_t(sequence)]);
        h.bytes = sizeof(ImuSample);
        return h;
    }

    const FormatDescriptor& formatOf(SourceId id) const noexcept override {
        const auto it = imu_.find(id);
        if (it != imu_.end()) return it->second.fd;
        static const FormatDescriptor kEmpty{};
        return kEmpty;
    }

    bool validate(SourceId, const SourceRing::ReadHandle&) const noexcept override {
        return true;   // owned RAM — the bytes are stable, there is no race
    }

private:
    struct Stream {
        FormatDescriptor       fd;
        std::vector<ImuSample> samples;
    };
    std::unordered_map<SourceId, Stream> imu_;
};

} // namespace pinpoint
