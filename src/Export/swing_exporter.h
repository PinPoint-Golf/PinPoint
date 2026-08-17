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

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QQuaternion>
#include <QRectF>
#include <QString>
#include <vector>

#include "types.h"

namespace pinpoint {

class SwingWindow;

// One camera to export.  alias/fileName are resolved (sanitised, deduped) on
// the UI thread before the job is handed to the worker.
struct SwingExportCamera {
    SourceId sourceId = kInvalidSourceId;
    QString  alias;        // human label recorded in swing.json
    QString  fileName;     // "<alias>.mp4"
    // Camera setup at capture time (stream "setup" object): CameraInstance
    // perspective enum (None 0, DownTheLine 1, FaceOn 2, Other 3), mirroring,
    // and the AppSettings fixed-in-place flag (the camera-side "calibrated").
    int      perspective  = 0;
    bool     mirrored     = false;
    bool     fixedInPlace = false;
    // Ball-detection provenance (ball_detection_calibration.md §7): whether
    // the environment-calibrated detector was active on this stream, its
    // validation margin / timestamp, and the drift severity at capture.
    bool     ballCalibrated     = false;
    double   ballMargin         = 0.0;
    qint64   ballCalibratedAtMs = 0;     // epoch ms, 0 = n/a
    double   ballDriftAtCapture = 0.0;
    // Calibrated ball position + scale in FULL-FRAME normalized coords, co-
    // registered with the shaft-track head samples — the stable address-ball
    // reference for the deferred low-point-ahead-of-ball metric. radiusNorm is
    // normalized to frame width (px scale = kBallDiameterMm / (2·radiusNorm·W)).
    // hasPos=false ⇒ the block's position/radius are omitted from swing.json.
    bool     ballHasPos         = false;
    double   ballCenterX        = 0.0;   // [0,1]
    double   ballCenterY        = 0.0;   // [0,1]
    double   ballRadiusNorm     = 0.0;   // normalized to frame width
    QString  ballPosSource;              // "calibrated" (else empty)
    // Hitting-area ROI (ball search box) at capture, full-frame normalized —
    // lets offline re-analysis (BallRunner) search the same box the live
    // detector used instead of the pose-derived stance corridor. Empty ⇒ omitted.
    QRectF   ballSearchRoi;
    // Learned empty-mat baseline B (v2 temporal detector), cached live by
    // CameraInstance and copied here at export time so offline re-analysis can
    // reconstruct the tracker instead of self-seeding over an already-placed
    // ball. blob empty ⇒ no baseline learned live
    // (legacy swing, or a shot fired before the first seed completed) — the
    // exporter omits the sidecar + JSON key entirely. fps is provenance only
    // (re-analysis re-measures its own rate; see ball_detector.h R2 note).
    QByteArray ballBaselineBlob;             // row-major float32, w*h*4 bytes
    int        ballBaselineW    = 0;
    int        ballBaselineH    = 0;
    QRectF     ballBaselineRoi;              // full-frame normalized
    double     ballBaselineRHat   = 0.0;
    double     ballBaselineFps    = 0.0;
    double     ballBaselineNoise0 = 1.0;
};

// Per-IMU device configuration at capture time (stream "device" object),
// keyed by device serial like imuAliasBySerial. outputRateHz is the live
// instance's rate — authoritative over the registration-time ImuFormat.
struct SwingImuDeviceInfo {
    int     outputRateHz = 0;
    QString fusionMode;          // device 6/9-axis algorithm
    QString orientationFilter;   // host fusion algo (Madgwick / ESKF)
    QString placementSlot;       // "A"/"B"/"C" (AppSettings imuPlacement)
    int     role = 0;            // pinpoint::analysis::SegmentRole (0 = Unknown)
    QString roleName;            // stable role name, e.g. "LeadHand" (segmentRoleName)

    // Live A/M calibration snapshot, baked into the stream's "device" block so a
    // swing captured with analysis SKIPPED (corpus capture — no analysis.bindings)
    // can still be re-analysed: the disk loader rebuilds the IMU→segment bindings
    // from here when analysis.bindings is absent. alignA: fusion-world →
    // anatomical-world (A); mountM: anatomical-body → sensor-body (M).
    bool        hasCalibration = false;   // false → omit the calibration keys entirely
    QQuaternion alignA;
    QQuaternion mountM;
    bool        anatCalibrated = false;
    bool        calibrated     = false;
    double      mountDeviationDeg     = 0.0;
    double      mountGravityErrorDeg  = 0.0;
    QString     calibratedAtUtc;          // ISO8601; empty = never calibrated
    double      calibAgeSec = -1.0;       // age at shot time; -1 = never calibrated

    // HackMotion only: measured palm-minus-lower-arm skew, microseconds — the
    // SESSION MEDIAN, not a mean and never one record's reading. A single record's
    // tick difference is dominated by ±½-sample pairing jitter (89 and 99 ticks on
    // two consecutive records against a session median of 59), because the two units
    // share a sample index by construction and run two free-running MCU timers. The
    // wG3's two units are read from ONE stream but are NOT sampled
    // simultaneously — the measured skew is a stable ~0.92 ms, worth ~0.9° of
    // relative angle at 1000°/s (the device's primary output, wrist angular
    // velocity through impact). It is recorded rather than silently
    // subtracted from one lane's timestamps because its physical meaning is
    // unresolved and cannot be settled from the counters alone: it could be
    // real inter-unit sampling skew, or just the arbitrary phase between two
    // free-running counters that happens to be stable. 0.0 = not measured
    // (omit the key rather than write a fake zero-skew claim).
    double      skewUs = 0.0;

    // ── HackMotion capture provenance (Phase B′) ─────────────────────────────
    //
    // ⚠ THESE SAY WHETHER THE NUMBERS IN THIS STREAM CAN BE TRUSTED, and nothing
    // else in swing.json carries that. A HackMotion lane is ten floats per sample
    // like any other, so a reading taken before calibration and one taken after are
    // byte-indistinguishable — and the device applies its transform itself, so the
    // difference cannot be recovered later. §8.1 puts the uncalibrated mounting
    // offset at 11-15° at a straight wrist: real geometry, meaningless anatomy.
    //
    // -1 throughout means NOT MEASURED, never "fine". The exporter omits the keys
    // rather than writing a default that reads as a clean bill of health.
    int  hmCalibrationStateAtStart = -1;  // hm_calibration_state at the window start
    int  hmCalibrationStateAtEnd   = -1;  // ...and at its end
    bool hmCalibrationSpansTransition = false;  // it changed INSIDE the window
    int  hmConfigBits = -1;               // the `a0 01 <cfg>` byte behind these samples

    // ⚠ Saturation, counted over THIS window. int16 fields saturate rather than
    // wrap, so a clipped peak is a plausible flat top and nothing in the protocol
    // reports it (§6.4). A non-zero count here means a metric computed at impact may
    // be reading a clamped value; it is not a reason to discard the swing, and the
    // decision belongs to whoever reads it.
    int  hmPinnedSamples = 0;
    // Samples whose quaternion norm was outside tolerance — the decode may have been
    // misaligned, which makes the orientation meaningless rather than merely noisy.
    int  hmQuatNormSuspect = 0;
    // ⚠ Non-zero means pinning outran the capture ring, so hmPinnedSamples is a
    // FLOOR and not a count. It also invalidates the rarity assumption that ring is
    // sized on — see the plan's Phase B′.
    int  hmProvenanceDropped = 0;
    // ⚠ SESSION TOTAL, not windowed: a sample skipped for having no mapped host time
    // has no host time to attribute to a window. Should be 0 — the clock fit exists
    // from the first live frame — so a non-zero value is a symptom, not a rate.
    int  hmNoFitSkippedSession = 0;
};

// Host/app provenance recorded under capture.host — explains cross-host
// analysis variance (e.g. pose backend CPU vs CUDA) in a SwingLab corpus.
struct SwingHostInfo {
    QString appVersion, gitSha, hostname, platform, poseBackend;
};

// One camera's 2D pose series for the swing window, resolved on the UI thread.
// The swing exporter only *serialises* whatever is present here — it does NOT
// run pose estimation. There is no producer yet (pose buffering / the analyzer
// pose pipeline are a separate scope), so this is normally empty and nothing is
// written; the plumbing is in place for when a producer lands.
struct SwingPoseStream {
    QString alias;                 // camera label (matches the video stream alias)
    QString serial;                // camera serial, for cross-stream correlation
    std::vector<int64_t> tUs;      // per-frame timestamps, window-relative (us)
    // 17 COCO keypoints per frame as (y, x, score) normalised 0..1, flattened to
    // 51 floats per frame; size MUST be tUs.size() * 51.
    std::vector<float> keypoints;
};

// One camera's face-on ball stream for the swing window (v3.4 design §9.7 —
// deliberately low-entropy: a constant plus a single launch step). Sourced
// from CameraInstance::ballSamples()/ballLaunchInfo(), resolved on the UI
// thread. Empty ⇒ nothing written (additive — ball detection may be
// disabled, or this camera may not be the one running it).
struct SwingBallStream {
    QString alias;
    QString serial;
    std::vector<int64_t> tUs;      // per-frame timestamps, window-relative (us)
    // found/x/y/r/conf per frame, flattened to 5 floats/frame (x,y,r normalised
    // 0..1; found is 0.0/1.0); size MUST be tUs.size() * 5.
    std::vector<float> data;
    int64_t launchTUs = -1;        // window-relative; -1 = no launch in this window
    float   launchX = 0.f, launchY = 0.f;
};

// Self-contained job description.  Everything that touches QSettings or
// controllers is resolved on the UI thread; the worker sees only values.
struct SwingExportJob {
    QString swingDir;      // absolute, already created by SwingPaths
    QString swingId;       // "swing_0007"
    int     swingIndex = 0;

    std::vector<SwingExportCamera> cameras;
    QHash<QString, QString> imuAliasBySerial;  // device serial/id -> alias
    QHash<QString, SwingImuDeviceInfo> imuDeviceBySerial;  // device serial/id -> config
    std::vector<SwingPoseStream> poseStreams;  // pose to serialise (empty today)
    std::vector<SwingBallStream> ballStreams;  // v3.4 (design §9.7) — face-on ball stream(s)

    // Session context + provenance for the top-level "capture" block.
    int     sessionType = -1;    // SessionController::Type (-1 = none)
    int     shotSource  = 0;     // ShotController::Source as int
    QString swingDetectionSensitivity;   // "Low"/"Medium"/"High"
    QString motionCaptureQuality;        // "Medium"/"High" — offline pose model tier
    qint64  imuBleLatencyUs     = 0;     // detector back-dating constants at capture
    int     audioDeviceLatencyUs = 0;    // residual device latency (post mic-distance split)
    // Acoustic travel hitting-strip -> mic subtracted from the anchor at capture
    // (AppSettings::micTravelUs). Persisted as latencyUs.micTravel; its PRESENCE
    // tells SwingReanalyzer the anchor is already travel-corrected — absent =
    // legacy capture whose anchor carries the old 20 ms over-correction.
    qint64  micTravelUs         = 0;
    SwingHostInfo host;

    QString codec = QStringLiteral("h264");  // AppSettings videoCodec -> factory key
    int  crf     = 23;     // from AppSettings videoQuality
    bool saveImu = true;   // AppSettings saveImuStreams

    // AppSettings videoResolutionMode — export-time downscale (never upscale):
    // "native" (source), "half" (½), "1080p" / "4k" (fit to that line count).
    QString resolutionMode = QStringLiteral("native");
    // AppSettings saveRawFrames — also dump the undecoded sensor payloads to an
    // "<alias>.raw" sidecar (single concatenated blob) per camera.
    bool    saveRaw = false;
    // AppSettings imuDataFormat — "json" (inline in swing.json), "csv", or
    // "binary"; csv/binary write an "imu_<alias>.<ext>" sidecar instead.
    QString imuFormat = QStringLiteral("json");
    // AppSettings savePoseKeypoints — gate for serialising poseStreams (above).
    bool    savePose = true;

    QString athleteName, athleteUuid, handedness;
    QString sessionId;     // session folder name, e.g. "2026-06-05_Mark-Liversedge_Swing_01"

    // Club geometry for the shaft tracker's E1 band matcher, resolved from the
    // athlete's active club at capture and persisted into capture.club so
    // re-analysis can recover it (swing.json is otherwise the only record of the
    // club used). Empty/0 ⇒ the club block is omitted.
    double  clubLengthM = 0.0;             // metres
    QString shaftType;                     // "steel" | "graphite" | ""
    std::vector<double> bandCentersMm;     // retro-band centres from the butt (mm)
    double  hoselFromButtMm = 0.0;         // hosel offset from the butt (mm); 0 = unknown

    // Club-length prior (club_length_fusion.h / plan: robust club length):
    // clubName is the canonical club-vocabulary id (persisted so re-analysis can
    // rebuild the athleteUuid|clubName|cameraKey prior key); priorClubLen* are
    // the SAME values ShotProcessor fed into the ShotAnalysisJob for this shot
    // (the prior BEFORE this shot's update), so re-analysis reproduces the exact
    // fuse the live shot ran. <0 / 0 ⇒ no prior joined this shot.
    QString clubName;
    double  priorClubLenPx    = -1.0;
    double  priorClubLenVarPx = -1.0;
    int     priorClubLenN     = 0;

    // UTC instant snapshotted on the UI thread right after the window was
    // captured — at that moment wallclock ~= monotonic endTimestampUs().
    QDateTime wallclockAnchorUtc;

    // Impact thumbnail (thumb.jpg in the swing dir): the frame nearest this
    // instant from the designated camera (face-on, else the first exported
    // stream as a fallback). -1 disables thumbnail extraction.
    SourceId thumbnailSourceId    = kInvalidSourceId;
    int64_t  thumbnailTimestampUs = -1;

    // Impact instant, WINDOW-RELATIVE microseconds (impact − window.start), written
    // to capture.impactUs. This is the re-analysis impact reference: a corpus swing
    // captured with analysis skipped has no analysis.phases[Impact], so offline
    // re-analysis recovers impact from here. -1 = unknown.
    int64_t  impactUs = -1;

    // NOTE (product decision, 2026-06-11): exports are NEVER trimmed to the
    // detected swing — the saved artifact preserves every captured frame.
    // Segmentation bounds trim playback (replay span, metric grids) and the
    // analyzer's scan only.
};

struct SwingExportResult {
    bool        ok = false;
    QString     swingDir;
    QString     error;
    QString     thumbnailPath;   // absolute path to thumb.jpg; empty if none written
    QJsonObject manifest;        // the raw pinpoint.swing tree (no "analysis"); the GUI
                                 // thread writes the unified swing.json at the join.
};

// Stateless worker entry point.  Runs on a worker thread; the window must stay
// alive (buffer Paused) until this returns — enforced by CameraManager's
// resume gating.  Peak extra memory is one BGR scratch frame plus the
// encoder's single YUV frame; payloads are read zero-copy from the window.
class SwingExporter {
public:
    static SwingExportResult run(const SwingWindow& window, const SwingExportJob& job);

    // The top-level "capture" block (session context + host provenance) built
    // from the job's value fields. Shared with ShotProcessor's degraded
    // analysis-only manifest so both paths record identical metadata.
    static QJsonObject captureBlock(const SwingExportJob& job);
};

} // namespace pinpoint
