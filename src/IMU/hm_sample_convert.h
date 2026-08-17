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

#include <hackmotion/sample.h>

#include "imu_sample.h"

// ---------------------------------------------------------------------------
// HackMotion wire block -> pinpoint::ImuSample
// ---------------------------------------------------------------------------
//
// This is the ONLY place in the tree where a HackMotion reading becomes an
// ImuSample, so it is also where the three things that make this lane unlike a
// Witmotion lane are written down. Everything downstream of here sees a 40-byte
// imu_sample_v2 frame with no memory of where it came from — the provenance the
// caller records at registration is the only thing that carries the difference,
// which is why it is spelled out below rather than left to the brief.
//
// ⚠ ACCEL HERE IS GRAVITY-REMOVED LINEAR ACCELERATION, IN g, READING ≈0 AT REST.
// A Witmotion lane's accel channel holds the raw accelerometer vector, which
// reads 1 g at rest and contains gravity. The two share a field name, a unit and
// a struct, and are NOT the same physical quantity. Nothing downstream may
// compare, difference, threshold or pool them across lanes — an impact detector
// tuned on one is meaningless on the other, and "the accel looks wrong, it's
// near zero" is this device working correctly (spec §6.4, hm_unit_sample).
//
// ⚠ THERE IS NO AGGREGATE ACROSS THE TWO UNITS, AND THERE MUST NEVER BE ONE.
// The wG3 is one peripheral carrying two units 3-8 cm apart on hand and
// forearm, so under rotation they sit at different radii and their linear
// accelerations legitimately differ by roughly ω²r — the palm unit read
// 31-51 m/s² MORE than the lower arm, consistently, across five golf swings
// (§6.4). They are SUPPOSED to disagree. That is why this function converts ONE
// block and takes no view on which one: the caller names the unit and writes it
// to its own EventBuffer source. Averaging the two, or treating the gap as a
// fault to be reconciled, would destroy the only physical signal the pair
// carries.
//
// ⚠ THE QUATERNION IS STORED EXACTLY AS IT ARRIVES, AND IT MAPS WORLD -> BODY.
// That is the CONJUGATE of what the rest of our IMU code assumes (§6.7), so
// composition order differs throughout: q_rel = q_palm ⊗ q_arm*, not
// q_arm* ⊗ q_palm. Reconciling it — together with the constant per-unit rotation
// into our anatomical frame — is Phase D's job, done once against a known
// single-axis motion. Do NOT conjugate, remap or renormalise here because a cube
// or a wrist angle "looks better": the wrist ANGLE is convention-blind
// (2·acos|q_a·q_b| is identical under both orders), so a silent conjugation or
// component swap leaves every plausibility check passing while mirroring every
// decomposed component. Nothing else in the pipeline would catch it, which is
// exactly why hm_sample_convert_test.cpp pins each component by position.
//
// WHAT THIS FUNCTION DELIBERATELY DOES NOT DO — all of it belongs to the caller:
//   - TIME. ImuSample carries no timestamp; the EventBuffer takes it separately.
//     Use hm_sample::host_time_us. Per-source monotonicity is the buffer's
//     constraint (event_buffer.cpp:84 clamps a violation and counts it), and
//     host_time_us is the clock fit applied to the monotonically increasing
//     sample_index, so it is monotonic per source BY CONSTRUCTION rather than by
//     luck — stated here so the next reader does not have to re-derive it, and
//     so the day the caller starts stamping arrival instants instead
//     (host_recv_us, one-sidedly late and NOT monotonic) it is an obvious break.
//   - SKEW. The palm block trails the lower arm by a stable 0.92 ms (§10.3),
//     worth ~0.9° at 1,000 °/s. Carry hm_sample::skew_us into provenance; do not
//     paper over it by stamping both blocks with one host time and calling them
//     simultaneous.
//   - PINNING. hm_unit_sample::pinned_mask says a channel saturated. int16 fields
//     saturate rather than wrap, so a clipped peak is a plausible-looking flat
//     top and nothing else reports it. That is a provenance/counter concern, not
//     a reason to reject or alter a sample here.
//   - RATE. Dense bursts reach the device's full ≈799 Hz in every session
//     containing motion (§6.6). Size rings from how often the caller drains, not
//     from a nominal rate.
namespace pinpoint::hm {

// Spec §6.4 gives the accelerometer scale as "× 0.0098 -> m/s² (1 LSB = 1 mg)",
// so the device's native quantum is exactly 1 mg and raw counts ARE milli-g.
inline constexpr float kAccelLsbG = 0.001f;   // spec §6.4: 1 LSB = 1 mg

// One wire block -> one 40-byte ImuSample. Header-only and pure: no Qt, no
// clock, no session, no side effects — so the unit test compiles it without a
// library and the app's CMake needs no new source file.
inline ImuSample toImuSample(const hm_unit_sample &b)
{
    // ⚠ g COMES FROM THE RAW COUNTS, NOT FROM linear_accel_mps2 / 9.80665.
    // 1 LSB is exactly 1 mg, so raw × 0.001 is exact. Going via the scaled field
    // would re-import the library's own g ≈ 9.8 rounding (0.0098 m/s² per mg is
    // 0.99932 mg of true gravity) and land every reading 0.068 % low for no
    // reason at all. hm_unit_sample's header says the raw counts are the
    // authoritative form — "they stay correct under any configuration" — and
    // this is the case that proves it. The test asserts against the 0.99932
    // specifically, so the shortcut cannot come back unnoticed.
    const float ax = static_cast<float>(b.linear_accel_raw[0]) * kAccelLsbG;
    const float ay = static_cast<float>(b.linear_accel_raw[1]) * kAccelLsbG;
    const float az = static_cast<float>(b.linear_accel_raw[2]) * kAccelLsbG;

    // ⚠ GYRO IS THE SCALED FIELD, AND HERE THAT IS THE AUTHORITATIVE ONE.
    // The counts-to-°/s divisor is config-dependent — /8 with config bit 6 set,
    // /16 clear (§6.4) — and only the library knows which `a0 01 <cfg>` produced
    // this sample. Recomputing from gyro_raw with an assumed divisor is silently
    // wrong by a factor of two on exactly the channel that dominates a swing.
    const float gx = b.gyro_dps[0];
    const float gy = b.gyro_dps[1];
    const float gz = b.gyro_dps[2];

    // Verbatim, in arrival order w, x, y, z. See the world->body warning above.
    return makeImuSample(ax, ay, az,
                         gx, gy, gz,
                         b.q_world_to_body[0], b.q_world_to_body[1],
                         b.q_world_to_body[2], b.q_world_to_body[3]);
}

} // namespace pinpoint::hm
