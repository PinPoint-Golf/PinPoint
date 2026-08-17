// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// hm_skew_stats.h — the inter-unit skew, aggregated the way the measurement
// actually behaves.
// ---------------------------------------------------------------------------
//
// The wG3's two units are read from ONE stream and are NOT sampled
// simultaneously. `hm_sample::skew_us` carries palm − lower_arm for one record,
// and spec §10.3 measures a STABLE 59 ticks (0.92 ms) across a session — worth
// ~0.9° of relative angle at 1,000 °/s, which is the device's primary output.
//
// ⚠ BUT THAT STABLE FIGURE IS SESSION-LEVEL AND DOES NOT EXIST IN ONE RECORD.
// The two units share a sample index by construction — a record carries one
// header, read once, applying to every block in it — while running two
// free-running MCU timers. So a single record's difference is dominated by
// ±½-sample PAIRING JITTER: libhackmotion measured 89 and 99 ticks on two
// consecutive records of one capture against that session median of 59. At the
// device's ≈799 Hz internal rate a half sample is ~626 µs, so ~1250 µs of
// scatter is what a perfectly healthy device looks like.
//
// Two consequences, and both were got wrong before this header existed:
//
//   USE A MEDIAN, NOT A MEAN. A mean carries every jitter outlier into the
//     number that gets baked into swing.json as the session's skew.
//
//   ⚠ DO NOT TEST STABILITY WITH A MIN/MAX SPREAD. The spread measures the
//     jitter, not the offset, so a spread threshold fires on noise from a good
//     device — which is exactly what our 2 ms threshold would have done. §10.3
//     established stability by splitting a 238 s session and finding the two
//     halves' medians IDENTICAL, so that is the test: movement of the MEDIAN
//     between the first and second halves of the run.
//
// Pure and header-only so the arithmetic that decides a "your sensor is not
// behaving" warning is testable without a device.
#ifndef PINPOINT_HM_SKEW_STATS_H
#define PINPOINT_HM_SKEW_STATS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pinpoint::hm {

// Median of a COPY, so the caller's store keeps its arrival order — which is what
// lets the half-split below mean anything, and what keeps min/max describing the
// same unmodified run. Even counts take the mean of the two middle values. An
// empty run has no median and returns 0.0; callers must gate on the count rather
// than read that as a measurement, because 0 µs is a perfectly plausible skew.
inline double skewMedianUs(std::vector<int32_t> v)
{
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(mid), v.end());
    const double hi = static_cast<double>(v[mid]);
    if (v.size() % 2 != 0) return hi;
    // The lower middle is the largest element below the pivot, and nth_element has
    // already partitioned everything below `mid` for us — so this is a linear scan
    // of that partition rather than a second full selection.
    const auto lo = std::max_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(mid));
    return (static_cast<double>(*lo) + hi) / 2.0;
}

// Below this the two halves' medians are noise about each other and comparing them
// would manufacture a warning out of a short session.
inline constexpr size_t kMinSamplesForHalfSplit = 64;

struct HalfSplit {
    double deltaUs = 0.0;    // median(first half) − median(second half)
    bool   valid   = false;  // false when the run is too short to split
};

// ⚠ SPLIT IN ARRIVAL ORDER. The halves must be the first and second halves of the
// RUN; splitting sorted values would compare the low half against the high half and
// report a large "drift" for every session.
inline HalfSplit skewHalfSplit(const std::vector<int32_t> &samples)
{
    HalfSplit hs;
    if (samples.size() < kMinSamplesForHalfSplit)
        return hs;
    const size_t mid = samples.size() / 2;
    const std::vector<int32_t> first(samples.begin(),
                                     samples.begin() + static_cast<ptrdiff_t>(mid));
    const std::vector<int32_t> second(samples.begin() + static_cast<ptrdiff_t>(mid),
                                      samples.end());
    hs.deltaUs = skewMedianUs(first) - skewMedianUs(second);
    hs.valid   = true;
    return hs;
}

} // namespace pinpoint::hm

#endif // PINPOINT_HM_SKEW_STATS_H
