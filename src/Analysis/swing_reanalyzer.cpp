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

#include "swing_reanalyzer.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuaternion>
#include <QRectF>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <unordered_map>
#include <vector>

#include "format_descriptor.h"
#include "imu_sample.h"
#include "pixel_format_names.h"
#include "source_ring.h"
#include "swing_analysis.h"
#include "swing_payload_source.h"
#include "types.h"

#include "../Core/pp_debug.h"

using namespace pinpoint;

namespace pinpoint::analysis {

namespace {

std::vector<int64_t> readTimestamps(const QJsonArray& a)
{
    std::vector<int64_t> out;
    out.reserve(size_t(a.size()));
    for (const QJsonValue& v : a)
        out.push_back(int64_t(v.toDouble()));
    return out;
}

// Parse one IMU→segment binding from a JSON object carrying role + the A/M
// quaternions ([scalar,x,y,z]) + calibration provenance. Shared by both binding
// sources: analysis.bindings[] (analysed swings) and the per-stream device.*
// fallback (analysis-skipped corpus swings) — keeping the field set in one place.
ImuSegmentBinding parseBinding(const QJsonObject& b, SourceId source)
{
    const QJsonArray A = b[QStringLiteral("alignA")].toArray();
    const QJsonArray M = b[QStringLiteral("mountM")].toArray();
    ImuSegmentBinding bind;
    bind.source = source;
    bind.role   = SegmentRole(b[QStringLiteral("role")].toInt());
    bind.alignA = QQuaternion(float(A[0].toDouble()), float(A[1].toDouble()),
                              float(A[2].toDouble()), float(A[3].toDouble()));
    bind.mountM = QQuaternion(float(M[0].toDouble()), float(M[1].toDouble()),
                              float(M[2].toDouble()), float(M[3].toDouble()));
    bind.calibrated = !b.contains(QStringLiteral("calibrated"))
                   || b[QStringLiteral("calibrated")].toBool();
    bind.anatCalibrated       = b[QStringLiteral("anatCalibrated")].toBool(bind.calibrated);
    bind.mountDeviationDeg     = b[QStringLiteral("mountDeviationDeg")].toDouble();
    bind.mountGravityErrorDeg  = b[QStringLiteral("mountGravityErrorDeg")].toDouble();
    bind.calibratedAtUtc       = b[QStringLiteral("calibratedAt")].toString();
    bind.calibAgeSec           = b[QStringLiteral("calibAgeSec")].toDouble(-1.0);
    return bind;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-camera lazy frame readers. read(seq) streams ONE frame from disk into a
// single reusable buffer and returns a handle to it — valid until the next read()
// on the same reader (the analysis stages read frames strictly one-at-a-time, so
// a single buffer per camera satisfies the SwingPayloadSource contract). Memory
// stays bounded to one decoded frame per camera, never the whole window.
// ─────────────────────────────────────────────────────────────────────────────
class CameraReader {
public:
    virtual ~CameraReader() = default;
    virtual SourceRing::ReadHandle read(uint64_t seq) const noexcept = 0;
    FormatDescriptor fd;
};

// Raw sidecar: fixed-stride payloads, frame i at byte offset i*frameBytes — the
// exact bytes the live analyzer saw (bit-faithful). Random access is a seek+read.
class RawFrameReader final : public CameraReader {
public:
    QString path;
    qint64  frameBytes = 0;

    SourceRing::ReadHandle read(uint64_t seq) const noexcept override
    {
        if (!opened_) {
            file_.setFileName(path);
            opened_ = file_.open(QIODevice::ReadOnly);
        }
        if (!opened_ || frameBytes <= 0)
            return {};
        if (!file_.seek(qint64(seq) * frameBytes))
            return {};
        buf_.resize(size_t(frameBytes));
        const qint64 n = file_.read(reinterpret_cast<char*>(buf_.data()), frameBytes);
        if (n != frameBytes)
            return {};
        SourceRing::ReadHandle h;
        h.data  = buf_.data();
        h.bytes = size_t(frameBytes);
        return h;
    }

private:
    mutable QFile                  file_;
    mutable std::vector<std::byte> buf_;
    mutable bool                   opened_ = false;
};

// MP4 fallback (no raw sidecar): decode to BGR24. Decode-forward to the target
// frame (rewind on a back-seek) so the result is frame-accurate regardless of the
// container's keyframe layout — lossy, slower, used only when raw is absent.
class Mp4FrameReader final : public CameraReader {
public:
    QString path;

    SourceRing::ReadHandle read(uint64_t seq) const noexcept override
    {
        try {
            if (!opened_) {
                opened_   = cap_.open(path.toStdString());
                nextFrame_ = 0;
            }
            if (!opened_ || !cap_.isOpened())
                return {};
            if (int64_t(seq) < nextFrame_) {
                cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
                nextFrame_ = 0;
            }
            while (nextFrame_ < int64_t(seq)) {
                if (!cap_.grab())
                    return {};
                ++nextFrame_;
            }
            cv::Mat bgr;
            if (!cap_.read(bgr) || bgr.empty())
                return {};
            ++nextFrame_;
            if (!bgr.isContinuous())
                bgr = bgr.clone();
            const auto* p = reinterpret_cast<const std::byte*>(bgr.data);
            const size_t bytes = size_t(bgr.total() * bgr.elemSize());
            buf_.assign(p, p + bytes);
            SourceRing::ReadHandle h;
            h.data  = buf_.data();
            h.bytes = buf_.size();
            return h;
        } catch (...) {
            return {};
        }
    }

private:
    mutable cv::VideoCapture       cap_;
    mutable int64_t                nextFrame_ = 0;
    mutable std::vector<std::byte> buf_;
    mutable bool                   opened_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Disk-backed SwingPayloadSource: IMU samples fully in RAM (tiny), camera frames
// streamed via the readers above. The validate() seqlock check is moot (the bytes
// are stable on disk / in our owned vectors) so it always succeeds.
// ─────────────────────────────────────────────────────────────────────────────
class SwingDiskSource final : public SwingPayloadSource {
public:
    void addImu(SourceId id, FormatDescriptor fd, std::vector<ImuSample> samples)
    {
        imu_[id] = ImuStream{ std::move(fd), std::move(samples) };
    }
    void addCamera(SourceId id, std::unique_ptr<CameraReader> reader)
    {
        cam_[id] = std::move(reader);
    }

    SourceRing::ReadHandle payloadOf(SourceId id, uint64_t sequence) const noexcept override
    {
        if (auto it = imu_.find(id); it != imu_.end()) {
            const ImuStream& s = it->second;
            if (sequence >= s.samples.size())
                return {};
            SourceRing::ReadHandle h;
            h.data  = reinterpret_cast<const std::byte*>(&s.samples[sequence]);
            h.bytes = sizeof(ImuSample);
            return h;
        }
        if (auto it = cam_.find(id); it != cam_.end())
            return it->second->read(sequence);
        return {};
    }

    const FormatDescriptor& formatOf(SourceId id) const noexcept override
    {
        if (auto it = imu_.find(id); it != imu_.end())
            return it->second.fd;
        if (auto it = cam_.find(id); it != cam_.end())
            return it->second->fd;
        static const FormatDescriptor kEmpty{};
        return kEmpty;
    }

    bool validate(SourceId, const SourceRing::ReadHandle&) const noexcept override
    {
        return true;   // bytes are stable (disk / owned RAM) — no seqlock race
    }

private:
    struct ImuStream {
        FormatDescriptor       fd;
        std::vector<ImuSample> samples;
    };
    std::unordered_map<SourceId, ImuStream>                     imu_;
    std::unordered_map<SourceId, std::unique_ptr<CameraReader>> cam_;
};

} // namespace

LoadedSwing SwingDiskLoader::load(const QString& swingDir, const SwingLoadOptions& opts)
{
    LoadedSwing out;

    QFile f(swingDir + QStringLiteral("/swing.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        out.error = QStringLiteral("cannot open %1/swing.json").arg(swingDir);
        return out;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.isEmpty()) {
        out.error = QStringLiteral("swing.json parse failed");
        return out;
    }
    const QJsonObject analysisIn = root[QStringLiteral("analysis")].toObject();
    const QJsonObject captureIn  = root[QStringLiteral("capture")].toObject();

    auto src = std::make_unique<SwingDiskSource>();
    std::vector<IndexEntry> entries;
    ShotAnalysisJob         job;
    QHash<QString, SourceId> imuSerialToId;
    // Bindings rebuilt from each IMU stream's device.* calibration — the fallback
    // used when analysis.bindings is absent (analysis-skipped corpus swings).
    std::vector<ImuSegmentBinding> deviceBindings;

    int64_t  tMin   = INT64_MAX, tMax = INT64_MIN;
    SourceId nextId = 0;
    bool     anyRaw = false;

    struct VidTmp { SourceId id; bool faceOn; };
    std::vector<VidTmp> vids;

    for (const QJsonValue& sv : root[QStringLiteral("streams")].toArray()) {
        const QJsonObject s    = sv.toObject();
        const QString     kind = s[QStringLiteral("kind")].toString();

        if (kind == QLatin1String("video")) {
            std::vector<int64_t> tUs =
                readTimestamps(s[QStringLiteral("frames")].toObject()[QStringLiteral("t_us")].toArray());
            if (tUs.empty())
                continue;

            const QString alias  = s[QStringLiteral("alias")].toString();
            const QString serial = s[QStringLiteral("source")].toObject()[QStringLiteral("serial")].toString();
            const int encW = s[QStringLiteral("encoded")].toObject()[QStringLiteral("width")].toInt();
            const int encH = s[QStringLiteral("encoded")].toObject()[QStringLiteral("height")].toInt();
            int fpsNum = 150, fpsDen = 1;
            double exposureUs = 0.0;
            ExposureSource exposureSource = ExposureSource::Unknown;
            if (s.contains(QStringLiteral("capture"))) {
                const QJsonObject c = s[QStringLiteral("capture")].toObject();
                if (c[QStringLiteral("fps_num")].toInt() > 0 && c[QStringLiteral("fps_den")].toInt() > 0) {
                    fpsNum = c[QStringLiteral("fps_num")].toInt();
                    fpsDen = c[QStringLiteral("fps_den")].toInt();
                }
                exposureUs     = c[QStringLiteral("exposureUs")].toDouble(0.0);
                exposureSource = exposureSourceFromName(
                    c[QStringLiteral("exposureSource")].toString().toStdString(),
                    c[QStringLiteral("exposureAuto")].toBool(false));
            }

            // Prefer the raw sidecar (bit-faithful) when present on disk.
            bool    useRaw = false;
            QString rawPath, rawPf;
            int     rawW = 0, rawH = 0;
            qint64  rawStride = 0, rawFrameBytes = 0;
            if (s.contains(QStringLiteral("raw"))) {
                const QJsonObject r = s[QStringLiteral("raw")].toObject();
                rawPath = swingDir + QLatin1Char('/') + r[QStringLiteral("file")].toString();
                if (QFile::exists(rawPath)) {
                    useRaw        = true;
                    rawW          = r[QStringLiteral("width")].toInt();
                    rawH          = r[QStringLiteral("height")].toInt();
                    rawPf         = r[QStringLiteral("pixelFormat")].toString();
                    rawStride     = qint64(r[QStringLiteral("stride")].toDouble());
                    rawFrameBytes = qint64(r[QStringLiteral("frameBytes")].toDouble());
                }
            }
            anyRaw |= useRaw;

            FormatDescriptor fd;
            fd.device        = DeviceKind::Camera_UVC;
            fd.device_serial = serial.toStdString();
            CameraFormat cf{};
            cf.fps_numerator   = uint32_t(fpsNum);
            cf.fps_denominator = uint32_t(fpsDen);
            cf.exposure_us     = exposureUs;      // 0.0 for legacy swings (no exposure key)
            cf.exposure_source = exposureSource;
            std::unique_ptr<CameraReader> reader;
            if (useRaw) {
                cf.pixel_format      = pixelFormatFromName(rawPf.toStdString());
                cf.width             = uint32_t(rawW);
                cf.height            = uint32_t(rawH);
                cf.max_payload_bytes = uint32_t(rawFrameBytes);
                cf.plane_strides[0]  = uint32_t(rawStride);
                auto rr      = std::make_unique<RawFrameReader>();
                rr->path     = rawPath;
                rr->frameBytes = rawFrameBytes;
                reader       = std::move(rr);
            } else {
                cf.pixel_format      = PixelFormat::BGR24;
                cf.width             = uint32_t(encW);
                cf.height            = uint32_t(encH);
                cf.max_payload_bytes = uint32_t(encW * encH * 3);
                cf.plane_strides[0]  = uint32_t(encW * 3);
                auto mr  = std::make_unique<Mp4FrameReader>();
                mr->path = swingDir + QLatin1Char('/') + s[QStringLiteral("file")].toString();
                reader   = std::move(mr);
            }
            fd.format  = cf;
            reader->fd = fd;

            // Face-on: recorded perspective (FaceOn == 2) unless the tool forces a
            // substring match; legacy streams without "setup" fall back to it too.
            bool faceOn;
            if (s.contains(QStringLiteral("setup")) && !opts.faceOnExplicit)
                faceOn = (s[QStringLiteral("setup")].toObject()[QStringLiteral("perspective")].toInt() == 2);
            else
                faceOn = alias.contains(opts.faceOnSubstring, Qt::CaseInsensitive)
                      || s[QStringLiteral("file")].toString().contains(opts.faceOnSubstring, Qt::CaseInsensitive);

            // Hitting-area search box the face-on stream recorded (captures since
            // ball-ROI persistence) — BallRunner uses it instead of the pose stance
            // corridor, so re-analysis skips feet/shoe distractors like the live path.
            if (faceOn) {
                const QJsonArray sr = s[QStringLiteral("setup")].toObject()
                    [QStringLiteral("ballDetection")].toObject()
                    [QStringLiteral("searchRoi")].toArray();
                if (sr.size() == 4)
                    job.ballSearchRoi = QRectF(sr.at(0).toDouble(), sr.at(1).toDouble(),
                                               sr.at(2).toDouble(), sr.at(3).toDouble());

                // Persisted live empty-mat baseline (captures since baseline
                // persistence): BallRunner reconstructs the tracker from it instead
                // of self-seeding over the swing's opening frames, where the ball
                // already sits. Validate dims, roi rect, and the blob file before
                // trusting it — any failure warns and leaves the ref empty.
                const QJsonObject bl = s[QStringLiteral("setup")].toObject()
                    [QStringLiteral("ballDetection")].toObject()
                    [QStringLiteral("baseline")].toObject();
                if (!bl.isEmpty()) {
                    BallBaselineRef ref;
                    ref.path = swingDir + QLatin1Char('/') + bl[QStringLiteral("file")].toString();
                    ref.w    = bl[QStringLiteral("w")].toInt();
                    ref.h    = bl[QStringLiteral("h")].toInt();
                    const QJsonArray br = bl[QStringLiteral("roi")].toArray();
                    if (br.size() == 4)
                        ref.roi = QRectF(br.at(0).toDouble(), br.at(1).toDouble(),
                                         br.at(2).toDouble(), br.at(3).toDouble());
                    ref.rHat   = bl[QStringLiteral("rHat")].toDouble();
                    ref.fps    = bl[QStringLiteral("fps")].toDouble();
                    ref.noise0 = bl[QStringLiteral("noise0")].toDouble(1.0);
                    if (ref.w <= 0 || ref.h <= 0)
                        ppWarn() << "[Reanalysis]" << swingDir
                                 << "ballDetection.baseline has bad dims" << ref.w << "x"
                                 << ref.h << "— ignoring";
                    else if (br.size() != 4 || ref.roi.isEmpty())
                        ppWarn() << "[Reanalysis]" << swingDir
                                 << "ballDetection.baseline roi missing/empty — ignoring";
                    else if (!QFileInfo::exists(ref.path))
                        ppWarn() << "[Reanalysis]" << swingDir
                                 << "ballDetection.baseline blob missing at" << ref.path
                                 << "— ignoring";
                    else
                        job.ballBaseline = std::move(ref);
                }
            }

            const SourceId id = nextId++;
            for (size_t i = 0; i < tUs.size(); ++i)
                entries.push_back(IndexEntry{ tUs[i], id, uint64_t(i) });
            tMin = std::min(tMin, tUs.front());
            tMax = std::max(tMax, tUs.back());

            src->addCamera(id, std::move(reader));
            vids.push_back({ id, faceOn });

        } else if (kind == QLatin1String("imu")) {
            const QJsonObject samples = s[QStringLiteral("samples")].toObject();
            std::vector<int64_t> tUs = readTimestamps(samples[QStringLiteral("t_us")].toArray());

            std::vector<ImuSample> rows;
            rows.reserve(size_t(samples[QStringLiteral("data")].toArray().size()));
            for (const QJsonValue& dv : samples[QStringLiteral("data")].toArray()) {
                const QJsonArray d = dv.toArray();
                std::array<float, 10> a{};
                for (int i = 0; i < 10 && i < d.size(); ++i)
                    a[size_t(i)] = float(d.at(i).toDouble());
                rows.push_back(makeImuSample(a[0], a[1], a[2], a[3], a[4],
                                             a[5], a[6], a[7], a[8], a[9]));
            }
            if (rows.size() != tUs.size() || rows.empty())
                continue;

            const QString alias  = s[QStringLiteral("alias")].toString();
            const QString serial = s[QStringLiteral("source")].toObject()[QStringLiteral("serial")].toString();
            int rateHz = 200;
            if (s[QStringLiteral("device")].toObject()[QStringLiteral("outputRateHz")].toInt() > 0)
                rateHz = s[QStringLiteral("device")].toObject()[QStringLiteral("outputRateHz")].toInt();

            FormatDescriptor fd;
            fd.device        = DeviceKind::IMU_WitMotion;
            fd.device_serial = serial.toStdString();
            ImuFormat imf{};
            imf.device         = DeviceKind::IMU_WitMotion;
            imf.sample_rate_hz = uint32_t(rateHz);
            imf.packet_bytes   = sizeof(ImuSample);
            imf.packet_schema  = "imu_sample_v2";
            fd.format          = imf;

            const SourceId id = nextId++;
            for (size_t i = 0; i < tUs.size(); ++i)
                entries.push_back(IndexEntry{ tUs[i], id, uint64_t(i) });
            tMin = std::min(tMin, tUs.front());
            tMax = std::max(tMax, tUs.back());

            job.imuSources.push_back(id);
            if (!serial.isEmpty())
                imuSerialToId.insert(serial, id);
            src->addImu(id, fd, std::move(rows));

            // Fallback binding from the stream's device.* calibration (present only
            // when the exporter baked it in — every capture since corpus support).
            const QJsonObject deviceObj = s[QStringLiteral("device")].toObject();
            if (deviceObj[QStringLiteral("alignA")].toArray().size() == 4
                && deviceObj[QStringLiteral("mountM")].toArray().size() == 4)
                deviceBindings.push_back(parseBinding(deviceObj, id));

        } else if (kind == QLatin1String("ball")) {
            // v3.4 (design §9.7): NOT an EventBuffer/ring source — just carried
            // straight into job.ballTrack (Decision A, plan §1). t_us is in the
            // same window-relative domain as the video/imu streams above, so no
            // conversion is needed for internal consistency with tUs[i] derived
            // from this same reconstructed window elsewhere.
            const QJsonObject frames = s[QStringLiteral("frames")].toObject();
            std::vector<int64_t> bTUs = readTimestamps(frames[QStringLiteral("t_us")].toArray());
            const QJsonArray data = frames[QStringLiteral("data")].toArray();
            if (bTUs.size() != size_t(data.size()) || bTUs.empty())
                continue;

            analysis::BallTrack2D bt;
            bt.frames.reserve(bTUs.size());
            for (size_t i = 0; i < bTUs.size(); ++i) {
                const QJsonArray fr = data.at(int(i)).toArray();
                analysis::BallSample2D bs;
                bs.t_us = bTUs[i];
                if (fr.size() >= 5) {
                    bs.found      = fr.at(0).toDouble() != 0.0;
                    bs.center     = QPointF(fr.at(1).toDouble(), fr.at(2).toDouble());
                    bs.radiusNorm = float(fr.at(3).toDouble());
                    bs.conf       = float(fr.at(4).toDouble());
                }
                bt.frames.push_back(bs);
            }
            if (s.contains(QStringLiteral("launch"))) {
                const QJsonObject launch = s[QStringLiteral("launch")].toObject();
                bt.launchTUs    = int64_t(launch[QStringLiteral("t_us")].toDouble());
                bt.launchCenter = QPointF(launch[QStringLiteral("x")].toDouble(),
                                          launch[QStringLiteral("y")].toDouble());
            }
            job.ballTrack = std::move(bt);
        }
    }

    // Precedence (re-analysis only): a valid persisted baseline makes the
    // BallRunner re-run authoritative — the recorded "ball" stream carries a known
    // wrong-time-base accumulator snapshot (junk on live captures to date), so it
    // must not shadow the re-run. Empty ballTrack ⇒ the wrist_analyzer ladder falls
    // through to BallRunner.
    if (job.ballBaseline.isValid())
        job.ballTrack = {};

    // Sanity gate: drop a recorded ball stream whose WHOLE span lies outside the
    // reconstructed window [0, tMax−tMin] — the wrong-time-base recording bug
    // (warn-and-drop; the bug itself is out of scope). Conservative: only when
    // every frame is out, keyed off the time-ordered ends.
    if (!job.ballTrack.frames.empty() && tMax > tMin) {
        const int64_t span = tMax - tMin;
        if (job.ballTrack.frames.front().t_us > span
            || job.ballTrack.frames.back().t_us < 0) {
            ppWarn() << "[Reanalysis]" << swingDir
                     << "recorded ball stream lies wholly outside the window [0," << span
                     << "] (t_us" << job.ballTrack.frames.front().t_us << ".."
                     << job.ballTrack.frames.back().t_us << ") — dropping";
            job.ballTrack = {};
        }
    }

    if (vids.empty()) {
        out.error = QStringLiteral("no video streams in swing.json");
        return out;
    }

    // Camera order: face-on first (analyzers prefer it without re-sorting).
    for (const VidTmp& v : vids) {
        if (v.faceOn) {
            job.cameraSources.insert(job.cameraSources.begin(), v.id);
            ++job.faceOnCameraCount;
        } else {
            job.cameraSources.push_back(v.id);
        }
    }

    // Match the live window's time-ordered index (stable: per-source order kept).
    std::stable_sort(entries.begin(), entries.end(),
                     [](const IndexEntry& a, const IndexEntry& b) {
                         return a.timestamp_us < b.timestamp_us;
                     });

    // Session type — recorded capture.sessionType (caller may override later).
    if (captureIn.contains(QStringLiteral("sessionType"))
        && captureIn[QStringLiteral("sessionType")].toInt() >= 0)
        job.sessionType = captureIn[QStringLiteral("sessionType")].toInt();

    // Offline pose-model tier — recorded capture.motionCaptureQuality, so
    // re-analysis reproduces the model the swing was captured with (High ->
    // ViTPose++-L when the model is present on THIS host, else it degrades to B),
    // deterministically and independent of the re-running machine's live setting.
    // Absent on legacy swings ⇒ empty ⇒ ViTPose-B (unchanged historical behaviour).
    job.motionCaptureQuality = captureIn[QStringLiteral("motionCaptureQuality")].toString();

    // Impact, most-precise source first (all WINDOW-RELATIVE µs to match the
    // reconstructed timeline): capture.impactUs (exact back-dated estimate, already
    // relative — corpus swings incl. analysis-skipped) → the recorded Impact phase
    // (analysed swings) → thumbnail's nearest-captured-frame instant (quantized,
    // last resort).
    //   NB: analysis.phases t_us are ABSOLUTE (nowMicros — the analyzer ran on the
    //   live absolute window), unlike streams/thumbnail/capture.impactUs which are
    //   relative. Convert by subtracting clock.t0_us (the absolute window start).
    //   Synthetic swings have no clock block (t0=0) and already store relative
    //   phases, so the subtraction is a no-op there.
    const qint64 t0Us =
        qint64(root[QStringLiteral("clock")].toObject()[QStringLiteral("t0_us")].toDouble(0));
    qint64 impact = -1;
    if (captureIn.contains(QStringLiteral("impactUs")))
        impact = qint64(captureIn[QStringLiteral("impactUs")].toDouble(-1));
    if (impact <= 0) {
        for (const QJsonValue& pv : analysisIn[QStringLiteral("phases")].toArray())
            if (pv.toObject()[QStringLiteral("phase")].toInt() == int(Phase::Impact))
                impact = qint64(pv.toObject()[QStringLiteral("t_us")].toDouble()) - t0Us;
    }
    if (impact <= 0 && root.contains(QStringLiteral("thumbnail")))
        impact = qint64(root[QStringLiteral("thumbnail")].toObject()[QStringLiteral("t_us")].toDouble(-1));
    job.impactUs = impact;

    const QString hand = root[QStringLiteral("athlete")].toObject()[QStringLiteral("handedness")].toString();
    job.handedness = hand.compare(QLatin1String("Left"), Qt::CaseInsensitive) == 0  ? 2
                   : hand.compare(QLatin1String("Right"), Qt::CaseInsensitive) == 0 ? 1
                                                                                     : 0;

    // Club geometry (shaft-tracker E1 band matcher): recover the club that was
    // used from capture.club (persisted since the club-persistence change). Absent
    // on older swings → the shaft tracker runs ray-only (no band-lock DP wells).
    const QJsonObject clubIn = captureIn[QStringLiteral("club")].toObject();
    if (!clubIn.isEmpty()) {
        const double lmm = clubIn[QStringLiteral("lengthMm")].toDouble(0.0);
        if (lmm > 0.0) job.clubLengthM = lmm / 1000.0;
        job.shaftType = clubIn[QStringLiteral("shaftType")].toString();
        job.hoselFromButtMm = clubIn[QStringLiteral("hoselFromButtMm")].toDouble(0.0);
        for (const QJsonValue& v : clubIn[QStringLiteral("bandCentersMm")].toArray())
            job.bandCentersMm.push_back(v.toDouble());

        // Club-length prior (club_length_fusion.h): re-analysis replays the
        // RECORDED prior — the state the live fuse actually used for THIS shot —
        // never AppSettings (deterministic, cross-host). Absent block ⇒ the shot
        // ran prior-free; the fields keep their no-prior defaults.
        job.clubName = clubIn[QStringLiteral("name")].toString();
        const QJsonObject priorIn = clubIn[QStringLiteral("lengthPrior")].toObject();
        if (!priorIn.isEmpty()) {
            const double px = priorIn[QStringLiteral("px")].toDouble(-1.0);
            const int    n  = priorIn[QStringLiteral("n")].toInt(0);
            if (px > 0.0 && n > 0) {
                job.priorClubLenPx    = px;
                job.priorClubLenVarPx = priorIn[QStringLiteral("varPx")].toDouble(-1.0);
                job.priorClubLenN     = n;
            }
        }
    }

    // IMU → segment bindings: the persisted session A/M calibration, serial-keyed.
    // Identity A/M is NOT synthesised — re-fusing without the recorded calibration
    // would be fabrication (mirrors swinglab_run).
    for (const QJsonValue& bv : analysisIn[QStringLiteral("bindings")].toArray()) {
        const QJsonObject b  = bv.toObject();
        const auto        it = imuSerialToId.constFind(b[QStringLiteral("serial")].toString());
        if (it == imuSerialToId.constEnd())
            continue;
        job.imuBindings.push_back(parseBinding(b, it.value()));
    }

    // No analysis.bindings (analysis-skipped corpus swing) → use the per-stream
    // device.* calibration the exporter baked into each IMU stream.
    if (job.imuBindings.empty())
        job.imuBindings = std::move(deviceBindings);

    // Re-fusing on an uncalibrated A/M snapshot yields a confident-looking but
    // unreliable result — flag it, the way swinglab_run used to (don't silently
    // trust it into a corpus).
    for (const ImuSegmentBinding& b : job.imuBindings)
        if (!b.calibrated)
            ppWarn() << "[Reanalysis]" << swingDir << "binding role" << int(b.role)
                     << "recorded UNCALIBRATED (mount dev"
                     << b.mountDeviationDeg << "deg, gravity err"
                     << b.mountGravityErrorDeg << "deg) — results unreliable";

    out.window.emplace(std::move(src), std::move(entries), tMin, tMax);
    out.job     = std::move(job);
    out.usedRaw = anyRaw;
    out.ok      = true;
    return out;
}

ReanalyzeResult reanalyzeSwingDir(const QString& swingDir, const ReanalyzeOptions& opts)
{
    ReanalyzeResult out;

    LoadedSwing ls = SwingDiskLoader::load(swingDir);
    if (!ls.ok) {
        out.error = ls.error;
        return out;
    }
    out.usedRaw = ls.usedRaw;

    if (!opts.tuningOverrides.isEmpty())
        ls.job.tuningOverrides = opts.tuningOverrides;
    if (opts.sessionTypeOverride >= 0)
        ls.job.sessionType = opts.sessionTypeOverride;
    if (!opts.poseTrackPath.isEmpty())
        ls.job.poseTrackPath = opts.poseTrackPath;
    ls.job.fullWindow = opts.fullWindow;
    // Fail closed on an unknown discipline rather than silently analysing as Wrist
    // and writing a wrong-discipline analysis block back (our exports always carry
    // capture.sessionType; swinglab_run resolves its own default before analyze()).
    if (ls.job.sessionType < 0) {
        out.error = QStringLiteral("This shot's recording doesn't note its session type — "
                                   "it was captured before re-analysis was supported.");
        return out;
    }
    if (ls.job.impactUs <= 0) {
        out.error = QStringLiteral("This shot has no recorded impact time, so it can't be re-analysed.");
        return out;
    }
    // Wrist analysis needs at least ONE usable modality. Without IMU A/M bindings
    // the fuser produces nothing (wrist metrics can't be re-derived), but a face-on
    // camera still yields a valid vision-only analysis (pose + shaft track + hands-
    // only phase landmarks). Refuse only when NEITHER is present — that path would
    // return a degenerate result the caller would write back over the original.
    if (ls.job.sessionType == 1 && ls.job.imuBindings.empty() && ls.job.faceOnCameraCount == 0) {
        out.error = QStringLiteral("No IMU calibration and no face-on camera were saved with this "
                                   "shot, so it can't be re-analysed.");
        return out;
    }

    try {
        auto analyzer = makeShotAnalyzer(ls.job.sessionType);
        out.analysis  = analyzer->analyze(*ls.window, ls.job);
        out.ok        = out.analysis.ok;
        out.error     = out.analysis.error;
    } catch (const std::exception& e) {
        out.error = QStringLiteral("analyzer threw: %1").arg(QString::fromUtf8(e.what()));
    } catch (...) {
        out.error = QStringLiteral("analyzer threw an unknown exception");
    }
    return out;
}

} // namespace pinpoint::analysis
