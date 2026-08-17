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

#include "swing_exporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <variant>

#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>
#include <QTimeZone>

#include <opencv2/imgproc.hpp>

#include "format_descriptor.h"
#include "frame_decode.h"
#include "imu_sample.h"
#include "pixel_format_names.h"   // canonical pixelFormatName (shared with the reanalyzer)
#include "swing_paths.h"
#include "swing_window.h"
#include "video_encoder.h"
#include "../Core/pp_debug.h"

namespace pinpoint {

namespace {

QJsonArray toJsonTimestamps(const std::vector<int64_t>& tUs)
{
    QJsonArray a;
    for (int64_t t : tUs)
        a.append(static_cast<qint64>(t));
    return a;
}

// Export-time target size for a source frame, honoring AppSettings
// videoResolutionMode. Preserves aspect, forces even dims (libx264/yuv420p), and
// NEVER upscales — mirroring the exporter's "crop, never pad" rule (we don't
// invent pixels). "1080p"/"4k" fit to that many scan lines; "half" halves both
// axes; "native"/unknown keep the (even-cropped) source.
void exportTargetSize(int srcW, int srcH, const QString& mode, int& outW, int& outH)
{
    const int evenW = srcW & ~1;
    const int evenH = srcH & ~1;

    if (mode == QLatin1String("half")) {
        outW = (srcW / 2) & ~1;
        outH = (srcH / 2) & ~1;
        return;
    }

    int targetH = 0;
    if (mode == QLatin1String("1080p"))    targetH = 1080;
    else if (mode == QLatin1String("4k"))  targetH = 2160;
    else { outW = evenW; outH = evenH; return; }   // "native" / unknown

    if (targetH >= srcH) { outW = evenW; outH = evenH; return; }   // never upscale
    const double scale = static_cast<double>(targetH) / static_cast<double>(srcH);
    outW = static_cast<int>(std::lround(srcW * scale)) & ~1;
    outH = targetH & ~1;
}

// Demosaics the frame nearest impactUs from one camera and writes it as
// thumb.jpg (≤480 px wide) in the swing dir.  One small frame off the hot
// path — the extra demosaic is irrelevant next to the encode loop.  Returns
// the absolute entry timestamp used, or -1 when nothing could be written
// (no frames, unsupported format, short payload, imwrite failure).
int64_t writeThumbnail(const SwingWindow& window, SourceId sid,
                       int64_t impactUs, const QString& thumbPath)
{
    const auto entries = window.entriesFor(sid);
    if (entries.empty())
        return -1;

    const FormatDescriptor& fd = window.formatOf(sid);
    const auto* cfmt = std::get_if<CameraFormat>(&fd.format);
    if (!cfmt)
        return -1;

    // Entry nearest impact — clamps naturally to the captured range when a
    // backdated impact timestamp falls outside it.
    const IndexEntry* best = &entries.front();
    for (const IndexEntry& e : entries) {
        if (std::llabs(e.timestamp_us - impactUs) < std::llabs(best->timestamp_us - impactUs))
            best = &e;
    }

    // Shared zero-copy demosaic (frame_decode) — false on unsupported formats
    // and absent/short payloads alike.
    const SourceRing::ReadHandle handle = window.payloadOf(*best);
    cv::Mat bgr;
    if (!decodeToBgr(*cfmt, handle.data, handle.bytes, bgr))
        return -1;

    // Downscale for cheap carousel I/O; never upscale.
    constexpr int kMaxThumbWidth = 480;
    cv::Mat thumb;
    if (bgr.cols > kMaxThumbWidth) {
        const double scale = double(kMaxThumbWidth) / bgr.cols;
        cv::resize(bgr, thumb, cv::Size(), scale, scale, cv::INTER_AREA);
    } else {
        thumb = bgr;
    }

    // Save via Qt, NOT cv::imwrite: OpenCV's imgcodecs resolves libjpeg symbols
    // against whichever libjpeg generation loaded first in this multi-FFmpeg
    // process, and a mismatched jpeg_error_mgr layout turns its error-handler
    // longjmp into a crash. QImage uses Qt's own (self-consistent) JPEG handler.
    const QImage img(thumb.data, thumb.cols, thumb.rows,
                     static_cast<qsizetype>(thumb.step), QImage::Format_BGR888);
    if (!img.save(thumbPath, "JPG", 85)) {
        ppWarn() << "[SwingExport] failed to write thumbnail" << thumbPath;
        return -1;
    }
    return best->timestamp_us;
}

} // namespace

SwingExportResult SwingExporter::run(const SwingWindow& window, const SwingExportJob& job)
{
    SwingExportResult result;
    result.swingDir = job.swingDir;

    const int64_t t0 = window.startTimestampUs();

    struct VideoStreamRecord {
        const SwingExportCamera* cam = nullptr;
        const CameraFormat*      fmt = nullptr;
        QString serial;
        const char* demosaicTag = "none";
        int     outW = 0;          // encoded dimensions (post-downscale)
        int     outH = 0;
        QString rawFile;           // "<alias>.raw" sidecar; empty when not saved
        size_t  rawStride = 0;     // plane-0 stride of the raw payload
        size_t  rawFrameBytes = 0; // bytes per raw frame written
        std::vector<int64_t> tUs;
    };
    std::vector<VideoStreamRecord> videoStreams;

    // ── Per-camera encode (sequential — lowest peak RAM; per-camera
    //    parallelism is a possible future option) ──────────────────────────
    cv::Mat bgr;      // single reused BGR scratch across all cameras
    cv::Mat scaled;   // reused downscale scratch (only used when resizing)
    for (const SwingExportCamera& cam : job.cameras) {
        const auto entries = window.entriesFor(cam.sourceId);
        if (entries.empty()) {
            ppWarn() << "[SwingExport] no frames for camera" << cam.alias << "— skipping";
            continue;
        }

        const FormatDescriptor& fd = window.formatOf(cam.sourceId);
        const auto* cfmt = std::get_if<CameraFormat>(&fd.format);
        if (!cfmt) {
            ppWarn() << "[SwingExport] source" << cam.sourceId << "is not a camera — skipping";
            continue;
        }

        const DemosaicPlan plan = demosaicPlanFor(cfmt->pixel_format);
        if (!plan.supported) {
            ppWarn() << "[SwingExport] unsupported pixel format"
                     << pixelFormatName(cfmt->pixel_format)
                     << "for camera" << cam.alias << "— skipping";
            continue;
        }

        const int srcW  = static_cast<int>(cfmt->width);
        const int srcH  = static_cast<int>(cfmt->height);
        const int cropW = srcW & ~1;   // even crop of the source (no padding —
        const int cropH = srcH & ~1;   // libx264/yuv420p needs even dims)
        if (cropW <= 0 || cropH <= 0) {
            ppWarn() << "[SwingExport] degenerate dimensions for" << cam.alias << "— skipping";
            continue;
        }
        const cv::Rect cropRect(0, 0, cropW, cropH);

        // Raw-payload geometry (shared with the per-frame decode below). Cannot
        // fail here — the format and dimensions were vetted above.
        size_t stride = 0, minBytes = 0;
        if (!frameGeometry(*cfmt, stride, minBytes)) {
            ppWarn() << "[SwingExport] degenerate dimensions for" << cam.alias << "— skipping";
            continue;
        }

        // Honor videoResolutionMode as an export-time downscale (never upscale).
        int outW = cropW, outH = cropH;
        exportTargetSize(srcW, srcH, job.resolutionMode, outW, outH);
        if (outW <= 0 || outH <= 0) { outW = cropW; outH = cropH; }
        const bool needResize = (outW != cropW) || (outH != cropH);

        auto encoder = makeVideoEncoder(job.codec.toStdString());
        if (!encoder) {
            result.error = QStringLiteral("no '%1' encoder available (built without FFmpeg?)")
                               .arg(job.codec);
            return result;
        }

        VideoEncoderConfig cfg;
        cfg.width   = outW;
        cfg.height  = outH;
        cfg.out_fps = 30;
        cfg.crf     = job.crf;
        cfg.preset  = "medium";
        cfg.path    = QString(job.swingDir + QLatin1Char('/') + cam.fileName).toStdString();
        if (!encoder->open(cfg)) {
            result.error = QStringLiteral("failed to open encoder for %1").arg(cam.fileName);
            return result;
        }

        VideoStreamRecord rec;
        rec.cam         = &cam;
        rec.fmt         = cfmt;
        rec.serial      = QString::fromStdString(fd.device_serial);
        rec.demosaicTag = plan.tag;
        rec.outW        = outW;
        rec.outH        = outH;
        rec.tUs.reserve(entries.size());

        // Optional raw-payload sidecar (saveRawFrames): the undecoded sensor
        // bytes for every encoded frame, concatenated into one "<alias>.raw".
        QFile rawSink;
        if (job.saveRaw) {
            const QString rawName = cam.alias + QStringLiteral(".raw");
            rawSink.setFileName(job.swingDir + QLatin1Char('/') + rawName);
            if (rawSink.open(QIODevice::WriteOnly)) {
                rec.rawFile       = rawName;
                rec.rawStride     = stride;
                rec.rawFrameBytes = minBytes;
            } else {
                ppWarn() << "[SwingExport] could not open raw sidecar" << rawName
                         << "— continuing without raw frames";
            }
        }

        for (const IndexEntry& e : entries) {
            const SourceRing::ReadHandle handle = window.payloadOf(e);
            // Skip absent/short payloads entirely so MP4 frame i == t_us[i].
            if (!handle.data || handle.bytes < minBytes)
                continue;

            // Shared zero-copy demosaic (frame_decode) of the frozen ring
            // payload (stable while Paused). Infallible after the guards above.
            if (!decodeToBgr(*cfmt, handle.data, handle.bytes, bgr))
                continue;

            // Raw sidecar mirrors the encoded frame set exactly (same guard), so
            // raw frame i corresponds to encoded frame i and t_us[i]. A short
            // write (disk full) would desync that mapping — drop the sidecar
            // instead of recording a truncated file as complete.
            if (rawSink.isOpen()
                && rawSink.write(reinterpret_cast<const char*>(handle.data),
                                 static_cast<qint64>(minBytes))
                       != static_cast<qint64>(minBytes)) {
                ppWarn() << "[SwingExport] short write on raw sidecar" << rec.rawFile
                         << "— dropping raw frames (disk full?)";
                rawSink.close();
                rawSink.remove();
                rec.rawFile.clear();
            }

            // TODO(restorer): frame restoration hook

            const cv::Mat cropped = bgr(cropRect);   // header only — no copy
            bool encOk;
            if (needResize) {
                cv::resize(cropped, scaled, cv::Size(outW, outH), 0, 0, cv::INTER_AREA);
                encOk = encoder->writeBgr(scaled);
            } else {
                encOk = encoder->writeBgr(cropped);
            }
            if (!encOk) {
                result.error = QStringLiteral("encode failed for %1").arg(cam.fileName);
                return result;
            }
            rec.tUs.push_back(e.timestamp_us - t0);
        }
        if (rawSink.isOpen())
            rawSink.close();

        // Every payload skipped (size/descriptor mismatch) would otherwise
        // "succeed" with an empty MP4 — fail loudly instead so the UI surfaces
        // it (the toast on swingSaveFailed) rather than a blank swing.
        if (rec.tUs.empty()) {
            result.error = QStringLiteral("0 frames written for %1 — all %2 buffered "
                                          "payloads skipped (payload/descriptor mismatch?)")
                               .arg(cam.fileName).arg(entries.size());
            return result;
        }

        if (!encoder->finish()) {
            result.error = QStringLiteral("failed to finalise %1").arg(cam.fileName);
            return result;
        }

        ppInfo() << "[SwingExport]" << cam.fileName << ":" << rec.tUs.size() << "frames";
        videoStreams.push_back(std::move(rec));
    }

    // Every requested camera was skipped (no frames / unsupported format /
    // degenerate dims) — fail loudly instead of returning a vacuous success.
    // ok=true with zero streams would mark the shot hasVideo=true and offer a
    // replay that can never start.
    if (videoStreams.empty()) {
        result.error = QStringLiteral("no exportable video streams — all %1 camera(s) "
                                      "skipped (no frames or unsupported pixel format)")
                           .arg(job.cameras.size());
        return result;
    }

    // ── Impact thumbnail ──────────────────────────────────────────────────
    // Prefer the designated camera (face-on); fall back to the other exported
    // streams in order. A missing thumbnail never fails the export.
    int64_t thumbTsUs = -1;
    if (job.thumbnailTimestampUs >= 0) {
        std::vector<SourceId> tryOrder;
        if (job.thumbnailSourceId != kInvalidSourceId)
            tryOrder.push_back(job.thumbnailSourceId);
        for (const VideoStreamRecord& rec : videoStreams)
            if (std::find(tryOrder.begin(), tryOrder.end(), rec.cam->sourceId) == tryOrder.end())
                tryOrder.push_back(rec.cam->sourceId);

        const QString thumbPath = job.swingDir + QStringLiteral("/thumb.jpg");
        for (SourceId sid : tryOrder) {
            thumbTsUs = writeThumbnail(window, sid, job.thumbnailTimestampUs, thumbPath);
            if (thumbTsUs >= 0) {
                result.thumbnailPath = thumbPath;
                break;
            }
        }
        if (result.thumbnailPath.isEmpty())
            ppWarn() << "[SwingExport] no thumbnail written for" << job.swingId;
    }

    // ── swing.json ────────────────────────────────────────────────────────
    QJsonArray streams;

    for (const VideoStreamRecord& rec : videoStreams) {
        QJsonObject s;
        s[QStringLiteral("kind")]  = QStringLiteral("video");
        s[QStringLiteral("alias")] = rec.cam->alias;
        s[QStringLiteral("file")]  = rec.cam->fileName;
        s[QStringLiteral("source")] = QJsonObject{
            {QStringLiteral("serial"),      rec.serial},
            {QStringLiteral("pixelFormat"), pixelFormatName(rec.fmt->pixel_format)},
            {QStringLiteral("width"),       static_cast<int>(rec.fmt->width)},
            {QStringLiteral("height"),      static_cast<int>(rec.fmt->height)},
        };
        // Encoded (post-downscale) dimensions — equal to source when native.
        s[QStringLiteral("encoded")] = QJsonObject{
            {QStringLiteral("width"),  rec.outW},
            {QStringLiteral("height"), rec.outH},
        };
        // Raw sidecar (saveRawFrames) — enough metadata to reconstruct the
        // undecoded payloads: one frame is `frameBytes` long at `stride`, same
        // pixelFormat/dimensions as the source, `count` frames matching t_us.
        if (!rec.rawFile.isEmpty()) {
            s[QStringLiteral("raw")] = QJsonObject{
                {QStringLiteral("file"),        rec.rawFile},
                {QStringLiteral("pixelFormat"), pixelFormatName(rec.fmt->pixel_format)},
                {QStringLiteral("width"),       static_cast<int>(rec.fmt->width)},
                {QStringLiteral("height"),      static_cast<int>(rec.fmt->height)},
                {QStringLiteral("stride"),      static_cast<qint64>(rec.rawStride)},
                {QStringLiteral("frameBytes"),  static_cast<qint64>(rec.rawFrameBytes)},
                {QStringLiteral("count"),       static_cast<qint64>(rec.tUs.size())},
            };
        }
        QJsonObject captureObj{
            {QStringLiteral("fps_num"), static_cast<int>(rec.fmt->fps_numerator)},
            {QStringLiteral("fps_den"), static_cast<int>(rec.fmt->fps_denominator)},
        };
        // Exposure (additive) — written only when known so legacy streams stay
        // clean. exposureUs feeds the shaft detector's blur/wedge assessment;
        // exposureSource records chunk-measured vs fps-derived provenance;
        // exposureAuto (only for measured streams) flags auto-exposure drift.
        if (rec.fmt->exposure_source != ExposureSource::Unknown && rec.fmt->exposure_us > 0.0) {
            captureObj[QStringLiteral("exposureUs")]     = rec.fmt->exposure_us;
            captureObj[QStringLiteral("exposureSource")] =
                QString::fromLatin1(exposureSourceName(rec.fmt->exposure_source));
            if (rec.fmt->exposure_source == ExposureSource::Measured
                || rec.fmt->exposure_source == ExposureSource::MeasuredAuto)
                captureObj[QStringLiteral("exposureAuto")] =
                    (rec.fmt->exposure_source == ExposureSource::MeasuredAuto);
        }
        s[QStringLiteral("capture")] = captureObj;
        // Camera setup at capture time (additive). perspectiveName saves
        // readers a magic-number table; fixedInPlace is the camera-side
        // "calibrated" signal SwingLab filters on.
        static const char* kPerspectiveNames[] = {"None", "DownTheLine", "FaceOn", "Other"};
        const int pi = (rec.cam->perspective >= 0 && rec.cam->perspective <= 3)
                           ? rec.cam->perspective : 0;
        QJsonObject ballDetection{
            {QStringLiteral("calibrated"),     rec.cam->ballCalibrated},
            {QStringLiteral("margin"),         rec.cam->ballMargin},
            {QStringLiteral("driftAtCapture"), rec.cam->ballDriftAtCapture},
        };
        ballDetection[QStringLiteral("calibratedAt")] = rec.cam->ballCalibratedAtMs > 0
            ? QJsonValue(QDateTime::fromMSecsSinceEpoch(rec.cam->ballCalibratedAtMs, QTimeZone::UTC)
                             .toString(Qt::ISODate))
            : QJsonValue();
        // Additive: stable ball position + scale (full-frame normalized), co-
        // registered with analysis.club head samples. Omitted when unavailable
        // so older/uncalibrated swings simply carry no position (see
        // docs/design/low_point_metric_design.md).
        if (rec.cam->ballHasPos) {
            ballDetection[QStringLiteral("center")] = QJsonArray{
                rec.cam->ballCenterX, rec.cam->ballCenterY };
            ballDetection[QStringLiteral("radiusNorm")]     = rec.cam->ballRadiusNorm;
            ballDetection[QStringLiteral("positionSource")] = rec.cam->ballPosSource;
        }
        // Hitting-area search box (full-frame normalized) — offline re-analysis
        // uses it so ball detection searches the same region the live detector
        // did, excluding out-of-box distractors (feet/shoes). Omitted when unset.
        if (!rec.cam->ballSearchRoi.isEmpty()) {
            const QRectF &r = rec.cam->ballSearchRoi;
            ballDetection[QStringLiteral("searchRoi")] = QJsonArray{
                r.x(), r.y(), r.width(), r.height() };
        }
        // Learned empty-mat baseline sidecar — a raw row-major float32 blob,
        // not an image (the QImage-not-cv::imwrite rule is moot here).
        // Written only when CameraInstance actually cached
        // one live; blob empty ⇒ nothing was learned (legacy swing, or a shot
        // fired before the first seed completed) — omit sidecar + JSON key.
        // Short-write handling mirrors the raw sidecar above: drop the file and
        // the key rather than record a broken reference.
        if (!rec.cam->ballBaselineBlob.isEmpty() && rec.cam->ballBaselineW > 0
            && rec.cam->ballBaselineH > 0 && !rec.cam->ballBaselineRoi.isEmpty()) {
            const QString blobName = rec.cam->alias + QStringLiteral(".ballbase.f32");
            QFile bf(job.swingDir + QLatin1Char('/') + blobName);
            if (bf.open(QIODevice::WriteOnly)) {
                const qint64 want = static_cast<qint64>(rec.cam->ballBaselineBlob.size());
                const bool ok = (bf.write(rec.cam->ballBaselineBlob) == want);
                bf.close();
                if (ok) {
                    const QRectF &br = rec.cam->ballBaselineRoi;
                    ballDetection[QStringLiteral("baseline")] = QJsonObject{
                        {QStringLiteral("file"),   blobName},
                        {QStringLiteral("w"),      rec.cam->ballBaselineW},
                        {QStringLiteral("h"),      rec.cam->ballBaselineH},
                        {QStringLiteral("roi"),    QJsonArray{br.x(), br.y(), br.width(), br.height()}},
                        {QStringLiteral("rHat"),   rec.cam->ballBaselineRHat},
                        {QStringLiteral("fps"),    rec.cam->ballBaselineFps},
                        {QStringLiteral("noise0"), rec.cam->ballBaselineNoise0},
                    };
                } else {
                    ppWarn() << "[SwingExport] short write on baseline sidecar" << blobName
                             << "— dropping (disk full?)";
                    bf.remove();
                }
            } else {
                ppWarn() << "[SwingExport] could not open baseline sidecar" << blobName
                         << "— continuing without a learned baseline";
            }
        }
        s[QStringLiteral("setup")] = QJsonObject{
            {QStringLiteral("perspective"),     rec.cam->perspective},
            {QStringLiteral("perspectiveName"), QString::fromLatin1(kPerspectiveNames[pi])},
            {QStringLiteral("mirrored"),        rec.cam->mirrored},
            {QStringLiteral("fixedInPlace"),    rec.cam->fixedInPlace},
            {QStringLiteral("ballDetection"),   ballDetection},
        };
        s[QStringLiteral("playback")] = QJsonObject{{QStringLiteral("fps"), 30}};
        s[QStringLiteral("processing")] = QJsonObject{
            {QStringLiteral("demosaic"), QString::fromLatin1(rec.demosaicTag)},
            {QStringLiteral("restorer"), QStringLiteral("none")},
        };
        s[QStringLiteral("frames")] = QJsonObject{
            {QStringLiteral("count"), static_cast<qint64>(rec.tUs.size())},
            {QStringLiteral("t_us"),  toJsonTimestamps(rec.tUs)},
        };
        streams.append(s);
    }

    if (job.saveImu) {
        // Discover IMU sources from the window itself (distinct source ids with
        // an ImuFormat descriptor) — the job only carries alias lookups.
        std::vector<SourceId> imuIds;
        for (const IndexEntry& e : window.entries()) {
            if (std::find(imuIds.begin(), imuIds.end(), e.source_id) != imuIds.end())
                continue;
            if (std::holds_alternative<ImuFormat>(window.formatOf(e.source_id).format))
                imuIds.push_back(e.source_id);
        }

        // imuDataFormat: "json" inlines samples in swing.json; "csv"/"binary"
        // write an "imu_<alias>.<ext>" sidecar and reference it instead.
        const bool imuCsv = job.imuFormat == QLatin1String("csv");
        const bool imuBin = job.imuFormat == QLatin1String("binary");

        for (SourceId sid : imuIds) {
            const FormatDescriptor& fd = window.formatOf(sid);
            const QString serial = QString::fromStdString(fd.device_serial);
            const QString alias  = job.imuAliasBySerial.value(serial, serial);
            // HackMotion lanes need honest units/instrument tagging below — the
            // accel channel is a different physical quantity from a Witmotion
            // lane's and nothing downstream may compare or pool the two. Every
            // use of this pointer is inside a branch that runs ONLY for this
            // device kind, so a Witmotion export is byte-identical to before.
            const ImuFormat* imf = std::get_if<ImuFormat>(&fd.format);
            const bool isHackMotion = imf && imf->device == DeviceKind::IMU_HackMotion;

            std::vector<int64_t> tUs;
            std::vector<std::array<float, 10>> rows;   // accel3, gyro3, quat4(wxyz)
            for (const IndexEntry& e : window.entriesFor(sid)) {
                const SourceRing::ReadHandle handle = window.payloadOf(e);
                if (!handle.data || handle.bytes < sizeof(ImuSample))
                    continue;
                ImuSample sample;                                    // alignment-safe
                std::memcpy(&sample, handle.data, sizeof(ImuSample));
                rows.push_back({sample.accel_x, sample.accel_y, sample.accel_z,
                                sample.gyro_x,  sample.gyro_y,  sample.gyro_z,
                                sample.quat_w,  sample.quat_x,
                                sample.quat_y,  sample.quat_z});
                tUs.push_back(e.timestamp_us - t0);
            }
            if (tUs.empty())
                continue;

            QJsonObject s;
            s[QStringLiteral("kind")]   = QStringLiteral("imu");
            s[QStringLiteral("alias")]  = alias;
            // "imu_sample_v2" names the 40-byte record LAYOUT (imu_sample.h),
            // which a HackMotion lane shares byte-for-byte with a Witmotion
            // one — do not invent a second schema string for it. The semantic
            // difference (units, world->body quat) travels in "instrument"
            // and "units" below instead.
            s[QStringLiteral("schema")] = QStringLiteral("imu_sample_v2");
            s[QStringLiteral("source")] = QJsonObject{{QStringLiteral("serial"), serial}};
            // Device configuration at capture time (additive) — outputRateHz
            // replaces SwingLab's hardcoded 200 Hz assumption.
            if (job.imuDeviceBySerial.contains(serial)) {
                const SwingImuDeviceInfo& dev = job.imuDeviceBySerial[serial];
                QJsonObject deviceObj{
                    {QStringLiteral("outputRateHz"),      dev.outputRateHz},
                    {QStringLiteral("fusionMode"),        dev.fusionMode},
                    {QStringLiteral("orientationFilter"), dev.orientationFilter},
                    {QStringLiteral("placementSlot"),     dev.placementSlot},
                    {QStringLiteral("role"),              dev.role},
                    {QStringLiteral("roleName"),          dev.roleName},
                };
                // A/M calibration snapshot — present for every capture so an
                // analysis-skipped corpus swing remains re-analysable. Same
                // [scalar,x,y,z] quaternion order as analysis.bindings.
                if (dev.hasCalibration) {
                    auto quatJson = [](const QQuaternion& q) {
                        return QJsonArray{ q.scalar(), q.x(), q.y(), q.z() };
                    };
                    deviceObj[QStringLiteral("alignA")]               = quatJson(dev.alignA);
                    deviceObj[QStringLiteral("mountM")]               = quatJson(dev.mountM);
                    deviceObj[QStringLiteral("calibrated")]           = dev.calibrated;
                    deviceObj[QStringLiteral("anatCalibrated")]       = dev.anatCalibrated;
                    deviceObj[QStringLiteral("mountDeviationDeg")]    = dev.mountDeviationDeg;
                    deviceObj[QStringLiteral("mountGravityErrorDeg")] = dev.mountGravityErrorDeg;
                    deviceObj[QStringLiteral("calibAgeSec")]          = dev.calibAgeSec;
                    if (!dev.calibratedAtUtc.isEmpty())
                        deviceObj[QStringLiteral("calibratedAt")]     = dev.calibratedAtUtc;
                }
                // HackMotion only: the two units' blocks are NOT paired as
                // simultaneous (see SwingImuDeviceInfo::skewUs) — recorded
                // rather than silently applied because whether it is real
                // sampling skew or arbitrary counter phase is unresolved.
                // isHackMotion gates this so a Witmotion export — whose
                // skewUs is always the untouched 0.0 default — cannot gain
                // this key by a future accident elsewhere.
                if (isHackMotion && dev.skewUs != 0.0 && std::isfinite(dev.skewUs))
                    deviceObj[QStringLiteral("skewUs")] = dev.skewUs;
                s[QStringLiteral("device")] = deviceObj;
            }
            if (isHackMotion) {
                // Honest units for a HackMotion lane, plus a machine-checkable
                // discriminator (Phase F is the consumer) instead of making a
                // reader parse prose out of the unit strings:
                //  - accel is gravity-removed LINEAR acceleration in g (reads
                //    ~0 at rest) — a different physical quantity from a
                //    Witmotion lane's accel channel; nothing downstream may
                //    compare or pool the two ("g (linear, gravity-removed —
                //    NOT comparable to a Witmotion accel channel)").
                //  - quat maps world->body (§6.7), the CONJUGATE of what the
                //    rest of the pipeline assumes; unreconciled until Phase D.
                s[QStringLiteral("instrument")] = QStringLiteral("hackmotion");
                s[QStringLiteral("units")] = QJsonObject{
                    {QStringLiteral("accel"),
                     QStringLiteral("g (linear, gravity-removed — NOT comparable to a Witmotion accel channel)")},
                    {QStringLiteral("gyro"), QStringLiteral("deg/s")},
                    {QStringLiteral("quat"),
                     QStringLiteral("wxyz, world->body (conjugate of the pipeline's convention — unreconciled, Phase D)")},
                };
            } else {
                s[QStringLiteral("units")]  = QJsonObject{
                    {QStringLiteral("accel"), QStringLiteral("g")},
                    {QStringLiteral("gyro"),  QStringLiteral("deg/s")},
                    {QStringLiteral("quat"),  QStringLiteral("wxyz")},
                };
            }

            // Inline-JSON samples object (also the fallback if a sidecar fails).
            auto inlineSamples = [&]() -> QJsonObject {
                QJsonArray data;
                for (const auto& r : rows)
                    data.append(QJsonArray{r[0], r[1], r[2], r[3], r[4],
                                           r[5], r[6], r[7], r[8], r[9]});
                return QJsonObject{
                    {QStringLiteral("count"), static_cast<qint64>(tUs.size())},
                    {QStringLiteral("t_us"),  toJsonTimestamps(tUs)},
                    {QStringLiteral("data"),  data},
                };
            };

            if (imuCsv || imuBin) {
                const QString fileName = QStringLiteral("imu_") + SwingPaths::sanitise(alias)
                                       + (imuCsv ? QStringLiteral(".csv") : QStringLiteral(".bin"));
                const QString path = job.swingDir + QLatin1Char('/') + fileName;
                bool wrote = false;
                QFile f(path);
                if (imuCsv) {
                    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        QTextStream out(&f);
                        out << "t_us,accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z,"
                               "quat_w,quat_x,quat_y,quat_z\n";
                        for (size_t i = 0; i < tUs.size(); ++i) {
                            out << static_cast<qint64>(tUs[i]);
                            for (float v : rows[i]) out << ',' << v;
                            out << '\n';
                        }
                        out.flush();
                        // Disk-full/short-write check: a truncated sidecar
                        // referenced as complete by swing.json is worse than
                        // the inline-JSON fallback.
                        wrote = (out.status() == QTextStream::Ok
                                 && f.error() == QFileDevice::NoError);
                    }
                } else {   // binary: little-endian i64 t_us + 10×f32 per record
                    if (f.open(QIODevice::WriteOnly)) {
                        wrote = true;
                        for (size_t i = 0; i < tUs.size() && wrote; ++i) {
                            const qint64 t = static_cast<qint64>(tUs[i]);
                            const qint64 rowBytes =
                                static_cast<qint64>(sizeof(float) * rows[i].size());
                            wrote = f.write(reinterpret_cast<const char*>(&t),
                                            sizeof(t)) == sizeof(t)
                                 && f.write(reinterpret_cast<const char*>(rows[i].data()),
                                            rowBytes) == rowBytes;
                        }
                    }
                }
                if (wrote) {
                    s[QStringLiteral("samples")] = QJsonObject{
                        {QStringLiteral("count"),  static_cast<qint64>(tUs.size())},
                        {QStringLiteral("file"),   fileName},
                        {QStringLiteral("format"), job.imuFormat},
                        {QStringLiteral("record"), imuBin
                             ? QStringLiteral("le: i64 t_us, f32[10] accel3,gyro3,quat4")
                             : QStringLiteral("t_us,accel3,gyro3,quat4")},
                    };
                } else {
                    ppWarn() << "[SwingExport] could not write IMU sidecar" << path
                             << "— falling back to inline JSON";
                    s[QStringLiteral("samples")] = inlineSamples();
                }
            } else {
                s[QStringLiteral("samples")] = inlineSamples();
            }
            streams.append(s);
        }
    }

    // ── Pose streams ────────────────────────────────────────────────────────
    // Serialise pre-computed 2D pose IF the job carries any. The exporter never
    // runs pose estimation itself — `poseStreams` is populated upstream (by a
    // future pose producer) and is empty today, so this normally writes nothing.
    if (job.savePose) {
        constexpr int kKpStride = 51;   // 17 COCO keypoints × (y, x, score)
        for (const SwingPoseStream& p : job.poseStreams) {
            if (p.tUs.empty())
                continue;
            QJsonArray data;
            for (size_t f = 0; f < p.tUs.size(); ++f) {
                QJsonArray frame;
                const size_t base = f * static_cast<size_t>(kKpStride);
                for (int k = 0; k < kKpStride && base + k < p.keypoints.size(); ++k)
                    frame.append(p.keypoints[base + k]);
                data.append(frame);
            }
            QJsonObject s;
            s[QStringLiteral("kind")]   = QStringLiteral("pose");
            s[QStringLiteral("alias")]  = p.alias;
            s[QStringLiteral("schema")] = QStringLiteral("pose_movenet_v1");
            s[QStringLiteral("source")] = QJsonObject{{QStringLiteral("serial"), p.serial}};
            s[QStringLiteral("layout")] = QStringLiteral("coco17:y,x,score");
            s[QStringLiteral("frames")] = QJsonObject{
                {QStringLiteral("count"), static_cast<qint64>(p.tUs.size())},
                {QStringLiteral("t_us"),  toJsonTimestamps(p.tUs)},
                {QStringLiteral("data"),  data},
            };
            streams.append(s);
        }
    }

    // ── Ball stream(s) (v3.4 design §9.7) ──────────────────────────────────
    // Additive — readers ignore unknown "kind" values by contract, same as
    // the pose stream above. Empty when ball detection was off/unenabled for
    // every exported camera.
    {
        constexpr int kBallStride = 5;   // found, x, y, r, conf
        for (const SwingBallStream& b : job.ballStreams) {
            if (b.tUs.empty())
                continue;
            QJsonArray data;
            for (size_t f = 0; f < b.tUs.size(); ++f) {
                QJsonArray frame;
                const size_t base = f * static_cast<size_t>(kBallStride);
                for (int k = 0; k < kBallStride && base + k < b.data.size(); ++k)
                    frame.append(b.data[base + k]);
                data.append(frame);
            }
            QJsonObject s;
            s[QStringLiteral("kind")]   = QStringLiteral("ball");
            s[QStringLiteral("alias")]  = b.alias;
            s[QStringLiteral("schema")] = QStringLiteral("ball_v2");
            s[QStringLiteral("source")] = QJsonObject{{QStringLiteral("serial"), b.serial}};
            s[QStringLiteral("layout")] = QStringLiteral("found,x,y,r,conf");
            s[QStringLiteral("frames")] = QJsonObject{
                {QStringLiteral("count"), static_cast<qint64>(b.tUs.size())},
                {QStringLiteral("t_us"),  toJsonTimestamps(b.tUs)},
                {QStringLiteral("data"),  data},
            };
            if (b.launchTUs >= 0) {
                s[QStringLiteral("launch")] = QJsonObject{
                    {QStringLiteral("t_us"), static_cast<qint64>(b.launchTUs)},
                    {QStringLiteral("x"),    b.launchX},
                    {QStringLiteral("y"),    b.launchY},
                };
            }
            streams.append(s);
        }
    }

    // Wallclock: the anchor was snapshotted when the window was captured, i.e.
    // at monotonic ~endTimestampUs(); subtracting the window duration gives an
    // honest (ms-accurate) wallclock for t0.
    const int64_t durationUs = window.endTimestampUs() - t0;
    const QDateTime wallclock = job.wallclockAnchorUtc.addMSecs(-durationUs / 1000);

    QJsonObject root;
    root[QStringLiteral("schema")] = QStringLiteral("pinpoint.swing/1");
    root[QStringLiteral("swing")] = QJsonObject{
        {QStringLiteral("index"), job.swingIndex},
        {QStringLiteral("id"),    job.swingId},
    };
    root[QStringLiteral("athlete")] = QJsonObject{
        {QStringLiteral("name"),       job.athleteName},
        {QStringLiteral("uuid"),       job.athleteUuid},
        {QStringLiteral("handedness"), job.handedness},
    };
    root[QStringLiteral("session")] = QJsonObject{{QStringLiteral("dir"), job.sessionId}};
    root[QStringLiteral("clock")] = QJsonObject{
        {QStringLiteral("t0_us"),     static_cast<qint64>(t0)},
        {QStringLiteral("unit"),      QStringLiteral("us")},
        {QStringLiteral("wallclock"), wallclock.toString(Qt::ISODateWithMs)},
    };
    root[QStringLiteral("window")] = QJsonObject{
        {QStringLiteral("start_us"), 0},
        {QStringLiteral("end_us"),   static_cast<qint64>(durationUs)},
    };
    if (!result.thumbnailPath.isEmpty()) {
        // Additive — readers ignore unknown keys by contract.
        root[QStringLiteral("thumbnail")] = QJsonObject{
            {QStringLiteral("file"), QStringLiteral("thumb.jpg")},
            {QStringLiteral("t_us"), static_cast<qint64>(thumbTsUs - t0)},
        };
    }
    root[QStringLiteral("capture")] = SwingExporter::captureBlock(job);
    root[QStringLiteral("streams")] = streams;

    // swing.json is NOT written here. The worker returns its raw manifest and the GUI
    // thread writes ONE unified swing.json (raw + the analyzer's "analysis" block) at the
    // join — ShotProcessor::maybeJoin() -> SwingDocWriter::writeSwingJson(). The media
    // (MP4s + thumb.jpg) above are still written on this worker.
    result.manifest = root;
    result.ok       = true;
    return result;
}

QJsonObject SwingExporter::captureBlock(const SwingExportJob& job)
{
    QJsonObject cap{
        {QStringLiteral("sessionType"), job.sessionType},
        {QStringLiteral("shotSource"),  job.shotSource},
        // Window-relative impact (-1 = unknown) — the re-analysis impact reference,
        // present even for analysis-skipped corpus swings (no analysis.phases block).
        {QStringLiteral("impactUs"),    static_cast<qint64>(job.impactUs)},
        {QStringLiteral("swingDetectionSensitivity"), job.swingDetectionSensitivity},
        {QStringLiteral("motionCaptureQuality"),      job.motionCaptureQuality},
        {QStringLiteral("latencyUs"), QJsonObject{
            {QStringLiteral("imuBle"),      static_cast<qint64>(job.imuBleLatencyUs)},
            {QStringLiteral("audioDevice"), job.audioDeviceLatencyUs},
            // Presence marks a travel-corrected anchor (see SwingExportJob).
            {QStringLiteral("micTravel"),   static_cast<qint64>(job.micTravelUs)},
        }},
        {QStringLiteral("host"), QJsonObject{
            {QStringLiteral("app"),         QStringLiteral("PinPointStudio")},
            {QStringLiteral("version"),     job.host.appVersion},
            {QStringLiteral("gitSha"),      job.host.gitSha},
            {QStringLiteral("hostname"),    job.host.hostname},
            {QStringLiteral("platform"),    job.host.platform},
            {QStringLiteral("poseBackend"), job.host.poseBackend},
        }},
    };
    // Club geometry (shaft-tracker E1 band matcher) — persisted so re-analysis
    // recovers it. Omitted when unresolved (untaped/no active club). clubName
    // alone is enough to write the block: an untaped club with no recorded
    // geometry still carries the length-prior identity (club_length_fusion.h).
    if (job.clubLengthM > 0.0 || !job.bandCentersMm.empty() || !job.clubName.isEmpty()) {
        QJsonArray bands;
        for (double b : job.bandCentersMm) bands.append(b);
        QJsonObject club{
            {QStringLiteral("lengthMm"),        job.clubLengthM * 1000.0},
            {QStringLiteral("shaftType"),       job.shaftType},
            {QStringLiteral("bandCentersMm"),   bands},
            // Additive (Phase A5): hosel offset from the butt, mm. 0 = unknown —
            // absent on swings captured before this field existed.
            {QStringLiteral("hoselFromButtMm"), job.hoselFromButtMm},
            // Additive (club-length fusion): canonical club-vocabulary id — half
            // the persistent prior key (athleteUuid|clubName|cameraKey).
            {QStringLiteral("name"),            job.clubName},
        };
        // The prior the live analysis fuse ACTUALLY used for this shot (state
        // BEFORE this shot's update) — re-analysis replays it for a byte-exact
        // fuse instead of reading AppSettings (deterministic, cross-host).
        // Omitted when no prior joined (first shot / cold key / camera unfixed).
        if (job.priorClubLenN > 0 && job.priorClubLenPx > 0.0)
            club[QStringLiteral("lengthPrior")] = QJsonObject{
                {QStringLiteral("px"),    job.priorClubLenPx},
                {QStringLiteral("varPx"), job.priorClubLenVarPx},
                {QStringLiteral("n"),     job.priorClubLenN},
            };
        cap[QStringLiteral("club")] = club;
    }
    return cap;
}

} // namespace pinpoint
