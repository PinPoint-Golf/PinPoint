// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// hm_capture_provenance_test — what a recorded HackMotion window may claim.
// ---------------------------------------------------------------------------
//
// ⚠ THE FAILURE THIS GUARDS IS A SWING THAT LIES BY OMISSION. A HackMotion lane
// reaches swing.json as ten floats per sample, so a reading taken before the
// device was calibrated and one taken after are byte-identical in the recording —
// and the transform is applied ON-DEVICE and cannot be recovered later. Likewise
// a clipped channel saturates rather than wraps, so it arrives as a plausible
// flat top. If the provenance is wrong, nothing downstream can tell.
//
// So the assertions here are mostly about the ABSENT forms and the boundaries:
// that "not measured" never renders as a clean bill of health, that a window
// claims exactly the samples inside it and no others, and that the ring says so
// when it has dropped something rather than reporting a smaller count as if it
// were the truth.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "hm_capture_provenance.h"

using namespace pinpoint::hm;

namespace {

// Channel masks are wr_channel bits; the values are arbitrary here and only have
// to survive the round trip.
constexpr uint8_t kAccelX = 1u << 0;
constexpr uint8_t kGyroZ  = 1u << 5;

// wr_calibration_state, spelled locally so the test does not depend on the
// library header for three integers.
constexpr int kUnknown      = 0;
constexpr int kUncalibrated = 1;
constexpr int kCalibrated   = 2;

constexpr int kConfig7e = 0x7e;

void test_absent_is_not_clean()
{
    // ⚠ THE LOAD-BEARING CASE. A log that has seen nothing must not answer as
    // though it had seen good data: -1 is "not measured" and the exporter omits
    // the key entirely, whereas a 0 would be read as wr_calibration_state's
    // WR_CAL_UNKNOWN — a positive claim about a device that never streamed.
    const CaptureProvenanceLog log;
    const CaptureProvenance p = log.inWindow(0, 1'000'000);

    assert(p.calibration.stateAtStart == -1);
    assert(p.calibration.stateAtEnd   == -1);
    assert(!p.calibration.spansTransition);
    assert(p.configBits == -1);
    assert(p.exceptions.empty());
    assert(p.exceptionsDropped == 0);
    assert(p.noFitSkipped == 0);
}

void test_window_selects_only_what_it_contains()
{
    CaptureProvenanceLog log;
    // Three pinned samples: one before the window, one inside, one after.
    log.noteSample(1'000, kAccelX, 0, false, kCalibrated, kConfig7e);
    log.noteSample(5'000, kAccelX, 0, false, kCalibrated, kConfig7e);
    log.noteSample(9'000, kAccelX, 0, false, kCalibrated, kConfig7e);

    const CaptureProvenance p = log.inWindow(4'000, 6'000);
    assert(p.exceptions.size() == 1);
    assert(p.exceptions[0].hostTimeUs == 5'000);

    // ⚠ Inclusive at both ends, matching the window the exporter builds from the
    // same two timestamps. A sample exactly on a boundary belongs to the window; an
    // exclusive end here would drop the impact sample of a window that ends at it.
    const CaptureProvenance edges = log.inWindow(1'000, 9'000);
    assert(edges.exceptions.size() == 3);
}

void test_pinning_is_attributed_to_the_unit_that_clipped()
{
    CaptureProvenanceLog log;
    // Palm only — the physically expected case, since the palm unit sits at the
    // larger radius and saturates first (spec §6.4).
    log.noteSample(2'000, 0, kGyroZ, false, kCalibrated, kConfig7e);

    const CaptureProvenance p = log.inWindow(0, 10'000);
    assert(p.exceptions.size() == 1);
    assert(p.exceptions[0].unit == 1);              // WR_UNIT_PALM
    assert(p.exceptions[0].reason == SampleException::Pinned);
    assert(p.exceptions[0].channelMask == kGyroZ);  // WHICH channel, not merely that one did
}

void test_both_units_clipping_is_two_entries_not_one()
{
    CaptureProvenanceLog log;
    log.noteSample(3'000, kAccelX, kGyroZ, false, kCalibrated, kConfig7e);

    const CaptureProvenance p = log.inWindow(0, 10'000);
    assert(p.exceptions.size() == 2);
    assert(p.exceptions[0].unit == 0);   // lower arm
    assert(p.exceptions[1].unit == 1);   // palm
}

void test_suspect_norm_is_recorded_once_at_sample_level()
{
    // ⚠ A suspect quaternion norm says the DECODE may be misaligned, which is a
    // property of the record. Splitting it across the two units would double the
    // count and imply the library located one block and not the other.
    CaptureProvenanceLog log;
    log.noteSample(4'000, 0, 0, true, kCalibrated, kConfig7e);

    const CaptureProvenance p = log.inWindow(0, 10'000);
    assert(p.exceptions.size() == 1);
    assert(p.exceptions[0].unit == kSampleLevel);
    assert(p.exceptions[0].reason == SampleException::QuatNormSuspect);
}

void test_calibration_span_holds_across_a_quiet_window()
{
    CaptureProvenanceLog log;
    log.noteSample(1'000, 0, 0, false, kCalibrated, kConfig7e);
    log.noteSample(2'000, 0, 0, false, kCalibrated, kConfig7e);

    // A window entirely after the single transition: the state is known and did not
    // move, which is the ordinary case and must not set spansTransition.
    const CaptureProvenance p = log.inWindow(1'500, 5'000);
    assert(p.calibration.stateAtStart == kCalibrated);
    assert(p.calibration.stateAtEnd   == kCalibrated);
    assert(!p.calibration.spansTransition);
    assert(p.configBits == kConfig7e);
}

void test_calibration_change_inside_the_window_is_flagged()
{
    // The case the flag exists for: calibrated at the start, lost by the end. One
    // state value cannot express that, which is why the span carries two and a flag.
    CaptureProvenanceLog log;
    log.noteSample(1'000, 0, 0, false, kCalibrated,   kConfig7e);
    log.noteSample(5'000, 0, 0, false, kUncalibrated, kConfig7e);

    const CaptureProvenance p = log.inWindow(2'000, 8'000);
    assert(p.calibration.stateAtStart == kCalibrated);
    assert(p.calibration.stateAtEnd   == kUncalibrated);
    assert(p.calibration.spansTransition);
}

void test_stream_starting_inside_the_window_reports_its_first_state()
{
    // ⚠ Nothing precedes the window, so the stream began inside it. The first state
    // seen is the best available answer for the start — reporting -1 here would say
    // "no data" about a window that demonstrably has some.
    CaptureProvenanceLog log;
    log.noteSample(5'000, 0, 0, false, kUnknown, kConfig7e);

    const CaptureProvenance p = log.inWindow(1'000, 8'000);
    assert(p.calibration.stateAtStart == kUnknown);
    assert(p.calibration.stateAtEnd   == kUnknown);
    assert(!p.calibration.spansTransition);   // one state, not a transition
}

void test_window_before_any_sample_claims_nothing()
{
    CaptureProvenanceLog log;
    log.noteSample(9'000, kAccelX, 0, false, kCalibrated, kConfig7e);

    const CaptureProvenance p = log.inWindow(1'000, 2'000);
    assert(p.calibration.stateAtStart == -1);
    assert(p.exceptions.empty());
}

void test_ring_overflow_is_reported_and_keeps_the_recent_end()
{
    // ⚠ THE POINT OF THE COUNTER. Once the ring wraps, the entries are a FLOOR and
    // not a count — and a reader told only "N pinned samples" would take a
    // truncated number for the truth. Sustained pinning also invalidates the rarity
    // assumption the ring is sized on, which is a design signal, not just a stat.
    CaptureProvenanceLog log;
    const size_t n = CaptureProvenanceLog::kMaxExceptions + 100;
    for (size_t i = 0; i < n; ++i)
        log.noteSample(int64_t(i) + 1, kAccelX, 0, false, kCalibrated, kConfig7e);

    const CaptureProvenance p = log.inWindow(0, int64_t(n) + 1);
    assert(p.exceptionsDropped == 100);
    assert(p.exceptions.size() == CaptureProvenanceLog::kMaxExceptions);

    // The recent end is what a just-exported swing needs, and the list must still
    // read forwards in time after wrapping.
    assert(p.exceptions.back().hostTimeUs == int64_t(n));
    for (size_t i = 1; i < p.exceptions.size(); ++i)
        assert(p.exceptions[i - 1].hostTimeUs <= p.exceptions[i].hostTimeUs);
}

void test_no_fit_skips_are_counted_not_windowed()
{
    // ⚠ A sample skipped for having no mapped host time has no host time to place it
    // in a window with, so it is a session total by necessity. The test pins that it
    // is reported at all — the count is the only trace that samples went missing.
    CaptureProvenanceLog log;
    log.noteNoFitSkipped();
    log.noteNoFitSkipped();

    assert(log.inWindow(0, 1).noFitSkipped == 2);
    assert(log.inWindow(1'000'000, 2'000'000).noFitSkipped == 2);
}

} // namespace

int main()
{
    test_absent_is_not_clean();
    test_window_selects_only_what_it_contains();
    test_pinning_is_attributed_to_the_unit_that_clipped();
    test_both_units_clipping_is_two_entries_not_one();
    test_suspect_norm_is_recorded_once_at_sample_level();
    test_calibration_span_holds_across_a_quiet_window();
    test_calibration_change_inside_the_window_is_flagged();
    test_stream_starting_inside_the_window_reports_its_first_state();
    test_window_before_any_sample_claims_nothing();
    test_ring_overflow_is_reported_and_keeps_the_recent_end();
    test_no_fit_skips_are_counted_not_windowed();

    std::printf("hm_capture_provenance_test: all assertions passed\n");
    return 0;
}
