# Pinpoint Swing Export — Developer Guide

**Audience**: Developers working on or integrating with the Pinpoint application
**Location**: `src/Export/` (plus integration in `src/Gui/shot/shot_processor.{h,cpp}`)
**Language**: C++20, Qt 6
**Status**: Production — video + IMU + ball streams (codec h264/h265, container mp4/mov/mkv, export-time downscale, optional raw-payload sidecar, IMU json/csv/binary, pattern-based session-folder naming), impact thumbnail, extensive capture provenance, and a bulk zip export. Pose streams are serialised **if present on the job** but still have no producer; metrics/launch-monitor streams are schema-ready but not produced

> **⚠ Ownership moved (2026-06), and this guide predates several later changes.**
> The swing machinery described below as living in `CameraManager` (window
> capture, replay, `startSwingSave()`, `buildSwingExportJob()`,
> `maybeFinishSwing()`, the resume gating) **migrated to `ShotProcessor`**
> (`src/Gui/shot/shot_processor.{h,cpp}`), which drives it from
> `ShotController::shotDetected` and runs a per-session-type `ShotAnalyzer`
> **before** the export (the two heavy workers are sequenced, not overlapped —
> see the shot-analyzer guide §11). The join is now `maybeJoin()`/`finishShot()`;
> the teardown stop-barrier is `ShotProcessor::finishNowBlocking()`; resume
> gating keys off `EventBuffer::swingWindowLive()`. Read `CameraManager` as
> `ShotProcessor` wherever the text below touches window lifetime, and
> "5 s window" as **4 s**.
>
> Since then the exporter has also gained: the impact thumbnail (`thumb.jpg`, via
> `QImage::save` — see CLAUDE.md for the libjpeg-clash rationale), NV12/YUV420P
> handling, a **shared decode path** (`frame_decode.h`, §3), the **ball stream**
> and its persisted empty-mat baseline, the **capture provenance block**, and
> **`SwingZipExporter`** for bulk export. Those are covered in §3 and §6 below.
> The core invariants — values-in jobs, borrowed window, zero-copy reads,
> one-writer-per-file — are unchanged.

---

## Contents

1. [What Swing Export Is](#1-what-swing-export-is)
2. [Where It Fits in Pinpoint](#2-where-it-fits-in-pinpoint)
3. [Core Concepts](#3-core-concepts)
4. [The Export Pipeline — Frame by Frame](#4-the-export-pipeline--frame-by-frame)
5. [Resume Gating — the CameraManager Join](#5-resume-gating--the-cameramanager-join)
6. [The `swing.json` Sidecar](#6-the-swingjson-sidecar)
7. [On-Disk Layout — `SwingPaths`](#7-on-disk-layout--swingpaths)
8. [The Encoder — `IVideoEncoder` / `FfmpegVideoEncoder`](#8-the-encoder--ivideoencoder--ffmpegvideoencoder)
9. [Configuration — AppSettings Keys](#9-configuration--appsettings-keys)
10. [Threading Model](#10-threading-model)
11. [Build System — FFmpeg Detection](#11-build-system--ffmpeg-detection)
12. [Extending the Module](#12-extending-the-module)
13. [Common Mistakes](#13-common-mistakes)
14. [File Map](#14-file-map)
15. [Testing](#15-testing)

Two sections carry most of what changed since this guide was first written:
**§3** (the job's provenance fields and the shared `frame_decode` path) and
**§6** (the ball stream, the baseline sidecar, and `swing_summary.json`).

---

## 1. What Swing Export Is

Swing Export persists a captured swing to disk the moment it is detected. For each
swing it writes one folder containing:

- **One H.264/MP4 per camera**, named by the camera alias. Every captured frame is
  kept; the container framerate is fixed at **30 fps**, so a 150 fps capture plays
  back as ≈5× slow motion with zero frames dropped or duplicated.
- **One `swing.json`** — an extensible sidecar holding video stream descriptors, a
  per-frame microsecond timestamp index, and the embedded IMU sample streams.

It is built around one hard constraint: **the video rings are GB-scale and must
never be duplicated in RAM.** The exporter streams frame-by-frame directly out of
the frozen `SwingWindow` via zero-copy reads. Peak extra memory is one reusable
BGR scratch frame plus the encoder's single YUV frame — never the ring.

It is not a general recording facility. It runs only while the `EventBuffer` is
`Paused` with a live `SwingWindow`, concurrently with the on-screen replay, and the
buffer cannot resume until it finishes.

---

## 2. Where It Fits in Pinpoint

```
┌────────────────────────────────────────────────────────────────────────┐
│  Ball lost (swing complete)                                            │
│       │                                                                │
│  EventBuffer::pause()  ──►  captureSwingWindow(5000ms)                 │
│       │                          │                                     │
│       │                    m_swingWindow (frozen, zero-copy)           │
│       │                     │              │                           │
│       │              [Replay ¼×]     [SwingExporter]                   │
│       │              (UI thread,     (QtConcurrent worker:             │
│       │               60Hz timer)     per-camera MP4 + swing.json)     │
│       │                     │              │                           │
│       │                     └──── join ────┘                           │
│       │                  maybeFinishSwing()                            │
│       │             (both done → window destroyed)                     │
│       ▼                          │                                     │
│  EventBuffer::resume()  ◄────────┘  (iff recording && ball present)    │
└────────────────────────────────────────────────────────────────────────┘
```

### The export workflow

1. **Ball lost**: `CameraManager::onCameraBallPresenceChanged(false)` pauses the
   buffer and calls `startReplay()`.
2. **Window capture**: `startReplay()` captures a trailing 5 s `SwingWindow`,
   builds the replay tracks, and emplaces the window into `m_swingWindow`.
3. **Job build (UI thread)**: `buildSwingExportJob()` resolves everything that
   touches QSettings or controllers — camera aliases, CRF, IMU alias map, athlete
   metadata, the output directory — into a value-type `SwingExportJob`.
4. **Worker launch**: `startSwingSave()` hands the job plus a pointer to the live
   window to `QtConcurrent::run`. The replay and the save now read the same frozen
   window concurrently.
5. **Encode**: the worker encodes each camera sequentially, then writes
   `swing.json` atomically via `QSaveFile`.
6. **Join**: replay end and save completion both funnel into
   `maybeFinishSwing()`. Whichever finishes **last** destroys the window and
   re-evaluates the resume condition.
7. **Resume**: the buffer resumes only if recording is active, a ball is present,
   and nothing (e.g. `stopAll()`) vetoed auto-resume.

The exporter is invisible to QML in v1. `CameraManager` emits
`swingSaved(QString path)` / `swingSaveFailed(QString error)`; diagnostics go
through `ppInfo()`/`ppWarn()`/`ppError()` (auto-captured by `PpMessageLog`).

---

## 3. Core Concepts

### SwingExportJob — values in, no controllers

The worker must never touch `QSettings`, `AppSettings`, or any controller — those
are UI-thread objects. Everything is resolved up-front into a `SwingExportJob`:

```cpp
struct SwingExportJob {
    QString swingDir;                          // absolute, already created
    QString swingId;       int swingIndex;

    std::vector<SwingExportCamera> cameras;    // aliases sanitised + deduped
    QHash<QString, QString>            imuAliasBySerial;
    QHash<QString, SwingImuDeviceInfo> imuDeviceBySerial;   // rate, fusion, A/M
    std::vector<SwingPoseStream> poseStreams;  // no producer yet
    std::vector<SwingBallStream> ballStreams;  // face-on ball track + launch step

    // Capture provenance — session context, detector constants, host/build
    int sessionType, shotSource;
    QString swingDetectionSensitivity, motionCaptureQuality;
    qint64 imuBleLatencyUs;  int audioDeviceLatencyUs;
    SwingHostInfo host;                        // appVersion, gitSha, hostname,
                                               // platform, poseBackend

    // Encode + serialisation settings
    QString codec;  int crf;  bool saveImu;
    QString resolutionMode;                    // native | half | 1080p | 4k
    bool    saveRaw;                           // "<alias>.raw" sidecars
    QString imuFormat;                         // json | csv | binary
    bool    savePose;

    QString athleteName, athleteUuid, handedness, sessionId;

    // Club geometry + the club-length prior, so re-analysis reproduces the fuse
    double clubLengthM, hoselFromButtMm;  QString shaftType, clubName;
    std::vector<double> bandCentersMm;
    double priorClubLenPx, priorClubLenVarPx;  int priorClubLenN;

    QDateTime wallclockAnchorUtc;              // see §6, clock block
    SourceId  thumbnailSourceId;  int64_t thumbnailTimestampUs;
    int64_t   impactUs;                        // WINDOW-RELATIVE µs
};
```

`SwingExporter::run(const SwingWindow&, const SwingExportJob&)` is a **stateless
static function** — all scratch is local, the result comes back as a
`SwingExportResult{ok, swingDir, error, thumbnailPath, manifest}` value through
the `QFuture`.

Note the last field: the exporter **returns** the raw `pinpoint.swing` manifest
tree rather than writing it. The unified `swing.json` (manifest + the analyzer's
`"analysis"` block) is written once, at the join, on the GUI thread — see the
shot-analyzer guide §9. `SwingExporter::captureBlock(job)` is exposed separately
so the degraded analysis-only path can synthesise a header with *identical*
metadata.

### Two provenance rules the job encodes

**`impactUs` is window-relative, and it is the re-analysis anchor.** A corpus
swing captured with analysis skipped has no `analysis.phases[Impact]`, so
`capture.impactUs` is the only record of when the ball was struck. Everything
under `analysis` is likewise normalised to window-relative µs on write — absolute
`nowMicros()` values are meaningless in a file that outlives the process.

**Everything re-analysis needs must be *in the file*.** The club geometry, the
club-length prior as it stood *before* this shot's update, the motion-capture
quality tier, the IMU A/M calibration snapshot, the hitting-area ROI, and the
learned empty-mat baseline are all persisted for one reason: offline re-analysis
must reproduce what the live shot did without reading `AppSettings`, which may
have changed since. If you add an input that affects analysis, persist it here.

### `SwingExportCamera` — per-stream setup, not just a filename

Each camera carries its capture-time setup into the stream's `"setup"` object:
perspective (None/DownTheLine/FaceOn/Other), mirroring, fixed-in-place, exposure
and its provenance, plus the ball-detection block — calibration state, the
calibrated ball position and radius (full-frame normalised, co-registered with
the shaft-track head samples), the hitting-area ROI, and the learned baseline
blob. The baseline is written as a sidecar; an empty blob omits both the sidecar
and its JSON key entirely, and re-analysis then self-seeds as before.

### IVideoEncoder — abstract base + factory

The codec backend is product-specific, so the encoder is an abstract interface
obtained through a factory:

```cpp
class IVideoEncoder {
    virtual bool open(const VideoEncoderConfig&) = 0;
    virtual bool writeBgr(const cv::Mat& bgr) = 0;  // encoder owns pts
    virtual bool finish() = 0;                      // flush + trailer
};
std::unique_ptr<IVideoEncoder> makeVideoEncoder(const std::string& codec);
```

`"h264"` → `FfmpegVideoEncoder("libx264")`, `"h265"` → `FfmpegVideoEncoder("libx265")`
when built with `HAVE_FFMPEG`; any other key falls back to H.264 (so a stale
`prores`/`raw` setting never loses a swing). A no-FFmpeg build → `nullptr`, and the
save fails gracefully with a logged error. `video_encoder.cpp` is the **only** file
in the module with `HAVE_FFMPEG` ifdefs.

### The borrowed window

The worker receives the `SwingWindow` **by const reference** — it does not own it,
copy it, or extend its life. The contract is enforced entirely by `CameraManager`:
the window is destroyed only in `maybeFinishSwing()`, on the UI thread, strictly
after the worker has returned (guaranteed by `QFutureWatcher::finished` ordering —
see §10).

### Zero-copy reads, through one shared decode path

Per frame, the worker calls `window.payloadOf(entry)` and wraps the returned bytes
in a `cv::Mat` header **without copying**:

```cpp
const cv::Mat raw(srcH, srcW, plan.matType,
                  const_cast<std::byte*>(handle.data), stride);
cv::cvtColor(raw, bgr, plan.cvtCode);    // demosaic into the ONE reused scratch
```

While the buffer is `Paused` the producers are stopped, so the bytes are stable —
this is the same guarantee the replay path relies on.

The demosaic table used to live inside `SwingExporter`. It is now
**`src/Export/frame_decode.h`** — pure functions, no Qt, no logging — because the
shot analyzer decodes the same frozen payloads and the two must not be able to
diverge. It exposes:

| Function | Purpose |
|---|---|
| `demosaicPlanFor(PixelFormat)` | `{supported, matType, cvtCode, tag, rowsNum/rowsDen}`. The `tag` is what lands in `swing.json` `processing.demosaic`. |
| `frameGeometry(CameraFormat&, stride, minBytes)` | Plane-0 stride and the minimum bytes of one frame. False for unsupported formats or degenerate dimensions. |
| `decodeFrame(...)` | One payload → BGR `CV_8UC3`. False on null/short payloads and unsupported formats. |

Two details in there that bite:

- The mapping deliberately **mirrors the live-view path**
  (`camera_instance.cpp` → `raw_video_frame.cpp`) so an exported frame's colour
  matches what was on screen. The edge-aware `_EA` variants share the pattern
  naming — better quality, and the cost is irrelevant off the hot path.
- Planar 4:2:0 payloads (NV12/I420) are **1.5× image-height rows tall**, which is
  what `rowsNum`/`rowsDen` encode. Treating them as `height` rows silently
  truncates the chroma planes.

Only 8-bit formats are handled; MJPEG, H264_NAL and 12/16-bit Bayer are
unsupported and skip the source with a warning.

---

## 4. The Export Pipeline — Frame by Frame

Per camera (cameras are encoded **sequentially** — lowest peak RAM; per-camera
parallelism is a possible future option):

1. `entriesFor(sourceId)` → ordered frame entries; `formatOf(sourceId)` →
   `CameraFormat` (dimensions, pixel format, capture fps, strides).
2. Pixel format is mapped to a `DemosaicPlan` (see below). Unsupported formats
   (`MJPEG`, `H264_NAL`, 12/16-bit Bayer) skip the **source** with a `ppWarn` —
   the remaining cameras still export.
3. Dimensions are **cropped** (never padded) to even values — libx264/yuv420p
   requires them, and padding would invent pixels.
4. `makeVideoEncoder(job.codec)` → `open()` with CRF from settings, preset
   `medium`, output 30 fps.
5. Per entry: `payloadOf(e)` — a null or short payload **skips the entry
   entirely**, which keeps the invariant *MP4 frame i == `frames.t_us[i]`*.
   Demosaic into the reused scratch, then:
   ```cpp
   // TODO(restorer): frame restoration hook
   ```
   This comment marks the **single** documented insertion point where a future
   denoise/sharpen stage will go. Nothing else sits between demosaic and encode.
6. `writeBgr(bgr(cropRect))`; record `e.timestamp_us - t0` for the JSON index.
7. `finish()` flushes the encoder and writes the MP4 trailer.

### Demosaic — matching the live view exactly

The OpenCV `COLOR_Bayer*` constants are offset from the sensor pattern naming —
getting this wrong swaps colour channels. The exporter's mapping **mirrors the
live-view path** (`camera_instance.cpp` PixelFormat→BayerPattern,
`raw_video_frame.cpp` pattern→cvtColor code) so exported colour is identical to
what the user saw on screen:

| `PixelFormat` | cvtColor code | JSON `demosaic` tag |
|---|---|---|
| `BayerRG8` | `COLOR_BayerRGGB2BGR_EA` | `"EA"` |
| `BayerBG8` | `COLOR_BayerBGGR2BGR_EA` | `"EA"` |
| `BayerGR8` | `COLOR_BayerGRBG2BGR_EA` | `"EA"` |
| `BayerGB8` | `COLOR_BayerGBRG2BGR_EA` | `"EA"` |
| `Mono8` | `COLOR_GRAY2BGR` | `"none"` |
| `BGR24` | passthrough (no copy) | `"none"` |
| `YUV422` / `YUYV` | `COLOR_YUV2BGR_YUYV` | `"none"` |
| `UYVY` | `COLOR_YUV2BGR_UYVY` | `"none"` |

The `_EA` (edge-aware) variants share the same pattern naming as the live view's
bilinear codes — identical colour mapping, better interpolation quality. The cost
difference is irrelevant off the hot path.

### IMU streams

When `saveImuStreams` is enabled, IMU sources are **discovered from the window
itself**: distinct `source_id`s in `window.entries()` whose `FormatDescriptor`
holds the `ImuFormat` variant. Each payload is `memcpy`d into a local `ImuSample`
(the alignment-safe equivalent of a reinterpret-cast — the payload is the decoded
fixed 40-byte sample) and emitted as parallel `t_us` + 10-float rows:
accel xyz (g), gyro xyz (deg/s), quat wxyz.

---

## 5. Resume Gating — the CameraManager Join

This is the part most likely to bite you when modifying `CameraManager`.
**The `SwingWindow` is valid only while the buffer is `Paused`. The save worker
reads ring memory through it. Therefore the buffer must not resume — and the
window must not be destroyed — until BOTH the replay and the save have finished.**

### State

```cpp
QFutureWatcher<SwingExportResult> m_swingSaveWatcher;  // finished → onSwingSaveFinished
bool m_swingSaveInFlight = false;   // worker still reading the window
bool m_swingAutoResume   = true;    // cleared by stopAll() / stopReplay(false)
```

All of this state lives on the UI thread. The worker communicates only through
`QFutureWatcher::finished` (delivered on the UI thread), so no mutexes exist.

### The join

```cpp
void CameraManager::maybeFinishSwing()
{
    if (m_replaying || m_swingSaveInFlight) return;   // wait for BOTH
    if (!m_swingWindow) return;                        // already finished
    m_swingWindow.reset();                             // worker has returned — safe
    if (m_swingAutoResume && m_recording && m_ballPresentCount > 0)
        resumeBuffer();
}
```

- `stopReplay()` no longer resets the window or resumes — it clears replay state
  and calls `maybeFinishSwing()`.
- `onSwingSaveFinished()` clears `m_swingSaveInFlight`, emits
  `swingSaved`/`swingSaveFailed`, and calls `maybeFinishSwing()`. **The failure
  path joins identically** — a failed save must still release the window and let
  the buffer resume.
- The ball condition is evaluated **at join time**, not at `stopReplay()` time, so
  a ball re-appearing (or leaving) during a long save is handled correctly.

### Every path that could violate the invariant, and its guard

| Path | Guard |
|---|---|
| QML / ball-handler calling `resumeBuffer()` mid-save | `resumeBuffer()` returns early while `m_swingWindow` is live — the hard backstop. The join re-checks ball presence, so a resume is never lost. |
| Second swing while a save is in flight | `startReplay()` returns early if `m_swingWindow` is live (a second `captureSwingWindow()` would assert in `EventBuffer`). |
| `stopAll()` mid-save | Sets `m_swingAutoResume = false` *before* `stopReplay(false)` so a save finishing later cannot resume a stopped session. `startReplay()` re-arms the flag for the next swing cycle. |
| Camera deselect mid-save (`setSelected`) | `deregisterSource()` asserts no live window, and the worker reads ring memory — so the path **blocks**: `stopReplay(false)` → `m_swingSaveWatcher.waitForFinished()` → `m_swingWindow.reset()`. (This also fixed a pre-existing deselect-during-replay assert.) |
| App exit (`~CameraManager`) | Waits on the watcher, then resets the window, before any ring teardown. Abandoning the worker would be a use-after-free. |
| Ball lost again while save in flight | `pauseBuffer()` is a no-op (already Paused) and `startReplay()`'s window guard returns — correct, because the buffer captured nothing new while Paused. |

### Rules

**You MUST:**
- Destroy `m_swingWindow` only inside `maybeFinishSwing()` (or after a blocking
  `waitForFinished()` in the teardown paths).
- Route every new resume path through `resumeBuffer()` so the window guard applies.
- Call `maybeFinishSwing()` after clearing any of the two completion flags.

**You MUST NOT:**
- Add a resume call that bypasses `resumeBuffer()`.
- Capture a new `SwingWindow` while one is live.
- Touch `m_swingWindow` from the worker — the worker sees only the const reference
  it was handed.

---

## 6. The `swing.json` Sidecar

Schema identifier: **`pinpoint.swing/1`**. Built with `QJsonDocument`, written
atomically with `QSaveFile`. All stream timestamps are **relative µs offsets from
`t0`** (small, exact); `t0_us` is the absolute capture-clock value. A new stream
type later is just another element of `streams[]` — readers must ignore unknown
`kind`s.

> **This is the exporter's *raw* manifest.** At the analyzer→exporter join,
> `SwingDocWriter::writeSwingJson()` (§15) composes this tree with the analyzer's
> output into **one** `swing.json` — bumping the on-disk schema to **`pinpoint.swing/2`**
> and adding inline `analysis` (`pinpoint.analysis/2`: score, tier, metric series,
> phases) and `review` (user rating/note) blocks, plus a `thumbnail` block. There is
> no separate `analysis.json`. The raw `streams[]` shown below are preserved verbatim.

```json
{
  "schema": "pinpoint.swing/1",
  "swing":   { "index": 7, "id": "swing_0007" },
  "athlete": { "name": "Mark Liversedge", "uuid": "…", "handedness": "Right" },
  "session": { "dir": "2026-06-05_session-01" },
  "clock":   { "t0_us": 173456789012345, "unit": "us",
               "wallclock": "2026-06-05T14:22:01.234Z" },
  "window":  { "start_us": 0, "end_us": 4980000 },
  "capture": {
    "sessionType": 1, "shotSource": 1,
    "swingDetectionSensitivity": "Medium",
    "latencyUs": { "imuBle": 30000, "audioDevice": 20000 },
    "host": { "app": "PinPointStudio", "version": "0.1", "gitSha": "cab95fb",
              "hostname": "studio-pc", "platform": "Ubuntu 26.04",
              "poseBackend": "CUDA" }
  },
  "streams": [
    {
      "kind": "video", "alias": "faceOn", "file": "faceOn.mp4",
      "source": { "serial": "…", "pixelFormat": "BayerRG8",
                  "width": 1936, "height": 1096 },
      "capture":  { "fps_num": 150, "fps_den": 1,
                    "exposureUs": 250.0, "exposureAuto": false, "exposureSource": "measured" },
      "setup":    { "perspective": 2, "perspectiveName": "FaceOn",
                    "mirrored": false, "fixedInPlace": true,
                    "ballDetection": { "calibrated": true, "margin": 0.42,
                                       "driftAtCapture": 0.0,
                                       "calibratedAt": "2026-06-12T09:14:03Z",
                                       "center": [0.51, 0.78], "radiusNorm": 0.012,
                                       "positionSource": "calibrated" } },
      "playback": { "fps": 30 },
      "processing": { "demosaic": "EA", "restorer": "none" },
      "frames": { "count": 742, "t_us": [0, 6671, 13342] }
    },
    {
      "kind": "imu", "alias": "leadWrist", "schema": "imu_sample_v2",
      "source": { "serial": "…" },
      "device": { "outputRateHz": 200, "fusionMode": "6axis",
                  "orientationFilter": "Madgwick", "placementSlot": "A" },
      "units": { "accel": "g", "gyro": "deg/s", "quat": "wxyz" },
      "samples": { "count": 498, "t_us": [0, 5000, 10000],
                   "data": [[0.01, 0.99, 0.02, 1.2, 0.3, -0.1, 1, 0, 0, 0]] }
    }
  ]
}
```

### Capture provenance (additive, 2026-06)

Recorded so SwingLab can filter a corpus by calibration/session provenance and
stop hardcoding assumptions; all additive (absent on legacy swings):

- **Top-level `capture`** — session context (`sessionType` = `SessionController::Type`,
  `shotSource` = `ShotController::Source`, detection sensitivity, the detector
  back-dating latencies) and host provenance (`version`/`gitSha` from the generated
  `pp_version.h`, hostname/platform from `QSysInfo`, `poseBackend` from the first
  camera's live pose-backend label). Built by `SwingExporter::captureBlock()` —
  shared with `ShotProcessor::buildSynthManifest()` so the degraded analysis-only
  path records identical metadata.
- **Per-video-stream `setup`** — `perspective`/`perspectiveName` (CameraInstance
  enum: None 0, DownTheLine 1, FaceOn 2, Other 3), `mirrored`, `fixedInPlace`
  (the camera-side "calibrated" signal), and `ballDetection` — the calibrated
  ball detector's state at capture: `calibrated` (profile loaded), `margin`
  (the profile's validated ball/empty separation), `driftAtCapture`
  (illumination-drift severity, 0 when clean), `calibratedAt` (ISO 8601 UTC,
  null when never calibrated). SwingLab filters the corpus on these
  (`ballCalibrated`/`ballMargin` in corpus.json). When a valid calibration ball
  is present the block also carries the stable ball **position + scale** —
  `center` `[x,y]` and `radiusNorm` (full-frame normalized, radius to frame
  width) + `positionSource` — co-registered with `analysis.club.samples[].head`;
  this is the enabling data for the deferred low-point-ahead-of-ball metric
  (`docs/design/low_point_metric_design.md`), omitted on uncalibrated streams.
- **Per-video-stream `capture` exposure** (additive, 2026-07) — alongside
  `fps_num`/`fps_den`: `exposureUs` (per-stream exposure time, microseconds),
  `exposureSource` (`"measured"` = read from the frame's chunk data on industrial
  cameras, `"derived"` = `1/(2·fps)` fallback where the frame carries no exposure,
  e.g. webcams), and `exposureAuto` (bool, measured streams only — `true` when the
  camera ran auto-exposure so the value is frame-varying). Sourced from
  `CameraFormat::exposure_us`/`exposure_source`; keys omitted when unknown. Consumed
  by the shaft detector to gauge motion-blur/wedge width (`ω · t_exposure`); see
  [`shaft_detection_improvements.md`](../design/shaft_detection_improvements.md)
  (§5.3, `1/(2·fps)` fallback).
- **Per-IMU-stream `device`** — `outputRateHz` (live instance rate — authoritative
  over the registration-time `ImuFormat`), `fusionMode` (device 6/9-axis),
  `orientationFilter` (host fusion: Madgwick/ESKF), `placementSlot` (A/B/C).
- **`analysis.bindings[]` calibration status** — alongside `alignA`/`mountM`:
  `calibrated` (composite gate: `anatCalibrated` AND mount deviation ≤ 15° AND
  gravity error ≤ 25° — `ImuInstance::fullyCalibrated()`), the two gate angles,
  `calibratedAt` (ISO8601 UTC) and `calibAgeSec` at shot time (−1 / empty = never
  calibrated). **The A/M snapshot is *also* duplicated into each IMU stream's
  `device` block**, and that duplication is load-bearing: a swing captured with
  analysis skipped (corpus capture) has no `analysis.bindings` at all, so the
  disk loader rebuilds the IMU→segment bindings from `device` instead.
- **`capture.impactUs`** (window-relative µs) — the re-analysis impact anchor for
  an analysis-skipped swing, which has no `analysis.phases[Impact]` to read.
- **`capture.club`** — club length, shaft type, retro-band centres and hosel
  offset, plus the club-length prior as it stood *before* this shot's update, so
  re-analysis reproduces the exact length fuse the live shot ran.
- **`capture.motionCaptureQuality`** — the offline pose-model tier, so
  re-analysis picks the same model rather than the current setting.

### The ball stream and the baseline sidecar

The face-on camera contributes a `kind: "ball"` stream — deliberately
low-entropy: per-frame `found/x/y/r/conf` plus a single launch step
(`launchTUs`, `launchX`, `launchY`, window-relative). Empty means nothing is
written: ball detection may be off, or this may not be the camera running it.

Separately, each camera may carry its learned **empty-mat baseline** `B` — the
v2 temporal detector's self-calibrated reference (see the shot-detector guide
§7) — as a raw `float32` sidecar plus a JSON key holding its dimensions, ROI,
radius estimate, noise level and (provenance-only) fps. This exists so offline
re-analysis reconstructs the tracker from the baseline the live session actually
learned, instead of self-seeding over a window in which the ball is *already
placed* — which would subtract the ball into the baseline and then never detect
it. An absent blob omits both the sidecar and the key, and re-analysis
self-seeds as before.

### Timestamps in the `analysis` block are window-relative too

Everything under `analysis` — metric series `t_us`, phase events — is normalised
to window-relative µs on write, matching the stream timestamps. Absolute
`nowMicros()` values are meaningless in a file that outlives the process; the
reader re-bases them when reconstructing `analysisDetail`.

### `swing_summary.json` — a regenerable index sidecar

A Wrist swing's `swing.json` runs to tens of MB (`analysis.pose2d` alone is
~13 MB, retained for replay overlays), and the session picker needs about eight
scalars from each. Parsing every document to draw a list was a visible stall.

`swing_summary.json` (schema `pinpoint.swingsummary/1`, a few hundred bytes)
caches exactly those scalars, guarded by the source document's **size + mtime**
so any out-of-band rewrite — re-analysis, corpus tooling — is detected and the
sidecar regenerated. It is **pure cache**: always safe to delete, always
regenerable.

Two API details matter:

- `SwingDocReader::readSwingSummary(dir, writeSidecar)` prefers the sidecar and
  falls back to a full parse on a miss or stale guard. Pass `writeSidecar=false`
  on any GUI-thread path that must never fat-parse — the caller then gets
  `ok=false` for an un-indexed swing and renders it without detail, rather than
  stalling.
- Both the cheap and the full path extract through **one** function
  (`summaryFromRoot`), so they cannot disagree about a score shape, a club
  fallback or a thumbnail name. `SwingSummary::fromSidecar` is provenance the
  parity test uses to prove the cheap path was actually exercised — a bug that
  always fell back would otherwise pass silently while the stall crept back.

### Field sources

| Field | Source |
|---|---|
| `t0_us` | `window.startTimestampUs()` — the monotonic capture clock (`EventBuffer::nowMicros()` epoch) |
| `wallclock` | Honest approximation: a UTC anchor is snapshotted on the UI thread right after window capture (when wallclock ≈ monotonic `endTimestampUs()`), then the window duration is subtracted. Accurate to milliseconds. |
| `window.end_us` | `endTimestampUs() − t0` |
| video `source.*`, `capture.*` | `CameraFormat` via `window.formatOf()` (`serial` from `FormatDescriptor::device_serial`) |
| video `capture.exposureUs` / `exposureSource` / `exposureAuto` | `CameraFormat::exposure_us` / `exposure_source` — stamped from Spinnaker chunk data (`GetChunkData().GetExposureTime()`) or fps-derived; read back via `exposureSourceFromName()` |
| video `frames.t_us` | Recorded in the encode loop — **written frames only**, so frame *i* in the MP4 corresponds to entry *i* here, always |
| imu `samples` | Decoded `ImuSample` payloads (40 bytes / 10 floats, schema `imu_sample_v2` — accel, gyro, and quaternion all in the raw sensor frame; see [`imu_frame_contract.md`](../design/imu_frame_contract.md)) |
| `athlete`, `session`, `swing` | The `SwingExportJob` (resolved on the UI thread) |

The `frames.t_us` index is what lets any downstream tool map output frame *i* to
its true capture instant and cross-reference IMU samples — the MP4 itself carries
no real-time timing (it plays at a fixed 30 fps).

> **`device_serial` note**: `EventBuffer::registerSource()` normalises the
> registrar's `SourceDescriptor::identifier` (serial when present, else opaque
> device id) into `FormatDescriptor::device_serial`, so window readers can
> attribute data to a physical device without access to the descriptor. This was
> added for the exporter and applies to every source.

### Reload & replay — consumer contract

Two readers consume `swing.json` from disk: `SwingDocReader::readSwingJson()`
(carousel reload at startup) and `ShotReplayController` (disk-backed playback).
Both are deliberately **forward-compatible** — they select streams by `kind` and
ignore unknown keys — so the additive blocks below cause no breakage when absent
and no parsing when present-but-unconsumed:

- **Container** — replay feeds `QMediaPlayer` the `file` field verbatim, so
  `mp4`/`mov`/`mkv` all work. `errorOccurred`/`InvalidMedia` are surfaced
  (`replayFailed`) rather than rendering a silent black frame.
- **`encoded` block / downscale** — transparent: `VideoOutput` sizes from the
  decoded frame; no reader reads stream dimensions.
- **`raw` block + `<alias>.raw` sidecar** — **not consumed yet.** A future
  "view raw / reprocess" reader reconstructs frames from
  `pixelFormat`/`width`/`height`/`stride`/`frameBytes`/`count`.
- **IMU streams (`kind:"imu"`)** — **not consumed yet.** When an IMU-reload
  consumer is added it MUST branch on `samples.format`: inline `data` for
  `json`, else load the `imu_<alias>.csv`/`.bin` sidecar named by `file`
  (binary record = LE `i64 t_us` + `f32[10]` accel3/gyro3/quat4).
- **Pose streams (`kind:"pose"`, `pose_movenet_v1`)** — **not consumed yet.**
  The basis for a future skeleton-overlay-on-replay: per-frame `frames.data`
  holds 51 floats (`layout: coco17:y,x,score`, normalised 0..1) against
  `frames.t_us`. Empty today (no producer).

`latestSessionDir()` selects the most recent session by directory mtime, not by
name — session folder names now embed the naming-pattern tokens, so a name sort
no longer tracks recency.

---

## 7. On-Disk Layout — `SwingPaths`

```
<athleteLibraryPath>/
  <athlete-name-sanitised>/            athlete level
    <YYYY-MM-DD_session-NN>/           session level — date + per-day index
      swing_0001/
        <faceOnAlias>.mp4
        <dtlAlias>.mp4
        swing.json
      swing_0002/
        …
```

### Session allocation policy

The session folder is allocated **lazily on the first save and cached for the
lifetime of the `SwingPaths` instance** — i.e. per app run. All swings recorded
today by the same athlete into the same library share one `session-NN`, across
Stop/Start cycles. The cache key is `(athleteUuid, todayISO, libraryRoot)`; a new
day, an athlete switch, or a library-root change reallocates. `NN` is
`max(existing for today) + 1`, zero-padded to two digits.

### Swing allocation

Count of existing `swing_*` directories + 1, formatted `swing_%04d`, probed upward
until unused (gaps and collisions are tolerated), then created with `mkpath`.

### Fallbacks

- Empty `athleteLibraryPath` → `<AppDataLocation>/swings`, with a `ppWarn`.
- Empty athlete name → uuid, then `"unknown"`.

### `sanitise()`

One shared rule for athlete folders and camera-alias filenames: trim → any char
outside `[A-Za-z0-9._-]` becomes `-` → collapse separator runs → strip
leading/trailing `-`/`.` → truncate to 64 → `"unknown"` if nothing survives.
Camera filename collisions are deduped `-2`, `-3`, … in `buildSwingExportJob()`.

---

## 8. The Encoder — `IVideoEncoder` / `FfmpegVideoEncoder`

### Output contract

- Container from the file extension (`mp4`/`mov`/`mkv`), codec **libx264** or
  **libx265** (selected by name at construction), `AV_PIX_FMT_YUV420P`. `profile
  High` is set for libx264 only (libx265 rejects that profile name).
- Colour tagged **BT.709, limited range** (`color_primaries`/`color_trc`/
  `colorspace = BT709`, `color_range = MPEG`), and the BGR→YUV conversion uses
  matching ITU-709 coefficients via `sws_setColorspaceDetails` — tags and pixels
  agree.
- `movflags +faststart` — the `moov` atom precedes `mdat`, so clips stream/scrub
  immediately.
- `time_base = {1, 30}`, **pts = sequential frame index** (0, 1, 2, …). This is
  the entire slow-motion mechanism: source frame count == output frame count, and
  the clip plays at 30 fps regardless of capture rate.
- CRF from settings, preset `medium`. Encode speed is deliberately not optimised —
  the save hides under the 20–30 s analysis/replay pause.

### Lifecycle

`open()` follows the canonical libav sequence (`avformat_alloc_output_context2` →
`avcodec_find_encoder_by_name(m_codecName)` → `avformat_new_stream` →
`avcodec_alloc_context3` → `avcodec_open2` → `avcodec_parameters_from_context` →
`avio_open` → `avformat_write_header`). One `AVFrame` and one `AVPacket` are
allocated once and reused for every frame. Every libav return code is checked; any
failure logs via `ppError` (with `av_strerror` — the `av_err2str` macro is C-only),
runs the idempotent `cleanup()`, and returns `false`. `cleanup()` is also the
destructor path, so a mid-encode failure can never leak contexts; the partial file
is left truncated and the caller treats the export as failed.

### ⚠ The packet-duration fix — do not remove

```cpp
// drainPackets():
m_pkt->duration = 1;     // one tick of time_base {1, out_fps}
av_packet_rescale_ts(m_pkt, m_enc->time_base, m_stream->time_base);
```

The libx264 wrapper emits packets with `duration = 0`. With B-frame reordering,
the mov muxer then computes the track's edit list ending at the **last DTS**
instead of last pts + duration. The final reordered frame falls outside the edit
list, gets the `AV_PKT_FLAG_DISCARD` flag on demux, and **every decoder silently
drops the last frame of every clip** — the file still reports the full
`nb_frames`, so nothing obvious fails. Found via
`ffprobe -count_frames` (90 packets, 89 decoded frames). Since every frame here is
exactly one time-base tick, stamping the duration explicitly is always correct.

### Header hygiene

`ffmpeg_video_encoder.h` **forward-declares** the libav types (`AVFormatContext`
etc.) and includes no FFmpeg headers — only the `.cpp` does (inside
`extern "C"`). The header therefore parses in any TU regardless of whether FFmpeg
dev headers are installed.

---

## 9. Configuration — AppSettings Keys

The exporter reuses existing keys — **do not invent parallel settings**.

| Key | Used as |
|---|---|
| `general/athleteLibraryPath` | Library root for `SwingPaths` |
| `storage/sessionNamingPattern` | Composes the session-folder name (`date`/athlete/session-type tokens) in `SwingPaths::allocateSwingDir` |
| `storage/videoCodec` | Factory key (`"h264"` → libx264, `"h265"` → libx265; other → h264 fallback). `FfmpegVideoEncoder` |
| `storage/videoQuality` | CRF: `low`=28, `medium`=23, `high`=18, `lossless`=0 |
| `storage/videoContainer` | `mp4`/`mov`/`mkv` — sets the clip file extension; the muxer is guessed from it |
| `storage/videoResolutionMode` | `native`/`half`/`1080p`/`4k` — export-time downscale, never upscales |
| `storage/saveImuStreams` | Gates the IMU streams in `swing.json` |
| `storage/imuDataFormat` | `json` (inline) / `csv` / `binary` — csv/binary write an `imu_<alias>.<ext>` sidecar |
| `storage/saveRawFrames` | Dumps undecoded sensor payloads to an `<alias>.raw` sidecar (+ a `raw` block per video stream) |
| `storage/skipAnalysisForRawCapture` | With `saveRawFrames` on: skip analysis and the replay entirely, export frames only. Corpus-capture mode — each shot lands instantly and is re-analysed in bulk later (shot-analyzer guide §4/§9a) |
| `storage/savePoseKeypoints` | Gates serialising `kind:"pose"` streams — the exporter only *writes* pose carried on the job; no producer yet (empty today) |
| `general/motionCaptureQuality` | Recorded into `capture` so re-analysis picks the same offline pose model |
| `camera/alias` (`cameraAlias()` map) | MP4 filenames, keyed by `cameraKey()` |
| `imu/alias` (`imuAlias()` map) | IMU stream aliases, matched by serial/device-id |

All keys are read on the UI thread in `buildSwingExportJob()` via the single
shared `AppSettings` instance (see the AppSettings rule in `CLAUDE.md`).

---

## 10. Threading Model

**`QtConcurrent::run` + a `QFutureWatcher` value member** — not a `QThread`
worker. The save is a one-shot job returning a value, which is exactly QFuture's
shape; a `moveToThread` worker would add a class, lifetime management, and
queued-signal plumbing for zero benefit. (The `TtsController` QThread pattern is
for long-lived stateful services.)

Two properties of `QFutureWatcher` carry the safety argument:

1. `finished` is delivered **on the watcher's thread** (the UI thread), so all
   swing-lifecycle state is single-threaded and mutex-free.
2. `finished` cannot fire before the worker lambda has **fully returned**, so by
   the time `maybeFinishSwing()` destroys the window, no code can still be reading
   through it.

What runs where:

| UI thread | Worker thread |
|---|---|
| Window capture, replay | `SwingExporter::run()` |
| `buildSwingExportJob()` — all QSettings/controller access, alias resolution, directory creation | Zero-copy window reads, demosaic, encode, JSON build/write |
| Window destruction, resume decision | `ppInfo`/`ppWarn`/`ppError` (thread-safe) |

The worker uses only `const` methods of `SwingWindow` over frozen data, so it can
read concurrently with the replay's `payloadOf()` calls on the UI thread.

The job runs on the **global** QThreadPool. That is fine for one encode at a time;
if other QtConcurrent users ever appear, consider a dedicated single-thread pool.

---

## 11. Build System — FFmpeg Detection

FFmpeg is a **real link dependency** here (libavcodec, libavformat, libavutil,
libswscale via `pkg_check_modules(FFMPEG IMPORTED_TARGET …)`), distinct from the
dlsym-based log suppression in `pp_debug.cpp`, which targets Qt Multimedia's
*bundled* libavutil and needs no link. GPL builds of FFmpeg (libx264) are fine —
the project is GPLv2+.

- **macOS**: `brew --prefix ffmpeg` is prepended to `PKG_CONFIG_PATH` (mirrors the
  OpenCV handling).
- **Windows**: probes `C:/ffmpeg`, `C:/tools/ffmpeg`; override with
  `-DFFMPEG_DIR=`.
- **Linux**: distro dev packages (`libavcodec-dev libavformat-dev libavutil-dev
  libswscale-dev`).

`video_encoder.*`, `swing_paths.*`, `swing_exporter.*` are **always compiled**;
only `ffmpeg_video_encoder.*` and the `HAVE_FFMPEG` define are conditional. Without
FFmpeg the app builds and runs normally — `makeVideoEncoder()` returns `nullptr`
and each save fails fast with a logged "no encoder available".

### Coexistence with Qt's bundled FFmpeg

The process ends up with **two** FFmpeg generations loaded: ours from the system
(e.g. `libavutil.so.60`) and Qt Multimedia's bundled copy (e.g. `.so.59`, loaded
lazily by its media plugin). This is safe: the SONAMEs differ and FFmpeg exports
versioned symbols (`av_log@LIBAVUTIL_59` vs `…@LIBAVUTIL_60`), so the dynamic
linker keeps them apart. Two consequences:

- CMake's `PkgConfig::FFMPEG` imported target links by **absolute path** — never
  replace it with `-L`/`-l` flags, which could resolve against Qt's lib dir.
- `av_log_set_level()` must be called on **our** instance (the encoder does this
  in `open()`); the dlsym suppression in `pp_debug.cpp` only reaches Qt's copy.

---

## 12. Extending the Module

### Adding a new stream kind to `swing.json`

Append another object to `streams[]` with a new `kind` (e.g. `"pose"`,
`"metrics"`, `"launch"`). Keep the established shape: `kind`, `alias`, `source`,
a units/schema block, and parallel `t_us` + data arrays. Do **not** bump the
schema version for additive changes — readers ignore unknown kinds by contract.

### Bulk export — `SwingZipExporter`

The carousel's "export selected shots" action is a separate worker
(`swing_zip_exporter.{h,cpp}`, QML context property `swingExporter`), built on the
same discipline as `SwingExporter`: a self-contained value job, a stateless static
worker on `QtConcurrent`, the result delivered back on the UI thread via
`QFutureWatcher`.

- `camerasForShots(dirs)` reads each shot's `swing.json` and unions its
  `kind=="video"` streams, de-duped by file in first-seen order — that list is the
  model for the options sheet's camera checkboxes. An unreadable `swing.json` is
  skipped rather than fatal.
- `exportShots(dirs, selectedVideoFiles, includeJson)` builds `~/<session>.zip`.
  Extracting it reproduces the session: a top-level `<session>/` with one
  `swing_NNNN/` per shot. Each shot contributes `thumb.jpg` (always), the selected
  videos, and — only with the JSON toggle on — `swing.json` plus the IMU sidecars.

Two things it deliberately does *not* do. **Raw frame sidecars are never
included** — they are the largest artifact by far and useless outside this
machine. And **the archive is never held in memory**: it streams straight to the
on-disk file via `QZipWriter`'s filename constructor, so peak RAM is one member
at a time (bounded by one swing's MP4, since `*.raw` is excluded). A session can
be multiple GB; building the zip in a `QByteArray` would not survive it.

### Adding a new encoder backend (`h265`, ProRes, hardware)

1. Implement `IVideoEncoder` in a new `src/Export/<name>_encoder.{h,cpp}`.
2. Add its factory key in `video_encoder.cpp`.
3. Gate compilation in CMake the same way `ffmpeg_video_encoder` is gated.
The exporter and `CameraManager` need no changes — codec selection already flows
from `storage/videoCodec`.

### The frame-restoration hook

Denoise/sharpen goes in exactly one place — the marked hook between demosaic and
`writeBgr()` in `swing_exporter.cpp`. It receives the reused BGR scratch and must
write its result back into a BGR mat of the same dimensions. Record the stage in
`processing.restorer` (currently always `"none"`).

### Per-camera parallel encode

Possible future optimisation: the window is read-only and each camera writes its
own file, so cameras could encode in parallel at the cost of one BGR scratch + one
encoder per worker. Bound the pool and measure peak RSS first.

### Not yet handled (v1 scope)

12/16-bit Bayer, `MJPEG`/`H264_NAL` passthrough, an actual **pose producer**
(the exporter serialises `poseStreams` but nothing fills them yet), launch-monitor
streams, any UI beyond the two status signals.

---

## 13. Common Mistakes

### Resuming the buffer while a save is in flight

The worker reads ring memory zero-copy; `resume()` clears the rings — instant
use-after-free. All resume paths must go through `CameraManager::resumeBuffer()`,
which refuses while `m_swingWindow` is live. If you add a new resume path, route
it through `resumeBuffer()` — never call `m_eventBuffer->resume()` directly from
swing-adjacent code.

### Destroying `m_swingWindow` anywhere except the join

`stopReplay()` used to reset the window; it deliberately no longer does. If you
reintroduce a reset on the replay path, a save that outlives the replay will read
freed memory. The only legitimate places are `maybeFinishSwing()` and the
teardown paths that first call `m_swingSaveWatcher.waitForFinished()`.

### Recording a `t_us` entry for a skipped frame

If `payloadOf()` returns null data, skip the entry **entirely** — both the encode
and the timestamp. Recording the timestamp but not the frame (or vice versa)
breaks the *MP4 frame i == `t_us[i]`* invariant that all downstream tooling
relies on.

### Copying window payloads "to be safe"

The whole design exists to avoid this. The window is frozen — wrap the payload in
a `cv::Mat` header and demosaic straight into the reused scratch. If you find
yourself calling `.clone()` or building a `QByteArray` from a payload on this
path, stop.

### Touching `AppSettings` / controllers from the worker

Everything the worker needs is in the `SwingExportJob`. If a new feature needs
another setting, resolve it in `buildSwingExportJob()` on the UI thread and add a
field to the job struct.

### Removing the `pkt->duration` stamp

See §8 — without it every exported clip silently loses its final frame. The
failure is invisible to ffprobe's metadata (`nb_frames` still reports the full
count); only `-count_frames` or an actual decode reveals it.

### Re-deriving the Bayer mapping

The OpenCV `COLOR_Bayer*` constants do not match the sensor pattern names
one-to-one. The table in `frame_decode.h` mirrors the live-view mapping
deliberately — if exported colours ever diverge from the on-screen image, compare
against `camera_instance.cpp`'s PixelFormat→pattern switch before touching it.
And note the table is now **shared with the shot analyzer**: a change here
changes what analysis sees, not just what is written to disk. That is the point
of the extraction — the two can never diverge — but it means the blast radius is
wider than the file's name suggests.

### Adding an analysis input without persisting it

Offline re-analysis reads `swing.json` and never `AppSettings`, so anything that
affects analysis and lives only in settings makes a re-analysed swing disagree
with the live one — silently, with the same score field showing a different
number. Club geometry, the length prior, the quality tier, the IMU A/M snapshot,
the ball ROI and the empty-mat baseline are all in the file for exactly this
reason. Add yours to `SwingExportJob` and the manifest at the same time you add
it to `ShotAnalysisJob`.

### Blocking the UI thread on the watcher outside teardown

`waitForFinished()` is acceptable only where correctness demands it
(`setSelected()` deregistration, the destructor). Everywhere else the join is
asynchronous by design — a long save must not freeze the UI.

---

## 14. File Map

```
src/Export/
├── video_encoder.h             VideoEncoderConfig, IVideoEncoder (abstract),
│                               makeVideoEncoder() declaration. No libav includes.
├── video_encoder.cpp           Factory — the only file with HAVE_FFMPEG ifdefs
│
├── ffmpeg_video_encoder.h      Concrete encoder; libav types forward-declared
├── ffmpeg_video_encoder.cpp    libav call sequence, BT.709 sws, RAII cleanup,
│                               the pkt->duration fix (§8)
│
├── frame_decode.h              DemosaicPlan / frameGeometry / decodeFrame — the
├── frame_decode.cpp              ONE decode path, shared with the analyzer (§3)
│
├── swing_paths.h               SwingPaths — session/swing dir allocation + cache
├── swing_paths.cpp             sanitise(), per-app-run session policy
│
├── swing_exporter.h            SwingExportJob/Camera/Result, the stream structs,
├── swing_exporter.cpp            SwingExporter::run + captureBlock(); per-camera
│                                 encode loop, thumbnail, IMU/ball streams,
│                                 baseline sidecar, manifest builder
│
├── swing_doc.h                 SwingDocWriter/Reader — the unified swing.json,
├── swing_doc.cpp                 the review write-through, and the regenerable
│                                 swing_summary.json index sidecar (§6)
│
├── swing_zip_exporter.h        Bulk "export selected shots to a zip" (§12)
├── swing_zip_exporter.cpp
│
└── tests/
    └── swing_doc_test.cpp      Document round-trip; BUILT BY THE ANALYSIS SUITE

src/Gui/shot/
└── shot_processor.{h,cpp}      startSwingSave(), buildSwingExportJob(),
                                onSwingSaveFinished(), maybeJoin(), finishShot(),
                                finishNowBlocking() — the teardown stop-barrier

src/Buffer/
└── event_buffer.cpp            registerSource() normalises identifier →
                                FormatDescriptor::device_serial (§6 note)
```

### Verifying an export by hand

```bash
# Stream/colour/profile checks — expect h264 High, yuv420p, bt709×3, tv range, 30/1:
ffprobe -v error -show_streams <swing_dir>/<alias>.mp4 \
  | grep -E "codec_name|profile|pix_fmt|color_|r_frame_rate|nb_frames"

# The check that actually catches the last-frame bug — decoded count must equal
# nb_frames and the json frames.count:
ffprobe -v error -count_frames -select_streams v \
  -show_entries stream=nb_frames,nb_read_frames <swing_dir>/<alias>.mp4

# faststart: moov must precede mdat
python3 -c "d=open('<file>','rb').read(); print(d.find(b'moov') < d.find(b'mdat'))"
```

Cross-check `swing.json`: `frames.count` == decoded frame count per video stream,
`t_us` arrays non-negative and monotonic, IMU rows are 10 floats, and
`samples.count` == `imuSampleCount()` for each IMU source.

---

## 15. Testing

This module's automated coverage is the **`swing.json` document round-trip**. The
encoder and frame pipeline have no unit test — they need a live FFmpeg build and
real captured frames — and are validated by hand (see
[Verifying an export by hand](#verifying-an-export-by-hand) above).

### `swing_doc_test` — the unified document round-trip

`src/Export/tests/swing_doc_test.cpp` exercises `SwingDocWriter` / `SwingDocReader`
(§6) end to end against a temp directory:

- **Unified write** — `writeSwingJson(rawManifest, analysis)` composes the exporter's
  raw `pinpoint.swing/1` tree with a `SwingAnalysis` into one document, bumping the
  on-disk schema to **`pinpoint.swing/2`** and adding an inline `analysis` block
  (`pinpoint.analysis/2`: `score`, `tier`, metric `series` with `t_us`/`value`/
  `phaseSamples`, and `phases`). The raw `streams` / `swing` blocks survive untouched.
- **Reader** — `readSwingJson()` reconstructs a `PersistedShot` (ordinal, `hasVideo`,
  thumbnail path, `hh:mm:ss` from `clock.wallclock`, `score`, flat impact `metrics`,
  and `analysisDetail` for the replay graph) — the same shapes `ShotProcessor`
  produces live, so a reloaded shot is indistinguishable from a freshly captured one.
- **Review write-through** — `updateReview(rating, note)` lands an additive `review`
  block without disturbing the raw/analysis blocks, replaces (does not append) on
  rewrite, clamps the rating to 0–5, and fails harmlessly (returns `false`) when no
  `swing.json` exists.
- **Raw-only** — `writeSwingJson(…, nullptr)` omits the `analysis` block while still
  emitting `pinpoint.swing/2` (export-succeeded-but-analysis-failed degrades cleanly).

- **Summary sidecar** — `swing_summary.json` is written on a full read, reused on
  the next one, and **invalidated by the source document's size/mtime**. The test
  also asserts the cheap path was genuinely taken (`SwingSummary::fromSidecar`)
  and that an orphaned sidecar copied into a directory with no `swing.json` is
  rejected rather than trusted.

There is **no Export test project**. `swing_doc_test.cpp` lives in
`src/Export/tests/` but is compiled by the **Analysis suite**
(`src/Analysis/tests/CMakeLists.txt`) because it links `swing_analysis.h`:

```bash
cmake -S src/Analysis/tests -B build/analysis-tests
cmake --build build/analysis-tests -j6
ctest --test-dir build/analysis-tests -R swing_doc_test --output-on-failure
```

It is self-contained (own `main()` + `check()`, no GoogleTest). See
`docs/developer/testing_developer_guide.md` for the umbrella build and the
per-suite conventions, and the EventBuffer suite for the borrowed-window contract
this module relies on.

---

*For the EventBuffer contracts this module depends on — pause/resume semantics,
SwingWindow lifetime, zero-copy read safety — see
`docs/developer/event_buffer_developer_guide.md`, in particular §9 (SwingWindow) and §14
(Common Mistakes).*
