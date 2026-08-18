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

// RAM-vs-disk SwingWindow parity — the accuracy gate for the streaming, disk-
// backed SwingWindow used by offline re-analysis. The same synthetic swing is
// built two ways and proven observationally identical:
//
//   W_ram  : registered in a real EventBuffer, frames+IMU written, captured.
//   W_disk : that window exported (SwingExporter, saveRaw) to a temp dir, then
//            reloaded via SwingDiskLoader (raw-sidecar backing — bit-faithful).
//
//   Test 1  byte-exact backing parity — counts, formats, every frame/IMU payload
//           (memcmp), interpolateImu over a sweep, window bounds. This exhaustively
//           covers the analyzer's observable surface → a deterministic analyzer
//           CANNOT diverge between the two backings.
//   Test 2  analyzer-layer parity — ImuVisionFuser (a real IMU analysis stage)
//           run on both windows must produce identical fused streams.
//   Test 3  MP4 fallback (tolerance) — reload the same swing via the lossy MP4
//           path: counts/format match, decoded pixels are close (not bit-exact).
//   Test 4  double-run analyzer determinism — WristAnalyzer::analyze() (the stage
//           pipeline, wrist_analyzer.cpp) run TWICE over the SAME job with identical
//           empty overrides must produce byte-identical "analysis" JSON (serialized
//           through the production SwingDocWriter::writeSwingJson seam, wall-clock
//           "timings" excluded by design), over the SAME RAM window Tests 1-3 built.
//           Run twice: 4a IMU-only (no camera), 4b camera path (+ face-on cam).
//
// Built as a tools target (PINPOINT_BUILD_TOOLS) in the root CMakeLists, beside
// swinglab_run, where the Buffer/Export/FFmpeg/OpenCV deps are already wired.

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "event_buffer.h"
#include "format_descriptor.h"
#include "imu_sample.h"
#include "source_descriptor.h"
#include "swing_window.h"

#include "frame_decode.h"
#include "swing_doc.h"
#include "swing_exporter.h"

#include "imu_vision_fuser.h"
#include "swing_reanalyzer.h"
#include "wrist_analyzer.h"

using namespace pinpoint;
using namespace pinpoint::analysis;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

namespace {

// ── Synthetic swing geometry ────────────────────────────────────────────────
constexpr int     kW       = 64;
constexpr int     kH       = 48;
constexpr int     kFrames  = 12;
constexpr int     kImu     = 30;
constexpr int64_t kT0      = 1'000'000;
constexpr int64_t kFrameDt = 10'000;   // 100 fps
constexpr int64_t kImuDt   = 5'000;    // 200 Hz
constexpr int64_t kImpact  = kT0 + 50'000;

// A smooth per-(x,y,frame) Bayer byte — gradients survive h264 near-losslessly,
// keeping the Test-3 tolerance meaningful; the value is still distinct per frame.
uint8_t bayerByte(int x, int y, int i)
{
    return uint8_t((x + y + i * 3) & 0xFF);
}

ImuSample makeSyntheticImu(int j)
{
    const float a   = float(j) * 0.03f;          // slowly rotating quaternion
    const float qw  = std::cos(a * 0.5f);
    const float qz  = std::sin(a * 0.5f);
    return makeImuSample(0.01f * j, -0.02f * j, 1.0f,       // accel (g)
                         0.5f * j, 0.25f * j, -0.1f * j,    // gyro (deg/s)
                         qw, 0.0f, 0.0f, qz);               // quat (wxyz)
}

// Distinct source ids present in a window (order-independent).
std::vector<SourceId> sourceIds(const SwingWindow& w)
{
    std::vector<SourceId> ids;
    for (const IndexEntry& e : w.entries())
        if (std::find(ids.begin(), ids.end(), e.source_id) == ids.end())
            ids.push_back(e.source_id);
    return ids;
}

// Match sources across two windows by device_serial (the absolute SourceId
// numbering need not agree between the two backings).
SourceId matchBySerial(const SwingWindow& w, const std::string& serial)
{
    for (SourceId id : sourceIds(w))
        if (w.formatOf(id).device_serial == serial)
            return id;
    return kInvalidSourceId;
}

bool cameraFormatsEqual(const CameraFormat& a, const CameraFormat& b)
{
    return a.pixel_format == b.pixel_format && a.width == b.width && a.height == b.height
        && a.fps_numerator == b.fps_numerator && a.fps_denominator == b.fps_denominator
        && a.max_payload_bytes == b.max_payload_bytes
        && a.plane_strides[0] == b.plane_strides[0];
}

// ── Build the in-RAM (ring-backed) window ───────────────────────────────────
struct RamSwing {
    std::unique_ptr<EventBuffer> buf;          // outlives the window (RingPayloadSource holds it)
    std::optional<SwingWindow>   window;
    SourceId                     camId = kInvalidSourceId;
    SourceId                     imuId = kInvalidSourceId;
    std::string                  camSerial = "CAMSERIAL1";
    std::string                  imuSerial = "IMUSER1";
};

RamSwing buildRamSwing()
{
    RamSwing r;
    r.buf = std::make_unique<EventBuffer>();
    EventBuffer& buf = *r.buf;

    SourceDescriptor cdesc;
    cdesc.name       = "FaceCam";
    cdesc.identifier = r.camSerial;
    {
        CameraFormat cf{};
        cf.pixel_format      = PixelFormat::BayerRG8;
        cf.width             = kW;
        cf.height            = kH;
        cf.fps_numerator     = 100;
        cf.fps_denominator   = 1;
        cf.max_payload_bytes = uint32_t(kW * kH);
        cf.plane_strides[0]  = uint32_t(kW);
        cdesc.format.device        = DeviceKind::Camera_UVC;
        cdesc.format.device_serial = r.camSerial;
        cdesc.format.format        = cf;
    }
    cdesc.window_duration          = std::chrono::milliseconds(8000);
    cdesc.expected_interarrival_us = std::chrono::microseconds(kFrameDt);
    r.camId = buf.registerSource(cdesc);

    SourceDescriptor idesc;
    idesc.name       = "Hand";
    idesc.identifier = r.imuSerial;
    {
        ImuFormat imf{};
        imf.device         = DeviceKind::IMU_WitMotion;
        imf.sample_rate_hz = 200;
        imf.packet_bytes   = sizeof(ImuSample);
        imf.packet_schema  = "imu_sample_v2";
        idesc.format.device        = DeviceKind::IMU_WitMotion;
        idesc.format.device_serial = r.imuSerial;
        idesc.format.format        = imf;
    }
    idesc.window_duration          = std::chrono::milliseconds(8000);
    idesc.expected_interarrival_us = std::chrono::microseconds(kImuDt);
    r.imuId = buf.registerSource(idesc);

    buf.start();

    // IMU samples first (cheap), then camera frames — at their real timestamps.
    for (int j = 0; j < kImu; ++j) {
        auto slot = buf.acquireWriteSlot(r.imuId);
        if (!slot.valid) continue;
        const ImuSample s = makeSyntheticImu(j);
        std::memcpy(slot.data, &s, sizeof s);
        *slot.bytes_written = uint32_t(sizeof s);
        *slot.timestamp_us  = kT0 + int64_t(j) * kImuDt;
        buf.publish(r.imuId, slot.sequence);
    }
    std::vector<uint8_t> frame(size_t(kW * kH));
    for (int i = 0; i < kFrames; ++i) {
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                frame[size_t(y * kW + x)] = bayerByte(x, y, i);
        auto slot = buf.acquireWriteSlot(r.camId);
        if (!slot.valid) continue;
        std::memcpy(slot.data, frame.data(), frame.size());
        *slot.bytes_written = uint32_t(frame.size());
        *slot.timestamp_us  = kT0 + int64_t(i) * kFrameDt;
        buf.publish(r.camId, slot.sequence);
    }

    buf.pause();
    const int64_t lastImu   = kT0 + int64_t(kImu - 1) * kImuDt;
    const int64_t lastFrame = kT0 + int64_t(kFrames - 1) * kFrameDt;
    r.window.emplace(buf.captureSwingWindow(kT0 - 1, std::max(lastImu, lastFrame) + 1));
    return r;
}

SwingExportJob baseExportJob(const RamSwing& ram, const QString& dir, bool saveRaw)
{
    SwingExportJob job;
    job.swingDir = dir;
    job.swingId  = "swing_0001";
    SwingExportCamera cam;
    cam.sourceId    = ram.camId;
    cam.alias       = "FaceCam";
    cam.fileName    = "FaceCam.mp4";
    cam.perspective = 2;   // FaceOn
    job.cameras.push_back(cam);
    job.imuAliasBySerial[QString::fromStdString(ram.imuSerial)] = "Hand";
    SwingImuDeviceInfo dev;
    dev.outputRateHz = 200;
    dev.role         = int(SegmentRole::LeadHand);
    dev.roleName     = "LeadHand";
    job.imuDeviceBySerial[QString::fromStdString(ram.imuSerial)] = dev;
    job.sessionType          = 1;
    job.codec                = "h264";
    job.crf                  = 23;
    job.saveImu              = true;
    job.saveRaw              = saveRaw;
    job.resolutionMode       = "native";
    job.imuFormat            = "json";
    job.athleteName          = "Test";
    job.handedness           = "Right";
    job.wallclockAnchorUtc   = QDateTime::currentDateTimeUtc();
    job.thumbnailSourceId    = ram.camId;
    job.thumbnailTimestampUs = kImpact;
    return job;
}

// Export W_ram to <dir> and write the swing.json (no analysis) the loader reads.
bool exportSwing(const SwingWindow& w, const SwingExportJob& job)
{
    const SwingExportResult res = SwingExporter::run(w, job);
    if (!res.ok) {
        std::fprintf(stderr, "  export failed: %s\n", res.error.toUtf8().constData());
        return false;
    }
    QString werr;
    if (!SwingDocWriter::writeSwingJson(job.swingDir, res.manifest, nullptr, &werr)) {
        std::fprintf(stderr, "  writeSwingJson failed: %s\n", werr.toUtf8().constData());
        return false;
    }
    return true;
}

// ── Test 1: bit-exact backing parity (raw path) ─────────────────────────────
void testBackingParity(const RamSwing& ram, const SwingWindow& disk)
{
    std::fprintf(stderr, "[Test 1] byte-exact RAM-vs-disk backing parity\n");
    const SwingWindow& w0 = *ram.window;

    CHECK(sourceIds(w0).size() == sourceIds(disk).size());

    // swing.json stores WINDOW-RELATIVE timestamps (exporter subtracts t0), so the
    // disk timeline is the RAM timeline shifted by a constant t0. Derive that shift
    // from the camera's first entry and require it to hold for every entry/source —
    // payloads are then compared bit-for-bit. (The analyzer is shift-invariant: it
    // works relative to impact, which swing.json stores in the same relative domain.)
    int64_t shift = 0;
    bool     shiftKnown = false;

    struct Pair { SourceId ram; SourceId disk; bool isImu; };
    const Pair pairs[] = {
        { ram.camId, matchBySerial(disk, ram.camSerial), false },
        { ram.imuId, matchBySerial(disk, ram.imuSerial), true  },
    };

    for (const Pair& p : pairs) {
        CHECK(p.disk != kInvalidSourceId);
        if (p.disk == kInvalidSourceId) continue;

        // Format descriptor parity.
        const FormatDescriptor& fr = w0.formatOf(p.ram);
        const FormatDescriptor& fd = disk.formatOf(p.disk);
        CHECK(fr.device == fd.device);
        CHECK(fr.device_serial == fd.device_serial);
        if (p.isImu) {
            const auto* ar = std::get_if<ImuFormat>(&fr.format);
            const auto* ad = std::get_if<ImuFormat>(&fd.format);
            CHECK(ar && ad);
            if (ar && ad) {
                CHECK(ar->sample_rate_hz == ad->sample_rate_hz);
                CHECK(ar->packet_bytes   == ad->packet_bytes);
                CHECK(ar->packet_schema  == ad->packet_schema);
            }
        } else {
            const auto* ar = std::get_if<CameraFormat>(&fr.format);
            const auto* ad = std::get_if<CameraFormat>(&fd.format);
            CHECK(ar && ad);
            if (ar && ad) CHECK(cameraFormatsEqual(*ar, *ad));
        }

        // Per-entry payload parity (chronological alignment) + constant shift.
        // Camera frames go through the raw sidecar → BIT-EXACT (memcmp). IMU goes
        // through inline JSON → float values, exact to a JSON double round-trip
        // (≤ ~1 ULP); compared within a tight epsilon (the binary/csv IMU sidecar
        // would be bit-exact, but json is the default corpus format).
        const std::vector<IndexEntry> er = w0.entriesFor(p.ram);
        const std::vector<IndexEntry> ed = disk.entriesFor(p.disk);
        CHECK(er.size() == ed.size());
        const size_t n = std::min(er.size(), ed.size());
        size_t mismatches = 0, shiftViolations = 0;
        double maxImuDelta = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const int64_t d = er[i].timestamp_us - ed[i].timestamp_us;
            if (!shiftKnown) { shift = d; shiftKnown = true; }
            else if (d != shift) ++shiftViolations;
            const SourceRing::ReadHandle hr = w0.payloadOf(er[i]);
            const SourceRing::ReadHandle hd = disk.payloadOf(ed[i]);
            if (!hr.data || !hd.data || hr.bytes != hd.bytes) { ++mismatches; continue; }
            if (p.isImu) {
                ImuSample sr{}, sd{};
                std::memcpy(&sr, hr.data, sizeof sr);
                std::memcpy(&sd, hd.data, sizeof sd);
                const float* fr2 = reinterpret_cast<const float*>(&sr);
                const float* fd2 = reinterpret_cast<const float*>(&sd);
                for (size_t k = 0; k < sizeof(ImuSample) / sizeof(float); ++k) {
                    const double delta = std::abs(double(fr2[k]) - double(fd2[k]));
                    maxImuDelta = std::max(maxImuDelta, delta);
                    if (delta > 1e-4 + 1e-4 * std::abs(double(fr2[k]))) ++mismatches;
                }
            } else if (std::memcmp(hr.data, hd.data, hr.bytes) != 0) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        CHECK(shiftViolations == 0);
        if (p.isImu)
            std::fprintf(stderr, "  imu: %zu entries, %zu mismatches, maxDelta=%.2e, shift=%lld\n",
                         n, mismatches, maxImuDelta, (long long)shift);
        else
            std::fprintf(stderr, "  cam: %zu entries, %zu payload mismatches (bit-exact), shift=%lld\n",
                         n, mismatches, (long long)shift);
    }

    // interpolateImu parity across a sweep — query RAM at T (absolute) and DISK at
    // T-shift (relative); the interpolated samples must be byte-identical.
    const SourceId imuD = matchBySerial(disk, ram.imuSerial);
    size_t interpMismatch = 0, interpAgree = 0;
    for (int64_t t = kT0 - 2 * kImuDt; t <= kT0 + int64_t(kImu) * kImuDt; t += 777) {
        std::byte a[sizeof(ImuSample)];
        std::byte b[sizeof(ImuSample)];
        const bool okR = w0.interpolateImu(ram.imuId, t, a, sizeof a);
        const bool okD = disk.interpolateImu(imuD, t - shift, b, sizeof b);
        CHECK(okR == okD);
        if (okR && okD) {
            if (std::memcmp(a, b, sizeof a) != 0) ++interpMismatch; else ++interpAgree;
        }
    }
    CHECK(interpAgree > 0);
    CHECK(interpMismatch == 0);
    std::fprintf(stderr, "  interpolateImu: %zu agree, %zu mismatch\n",
                 interpAgree, interpMismatch);
}

// ── Test 2: analyzer-layer parity (ImuVisionFuser) ──────────────────────────
void testFuserParity(const RamSwing& ram, const SwingWindow& disk)
{
    std::fprintf(stderr, "[Test 2] ImuVisionFuser RAM-vs-disk parity\n");

    auto binding = [](SourceId id) {
        ImuSegmentBinding b;
        b.source     = id;
        b.role       = SegmentRole::LeadHand;
        b.calibrated = true;            // alignA/mountM default to identity
        return b;
    };

    const FusedStreams sr =
        ImuVisionFuser::fuse(*ram.window, { binding(ram.imuId) });
    const FusedStreams sd =
        ImuVisionFuser::fuse(disk, { binding(matchBySerial(disk, ram.imuSerial)) });

    // The fused grids are the same length, shifted by the same constant t0 as the
    // raw timeline (RAM absolute vs disk window-relative).
    CHECK(sr.timeGrid.size() == sd.timeGrid.size());
    CHECK(sr.segments.size() == sd.segments.size());
    CHECK(!sr.segments.empty());
    if (sr.timeGrid.size() == sd.timeGrid.size() && !sr.timeGrid.empty()) {
        const int64_t gshift = sr.timeGrid.front() - sd.timeGrid.front();
        size_t gridShiftViolations = 0;
        for (size_t k = 0; k < sr.timeGrid.size(); ++k)
            if (sr.timeGrid[k] - sd.timeGrid[k] != gshift) ++gridShiftViolations;
        CHECK(gridShiftViolations == 0);
    }

    const size_t ns = std::min(sr.segments.size(), sd.segments.size());
    size_t quatMismatch = 0;
    for (size_t s = 0; s < ns; ++s) {
        const SegmentStream& a = sr.segments[s];
        const SegmentStream& b = sd.segments[s];
        CHECK(a.role == b.role);
        CHECK(a.qAnat.size() == b.qAnat.size());
        const size_t nq = std::min(a.qAnat.size(), b.qAnat.size());
        for (size_t k = 0; k < nq; ++k) {
            const QQuaternion d = a.qAnat[k] - b.qAnat[k];
            if (d.lengthSquared() > 1e-12f) ++quatMismatch;
        }
    }
    CHECK(quatMismatch == 0);
    std::fprintf(stderr, "  grid=%zu segments=%zu quat mismatches=%zu\n",
                 sr.timeGrid.size(), sr.segments.size(), quatMismatch);

    // ── The grid rate follows the data, and the FLOOR must bind here ─────────
    //
    // ⚠ THIS IS THE REGRESSION GATE FOR THE WHOLE CORPUS. This fixture is an
    // ordinary Witmotion-shaped capture, so its fastest lane is nowhere near the
    // deferred rates and gridHzForWindow() must return EXACTLY the rate this
    // pipeline hardcoded before Phase E. If it ever returns something else,
    // every existing swing's metrics move underneath us — silently, because
    // nothing else in this suite would notice.
    const double gridRam  = ImuVisionFuser::gridHzForWindow(*ram.window, { binding(ram.imuId) });
    const double gridDisk = ImuVisionFuser::gridHzForWindow(disk, { binding(matchBySerial(disk, ram.imuSerial)) });
    CHECK(gridRam  == ImuVisionFuser::kGridHzMin);
    CHECK(gridDisk == ImuVisionFuser::kGridHzMin);
    std::fprintf(stderr, "  gridHz ram=%.1f disk=%.1f (floor %.1f — corpus unmoved)\n",
                 gridRam, gridDisk, ImuVisionFuser::kGridHzMin);

    // And the peak measurement must actually SEE this lane, or the check above
    // would pass for the entirely wrong reason — a lane it could not measure
    // also lands on the floor.
    const double peak = ImuVisionFuser::peakHzFor(*ram.window, ram.imuId,
                                                  ImuVisionFuser::kProbeUs);
    CHECK(peak > 0.0);
    std::fprintf(stderr, "  measured peak rate on the fixture lane = %.1f Hz\n", peak);
}

// ── Test 3: MP4 fallback (tolerance) ────────────────────────────────────────
void testMp4Fallback(const RamSwing& ram, const SwingWindow& mp4)
{
    std::fprintf(stderr, "[Test 3] MP4 fallback (tolerance)\n");

    const SourceId camD = matchBySerial(mp4, ram.camSerial);
    CHECK(camD != kInvalidSourceId);
    if (camD == kInvalidSourceId) return;

    const std::vector<IndexEntry> er = ram.window->entriesFor(ram.camId);
    const std::vector<IndexEntry> ed = mp4.entriesFor(camD);
    CHECK(er.size() == ed.size());

    const FormatDescriptor& fd = mp4.formatOf(camD);
    const auto* cf = std::get_if<CameraFormat>(&fd.format);
    CHECK(cf != nullptr);
    if (cf) {
        CHECK(cf->pixel_format == PixelFormat::BGR24);
        CHECK(cf->width == kW && cf->height == kH);
    }

    // Compare one decoded frame: reference = demosaiced raw of W_ram; actual = the
    // MP4-decoded BGR. Lossy h264 → mean-abs-diff, not bit-exact.
    const size_t n = std::min(er.size(), ed.size());
    if (n > 0 && cf) {
        const size_t i = n / 2;
        const SourceRing::ReadHandle hr = ram.window->payloadOf(er[i]);
        const auto* rawCf = std::get_if<CameraFormat>(&ram.window->formatOf(ram.camId).format);
        cv::Mat ref;
        CHECK(rawCf && decodeToBgr(*rawCf, hr.data, hr.bytes, ref));

        const SourceRing::ReadHandle hd = mp4.payloadOf(ed[i]);
        CHECK(hd.data && hd.bytes == size_t(kW * kH * 3));
        const cv::Mat got(kH, kW, CV_8UC3,
                          const_cast<void*>(reinterpret_cast<const void*>(hd.data)));

        if (!ref.empty() && hd.data && ref.size() == got.size()) {
            cv::Mat diff;
            cv::absdiff(ref, got, diff);
            const double mad = cv::mean(diff)[0];
            CHECK(mad < 25.0);   // smooth gradient + CRF 23 → small residual
            std::fprintf(stderr, "  frame %zu mean-abs-diff = %.2f (threshold 25)\n", i, mad);
        }
    }
    std::fprintf(stderr, "  mp4 entries=%zu (ram=%zu)\n", ed.size(), er.size());
}

// ── Test 4: double-run analyzer determinism ─────────────────────────────────

// Path to the first differing value between two JSON trees ("" when equal).
// Object key sets are unioned so a key present on only one side is reported as
// a diff instead of silently skipped; array length mismatches are reported
// before recursing into elements.
QString firstJsonDiff(const QJsonValue &a, const QJsonValue &b, const QString &path)
{
    if (a.type() != b.type())
        return QStringLiteral("%1 (type %2 vs %3)").arg(path).arg(int(a.type())).arg(int(b.type()));

    if (a.isObject()) {
        const QJsonObject oa = a.toObject(), ob = b.toObject();
        QStringList keys = oa.keys();
        for (const QString &k : ob.keys())
            if (!keys.contains(k)) keys.append(k);
        keys.sort();
        for (const QString &k : keys) {
            const QString sub = path + QLatin1Char('.') + k;
            if (!oa.contains(k)) return sub + QStringLiteral(" (missing in run A)");
            if (!ob.contains(k)) return sub + QStringLiteral(" (missing in run B)");
            const QString d = firstJsonDiff(oa.value(k), ob.value(k), sub);
            if (!d.isEmpty()) return d;
        }
        return QString();
    }
    if (a.isArray()) {
        const QJsonArray aa = a.toArray(), ab = b.toArray();
        if (aa.size() != ab.size())
            return QStringLiteral("%1[] size %2 vs %3").arg(path).arg(aa.size()).arg(ab.size());
        for (int i = 0; i < aa.size(); ++i) {
            const QString d = firstJsonDiff(aa.at(i), ab.at(i), QStringLiteral("%1[%2]").arg(path).arg(i));
            if (!d.isEmpty()) return d;
        }
        return QString();
    }
    if (a != b) {
        auto leaf = [](const QJsonValue &v) {
            if (v.isString()) return v.toString();
            if (v.isDouble()) return QString::number(v.toDouble(), 'g', 17);
            if (v.isBool())   return QString::fromLatin1(v.toBool() ? "true" : "false");
            if (v.isNull())   return QStringLiteral("null");
            return QStringLiteral("?");
        };
        return QStringLiteral("%1: %2 != %3").arg(path, leaf(a), leaf(b));
    }
    return QString();
}

// Serialize a ShotAnalysisResult through the PRODUCTION swing.json seam
// (SwingDocWriter::writeSwingJson — the analysis-block builder is anonymous-
// namespace private to swing_doc.cpp, so writing + re-reading the file is the
// smallest available seam) and return its "analysis" object with the wall-
// clock-only "timings" key removed (the one expected divergence between two
// runs of a deterministic pipeline — covered separately by the presence-pattern
// check in runDoubleRunParity). A halted result (detail == nullptr) writes no
// "analysis" key at all; that reads back as an empty QJsonObject, same as its
// counterpart, so the two compare equal without special-casing here.
QJsonObject serializedAnalysisMinusTimings(const QString &dir, const ShotAnalysisResult &r)
{
    QString err;
    if (!SwingDocWriter::writeSwingJson(dir, QJsonObject(), r.detail.get(), &err)) {
        std::fprintf(stderr, "  writeSwingJson failed: %s\n", err.toUtf8().constData());
        return QJsonObject();
    }
    QFile f(dir + QStringLiteral("/swing.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "  reopen failed: %s\n", f.errorString().toUtf8().constData());
        return QJsonObject();
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    QJsonObject an = root.value(QStringLiteral("analysis")).toObject();
    an.remove(QStringLiteral("timings"));
    return an;
}

// Run WristAnalyzer::analyze() TWICE over the SAME job with identical (empty)
// tuningOverrides and assert the two results are indistinguishable — a determinism
// check on the stage pipeline (the analyzer must be a pure function of the frozen
// window + job). Serialization/diff machinery is shared with the RAM-vs-disk tests.
//
// imuBindings binds the fixture's ONE synthetic IMU source (ram.imuId) TWICE —
// once as LeadForearm, once as LeadHand. This is the construction that drives a
// REAL (non-halted) run rather than only the degraded path: metric_extractor.cpp
// requires a LeadForearm stream together with EITHER a LeadHand or a LeadUpperArm
// stream before it emits anything, and the fixture (built for Tests 1-3's byte-exact
// backing parity, not for exercising wrist kinematics) registers exactly one physical
// IMU source. Binding it under two roles is physically degenerate — both segments
// read the identical orientation stream, so the relative wrist angle is a fixed
// constant rather than a real swing — but it is numerically legitimate and fully
// deterministic (ImuVisionFuser::fuse has no dedup-by-source logic; see imu_vision_
// fuser.cpp), and it drives WristMetrics -> PhaseSegmenter -> ResemblanceStage ->
// AssessmentStage, instead of only RequireProductsStage's halt branch.
//
// withCamera additionally binds the fixture's face-on camera. No particular
// pose/ball/shaft outcome is assumed: PoseRunner::run() degrades to an empty
// track when no ViTPose model is resolvable at runtime, and BallRunner needs
// >= 26 frames (this fixture has 12) so it degrades regardless — but a model
// IS present in some build trees (checked in next to the test binary), in
// which case real inference runs instead. Either way both runs call
// PoseRunner/BallRunner/ShaftTracker with identical arguments over the SAME
// frozen window, so the two must agree regardless of which outcome occurs.
void runDoubleRunParity(const RamSwing &ram, bool withCamera)
{
    const char *tag = withCamera ? "4b" : "4a";
    std::fprintf(stderr, "[Test %s] double-run analyzer determinism (%s)\n", tag,
                 withCamera ? "camera path" : "IMU-only");

    ImuSegmentBinding fore, hand;
    fore.source = ram.imuId; fore.role = SegmentRole::LeadForearm; fore.calibrated = true;
    hand.source = ram.imuId; hand.role = SegmentRole::LeadHand;    hand.calibrated = true;

    ShotAnalysisJob job;
    job.sessionType   = 1;   // Wrist
    job.impactUs      = kImpact;
    job.handedness    = 1;   // right
    job.runAssessment = true;
    job.imuBindings   = { fore, hand };
    if (withCamera) {
        job.cameraSources     = { ram.camId };
        job.faceOnCameraCount = 1;
    }

    WristAnalyzer analyzer;
    const ShotAnalysisResult rA = analyzer.analyze(*ram.window, job);
    const ShotAnalysisResult rB = analyzer.analyze(*ram.window, job);

    CHECK(rA.ok == rB.ok);
    CHECK(rA.error == rB.error);
    CHECK(rA.score == rB.score);
    CHECK(rA.metrics == rB.metrics);
    CHECK(rA.tracePoints == rB.tracePoints);
    CHECK((rA.detail == nullptr) == (rB.detail == nullptr));

    if (rA.detail && rB.detail) {
        // Timings are wall-clock, so only their PRESENCE (stage ran vs skipped)
        // is comparable between two independently-timed runs, not the values.
        const AnalysisTimings &ta = rA.detail->timings;
        const AnalysisTimings &tb = rB.detail->timings;
        CHECK((ta.poseMs  >= 0) == (tb.poseMs  >= 0));
        CHECK((ta.ballMs  >= 0) == (tb.ballMs  >= 0));
        CHECK((ta.shaftMs >= 0) == (tb.shaftMs >= 0));
        CHECK((ta.totalMs >= 0) == (tb.totalMs >= 0));
        std::fprintf(stderr, "  ok=%d score=%d series=%zu timings-present(pose/ball/shaft/total)"
                             " A=%d/%d/%d/%d B=%d/%d/%d/%d\n",
                     rA.ok, rA.score, rA.detail->series.size(),
                     ta.poseMs >= 0, ta.ballMs >= 0, ta.shaftMs >= 0, ta.totalMs >= 0,
                     tb.poseMs >= 0, tb.ballMs >= 0, tb.shaftMs >= 0, tb.totalMs >= 0);
    } else {
        std::fprintf(stderr, "  ok=%d (halted) error A=\"%s\" B=\"%s\"\n", rA.ok,
                     rA.error.toUtf8().constData(), rB.error.toUtf8().constData());
    }

    QTemporaryDir dA, dB;
    CHECK(dA.isValid() && dB.isValid());
    const QJsonObject anA = serializedAnalysisMinusTimings(dA.path(), rA);
    const QJsonObject anB = serializedAnalysisMinusTimings(dB.path(), rB);
    const QString diff = firstJsonDiff(anA, anB, QStringLiteral("analysis"));
    CHECK(diff.isEmpty());
    if (!diff.isEmpty())
        std::fprintf(stderr, "  FIRST DIFF: %s\n", diff.toUtf8().constData());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    RamSwing ram = buildRamSwing();
    if (!ram.window) {
        std::fprintf(stderr, "FATAL: could not build in-RAM window\n");
        return 1;
    }

    // ── raw-sidecar export → disk window (Tests 1 & 2) ──
    QTemporaryDir rawDir;
    CHECK(rawDir.isValid());
    if (!exportSwing(*ram.window, baseExportJob(ram, rawDir.path(), /*saveRaw=*/true))) {
        std::fprintf(stderr, "FATAL: raw export failed\n");
        return 1;
    }
    LoadedSwing diskRaw = SwingDiskLoader::load(rawDir.path());
    CHECK(diskRaw.ok);
    CHECK(diskRaw.usedRaw);
    if (diskRaw.ok && diskRaw.window) {
        testBackingParity(ram, *diskRaw.window);
        testFuserParity(ram, *diskRaw.window);
    } else {
        std::fprintf(stderr, "FATAL: raw reload failed: %s\n", diskRaw.error.toUtf8().constData());
        ++g_failures;
    }

    // ── MP4-only export → disk window (Test 3) ──
    QTemporaryDir mp4Dir;
    CHECK(mp4Dir.isValid());
    if (exportSwing(*ram.window, baseExportJob(ram, mp4Dir.path(), /*saveRaw=*/false))) {
        LoadedSwing diskMp4 = SwingDiskLoader::load(mp4Dir.path());
        CHECK(diskMp4.ok);
        CHECK(!diskMp4.usedRaw);
        if (diskMp4.ok && diskMp4.window)
            testMp4Fallback(ram, *diskMp4.window);
    } else {
        std::fprintf(stderr, "FATAL: mp4 export failed\n");
        ++g_failures;
    }

    // ── double-run analyzer determinism (Test 4) — over the RAM window ──
    runDoubleRunParity(ram, /*withCamera=*/false);   // 4a: IMU-only
    runDoubleRunParity(ram, /*withCamera=*/true);    // 4b: camera path

    std::fprintf(stderr, "\n%s — %d check failure(s)\n",
                 g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
