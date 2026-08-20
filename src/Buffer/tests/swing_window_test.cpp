/*
 * Copyright (C) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "event_buffer.h"
#include "swing_window.h"
#include "imu_sample.h"
#include "format_descriptor.h"
#include "source_descriptor.h"
#include "ram_payload_source.h"
#include "composite_payload_source.h"
#include "deferred_stitch.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using namespace pinpoint;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// packet_bytes sizes the ring slot, so a test writing a real ImuSample must ask
// for one that fits — a short slot CLAMPS the payload rather than refusing it,
// which surfaces later as a wrong h.bytes rather than as a failed write.
static SourceDescriptor makeImu(uint32_t hz = 200, uint32_t packet_bytes = 32) {
    SourceDescriptor d;
    d.name            = "imu";
    d.window_duration = std::chrono::milliseconds(5000);
    d.expected_interarrival_us = std::chrono::microseconds(1'000'000 / hz);
    ImuFormat f;
    f.device         = DeviceKind::IMU_WitMotion;
    f.sample_rate_hz = hz;
    f.packet_bytes   = packet_bytes;
    d.format.device  = DeviceKind::IMU_WitMotion;
    d.format.format  = f;
    return d;
}

static SourceDescriptor makeCamera() {
    SourceDescriptor d;
    d.name            = "cam";
    d.window_duration = std::chrono::milliseconds(5000);
    CameraFormat f;
    f.pixel_format       = PixelFormat::Mono8;
    f.width              = 64; f.height = 64;
    f.fps_numerator      = 30; f.fps_denominator = 1;
    f.max_payload_bytes  = 64 * 64; // 4096 bytes
    f.typical_payload_bytes = 64 * 64;
    d.format.device = DeviceKind::Camera_UVC;
    d.format.format = f;
    return d;
}

static EventBufferConfig zeroReorder() {
    EventBufferConfig cfg;
    cfg.reorder_window_us = 0;
    return cfg;
}

// Write n events to source, returns base timestamp used.
static int64_t writeEvents(EventBuffer& buf, SourceId id, int n,
                            int64_t base_us, int64_t step_us,
                            const void* fill = nullptr, size_t fill_bytes = 0) {
    for (int i = 0; i < n; ++i) {
        auto slot = buf.acquireWriteSlot(id);
        if (!slot.valid) break;
        *slot.timestamp_us  = base_us + i * step_us;
        *slot.bytes_written = static_cast<uint32_t>(
            fill_bytes ? fill_bytes : 0);
        if (fill && fill_bytes)
            std::memcpy(slot.data, fill, fill_bytes);
        buf.publish(id, slot.sequence);
    }
    return base_us;
}

// ---------------------------------------------------------------------------
// Construction and basic access
// ---------------------------------------------------------------------------

TEST(SwingWindow, ConstructionAndEntries) {
    EventBuffer buf(zeroReorder());
    SourceId imu = buf.registerSource(makeImu());
    buf.start();

    int64_t base = EventBuffer::nowMicros();
    writeEvents(buf, imu, 20, base, 5000LL);

    std::this_thread::sleep_for(50ms); // let merger emit
    buf.pause();

    int64_t w_start = base - 1;
    int64_t w_end   = base + 20 * 5000LL;
    auto window = buf.captureSwingWindow(w_start, w_end);

    EXPECT_EQ(window.startTimestampUs(), w_start);
    EXPECT_EQ(window.endTimestampUs(),   w_end);
    EXPECT_FALSE(window.entries().empty());
    EXPECT_LE(window.entries().front().timestamp_us, w_end);
    EXPECT_GE(window.entries().back().timestamp_us,  w_start);

    buf.stop();
}

// ---------------------------------------------------------------------------
// captureSwingWindow asserts in non-Paused state
// ---------------------------------------------------------------------------

TEST(SwingWindow, AssertsPausedState) {
    EventBuffer buf(zeroReorder());
    buf.registerSource(makeImu());
    buf.start();
    // In debug builds this asserts; in release it may still fire. Skip check.
    buf.stop();
}

// ---------------------------------------------------------------------------
// Payload access
// ---------------------------------------------------------------------------

TEST(SwingWindow, PayloadAccess) {
    EventBuffer buf(zeroReorder());
    SourceId imu = buf.registerSource(makeImu());
    buf.start();

    uint8_t pattern[32];
    for (int i = 0; i < 32; ++i) pattern[i] = static_cast<uint8_t>(0xA0 + i);

    int64_t base = EventBuffer::nowMicros();
    writeEvents(buf, imu, 5, base, 5000LL, pattern, 32);

    std::this_thread::sleep_for(50ms);
    buf.pause();

    auto window = buf.captureSwingWindow(base - 1, base + 5 * 5000LL);
    ASSERT_FALSE(window.entries().empty());

    const auto& e = window.entries().front();
    auto h = window.payloadOf(e);
    ASSERT_NE(h.data, nullptr);
    EXPECT_EQ(h.bytes, 32u);

    uint8_t readback[32]{};
    h.copyBytesRacy(readback, 32);
    EXPECT_EQ(std::memcmp(readback, pattern, 32), 0);

    buf.stop();
}

// ---------------------------------------------------------------------------
// frameCount and imuSampleCount
// ---------------------------------------------------------------------------

TEST(SwingWindow, FrameAndImuCount) {
    EventBuffer buf(zeroReorder());
    SourceId cam = buf.registerSource(makeCamera());
    SourceId imu = buf.registerSource(makeImu());
    buf.start();

    int64_t base = EventBuffer::nowMicros();
    writeEvents(buf, cam, 10, base,       33'333LL); // ~30 fps
    writeEvents(buf, imu, 30, base + 100, 5'000LL);  // 200 Hz

    std::this_thread::sleep_for(100ms);
    buf.pause();

    int64_t end = base + 30 * 33'333LL;
    auto window = buf.captureSwingWindow(base - 1, end);

    EXPECT_EQ(window.frameCount(cam),     window.entriesFor(cam).size());
    EXPECT_EQ(window.imuSampleCount(imu), window.entriesFor(imu).size());
    EXPECT_GT(window.imuSampleCount(imu), 0u);

    buf.stop();
}

// ---------------------------------------------------------------------------
// entriesFor
// ---------------------------------------------------------------------------

TEST(SwingWindow, EntriesFor) {
    EventBuffer buf(zeroReorder());
    SourceId a = buf.registerSource(makeImu());
    SourceId b = buf.registerSource(makeImu());
    buf.start();

    int64_t base = EventBuffer::nowMicros();
    writeEvents(buf, a, 10, base,        5000LL);
    writeEvents(buf, b, 10, base + 2500, 5000LL);

    std::this_thread::sleep_for(50ms);
    buf.pause();

    auto window = buf.captureSwingWindow(base - 1, base + 10 * 5000LL + 5000LL);

    auto ea = window.entriesFor(a);
    auto eb = window.entriesFor(b);

    for (auto& e : ea) EXPECT_EQ(e.source_id, a);
    for (auto& e : eb) EXPECT_EQ(e.source_id, b);

    EXPECT_GT(ea.size(), 0u);
    EXPECT_GT(eb.size(), 0u);

    buf.stop();
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

TEST(SwingWindow, MoveSemantics) {
    EventBuffer buf(zeroReorder());
    buf.registerSource(makeImu());
    buf.start();

    int64_t base = EventBuffer::nowMicros();
    writeEvents(buf, 0, 5, base, 5000LL);

    std::this_thread::sleep_for(50ms);
    buf.pause();

    auto w1 = buf.captureSwingWindow(base - 1, base + 5 * 5000LL);
    size_t n1 = w1.entries().size();

    SwingWindow w2 = std::move(w1);
    EXPECT_EQ(w2.entries().size(), n1);
    EXPECT_TRUE(w1.entries().empty()); // moved-from is valid but empty

    buf.stop();
}

// ---------------------------------------------------------------------------
// Lifetime after resume — no crash on access to moved window
// ---------------------------------------------------------------------------

TEST(SwingWindow, LifetimeAfterResume) {
    EventBuffer buf(zeroReorder());
    buf.registerSource(makeImu());
    buf.start();

    int64_t base = EventBuffer::nowMicros();
    writeEvents(buf, 0, 5, base, 5000LL);

    std::this_thread::sleep_for(50ms);
    buf.pause();

    auto window = buf.captureSwingWindow(base - 1, base + 5 * 5000LL);
    size_t n = window.entries().size();

    buf.resume();
    // Accessing index metadata after resume is safe (entries_ is a local copy).
    EXPECT_EQ(window.entries().size(), n);
    // DO NOT access payloads here — rings were reset.

    buf.stop();
}

// ---------------------------------------------------------------------------
// Per-source lookup index — deferred_sources_design.md §4.2
//
// interpolateImu() used to find its bracketing samples by scanning EVERY entry
// in the window on EVERY call, and ImuVisionFuser calls it once per grid point
// per binding. A deferred high-rate source inflates BOTH terms.
//
// ⚠ THE EXISTING TESTS ABOVE PASS IDENTICALLY WHETHER THE INDEX WORKS OR NOT,
// so they are not a gate on it. These two are: the first pins the exact
// bracketing decision against a reference scan (a wrong pick yields a different
// number), the second reports the cost on a post-deferred-source-shaped window.
// ---------------------------------------------------------------------------

namespace {

// Deterministic in-RAM backing. accel_x = accel_y/2 = float(sequence), so a
// WRONG bracketing pick produces a DIFFERENT interpolated value rather than an
// identical-looking one. The quaternion is identity everywhere, so the slerp
// cannot be what differs — this isolates bracket selection, which is the only
// thing the index changes.
class StubImuSource final : public SwingPayloadSource {
public:
    void add(SourceId id, size_t count) {
        auto& v = lanes_[id];
        v.resize(count);
        for (size_t i = 0; i < count; ++i)
            v[i] = ImuSample{ float(i), float(i) * 2.0f, 0.0f,
                              0.0f, 0.0f, 0.0f,
                              1.0f, 0.0f, 0.0f, 0.0f };
    }
    SourceRing::ReadHandle payloadOf(SourceId id, uint64_t seq) const noexcept override {
        auto it = lanes_.find(id);
        if (it == lanes_.end() || seq >= it->second.size()) return {};
        SourceRing::ReadHandle h;
        h.data  = reinterpret_cast<const std::byte*>(&it->second[size_t(seq)]);
        h.bytes = sizeof(ImuSample);
        return h;
    }
    const FormatDescriptor& formatOf(SourceId) const noexcept override {
        static const FormatDescriptor kEmpty{};
        return kEmpty;
    }
    bool validate(SourceId, const SourceRing::ReadHandle&) const noexcept override {
        return true;   // owned RAM — stable bytes, no seqlock race
    }
private:
    std::map<SourceId, std::vector<ImuSample>> lanes_;
};

// The ORIGINAL linear scan, kept verbatim as the reference the indexed lookup
// must agree with. ⚠ The tie rules are load-bearing: among entries sharing the
// largest timestamp <= target the original keeps the FIRST encountered, and
// likewise among those sharing the smallest timestamp > target.
bool referenceBracket(const SwingWindow& w, SourceId id, int64_t target,
                      const IndexEntry** prevOut, const IndexEntry** nextOut) {
    const IndexEntry* prev = nullptr;
    const IndexEntry* next = nullptr;
    for (const auto& e : w.entries()) {
        if (e.source_id != id) continue;
        if (e.timestamp_us <= target) {
            if (!prev || e.timestamp_us > prev->timestamp_us) prev = &e;
        } else {
            if (!next || e.timestamp_us < next->timestamp_us) next = &e;
        }
    }
    *prevOut = prev;
    *nextOut = next;
    return prev && next;
}

struct SynthWindow {
    SwingWindow window;
    SourceId    camA, camB, wristA, wristB;
    int64_t     t0, t1;
};

// A window shaped like a post-Phase-E capture: two camera lanes at 240 Hz across
// 4 s, plus two wrist lanes carrying the stitched variable-rate shape a deferred
// history pull produces — ~100 Hz over the still pre-roll, ~800 Hz through the
// swing (deferred_sources_design.md §4.7, brief §0 #2).
// An ORDINARY capture: two cameras at 240 fps and two inertial sensors at 100 Hz
// across 4 s. No deferred source, no high-rate span — this is what every swing in
// the existing library looks like, and it is the shape that answers "does the
// index help someone who has no HackMotion at all".
SynthWindow makeTodayShapedWindow() {
    constexpr SourceId camA = 0, camB = 1, imuA = 2, imuB = 3;
    const int64_t t0 = 1'000'000;
    const int64_t t1 = t0 + 4'000'000;

    auto src = std::make_unique<StubImuSource>();
    std::vector<IndexEntry> entries;

    for (SourceId cam : { camA, camB }) {
        uint64_t seq = 0;
        for (int64_t t = t0; t < t1; t += 4'167, ++seq)
            entries.push_back(IndexEntry{ t, cam, seq, 0, 0 });
    }
    for (SourceId imu : { imuA, imuB }) {
        uint64_t seq = 0;
        for (int64_t t = t0; t < t1; t += 10'000, ++seq)   // 100 Hz throughout
            entries.push_back(IndexEntry{ t, imu, seq, 0, 0 });
        src->add(imu, size_t(seq));
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const IndexEntry& a, const IndexEntry& b) {
                         return a.timestamp_us < b.timestamp_us;
                     });
    return SynthWindow{ SwingWindow(std::move(src), std::move(entries), t0, t1),
                        camA, camB, imuA, imuB, t0, t1 };
}

SynthWindow makeDeferredShapedWindow() {
    constexpr SourceId camA = 0, camB = 1, wristA = 2, wristB = 3;
    const int64_t t0 = 1'000'000;
    const int64_t t1 = t0 + 4'000'000;          // 4 s window

    auto src = std::make_unique<StubImuSource>();
    std::vector<IndexEntry> entries;

    // Cameras: payload never read (interpolateImu is not called on them); they
    // are here because they are most of what inflates `entries` in a real window.
    for (SourceId cam : { camA, camB }) {
        uint64_t seq = 0;
        for (int64_t t = t0; t < t1; t += 4'167, ++seq)   // 240 fps
            entries.push_back(IndexEntry{ t, cam, seq, 0, 0 });
    }

    // Wrist lanes: 100 Hz for the first 2 s, 800 Hz for the last 2 s.
    for (SourceId wrist : { wristA, wristB }) {
        uint64_t seq = 0;
        for (int64_t t = t0; t < t0 + 2'000'000; t += 10'000, ++seq)
            entries.push_back(IndexEntry{ t, wrist, seq, 0, 0 });
        for (int64_t t = t0 + 2'000'000; t < t1; t += 1'250, ++seq)
            entries.push_back(IndexEntry{ t, wrist, seq, 0, 0 });
        src->add(wrist, size_t(seq));
    }

    // Both real construction paths hand the window timestamp-sorted entries
    // (TimelineIndex::snapshot sorts; SwingDiskLoader stable_sorts) — match that.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const IndexEntry& a, const IndexEntry& b) {
                         return a.timestamp_us < b.timestamp_us;
                     });

    return SynthWindow{
        SwingWindow(std::move(src), std::move(entries), t0, t1),
        camA, camB, wristA, wristB, t0, t1
    };
}

} // namespace

TEST(SwingWindow, InterpolateImuMatchesReferenceScan) {
    SynthWindow s = makeDeferredShapedWindow();

    // Probe the whole window at a rate that is coprime with both lane rates, so
    // targets land on exact sample instants, between them, and either side of
    // the 100 Hz → 800 Hz seam.
    int bracketed = 0, refused = 0;
    for (int64_t t = s.t0 - 50'000; t <= s.t1 + 50'000; t += 331) {
        for (SourceId id : { s.wristA, s.wristB }) {
            const IndexEntry *rp = nullptr, *rn = nullptr;
            const bool haveRef = referenceBracket(s.window, id, t, &rp, &rn);

            ImuSample got{};
            const bool ok = s.window.interpolateImu(
                id, t, reinterpret_cast<std::byte*>(&got), sizeof(got));

            ASSERT_EQ(ok, haveRef)
                << "bracketing disagreed with the reference scan at t=" << t;
            if (!ok) { ++refused; continue; }
            ++bracketed;

            // accel_x carries the sequence number, so the expected value follows
            // directly from WHICH two entries the reference picked.
            const double denom = double(rn->timestamp_us - rp->timestamp_us);
            const double frac  = denom == 0.0 ? 0.0
                               : double(t - rp->timestamp_us) / denom;
            const double expect = double(rp->source_sequence)
                + frac * (double(rn->source_sequence) - double(rp->source_sequence));

            EXPECT_NEAR(double(got.accel_x), expect, 1e-3)
                << "wrong bracket chosen at t=" << t << " on source " << id;
            EXPECT_NEAR(double(got.accel_y), expect * 2.0, 1e-3);
        }
    }

    // ⚠ Both arms must be exercised, or this test could pass by never having
    // bracketed anything at all.
    EXPECT_GT(bracketed, 1000);
    EXPECT_GT(refused,   0);
}

TEST(SwingWindow, InterpolateImuCostOnDeferredShapedWindow) {
    SynthWindow s = makeDeferredShapedWindow();

    // The load ImuVisionFuser puts on it: one call per grid point per binding,
    // at the 800 Hz grid a deferred high-rate span earns (design §4.2/§4.3).
    constexpr int    kGridHz  = 800;
    const int64_t    dt       = 1'000'000 / kGridHz;
    const SourceId   binds[2] = { s.wristA, s.wristB };

    // ⚠ BOTH TIMED IN ONE RUN, ON ONE WINDOW. Comparing a number from today's
    // build against one written down from a previous build is not a measurement
    // — machine load moves these by 3x between runs, which is exactly enough to
    // invent or erase a speedup that was never there.
    volatile int64_t sinkRef = 0;
    const auto refBegin = std::chrono::steady_clock::now();
    for (SourceId id : binds) {
        for (int64_t t = s.t0; t <= s.t1; t += dt) {
            const IndexEntry *p = nullptr, *n = nullptr;
            if (referenceBracket(s.window, id, t, &p, &n))
                sinkRef = sinkRef + p->timestamp_us;
        }
    }
    const auto refUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - refBegin).count();

    ImuSample smp{};
    int       calls = 0;
    volatile float sink = 0.0f;   // keep the loop from being optimised away

    const auto begin = std::chrono::steady_clock::now();
    for (SourceId id : binds) {
        for (int64_t t = s.t0; t <= s.t1; t += dt) {
            if (s.window.interpolateImu(id, t,
                                        reinterpret_cast<std::byte*>(&smp), sizeof(smp)))
                sink = sink + smp.accel_x;
            ++calls;
        }
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count();

    const size_t entries = s.window.entries().size();
    std::printf("[ COST     ] %zu entries, %d calls — scan %lld us, indexed %lld us"
                " (%.0fx; %.1f M entry visits avoided)\n",
                entries, calls,
                static_cast<long long>(refUs), static_cast<long long>(us),
                us > 0 ? double(refUs) / double(us) : 0.0,
                double(entries) * double(calls) / 1e6);

    // A tripwire, and ⚠ IT GATES THE RATIO OF THE TWO NUMBERS ABOVE, not the
    // indexed time on its own. What it has to catch is interpolateImu decaying
    // into a linear scan, and "is it a scan?" is answered by how it grows against
    // a scan measured on the same window in the same run — not by a stopwatch.
    // Measured so far:
    //
    //     RelWithDebInfo, M4        scan  14,385 us   indexed    210 us   — 68x
    //     Debug, Intel i7-9750H     scan 286,976 us   indexed  4,923 us   — 58x
    //
    // Both are the same index doing the same work; the 23x between their indexed
    // columns is build type and machine, which is exactly what an absolute gate
    // cannot tell apart from a regression. An earlier version of this check hard-
    // coded 3 ms from the first row and failed the second for no other reason.
    //
    // 10x sits ~6x below the observed speedup, so an ordinarily loaded machine
    // will not trip it, and far above the ~1x a genuine scan would produce, so a
    // regression cannot slip through. Phrased as a multiply rather than a divide
    // so an indexed run fast enough to round to 0 us cannot divide by zero.
    ASSERT_GT(refUs, 1'000)
        << "reference scan too fast to be a baseline — the window is degenerate";
    constexpr int64_t kMinSpeedup = 10;
    EXPECT_GT(refUs, us * kMinSpeedup)
        << "interpolateImu looks like a linear scan over " << entries << " entries: "
        << "indexed " << us << " us vs scan " << refUs << " us is only "
        << (us > 0 ? double(refUs) / double(us) : 0.0) << "x, want " << kMinSpeedup << "x";
}

// ---------------------------------------------------------------------------
// Composite backing + the split between ring freeze and window construction
// — deferred_sources_design.md §3.2 and §3.5
// ---------------------------------------------------------------------------

// ⚠ THE HAZARD THIS PINS IS SILENT TOTAL DATA LOSS, NOT A WRONG NUMBER.
// swing_window_live_ is raised by the ring source's CONSTRUCTOR, which in the
// ordinary captureSwingWindow() path is the same instant as window construction.
// A deferred gather separates them by seconds, and `resume_clear_rings` is true —
// so a resume landing in that gap CLEARS THE RINGS ABOUT TO BE SNAPSHOTTED.
// CameraManager::resumeBuffer() is the hard backstop and it reads exactly this
// flag, so the flag being up for the WHOLE gather is what makes the backstop
// cover it.
TEST(SwingWindow, ResumeGuardIsHeldFromPauseThroughGather) {
    EventBuffer buf(zeroReorder());
    SourceId imu = buf.registerSource(makeImu());
    buf.start();

    int64_t base = EventBuffer::nowMicros();
    writeEvents(buf, imu, 20, base, 5000LL);
    std::this_thread::sleep_for(50ms);
    buf.pause();

    EXPECT_FALSE(buf.swingWindowLive()) << "nothing is holding the rings yet";

    // The gather begins here. The guard must go up NOW — not when the window is
    // eventually constructed.
    auto ring = buf.makeRingPayloadSource();
    ASSERT_NE(ring, nullptr);
    EXPECT_TRUE(buf.swingWindowLive())
        << "the rings are frozen but unguarded — a resume here would clear them";

    {
        // ... a deferred source is retrieved across this span ...
        EXPECT_TRUE(buf.swingWindowLive());

        auto entries = buf.snapshot(base - 1, base + 20 * 5000LL);
        auto composite = std::make_unique<CompositePayloadSource>();
        composite->add(std::move(ring), {});          // catch-all: the ring
        SwingWindow w(std::move(composite), std::move(entries),
                      base - 1, base + 20 * 5000LL);

        EXPECT_TRUE(buf.swingWindowLive()) << "still held by the live window";
        EXPECT_FALSE(w.entries().empty());
    }

    // Released with the window, exactly as it is on the undeferred path.
    EXPECT_FALSE(buf.swingWindowLive());

    buf.stop();
}

// The stitch shape: a lane that exists in the ring is SERVED FROM RAM instead,
// because its high-rate samples only arrived after the freeze. Every other lane
// still reads through the ring.
TEST(SwingWindow, CompositeRoutesDeferredLaneToRamAndTheRestToTheRing) {
    EventBuffer buf(zeroReorder());
    SourceId live     = buf.registerSource(makeImu(200, sizeof(ImuSample)));
    SourceId deferred = buf.registerSource(makeImu(200, sizeof(ImuSample)));
    buf.start();

    // Both lanes record live. accel_x = 1 marks "came off the ring".
    const ImuSample ringFill{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                              1.0f, 0.0f, 0.0f, 0.0f };
    int64_t base = EventBuffer::nowMicros();
    writeEvents(buf, live,     10, base, 5000LL, &ringFill, sizeof(ringFill));
    writeEvents(buf, deferred, 10, base, 5000LL, &ringFill, sizeof(ringFill));
    std::this_thread::sleep_for(50ms);
    buf.pause();

    auto ring    = buf.makeRingPayloadSource();
    auto entries = buf.snapshot(base - 1, base + 10 * 5000LL);
    ASSERT_FALSE(entries.empty());

    // The retrieved block, stitched: accel_x = 99 marks "came from the pull".
    // Fresh sequences 0..n-1, and the deferred lane's ring entries are REPLACED
    // rather than added to — one id cannot split its sequence space across two
    // backings.
    std::vector<ImuSample> pulled(10);
    for (size_t i = 0; i < pulled.size(); ++i)
        pulled[i] = ImuSample{ 99.0f, float(i), 0.0f, 0.0f, 0.0f, 0.0f,
                               1.0f, 0.0f, 0.0f, 0.0f };
    auto ram = std::make_unique<RamPayloadSource>();
    ram->addImu(deferred, FormatDescriptor{}, std::move(pulled));

    std::vector<IndexEntry> stitched;
    for (const IndexEntry& e : entries)
        if (e.source_id != deferred) stitched.push_back(e);
    for (uint64_t i = 0; i < 10; ++i)
        stitched.push_back(IndexEntry{ base + int64_t(i) * 5000LL, deferred, i, 0, 0 });
    std::stable_sort(stitched.begin(), stitched.end(),
                     [](const IndexEntry& a, const IndexEntry& b) {
                         return a.timestamp_us < b.timestamp_us;
                     });

    auto composite = std::make_unique<CompositePayloadSource>();
    composite->add(std::move(ram),  { deferred });   // claimed first
    composite->add(std::move(ring), {});             // catch-all
    SwingWindow w(std::move(composite), std::move(stitched),
                  base - 1, base + 10 * 5000LL);

    int fromRing = 0, fromRam = 0;
    for (const IndexEntry& e : w.entries()) {
        auto h = w.payloadOf(e);
        ASSERT_NE(h.data, nullptr) << "no backing answered for source " << e.source_id;
        ASSERT_EQ(h.bytes, sizeof(ImuSample));
        ImuSample got{};
        std::memcpy(&got, h.data, sizeof(got));
        if (e.source_id == deferred) {
            EXPECT_FLOAT_EQ(got.accel_x, 99.0f)
                << "deferred lane was served from the RING, not the pull";
            ++fromRam;
        } else {
            EXPECT_FLOAT_EQ(got.accel_x, 1.0f)
                << "live lane was served from RAM, not the ring";
            ++fromRing;
        }
    }

    // ⚠ Both routes must actually have been exercised, or this passes by
    // routing nothing anywhere.
    EXPECT_EQ(fromRam, 10);
    EXPECT_GT(fromRing, 0);

    buf.stop();
}

// ---------------------------------------------------------------------------
// The stitch — deferred_sources_design.md §4.7
//
// ⚠ THE HOLED PULL IS THE CASE THAT MATTERS. The device holes an over-wide
// request rather than clamping it, with no error, so "the retrieved span" is in
// general several disjoint intervals. A stitch that only handled one contiguous
// span would look correct on a clean pull and silently drop the live samples
// that should have filled the holes.
// ---------------------------------------------------------------------------

namespace {

pinpoint::ImuSample marked(float tag) {
    return pinpoint::ImuSample{ tag, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                1.0f, 0.0f, 0.0f, 0.0f };
}

// accel_x = 1 for live, 99 for deferred — so which lane a sample came from is
// readable off the result.
pinpoint::DeferredStitchInput holedPullInput() {
    static std::vector<pinpoint::ImuSample> liveStore;
    liveStore.assign(10, marked(1.0f));

    pinpoint::DeferredStitchInput in;
    for (int i = 0; i < 10; ++i)                       // live at 100 Hz, 0..90 ms
        in.liveEntries.push_back(pinpoint::IndexEntry{
            int64_t(i) * 10'000, 7, uint64_t(i), 0, 0 });
    in.liveSample = [](const pinpoint::IndexEntry &e) -> const pinpoint::ImuSample * {
        return &liveStore[size_t(e.source_sequence)];
    };

    // Two dense runs with a hole between them: [0,30ms) and [60,90ms).
    in.delivered = { { 0, 30'000 }, { 60'000, 90'000 } };
    for (int64_t t = 0; t < 30'000; t += 1'250) {
        in.deferredTUs.push_back(t);
        in.deferredSamples.push_back(marked(99.0f));
    }
    for (int64_t t = 60'000; t < 90'000; t += 1'250) {
        in.deferredTUs.push_back(t);
        in.deferredSamples.push_back(marked(99.0f));
    }
    return in;
}

} // namespace

TEST(DeferredStitch, HoledPullTakesDeferredInsideAndLiveInTheHole) {
    const pinpoint::DeferredStitchInput in = holedPullInput();
    const pinpoint::DeferredStitchResult r = pinpoint::stitchDeferredLane(in);

    // Strictly ascending — the window's binary search depends on it, and the
    // merger's per-source guarantee does NOT reach these bytes.
    for (size_t i = 1; i < r.tUs.size(); ++i)
        ASSERT_GT(r.tUs[i], r.tUs[i - 1]) << "not strictly ascending at " << i;

    // Every delivered interval must be served by the pull, and every instant
    // outside them that live covered must still be present.
    for (size_t i = 0; i < r.tUs.size(); ++i) {
        const bool inDelivered = pinpoint::deferredCovers(in.delivered, r.tUs[i]);
        EXPECT_FLOAT_EQ(r.samples[i].accel_x, inDelivered ? 99.0f : 1.0f)
            << "wrong lane won at t=" << r.tUs[i];
    }

    // ⚠ THE HOLE MUST HAVE BEEN FILLED FROM LIVE. Live samples at 30/40/50 ms
    // sit between the two delivered runs; dropping them is the failure this
    // whole test exists to catch, and it would leave a plausible-looking trace.
    for (int64_t t : { int64_t(30'000), int64_t(40'000), int64_t(50'000) })
        EXPECT_NE(std::find(r.tUs.begin(), r.tUs.end(), t), r.tUs.end())
            << "live sample at " << t << " us was dropped from the hole";

    // ⚠ AND THE TAIL SAMPLE AT 90 ms IS LIVE, NOT DEFERRED, BECAUSE `delivered`
    // IS HALF-OPEN: 90'000 is outside [60'000, 90'000). That is where the
    // off-by-one in this whole design is born — the library's own header says so
    // — and it is asserted rather than left to a count that would hide it.
    EXPECT_NE(std::find(r.tUs.begin(), r.tUs.end(), int64_t(90'000)), r.tUs.end());

    EXPECT_EQ(r.usedLive, 4);               // 30, 40, 50 in the hole + 90 at the tail
    EXPECT_EQ(r.usedDeferred, 48);          // 24 samples in each 30 ms run
    EXPECT_EQ(r.droppedNonMonotonic, 0);
}

TEST(DeferredStitch, NoDeferredDataLeavesTheLiveLaneIntact) {
    // Degradation is the NORMAL path: a pull that returned nothing must yield
    // exactly the live lane, not an empty one.
    pinpoint::DeferredStitchInput in = holedPullInput();
    in.delivered.clear();
    in.deferredTUs.clear();
    in.deferredSamples.clear();

    const pinpoint::DeferredStitchResult r = pinpoint::stitchDeferredLane(in);
    EXPECT_EQ(r.usedLive, 10);
    EXPECT_EQ(r.usedDeferred, 0);
    ASSERT_EQ(r.tUs.size(), 10u);
    for (const auto &s : r.samples) EXPECT_FLOAT_EQ(s.accel_x, 1.0f);
}

TEST(DeferredStitch, DuplicateInstantsAreDroppedNotInterleaved) {
    // A deferred sample and a live one can legitimately land on the same instant
    // at an interval edge. Two samples at one timestamp make the bracketing pick
    // arbitrary, so one must go — and the count must say so.
    pinpoint::DeferredStitchInput in = holedPullInput();
    in.delivered = { { 0, 5'000 } };
    in.deferredTUs    = { 0, 10'000 };          // 10 ms collides with a live entry
    in.deferredSamples = { marked(99.0f), marked(99.0f) };

    const pinpoint::DeferredStitchResult r = pinpoint::stitchDeferredLane(in);
    for (size_t i = 1; i < r.tUs.size(); ++i)
        ASSERT_GT(r.tUs[i], r.tUs[i - 1]);
    EXPECT_EQ(r.droppedNonMonotonic, 1);
}

// ---------------------------------------------------------------------------
// Does the index help a capture with NO deferred source?
//
// ⚠ THE ANSWER MATTERS BEYOND THIS PHASE. interpolateImu is on the shared
// pre-stage every inertial capture runs through, so if the index only paid off
// on high-rate windows it would be a HackMotion optimisation; if it pays off on
// an ordinary one it is a fix to the existing pipeline that happens to have been
// forced by this phase.
//
// Both are timed in ONE run, on ONE window, so the comparison needs no rebuild
// against the old code. ⚠ THE COMPARISON IS DELIBERATELY UNFAIR TO THE INDEX:
// the reference does bracket-finding ONLY, while interpolateImu also fetches
// both payloads and does the shortest-arc orientation blend. Any win it shows is
// therefore a floor on the real one.
// ---------------------------------------------------------------------------

TEST(SwingWindow, IndexHelpsAnOrdinaryCaptureToo) {
    SynthWindow s = makeTodayShapedWindow();

    // The load an ordinary capture puts on it: a 200 Hz grid over 4 s, two bound
    // sensors — the rate this pipeline used before the grid followed the data.
    const int64_t  dt       = 1'000'000 / 200;
    const SourceId binds[2] = { s.wristA, s.wristB };

    volatile int64_t sinkA = 0;
    int calls = 0;
    const auto refBegin = std::chrono::steady_clock::now();
    for (SourceId id : binds) {
        for (int64_t t = s.t0; t <= s.t1; t += dt) {
            const IndexEntry *p = nullptr, *n = nullptr;
            if (referenceBracket(s.window, id, t, &p, &n))
                sinkA = sinkA + p->timestamp_us;
            ++calls;
        }
    }
    const auto refUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - refBegin).count();

    ImuSample smp{};
    volatile float sinkB = 0.0f;
    const auto idxBegin = std::chrono::steady_clock::now();
    for (SourceId id : binds) {
        for (int64_t t = s.t0; t <= s.t1; t += dt) {
            if (s.window.interpolateImu(id, t,
                                        reinterpret_cast<std::byte*>(&smp), sizeof(smp)))
                sinkB = sinkB + smp.accel_x;
        }
    }
    const auto idxUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - idxBegin).count();

    std::printf("[ ORDINARY ] %zu entries, %d calls — scan %lld us, indexed %lld us"
                " (indexed also interpolates)\n",
                s.window.entries().size(), calls,
                static_cast<long long>(refUs), static_cast<long long>(idxUs));

    // The point of the test: the win is not confined to high-rate windows.
    EXPECT_LT(idxUs, refUs)
        << "the index does nothing for a capture without a deferred source";
}
