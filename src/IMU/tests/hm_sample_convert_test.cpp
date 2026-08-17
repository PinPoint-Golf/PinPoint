// Unit pin for the ONE HackMotion -> ImuSample conversion (src/IMU/hm_sample_convert.h).
// Run via CTest (src/IMU/tests/CMakeLists.txt):
//   cmake -S src/IMU/tests -B build/imu-tests
//   cmake --build build/imu-tests && ctest --test-dir build/imu-tests --output-on-failure
//
// WHY THIS EXISTS. Every property this file asserts is one that a WRONG answer
// still looks completely plausible for:
//
//   - a 0.068 % accel scale error (going via the library's scaled m/s² field
//     instead of the raw milli-g counts) is invisible in any plot and in every
//     threshold we have;
//   - a gyro re-derived from raw counts with the wrong config-dependent divisor
//     is out by exactly 2× — a clean, believable waveform;
//   - a conjugated or component-swapped quaternion leaves the wrist ANGLE
//     correct, because 2·acos|q_a·q_b| is convention-blind, while mirroring
//     every decomposed component;
//   - averaging or cross-mixing the two units' accelerations produces a
//     smoother, more "sensible" looking pair of lanes than the truth does.
//
// None of those fail loudly anywhere downstream. This is the only place that
// says so, which is why the assertions name the wrong value explicitly rather
// than merely confirming the right one.
//
// Everything here is hardware-free: hm_unit_sample and hm_sample are PODs, so a
// block is built by hand and no session, link or device is involved.

#include "hm_sample_convert.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(label, cond)                                                    \
    do {                                                                      \
        const bool ok = (cond);                                               \
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);              \
        if (!ok) ++g_fail;                                                    \
    } while (0)

using pinpoint::ImuSample;
using pinpoint::hm::kAccelLsbG;
using pinpoint::hm::toImuSample;

namespace {

// The library's own scaling, reproduced so a hand-built block is INTERNALLY
// CONSISTENT — otherwise "the converter ignored linear_accel_mps2" would pass
// for the trivial reason that the field was left at zero.
constexpr float kHmAccelLsbMps2 = 0.0098f;   // spec §6.4, == HM_ACCEL_LSB_MPS2
constexpr float kStandardG      = 9.80665f;  // the divisor the shortcut would use

// One wire block. `gyroRawInconsistent` is set deliberately at odds with the
// scaled gyro so a converter that re-derives °/s from counts cannot pass.
hm_unit_sample makeBlock(const int16_t accelRaw[3],
                         const float   gyroDps[3],
                         const int16_t gyroRawInconsistent[3],
                         const float   quatWxyz[4])
{
    hm_unit_sample b;
    std::memset(&b, 0, sizeof(b));

    for (int i = 0; i < 3; ++i) {
        b.linear_accel_raw[i]  = accelRaw[i];
        b.linear_accel_mps2[i] = static_cast<float>(accelRaw[i]) * kHmAccelLsbMps2;
        b.gyro_dps[i]          = gyroDps[i];
        b.gyro_raw[i]          = gyroRawInconsistent[i];
    }
    for (int i = 0; i < 4; ++i) {
        b.q_world_to_body[i]     = quatWxyz[i];
        b.q_world_to_body_raw[i] = static_cast<int16_t>(quatWxyz[i] * HM_QUAT_SCALE);
    }
    b.has_ticks = 1;
    return b;
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

} // namespace

int main()
{
    std::printf("HackMotion block -> ImuSample conversion\n");

    // A quaternion whose four components are all distinct in magnitude AND mixed
    // in sign: any conjugation, negation or reordering moves at least one
    // component to a value no other component holds, so a positional assertion
    // catches it. A symmetric or all-positive quaternion would not.
    const float rawQ[4] = { 0.6f, 0.1f, -0.5f, 0.4f };
    const float qn = std::sqrt(rawQ[0] * rawQ[0] + rawQ[1] * rawQ[1] +
                               rawQ[2] * rawQ[2] + rawQ[3] * rawQ[3]);
    const float q[4] = { rawQ[0] / qn, rawQ[1] / qn, rawQ[2] / qn, rawQ[3] / qn };

    // ── 1. The g conversion comes from the RAW milli-g counts ─────────────────
    // 1 LSB is exactly 1 mg (§6.4: "× 0.0098 -> m/s², 1 LSB = 1 mg"), so
    // raw × 0.001 is EXACT. The shortcut — linear_accel_mps2 / 9.80665 — folds
    // in the library's g ≈ 9.8 rounding and returns 0.99932 g for a reading that
    // is 1.000 g by construction. THAT is the regression this block catches: the
    // two answers differ by 0.068 %, which no plot, no threshold and no reviewer
    // will ever notice, and which would put this lane permanently out of scale
    // with itself.
    {
        std::printf("\nAccel: raw milli-g counts, not the scaled m/s2 field\n");
        const int16_t accelRaw[3] = { 1000, -1000, 0 };
        const float   gyroDps[3]  = { 0.0f, 0.0f, 0.0f };
        const int16_t gyroRaw[3]  = { 0, 0, 0 };
        const hm_unit_sample b = makeBlock(accelRaw, gyroDps, gyroRaw, q);
        const ImuSample s = toImuSample(b);

        CHECK("+1000 counts -> +1.000000 g exactly", near(s.accel_x,  1.0f, 1e-6f));
        CHECK("-1000 counts -> -1.000000 g exactly", near(s.accel_y, -1.0f, 1e-6f));
        // Gravity-removed linear acceleration reads ≈0 at rest — a zero count is
        // a legitimate, expected reading on this lane, unlike a Witmotion lane
        // where a zero axis triple would mean the sensor had stopped reporting.
        CHECK("0 counts -> 0 g (at rest is ~0 here, by design)",
              near(s.accel_z, 0.0f, 1e-9f));

        const float shortcut = b.linear_accel_mps2[0] / kStandardG;   // 0.99932...
        CHECK("the m/s2/9.80665 shortcut really does differ (sanity of the trap)",
              !near(shortcut, 1.0f, 1e-4f));
        CHECK("accel_x is NOT the 0.99932 g the shortcut would give",
              !near(s.accel_x, shortcut, 1e-5f));
        CHECK("kAccelLsbG is exactly 1 mg", kAccelLsbG == 0.001f);
    }

    // ── 2. Full scale is the spec's ±32.8 g ──────────────────────────────────
    // int16 saturates rather than wraps, so a pinned channel arrives as a
    // plausible flat-topped peak. The number it flattens to still has to be the
    // documented full scale, or a saturation check written downstream against
    // ±32.8 g would never trigger.
    {
        std::printf("\nAccel full scale\n");
        const int16_t accelRaw[3] = { 32767, -32768, 12345 };
        const float   gyroDps[3]  = { 0.0f, 0.0f, 0.0f };
        const int16_t gyroRaw[3]  = { 0, 0, 0 };
        const ImuSample s = toImuSample(makeBlock(accelRaw, gyroDps, gyroRaw, q));

        CHECK("32767 counts -> 32.767 g (spec's +/-32.8 g)",
              near(s.accel_x, 32.767f, 1e-4f));
        CHECK("-32768 counts -> -32.768 g", near(s.accel_y, -32.768f, 1e-4f));
        CHECK("12345 counts -> 12.345 g", near(s.accel_z, 12.345f, 1e-4f));
    }

    // ── 3. Gyro passes through; it is NOT re-derived from counts ──────────────
    // The counts-to-°/s divisor is config-dependent (/8 with config bit 6 set,
    // /16 clear — §6.4) and only the library knows which configuration produced
    // this sample. gyro_raw here is deliberately inconsistent with gyro_dps: a
    // converter that recomputes lands on 1/16, 2/16, 3/16 instead of the real
    // rates, and nothing about the resulting waveform looks wrong.
    {
        std::printf("\nGyro: library's scaled dps wins over raw counts\n");
        const int16_t accelRaw[3] = { 0, 0, 0 };
        const float   gyroDps[3]  = { 12.5f, -300.25f, 2047.75f };
        const int16_t gyroRaw[3]  = { 1, 2, 3 };   // nothing to do with the above
        const ImuSample s = toImuSample(makeBlock(accelRaw, gyroDps, gyroRaw, q));

        CHECK("gyro_x is gyro_dps[0] verbatim", s.gyro_x ==  12.5f);
        CHECK("gyro_y is gyro_dps[1] verbatim", s.gyro_y == -300.25f);
        CHECK("gyro_z is gyro_dps[2] verbatim", s.gyro_z ==  2047.75f);
        CHECK("gyro_x is NOT gyro_raw[0]/16 (bit-6-clear divisor)",
              !near(s.gyro_x, 1.0f / 16.0f, 1e-6f));
        CHECK("gyro_y is NOT gyro_raw[1]/8 (bit-6-set divisor)",
              !near(s.gyro_y, 2.0f / 8.0f, 1e-6f));
    }

    // ── 4. Quaternion: verbatim, no sign change, no reordering ────────────────
    // ⚠ THIS IS THE ASSERTION NOTHING ELSE IN THE PIPELINE DUPLICATES. The
    // streamed quaternion maps world -> body (§6.7), the conjugate of what the
    // rest of our IMU code assumes, and reconciling that is Phase D's job. If
    // someone "fixes" it here — a conjugation, an axis swap, a renormalisation —
    // the wrist ANGLE stays correct, because 2·acos|q_a·q_b| is identical under
    // either convention, while every decomposed component is mirrored. So no
    // plausibility check, no cube, no angle plot and no other test would fail.
    // Only a positional, sign-sensitive comparison right here catches it.
    {
        std::printf("\nQuaternion: stored exactly as it arrives\n");
        const int16_t accelRaw[3] = { 0, 0, 0 };
        const float   gyroDps[3]  = { 0.0f, 0.0f, 0.0f };
        const int16_t gyroRaw[3]  = { 0, 0, 0 };
        const ImuSample s = toImuSample(makeBlock(accelRaw, gyroDps, gyroRaw, q));

        CHECK("quat_w == q[0] (w first, as it arrives)", s.quat_w == q[0]);
        CHECK("quat_x == q[1]", s.quat_x == q[1]);
        CHECK("quat_y == q[2]", s.quat_y == q[2]);
        CHECK("quat_z == q[3]", s.quat_z == q[3]);

        // Spelled out separately so a failure reads as "someone conjugated it"
        // rather than as an anonymous float mismatch.
        CHECK("vector part is NOT negated (no silent conjugation)",
              s.quat_x != -q[1] && s.quat_y != -q[2] && s.quat_z != -q[3]);
        CHECK("w and x are not swapped", s.quat_w != q[1] && s.quat_x != q[0]);
        CHECK("y and z are not swapped", s.quat_y != q[3] && s.quat_z != q[2]);

        // The fixture itself has to be capable of catching a swap.
        CHECK("fixture quaternion has four distinct, asymmetric components",
              q[0] != q[1] && q[0] != q[2] && q[0] != q[3] &&
              q[1] != q[2] && q[1] != q[3] && q[2] != q[3] &&
              !near(std::fabs(q[0]), std::fabs(q[2]), 1e-3f));
    }

    // ── 5. Per-block and unit-agnostic: no averaging, no cross-talk ───────────
    // ⚠ THE TWO UNITS ARE SUPPOSED TO DISAGREE. They sit 3-8 cm apart on hand and
    // forearm, so under rotation they are at different radii and their linear
    // accelerations differ by roughly ω²r — the palm read 31-51 m/s² MORE than
    // the lower arm, consistently, across five golf swings (§6.4). That is the
    // physical signal, not an error to be reconciled: the ~4 g gap below is
    // squarely inside the measured band. A converter that averaged the pair, or
    // that reached into the enclosing hm_sample for "the device's" acceleration,
    // would produce two smoother and entirely fictitious lanes. This function
    // therefore takes ONE block and the caller names the unit.
    {
        std::printf("\nPer-block conversion: the two units stay separate\n");
        hm_sample all;
        std::memset(&all, 0, sizeof(all));

        const int16_t armRaw[3]  = {  500, -200,  100 };   // 0.5 / -0.2 / 0.1 g
        const int16_t palmRaw[3] = { 4500, -200, 4100 };   // +4 g on x and z
        const float   armGyro[3]  = { 100.0f, -50.0f,  25.0f };
        const float   palmGyro[3] = { 100.0f, -50.0f,  25.0f };  // rotation is shared
        const int16_t gyroRaw[3]  = { 7, 7, 7 };
        const float   palmQ[4] = { q[3], q[2], q[1], q[0] };   // a different rotation

        all.lower_arm = makeBlock(armRaw,  armGyro,  gyroRaw, q);
        all.palm      = makeBlock(palmRaw, palmGyro, gyroRaw, palmQ);

        const ImuSample arm  = toImuSample(all.lower_arm);
        const ImuSample palm = toImuSample(all.palm);

        CHECK("lower arm accel is its own block, unmodified",
              near(arm.accel_x, 0.5f, 1e-6f) && near(arm.accel_y, -0.2f, 1e-6f) &&
              near(arm.accel_z, 0.1f, 1e-6f));
        CHECK("palm accel is its own block, unmodified",
              near(palm.accel_x, 4.5f, 1e-6f) && near(palm.accel_y, -0.2f, 1e-6f) &&
              near(palm.accel_z, 4.1f, 1e-6f));
        // The mean of 0.5 and 4.5 is 2.5 — what an aggregate would hand both lanes.
        CHECK("neither lane carries the mean of the two (no averaging)",
              !near(arm.accel_x, 2.5f, 1e-3f) && !near(palm.accel_x, 2.5f, 1e-3f));
        CHECK("the ~4 g palm/arm gap survives conversion (it is the physics)",
              near(palm.accel_x - arm.accel_x, 4.0f, 1e-5f));

        CHECK("each unit keeps its own quaternion (no cross-talk)",
              arm.quat_w == q[0] && palm.quat_w == palmQ[0] &&
              arm.quat_z == q[3] && palm.quat_z == palmQ[3]);
        CHECK("shared rotation rate comes through identically on both",
              arm.gyro_x == palm.gyro_x && arm.gyro_y == palm.gyro_y &&
              arm.gyro_z == palm.gyro_z);

        // Converting the same block twice must give the same answer — the
        // function holds no state, so a per-unit accumulator sneaking in later
        // (a filter, a bias estimate) shows up here.
        const ImuSample again = toImuSample(all.palm);
        CHECK("conversion is pure: same block -> same sample",
              std::memcmp(&palm, &again, sizeof(ImuSample)) == 0);
    }

    // ── 6. The stored schema is still imu_sample_v2 ───────────────────────────
    // Cheap guard: the exporter writes these fields verbatim in struct order, so
    // a field added or reordered in ImuSample changes the on-disk frame. Free to
    // assert, and this is the file that would have to change with it.
    {
        std::printf("\nSchema\n");
        CHECK("sizeof(ImuSample) == 40 (imu_sample_v2)", sizeof(ImuSample) == 40);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "OK",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
