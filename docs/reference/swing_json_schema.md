# `swing.json` schema reference

> **Date:** 2026-07-13 · **Applies to:** the per-shot `swing.json` manifest written to each swing directory · **Schema:** top-level `pinpoint.swing/2`, embedded `analysis` block `pinpoint.analysis/3`

Every captured shot produces one directory (`<session>/swing_<NNNN>/`) containing the media and a single `swing.json` manifest. This document is the field-by-field reference. Snippets are from a real swing — `2026-07-05_Mark-Liversedge_Wrist_02/swing_0006` (a camera-only Wrist shot; "s06" in the shaft-lab corpus) — with IMU-specific blocks taken from an IMU swing where noted.

## Source of truth

| Concern | File |
|---|---|
| Manifest (raw) writer — `capture`, `streams`, `clock`, `window`, `swing`, `athlete`, `session`, `thumbnail` | `src/Export/swing_exporter.cpp` (`captureBlock`, stream serialisation) |
| `analysis` block writer | `src/Export/swing_doc.cpp` `serializeAnalysis()` |
| Unified write (`analysis` replaces only itself; the rest is preserved) | `src/Export/swing_doc.cpp` `SwingDocWriter::writeSwingJson()` |
| `review` block writer | `src/Export/swing_doc.cpp` `SwingDocWriter::updateReview()` |
| Readers | `SwingDocReader::readSwingJson` (swing_doc.cpp), `SwingDiskLoader::load` (swing_reanalyzer.cpp), `disk_replay_source.cpp`, `swing_data_source.cpp` |
| Enums (Phase, SegmentRole, ReconstructionTier, ShaftSampleFlags) | `src/Analysis/swing_analysis.h` |

**How it is written.** The export worker builds the *raw manifest* (`SwingExporter`) and, at the join, `SwingDocWriter::writeSwingJson()` merges the analyzer's output into it as the `analysis` object and writes the whole document. **Re-analysis rewrites only `analysis`** — `capture`/`streams`/`clock`/`window`/`review` are preserved verbatim.

**Additive by contract.** Readers ignore unknown keys, and every `analysis` sub-block is optional — a swing captured with analysis skipped (corpus capture) has no `analysis` object at all; a camera-only swing has no IMU streams and no `bindings`; an older file may lack newer blocks. Consumers must tolerate absence.

## Timestamp domains — read this first

All `t_us` values are **microseconds, window-relative** (0-based; `window.start_us` is `0`), matching `capture.impactUs`, the stream frame times, and the window bounds. The **one** absolute value is `clock.t0_us` — the EventBuffer clock instant of the window start. To recover an absolute timestamp: `absolute = clock.t0_us + t_us`.

> **Legacy caveat.** `analysis` timestamps written before 2026-07-07 may be **absolute** (a live capture wrote the EventBuffer clock domain directly; re-analysis always wrote relative). The writer now normalises all `analysis` `t_us` to window-relative regardless of source, and the review readers are domain-aware (`t >= t0 ? t - t0 : t`) so both old and new files render. New files are uniformly window-relative.

## Top-level structure

```json
{
  "schema": "pinpoint.swing/2",
  "clock":   { … },        // time base
  "window":  { … },        // captured span (window-relative bounds)
  "swing":   { … },        // id / index
  "session": { … },        // owning session dir
  "athlete": { … },        // who
  "capture": { … },        // shot setup + provenance (+ club geometry)
  "streams": [ … ],        // video + IMU streams
  "thumbnail": { … },      // impact still
  "review":  { … },        // OPTIONAL — user rating/note/club
  "launchMonitor": { … },  // OPTIONAL — a connected launch monitor's readings
  "analysis":{ … }         // OPTIONAL — the analyzed swing (pinpoint.analysis/3)
}
```

| Block | Required | Purpose |
|---|:---:|---|
| `schema` | ✓ | Document schema id, `pinpoint.swing/2`. |
| `clock` | ✓ | Absolute anchor (`t0_us`) + wallclock. |
| `window` | ✓ | Window-relative capture span. |
| `swing` | ✓ | Swing id + index within the session. |
| `session` | ✓ | Owning session folder. |
| `athlete` | ✓ | Athlete identity + handedness. |
| `capture` | ✓ | Session type, impact, latencies, host, club geometry. |
| `streams` | ✓ | One entry per camera / IMU. |
| `thumbnail` | ✓ | Impact-frame JPEG reference. |
| `review` | — | User rating/note/club (added by `updateReview`). |
| `launchMonitor` | — | A launch monitor's readings for this shot (added by `updateLaunchMonitor`). |
| `analysis` | — | The analyzed swing. Absent for analysis-skipped captures. |

---

## `clock`, `window`, `swing`, `session`, `athlete`

```json
"clock":   { "t0_us": 176400665083, "unit": "us", "wallclock": "2026-07-05T11:32:34.072Z" },
"window":  { "start_us": 0, "end_us": 5000000 },
"swing":   { "id": "swing_0006", "index": 6 },
"session": { "dir": "2026-07-05_Mark-Liversedge_Wrist_02" },
"athlete": { "handedness": "Right", "name": "Mark Liversedge",
             "uuid": "52ff45d8-37a6-4474-bdc6-5a4184f78387" }
```

| Field | Type | Notes |
|---|---|---|
| `clock.t0_us` | int µs | **Absolute** EventBuffer instant of the window start. The only absolute time in the file. |
| `clock.wallclock` | ISO-8601 | UTC wallclock snapshotted just after capture. |
| `window.start_us` / `end_us` | int µs | Window-relative span (`start_us` is always `0`). |
| `swing.id` / `index` | str / int | Folder name and 1-based index within the session. |
| `session.dir` | str | Session folder name. |
| `athlete.handedness` | `"Right"`\|`"Left"` | Drives lead-arm sign throughout analysis. |
| `athlete.uuid` | str | Athlete record key (QSettings). |

---

## `capture`

Shot setup + provenance. The `club` sub-block was added 2026-07-07 so re-analysis can recover the club's retro-band geometry (the shaft tracker's E1 band matcher).

```json
"capture": {
  "sessionType": 1,                 // 0 Swing · 1 Wrist · 2 GRF · 3 Coach
  "shotSource": 4,                  // ShotController::Source: 0 Manual·1 Imu·2 Pose·3 Ball·4 Acoustic
  "impactUs": 3481388,              // window-relative impact instant (-1 = unknown)
  "swingDetectionSensitivity": "Medium",
  "latencyUs": { "imuBle": 30000, "audioDevice": 20000 },
  "host": { "app": "PinPointStudio", "version": "0.1.10007", "gitSha": "cb5c646",
            "hostname": "GOLFSIMPC", "platform": "Windows 11 Version 25H2", "poseBackend": "CUDA" },
  "club": { "lengthMm": 940, "shaftType": "steel", "hoselFromButtMm": 0,
            "bandCentersMm": [308, 362, 560, 758, 808, 854],
            "name": "7 IRON",
            "lengthPrior": { "px": 372.4, "varPx": 96.1, "n": 5 } }
}
```

| Field | Type | Notes |
|---|---|---|
| `sessionType` | int | `SessionController::Type`. Selects the analyzer (only Wrist=1 is non-stub). |
| `impactUs` | int µs | Window-relative impact — the re-analysis impact reference (present even for analysis-skipped captures). |
| `swingDetectionSensitivity` | str | `Low`/`Medium`/`High`. |
| `latencyUs.*` | int µs | Detector back-dating constants at capture time. |
| `host.*` | str | App/build/machine provenance. `poseBackend` = CUDA/CoreML/CPU. |
| `club.lengthMm` | int | Shaft length; sizes the shaft-tracker head extrapolation. |
| `club.shaftType` | str | `steel`/`graphite`/`""`. |
| `club.hoselFromButtMm` | int | **Added 2026-07-09.** Hosel offset from the butt, mm — where the head sits relative to the grip end (`0` = unknown). Plumbed through `ShotAnalysisJob` and replayed on re-analysis. Absent on swings captured before 2026-07-09. |
| `club.bandCentersMm` | int[] | Retro-band centres from the butt. **Empty ⇒ untaped** → the shaft tracker runs E2 (ray) evidence only. Absent on swings captured before 2026-07-07. |
| `club.name` | str | Canonical club-vocabulary id — half the persistent club-length prior key (`athleteUuid\|clubName\|cameraKey`). Added 2026-07-10 (length fusion). |
| `club.lengthPrior` | obj | The persistent club-length prior **the live fuse actually used for this shot** (state before this shot's update): `px` (EMA length), `varPx` (EW variance, px²), `n` (updates folded in). **Re-analysis replays this recorded prior, never AppSettings** — deterministic, cross-host. Omitted when the shot ran prior-free. |

---

## `streams`

An array with one entry per camera or IMU. `kind` discriminates.

### Video stream (`kind: "video"`)

```json
{
  "kind": "video", "alias": "Face-On", "file": "Face-On.mp4",
  "encoded": { "width": 1280, "height": 1024 },
  "source":  { "serial": "17453937", "width": 1280, "height": 1024, "pixelFormat": "BayerRG8" },
  "raw": { "file": "Face-On.raw", "count": 746, "frameBytes": 1310720,
           "stride": 1280, "width": 1280, "height": 1024, "pixelFormat": "BayerRG8" },
  "frames": { "count": 746, "t_us": [2720, 9402, 16126, … ] },
  "capture": { "exposureAuto": true, "exposureSource": "measured",
               "exposureUs": 6573.56, "fps_num": 150713, "fps_den": 1000 },
  "playback": { "fps": 30 },
  "processing": { "demosaic": "EA", "restorer": "none" },
  "setup": { "perspective": 2, "perspectiveName": "FaceOn", "mirrored": false,
             "fixedInPlace": true,
             "ballDetection": { "calibrated": false, "margin": 0, "driftAtCapture": 0,
                                "calibratedAt": null, "searchRoi": [0.36, 0.72, 0.30, 0.24] } }
}
```

| Field | Type | Notes |
|---|---|---|
| `alias` | str | Human/UI name; also the media filename stem. |
| `file` | str | Encoded MP4 in the swing dir. |
| `encoded.width/height` | int | MP4 dimensions. |
| `source.*` | — | Sensor native dims + `pixelFormat` + camera `serial`. |
| `raw` | obj | **Optional** undecoded sensor sidecar (`<alias>.raw`, single concatenated blob). `frameBytes`/`stride`/`pixelFormat` describe its layout. Present when raw-frame saving is on. |
| `frames.t_us` | int[] µs | Window-relative per-frame capture times — the replay master clock and the domain analysis samples align to. `count` = frame total. |
| `capture.fps_num/den` | int | True frame rate (`fps_num/fps_den`) from clip metadata; the analysis timebase. |
| `capture.exposureUs` | float µs | Exposure — used by the shaft tracker's blur model. |
| `playback.fps` | int | Container playback rate only (casual scrub speed), **not** the analysis rate. |
| `processing.demosaic` | str | `EA` (edge-aware) / `bilinear` / `none`. |
| `setup.perspective` | int | `0` None · `1` DownTheLine · **`2` FaceOn** · `3` Other. `perspectiveName` is the label. The shaft tracker + body viz key on perspective 2. May be `null` on older exports. |
| `setup.mirrored` | bool | Webcam mirror flag (affects pose/shaft chirality). |
| `setup.fixedInPlace` | bool | Camera-fixed calibration state. |

**`setup.ballDetection`** — ball-detector provenance plus the search box offline re-analysis uses.

| Field | Type | Notes |
|---|---|---|
| `calibrated` | bool | Whether a v1 calibration profile was active. The v1 calibration stack is retired (temporal v2 detector), so now always `false`. |
| `margin` / `driftAtCapture` | float | Legacy v1 calibration diagnostics (`0` on the v2 path). |
| `calibratedAt` | ISO \| null | v1 calibration timestamp (`null` on v2). |
| `center` / `radiusNorm` / `positionSource` | float[2] / float / str | **Optional** — a stable calibrated ball position (full-frame normalized; radius to width). Present only when a calibrated position exists (`positionSource: "calibrated"`). |
| `searchRoi` | float[4] | **Added 2026-07-08.** The hitting-area box `[x, y, w, h]`, full-frame normalized — the region the live ball detector searched. Offline re-analysis (`BallRunner`) searches this box instead of the pose-derived stance corridor, so it matches live detection and skips out-of-box distractors (feet/shoes). **Omitted** when no hitting area is set; swings captured before 2026-07-08 lack it (re-analysis falls back to the stance corridor). |
| `baseline` | obj | **Added 2026-07-10.** The live detector's learned empty-mat baseline, snapshotted at seed time so offline re-analysis (`BallRunner`) reconstructs the exact baseline the session learned instead of self-seeding over the swing's opening frames (where the ball already sits, which bakes it into the subtracted baseline). `{ "file": "<alias>.ballbase.f32", "w": int, "h": int, "roi": [x,y,w,h], "rHat": float, "fps": float, "noise0": float }`. `file` names the raw float32 sidecar (see below); `w`/`h` are its ROI pixel dims; `roi` is the full-frame-normalized box `B` covers; `rHat` is the seed-time radius; `noise0` is the robust-noise fallback. `fps` is **provenance only** — the offline tracker re-measures its own pushed-frame rate. On re-analysis a valid baseline makes the `BallRunner` re-run authoritative: the recorded `ball` stream (known wrong-time-base) is dropped so it can't shadow the re-run. **Omitted** for swings captured before 2026-07-10, or when the ROI was reset mid-seed. |

### IMU stream (`kind: "imu"`)

From an IMU swing (`2026-06-11_…_Wrist_01/swing_0009`):

```json
{
  "kind": "imu", "alias": "Green Sensor", "schema": "imu_sample_v2",
  "source": { "serial": "C6:05:20:9D:04:76" },
  "units": { "accel": "g", "gyro": "deg/s", "quat": "wxyz" },
  "samples": {
    "count": 492,
    "t_us": [3878, 6587, 9308, … ],
    "data": [ [-0.984, -0.013, -0.102,  11.108, 2.746, -2.624,  0.474, 0.342, 0.615, -0.529], … ]
  }
}
```

| Field | Type | Notes |
|---|---|---|
| `schema` | str | `imu_sample_v2` — the per-sample row format. |
| `source.serial` | str | Device serial/MAC (stable across runs; binding key). |
| `units` | obj | `accel` g · `gyro` deg/s · `quat` wxyz. |
| `samples.t_us` | int[] µs | Window-relative sample times. |
| `samples.data` | float[][10] | Per sample: `[ax,ay,az, gx,gy,gz, qw,qx,qy,qz]` — accel (g), gyro (deg/s), host-fused orientation quaternion (wxyz). |
| `device` | obj | **Optional** (newer exports): `role` (SegmentRole int), `outputRateHz`, `fusionMode`, `orientationFilter`, `placementSlot`. Absent on older files → role falls back to the placement map. |

The quaternion is the **host-fused** world orientation PinPoint owns (Madgwick/ESKF over raw accel+gyro), not the device's on-board Euler output — see `docs/design/imu_frame_contract.md`.

---

## `thumbnail`, `review`

```json
"thumbnail": { "file": "thumb.jpg", "t_us": 3479416 },
"review":    { "rating": 0, "note": "", "club": "GAP WEDGE" }
```

| Field | Type | Notes |
|---|---|---|
| `thumbnail.file` / `t_us` | str / int µs | Impact-nearest JPEG (≤480 px) + its window-relative frame time. |
| `review.rating` | int 0–5 | 0 = unrated. |
| `review.note` | str | Free text. |
| `review.club` | str | User-chosen club label (distinct from `capture.club`). Whole block is **optional** — written only by `updateReview`. |

---

## `launchMonitor`

Written by `SwingDocWriter::updateLaunchMonitor` when a connected launch monitor reports the shot.
**Optional and written late** — the device writes its file after the shot, and `swing.json` is not
written until the analyzer and exporter both finish (12–37 s), so this block normally arrives by a
second, atomic rewrite rather than with the original document.

```json
"launchMonitor": {
  "kind": "gcquad",
  "deviceShotId": "283",
  "deviceClub": "Irn",
  "sourcePath": "/Volumes/FSX/LastShot.CSV",
  "readAtMs": 1785000000000,
  "clubheadSpeed": 87.19, "ballSpeed": 111.68, "smashFactor": 1.281,
  "carryDistance": 150.4, "spinRate": 7614, "faceAngle": 6.94, …
}
```

| Field | Type | Notes |
|---|---|---|
| `kind` | str | Which connector produced it (`"gcquad"`). Recorded, not assumed. |
| `deviceShotId` | str | The DEVICE's own shot counter — **not** a PinPoint shot id, and never treated as one. It is how the connector tells a fresh row from the same row restated. |
| `deviceClub` | str | The club the monitor believed was in use. Provenance only: PinPoint's own club selection resolves normative corridors, because these codes are coarser than our per-club contexts. |
| `sourcePath` / `readAtMs` | str / int ms | Where it came from and when we read it. |
| _readings_ | number | One key per value the device reported, **already converted into the metric catalogue's units** (mph, yd, ft, °, rpm, mm). A value the device did not report is absent, never zero. Includes columns that map to no metric, so the block is a full record of what was said. |

Every reading also appears in `analysis.metrics[]` under an `lm.`-prefixed key (`lm.clubheadSpeed`,
`lm.faceAngle`, …) as an empty curve carrying one `phaseSamples` entry at Impact — the same shape
every point-in-time metric uses. **The `lm.` prefix is the point, not a decoration**: six of these
quantities are ones PinPoint also estimates optically, and those keep their bare keys untouched
alongside. A launch monitor is a reference instrument to check our estimates against, so the
measurement must sit beside the estimate rather than replace it.

The metric entries are written only when the document already has an `analysis` block; the raw block
above is written regardless, so a shot whose analysis failed still keeps what the device said.

### Device-only swings

When `launchmonitor/standaloneShots` is on and nothing else is capturing, the reading creates the
whole document (`SwingDocWriter::writeDeviceOnlySwing`). It is not a degraded capture — it is a
different document, and it says so rather than leaving blanks that read as failure:

| Block | In a device-only swing |
|---|---|
| `streams` | **`[]`** — the fact, not a failure. `hasVideo` derives from it, so the picker and carousel already read it right. |
| `thumbnail` | **absent** — an empty block would point at a file never written. |
| `window` | zero-length. There was nothing to capture, and a fabricated span would put a scrubber on a shot with no frames. |
| `capture.shotSource` | `"launchMonitor"`, so a reader never infers it from the absence of streams. |
| `capture.impactUs` | **`-1`** — the device says a ball was struck, never when. |
| `analysis` | `metrics[]` (the `lm.*` readings) and `phases[]` (one entry) and **nothing else** — no `score`, no `pose2d`, no `club`, no `bindings`. Omitting them is what stops this being mistaken for an analysed swing whose analysis came out empty. |

**The Impact phase event is manufactured, and it has to be.** Every `lm.*` measure reduces `at p7`,
and `buildPhaseGrid` returns an empty grid when `phases[]` is empty — *"unsegmented: no phase to read
anything at, so nothing is producible"* — so without it the readings would persist and then resolve
to nothing at all. Its `conf` is `1.0` because the **event** is certain: a ball was struck and the
device measured it. What is unknown is the *instant*, and that is carried by `capture.impactUs = -1`,
not by pretending the timestamp is good.

---

## `analysis` (`pinpoint.analysis/3`)

The analyzed swing. Every sub-block is additive. `tier` records the reconstruction quality; camera-only swings are `0` (Angles2D), IMU-fused are `1` (Mono3DPlusImu).

```json
"analysis": {
  "schema": "pinpoint.analysis/3",
  "tier": 0,
  "score": { … },
  "metrics": [ … ],
  "phases": [ … ],
  "segmentation": { … },
  "pose2d": { … },
  "club": { … },
  "ball": { … },          // OPTIONAL (face-on ball track — replay overlay)
  "bindings": [ … ],      // IMU only
  "assessment": { … },    // OPTIONAL (Wrist coach feed)
  "filter": { … },        // OPTIONAL (offline re-fusion diagnostic)
  "timings": { … }        // OPTIONAL (per-stage analyzer wall times)
}
```

`tier` ∈ `ReconstructionTier`: `0` Angles2D · `1` Mono3DPlusImu · `2` Stereo3D · `3` ClubInstrumented.

### `score` (ScoreBreakdown, /3)

```json
"score": { "overall": 0, "kind": "resemblance", "pattern": "unknown",
           "blended": false, "resemblance": { … }, "interval": { … } }
```

`kind` is `resemblance` (Wrist) or `adherence` (Swing/GRF). `overall` is 0–100.

> **Version note.** In `pinpoint.analysis/2`, `score` was a bare integer (e.g. `"score": 6`). `/3` promotes it to this object (design §B.0a/§B.7). Readers handle both.

### `metrics[]`

Per-metric time series + phase-anchored samples.

```json
{
  "key": "impactShaftLean", "label": "Shaft lean", "unit": "°",
  "t_us":  [2720, 9402, 16126, … ],
  "value": [26.0, 26.0, 26.0, … ],
  "phaseSamples": [ { "phase": 5, "t_us": 3479416, "value": 32.0, "band": "" } ]
}
```

| Field | Type | Notes |
|---|---|---|
| `key` / `label` / `unit` | str | Metric id, display label, unit. Wrist keys: `leadWristFlexExt`, `leadWristRadUln`, `forearmPronation`, `leadArmFlexion`, `impactShaftLean`. Head keys (WB2, added 2026-07-13): `headSway`, `headLift` (unit `×frame` = fraction of frame width, isotropic; Address-referenced), `headTilt` (unit `°`, eye-line angle Δ from address). Foot keys (WB3, added 2026-07-13): `stanceWidth` (unit `×frame`, heel-to-heel, isotropic), `leadFootFlare` / `trailFootFlare` (unit `°`, that foot's heel→bigtoe vs image +x), `toeLineAngle` (unit `°`, lead-bigtoe→trail-bigtoe vs image +x) — these four are **address-only scalars**: `t_us`/`value` are **empty arrays** and the single measurement lives in `phaseSamples[0]` (`phase: 0` Address) — see the note below. `leadHeelLift` (unit `×frame`, Address-referenced heel-vs-toe elevation, `+` = heel lifts) is a normal per-frame curve like the head keys. **Face-on producer batch (2026-08-02):** lower body `leadKneeDrift` / `pelvisSway` / `pelvisLift` (`% stance width`), `hipLineTilt` / `feetAlignment` (`°`), `comOverLeadFoot` (`% stance width`, and the ONLY key that samples `Phase::Finish`); upper body `secondaryAxisTilt` / `spineSideBend` / `shoulderPlaneAngle` / `elbowAlignment` / `leadArmToTorso` (`°`), `thoraxLateralDrift` (`% stance width`), `trailElbowHeight` / `leadUpperArmToChest` (`% shoulder width`), `leadHandWidth` (`% arm length`); rotation `pelvisRotation` / `thoraxRotation` / `xFactor` / `xFactorStretch` (`°`, UNSIGNED magnitudes of turn from address — positive at the top AND at impact); club `shaftAngleVsHorizontal` (`°`, per-frame) plus the Impact-only scalars `attackAngle` (`°`) and `lowPointAhead` (`in`), both empty-curve + one `phaseSamples[0]` at `phase: 5` (Impact) rather than Address; and `trailWristFlexExt` (`°`, apparent camera-plane angle, EXTENSION-positive — the opposite polarity to the lead wrist, see `pinpoint_sign_conventions.md`). A rotation key may carry `sigma` (1σ, degrees) when it was estimated from the camera rather than measured from a trunk IMU; absent means not characterised, NOT zero error. UNSCORED (no `phaseSamples[].band`). Readers must not assume a fixed key set. |
| `t_us` / `value` | int[] / float[] | Parallel arrays; window-relative times. **May both be empty** — see `stanceWidth`/`leadFootFlare`/`trailFootFlare`/`toeLineAngle` above: a metric that is one address-time measurement rather than a curve carries it solely in `phaseSamples[0]`. No dedicated "scalar metric" shape exists elsewhere in this schema (`score.metrics`-style audit trails are never serialized), so this reuses the existing `metrics[]` contract as-is — every writer/reader already loops `t_us`/`value`/`phaseSamples` independently with no non-empty-curve assumption. |
| `phaseSamples[]` | obj[] | Value sampled at each phase: `phase` (Phase int), `t_us`, `value`, `band` (coaching-band label, may be `""`). |
| `sigma` | float | **Optional** (added 2026-07-21). 1σ *measurement* uncertainty on this metric's value, in the metric's own unit — the separate track from the coaching band. **Absent means "not characterised", NOT "zero error"**: only producers that actually propagated an error budget write it, so every metric predating the field serialises exactly as before. Today only `tempoRatio` / `tempoBackswing` carry it. Confidence widens this; it never moves `value`. |

### `phases[]` — the phase ladder

```json
"phases": [ { "phase": 0, "t_us": 2809565, "conf": 0.5, "segment": 0 },   // Address
            { "phase": 2, "t_us": 3238327, "conf": 0.5, "segment": 0 },   // Top
            { "phase": 5, "t_us": 3479416, "conf": 0.5, "segment": 0 },   // Impact
            { "phase": 7, "t_us": 3767410, "conf": 0.5, "segment": 0 } ]  // Finish
```

| Field | Type | Notes |
|---|---|---|
| `phase` | int (`Phase`) | See table below. |
| `t_us` | int µs | Window-relative event time. |
| `conf` | float 0–1 | Detection confidence; low-conf ticks fade in the UI. IMU-derived ≈ high; the vision-only fallback emits a flat 0.5. |
| `segment` | int (`SegmentRole`) | Which segment the event was measured from (provenance). |

**`Phase`:** `0` Address · `1` Takeaway · `2` Top · `3` Transition · `4` Downswing · `5` Impact · `6` Release · `7` Finish · `8` MidBackswing · `9` Delivery · `10` MaxSpeed · `11` FollowThrough · `13` ArmParallelDown (P5) · `14` ShaftParallelThrough (P8). `13` is emitted from the lead-forearm parallel crossing; `14` is emitted as a forearm PROXY for shaft-parallel-through, so its `conf` is capped ≤0.4 until a shaft-measured P8 lands. `12` ShaftParallelBack (P2) is still **not emitted** into `phases[]` — the P-positions (including P2) live in `analysis.club.positions[]` instead.

> An IMU swing emits the full ladder; a **camera-only** swing emits only the four anchors `{Address, Top, Impact, Finish}` at vision-grade confidence (the shaft tracker's hands-only phase model — same enum, fewer ticks).

### `segmentation`

```json
"segmentation": { "swingStartUs": 2709565, "swingEndUs": 3867410, "conf": 0.5, "version": 2 }
```

Swing bounds consumers truncate to (replay span, metric grids, heavy-stage scan). `conf == 0` means "bounds are just the window". Absent block ⇒ full-window bounds.

### `pose2d`

Offline pose keypoints, normalized 0..1. **Widened 2026-07-13 (WB0, additive):** `kp` grew `float[51]` → `float[399]` — 133 COCO-WholeBody keypoints. Indices 0–16 are the **unchanged** COCO body joints (identical values to a pre-WB0 file); the tail is purely additive: **17–22 feet** (L bigtoe/smalltoe/heel, R bigtoe/smalltoe/heel), **23–90 face** (68-pt contour), **91–111 left hand** / **112–132 right hand** (21 each: wrist root, then 4 per finger thumb→pinky). `smoothed[].kp`/`tier`/`sigma` widened identically; new `keypointCount` field. Readers use bounded loops, so old 51-float files and new 399-float files load interchangeably (old files leave the tail defaulted).

```json
"pose2d": {
  "camera": 0,
  "keypointCount": 133,
  "decode": "dark",                                                   // OPTIONAL — WB1 provenance
  "cropRect": { "x": 0.34, "y": 0.11, "w": 0.33, "h": 0.78 },         // OPTIONAL — WB1 person crop
  "frames": [ {
    "t_us": 2720,
    "kp":   [0.5469, 0.2930, 0.9349,  0.5573, 0.2852, 0.9591,  … ],   // 133×(x,y,conf) = 399
    "lead":  [0.5599, 0.6012], "trail": [0.5399, 0.6031],
    "handConf": 0.7535
  }, … ],
  "smoothed": [ {                                                     // OPTIONAL — motion-overlay track
    "t_us": 2720,
    "kp":    [0.5471, 0.2931, 0.9349,  0.5574, 0.2853, 0.9591,  … ],  // 133×(x,y,conf) = 399
    "tier":  [2, 2, 2,  1, 0,  … ],                                   // PoseTier per kp (133)
    "sigma": [1.8, 1.8, 2.4,  3.1, 0.0,  … ]                          // posterior σ px (133)
  }, … ]
}
```

| Field | Type | Notes |
|---|---|---|
| `camera` | int | Source id of the pose camera. |
| `keypointCount` | int | **Added 2026-07-13 (WB0).** Explicit `kp` width: `133` (COCO-WholeBody). Absent on older files ⇒ `17`. Provenance, not a parse contract — readers stay bounded-loop. |
| `decode` | string | **Optional — added 2026-07-13 (WB1).** Sub-pixel decode used for the offline pose pass: `"dark"` (DARK distribution-aware refinement). Written **only when non-legacy** — an argmax (`±0.25`-shift) run **omits** the field, so absent ⇒ `"argmax"`. Provenance only. |
| `cropRect` | obj | **Optional — added 2026-07-13 (WB1).** The swing-level person crop the pose pass ran inference on, `{x, y, w, h}` in **full-frame normalized** coords. **Absent** ⇒ the full-frame fallback ran (bbox too sparse / no resolution gain / crop disabled). Provenance/inspection only — all persisted `kp` are already full-frame normalized (back-projected through this crop). |
| `frames[].t_us` | int µs | Window-relative; pose is adaptively sampled (dense near impact), so frames < camera frames. |
| `frames[].kp` | float[399] | 133 COCO-WholeBody keypoints as flat `[x, y, conf] × 133`, normalized. Indices 0–16 = the unchanged COCO body joints; tail = feet/face/hands per the layout above. `float[51]` (17 kp) before 2026-07-13. |
| `frames[].lead` / `trail` | float[2] | Lead/trail hand centroids (COCO wrists on fallback), normalized. Unchanged by WB0 — still the derived grip anchors all consumers use. |
| `frames[].handConf` | float | 0 when wrist-fallback. |
| `smoothed[]` | obj[] | **Optional — added 2026-07-12** (motion-overlay fan/trace track). De-jittered companion to `frames`, produced by one offline RTS pass (`pose_smoother.{h,cpp}`, a per-axis 3-state `[p,v,a]` KF) in `WristAnalyzer`. Written **only when non-empty** (absent on swings analysed before the smoother existed ⇒ old swings must be re-analysed to gain fan/trace; `frame`-mode overlays fall back to raw `frames`). Parallel to `frames` on the **same `t_us` grid**; **no `lead`/`trail`/`handConf`** — the hands are not smoothed. |
| `smoothed[].t_us` | int µs | Window-relative (matches the aligned `frames` entry). |
| `smoothed[].kp` | float[399] | Smoothed 133 COCO-WholeBody keypoints, flat `[x, y, conf] × 133`, normalized (widened from `[51]` 2026-07-13). `conf` carries the overlay **render-alpha** contract, not the raw detector score. |
| `smoothed[].tier` | int[133] | Per-keypoint honesty `PoseTier`: **`0` Off** (raw passthrough — don't paint as confident) · **`1` Pred** (coasted/bridged estimate) · **`2` Meas** (measured & smoothed). Widened from `[17]` 2026-07-13. |
| `smoothed[].sigma` | float[133] | Per-keypoint posterior σ in **pixels**; `0` ⇒ no smoothed value for that keypoint that frame (the `kp` entry is a raw passthrough). Widened from `[17]` 2026-07-13. |

### `club` — the shaft track (`ShaftTrack2D`)

Written **only when the track is valid** (all-or-nothing consumer contract). Grip/head are normalized by the camera dims.

```json
"club": {
  "camera": 0, "valid": true, "coverage": 0.910,
  "imuVisionCorr": 0, "modelVisionResidualDeg": -1,
  "frameWidth": 1280, "frameHeight": 1024,
  "lengths": {
    "ballPx": 361.2, "bandPx": 377.8, "headP95Px": -1, "posePx": 248.5, "priorPx": 372.4,
    "fusedPx": 371.9, "fusedSigmaPx": 9.8, "fusedConf": 0.71,
    "fusedInstantPx": 370.2, "fusedInstantConf": 0.63,
    "ladderRung": 0, "ladderLenPx": 371.9, "nEstimators": 2, "priorN": 5, "headMeasN": 0
  },
  "samples": [ {
    "t_us": 2720,
    "grip":  [0.5499, 0.6021], "head": [0.4421, 0.8784],
    "theta": 2.0246, "thetaDot": 0.0, "lenPx": 0,
    "conf": 0.30, "headConf": 0.0, "headSigma": -1, "flags": 20
  }, … ],
  "predicted": [ … ]
}
```

| Field | Type | Notes |
|---|---|---|
| `valid` | bool | Coverage gate; consumers must check before use. |
| `coverage` | float | Fraction of span frames with a direct (band/ray) measurement. |
| `frameWidth/Height` | int | Camera dims — de-normalize `grip`/`head` by these. |
| `samples[].t_us` | int µs | Window-relative. |
| `samples[].grip` / `head` | float[2] | Normalized grip anchor and clubhead terminus. |
| `samples[].theta` | float rad | Shaft direction grip→head (image atan2 convention). |
| `samples[].thetaDot` | float rad/s | Smoothed angular velocity. |
| `samples[].lenPx` | float | Visible shaft extent (decoration; θ is the precision channel). |
| `samples[].conf` | float 0–1 | Per-sample confidence (θ-posterior). |
| `samples[].headConf` | float 0–1 | **Added 2026-07-09** (clubhead Stage-2 head pass). Confidence of the *measured* clubhead terminus; `0` when the head is projected/off-frame (see flags `0x10`/`0x80`) rather than measured. Default ON (`shaft.head.enabled`) since 2026-07-09; older files omit it ⇒ reader defaults `−1`. |
| `samples[].headSigma` | float px | **Added 2026-07-09.** Posterior σ (px) of the measured head radius (`headSigmaPx`); `−1` when the head pass is off or the head was not measured. |
| `samples[].lineConf` | float 0–1 | **Layer A snap** (`shaft_position_first`): normalized ridge support under the drawn line — "does this line lie on the club". Written only on vision-tier samples when the snap pass ran; **absent ⇒ −1** (snap off / non-vision tier). Added 2026-07-11. |
| `samples[].flags` | int (`ShaftSampleFlags`) | Bitfield (below). |
| `predicted[]` | obj[] | R7 pure-kinematic-model series (same shape); empty in v3. |
| `synth[]` | obj[] | **Layer C synthesized series** (`shaft_position_first` §2 Layer C, added 2026-07-11). One VISUALIZATION-tier sample per camera frame **strictly between** consecutive `positions[]` anchors — C¹ Hermite-interpolated (θ monotone-safe, grip cubic, length linear, conf decaying toward the span midpoint). Same normalized shape as `samples` **minus** `lineConf`; every entry carries `flags` bit `0x100` (`ShaftSynthesized`). **EXCLUDED from all metrics/scoring/estimands** — consumers must filter on the flag (same discipline as `KinematicPredicted`). **Written only when non-empty** (synthesis off / < 2 anchors ⇒ absent, block byte-identical). |
| `positions[]` | obj[] | **Coaching P-positions P1–P8** (`shaft_position_first` §2 Layer B, added 2026-07-11). One object per located position: `p` (1..8 coaching P-index), `t_us` (window-relative, sub-frame located), `grip`/`head` (normalized, same convention as `samples`), `theta` (rad, grip→head), `lenPx` (drawn grip→head length px), `conf`, `sigmaThetaDeg`/`sigmaLenPx` (fit posterior σ; **−1 in B1** — track-sampled, not yet fitted), `stackN` (shift-and-stack frame count; `0` in B1), `source` (`0` TrackSample / `1` MilestoneFit). P1/P4/P7 are the segmentation address/top/impact landmarks; **P2/P6/P8 are IMAGE-PLANE shaft-parallel crossings** (not true 3-D parallel — accepted face-on coaching practice, design §1); P3/P5 are lead-arm-parallel crossings. **Written only when non-empty** (extraction off / pre-v3.5 ⇒ absent, block byte-identical); missing crossings (abbreviated swings, absent φ) simply omit that P — read per-P coverage as `positions.length`/8. |
| `lengths` | obj | Multi-estimator club-length fusion (`club_length_fusion.h`), added 2026-07-10. Component estimates in px (`ballPx` grip→ball, `bandPx` band-scale, `headP95Px` Stage-2 head p95, `posePx` pose rung — sanity bound only, never fused; `-1` = estimator absent); `priorPx` = recorded prior; `fusedPx`/`fusedSigmaPx`/`fusedConf` = with-prior posterior; `fusedInstantPx`/`fusedInstantConf` = prior-free variant (the only value folded back into the prior — no self-reinforcement); `ladderRung`/`ladderLenPx` = what the length ladder actually used (rung 0 = fused); `nEstimators`/`priorN`/`headMeasN` = support counts. **Always written** (unlike its parent `club` block's validity gate — parity writers keep it even on abstain, `fusedPx < 0` ⇒ absent). |

**`ShaftSampleFlags`** (bitwise): `0x01` Measured · `0x02` ImuBridged · `0x04` Coasted · `0x08` Wedge · `0x10` HeadProjected · `0x20` KinematicPredicted · `0x40` BallAnchored · `0x80` HeadOffFrame · `0x100` **Synthesized** (Layer C, `synth[]` only — never in `samples[]`; excluded from metrics). E.g. `20` = `0x14` = HeadProjected|Coasted (a `pred`-tier frame); `17` = `0x11` = HeadProjected|Measured (a `ray`-tier frame); `1` = Measured with a real head (a `band`-tier frame). The C++ enum widened `uint8_t → uint16_t` at Layer C (2026-07-11) to make room for `0x100`; JSON has always carried `flags` as int, so this is a pure-recompile change with no schema/reader break.

### `ball` — the ball track (`BallTrack2D`)

Per-frame face-on ball position, drawn by the replay ball overlay (mirrors the live green ball circle). Added 2026-07-08. Coordinates are normalized 0..1 **full-frame** (same convention as `pose2d`; no `frameWidth`/`frameHeight` needed — the radius is normalized to frame width). Resolved from the live accumulator, a recorded raw `ball` stream, or the offline `BallRunner` (`WristAnalyzer`). Absent block ⇒ no ball data.

```json
"ball": {
  "camera": 0, "valid": true, "launchTUs": -1,
  "samples": [
    { "t_us": 620068, "found": true,  "x": 0.5484, "y": 0.8072, "r": 0.0074, "conf": 1.0 },
    { "t_us": 626700, "found": false, "x": 0,      "y": 0,      "r": 0,      "conf": 0.0 }
  ]
}
```

| Field | Type | Notes |
|---|---|---|
| `camera` | int | Source id of the ball (face-on) camera. |
| `valid` | bool | `true` when written; an absent block ⇒ no ball data. |
| `launchTUs` | int µs | Window-relative launch (collapse-cliff) instant; `-1` = no launch observed. |
| `samples[].t_us` | int µs | Window-relative frame time. |
| `samples[].found` | bool | Ball detected this frame. `found: false` frames still carry a `t_us` (gap marker) with zeroed position; the overlay draws nothing on them, so the circle vanishes at launch. |
| `samples[].x` / `y` | float | Ball centre, normalized 0..1 full-frame. `0` when `found` is false. |
| `samples[].r` | float | Ball radius, normalized to frame width. |
| `samples[].conf` | float 0–1 | Detection confidence. |

> The same per-frame track may also appear as a **raw `kind: "ball"` stream** (schema `ball_v2`, `layout: "found,x,y,r,conf"`, with a `frames{t_us,data}` block and optional `launch{t_us,x,y}`) — the offline re-analysis input, distinct from this analyzed `analysis.ball` block. Rare in practice; consumers that read `analysis.*` can ignore it.

### `bindings[]` (IMU only)

Per-device calibration snapshot (serial-keyed), so the offline runner can re-fuse with the exact anatomical transforms the app used.

```json
"bindings": [ {
  "serial": "WT901-1234", "role": 6, "roleName": "LeadHand",
  "alignA": [1,0,0,0], "mountM": [0.5,-0.5,-0.5,-0.5],
  "calibrated": true, "anatCalibrated": true,
  "mountDeviationDeg": 3.2, "mountGravityErrorDeg": 5.1,
  "calibratedAt": "2026-06-11T09:12:00.123Z", "calibAgeSec": 412.5
} ]
```

`role` is a `SegmentRole` int (`0` Unknown · `1` Pelvis · `2` Thorax · `3` T12 · `4` LeadUpperArm · `5` LeadForearm · `6` LeadHand · `7` TrailThigh · `8` LeadThigh · `9` Club). `alignA`/`mountM` are wxyz quaternions.

### `assessment` (optional) / `filter` (optional)

`assessment` = the AI-coach feedback feed (`scoreV2` + `findings[]`), written on live Wrist shots and the SwingLab known-groups input. `filter` = orientation-filter quality (`impactStepDeg`), present only when offline re-fusion drove the orientation. Both absent on this swing.

### `timings` (optional)

Per-stage analyzer wall times in milliseconds, self-reported by the analyzer so every live shot measures the < 20 s pipeline budget (the same numbers are echoed in SwingLab's `runmeta.json`). Written only when the total was measured; a stage that did not run stays `-1`.

```json
"timings": { "poseMs": 5480, "ballMs": 120, "shaftMs": 2900, "totalMs": 8600 }
```

| Field | Meaning |
|---|---|
| `poseMs` | Offline pose pass (`PoseRunner::run` / `loadFromJson`). `-1` when no camera ran. |
| `ballMs` | Face-on ball-track resolution (`BallRunner`, or an injected/recorded track). `-1` when the pose pass produced no frames. |
| `shaftMs` | Shaft track (`ShaftTracker::track`, incl. the additive ball-anchor pass). `-1` when the pose pass produced no frames. |
| `totalMs` | Whole `analyze()` wall time. Block present ⇒ this is `≥ 0`. |

---

## Companion files in the swing directory

| File | When |
|---|---|
| `<alias>.mp4` | Always — encoded video per camera. |
| `<alias>.raw` | When raw-frame saving is on — undecoded sensor sidecar (see `streams[].raw`). |
| `thumb.jpg` | Always — impact thumbnail. |
| `swing_summary.json` | Regenerable cache of the session-picker scalars — a few hundred bytes (see below). Written whenever `swing.json` is written or first read; **safe to delete**. |
| `imu_<alias>.csv` / `.bin` | When `imuDataFormat` ≠ `json` (otherwise IMU is inline in `streams`). |
| `<alias>.ballbase.f32` | When the face-on ball detector had a learned empty-mat baseline at export (see `streams[].setup.ballDetection.baseline`). Raw row-major `float32`, `w*h*4` bytes — the live detector's learned empty-mat DoG-response baseline `B` over the search ROI at seed time. Loaded by `BallRunner` on re-analysis; `fps` in the JSON block is provenance only. |
| `truth.json` | Markup/annotation ground truth (shaft-lab / markup lab), separate from `swing.json`. |

The **full per-swing / per-session folder layout** — including which of the above are app-written vs tooling artifacts — is documented in [`swing_folder_layout.md`](swing_folder_layout.md).

### `swing_summary.json` — the picker sidecar

A lean, regenerable cache of just the scalars the session picker needs, so opening the "choose a session" drawer never has to read a multi-MB `swing.json` (analysis blocks can run to tens of MB — `analysis.pose2d` alone is ~13 MB on a Wrist swing). Schema `pinpoint.swingsummary/1`.

```json
{
  "schema": "pinpoint.swingsummary/1",
  "source": { "size": 32710082, "mtime_ms": 1784667323792 },
  "ordinal": 1, "timestampLabel": "19:31:49", "wallclockMs": 1783708309065,
  "club": "DRIVER", "hasVideo": true, "thumbnailFile": "thumb.jpg", "score": 0
}
```

| Field | Type | Notes |
|---|---|---|
| `schema` | str | `pinpoint.swingsummary/1`. Any other value ⇒ treated as a miss (re-derived), so forward-compat is free. |
| `source.size` / `mtime_ms` | int | Size (bytes) and mtime (epoch ms) of the `swing.json` this was derived from. **Both** must match a fresh `stat()` or the sidecar is stale and re-derived. |
| `ordinal` | int | `swing.index`. |
| `timestampLabel` | str | Display `hh:mm:ss`; readers re-derive it from `wallclockMs` (absolute), so a library carried across timezones shows local times, not the indexer's. |
| `wallclockMs` | int | Absolute instant (epoch ms) from `clock.wallclock`; `0` = unknown. |
| `club` | str | Resolved club (`review.club`, else the `"DRIVER"` stub) — matches `readSwingJson`'s resolution exactly. |
| `hasVideo` | bool | True when any `streams[]` entry is `kind: "video"`. |
| `thumbnailFile` | str | Thumbnail **file name** (relative), resolved against the swing dir; empty when the doc has no `thumbnail` block. |
| `score` | int | Resolved overall score (`analysis.score.overall` in /3, or the bare int in /2). |

**Lifecycle.** Written by `SwingDocReader::writeSwingSummary` — inline at the end of `SwingDocWriter::writeSwingJson()` / `updateReview()` (so a newly captured, re-analysed, or re-rated swing indexes itself), and self-healingly on any read that finds the sidecar missing or stale (`readSwingSummary`). It is **pure cache**: an orphan (sidecar present, `swing.json` gone) is never trusted, and `find … -name swing_summary.json -delete` is always safe. It is **not** swept into session zip exports (the exporter matches `swing.json` by exact name). Source of truth: `src/Export/swing_doc.cpp`.

## Schema version history

| Version | Change |
|---|---|
| `pinpoint.swing/2` | Current document schema. |
| `pinpoint.analysis/3` | `score` promoted from bare int → `ScoreBreakdown` object (design §B.0a/§B.7). |
| `pinpoint.analysis/2` | `score` was a bare integer. Readers still accept it. |
| 2026-07-07 | `capture.club` added; all `analysis` `t_us` normalised to window-relative (readers domain-aware for legacy absolute files). |
| 2026-07-08 | `analysis.ball` added (face-on ball track for the replay overlay); `setup.ballDetection.searchRoi` added (hitting-area box for offline re-analysis). Both additive — no schema-version bump. |
| 2026-07-09 | Clubhead Stage-2 head pass: `capture.club.hoselFromButtMm` + `analysis.club.samples[].headConf`/`headSigma` (measured-head confidence + posterior σ) added, and `ShaftSampleFlags` gained `0x80` `HeadOffFrame`. The head pass (`shaft.head.enabled`) defaults ON from this date, so new Wrist swings carry real values. Additive — no schema-version bump. |
| 2026-07-09 | `analysis.timings` added (per-stage analyzer wall times: `poseMs`/`ballMs`/`shaftMs`/`totalMs`). Additive — no schema-version bump. |
| 2026-07-10 | `setup.ballDetection.baseline` object + `<alias>.ballbase.f32` sidecar added (persisted live empty-mat ball baseline for offline re-analysis). Additive — no schema-version bump. |
| 2026-07-10 | Club-length fusion: `capture.club.name` + `capture.club.lengthPrior` (recorded prior for deterministic re-analysis) and `analysis.club.lengths` (fused length ± σ + confidence) added. Additive — no schema-version bump. |
| 2026-07-11 | Shaft Layer A snap: `analysis.club.samples[].lineConf` (ridge support under the drawn line) added. Written only on vision-tier samples when the snap pass ran (`absent ⇒ −1`), so a snap-off run stays byte-identical. Additive — no schema-version bump. |
| 2026-07-11 | Shaft Layer B positions: `analysis.club.positions[]` (coaching P-positions P1–P8, `shaft_position_first`) added — P2/P6/P8 are image-plane shaft-parallel crossings (accepted face-on coaching practice, not 3-D geometry). Written only when non-empty (extraction off ⇒ absent, byte-identical). Additive — no schema-version bump. |
| 2026-07-11 | Shaft Layer C synthesis: `analysis.club.synth[]` (VISUALIZATION-tier interpolated series between P-anchors, `shaft_position_first`) added, and `ShaftSampleFlags` gained `0x100` `Synthesized` (C++ enum widened `uint8_t → uint16_t`; JSON already int, no reader break). Synthesized samples are excluded from all metrics/scoring by the flag. Written only when non-empty (synthesis off ⇒ absent, byte-identical). Additive — no schema-version bump. Also: `positions.enabled`/`fitEnabled` flipped default ON, so new Wrist swings carry `analysis.club.positions[]`. |
| 2026-07-12 | Motion overlay: `analysis.pose2d.smoothed[]` (de-jittered pose companion track for the fan/trace overlays, `pose_smoother.{h,cpp}`) added — parallel to `frames` with per-keypoint `tier`/`sigma` honesty. Written only when non-empty (absent ⇒ re-analyse to gain fan/trace). `Phase` enum gained `ShaftParallelBack=12`/`ArmParallelDown=13`/`ShaftParallelThrough=14` for the P-position system, but these were **not yet emitted** into `phases[]` at the time (deferred; see the 2026-08-09 row below for `13`/`14`). Additive — no schema-version bump. |
| 2026-07-13 | WB1 offline pose accuracy: `analysis.pose2d.decode` (`"dark"`) + `analysis.pose2d.cropRect` (`{x,y,w,h}` full-frame normalized swing-level person crop) added as provenance. Both written **only when non-legacy** (`decode` omitted for argmax, `cropRect` omitted on the full-frame fallback), so a flags-off run (`pose.crop.enabled=false`, `pose.decode.dark=false`) serialises **byte-identically** to the pre-WB1 tree. `kp` stay full-frame normalized (back-projected). Additive — no schema-version bump. |
| 2026-07-13 | WB2 head tracking: `analysis.metrics[]` gains `headSway`/`headLift` (`×frame` = fraction of frame width, isotropic — anisotropy of the separately-normalized x/y kp is corrected; Address-referenced) and `headTilt` (`°`, eye-line-angle Δ). Head position is a first-class golf metric. Derived from the (smoothed, else raw) face-on pose head keypoints (`head_track.{h,cpp}`); camera-only path too. UNSCORED (no reference bands yet — no `phaseSamples[].band`). Emitted only when the pose pass produced head-localizable frames. Additive — no schema-version bump; readers already iterate `metrics[]` key-agnostically. |
| 2026-07-13 | WB3 setup + footwork metrics: `analysis.metrics[]` gains `stanceWidth`/`leadFootFlare`/`trailFootFlare`/`toeLineAngle` (address-only scalars — **empty `t_us`/`value`, the single measurement in `phaseSamples[0]`**, the same generic `metrics[]` contract every reader already tolerates) and `leadHeelLift` (`×frame`, Address-referenced lead-heel-vs-toe elevation curve, `+` = heel lifts — a normal per-frame series like the head keys). Derived from the COCO-WholeBody foot keypoints (`foot_metrics.{h,cpp}`); camera-only path too; lead foot follows the athlete's handedness. UNSCORED. Emitted only when the pose pass produced foot-localizable frames (never on a legacy 17-kp track). Additive — no schema-version bump. Also: `streams[].setup.ballDetection` re-analysis (`BallRunner`) now derives its search corridor from these same foot keypoints when coverage allows (`ball.corridor.useFeet`, default on), falling back to the pre-WB3 ankle-based corridor otherwise — no swing.json field changes, this only affects re-analysis geometry. |
| 2026-07-21 | Tempo, stance-width mm, and ball position: `analysis.metrics[]` gains `tempoBackswing` (`s`, Address→Top) and `tempoRatio` (`:1`, `(Top−Address)/(Impact−Top)`) — both **Summary scalars** (empty `t_us`/`value`, single `phaseSamples[0]` at `phase: 5` **Impact**, since they describe the whole swing), emitted only when the ladder carries a confident Address+Top+Impact (the IMU `clampFallback` ladder has no Top ⇒ omitted, never estimated). Also `ballPosition` (`%`, ball projected onto the lead-heel→trail-heel line ÷ stance width; `phase: 0` Address scalar; **unclamped** — below 0 % means forward of the lead heel), emitted only when a ball is detected at address. `metrics[]` entries gain the **optional** `sigma` field (see the table above) — written today only by the two tempo metrics. **Changed, not added:** `stanceWidth`'s unit is now `mm` when the ball-diameter px→mm ruler resolves (`42.67 mm ÷ 2·radiusPx`, `src/Core/pp_physical_constants.h`), falling back to the previous `×frame` when no ball is detected — **readers must branch on `unit`, not assume it**. `leadHeelLift` deliberately stays `×frame`. All three producers gate on `tempo.enabled` / `ballpos.enabled`; both off ⇒ byte-identical to the pre-change tree (including `stanceWidth` reverting to `×frame`). Additive — no schema-version bump. |
| 2026-07-21 | Session-picker sidecar: a `swing_summary.json` companion file (`pinpoint.swingsummary/1`) is written beside each `swing.json` to cache the handful of scalars the "choose a session" drawer needs, so the picker never parses a multi-MB `swing.json` on the GUI thread. Pure regenerable cache, guarded by the source doc's size+mtime; written on every `swing.json` write/rewrite and self-healed on read. **Does not touch the `swing.json` schema** — separate file, separate schema id. See the sidecar section above and [`swing_folder_layout.md`](swing_folder_layout.md). |
| 2026-08-09 | Phase ladder gains real P5/P8 anchors: the segmenter now emits `13` ArmParallelDown (P5, lead-forearm parallel crossing) and `14` ShaftParallelThrough (P8) into `phases[]` in place of the old `4` Downswing / `6` Release ticks. `14` is a forearm PROXY for shaft-parallel-through (not shaft-measured), so its `conf` is capped ≤0.4 until a shaft-measured P8 exists. `12` ShaftParallelBack (P2) is still not emitted — P2 continues to live only in `analysis.club.positions[]`. `4`/`6` remain valid `Phase` values (still read from legacy files; readers must not assume they still appear in newly-written ladders). No schema-version bump. |
