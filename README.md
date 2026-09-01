<p align="left">
    <img src="src/Resources/icons/pinpointstudio_1024.png" width="150" />
  </p>

# PinPoint Studio

PinPoint Studio is a free, open source and cross-platform desktop application for serious golf swing analysis. It combines high-speed industrial cameras, Bluetooth IMUs, and on-device AI to build a complete picture of the swing — without sending data to the cloud unless you configure it to.

The app is currently in active prototyping. The core capture and analysis pipeline is functional; the coaching and session-history layers are in development.

<img width="2558" height="1440" alt="PPS-Home" src="https://github.com/user-attachments/assets/1ac15c00-7594-4423-aed3-4a890ee43192" />

The long term goal is to exploit computer vision and wearables to analyse golf movements and mechanistically determine your kinematic sequence aka Lateral-Rock-Twist-Jump, extract key golf swing metrics like X-Factor and tilt, working with the full swing or specialist shots such as pitching and in the sand, wrist angles to examine cupping, cocking and flipping, estimated ground forces to support the kinematic sequence analysis. 

Our ambition is to be a platform that can be used by golfers, coaches and researchers to improve everyone's golfing ability and understanding of the golf swing.

## Documentation

The `docs/` folder is organised by audience: **user**, **design**, **developer**, and **reference** (internal build/implementation plans live in `docs/implementation/`).

- [Building Instructions](BUILDING.md) — How to resolve dependencies and build PinPoint Studio.

**User & UX** — [`docs/user/`](docs/user)
- [UX Design](docs/user/pinpoint-ux-design.md) — UI structure, navigation, and interaction design rationale.
- [User Personas](docs/user/personas.md) — Definitions of the three primary user archetypes (club golfer, coach, researcher).
- [Persona UX Assessment](docs/user/pinpoint-persona-assessment.md) — UX evaluation against three user archetypes; identifies gaps and design priorities.
- [Wrist Calibration Guide](docs/user/wristcalibration.md) — How to mount the IMUs and run the two-pose wrist-motion calibration.

**Design** — [`docs/design/`](docs/design)
- [EventBuffer Design](docs/design/event_buffer_design.md) — Architecture and design rationale for the lock-free EventBuffer.
- [Shot Analyzer Design](docs/design/shot_analyzer_design.md) — Post-shot analysis pipeline: phase segmentation, metric extraction, scoring, and the per-session-type analyzer interface.
- [IMU Frame Contract](docs/design/imu_frame_contract.md) — The device-agnostic orientation boundary every IMU consumer depends on.
- [Calibrated Ball Detection](docs/design/ball_detection_calibration.md) — Environment-calibrated stationary-ball detection with a user-in-the-loop calibration protocol.
- [QML Design System](docs/design/pinpoint_qml_design_system.md) — Token system, typography rules, and component patterns; read before writing any QML.
- [Aesthetic Design Concepts](docs/design/aesthetic/pinpoint-aesthetic-concepts.md) — Three visual design directions (Editorial, Instrument, Studio) across light and dark themes.
- [Live Capture Collection](docs/design/live_capture_collection_design.md) — Why a paired phone never delivers swing video, the identity problem behind it, and the notification model that should replace today's toast cascade.
- [Wrist Motion Assessment](docs/design/wristmotion_assessment_design.md) — The wrist diagnostics engine + UI: per-position banding, the fault/strength rule engine, archetype band models, and the composite score.

**Developer guides** — [`docs/developer/`](docs/developer)
- [EventBuffer Developer Guide](docs/developer/event_buffer_developer_guide.md) — Tutorial covering usage, threading model, and integration patterns.
- [Swing Export Developer Guide](docs/developer/swing_export_developer_guide.md) — Per-swing MP4 + swing.json export: pipeline, resume gating, encoder, and sidecar schema.
- [Shot Detector Developer Guide](docs/developer/shot_detector_developer_guide.md) — Multi-modal shot detection: IMU impact + acoustic onset detectors, the arbiter, and latency-aware timestamping.
- [Shot Analyzer Developer Guide](docs/developer/shot_analyzer_developer_guide.md) — The post-shot pipeline: from frozen SwingWindow through analysis to score, metrics, and the unified swing.json.

**Reference** — [`docs/reference/`](docs/reference)
- [Wrist Metrics Reference](docs/reference/wristmetrics.md) — Lead-arm wrist-angle metrics: sign conventions, coaching names, and research-backed bands.
- [WT901BLE67 Protocol Reference](docs/reference/wt901ble67_protocol.md) — Packet formats, register map, and BLE transport details for the Witmotion IMU.

---

## UI shell

The interface uses a left-side navigation rail with an athlete avatar at the top, five mode buttons, and utility buttons at the bottom.

| Mode | Status | Description |
|---|---|---|
| **Home** | Active | Session type selection, device readiness, club selector, and Start button |
| **Swing** | Active | Multi-camera capture with pose estimation, manual + automatic SHOT triggers, and on-stage shot review |
| **Wrist** | Active | Live video tile per session-enabled camera (skeleton overlay) + live lead-arm wrist-angle metrics; SHOT runs the wrist analyzer — the first real one — and adds each shot to the session carousel for on-stage review; in **Analyse**, a Wrist Motion diagnostics panel scores each swing position against expected bands and surfaces named faults, strengths, and coaching (requires an athlete) |
| **GRF** | Placeholder | Ground reaction force analysis (requires an athlete) |
| **Coach** | Placeholder | AI coaching output (requires an athlete) |

Wrist, GRF, and Coach redirect to the Welcome screen until at least one athlete has been created.

### Session modes

Every session screen (Swing, Wrist, GRF, Coach) runs in one of three **modes**, chosen from the toolbar's mode switch. The active mode re-lays the centre stage and decides what the camera panel plays:

| Mode | Stage | Camera panel |
|---|---|---|
| **Capture** | Live camera tiles with overlays, the SHOT trigger, and the filling shot carousel — the recording surface | Live camera feeds |
| **Replay** | A captured swing promoted onto the stage: its video with the analyzed overlay, metric charts, and a scrubbable phase timeline — for watching it back | The swing's disk video, ¼ speed |
| **Analyse** | The *same* loaded video plus the swing's full metric charts and a read-only data table — for reading the numbers, not just the footage (the Wrist screen adds a Motion-diagnostics dashboard panel) | The same disk video (follows the Replay↔Analyse toggle) |

**Mode is the layout/activity axis, and it is *orthogonal* to the data source** — whether the stage shows the *live* session or a *loaded* past one. The two compose:

- **Capture is live-only.** Entering Capture always returns to the running live session and drops any focused swing; live capture keeps recording in the background while you review.
- **Replay and Analyse work on either data source** — a swing from the live session you are recording, or any swing from a session loaded off disk.
- **Replay ↔ Analyse share the focused swing and its loaded video**, so toggling between them never reloads or restarts playback; only entering Capture tears the loaded swing down.

A single click on any carousel card promotes that swing onto the stage and enters Replay. Each mode remembers its own panel set and stage arrangement, edited via the toolbar's View control. See [Shot review — the session stage](#shot-review--the-session-stage).

Each session screen carries a persistent **session toolbar** — clock, Capture control, central SHOT trigger, End Session, the mode switch and View control, and Cameras/IMUs device pills with in-panel device management and calibration. See [Session toolbar](#session-toolbar).

Three utility buttons sit at the bottom of the rail:

| Button | Description |
|---|---|
| **Play ▶** | Developer hatch — direct access to legacy tab pages during prototyping |
| **System ◈** | Opens the resource monitor (buffer, camera, and IMU diagnostics) |
| **Settings ⚙** | Opens the full Settings screen (see [Settings](#settings)) |

The **Settings** screen selects from eight visual themes — four aesthetics (Instrument, Editorial, Studio, Vector) × two modes (light, dark) — and the selected theme is persisted across restarts. See [Aesthetic Design Concepts](docs/design/aesthetic/pinpoint-aesthetic-concepts.md).

| Editorial | Instrument | Studio |
|---|---|---|
| ![Editorial light](docs/design/aesthetic/editorial_summary_light.png) | ![Instrument light](docs/design/aesthetic/instrument_summary_light.png) | ![Studio light](docs/design/aesthetic/studio_summary_light.png) |

---

## Features

### Home screen

The Home screen is the default landing page and the starting point for every session.

- **Session type cards** — Four modes displayed as selectable cards, each showing a description, required device counts, and live readiness indicators:

  | Mode | Cameras | IMUs | Description |
  |---|---|---|---|
  | **Swing analysis** | 2 required | 3 required | Sequencing and key swing metrics via spine IMUs |
  | **Wrist motion** | 1 optional | 2 required | Wrist angle and club delivery analysis |
  | **Ground forces** | 2 required | 3 required | Ground use and power generation via hip IMUs |
  | **AI coach** | 2 required | 3 required | Shot-by-shot feedback from an AI coach |

- **Device readiness** — Each card shows a live `✓ / ⚠` status for cameras and IMUs independently. Cameras require at least the stated number to be enumerated; Wrist motion shows the camera as optional (amber tick when absent, green when present).
- **Club selector** — Choose the club in play before starting; recorded with the session.
- **Start session** — Opens the session wizard once device requirements are met.

### Session wizard

A five-step guided flow that prepares a session before recording begins. Steps are shown as a horizontal progress rail; Back/Continue navigation is available at each step.

| Step | Name | Description |
|---|---|---|
| 0 | **Goals** | Confirm the session type and set an optional speed target for the session |
| 1 | **Cameras** | Review discovered cameras; assign face-on / down-the-line / other perspective; toggle mirroring |
| 2 | **IMUs** | Connect sensors; assign body placement slots (A–D); Continue is locked until all required IMUs are connected |
| 3 | **Calibration** | Two-phase IMU calibration (see below); Continue is locked until calibration is complete |
| 4 | **Ready** | Confirm the session summary; Start begins capture |

Pressing Back from any step returns to the previous one. Navigating back to the Calibration step retains a completed calibration for the life of the current `ImuInstance`; starting a new wizard session with the same connected device also restores it. The **Recalibrate** button is always available to restart the sequence.

### Session toolbar

A persistent toolbar pinned to the top of every mode screen (Swing, Wrist, GRF, Coach), built as a single reusable component shared across all four. It carries the session clock, one global capture control, a SHOT trigger, End Session, the mode switch and View control, and two device pills.

- **Capture** — anchored at the far left with the session clock alongside it. It is the **single owner of the EventBuffer state**: Capture/Stop toggles the user capture intent (`resumeBuffer` / `pauseBuffer`) and starts the session clock on first capture. Nothing else changes the net buffer state — ball detection is signal-only (it drives overlays, never capture).
- **SHOT** — centred trigger that funnels every shot source through a single `ShotController`: the manual button always, plus the automatic IMU-impact and acoustic-onset detectors when *Auto-detect swing* is on (pose/ball later). Armed only while the buffer is capturing and the shot processor is idle; firing it runs the post-shot pipeline (see [Shot capture & analysis](#shot-capture--analysis)). A **DETECT** cluster of per-modality dots (IMU / Acoustic / Ball) sits alongside — each glows while its detector is armed and flashes green on a firing.
- **Mode switch** — a three-segment **Capture / Replay / Analyse** control, the primary layout control of the stage. Selecting a mode re-lays the centre stage to that mode's saved layout; choosing Replay with no swing focused shows a "select a swing" prompt rather than blocking. Switching mode never stops live capture (that is the data-source axis — see [Session modes](#session-modes)).
- **View** — a pill showing the current mode; tapping it opens the View panel, which edits *that mode's* layout: which panels are shown (camera, charts, table, timeline, carousel, markup — plus a dashboard placeholder) and how the stage packs them (**tabs / split / stage**). Edits apply live and persist per mode; there are no named presets. (The markup panel is the in-app ground-truth labelling surface — see [Markup — ground-truth labelling](#markup--ground-truth-labelling).)
- **End Session** — ghost button (visible while a session runs) with a small confirm popup; ends the session clock, stops capture, and unlocks navigation.
- **Device pills** — Cameras and IMUs, each with a connected-count badge and aggregate state. A pill turns amber and reads **"calibrate"** when a connected device still needs calibration; the IMU pill instead warns **"battery N%"** (amber, or red below 20%) when any connected sensor drops below 50%.
- **Drop-down panels** — Tapping a pill opens a panel beneath it with a scoped action row (**Scan / Connect / Calibrate**) and a per-device list. Opening one panel closes the other; click-away or Esc dismisses.
  - **Per-device enable toggles** — Session-local enable/disable per camera and IMU, seeded from the Settings-level exclusion list but never written back (global enablement stays owned by the Settings screen). Camera session enablement lives in `CameraManager` so every toolbar and mode screen shares one list — the mode screens show a video tile per session-enabled camera, and toggling a camera off removes its tile. Connect connects every enabled, not-yet-connected device and starts the camera capture pipeline (the screens' video tiles stream from it); disabling a connected device disconnects it.
  - **Live pose toggle** — An all-cameras switch in the camera panel that gates pose inference itself (not just the overlay); ball detection and the shot replay pipeline are unaffected.
  - **IMU rows** — Live connection-state LED (grey idle · flashing grey/green connecting · green connected · red failed), battery and data-rate, and the configured body placement.
  - **Camera rows** — Connection-state dot, perspective, serial, and interface.
- **In-panel calibration** — The Calibrate action runs the calibration flow *inside* the panel; it never opens the full-screen wizard or leaves the mode screen. The IMU flow is the exact same state machine as the session wizard's Calibration step — extracted into a shared `ImuCalibrationFlow` component rendered compactly — so calibration is single-sourced. The Calibrate action and pill stay framed in call-to-action amber until calibration is *successful* (mount validation passes). Camera (stereo) calibration is a placeholder pending the calibration pipeline.

### Athlete management

Every session belongs to an athlete. The athlete management flow is the entry point to the app.

- **Create athlete** — Required fields: name, handedness. Recommended: height, weight, handicap, primary club. Optional: driver speed target, notes/tags.
- **Athlete picker** — Shows the three most-recently-active athletes as cards, plus a full searchable list. The selected athlete's initials appear in the rail avatar.
- **Delete athlete** — Destructive action available in the picker with a single click on the highlighted athlete.
- **Persistence** — All athlete records stored in `QSettings` (INI format); survives restarts. Heights stored in ft, weights in lb regardless of entry unit.
- **Navigation guard** — Wrist, GRF, and Coach modes require at least one athlete; selecting them from the Home screen redirects to the Welcome screen if the roster is empty.

### Swing — multi-camera video analysis

- **Multi-camera support** — Select any combination of discovered cameras; each gets its own side-by-side view with independent pose estimation. Start/Stop controls all cameras simultaneously.
- **Shared video view** — Every screen renders cameras through one component (`PpCameraFrame`) with per-screen configurable overlays (skeleton, hitting area, badges). Each `CameraInstance` *publishes* its frames to all subscribed views — any number of views can show the same camera at once, across screens.
- **Camera backends** — UVC webcams, Aravis (GenICam industrial cameras), Spinnaker (Teledyne/FLIR).
- **Spinnaker pipeline** — Raw Bayer bytes captured with no CPU demosaic on the hot path; a custom `QQuickRhiItem` runs a bilinear GPU Bayer demosaic shader at display rate while the pose estimator receives OpenCV-demosaiced frames at its already-throttled rate.
- **Pose estimation** — MoveNet SinglePose Lightning and Thunder via ONNX Runtime — real-time skeleton overlay on each live feed, switchable per camera.
- **Ball detection** — Drives the hitting-area overlay and ball-present indicator only. It is signal-only and never starts, stops, or replays capture (the buffer is owned solely by the Capture control).
- **GPU acceleration** — CoreML (Apple Silicon), CUDA 12/13 (NVIDIA on Linux/Windows).

### Shot capture & analysis

A shot is the unit of analysis. Every shot source funnels through one `ShotController`, and a single `ShotProcessor` owns the post-shot pipeline. Shots fire **manually** (the toolbar SHOT button) or **automatically**: with *Auto-detect swing* on — the default — an IMU-impact detector and an acoustic-onset detector each report candidates to an arbiter that fuses them (commit when two modalities agree within 40 ms, or on a lone high-confidence candidate) and back-dates the timestamp to true impact. Pose- and ball-based detectors join later.

- **Trigger → post-roll** — On a shot, the buffer keeps capturing for a short post-roll so the follow-through lands in the ring, then pauses and freezes the trailing ~5 s as an immutable `SwingWindow`.
- **Analyse ∥ export** — The frozen window feeds two concurrent workers reading it zero-copy: the per-session-type **shot analyzer** (Swing / Wrist / GRF / Coach) and the **swing exporter** (per-camera MP4 + thumbnail). The Wrist analyzer is the first real one — it segments swing phases, extracts lead-arm wrist metrics, and produces a banded swing score.
- **¼-speed auto-replay (Capture)** — Immediately after a shot, its camera footage replays in-place at ¼ speed on the live tiles with a `REPLAY ¼×` overlay and a pulsing badge — a transient confirmation that reads the frozen window's frames directly, independent of whether analysis or disk export succeeded. Press **Esc** to skip it. (Full, scrubbable review happens on the stage — see [Shot review — the session stage](#shot-review--the-session-stage).)
- **Persistence** — Each shot is written as one unified `swing.json` (raw frames + analysis) plus its MP4/thumbnail. Shots reload from disk on startup, so a session's history survives restarts; the analysing indicator on the toolbar shows when the pipeline is busy.

### Shot review — the session stage

A captured (or loaded) shot is reviewed by promoting it onto the main session **stage** — the same camera/charts/timeline panels the live session uses — rather than a pop-over. The shot **carousel** at the foot of every session screen is the filmstrip that drives it.

- **Single click promotes** — Clicking a card makes that swing the focused swing, loads it onto the stage, and enters **Replay** mode. Clicking another card swaps the focused swing in place (Lightroom-style filmstrip → loupe). The carousel stays hot during Capture, so you can drop into review mid-session — and on a live session, capture keeps recording in the background while you do.
- **Cross-machine safe** — The stage enumerates the *reviewed swing's own* camera streams from its `swing.json`, never the local rig, so a swing recorded on a different setup (different camera count, perspectives, aspect ratios) still plays back, degrading gracefully when streams or analysis are missing.
- **Stage panels** — In Replay the camera tiles render the swing's video with the analyzed skeleton/club overlay (face-on stream); the **charts** panel draws its metric traces; the **timeline** panel carries a scrub slider and bold, clickable phase pills — all locked to one playhead. An always-on **transport** (play/pause, frame-step, speed) keeps working even when the timeline panel is hidden. *Analyse* keeps that same footage and adds a read-only data table beside the charts.
- **Shot cards** — Thumbnail, swing score, and a tappable star rating; cards persist their rating and free-text note back to `swing.json` (rating is editable directly on the card, no pop-over).
- **Exit** — Leave Replay via the mode switch, the Capture control, or **Esc**; from a live session that returns you to the running capture.
- **Sessions & trash** — A sessions drawer opens past sessions from disk for review; shots move to trash (recoverable, with an Undo toast) rather than being deleted outright, and bulk export/trash act on the filtered selection.

### Markup — ground-truth labelling

A **Markup** stage panel — added to the View control alongside camera, charts, table, timeline, and carousel — turns the focused swing into a frame-accurate manual labelling surface. It is the in-app way to produce the ground truth that the swing analyzer is validated and tuned against, without ever leaving the app.

- **Operates on the focused swing** — load a swing onto the stage (click a carousel card, or enter Replay), then show the Markup panel. It decodes that swing's own face-on MP4 frame-by-frame on a dedicated Qt Multimedia path, independent of the replay transport, so labelling never disturbs playback.
- **Lay the club, tag the position** — scrub to a frame, click the grip then the clubhead to place the shaft on that frame, then press a P key (`1`–`9`, `0` = P10) to tag it as one of the ten golf P-positions (P1 Address … P10 Finish). A read-only recorded-pose skeleton (blue body, amber lead hand, purple trail hand) can be overlaid for reference; it is never part of the saved label.
- **Frame-accurate transport** — keyboard-driven: `a`/`d` step one frame, `space` / `[` / `]` jump by a configurable stride, the slider scrubs, and each tagged P-position chip jumps straight back to its frame. A HUD shows the exact frame index and timestamp; a per-swing progress readout tracks how many of the ten positions are complete.
- **Validation-compatible output** — labels are held normalised (resolution-agnostic) in memory and saved (`q`, or the Save button) as a `truth.json` sidecar next to the swing, byte-compatible with the SwingLab scoring harness. This closes the loop: capture in the app, label in the app, then score and tune the analyzer against your own ground truth.

### IMU — wrist motion capture

- **Device** — Witmotion WT901BLE67 BLE 6-axis IMU (accelerometer, gyroscope, Euler angles, quaternion).
- **Multi-device support** — Select any number of discovered IMUs simultaneously; each gets its own side-by-side 3D visualiser, state label, battery badge, rate selector, and Zero button. Mirrors the multi-camera chip pattern.
- **Device chips** — One toggle chip per enumerated IMU at the top of the Play → IMU tab; tap to connect/disconnect. Devices appear as soon as the BLE scan finds them.
- **3D orientation visualiser** — Labelled cube driven by the corrected quaternion; matches the physical device orientation in real time. The `ImuVizView` component is shared between the capture page (per-instance) and the settings test panel.
- **Auto-initialisation** — Sets vertical mounting, 6-axis algorithm, 100 Hz output rate, and zeros orientation to current position on every connect.
- **Orientation fusion** — Orientation is re-derived on-host from the raw gyroscope + accelerometer stream by a selectable software filter (Madgwick or ESKF), used by the wrist-kinematics calibration path. Chosen globally in Settings → IMUs and applied to all connected devices immediately.
- **Two-phase session calibration** — The session wizard Calibration step captures two reference quaternions for the lead-arm IMU (slot A):
  - *Phase 1 — arm at rest:* an animated guide demonstrates the resting position; once the IMU is stable for 3 cumulative seconds the arm-down quaternion is captured.
  - *Phase 2 — T-pose:* the guide raises to T-pose; another 3-second stable hold captures the T-pose quaternion.
  - Both quaternions are stored in memory on the `ImuInstance` for the duration of the session. They are not persisted to disk; reconnecting or deselecting the device clears them. The Recalibrate button restarts the sequence at any time.
- **Zero button** — Re-zeroes orientation on demand for mid-session repositioning, per device.
- **Rate selector** — Adjustable output rate per device (10 / 20 / 50 / 100 / 200 Hz).
- **Live data rate** — 2-second rolling Hz average shown per device.
- **Battery indicator** — Colour-coded BAT: N% badge per device, polled via register 0x64 every 60 s. The session toolbar's IMU pill also surfaces the lowest connected level, warning **"battery N%"** when any sensor drops below 50%.
- **Auto-retry** — One automatic retry after a 45-second cooldown on failed connections (device requires ~40 s to exit cooldown after a rejected attempt).
- **Session log** — Timestamped per-record diagnostics per device; Save Log writes to `~/imu_log_<MAC>_<timestamp>.txt`.

### Audio — speech interface

- **Speech-to-text** — Whisper.cpp (local, Vulkan/CUDA GPU-accelerated) with Azure Speech REST fallback for CPU-only systems. Backend badge in the UI shows GPU / Cloud / Apple; clickable to toggle when cloud fallback is available.
- **Text-to-speech** — Kokoro TTS (local, ONNX Runtime) with Azure Neural Voice fallback for CPU-only systems. Same badge/toggle pattern.
- **Latency display** — Per-request latency shown next to each badge (e.g. `523 ms`).
- **Acoustic shot detection** — The selected microphone also feeds an onset detector that auto-triggers shots on club-impact sound (one of the multi-modal detectors above); gated and tuned in Settings → Microphone, independent of voice/STT. See [Shot capture & analysis](#shot-capture--analysis).

### Settings

The Settings screen uses a sidebar navigation with full-text search (Ctrl/Cmd+F) and panel-level organisation.

| Panel | Status | Contents |
|---|---|---|
| **General** | Active | Language, measurement units, session behaviour (auto-detect swing, AI coaching), update and diagnostics preferences |
| **Appearance** | Active | Theme selector (8 options), font scale, UI density, reduce motion, pose overlay opacity |
| **Displays** | Active | Main display placement, window geometry memory, secondary display output, how the session diagnostics cast is surfaced (panel / auto-closing window / kiosk) with its delay, dwell and mirror, UI frame-rate cap, hardware acceleration |
| **Cameras** | Active | Per-camera enable/disable, view assignment (Face-on / Down-the-line / Other), mirrored image toggle, frame-rate chips, trigger mode (Free-run / HW sync), ROI crop with live preview; global pre-roll buffer and camera-sync toggle |
| **IMUs** | Active | Per-device enable/disable, body placement assignment (A–D), output rate chips, save-to-flash, live test panel with 3D viz and Euler angles; global auto-connect, auto-reconnect, save-calibration-to-flash, and orientation-fusion algorithm (Madgwick / ESKF) |
| **Microphone** | Active | Single-active input-device selection; "use microphone for shot detection" toggle (acoustic modality only — voice/STT unaffected); live calibration view with a dB level trace, trigger-threshold line, per-detection markers + chime, and a sensitivity slider |
| **Launch Monitor** | Placeholder | External launch monitor integration (not yet implemented) |
| **Storage** | Active | Athlete library path, session folder naming, auto-save; video codec/resolution/quality/container; sensor data export format |
| **Archiving** | Placeholder | Session archive path and retention policy (not yet implemented) |

The Cameras panel shows sensor info (vendor/model, resolution, pixel format, bit depth) and a real-time storage estimate (per-frame MB and ring-buffer slot count) that updates as the ROI is adjusted.

### Film — video annotation

- **YouTube download** — Bundled yt-dlp fetches videos from YouTube (Premium quality, browser cookie auth) to a local cache; no re-download on repeat analysis.
- **On-demand annotation** — Pause on a frame, click Annotate: runs a person segmentation model (u2netp) to isolate the golfer, blurs the background, then runs MoveNet for a clean pose estimate.
- **Skeleton overlay** — Background-blurred frame displayed with the MoveNet skeleton drawn on top.
- **Scrubbing** — Live frame preview while dragging the seek slider.

---

## Device lifecycle

Every physical device — camera or IMU — passes through the same four stages. New code must respect this contract; violating it corrupts the EventBuffer or leaks ring-buffer memory.

### Stages

```
Enumerated → Selected → Recording → Deselected
     ↑            ↓                      ↓
  (scan)    registerSource()      deregisterSource()
```

| Stage | Camera | IMU |
|---|---|---|
| **Enumerated** | `VideoInputFactory::enumerateDevices()` at `CameraManager` construction. Device appears in `cameraList`; no `CameraInstance` exists. | `DeviceEnumerator::scanImu()` starts async BLE scan at `ImuManager` construction. Device appears in `imuList` as discovered; no `ImuInstance` exists. |
| **Selected** | User taps chip → `CameraManager::setSelected(i, true)` → `CameraInstance` constructed → `EventBuffer::registerSource()`. | User taps chip → `ImuManager::setSelected(i, true)` → `ImuInstance` constructed → `EventBuffer::registerSource()`, then `start()` begins the async BLE connection. |
| **Recording** | `CameraManager::startAll()` → `CameraInstance::startRecording()` on each selected instance. The buffer enters Capturing when the user presses Capture (the session-global capture intent) — independent of ball detection. | IMU writes data continuously once the BLE connection is established; the EventBuffer's Capturing/Paused state gates whether the merger reads from the ring. |
| **Deselected** | User taps chip → `CameraManager::setSelected(i, false)` → `stopRecording()` if active → `deregisterFromBuffer()` → `deleteLater()`. | User taps chip → `ImuManager::setSelected(i, false)` → `stop()` (BLE disconnect) → `deregisterFromBuffer()` → deferred `deleteLater()`. |

### Invariants

These invariants must hold at all times:

1. **No registration at startup.** Neither manager creates instances or registers sources in its constructor. The first registration always follows an explicit user selection.

2. **Register on selection, deregister on deselection.** `registerSource()` is called exactly once — in the device instance constructor, which runs inside `setSelected(…, true)`. `deregisterSource()` is called exactly once — in `deregisterFromBuffer()`, which runs inside `setSelected(…, false)` and in the manager destructor.

3. **Buffer is paused around every register/deregister call.** `setSelected()` snapshots `wasCapturing`, calls `pause()` before touching sources, then restores the buffer state after. This prevents the EventBuffer merger from reading a half-initialised or already-freed source.

4. **`deregisterFromBuffer()` is called before `deleteLater()`.** The instance pointer is nulled and `instancesChanged()` emitted first so QML delegates are torn down while the object is still live; deregistration happens next; only then is the object queued for deletion.

5. **Excluded ≠ deselected.** The `excluded` flag is a Settings-level preference (applied via `setExcluded()`). Setting `excluded = true` on a currently-selected device triggers an explicit `setSelected(…, false)` call, which follows invariant 4 above. Clearing `excluded` on a deselected device triggers `setSelected(…, true)`.

6. **Re-enumeration is safe.** Calling `enumerateDevices()` again (e.g. after a settings scan) only adds new entries to `DeviceEnumerator`; it never removes or invalidates live instances or registered sources.

### Buffer state machine

The EventBuffer's net state is owned **solely by the session-global capture intent** (the toolbar Capture/Stop). Ball detection is signal-only and never moves it. `CameraManager` applies the intent; `ShotProcessor` owns the post-shot `SwingWindow` lifecycle.

```
Idle ──Capture──▶ Capturing ──SHOT──▶ post-roll ──▶ pause + freeze SwingWindow
  ▲                  ▲                                analyse ∥ export ──▶ ¼× replay
  └──── Stop ────────┴───────────── restore capture intent ◀── window destroyed
```

- **Idle** — before the first Capture (and with no registered sources).
- **Capturing** — capture intent is on; the merger reads all registered sources and builds the merged timeline. This is the steady state of a live session.
- **Paused** — capture intent is off (after Stop), or held transiently while the shot pipeline owns a `SwingWindow`. The merger does not advance the timeline; ring memory stays live.
- A SHOT keeps capturing through a short post-roll, then pauses and freezes the trailing ring as a `SwingWindow`. `resume()` is blocked while that window is live; once it is destroyed the user capture intent is re-applied (back to Capturing if the session is still capturing).

### Applying this to new device types

To add a new device type (e.g. a launch monitor, a force plate):

1. Create a `DeviceEnumerator` scan path; populate results with `DeviceType::YourType`.
2. Create a manager class (e.g. `LaunchMonitorManager`) following the `ImuManager` pattern: constructor scans only, no instances created.
3. Create an instance class (e.g. `LaunchMonitorInstance`) that calls `registerSource()` in its constructor and exposes `deregisterFromBuffer()`.
4. In `setSelected(…, true)`: pause buffer → construct instance (registers source) → re-apply the capture intent.
5. In `setSelected(…, false)`: pause buffer → stop → `deregisterFromBuffer()` → null the pointer → emit changed → `deleteLater()` → re-apply the capture intent.
6. In the manager destructor: repeat the deselection teardown for all live instances.

---

## Phone capture (PPCP)

A phone running **PinPoint Capture** joins a session as a camera. It pairs by scanning a code from the home screen (*Pair a device*), is remembered afterwards, and its cameras then appear in the devices list beside the USB and industrial ones. Pairing, the name this computer shows, per-phone health (battery / thermal) and *Forget* live in **Settings → Phones**.

The link is the **PinPoint Capture Protocol (PPCP)**: TLS 1.3 with an external pre-shared key, carried either over WiFi (the phone dials the host) or over a **USB cable** (the host dials the phone through Apple's usbmux tunnel). There is no unencrypted mode — a build made without the PPCP dependencies simply has no phone support, and says so in Settings → Phones.

### What each platform needs

Whether the feature is *compiled in* is decided by build-time dependencies — see [BUILDING.md](BUILDING.md). The table below is what must be present on the **user's** machine at run time for an installed build.

| Capability | macOS | Windows | Linux |
|---|---|---|---|
| **Pairing + WiFi link** (the baseline) | Nothing to install | Nothing to install | Nothing to install beyond the system `libssl` |
| **Reconnect discovery** (optional — a remembered phone finds this computer again without a new code) | Built into the OS | `dnssd.dll` from Apple's **Bonjour** — installed by Bonjour Print Services, iTunes, or the *Apple Devices* app | `libavahi-compat-libdnssd1` with `avahi-daemon` running |
| **Wired (USB) capture** (optional) | Built in — Apple's own `usbmuxd` at `/var/run/usbmuxd` | The **Apple Devices** app (Microsoft Store) or iTunes, which provides `AppleMobileDeviceService` on `127.0.0.1:27015` | The `usbmuxd` daemon (`apt install usbmuxd`) and its udev rules; the socket may need a group membership |
| **Firewall** | Allow incoming connections when macOS first prompts | Allow PinPoint Studio on **Private** networks — the first-run Windows Security alert gates pairing | Allow the listener port if a firewall is active |

Every one of the optional rows is *absent, not broken*, when the dependency is missing: no reconnection discovery still leaves pairing by code working, and no usbmux provider just means the cable is never offered. Nothing raises a banner; the reason is written to the application log.

### Troubleshooting — a wired link that keeps dropping (macOS)

**Symptom.** A cabled phone session runs for a few minutes and then every PPCP channel closes at once — cleanly from the phone's side, with broken pipes on the Mac's. It reconnects, then drops again, sooner when the link is busy. The cable, the phone and the app all look innocent.

**Cause — an OS setting, not the app and not `usbmuxd`.** macOS creates an **"iPhone USB"** network service for USB tethering whenever an iPhone is plugged in. If the phone is not actually sharing a hotspot, that service can sit stuck on a self-assigned `169.254.x.x` address, endlessly renegotiating a link that isn't there. Each renegotiation makes macOS perform a full USB `SetConfiguration` on the phone, which tears down **every** interface on the device — the usbmux tunnels along with it — and rebuilds them ~150 ms later. No electrical disconnect ever occurs, which is exactly why the hardware looks fine.

**Fix.** Turn the tethering service off. It is reversible, and it affects neither charging, nor syncing, nor PPCP:

```bash
networksetup -setnetworkserviceenabled "iPhone USB" off
# and to put it back:
networksetup -setnetworkserviceenabled "iPhone USB" on
```

**Confirming it**, if drops ever return:

```bash
/usr/bin/log stream --predicate 'eventMessage CONTAINS "setConfigurationGated" OR eventMessage CONTAINS "updateLinkStatus"' --info
```

Each drop lines up to the millisecond with an `AppleUSBNCMData::updateLinkStatus: linkStatus 0` followed by `IOUSBHostDevice::setConfigurationGated: … selected configuration 6`. A second tell is the usbmux device index climbing fast — ids incrementing through the twenties in a morning means the device is being re-enumerated, not that anything reconnected.

> ⚠ **Use the full path `/usr/bin/log`.** In `zsh`, `log` is a shell builtin that lists login records, so a bare `log show` / `log stream` returns nothing at all, with no error — which reads exactly like "the logs are empty" and is how this fault stayed hidden for a session.

---

## Technology

Built with **Qt 6.11** and **C++20**.

| Component | Technology |
|---|---|
| UI | Qt Quick / QML (Qt 6.11) |
| Speech-to-text | whisper.cpp (Vulkan / CUDA) + Azure Speech REST |
| Text-to-speech | Kokoro ONNX Runtime + Azure Neural Voice |
| Pose estimation | MoveNet Lightning / Thunder, ViTPose-B (ONNX Runtime) |
| Person segmentation | u2netp (ONNX Runtime) |
| Video download | yt-dlp (bundled binary) |
| GPU acceleration | Vulkan, CUDA 12 + 13, CoreML (Apple Silicon) |
| Image processing | OpenCV 3.0+ |
| IMU | Witmotion WT901BLE67 via Qt Bluetooth LE |
| Phone capture | PinPoint Capture Protocol (libppcp) — TLS 1.3 external PSK via OpenSSL 3; DNS-SD (Bonjour / Avahi) for discovery; usbmux for the wired link |
| Athlete data | QSettings (INI format, `~/.config/PinPointStudio/PinPointStudio.ini`) |

---

## Local files

PinPoint Studio reads and writes files in several locations. Platform paths shown for Linux; macOS and Windows equivalents are noted in brackets.

### Application data directory
`~/.local/share/PinPointStudio/` (macOS: `~/Library/Application Support/PinPointStudio/`, Windows: `%APPDATA%\PinPointStudio\`)

| Path | What | When |
|---|---|---|
| `models/whisper/<model>.bin` | Whisper STT model | Copied from the CMake build cache at build time |
| `models/kokoro/` | Kokoro TTS ONNX model + voice data | Downloaded from HuggingFace on first launch (skipped when cloud TTS is configured) |
| `models/llm/phi4-mini/` | Phi-4-mini AI-coach LLM (ONNX, ~4.9 GB) | Downloaded from HuggingFace on first launch when a local GPU is present |
| `film-cache/<video_id>.mp4` | Downloaded YouTube videos | Written by yt-dlp on demand; never auto-deleted |

### Application settings
`~/.config/PinPointStudio/PinPointStudio.ini` (macOS: `~/Library/Preferences/com.PinPointStudio.PinPointStudio.plist`, Windows: `%APPDATA%\PinPointStudio\PinPointStudio.ini`)

The app forces `QSettings::IniFormat` (see `src/Core/pp_settings.h`), so on Windows settings are an INI file, **not** registry keys.

**Status key** — ✅ wired (read by app code; drives behaviour) · ⚙️ live (applied interactively but not restored on next startup/reconnect) · 📋 planned (persisted; not yet consumed outside settings)

**UI**

| Key | Default | Status | What |
|---|---|---|---|
| `ui/themeIndex` | `0` | ✅ | Selected visual theme (0–7: Instrument light/dark, Editorial light/dark, Studio light/dark, Vector light/dark) |
| `ui/windowWidth` | `1120` | ✅ | Main window width in pixels; updated on every resize |
| `ui/windowHeight` | `700` | ✅ | Main window height in pixels; updated on every resize |
| `ui/windowX` | `-1` | ✅ | Saved window X position (`-1` = not saved) |
| `ui/windowY` | `-1` | ✅ | Saved window Y position (`-1` = not saved) |
| `ui/windowMaximized` | `false` | ✅ | Whether window was last maximised/full-screen |
| `ui/fontScale` | `-1.0` | ✅ | Font scale multiplier (`-1.0` = auto from display DPI) |
| `ui/density` | `"default"` | ✅ | UI density (`"default"`, `"compact"`, or `"spacious"`) |
| `ui/reduceMotion` | `false` | ✅ | Disable animated transitions |
| `ui/overlayOpacity` | `0.7` | ✅ | Opacity of the pose skeleton overlay (0.0–1.0) |
| `ui/gradientTitles` | `true` | ✅ | Gradient fill on display titles; `Theme.qml` → `Theme.gradientTitlesActive` → `PpDisplayText` (flat text when off) |
| `ui/timelineOrientation` | `"horizontal"` | ✅ | Session timeline rail orientation (`"horizontal"` top rail / `"vertical"` side rail); read by `PpViewPanel`, `ScreenSessionMode`, `ScreenWrist`, `PpTransitTimeline` |
| `ui/timelineSnapToPhases` | `false` | ✅ | Snap timeline scrubbing to swing-phase boundaries; read by `PpViewPanel` + the `PpTransitTimeline` instances |
| `ui/wristReferenceSwingDir` | _(empty)_ | ✅ | On-disk `swing_NNNN` directory used as the Wrist-diagnostics reference comparison; set/read by `WristDiagnostics.qml` |

**General**

| Key | Default | Status | What |
|---|---|---|---|
| `General/language` | `"en_GB"` | ✅ | UI language tag (e.g. `"en_US"`, `"fr_FR"`, `"ja_JP"`); restart required |
| `General/units` | `"mph"` | ✅ | Speed/distance unit (`"mph"` or `"kmh"`); used in session goals |
| `General/autoDetectSwing` | `true` | ✅ | Master toggle for automatic shot detection — when on, the IMU-impact and acoustic-onset detectors feed the arbiter during a live capture; when off, only the manual SHOT button fires |
| `General/swingDetectionSensitivity` | `"Medium"` | ✅ | IMU impact-detector threshold scale (`"Low"` = 1.5×, `"Medium"` = 1.0×, `"High"` = 0.7×) |
| `General/audioDeviceLatencyUs` | `20000` | ✅ | Microphone capture-chain latency (µs) used to back-date acoustic onsets to true impact |
| `General/audioInputDevice` | _(empty)_ | ✅ | Persistent id of the selected microphone (empty = system default) |
| `General/acousticShotDetectionEnabled` | `true` | ✅ | Gate for the acoustic shot-detection modality; independent of voice/STT |
| `General/acousticSensitivity` | `0.5` | ✅ | Acoustic onset sensitivity (`0.0` least … `1.0` most); maps to the absolute amplitude gate |
| `General/athleteLibraryPath` | _(empty)_ | ✅ | Root folder for session/swing output; read by `SwingPaths`, `swing_exporter`, `shot_processor`, `session_review_controller` (empty → default root) |
| `General/cloudFallbackStt` | `false` | ✅ | Force cloud STT even when a local GPU is present; selects the backend in `TranscriptionController` |
| `General/cloudFallbackTts` | `false` | ✅ | Force cloud TTS even when a local GPU is present; selects the backend in `TtsController` |
| `General/cloudFallbackLlm` | `false` | ✅ | Force cloud LLM (Gemini); selects the backend in `LlmController` (cloud is also used automatically when no local GPU) |
| `General/skippedUpdateVersion` | _(empty)_ | ✅ | Release version the user chose to skip; suppresses the update banner (`PpUpdateBanner.qml`) for that version |
| `General/autoSaveSession` | `true` | 📋 | Auto-save session data on capture end (persisted; no consumer yet) |
| `General/aiCoachingOnSessionEnd` | `true` | 📋 | Auto-generate a Claude coaching observation after each session |
| `General/checkForUpdates` | `true` | ✅ | Gate for the launch / automatic update check; read by `update_controller`, `linux_appimage_backend`, `win_sparkle_update` |
| `General/sendDiagnostics` | `false` | 📋 | Send anonymous crash/performance data |

**Display**

| Key | Default | Status | What |
|---|---|---|---|
| `display/mainDisplayMode` | `"primary"` | ✅ | Where to open the main window (`"primary"`, `"cursor"`, `"screen:<n>"`) |
| `display/rememberWindowGeometry` | `true` | ✅ | Restore exact window position and size from previous session |
| `display/secondaryDisplayMode` | `"none"` | 📋 | Secondary output the session diagnostics panel is cast to (`"none"` or `"screen:<n>"`) |
| `display/postShotContent` | `"replay"` | 📋 | Legacy, unread — chose replay vs metrics when the cast was the per-shot dashboard; kept so stored profiles survive |
| `display/postShotDelay` | `0.5` | 📋 | Seconds after a swing before the cast window appears |
| `display/postShotMirror` | `false` | 📋 | Mirror the cast horizontally, for a coach standing opposite the athlete |
| `display/uiFrameRateCap` | `"display"` | 📋 | UI render rate cap (`"display"` = match monitor refresh, or explicit Hz) |
| `display/hardwareAcceleration` | `true` | 📋 | Use GPU-accelerated rendering |

**Camera** — per-camera values are maps keyed by the camera's persistent serial-number key

| Key | Default | Status | What |
|---|---|---|---|
| `camera/excluded` | _(empty list)_ | ✅ | Serial-number keys of cameras excluded from capture |
| `camera/targetFps` | _(empty map)_ | ✅ | Per-camera frame-rate target; key → fps value |
| `camera/triggerMode` | _(empty map)_ | ✅ | Per-camera trigger mode; key → `"freerun"` or `"hwsync"` |
| `camera/roi` | _(empty map)_ | ✅ | Per-camera ROI; key → normalised `{x, y, w, h}` rect |
| `camera/perspective` | _(empty map)_ | ✅ | Per-camera view assignment; key → `0` (unassigned), `1` (down-the-line), `2` (face-on), `3` (other) |
| `camera/isMirrored` | _(empty map)_ | ✅ | Per-camera mirror flag; key → `true` when the camera delivers a horizontally mirrored image (typical webcam); absent for non-mirrored industrial cameras. Controls x-axis convention in `BodyPoseAdapter`. |
| `camera/fixedInPlace` | _(empty map)_ | ✅ | Per-camera wall-mount flag; a non-fixed connected camera drives the session toolbar's "calibrate" attention. Read by the session wizard and toolbar; not yet by capture |
| `camera/ballRoi` | _(empty map)_ | ✅ | Per-camera ball-detection ROI; key → normalised rect; read by `CameraManager` / `CameraInstance` |
| `camera/alias` | _(empty map)_ | ✅ | Per-camera display alias; key → name; read by `CameraManager`, `CameraInstance`, resource monitor, and the exporter |
| `camera/preroll` | `1.0` | 📋 | Pre-roll buffer in seconds (0.5 / 1.0 / 2.0); ring buffer still sized at fixed 5 s |
| `camera/syncEnabled` | `true` | 📋 | Lock frame timing across all enabled cameras |

**IMU** — per-device values are maps keyed by the device MAC address / UUID

| Key | Default | Status | What |
|---|---|---|---|
| `imu/excluded` | _(empty list)_ | ✅ | MAC addresses of IMUs excluded from the device list and auto-connect |
| `imu/orientationFilter` | `"Madgwick"` | ✅ | Global software orientation-fusion filter (`"Madgwick"` or `"ESKF"`); applied to all connected IMUs immediately |
| `imu/outputRateHz` | _(empty map)_ | ⚙️ | Per-device output rate; applied immediately when chip is tapped, not restored on reconnect |
| `imu/placement` | _(empty map)_ | ✅ | Per-device body placement; read by session wizard and resource monitor |
| `imu/alias` | _(empty map)_ | ✅ | Per-device display alias; read by `ImuManager` + `swing_exporter` |
| `imu/calibration` | _(empty map)_ | 📋 | Per-device calibration payload; persisted but no consumer yet |
| `imu/defaultFusionMode` | `"9axis"` | ✅ | Default fusion-mode label; read by the exporter (`shot_processor`) to stamp fusion provenance into `swing.json` when the per-device map has no entry |
| `imu/fusionMode` | _(empty map)_ | ✅ | Per-device fusion-mode label; read by the exporter for `swing.json` provenance (falls back to `imu/defaultFusionMode`). The per-device UI chips were removed, so the map is normally empty |
| `imu/mountOrientation` | _(empty map)_ | 📋 | Unused — backing key for the removed per-device mount chips; connect always forces vertical mount |
| `imu/autoConnect` | `true` | 📋 | Connect all enabled IMUs automatically before recording begins |
| `imu/autoReconnect` | `true` | 📋 | Attempt reconnect if the BLE link drops during a session |
| `imu/saveCalibrationToFlash` | `false` | 📋 | Persist zero-orientation and mag calibration to device flash |

**Session & view layout**

| Key | Default | Status | What |
|---|---|---|---|
| `session/goalsByType` | _(empty map)_ | ✅ | Per-session-type speed goals; key → target mph value |
| `session/lastType` | `0` | ✅ | Index of the last-used session type; pre-selects on next wizard open |
| `view/layoutByMode` | _(empty map)_ | ✅ | Session-stage layout per mode (`0`=Capture, `1`=Review, `2`=Analyse); read by `ViewLayout.qml` |
| `view/dataRegionByType` | _(empty map)_ | ✅ | Active data-viewer region per session type (Axial/Lower/Upper/Delivery/Custom); read by `PpDataViewer.qml` |
| `view/sectionCollapse` | _(empty map)_ | ✅ | Collapsed/expanded section state per screen+mode; read by `PpDataViewer` + `PpMetricChart` |
| `view/panelsByType` | _(empty map)_ | 📋 | Enabled panels per type — persisted, no consumer (vestigial; superseded by `view/layoutByMode`) |
| `view/arrangementByType` | _(empty map)_ | 📋 | Panel arrangement (tabs/split/stage) per type — persisted, no consumer (vestigial) |
| `view/presetByType` | _(empty map)_ | 📋 | Named layout preset per type — persisted, no consumer (vestigial) |

**Storage** — honored by the swing exporter (each shot writes one `swing.json` + per-camera clips into the session folder); see the [Swing Export Developer Guide](docs/developer/swing_export_developer_guide.md)

| Key | Default | Status | What |
|---|---|---|---|
| `storage/sessionNamingPattern` | `"date-name-type"` | ✅ | Session-folder name format (`"date-name-type"`, `"date-type-name"`, `"name-date-type"`, `"date-only"`); composed by `SwingPaths` |
| `storage/videoResolutionMode` | `"native"` | ✅ | Export-time resolution (`"4k"`, `"1080p"`, `"native"`, `"half"`); downscale only — never upscales |
| `storage/videoCodec` | `"h264"` | ✅ | Encoding codec (`"h264"` → libx264, `"h265"` → libx265); a legacy `"prores"`/`"raw"` value is coerced to `"h264"` on load |
| `storage/videoQuality` | `"medium"` | ✅ | Encoding quality → CRF (`"low"`=28, `"medium"`=23, `"high"`=18, `"lossless"`=0) |
| `storage/videoContainer` | `"mp4"` | ✅ | Container / clip extension (`"mp4"`, `"mov"`, `"mkv"`); selects the muxer |
| `storage/saveRawFrames` | `false` | ✅ | Also dump undecoded sensor payloads to an `<alias>.raw` sidecar per camera |
| `storage/skipAnalysisForRawCapture` | `false` | ✅ | Corpus capture: with raw-frame saving on, skip the analysis pass (capture-only); read by `ShotProcessor` (`saveRawFrames() && skipAnalysisForRawCapture()`) |
| `storage/savePoseKeypoints` | `true` | ✅ | Gate is wired — the exporter serialises pose streams when present, but no pose producer exists yet, so nothing is written today |
| `storage/saveImuStreams` | `true` | ✅ | Embed IMU quaternion/accelerometer streams in `swing.json` |
| `storage/imuDataFormat` | `"json"` | ✅ | IMU export format (`"json"` inline, or `"csv"`/`"binary"` sidecar) |
| `storage/saveLaunchMonitorData` | `true` | ✅ | Write a connected launch monitor's readings into each swing. Off means readings are read and discarded — nothing lands in `swing.json` and none of the `lm.*` metrics appear |

**Launch monitor** — the device connector (Settings → Launch Monitor)

| Key | Default | Status | What |
|---|---|---|---|
| `launchmonitor/kind` | `"none"` | ✅ | Which connector: `"none"`, or `"gcquad"` for a Foresight GC Quad read through FSX2020 |
| `launchmonitor/path` | `""` | ✅ | The **folder** FSX2020 writes `LastShot.CSV` into (not the file). Usually a share from the Windows machine running it — the connector itself is platform-agnostic |
| `launchmonitor/pollIntervalMs` | `250` | ✅ | How often that folder is re-read, 50–10000 ms. Raise it only for a slow or busy network share |
| `launchmonitor/chimeEnabled` | `true` | ✅ | Play a short quiet tone when a reading is folded into a swing. Independent of the shot chime, which fires seconds earlier |
| `launchmonitor/standaloneShots` | `false` | ✅ | Create a swing from the monitor's reading alone when no camera or IMU saw it — no video, no analysis, only its own measurements. Only while **capture is active**, with an athlete selected and a session running: recording a shot is a question about what the user is doing, and with no devices the buffer cannot answer it. Off by default |

**Testing the connector without a launch monitor.** `touch` will not do it — the connector treats
byte-identical contents as the same shot, correctly, since nothing about the file has changed. Use
the generator, which copies the real header verbatim from an exemplar and writes a genuinely
different row:

```bash
python3 tools/launchmonitor/fake_shot.py <the folder the connector watches>
python3 tools/launchmonitor/fake_shot.py <folder> --shots 6 --interval 4     # a session
python3 tools/launchmonitor/fake_shot.py <folder> --club Drv --shape slice   # a shaped shot
```

Shot ids continue from whatever is already in the target file, and the numbers stay internally
consistent — face-to-path really is face minus path, total spin is the resultant of back and side,
and the shape asked for is the shape the spin axis and the offline distance describe.

**Athletes** — one group per athlete, keyed by UUID (`athletes/<uuid>/…`)

| Key | Default | What |
|---|---|---|
| `currentAthleteUuid` | _(none)_ | UUID of the currently selected athlete |
| `athletes/<uuid>/name` | — | Full display name |
| `athletes/<uuid>/handedness` | `"Right"` | `"Right"` or `"Left"` |
| `athletes/<uuid>/heightValue` | `0.0` | Height stored in ft regardless of entry unit |
| `athletes/<uuid>/heightUnit` | `"ft"` | Unit used when the value was entered (`"ft"` or `"cm"`) |
| `athletes/<uuid>/weightValue` | `0.0` | Weight stored in lb regardless of entry unit |
| `athletes/<uuid>/weightUnit` | `"lb"` | Unit used when the value was entered (`"lb"` or `"kg"`) |
| `athletes/<uuid>/handicap` | `-999.0` | Golf handicap index (`-999.0` = not set) |
| `athletes/<uuid>/primaryClub` | `"Driver"` | Default club |
| `athletes/<uuid>/speedTarget` | `0.0` | Driver speed target in mph (`0.0` = not set) |
| `athletes/<uuid>/notes` | _(empty)_ | Free-text notes/tags |
| `athletes/<uuid>/createdAt` | — | Unix epoch seconds; set once at creation |
| `athletes/<uuid>/lastSessionAt` | `0` | Unix epoch seconds; updated after each session |
| `athletes/<uuid>/sessionCount` | `0` | Running count of completed sessions |

**STT**

| Key | Default | What |
|---|---|---|
| `stt/modelPath` | _(empty)_ | Manual override for the Whisper model path; takes priority over the platform app-data and executable-adjacent locations |

**Secrets** — all loaded at startup from env vars and persisted so subsequent launches work without the original env var

| Key | Env var | What |
|---|---|---|
| `secrets/assemblyaiApiKey` | `ASSEMBLYAI_API_KEY` | AssemblyAI streaming STT key (also settable via cmake `-DASSEMBLYAI_API_KEY=`) |
| `secrets/azureTtsApiKey` | `AZURE_TTS_API_KEY` | Azure Cognitive Services key for TTS (also covers STT if no dedicated STT key is set) |
| `secrets/azureSttApiKey` | `AZURE_STT_API_KEY` | Azure Cognitive Services key for STT (overrides `azureTtsApiKey` when present) |
| `secrets/geminiApiKey` | `GEMINI_API_KEY` | Google Gemini key for cloud AI coaching (LLM); read by `LlmController` / `GeminiLlmEngine` |

> **Note:** Keys written to settings persist even after the env var is removed. To clear a key, delete the relevant `secrets/` entry from the settings file directly (see `SecretsManager` in `src/Secrets/`).

### Next to the executable
`<install dir>/models/`

| File | What |
|---|---|
| `movenet_singlepose_lightning.onnx` | MoveNet Lightning pose model (~9 MB) |
| `movenet_singlepose_thunder.onnx` | MoveNet Thunder pose model (~30 MB) |
| `vitpose-b-wholebody.onnx` | ViTPose-B whole-body pose model (~330 MB) — present when `WITH_VITPOSE=ON` |
| `u2netp.onnx` | Person segmentation model (~4.7 MB) |
| `yt-dlp` / `yt-dlp.exe` | Bundled yt-dlp binary for YouTube download |

These are copied from the CMake build cache automatically — no manual placement needed.

### User home directory (on demand)

| File | What | Trigger |
|---|---|---|
| `~/pinpoint_audio_<timestamp>.wav` | Recorded audio session | Save Audio button |
| `~/imu_log_<MAC>_<timestamp>.txt` | IMU session log (one per device) | Save Log button |

---

## Network activity

PinPoint Studio is **local-first**: capture, pose estimation, swing analysis, speech-to-text, text-to-speech, and the AI coach all run on-device by default. No swing data, audio, or video is ever sent to a third party unless you turn on a cloud backend and supply the matching API key. There is **no telemetry, analytics, or crash reporting** of any kind.

The table below lists every point at which the application opens a network connection, what it is for, and when it happens.

**Classification** — 🟢 always-on by default (no key or opt-in needed) · 📦 automatic first-run (downloads a model the app needs, once) · ⚙️ configured (only with a Settings toggle on *and* an API key present) · 🔵 optional (only on an explicit user action)

| Activity | Destination | Scope | Sends | When | Class |
|---|---|---|---|---|---|
| **Update check** | `api.github.com` / GitHub releases (Linux); `github.com/.../appcast-{win,mac}.xml` (Windows / macOS) | Internet | Nothing (GET only) | ~4 s after launch, installed builds only; `General/checkForUpdates` (default **on**) | 🟢 |
| **Update download** | GitHub release assets (AppImage zsync / signed installer + signature) | Internet | Nothing (GET only) | After you accept an offered update | 🔵 |
| **Kokoro TTS model** | `huggingface.co/onnx-community/Kokoro-82M-ONNX` | Internet | Nothing (GET only) | First run, when the model isn't cached and cloud TTS is off | 📦 |
| **Phi-4 LLM model** (~4.9 GB) | `huggingface.co/microsoft/Phi-4-mini-instruct-onnx` | Internet | Nothing (GET only) | First run, **only if a local GPU is present** and the model isn't cached | 📦 |
| **Azure Speech — STT** | `ukwest.stt.speech.microsoft.com` | Internet | **Microphone audio** + key | Per utterance, while cloud STT is selected | ⚙️ `cloudFallbackStt` (default off) + Azure key |
| **Azure Speech — TTS** | `ukwest.tts.speech.microsoft.com` | Internet | **Text to speak** + key | Per synthesis, while cloud TTS is selected | ⚙️ `cloudFallbackTts` (default off) + Azure key |
| **Gemini — AI coach** | `generativelanguage.googleapis.com` | Internet | **Coach conversation** + prompt + key | Per coach message | ⚙️ `cloudFallbackLlm` **or no local GPU**, + Gemini key |
| **Film download** | YouTube etc. via the bundled `yt-dlp` binary | Internet | Video URL; optionally your **browser cookies** | When you download a video on the Film page | 🔵 |
| **Industrial cameras** | GigE Vision camera over Ethernet (Aravis / Spinnaker) | Local LAN | Camera control + video stream | When such a camera is connected and started | 🔵 |
| **GPU runtime page** | `github.com/.../releases/latest` | Internet | Opens your **browser** (not an in-app connection) | When you tap *Get GPU runtime* (Windows + CUDA only) | 🔵 |

Notes:

- **Cloud features are off out of the box.** STT, TTS, and the AI coach all run locally by default; the only content that ever leaves the device — audio, spoken text, or coach conversation — does so through the three ⚙️ rows above, each gated on both a Settings toggle and a stored API key.
- **The one automatic exception is the AI coach on a machine with no GPU.** With no local GPU there is no on-device LLM, so if a Gemini key is configured the coach uses Gemini cloud automatically (without the `cloudFallbackLlm` toggle); with no key, the coach is simply unavailable.
- **Model downloads are bootstrap, not telemetry.** On first run the app fetches its local AI models from HuggingFace (Kokoro TTS always; the Phi-4 coach model only when a GPU can run it). These are plain downloads, cached under the app-data directory, and never re-fetched once present.
- **Update checks default on**, but only run from an installed build, send no data beyond the HTTP request, and can be turned off (General → check for updates). A downloaded update is GPG / EdDSA signature-verified against a pinned key before it is applied.
- **API keys in transit:** Azure keys travel in request headers; the Gemini key is passed as a URL query parameter — keep it out of shared logs or proxies.
- **Not network traffic:** the WT901 IMU connects over **Bluetooth LE**, not IP. The bundled ONNX Runtime execution providers and the `yt-dlp` binary are fetched at *build* time by CMake, not at runtime.
- **Inactive code:** an AssemblyAI streaming-STT backend (`wss://streaming.assemblyai.com`) is compiled in but not wired into backend selection, so it never connects in the current build.

---

## Roadmap

- **Session recording** — attach the persisted per-shot history to the selected athlete and session model (per-shot capture, analysis, and `swing.json` persistence are already in place)
- **Two-camera 3D pose reconstruction** — triangulate occluded joints from a second viewpoint (multi-camera capture is already in place)
- **Kinematic metric extraction** — extend beyond the Wrist analyzer (live lead-arm wrist angles already shipped) to club head speed, hip/shoulder rotation, and lag angle from pose sequences and IMU data
- **AI coach integration** — session-aware coaching output in the Coach mode
- **GRF mode** — connect hip-IMU data to the athlete and session model (Home screen entry point and device requirements already in place)
- **Smartphone companion** — once core concepts are proven on desktop

It will be published as an open-source desktop application for use in golf studios and coaching facilities.

---

## Backlog

Pending or deferred development gathered from the design and implementation documents — work that was started, planned, or explicitly deferred but not yet finished. Grouped by area; see the referenced doc under `docs/` for detail.

### Shot Analyzer / Pose / Metrics

| Document | Pending / Deferred Item |
|---|---|
| shot_analyzer_design | M2 — single-camera metric calibration + monocular lift (MotionBERT) |
| shot_analyzer_design | M3 — second-camera triangulation + IMU slot-map fix |
| shot_analyzer_design | M4 — temporal smoothing + IK skeleton solver |
| shot_analyzer_design | M5 — reference-swing scoring & ranked fault feedback |
| shot_analyzer_design | M6 — cross-platform validation & performance polish |
| shot_analyzer_design | Learned motion priors (conv-autoencoder/ReMP) for gap fill |
| shot_analyzer_design | Extrinsic polish via Ceres bundle adjustment (MVP uses raw solvePnP) |
| shot_analyzer_design | Learned GolfDB/SwingNet ONNX phase-segmentation event detector |
| shot_analyzer_design | Band calibration vs coach ratings (corpus work) |
| shot_analyzer_m1_wrist | Phase 3 in-replay metric graph — shipped; hardware/real-shot verification pending |
| shot_analyzer_m1_wrist | Right-arm sign verification for left-handed golfers |
| shot_analyzer_m1_wrist | Finalize kWristBands centres/σ from real-swing data (currently provisional) |
| shot_analyzer_m1_wrist | Exact quaternion-referenced Δ-from-address curve |
| shot_analyzer_m1_wrist | Body-schematic 3D Y-bot visualization |
| shot_analyzer_viz | Exact quaternion-referenced Δ curve (deferred) |
| shot_analyzer_developer_guide | Swing / GRF / Coach analyzers still deterministic stubs awaiting pipelines |

### Wrist / Motion Assessment

| Document | Pending / Deferred Item |
|---|---|
| wristmotion_assessment_design | Trail-side full IMU instrumentation (F9–F11; awaits trail/shoulder producers) |
| wristmotion_assessment_design | PlayerBaselineBandProvider (reference player's own shots) |
| wristmotion_assessment_design | Phase 4 — population/ML-tuned reference bands |
| wristmotion_assessment_design | Phase 4 — drill-library integration |
| wristmotion_assessment_design | Phase 4 — club-specific ideal models / per-club band tuning |
| wrist_assessment | Per-athlete reference scope (currently global) + carousel "set as reference" |

### Shaft Detection / Tracking

| Document | Pending / Deferred Item |
|---|---|
| shaft_detection_skeleton_impl | K5 corpus validation & flag-flips (data-gated) |
| shaft_detection_skeleton_impl | R8-T2 temporal-difference faint-smear detector (blur-first follow-on) |
| shaft_detection_skeleton_design | Per-subject arm length from athlete profile (R1) |
| shaft_detection_skeleton_design | Lead-arm ID when handedness metadata is plumbed |
| shaft_detection_skeleton_design | DTL extension for shaft detection (R2/R6, addendum B.10) |
| shaft_detection_skeleton_design | Learned wrist-cock correlation table from IMU calibration (R6) |
| shaft_detection_skeleton_design | 3D swing-plane projection of wrist-cock angle (R6) |
| shaft_detection_skeleton_design | Putting/chipping gating of full-swing beta curve (R6) |
| shaft_tracker_impl | S5 hardware verification (overlay registration, threshold tuning, θ̇ correlation >0.9) |

### Ball Detection

| Document | Pending / Deferred Item |
|---|---|
| ball_detector_design | Phase 2 — YOLO model training & hosting |
| ball_detector_design | Phase 3 — offline trajectory refinement worker + ShotProcessor integration |
| ball_detector_design | Phase 4 — ball-based shot triggering via ballLaunched hook |
| ball_detection_calibration | Studio/hardware field validation (B5) before deprecating legacy Hough path |
| ball_detection_calibration | Ball-detection CNN + Kalman flight tracking (planned, not started) |

### Shot Detection

| Document | Pending / Deferred Item |
|---|---|
| shot_detection_impl | Phase 2.5 — audio as EventBuffer source (offline acoustic re-pinpointing) |
| shot_detection_impl | Phase 4 — audio-in-ring + ML IMU detector (not started) |
| shot_detector_developer_guide | Vision modality `ballLaunched` producer (detector live, launch hook not) |
| shot_detector_developer_guide | Phase 4 — auto-calibrate per-source latency via peak cross-correlation |
| shotdetection | Vision shot-detection latency/accuracy benchmarking (needs golf validation) |
| shotdetection | Cross-source clock alignment + fusion/arbitration policy (open questions) |

### IMU

| Document | Pending / Deferred Item |
|---|---|
| imu_rearchitecture | Phase 3 native-quaternion / 9-axis (optional, hardware-gated) |
| imu_rearchitecture | Open questions #1–8 need hardware verification |
| imu_pipeline_audit | Studio/hardware + SwingLab-replay validation pending (incl. R3-3 power-off → rescan chip-drop) |
| imu_pipeline_audit | R1-4 stop() watchdog (Windows) — deferred until a WinRT shutdown wedge is observed |
| imu_io_thread_impl | W4 studio validation pass |

### Segmentation / Export / Replay

| Document | Pending / Deferred Item |
|---|---|
| segmentation_v3_impl | S5/G6 ShaftTracker hardware validation; G5 vision-only fallback ladder (may defer) |
| swing_export_developer_guide | Frame restoration/denoise/sharpen hook (TODO, not implemented) |
| swing_export_developer_guide | Pose / metrics / launch-monitor streams schema-ready but no producer |
| swing_export_developer_guide | 12/16-bit Bayer handling; MJPEG/H264_NAL passthrough; per-camera parallel encode |
| event_buffer_design | Mobile profile config; hardware-sync placeholders; per-camera exposure estimate |

### SwingLab / Corpus

| Document | Pending / Deferred Item |
|---|---|
| swinglab_impl | L0 real-data missions await clean corpus v1 (pre-2026-06-11 recordings unreliable) |
| swinglab_developer_guide | First real-data missions pending clean corpus v1 |

### Platform Updates / Release (Linux / macOS / Windows)

| Document | Pending / Deferred Item |
|---|---|
| linux_update_impl | P0 clean-VM acceptance (BLE/cameras/GPU/x264/sig validate) pending |
| linux_update | Channels (Stable/Beta) deferred to GA; embed libappimageupdate (future) |
| macos_update_impl | S1 clean-second-Mac acceptance + real update-offer test (needs v>alpha3) |
| macos_update | Native arm64 feed (GA, currently x86_64/Rosetta); binary deltas (GA) |
| windows_update_impl | P3 CUDA AppId split clean-VM validation; one-click GPU fetch deferred to GA |
| windows_update | Authenticode signing & channels deferred to GA |
| vendor_neutral_gpu | Phase 0 WebGPU validation spike not started; DirectML wiring as fallback only |

### Tooling / Tests / Misc

| Document | Pending / Deferred Item |
|---|---|
| testing_developer_guide | Step 5 — add umbrella to CI; migrate Buffer/tests off private helpers |
| .claude/videoissue | Single-copy display/buffer frame path + format-conversion path not implemented |
