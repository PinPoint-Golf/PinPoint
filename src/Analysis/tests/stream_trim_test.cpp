// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// stream_trim_test — trimming a window must change how many samples a stream
// carries and NOTHING ELSE about it.
// ---------------------------------------------------------------------------
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests --target stream_trim_test
//   ctest --test-dir build/analyzer-tests -R stream_trim --output-on-failure
//
// ⚠ THE FAILURE THIS GUARDS AGAINST HAS ALREADY HAPPENED, AND IT WAS INVISIBLE.
// trimStreams() rebuilds each SegmentStream field by field. `hackMotion` was added
// to SegmentStream and not added here, so on a HackMotion swing the binding was
// right, the fuser was right, the conjugate was applied — and the metric still
// came out keyed `leadWristFlexExt` instead of `hm.leadWristFlexExt`, because the
// one bit of provenance fell off between them. The curve was correct. Only its
// name was wrong, and nothing downstream can tell those apart.
//
// ⚠ SO THE TEST IS NOT "hackMotion survives a trim". That would pin the one field
// we already know about and would say nothing the day the NEXT field is added — a
// field-by-field copy defaults every field its author did not know existed, and
// that is the actual defect. What is pinned instead is the general property:
//
//     a stream whose every non-sample field is set to a NON-DEFAULT value must
//     come back from a trim with every one of those values intact.
//
// populate() below sets each such field away from its default, and
// checkCarried() asserts each one came back. Adding a field to SegmentStream
// without adding it to BOTH lists leaves this test passing on a stale definition,
// which is why kNonSampleFields is stated as a number and checked by eye against
// the struct — see the note there. It is the cheapest honest tripwire available
// without reflection.

#include "../stream_trim.h"

#include <QQuaternion>
#include <QVector3D>

#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

namespace {

// ⚠ THE NUMBER OF NON-SAMPLE FIELDS ON SegmentStream, i.e. every member that is
// NOT one of the three per-sample vectors (qAnat, gyroDps, accelG). Today: `role`
// and `hackMotion`. If you add a field to SegmentStream, bump this, set it in
// populate() and assert it in checkCarried() — the compile-time assert below only
// catches a change in the struct's SIZE, which a bool packed into existing padding
// would slip past, so the three lists are what actually keep this honest.
constexpr int kNonSampleFields = 2;

// ⚠ THE TRIPWIRE, AND IT IS A COMPILE ERROR RATHER THAN A FAILING ASSERT.
// A structured binding must name EVERY member of an aggregate, so adding a field
// to SegmentStream stops this file compiling, with the reader standing in the one
// place that has to be updated. That is the whole point: the bug being guarded
// against is a field nobody remembered, so the guard has to fire without anyone
// remembering to look. Portable — no sizeof, no layout assumption.
//
// If you are here because this broke: add your field to populate() (set it to a
// NON-default value) and to checkCarried() (assert it came back), bump
// kNonSampleFields if it is not a per-sample vector, then name it below.
inline void segmentStreamMemberCountTripwire()
{
    const SegmentStream s{};
    const auto &[role, qAnat, gyroDps, accelG, hackMotion] = s;
    (void)role; (void)qAnat; (void)gyroDps; (void)accelG; (void)hackMotion;
}

// A stream with EVERY non-sample field set away from its default, so that a field
// silently left behind by the copy comes back as the default and is caught.
SegmentStream populate(size_t n)
{
    SegmentStream s;
    // --- non-sample fields: all non-default (see kNonSampleFields) --------------
    s.role       = SegmentRole::LeadHand;   // default is Unknown
    s.hackMotion = true;                    // default is false
    // --- per-sample fields ------------------------------------------------------
    for (size_t i = 0; i < n; ++i) {
        s.qAnat.push_back(QQuaternion(1.0f, float(i), 0.0f, 0.0f));
        s.gyroDps.push_back(QVector3D(float(i), 0.0f, 0.0f));
        s.accelG.push_back(QVector3D(0.0f, float(i), 0.0f));
    }
    return s;
}

// Every non-sample field populate() set, asserted back.
void checkCarried(const SegmentStream &t, const char *what)
{
    char label[128];
    std::snprintf(label, sizeof(label), "%s: role carried", what);
    check(t.role == SegmentRole::LeadHand, label);
    std::snprintf(label, sizeof(label), "%s: hackMotion carried", what);
    check(t.hackMotion, label);
}

FusedStreams grid(size_t n)
{
    FusedStreams f;
    for (size_t i = 0; i < n; ++i)
        f.timeGrid.push_back(int64_t(1000 + i * 100));   // 1000, 1100, ... 100 µs apart
    return f;
}

// ---------------------------------------------------------------------------

void test_every_non_sample_field_survives_a_trim()
{
    FusedStreams in = grid(10);
    in.segments.push_back(populate(10));

    const FusedStreams out = trimStreams(in, 1300, 1600);   // indices 3..6

    check(out.segments.size() == 1, "the stream is kept");
    if (out.segments.empty()) return;
    checkCarried(out.segments[0], "trimmed");

    // And it really did trim, so the assertion above is not passing on an untouched
    // copy — a `return in` in the wrong place would satisfy checkCarried trivially.
    check(out.timeGrid.size() == 4, "grid trimmed to the requested span");
    check(out.segments[0].qAnat.size() == 4, "samples trimmed with it");
}

void test_provenance_is_not_special_cased_by_role()
{
    // hackMotion and role are independent: a lane can be either segment. Pinned
    // because the fix that added hackMotion touched the LeadHand path first, and a
    // copy that carried it only alongside a particular role would have passed a
    // single-case test.
    FusedStreams in = grid(6);
    SegmentStream fore = populate(6);
    fore.role = SegmentRole::LeadForearm;
    in.segments.push_back(fore);

    const FusedStreams out = trimStreams(in, 1100, 1400);
    check(out.segments.size() == 1, "forearm lane kept");
    if (out.segments.empty()) return;
    check(out.segments[0].role == SegmentRole::LeadForearm, "forearm role carried");
    check(out.segments[0].hackMotion, "hackMotion carried on the forearm lane too");
}

void test_degenerate_bounds_return_the_input_untouched()
{
    // a >= b — the whole grid is kept. It returns `in` by value, so provenance is
    // trivially intact here; asserted anyway because this branch is the one that
    // would keep passing if the copy below it were deleted entirely.
    FusedStreams in = grid(5);
    in.segments.push_back(populate(5));

    const FusedStreams out = trimStreams(in, 9000, 9999);   // wholly past the grid
    check(out.timeGrid.size() == 5, "grid untouched on degenerate bounds");
    check(out.segments.size() == 1, "stream untouched on degenerate bounds");
    if (!out.segments.empty()) checkCarried(out.segments[0], "degenerate");
}

void test_a_misaligned_stream_is_dropped_not_misaligned()
{
    // qAnat shorter than the grid ⇒ the stream cannot be indexed against it, so it
    // is dropped. Silently trimming it would put every sample against the wrong
    // timestamp, which reads as a real (wrong) curve rather than as missing data.
    FusedStreams in = grid(10);
    SegmentStream good = populate(10);
    SegmentStream bad  = populate(4);      // 4 quaternions against a 10-sample grid
    bad.role = SegmentRole::LeadForearm;
    in.segments.push_back(good);
    in.segments.push_back(bad);

    const FusedStreams out = trimStreams(in, 1200, 1700);
    check(out.segments.size() == 1, "the misaligned stream is dropped");
    if (out.segments.empty()) return;
    check(out.segments[0].role == SegmentRole::LeadHand, "the aligned one is the survivor");
    checkCarried(out.segments[0], "with a dropped sibling");
}

void test_partial_inertials_do_not_cost_the_orientation_stream()
{
    // gyroDps/accelG are copied only when they match the grid; qAnat is the one
    // that must. A lane with orientation but no inertials still trims — and still
    // carries its provenance.
    FusedStreams in = grid(8);
    SegmentStream s = populate(8);
    s.gyroDps.clear();
    s.accelG.resize(3);                    // wrong length, not empty
    in.segments.push_back(s);

    const FusedStreams out = trimStreams(in, 1100, 1500);
    check(out.segments.size() == 1, "orientation-only lane kept");
    if (out.segments.empty()) return;
    check(out.segments[0].qAnat.size() == 5, "orientation trimmed");
    check(out.segments[0].gyroDps.empty(), "absent gyro stays absent");
    check(out.segments[0].accelG.empty(), "mismatched accel is dropped, not sliced");
    checkCarried(out.segments[0], "orientation-only");
}

void test_timestamps_stay_absolute()
{
    // Trimming is a window operation. Re-basing the grid to zero here would leave
    // every later join against the swing's phases silently off by the window start.
    FusedStreams in = grid(10);
    in.segments.push_back(populate(10));

    const FusedStreams out = trimStreams(in, 1300, 1600);
    check(!out.timeGrid.empty() && out.timeGrid.front() == 1300, "first kept t_us is absolute");
    check(!out.timeGrid.empty() && out.timeGrid.back() == 1600, "last kept t_us is absolute");
}

void test_bounds_are_inclusive_at_both_ends()
{
    // lower_bound/upper_bound: fromUs is kept when it lands exactly on a sample,
    // and so is toUs. Pinned because swapping either for its counterpart loses one
    // sample at one end — small enough to never be noticed and enough to shift a
    // phase-sampled value.
    FusedStreams in = grid(10);
    in.segments.push_back(populate(10));

    const FusedStreams out = trimStreams(in, 1000, 1900);   // exactly the full span
    check(out.timeGrid.size() == 10, "an exact-span trim keeps every sample");
}

} // namespace

int main()
{
    std::printf("=== stream trim: a window operation and nothing else ===\n");
    std::printf("  (SegmentStream has %d non-sample fields — see kNonSampleFields)\n",
                kNonSampleFields);

    test_every_non_sample_field_survives_a_trim();
    test_provenance_is_not_special_cased_by_role();
    test_degenerate_bounds_return_the_input_untouched();
    test_a_misaligned_stream_is_dropped_not_misaligned();
    test_partial_inertials_do_not_cost_the_orientation_stream();
    test_timestamps_stay_absolute();
    test_bounds_are_inclusive_at_both_ends();

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
