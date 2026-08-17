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

#include <cstdint>
#include <climits>

namespace pinpoint {

using SourceId = uint32_t;
static constexpr SourceId kInvalidSourceId = UINT32_MAX;

enum class DeviceKind {
    Camera_UVC,
    Camera_GenICam,
    Camera_AVFoundation,
    Camera_Camera2,
    IMU_WitMotion,
    IMU_Bosch,
    IMU_Custom,
    // ⚠ A separate KIND, not another vendor of the same thing: fusion happens on
    // the device, ONE peripheral presents TWO units (so one device registers two
    // sources), and its accel channel is gravity-removed LINEAR acceleration —
    // ≈0 at rest — rather than a raw accelerometer reading. A consumer that
    // compares this lane's accel against a WitMotion lane's is comparing two
    // different physical quantities.
    IMU_HackMotion,
    Marker_App,      // application-generated marker events (e.g. shot impact)
};

enum class PixelFormat {
    Unknown,
    Mono8, Mono12, Mono12Packed, Mono16,
    BayerRG8, BayerRG12, BayerRG16,
    BayerBG8, BayerGR8, BayerGB8, BayerGB16,
    YUV422, YUYV, UYVY,
    NV12, YUV420P,
    BGR24, RGB24, BGRA32, RGBA32,
    MJPEG, H264_NAL,
};

namespace IndexEntryFlags {
    static constexpr uint32_t None          = 0;
    static constexpr uint32_t SourceStalled = 1u << 0;
}

struct IndexEntry {
    int64_t  timestamp_us    = 0;
    SourceId source_id       = kInvalidSourceId;
    uint64_t source_sequence = 0;
    uint64_t global_sequence = 0;
    uint32_t flags           = 0;
};
static_assert(sizeof(IndexEntry) <= 40, "IndexEntry must not exceed 40 bytes");

} // namespace pinpoint
