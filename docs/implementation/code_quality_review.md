# Code Quality & Refactoring Review

**Date:** 2026-06-20
**Scope:** Full `src/` tree (~56k LOC, 15 subsystems), reviewed folder-by-folder for duplication, poorly-designed patterns, and maintainability. Build artefacts (`src/Buffer/build/`, vendored googletest) and `*/tests/` excluded.
**Method:** One assessment agent per major folder, each applying the same rubric (duplication / design / maintainability) and citing `file:line`. This document synthesises their findings.

---

## Executive summary

The codebase is, on the whole, **well-engineered and unusually well-commented**. The documented base+backend / thread-owning-controller architecture (see `CLAUDE.md`) is applied consistently and deliberately across hardware subsystems, the concurrency-critical code (`src/Buffer`, the profiler, the BLE transport) shows sound memory-ordering discipline, and the Phase-0 wrist-assessment engine (`src/Analysis`) is genuinely cleanly layered.

The debt that exists is **concentrated and mostly mechanical**, not architectural. It falls into a small number of recurring themes:

1. **Copy-paste of shared mechanics** — the same thread teardown dance, ONNX EP cascade, PCM conversion, byte formatter, and math helpers are re-implemented 3–6× each, often with subtle drift between copies.
2. **A handful of god objects / monolith files** — `CameraInstance` (1721 lines), `shaft_tracker::track()` (~350 lines), and three 2000+-line QML screens carry too many responsibilities.
3. **Latent concurrency bugs** in otherwise-careful code — a few genuinely multi-writer counters use non-atomic read-modify-write.
4. **Dead / placeholder surface for unlanded features** left inline as normal-looking code (a fully-built but never-wired STT backend, speculative enum values, stubbed export hooks).
5. **Security/correctness smells** in secret handling and a stale hardcoded cloud model id.

None of this is alarming, but the duplication actively raises the cost and risk of every future change to the affected mechanics. The highest-leverage work is **extracting the half-dozen shared helpers** below — each deletes drift-prone copies across multiple subsystems.

### Top 10 highest-leverage actions

| # | Action | Removes duplication across | Severity |
|---|---|---|---|
| 1 | Shared worker-thread RAII helper (`spawn`/`drain` thread + `moveToThread` + join) | ~12 sites in `src/Gui`, mirrored in STT/TTS/Audio | High |
| 2 | `AudioConverter::floatToInt16`/`int16ToFloat` + `makeWav16` | Audio, STT (×2), TTS, LLM | High |
| 3 | Shared ONNX EP-cascade + model-path resolver in `PoseEstimatorBase` | 4 Pose estimators (one silently CPU-only) | High |
| 4 | `pinpoint::fmtBytes`/`fmtMs` in a Core header | `pp_profiler` + 2 Gui monitor controllers | High |
| 5 | `analysis_math.h` (`kPi`, `wrapPi`, `radToDeg`, `nearestIndex`) | 6 Analysis files; quaternion math in 3 QML files | High |
| 6 | Fix non-atomic multi-writer counters (`source_published_`, `BleAdapterPool::m_index`) | Buffer, IMU — **correctness** | High |
| 7 | QML: add `Theme.colorOnAccent` + `PpToggle`/`SettingsRow`/`PpGhostButton` | ~33 `"white"` literals, ~25 row scaffolds, settings panels | High |
| 8 | Decide fate of unwired surface (AssemblyAI backend, pose streams, SyncSource placeholders) | STT, Export, Buffer | High |
| 9 | Extract `CameraInstance` ball/pipeline concerns; split 3 QML monoliths | Gui | High |
| 10 | Single device-key (`"Model\|Id"`) + `isInstalledBuild()` helpers | 8 Gui sites; 3 Update sites | Medium |

---

## Cross-cutting themes

These patterns each span **multiple folders** and are the most valuable to address because one fix removes several copies.

### A. Thread lifecycle boilerplate (High)
The documented teardown idiom — `invokeMethod(... BlockingQueuedConnection)` to move an object back to the main thread, then `quit()`/`wait()`/`delete` — is hand-written ~12× across `src/Gui` (`camera_instance.cpp:491-546`, `transcription_controller.cpp:147-175`, `tts_controller.cpp:132-362`, `llm_controller.cpp:447-464`) and mirrored in the STT/TTS workers. `film_controller.cpp:221-229` already factors it into a local `drainThread()` lambda. **Promote a shared `WorkerThread` RAII wrapper** (pairs a worker with its thread, runs the documented teardown in its destructor) and a `spawnWorkerThread(name, obj)` helper. This is the single biggest source of duplicated *and* risk-bearing code in the GUI layer, and it makes the many early-return/backend-swap paths exception-safe.

### B. Float↔int16 PCM conversion (High)
The clamp-and-scale loop `max(-1,min(1,s))*32767` (and its inverse) is copy-pasted across `STTBackendAzure.cpp:135-138`, `STTBackendAssemblyAI.cpp:118-121`, `AzureTTSEngine.cpp:92-95`, `stt_processor.cpp:259-279`, and `AudioConverter.cpp:35-60`. `AudioConverter` is the natural home — **add `floatToInt16`/`int16ToFloat` and a `makeWav16` WAV-header builder** (currently buried in `STTBackendAzure.cpp:102-140`) and route all callers through it.

### C. ONNX session / EP-cascade duplication (High)
The CoreML→CUDA→DirectML execution-provider cascade is duplicated verbatim in `pose_estimator_movenet.cpp:135-188`, `pose_estimator_vitpose.cpp:160-213`, and `pose_estimator_mediapipe.cpp:145-201` — and `person_segmenter.cpp:96-98` **silently runs CPU-only with no cascade at all** (GPU host gets no GPU segmentation). The model-path `#ifdef` resolver is repeated 6×, the 30-sample timing window 3×, and the `OrtState` pimpl 4×. **Promote MediaPipe's `applyEpCascade()`, a `resolveModelPath()`, and a `RollingStats` member into `PoseEstimatorBase`.** Note this cascade is also the runtime side of the `vendor_neutral_gpu.md` strategy — centralising it is a prerequisite for that work.

### D. Duplicated numeric helpers (High)
- **π** redefined in 6 non-test files (`phase_signals.cpp:27`, `wrist_analyzer.cpp:42`, `shaft_tracker_math.cpp:30`, `shaft_track_assembly.cpp:28`, `shaft_tracker.cpp:55`, `shaft_kinematics.h:58`).
- **`wrapPi`** (wrap angle to (−π,π]) reimplemented ≥4× with *two different algorithms* (`fmod` vs `while`-loop), feeding angular Kalman updates — correctness-relevant drift.
- **`nextPow2`** implemented 3× in `src/Buffer` (`source_ring.cpp:31`, `:292`) and again in QML (`CamerasPanel.qml:1489`).
- **Quaternion slerp/Hamilton product** reimplemented in 3 QML files (`BodyVizView.qml:146`, `ImuCalibrationFlow.qml:209`, `ArmVizView.qml:115`), one without the dot-product clamp.
- **`nearestIndex`/`phaseValue`/`find(series,key)`** each duplicated across `metric_extractor`/`phase_segmenter`/`swing_scorer`/`wrist_analyzer`.

**Create `src/Analysis/analysis_math.h`** for the scalar helpers and **expose slerp/quat-mul as `Q_INVOKABLE`s** on the existing `viz_frame.h` so QML stops re-deriving them.

### E. Byte/duration formatters (High)
`fmtBytes`/`fmtMs` exist in `pp_profiler.cpp:128-147` and are re-implemented in `resource_monitor_controller.cpp:63` and `profiler_controller.cpp:56-57` (same `1073741824`/`1048576` thresholds), plus QML `StoragePanel.qml:35-51`. **Hoist `pinpoint::fmtBytes`/`fmtMs` into a Core `pp_format.h`** returning `QString`.

### F. Non-atomic multi-writer counters (High — correctness)
Several otherwise-careful concurrency sites use non-atomic read-modify-write on genuinely multi-writer state:
- `event_buffer.cpp:231` — `source_published_.store(source_published_.load()+1)` runs on *multiple* producer threads (multi-camera/IMU); a lost update can stall the merger's cold-path wakeup. Give `WaitFlag` a `fetch_add`.
- `ble_adapter_pool.cpp:52-57` — `m_adapters[m_index++ % size]` with a plain `int`, called concurrently from per-IMU I/O threads. Make `m_index` `std::atomic`.
- `device_enumerator.cpp:182-227` — `m_devices` (a `QList`) is appended/iterated with **no mutex**, while every sibling in Core locks its shared state. Guard it or assert main-thread-only.

### G. Repeated base+backend+factory scaffolding (Medium)
The base+worker+factory pattern is applied in parallel across Audio/STT/TTS and the Gui cloud-fallback controllers. The agents agreed the **STT/TTS worker/factory parallelism is intentional and should be left** (payload signatures genuinely differ). But two pieces *are* worth unifying: (1) the **cloud-fallback backend triad** (`applyCloudFallbackPref`/`refreshCloudAvailability`/`switchTo*`) is structurally identical in `tts_controller.cpp:201-370`, `transcription_controller.cpp:191-228`, `llm_controller.cpp:434-484` — a `CloudFallbackBackend` mixin would remove three copies of the subtle "key changed while staying cloud" rebuild guard; (2) `STTProcessor::swapBackend()` duplicates ~50 lines of its own constructor — extract `createWorker()`/`wireWorker()`.

### H. Duplicated identity/probe helpers (Medium)
- **Device key** `description + "|" + (serial?:id)` is open-coded in 8 Gui sites (`camera_manager.cpp:770`, `camera_instance.cpp:112`, `imu_manager.cpp:88,162,189`, `resource_monitor_controller.cpp:174,326`, `shot_processor.cpp:672`) — `camera_instance.cpp:692` even carries a "Must mirror CameraManager::cameraKey()" comment. Put it on `Device`.
- **`isInstalledBuild()`** (installer-presence probe) duplicated across `win_sparkle_update.cpp:102`, `cuda_runtime_controller.cpp:46` ("duplicated here"), `mac_sparkle_update.mm:120`. Move to `platform_install.h`.
- **HTTP/JSON/SSE client boilerplate** (QNAM created on worker thread, single in-flight `m_reply`, error filtering, SSE line-buffering) duplicated between `GeminiLlmEngine.cpp` and `STTBackendAzure.cpp`. A shared `HttpJsonClient`/`SseReader` would remove the triplicate.

### I. Dead / placeholder surface presented as live code (Medium)
Unlanded-feature scaffolding is scattered through the tree looking like normal code:
- `STTBackendAssemblyAI` — 290 lines, fully implemented, **never instantiated** (not in the factory; `assemblyaiApiKey` read nowhere). Wire it or delete it.
- `event_buffer`/`source_descriptor.h:33-44` — `HardwarePts`/`HardwareTrigger` `SyncSource` values and `sync_group`/`sync_source` fields have **zero readers**.
- `swing_exporter.cpp` — `restorer` is a bare TODO that hardcodes `"none"` (`:314,:449`); the entire `savePose`/`poseStreams` path always iterates an empty vector (no producer).
- `wt9011dcl_base.cpp:157` — `rawPacketReady` emitted (with a clock read) on every packet, **connected to nothing**; `BleImuTransport::scan()` public surface is dead.
- `mac/win/linux` update — `m_appImageAssetName` written never read (`update_controller.cpp:315`); the all-zero placeholder-key gate (`:425`) is dead the moment the real fingerprint is set (which it already is — a contradiction).

Each of these reads as a bug or an incomplete feature to a future maintainer. **Delete, or fence behind a clearly-named `// UNWIRED:` marker**, per the team's preference.

### J. `#ifdef` platform sprawl vs the base+backend pattern (Medium)
`src/Update` is the one subsystem that *doesn't* follow the house base+backend pattern: `UpdateController` **is** the Linux engine (state machine + GPG verify + zsync) *and* a dispatcher to the macOS/Windows shims, gated inline by `#if defined(Q_OS_WIN)...#elif Q_OS_MACOS`. Extracting an `UpdateBackend` interface with `Linux/Mac/Win` implementations would remove the `#ifdef`s from the constructor/`checkNow`/`shutdownUpdater` and let the Linux-only members leave the shared header — bringing it in line with Video/Audio/IMU/STT.

### K. Pervasive magic numbers (Low–Medium)
Tuning constants and hardware parameters are inline literals throughout, often bypassing the config mechanisms that exist for them: analyzer thresholds that bypass `analysis_tuning.h`/`RuleTuning` (`assessment_rules.cpp:104`, `reference_bands.cpp:97`), IMU scale factors `/32768*16.0f` copy-pasted across 5 parse functions (`wt9011dcl_base.cpp:240-346`), audio sample rates (`16000`/`24000`/`32767`) scattered across Audio/STT/TTS, encoder `fps=30`/`gop=10` specified in 3 places that stay in sync only by coincidence (`swing_exporter.cpp:254,446`), ViTPose tensor shapes with no model-load validation (`vitpose.cpp:43-54`). Named constants / config structs per subsystem.

---

## Per-subsystem findings

Severity tags: **[H]** High, **[M]** Medium, **[L]** Low. Citations are `file:line`.

### src/Gui — C++ controllers (~74 files)
Consistent base+backend pattern, exceptionally well-commented lifecycle reasoning — but realised by copy-paste, with two controllers grown into god objects.
- **[H]** Thread teardown boilerplate duplicated ~12× → shared `WorkerThread` helper (Theme A).
- **[H]** Cloud-fallback triad duplicated across TTS/STT/LLM controllers (Theme G).
- **[H]** `CameraInstance` is a god object — 1721 lines, 37 `Q_PROPERTY`s, `setupPipeline()` ~185 lines (`camera_instance.cpp:304-489`), ~15 ball-detection members. Extract the ball-presence/calibration concern and pipeline construction into collaborators.
- **[M]** `CameraManager`/`ImuManager` duplicate their entire selection/persistence/EventBuffer-producer choreography (`camera_manager.cpp:293-349` vs `imu_manager.cpp:316-386`) → templated `DeviceManager<Entry>` base.
- **[M]** `"Model|Id"` device key open-coded 8× (Theme H).
- **[M]** `AppSettings *s = m_appSettings ? m_appSettings : &fallback` idiom appears 32×; on **write** paths a null settings silently mutates a stack-local and discards the write (`camera_manager.cpp:478`). Single `settings()` accessor; writes should early-return/assert.
- **[M]** `CameraManager::cameraList()` ~105-line method mixes QVariantMap presentation with ring-buffer sizing math that mirrors `CameraInstance` (`camera_manager.cpp:127-231`).
- **[M]** Workers managed as raw pointers with manual `delete` against the doc's guidance → RAII owner.
- **[L]** Divergent deselect teardown (`QTimer::singleShot`+guard vs `deleteLater`); voice URL/list duplicated; `main.cpp` composition root has positional destruction-order coupling; inconsistent error-surfacing convention.

### src/Gui — QML (94 files)
Design system (`Theme.qml`) is genuinely strong and mostly well-used; problems are monolith screens, a few missing reusable components forcing copy-paste, and business/numeric logic living in QML.
- **[H]** Decompose monoliths: `ScreenSessionWizard.qml` (2407 lines, 8 inline step panels), `CamerasPanel.qml` (2110, inline `CameraDeviceRow` + ROI editor), `ImusPanel.qml` (1438), `ScreenResourceMonitor.qml` (1811) → extract sub-components.
- **[H]** No `Theme.colorOnAccent` token → `"white"` hardcoded 33× across 13 files (violates the design doc's "never hardcode a colour" rule).
- **[H]** Missing `PpToggle` (reimplemented 5+ ways), `SettingsRow` (row scaffold duplicated ~25×, inline `searchHighlight` ~102×), `PpGhostButton` (~8×); hand-rolled chips that should use `PpSegmentedControl`.
- **[H]** Quaternion math + IMU **calibration state machine and pass/fail thresholds** run in QML (`ImuCalibrationFlow.qml:302-363`) — belongs on a C++ `CalibrationController`.
- **[M]** Ring-buffer/byte-format math in QML will drift from C++ (`CamerasPanel.qml:1467-1593`); BLE connect-pacing timer in the wizard (`ScreenSessionWizard.qml:1248-1322`) — macOS-sensitive timing that belongs on `ImuManager`.
- **[M]** Anchors-inside-Layout mixing and `parent.parent` reach-throughs pervasive; non-theme colour literals that won't adapt across aesthetics; `BodyVizView.qml` 9-deep nesting with a likely copy-paste scale asymmetry (`:419` vs `:525`).
- **[L]** Magic int where enum used in same file (`PpCameraFrame.qml:387`); untokenised `sp(9)`; banned `Font.DemiBold` (`PpTransitTimeline.qml:234`).

### src/Analysis (~41 files)
Splits into a swing-reconstruction chain and a cleanly-layered wrist-assessment engine (the best-factored part of the tree). Issues are duplicated math, magic tuning constants, and confusing naming.
- **[H]** Two unrelated subsystems both called "wrist analysis" — `wrist_analyzer.*` (produces series) vs `wrist_assessment_*` (consumes them); `wrist_angles.h` vs `wrist_angle_sampler.h` especially confusable. Rename/relocate the assessment cluster (e.g. `src/Analysis/assessment/`).
- **[H]** π (6×) and `wrapPi` (≥4×, two algorithms) duplicated (Theme D).
- **[M]** `nearestIndex`/`phaseValue`/`find` duplicated; `shaft_tracker::track()` is a ~350-line god-function (two ~20-line `tuning::apply` blocks, scale ladder, detect loop, prediction, guardrail inline); `detectShaft()` ~175 lines with confessed-dead config fields.
- **[M]** Hardcoded analyzer/archetype thresholds bypass the existing `analysis_tuning.h`/`RuleTuning` mechanism (`assessment_rules.cpp:104`, `reference_bands.cpp:97`, `wrist_assessment_engine.cpp:37`).
- **[M]** `MetricSeries` keys are stringly-typed and matched in 4+ places with dual spellings (`forearmPronation` vs `leadForearmRot`) → a `metric_keys.h` of named constants.
- **[L]** Two parallel scoring systems with separate band tables; `wrist_angles.h` buries ~10 lines of math under 45 lines of duplicated convention prose; 5 separate full-enum switches over `PpJointDof` → single metadata table.

### src/Video (33 files)
Well-engineered for a hard problem (5 backends, hardware/software crop, Bayer GPU demosaic). Dominant issue is duplicated GenICam format mapping and a recurring per-frame copy.
- **[H]** Pixel-format-string→`PixelEncoding` ladder duplicated 4× and already drifted (`video_input_factory.cpp:137,216`, `VideoInputSpinnaker.cpp:445`, `VideoInputAravis.cpp:353`) → one `genicamStringToPixelFormat()`.
- **[H]** Per-frame double/triple copy; the documented "single copy" goal is unmet on non-Bayer paths (`VideoInputApple.mm:78`→`video_frame_processor.cpp:30`→`video_preprocessor_opencv.cpp:46`). Carry `QImage`/`cv::Mat` end-to-end.
- **[H]** Capability-query boilerplate (~300 lines) duplicated between `enumerateDevices()` and each backend's `queryCapabilities()` → static `queryCapabilitiesFromNodeMap()`.
- **[M]** `suspend()`/`resume()` has three different semantics across backends (Spinnaker/Aravis keep streaming); `frameFormat()` returns null in 3 backends (a virtual that lies); BGR-vs-RGB carried by `QImage::format()` sniffing not metadata (`video_preprocessor_opencv.cpp:81`); macOS software-crop may silently no-op via the `default:` branch (`frame_crop.cpp:107`).
- **[L]** CameraManager regression risk structurally still present (`availableDevices()` still registers macOS webcams as `QtMultimedia`; only the factory `#ifdef` avoids it); `void*` SDK handles with C-style casts → pimpl; dead `else` branches in `queryCapabilities` (`VideoInputSpinnaker.cpp:603`, `VideoInputAravis.cpp:385`).

### src/IMU (23 files)
Genuinely well-architected (clean base/transport/processor split, careful BLE state machine). Most findings are residual cleanup the in-flight rearchitecture deferred — several rearch concerns (the `worldToScene`/`toAnatomical` helper, frame split) are **already landed**.
- **[H]** `rawPacketReady` fully dead — emitted with a clock read every packet, connected to nothing (`wt9011dcl_base.cpp:157`).
- **[H]** Up to 3 `nowMicros()` reads per packet; the published/detector timestamp is a separate later read (`imu_instance.cpp:101`) — audit item R2-1 only half-fixed.
- **[H]** `fuseRawImu` contract doc is now actively wrong — header says dt is clock-derived and clamped; impl uses fixed nominal dt (`imu_base.h:143` vs `imu_base.cpp:59`).
- **[M]** `eulerToQuat` duplicated in base+BLE and dead on the live path; `/32768` scale factors inline-magic across 5 parse functions (Theme K); dead `scan()`/discovery surface; capabilities advertise Euler/quaternion/9-axis features the BLE path doesn't use; `BleAdapterPool` round-robin non-atomic (Theme F).
- **[L]** `OutputRate` enum has unused sub-Hz values; `BleAdapterPool::initialize()` not idempotent against hot-plugged dongles (relevant to the USB-BT500 gotcha); `dispatchReadResponse` ~85-line if-ladder with inline battery LUT.

### src/Pose (15 files)
Well-thought base contract; three estimators clearly written by copy-paste.
- **[H]** EP cascade ×3 + person_segmenter CPU-only; model-path resolver ×6; timing window ×3; `OrtState` ×4 (Theme C).
- **[M]** Skeleton/keypoint indices are stringly-numeric magic outside `src/Pose` (`video_overlay_pose.cpp:43`, `pose_runner.cpp:161`) instead of the `PoseJoint` enum → export a `kPoseEdges[]` table; debug `cv::imwrite` to `~/mediapipe_crop_debug.jpg` on the hot path (`mediapipe.cpp:709`); ViTPose hardcodes tensor shapes with no model-load shape validation (out-of-bounds risk on model swap); `m_ready`/`isReady()` duplicated per subclass → base.
- **[L]** `reloadModel(int)` silently maps bad input to Lightning; per-model I/O dims/normalisation constants scattered → per-model descriptor struct.

### src/Buffer (19 source files; `build/` correctly gitignored, 0 tracked)
Well-engineered lock-free SPSC rings with sound memory-ordering; findings are duplication, dead code, and one real race.
- **[H]** Non-atomic multi-writer increment of `source_published_` (`event_buffer.cpp:231`) (Theme F).
- **[M]** Dead `ReadHandle::ring_` field; `nextPow2` ×3; `emitReady`/`drainAll` near-duplicate (`event_buffer.cpp:448-493`); placeholder `SyncSource` values with zero readers; `BufferState::Stopping` declared, never set.
- **[L]** `forEachActiveSlot` slot-scan idiom repeated 7×; `getSlotByIndex`/`slotStride` DMA helpers with no production consumer; `snapshot()` should be `const`; `interpolateImu` dereferences private `sources_[imu_id]` with no bounds check (UB on stale id).

### src/Core (18 files)
Coherent shared kernel, thoughtful profiler — but a fleet of inconsistent singletons and one unlocked shared container.
- **[H]** `DeviceEnumerator::m_devices` mutated with no locking from multiple threads (Theme F); `fmtBytes`/`fmtMs` duplicated 3× (Theme E).
- **[M]** 8 hand-rolled Meyers singletons with inconsistent `&`-vs-`*` return; dead `m_videoEnumerated`/`m_audioInputEnumerated`/`m_audioOutputEnumerated` flags + empty `enumerate()` stub; Qt category suppression (incl. all `qt.bluetooth`) statically baked, not escapable without recompile (`pp_debug.cpp:87`); `ppDebug()` disabled form is a dangling-`if` that can swallow a trailing `else`; profiler "standard scope" seeding (doc Phase 3) not wired.
- **[L]** GPU `sample()`/`processGpuBytes()` duplicate the locked query; atomic-max idiom ×3; `pp_os_metrics` global baseline corrupts on a second concurrent sampler; non-trivial `static` fn in `ort_log.h`.

### src/Export (14 files)
Clean atomic-write/worker discipline; debt is repeated JSON scaffolding, a contradictory schema version, and schema-ready-but-unproduced surface.
- **[H]** Dead/contradictory schema version — exporter stamps `pinpoint.swing/1` (`swing_exporter.cpp:630`) but the writer overwrites with `/2` (`swing_doc.cpp:183`); literal duplicated 3×. The whole swing.json header is built twice (`swing_exporter.cpp:629-649` vs `shot_processor.cpp:717-750`).
- **[M]** `QSaveFile` open/write/commit scaffolding ×3; the `{count,t_us,data}` stream block open-coded per writer; stubbed `restorer`/pose hooks inline as live-looking branches (Theme I); `club` hardcoded `"DRIVER"` on reload — silent data fabrication (`swing_doc.cpp:254`); encoder `fps=30`/`gop=10` magic in 3 places (Theme K).
- **[L]** `kPerspectiveNames` table duplicated with magic `<=3` bound; `m_headerWritten` set never read; aliasing contract of `decodeToBgr` scratch enforced nowhere.

### src/Audio, src/STT, src/TTS (48 files)
Consistent, deliberate base+worker+factory with good thread/resource discipline. The cross-subsystem worker parallelism is **intentional — leave it**.
- **[H]** PCM float↔int16 conversion ×4–5 (Theme B); `STTBackendAssemblyAI` fully built, never wired (Theme I).
- **[M]** WAV-header builder trapped in one backend; `swapBackend()` duplicates its constructor; Azure-key fallback lookup duplicated; `AzureTTSEngine` bypasses its factory (inconsistent with Kokoro); magic audio constants (`16000`/`24000`/`256`) scattered.
- **[L]** `STTBackendFactory::create` returns null for unhandled enum and `stt_processor.cpp:63` dereferences without guard; Apple STT 30 s blocking semaphore fallback wants a `ppWarn`; Kokoro diagnostic dump runs every synthesis in all builds; naming convention differs per directory (`STTWorker` vs `TtsWorker`) — low priority per the intentional partial rename.

### src/Update (10 files)
Clean and well-documented, but the one subsystem not following the base+backend pattern.
- **[H]** `UpdateController` conflates the Linux engine with the cross-platform façade via `#ifdef` sprawl (Theme J).
- **[M]** `isInstalledBuild()` duplicated 3× (Theme H); placeholder-key "refuse to arm" gate reimplemented with 3 different sentinels across platforms (security-critical, easy to half-update); dead `m_appImageAssetName` + contradictory dead placeholder-key branch (Theme I); `download()` retry-from-`Error` reuses stale state without a clean-slate reset.
- **[L]** GitHub repo URL hardcoded in 3 files (a repo rename edits all three); GPG verify shells out with no `gpg`-presence check (missing tool → misleading "signature failed"); namespace-scope `const QString` constants incur static-init cost.

### src/LLM, src/Secrets (12 files)
Small, clean, sensible base/backend pattern — but real security smells.
- **[H]** Secrets stored in **plaintext QSettings** (`SecretsManager.cpp:47`) — world-readable to the user account, synced to backups; compile-time `-DASSEMBLYAI_API_KEY=` bakes a recoverable literal into the binary. Flag in-code; prefer the platform keychain. ⚠ **Updated 25 Aug 2026 — the same file now also holds PPCP pairing keys.** libppcp erratum E56 made `RV` 7.2c a SHOULD and moved the `PRK` into this INI, because the keychain store existed on macOS only and left Windows and Linux with no persistence and therefore no reconnection. The store narrows the file to `0600` on the first pairing write, which closes the *other accounts on the machine* half of this finding for every key in it, and closes none of the *plaintext* half. ⛔ **This finding stands and is now worth more, not less** — but 'prefer the platform keychain' is no longer the automatic answer: it is what E56 was raised to stop mandating, and any fix has to work on Windows and Linux too. Stale/likely-invalid default cloud model id `"gemma-4-26b-a4b-it"` (`GeminiLlmEngine.h:44`) — not a real API model, will 404 on first `chat()`.
- **[M]** API key embedded in the request **URL query string** (`GeminiLlmEngine.cpp:66`) — leaks into proxy/crash logs; use the `x-goog-api-key` header. HTTP/SSE client boilerplate duplicated with STT (Theme H); `LlmEngine::loadModel(modelDir)` contract is phrased around local files, forcing the cloud engine to take-and-ignore the arg; no `LlmBackendFactory` (single-vendor coupling).
- **[L]** `LlmWorker::loadModel` emits a generic error plus a separate real one; hardcoded prompt template + `max_length`/`maxOutputTokens`/`temperature` literals; `SecretsBridge::hasAzureKey` OR-coupling surprise.

---

## Suggested sequencing

**Phase 1 — shared helpers (low risk, high duplication payoff).** Themes A–E + H: the worker-thread RAII helper, `AudioConverter` PCM/WAV methods, `PoseEstimatorBase` ONNX/EP consolidation, `analysis_math.h`, Core `pp_format.h`, and the device-key/`isInstalledBuild` helpers. Each is a mechanical extraction with clear before/after and removes drift across multiple folders.

**Phase 2 — correctness.** Theme F: fix the three non-atomic/unlocked multi-writer sites (`source_published_`, `BleAdapterPool::m_index`, `DeviceEnumerator::m_devices`) and the `interpolateImu` bounds check. Small diffs, genuine latent bugs.

**Phase 3 — decide the fate of unwired surface.** Theme I: for each of AssemblyAI backend, pose streams, `SyncSource` placeholders, `restorer`, dead `scan()`/`rawPacketReady`, and the contradictory update placeholder-key gate — either wire it or delete it. This is partly a product decision; the review can only flag it.

**Phase 4 — decompose monoliths.** `CameraInstance`, `shaft_tracker::track()`, and the three QML screens; add the missing QML components (`Theme.colorOnAccent`, `PpToggle`, `SettingsRow`, `PpGhostButton`) and move calibration/ring-buffer/quaternion logic out of QML into C++. Larger, more invasive — do after the shared helpers exist.

**Phase 5 — structural.** Theme J: refactor `src/Update` to the base+backend pattern; address secret storage (`src/Secrets` → keychain) and the LLM model-id/key-in-URL issues.

> Several of these items overlap with already-tracked work in the design docs (the IMU rearchitecture, the `vendor_neutral_gpu` EP work, the macOS update Stage 2). Where that's the case it's noted in the subsystem section — those should be folded into the existing plans rather than tracked twice.
