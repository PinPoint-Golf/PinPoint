// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// hm_skew_stats_test — the aggregation that decides whether we tell a coach
// their sensor is misbehaving.
// ---------------------------------------------------------------------------
//
// ⚠ THE FAILURE THIS GUARDS AGAINST HAS ALREADY HAPPENED ONCE. Our first version
// warned when the min/max SPREAD of `skew_us` exceeded 2 ms. libhackmotion's 0x90
// analysis then showed that a single record's difference is dominated by ±½-sample
// pairing jitter — ~1250 µs of scatter at the device's internal rate on a perfectly
// healthy unit — so that threshold fired on noise. The spread measures the jitter;
// only the median measures the offset.
//
// So these tests pin two things: that the median is right (including the even-count
// branch, which is the subtle one), and that stability is judged by movement of the
// median between the halves of a RUN, the way §10.3 judged it.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "hm_skew_stats.h"

using namespace pinpoint::hm;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// A run of `n` values around `centre` with alternating ±jitter — the shape a real
// capture has: a stable offset buried in half-sample pairing noise.
std::vector<int32_t> run(size_t n, int32_t centre, int32_t jitter)
{
    std::vector<int32_t> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i)
        v.push_back(centre + (i % 2 == 0 ? jitter : -jitter));
    return v;
}

void test_empty_has_no_median()
{
    // 0 µs is a perfectly plausible skew, so the caller must gate on the count.
    // This pins the value the caller will see, not a claim that it is meaningful.
    assert(near(skewMedianUs({}), 0.0));
}

void test_odd_count_is_the_middle_value()
{
    assert(near(skewMedianUs({ 5, 1, 3 }), 3.0));
    assert(near(skewMedianUs({ 900 }), 900.0));
}

void test_even_count_averages_the_two_middles()
{
    // ⚠ The subtle branch. nth_element positions the upper middle and partitions
    // everything below it; the lower middle is the max of that partition. Getting
    // this wrong returns the upper middle for every even run — right half the time,
    // which is exactly how it would survive a casual test.
    assert(near(skewMedianUs({ 1, 2, 3, 4 }), 2.5));
    assert(near(skewMedianUs({ 4, 3, 2, 1 }), 2.5));   // order must not matter
    assert(near(skewMedianUs({ 10, 20 }), 15.0));
}

void test_median_ignores_the_jitter_a_mean_would_absorb()
{
    // The real case: a stable 921 µs offset (§10.3's 59 ticks) with two big
    // single-record outliers of the kind libhackmotion measured (89 and 99 ticks).
    std::vector<int32_t> v = run(100, 921, 5);
    v.push_back(1390);
    v.push_back(1546);

    const double median = skewMedianUs(v);
    assert(median > 900.0 && median < 940.0);

    // The same data through a mean drifts toward the outliers. This is not a test of
    // the mean — it is the reason the mean was replaced, kept where it can be read.
    double sum = 0.0;
    for (int32_t s : v) sum += s;
    const double mean = sum / double(v.size());
    assert(mean > median);
}

void test_short_runs_are_not_judged()
{
    // Two medians over a handful of samples are noise about each other, and warning
    // on that would manufacture a fault out of a session that just started.
    const HalfSplit hs = skewHalfSplit(run(kMinSamplesForHalfSplit - 1, 921, 5));
    assert(!hs.valid);
    assert(near(hs.deltaUs, 0.0));
}

void test_a_stable_run_reports_no_movement()
{
    // §10.3's own result: identical medians across the halves of one session. Jitter
    // is present throughout and must not register as drift.
    const HalfSplit hs = skewHalfSplit(run(400, 921, 600));
    assert(hs.valid);
    assert(near(hs.deltaUs, 0.0));
}

void test_a_drifting_run_is_caught()
{
    // First half around 921 µs, second around 1121 — a 200 µs move of the offset
    // itself, which is the thing Phase E/G may not assume away.
    std::vector<int32_t> v = run(200, 921, 5);
    const std::vector<int32_t> late = run(200, 1121, 5);
    v.insert(v.end(), late.begin(), late.end());

    const HalfSplit hs = skewHalfSplit(v);
    assert(hs.valid);
    assert(near(hs.deltaUs, -200.0));   // first − second
}

void test_the_split_is_by_arrival_not_by_value()
{
    // ⚠ Splitting SORTED values would compare the low half against the high half and
    // report a large drift for every session that has any jitter at all. A stable run
    // whose values happen to arrive in ascending order is the case that separates the
    // two implementations: sorted-split says "drifting", arrival-split says so too —
    // and here it is CORRECT to say so, because ascending arrival IS a drift.
    std::vector<int32_t> ascending;
    for (int32_t i = 0; i < 200; ++i)
        ascending.push_back(800 + i);
    const HalfSplit rising = skewHalfSplit(ascending);
    assert(rising.valid);
    assert(rising.deltaUs < 0.0);       // later values are larger

    // Whereas the same VALUES shuffled into a stable interleave must report none.
    const HalfSplit stable = skewHalfSplit(run(200, 900, 100));
    assert(stable.valid);
    assert(near(stable.deltaUs, 0.0));
}

void test_negative_skew_is_ordinary()
{
    // palm − lower_arm has no reason to be positive; nothing may assume a sign.
    assert(near(skewMedianUs({ -900, -920, -910 }), -910.0));
    const HalfSplit hs = skewHalfSplit(run(200, -921, 5));
    assert(hs.valid);
    assert(near(hs.deltaUs, 0.0));
}

} // namespace

int main()
{
    test_empty_has_no_median();
    test_odd_count_is_the_middle_value();
    test_even_count_averages_the_two_middles();
    test_median_ignores_the_jitter_a_mean_would_absorb();
    test_short_runs_are_not_judged();
    test_a_stable_run_reports_no_movement();
    test_a_drifting_run_is_caught();
    test_the_split_is_by_arrival_not_by_value();
    test_negative_skew_is_ordinary();

    std::printf("hm_skew_stats_test: all assertions passed\n");
    return 0;
}
