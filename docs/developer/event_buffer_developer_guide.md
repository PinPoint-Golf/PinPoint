# Pinpoint Event Buffer — Developer Guide

**Audience**: Developers working on or integrating with the Pinpoint application  
**Location**: `src/Buffer/`  
**Language**: C++20  
**Status**: Production — shipping on Linux, macOS and Windows; 8 test targets +
a manual latency benchmark

---

## Contents

1. [What the Event Buffer Is](#1-what-the-event-buffer-is)
2. [Where It Fits in Pinpoint](#2-where-it-fits-in-pinpoint)
3. [Core Concepts](#3-core-concepts)
4. [Getting Started — A Complete Example](#4-getting-started--a-complete-example)
5. [Registering Sources](#5-registering-sources)
6. [Producing Data (Capture Threads)](#6-producing-data-capture-threads)
7. [The Buffer Lifecycle](#7-the-buffer-lifecycle)
8. [Consuming Data — Subscriptions](#8-consuming-data--subscriptions)
9. [The SwingWindow — Frozen Analysis View](#9-the-swingwindow--frozen-analysis-view)
10. [Diagnostics and Observability](#10-diagnostics-and-observability)
11. [Configuration](#11-configuration)
12. [Internals — How It Works](#12-internals--how-it-works)
13. [Platform-Specific Code and Why](#13-platform-specific-code-and-why)
14. [Common Mistakes](#14-common-mistakes)
15. [File Map](#15-file-map)

---

## 1. What the Event Buffer Is

The Pinpoint Event Buffer is a **pre-allocated, lock-free, circular buffer** that
ingests raw device data from multiple asynchronous sources — cameras and IMUs — and
presents it as a single unified, temporally-ordered stream.

It is designed around one specific requirement: **capture the last 5 seconds of
data continuously, then freeze it the instant a golf swing completes so analysis
can read it without any copying, locking, or race conditions.**

It is not a general-purpose message queue. It is not a database. It has no network
layer and no serialisation format. It stores raw device bytes exactly as they
arrived, tagged with a high-resolution timestamp, and keeps them available in a
ring that overwrites from the oldest end.

---

## 2. Where It Fits in Pinpoint

```
┌──────────────────────────────────────────────────────────────────────┐
│  Pinpoint Application                                                │
│                                                                      │
│  [CameraInstance]  ──publish──►   ┌─────────────────┐               │
│  [CameraInstance]  ──publish──►   │   EventBuffer   │               │
│  [ImuInstance]     ──publish──►   │  (src/Buffer/)  │               │
│                                   └────────┬────────┘               │
│                                            │                        │
│              ┌─────────────────────────────┤                        │
│              │                             │                        │
│         [Subscription]             [SwingWindow]                    │
│       (live consumers:          (owned by ShotProcessor;            │
│        UI preview,               read concurrently by               │
│        recorder)                 ShotAnalyzer + SwingExporter)      │
└──────────────────────────────────────────────────────────────────────┘
```

### The swing workflow

1. **App launch**: `EventBuffer::start()` is called in `main.cpp` with zero sources.
   The merger thread runs quietly, sleeping efficiently until devices are registered.
2. **Device selection**: The user selects cameras and IMUs from the UI.
   `CameraManager::setSelected()` briefly pauses the buffer, calls
   `registerSource()` (allocating ring memory once), then resumes. Ring memory
   stays allocated until the device is deselected — it is never freed between swings.
3. **Capture**: With sources registered, the buffer fills continuously. Camera frames
   and IMU packets are published into their respective rings. Old data is silently
   overwritten as rings wrap.
4. **Shot detection**: `ShotController` fires `shotDetected` (ball-launch / acoustic /
   IMU evidence — see `shot_detector_developer_guide.md`). `ShotProcessor` waits out
   a per-source post-roll so the follow-through lands in the ring, then calls
   `EventBuffer::pause()` — producers stop writing, the merger quiesces, rings
   freeze. The last 5 seconds are intact.
5. **Analysis**: `ShotProcessor` calls `EventBuffer::captureSwingWindow(4000ms)`,
   which returns a `SwingWindow` — a frozen, zero-copy view. `ShotAnalyzer` and
   `SwingExporter` then read that one window **concurrently** from two
   `QtConcurrent` workers (both `const`, both zero-copy) — frames and IMU samples
   come straight from ring memory.
6. **Dismiss**: After the workers join and the optional ¼× replay finishes, the
   `SwingWindow` is destroyed (ring memory stays allocated — only ring positions
   are reset), then `EventBuffer::resume()` clears the positions and restarts
   capture for the next swing.
7. **Device deselection**: When the user deselects a device, `CameraManager::setSelected()`
   pauses the buffer, calls `deregisterSource()` (freeing ring memory immediately),
   then resumes.
8. **App exit**: `aboutToQuit` fires `EventBuffer::stop()`, joining the merger thread.
   `~EventBuffer()` frees any remaining source rings.

The buffer is invisible to QML. Ownership splits three ways:

- The **managers** (`CameraManager`, `ImuManager`) own the buffer *state machine* —
  pause/resume sequencing and the register/deregister dance on device selection.
- The **per-device instances** (`CameraInstance`, `ImuInstance`) own the *data path*:
  each registers its own source on selection, publishes raw bytes from its
  capture/BLE thread, and deregisters on deselection.
- **`ShotProcessor`** (`src/Gui/shot/`) owns the *`SwingWindow`* for its entire
  lifetime. This migrated out of `CameraManager`: every resume path now funnels
  through `CameraManager::resumeBuffer()`, which refuses while
  `EventBuffer::swingWindowLive()` is true, so the window cannot outlive the
  freeze that makes it valid.

Note that the EventBuffer is a third, independent consumer of camera frames —
separate from both the pose/ball pipeline and the view fan-out (`CameraInstance`
publishes display frames to subscribed `QVideoSink`s; that path never touches the
ring). The `BufferController` exposes diagnostic properties to QML.

A `SwingWindow` is also produced **without** a live buffer: `SwingDiskLoader`
(`src/Analysis/swing_reanalyzer.cpp`) builds one from an exported swing folder so
offline re-analysis runs the identical analysis code. See §9.

---

## 3. Core Concepts

### Sources and SourceId

Every device that writes into the buffer registers as a **source** and receives a
`SourceId` — a small integer used for all subsequent operations. A source has a
`SourceDescriptor` that describes the device type, format, and expected data rate.
Sources come and go with device selection (register on select, deregister on
deselect); the buffer is briefly paused around each change.

A descriptor also carries an **`identifier`** — the device serial or an opaque
stable device id. It is what keys session-lifetime statistics (§10) and what lets
a window reader attribute a payload to a physical device, so it survives
deregistration where `SourceId` does not.

### SourceRing

Each source owns a **`SourceRing`** — a fixed-size, pre-allocated circular buffer
sized for that source's data rate and the configured window duration (default 5s).
Camera frames go in camera rings; IMU packets go in IMU rings. They are independent
— a fast IMU ring does not interact with a slow camera ring.

### TimelineIndex

A separate, lightweight **`TimelineIndex`** holds ordered references to events across
all source rings. Each entry is 40 bytes: a timestamp, a source ID, and a sequence
number pointing back into the source ring. The merger thread builds this in
timestamp order. Consumers read from here, then fetch payloads from source rings on
demand.

### Merger Thread

A dedicated background thread continuously polls each source ring for new events,
sorts them by timestamp using a k-way merge, applies a 5ms reorder window to absorb
cross-source clock jitter, and appends ordered `IndexEntry` records to the
`TimelineIndex`.

### SwingWindow

After `pause()`, `captureSwingWindow()` returns a **`SwingWindow`** — a frozen view
of a time range. It holds a snapshot of `IndexEntry` records (tiny — 40 bytes each)
and serves payloads through a **`SwingPayloadSource`** that it owns.

The payload source is the pluggable bit. Two backings exist, and the *same*
concrete `SwingWindow` type is fed by either:

| Backing | Where | Payload access |
|---|---|---|
| `RingPayloadSource` | `event_buffer.cpp`, built by `captureSwingWindow()` | Zero-copy from the frozen rings; seqlock-validated. Valid only while Paused. |
| `SwingDiskSource` | `src/Analysis/swing_reanalyzer.cpp`, built by `SwingDiskLoader` | Streams an exported swing folder; one decoded frame per camera resident at a time, IMU held in RAM. Always valid. |

That indirection is why offline re-analysis of an exported swing runs the *same*
analysis stages as live capture rather than a parallel code path.

---

## 4. Getting Started — A Complete Example

This example shows the full lifecycle from a C++ perspective.

```cpp
#include "event_buffer.h"
#include "source_descriptor.h"
#include "format_descriptor.h"

// ── 1. Create the buffer (lives in main.cpp alongside other controllers) ──
pinpoint::EventBuffer buffer;

// ── 2. Register sources (before start()) ──────────────────────────────────

// Camera source
pinpoint::SourceDescriptor camDesc;
camDesc.name = "left_camera";
camDesc.identifier = "SN-4412093";   // device serial — keys lifetime stats
camDesc.window_duration = std::chrono::milliseconds(5000);
camDesc.expected_interarrival_us = std::chrono::microseconds(33333); // 30fps

pinpoint::CameraFormat camFmt{};
camFmt.pixel_format        = pinpoint::PixelFormat::Mono8;
camFmt.width               = 1920;
camFmt.height              = 1080;
camFmt.fps_numerator       = 30;
camFmt.fps_denominator     = 1;
camFmt.max_payload_bytes   = 1920 * 1080;   // Mono8
camFmt.typical_payload_bytes = 1920 * 1080;
camFmt.plane_strides[0]    = 1920;          // bytes per row — NOT assumed from width
camDesc.format.device = pinpoint::DeviceKind::Camera_UVC;
camDesc.format.device_serial = camDesc.identifier;
camDesc.format.format = camFmt;

pinpoint::SourceId camId = buffer.registerSource(camDesc);

// IMU source
pinpoint::SourceDescriptor imuDesc;
imuDesc.name = "wt9011dcl_ble";
imuDesc.identifier = "C4:AC:59:11:22:33";
imuDesc.window_duration = std::chrono::milliseconds(5000);
imuDesc.expected_interarrival_us = std::chrono::microseconds(10000); // 100Hz

// IMU sources store DECODED samples, not raw device packets — the driver parses
// on the BLE thread and publishes a fixed-layout ImuSample (see §9).
pinpoint::ImuFormat imuFmt{};
imuFmt.device         = pinpoint::DeviceKind::IMU_WitMotion;
imuFmt.sample_rate_hz = 100;
imuFmt.packet_bytes   = sizeof(pinpoint::ImuSample);   // 40
imuFmt.packet_schema  = "imu_sample_v2";
imuDesc.format.device = pinpoint::DeviceKind::IMU_WitMotion;
imuDesc.format.format = imuFmt;

pinpoint::SourceId imuId = buffer.registerSource(imuDesc);

// ── 3. Start capture ──────────────────────────────────────────────────────
buffer.start();   // Idle → Capturing; launches merger thread

// ── 4. Producers publish from their capture threads ───────────────────────
// (This runs on a capture thread, not the main thread)
void onCameraFrame(const uint8_t* data, size_t len) {
    auto slot = buffer.acquireWriteSlot(camId);
    if (!slot.valid) return;                    // buffer is paused — discard
    std::memcpy(slot.data, data, len);
    *slot.bytes_written = len;
    *slot.timestamp_us  = pinpoint::EventBuffer::nowMicros();
    buffer.publish(camId, slot.sequence);
}

// ── 5. A consumer subscribes and reads events ─────────────────────────────
auto sub = buffer.subscribe();
pinpoint::IndexEntry entry;
while (sub.waitNext(entry, std::chrono::milliseconds(100))) {
    if (entry.flags & pinpoint::IndexEntryFlags::SourceStalled) {
        // A device went silent — handle accordingly
        continue;
    }
    auto handle = buffer.acquireReadHandle(entry.source_id, entry.source_sequence);
    if (!handle.data) continue;          // slot was overrun before we got here
    // use handle.data / handle.bytes ...
    if (!handle.validate(...)) continue; // verify no overwrite during read
}

// ── 6. Swing detected — freeze the buffer ────────────────────────────────
buffer.pause();   // Capturing → Paused

// ── 7. Extract and analyse the swing ─────────────────────────────────────
auto window = buffer.captureSwingWindow(std::chrono::milliseconds(4000));

for (const auto& e : window.entries()) {
    auto handle = window.payloadOf(e);
    // handle.data / handle.bytes — zero copy, guaranteed valid
    // while window is in scope and buffer is Paused
}

// ── 8. Resume for next swing ──────────────────────────────────────────────
// Destroy window first (best practice — payload access after resume undefined)
window = {};  // or let it go out of scope
buffer.resume();  // Paused → Capturing; rings cleared, fresh start

// ── 9. Stop at session end ────────────────────────────────────────────────
buffer.stop();    // → Idle; merger thread joined
```

---

## 5. Registering Sources

Registration is callable while `Idle` or `Paused`. The buffer does not need to be
Idle — device selection can happen at any point as long as the buffer is paused first.
`CameraManager::setSelected()` handles this automatically.

**Memory is allocated at registration and freed at deregistration** — never between
swings. This is deliberate: GB-scale ring allocations are expensive, and some operating
systems do not fully return freed memory to the system on `free()`. Keep rings warm.

### `deregisterSource()`

```cpp
// Deregister a source and immediately free its ring memory.
// ONLY callable while Paused.
// You MUST destroy any live SwingWindow before calling this.
// After this call 'id' is invalid — further calls with it are safe no-ops.
buffer.deregisterSource(id);
```

This is called automatically by `CameraInstance::deregisterFromBuffer()` when
`CameraManager::setSelected(index, false)` is called.

### `activeSourceCount()`

```cpp
size_t n = buffer.activeSourceCount();   // count of non-deregistered sources
```

### `SourceDescriptor` fields

| Field | Type | Purpose |
|---|---|---|
| `name` | `std::string` | Human-readable name for diagnostics |
| `identifier` | `std::string` | Device serial or opaque stable device id. Keys session-lifetime stats (§10) — survives deregistration. |
| `window_duration` | `chrono::milliseconds` | How much history to retain (default 5000ms) |
| `expected_interarrival_us` | `chrono::microseconds` | Used by liveness watchdog to detect stalls |
| `format.device` | `DeviceKind` | Camera, IMU, or `Marker_App` — see `types.h` |
| `format.device_serial` | `std::string` | Physical-device attribution for window readers. `registerSource()` falls back to `identifier` if this is empty. |
| `format.format` | `CameraFormat` or `ImuFormat` | Format-specific fields — see below |
| `sync_source` | `SyncSource` | Timestamping mode (always `SoftwareTimestamp` in v1) |
| `sync_group` | `optional<string>` | Hardware sync group (placeholder, unused in v1) |

`DeviceKind::Marker_App` is not a device: it registers a source the *application*
publishes into, so app-generated events (e.g. a shot-impact marker) land in the
same timeline as device data and are ordered against it by the merger.

### Camera format fields

| Field | Purpose |
|---|---|
| `pixel_format` | `PixelFormat` enum — e.g. `Mono8`, `BayerRG8`, `YUV422`. `pixel_format_names.h` maps it to/from the canonical `swing.json` string. |
| `width`, `height` | Frame dimensions in pixels |
| `fps_numerator`, `fps_denominator` | Frame rate as a fraction |
| `max_payload_bytes` | **Critical**: slots are sized to this. Must be ≥ largest frame you will ever publish. If a frame exceeds this, it is clamped and logged. |
| `typical_payload_bytes` | Used for diagnostics only |
| `plane_strides` | `array<uint32_t,4>` — bytes per row per plane. **Do not derive stride from `width`**: drivers pad rows, and a decoder that assumes `width` bytes shears the image. Set it from what the driver reports. |
| `exposure_us` | Per-stream exposure in microseconds; `0` = unknown. Motion-blur-aware stages read it. |
| `exposure_source` | `ExposureSource` provenance — `Measured` (read from the frame, manual exposure), `MeasuredAuto` (read from the frame, auto-exposure active so it varies), `Derived` (computed as `1/(2·fps)` — webcams that report nothing), `Unknown`. Round-trips through `swing.json` via `exposureSourceName()` / `exposureSourceFromName()`. |

### IMU format fields

| Field | Purpose |
|---|---|
| `device` | `DeviceKind` enum |
| `sample_rate_hz` | Nominal sample rate — informs watchdog threshold |
| `packet_bytes` | Fixed payload size per event. IMU sources publish **decoded** `ImuSample` records, so this is `sizeof(ImuSample)` (40), not the on-the-wire BLE packet size. |
| `packet_schema` | Opaque string identifying the payload layout. Currently `"imu_sample_v2"` — see `imu_sample.h`. |

### Slot count and memory

The ring for each source is allocated with `next_power_of_2(ceil(rate × window_seconds))`
slots, each `max_payload_bytes` in size. For a 30fps camera with 1080p Mono8 frames
over a 5s window:

```
slots = next_power_of_2(ceil(30 × 5)) = next_power_of_2(150) = 256
bytes = 256 × (1920 × 1080) = 256 × 2,073,600 ≈ 531 MB per camera
```

Register with accurate dimensions — over-estimating `fps_numerator` doubles the
slot count and wastes memory. The Pinpoint integration calls
`CameraInstance::updateBufferDescriptor()` once the camera starts and reports its
actual format, so registrations use real values rather than guesses.

---

## 6. Producing Data (Capture Threads)

### The four-step protocol

Every capture thread follows this protocol exactly:

```cpp
void onDeviceDataReady(/* device callback args */) {
    // Step 1: Timestamp FIRST — before any other work
    int64_t ts = pinpoint::EventBuffer::nowMicros();

    // Step 2: Acquire a slot — never blocks, never fails
    auto slot = buffer.acquireWriteSlot(sourceId);

    // Step 3: Check valid — false when buffer is paused
    if (!slot.valid) return;

    // Step 4: Write payload directly into slot.data
    std::memcpy(slot.data, deviceBytes, byteCount);
    *slot.bytes_written = byteCount;
    *slot.timestamp_us  = ts;

    // Step 5: Publish — validates, updates stats, releases to readers
    buffer.publish(sourceId, slot.sequence);
}
```

### Rules

**You MUST:**
- Call `nowMicros()` as the absolute first line
- Check `slot.valid` before writing
- Fill both `bytes_written` and `timestamp_us` before calling `publish()`
- Call `publish()` even for partial/short reads — record the actual size
- Use only `EventBuffer::nowMicros()` — no other clock source, ever

**You MUST NOT:**
- Allocate memory between acquire and publish
- Copy data through an intermediate buffer before the slot
- Hold any pointer to `slot.data` after calling `publish()` — the slot may be
  overwritten by the next `acquireWriteSlot()` call on the same ring
- Log synchronously inside the callback
- Call any lifecycle method (`start`, `pause`, `resume`, `stop`) from a capture thread

### `acquireWriteSlot` never blocks

If the buffer is paused, `slot.valid` is `false` and no ring state is modified —
the call returns immediately. If the ring is full (write head has lapped the read
head), the oldest slot is silently overwritten. This is the drop-oldest circular
behaviour: **producers are never stalled or slowed by consumers falling behind**.

### The clock

`EventBuffer::nowMicros()` returns microseconds since an arbitrary epoch using the
platform's highest-resolution monotonic clock:

- Linux / Android: `clock_gettime(CLOCK_MONOTONIC)` — ~20ns resolution
- Windows: `QueryPerformanceCounter` — ~100ns resolution
- macOS / iOS: `mach_absolute_time` — ~40ns resolution

It is lock-free, thread-safe, and never goes backwards. Every timestamp in the
system uses this single function. **Do not use `std::chrono::system_clock`,
`QDateTime::currentMSecsSinceEpoch()`, or any other time source** — mixing clock
sources corrupts the timeline ordering.

---

## 7. The Buffer Lifecycle

The buffer has four states. Transitions that are not listed are invalid and will
assert in debug builds.

**v6 model**: `start()` and `stop()` are app-level operations called once each
(in `main.cpp`). `pause()` and `resume()` are the capture gate — called
automatically by swing/ball detection. Device selection calls
`registerSource()`/`deregisterSource()` with a brief internal pause/resume.

```
  [constructed]
       │
    start()
       │
       ▼
   CAPTURING ◄──────── resume()
       │
    pause()
       │
       ▼
    PAUSED ──── captureSwingWindow() (analysis here)
       │
    stop()  ◄─── also callable from CAPTURING
       │
       ▼
     IDLE
       │
    start()  (next swing session)
```

### `start()` — Idle → Capturing *or* Paused

Launches the merger thread. Called once in `main.cpp` before any controllers are
constructed. The merger applies `ThreadPolicy::apply(ThreadRole::Merger)` at entry
to elevate its scheduling priority (see Section 13), then invokes the optional
thread-register hook (see below).

`start()` is valid with **zero registered sources**, and that is the normal case at
app launch. With no sources it enters **Paused**, not Capturing, and sets an
internal `no_source_paused_` flag — the merger parks rather than spinning over an
empty source set:

```cpp
buffer.isWaitingForSources();  // true: Paused only because nothing is registered
```

Use this to distinguish "waiting for the first device" from a deliberate
swing-analysis pause — they are the same `BufferState::Paused` otherwise. The flag
is set by `start()` with no sources, by `deregisterSource()` when the last source
goes, and by `resume()` when it declines (see below). It clears on a successful
`resume()`.

The GUI closes the loop rather than the buffer auto-resuming itself:
`CameraManager::applyCaptureIntent()` is called after every register/deregister and
funnels through `resumeBuffer()`, which is a no-op unless the buffer is Paused and
becomes effective once a source exists.

### Registering the merger thread with the profiler

`EventBuffer` deliberately does not know about the app's resource profiler — the
standalone Buffer tests build without it. Instead the app injects a hook, called
once from the merger thread at loop entry:

```cpp
// src/Gui/main.cpp
eventBuffer.setThreadRegisterHook([](const char *name) {
    pinpoint::osmetrics::registerThread(name);   // name == "Buffer.Merger"
});
eventBuffer.start();   // set the hook BEFORE start()
```

Set it before `start()`; it fires exactly once. See
`resource_profiler_developer_guide.md` for what registration buys you.

### `pause()` — Capturing → Paused

1. Sets `capturing_ = false` — producers see `slot.valid = false` on their next
   callback and discard it. No blocking on the capture threads.
2. Signals the merger to drain: the merger emits all pending events from its
   reorder heap into the index, then parks untimed on `control_wait_` (see §13.1a)
   — it consumes no CPU while paused, waking only on `resume()`/`stop()`. On
   Windows, entering the park also drops the global 1ms timer resolution back to
   the system default (see §13.1b).
3. Waits up to `pause_drain_timeout_ms` (default 20ms) for the merger to confirm
   it has drained. Logs a warning if this times out.
4. Rings are now frozen — no writes, no overwrites. Safe to read from any thread.
5. DMA queues remain registered. Cameras keep running at the OS level; callbacks
   fire and return immediately (discarded via `slot.valid`). This avoids camera
   re-initialisation latency on `resume()`.

### `resume()` — Paused → Capturing

0. **Refuses if no sources are registered.** It stays Paused, sets
   `no_source_paused_`, and returns — there is nothing to capture, and spinning the
   merger over an empty source set is pure waste.
1. If `resume_clear_rings` is true (default): **folds each ring's counters into the
   session-lifetime totals** (§10) *before* resetting the ring's write position to
   zero, then clears the `TimelineIndex`. The next swing gets a clean slate without
   the resource monitor losing every count on each pause→resume cycle.
2. Sets `capturing_ = true` — producers begin writing immediately on the next callback.
3. Wakes the merger out of its paused park: bumps `control_wait_` and notifies —
   done **last**, after the state transition is fully committed, so the woken
   merger never observes a torn mid-resume view. On its next normal-path iteration
   the merger re-raises the Windows 1ms timer resolution (see §13.1b).

Existing `Subscription` instances see a gap in sequence numbers after `resume()`.
This is expected — subscriptions that span a resume are invalidated. Live consumers
(UI preview) should call `resetToLatest()` after resume.

### `stop()` — Capturing or Paused → Idle

If Capturing, internally calls `pause()` first. Then clears `running_` and wakes
the merger — bumping `control_wait_` (in case it is parked on the pause gate) as
well as `index_wait_` (the normal-path cold timeout) — and joins its thread. On
exit the merger balances any outstanding Windows `timeBeginPeriod` (see §13.1b).
The buffer returns to Idle. Called once on app exit via
`QObject::connect(&app, &QGuiApplication::aboutToQuit, ...)` in `main.cpp`.
`~EventBuffer()` calls `stop()` as a safety net.

### State query

```cpp
buffer.state();               // returns BufferState enum
buffer.isCapturing();         // convenience — true only in Capturing state
buffer.isWaitingForSources(); // Paused only because nothing is registered
buffer.swingWindowLive();     // a SwingWindow captured from this buffer is alive
```

All are safe to call from any thread. `swingWindowLive()` is the app-wide resume
gate: `CameraManager::resumeBuffer()` returns early while it is true, so no path —
including QML — can unfreeze the rings out from under a window the shot workers
are still reading.

---

## 8. Consuming Data — Subscriptions

A `Subscription` gives a consumer a cursor into the `TimelineIndex`. Events arrive
in timestamp order across all sources.

```cpp
auto sub = buffer.subscribe();
pinpoint::IndexEntry entry;

// Blocking read with timeout
while (sub.waitNext(entry, std::chrono::milliseconds(100))) {
    if (entry.flags & pinpoint::IndexEntryFlags::SourceStalled) {
        // Handle stall marker — see Section 10
        continue;
    }

    // Zero-copy payload access
    auto handle = buffer.acquireReadHandle(entry.source_id, entry.source_sequence);
    if (!handle.data) continue;  // slot was overrun before we got here

    // Read handle.data / handle.bytes
    // ...

    // Validate AFTER consuming — if false, data was overwritten mid-read
    // This is rare with a 5s window but correct code must handle it
    if (!handle.validate(buffer.ringFor(entry.source_id)))
        continue;
}

// Non-blocking read
if (sub.tryNext(entry)) { /* ... */ }

// Skip-ahead after resume or overrun
sub.resetToLatest();

// How many events were overwritten since last read
uint64_t missed = sub.overrunsSinceLastRead();
```

### Latency expectations

Consumer-visible latency has two components:

1. **Merger response** — producer publish → event visible in `TimelineIndex`.
   Measured p50: ~4µs on Linux/Windows, ~5µs on macOS. This is the time from
   `publish()` to the merger waking and processing the event.

2. **Reorder window delay** — events are held back by 5ms (configurable) before
   emission to absorb cross-source jitter. Additionally, an event from a fast
   source (e.g. 200Hz IMU) cannot be emitted until all sources have advanced past
   its timestamp. With a 60fps camera (16.7ms interval), the end-to-end visible
   latency floor for IMU events is approximately:

   ```
   floor = reorder_window + 0.5 × slowest_source_interval
         = 5ms + 0.5 × 16.7ms ≈ 13ms
   ```

   This is correct by design. For live preview (~15ms behind real-time) it is
   imperceptible. For post-swing analysis via `SwingWindow`, latency is irrelevant
   — the buffer is frozen and all data is immediately available.

---

## 9. The SwingWindow — Frozen Analysis View

`SwingWindow` is the primary interface for post-swing analysis. A window captured
*from a buffer* can only be constructed while that buffer is `Paused`.

```cpp
buffer.pause();

// Last 4 seconds (covers backswing + downswing + follow-through)
auto window = buffer.captureSwingWindow(std::chrono::milliseconds(4000));

// Or explicit time range
auto window = buffer.captureSwingWindow(t_start_us, t_end_us);
```

### Accessing events

```cpp
// All events in timestamp order across all sources
std::span<const pinpoint::IndexEntry> entries = window.entries();

// Events for a specific source only
std::vector<pinpoint::IndexEntry> camEntries = window.entriesFor(camId);

// Convenience counts
size_t nFrames = window.frameCount(camId);
size_t nSamples = window.imuSampleCount(imuId);

// Zero-copy payload access — valid for the window's lifetime
pinpoint::SourceRing::ReadHandle h = window.payloadOf(entry);
// h.data points into frozen ring memory — no copy
// h.bytes is the actual payload size

// Format metadata for a source — pixel format, dimensions, plane strides,
// exposure and its provenance. Read this rather than assuming anything about
// the payload layout; in particular NEVER assume stride == width.
const pinpoint::FormatDescriptor& fmt = window.formatOf(camId);
```

### IMU interpolation

```cpp
// Get interpolated IMU state at an arbitrary timestamp
// (e.g. the exact moment a camera frame was captured)
pinpoint::ImuSample s{};
bool ok = window.interpolateImu(imuId, frame_timestamp_us,
                                reinterpret_cast<std::byte*>(&s), sizeof(s));
```

`out_bytes` must equal `sizeof(ImuSample)` (40) — the window interpolates decoded
samples, not raw device packets. Accel and gyro are lerped componentwise; the
orientation quaternion is **slerped along the shortest arc**, never interpolated
as Euler angles. All values are in the raw sensor body frame (schema
`imu_sample_v2`) — see `imu_sample.h` and `docs/design/imu_frame_contract.md`.

### Backing the window with something other than a live buffer

`SwingWindow`'s public constructor takes a `std::unique_ptr<const
SwingPayloadSource>`, so anything that can serve `(source_id, sequence) → bytes`
plus a `FormatDescriptor` can back one:

```cpp
class MySource final : public pinpoint::SwingPayloadSource {
    pinpoint::SourceRing::ReadHandle payloadOf(SourceId, uint64_t) const noexcept override;
    const pinpoint::FormatDescriptor& formatOf(SourceId) const noexcept override;
    bool validate(SourceId, const pinpoint::SourceRing::ReadHandle&) const noexcept override;
};

pinpoint::SwingWindow w(std::make_unique<MySource>(), std::move(entries), t0, t1);
```

The contract on `payloadOf()` is deliberately weak so a streaming backing is
possible: the bytes returned for a source need only stay valid **until the next
`payloadOf()` call on that same source id**. Analysis stages read camera frames
strictly one at a time, so a single reusable buffer per source satisfies it —
which is exactly how `SwingDiskSource` keeps one decoded frame per camera resident
instead of the whole swing.

`validate()` is the ring-only concern: `RingPayloadSource` re-checks the seqlock
generation against the live ring, while a disk/RAM backing returns `true`
unconditionally because its bytes cannot be recycled underneath the reader.

### Lifetime rules

- For a ring-backed window: `window.entries()` and `window.payloadOf()` are valid
  while the buffer is `Paused` and the `SwingWindow` object is alive. A disk-backed
  window has no such coupling.
- The resume/deregister guard (`swingWindowLive()`) is owned by
  `RingPayloadSource`, set in its constructor and cleared in its destructor — i.e.
  held for the window's whole lifetime, moves included. `CameraManager::resumeBuffer()`
  refuses while it is set, so in practice you cannot resume out from under a live
  window; the app destroys it in `ShotProcessor::finishShot()` and only then
  re-applies the capture intent.
- `SwingWindow` is non-copyable but movable. The move is defaulted — ownership of
  the payload source (and therefore of the guard) transfers with it, with no
  special-casing.
- Destroying a `SwingWindow` does not resume the buffer. `resume()` must be
  called explicitly when the application is ready for the next swing.
- Two workers may read one window concurrently. Every accessor is `const` and
  zero-copy; `ShotAnalyzer` and `SwingExporter` do exactly this.

### Memory cost

`captureSwingWindow()` allocates a `std::vector<IndexEntry>` for the index
snapshot. At 600 events/second aggregate (IMU + camera) over 4 seconds, this is
approximately 96 KB — negligible. Frame payloads are **not copied** — they are
read directly from the ring.

---

## 10. Diagnostics and Observability

### Per-source statistics

Every source ring maintains counters updated on the hot path:

```cpp
const pinpoint::SourceStats& stats = buffer.statsFor(sourceId);

stats.events_written.load();         // total events published
stats.events_overwritten.load();     // slots overwritten before merger drained them
stats.bytes_written_total.load();    // total bytes stored
stats.last_write_timestamp_us.load();// timestamp of most recent event
stats.max_inter_arrival_us.load();   // largest gap between consecutive events
stats.bounds_violations.load();      // publishes where bytes_written > slot capacity
stats.monotonicity_violations.load();// non-monotonic timestamps, clamped by merger
```

### Liveness watchdog

The merger thread checks each source every 10ms. A source is declared stalled once
it has been silent for

```
max(expected_interarrival_us × stall_threshold_mult, stall_threshold_min_us)
```

i.e. `5 ×` its expected interval, but **never less than 1 second**. That floor
matters: at 240fps the rate-derived threshold is ~21ms, and ordinary BLE/USB
delivery hiccups well under a second are normal and self-heal — without the floor
the timeline would fill with stall markers that mean nothing. When a source does
trip the threshold, a marker is emitted into the `TimelineIndex`:

```cpp
// In a consumer loop:
if (entry.flags & pinpoint::IndexEntryFlags::SourceStalled) {
    qWarning() << "Source stalled:" << entry.source_id;
}

// Query current stalled sources:
std::vector<pinpoint::SourceId> stalled = buffer.stalledSources();
```

The stall clears automatically when the source resumes writing.

### Diagnostics snapshot

Two consumers poll this. `BufferController` (`src/Gui/cameras/`) calls it on a 5s
timer, logs a line per source (at warn level when stalled), and republishes it to
QML as `state` / `sources` / `totalEvents`. `ResourceMonitorController`
(`src/Gui/monitor/`) takes the same snapshot for the resource-monitor screen — that
is where the lifetime totals are actually surfaced.

```cpp
pinpoint::EventBuffer::DiagnosticsSnapshot snap = buffer.diagnostics();

snap.state;                 // current BufferState
snap.timeline_entries;      // total events in TimelineIndex
snap.snapshot_timestamp_us; // when this snapshot was taken

for (const auto& src : snap.sources) {          // currently-registered sources
    src.id;
    src.name;               // source name string
    src.identifier;         // stable device id — keys the lifetime totals
    src.slot_count;
    src.events_written;     // since the last ring reset (i.e. last resume)
    src.events_overwritten;
    src.bytes_written_total;
    src.stalled;
    // Session-lifetime totals: folded history + the current ring's counters.
    // These SURVIVE the ring resets that pause→resume performs.
    src.lifetime_events_written;
    src.lifetime_events_overwritten;
    src.lifetime_bytes_written;
}

for (const auto& lt : snap.lifetime) {          // every source seen this session,
    lt.identifier;                              // INCLUDING deregistered ones
    lt.events_written;
    lt.events_overwritten;
    lt.bytes_written;
}
```

### Per-swing counters vs session-lifetime counters

This is the distinction to get right when reading diagnostics. The `SourceStats`
counters (and the matching `SourceInfo` fields) are **per capture session**: a
default `resume()` clears the rings, so they restart from zero on every
pause→resume — which, with swing replay, is once per shot. Before clearing,
`resume()` folds them into `lifetime_`, keyed by `SourceDescriptor::identifier`.
`deregisterSource()` folds too, so a device that has since been deselected still
appears in `snap.lifetime`.

So: use `events_written` to ask "how is this device doing right now"; use
`lifetime_events_written` or `snap.lifetime` to ask "how much has this device
delivered this session". The `lifetime_` map is mutated only on the control plane
(`resume()` / `deregisterSource()`, both serialised with the buffer Paused) and
never touched on the producer hot path.

### What to watch for

| Symptom | Likely cause | Action |
|---|---|---|
| `events_overwritten > 0` | Consumer too slow, or ring undersized | Check consumer latency; increase `window_duration` or reduce capture rate |
| `bounds_violations > 0` | Frame larger than `max_payload_bytes` | Increase `max_payload_bytes` in `SourceDescriptor` |
| `monotonicity_violations > 0` | Device sending non-monotonic timestamps | Informational only — merger clamps and continues |
| Source stalled, expected | IMU not connected in this session | Normal — watchdog only fires if `last_write_timestamp_us > 0` |
| Source stalled, unexpected | Cable unplugged, BLE dropped | Application should alert the user |

---

## 11. Configuration

`EventBufferConfig` is passed to the `EventBuffer` constructor. All fields have
defaults that work for the standard Pinpoint deployment.

```cpp
pinpoint::EventBufferConfig config;
config.reorder_window_us       = 5000;  // 5ms cross-source ordering window
config.watchdog_interval_ms    = 10;    // how often stall detection runs
config.stall_threshold_mult    = 5;     // stall if silent for 5 × interarrival
config.stall_threshold_min_us  = 1000000; // ...but never sooner than 1s (see §10)
config.timeline_index_capacity = 8192; // power of 2; covers ~10s at 500 events/s
config.merger_spin_iterations  = 64;   // ~10µs hot-path spinning before sleeping
config.merger_cold_timeout_us  = 500;  // yield budget per merger iteration (macOS)
                                       // or sleep cap (Linux/Windows)
config.pause_drain_timeout_ms  = 20;   // max wait for merger to quiesce on pause()
config.resume_clear_rings      = true; // clear rings on resume() for clean swing start
config.cpu_affinity_enabled    = false;// pin merger to core 1 (desktop Linux/Windows only)

pinpoint::EventBuffer buffer(config);
```

Tuning advice:
- `stall_threshold_min_us`: the floor exists because the rate-derived threshold is
  absurdly tight at high frame rates (~21ms at 240fps). Lower it only if you
  genuinely need sub-second stall reporting and can tolerate the false positives.
- `reorder_window_us`: increase if sources have high clock jitter (>5ms) and
  events arrive out of order. Increases end-to-end latency by the same amount.
- `resume_clear_rings`: set `false` if you want `SwingWindow` data to persist
  across resume for a multi-swing comparison feature. You are then responsible for
  ensuring consumers do not read stale data.
- `cpu_affinity_enabled`: only meaningful on Linux and Windows desktops with ≥4
  cores. Reduces merger jitter by preventing OS migration to low-power cores.

---

## 12. Internals — How It Works

This section explains the implementation for anyone modifying the buffer or
debugging subtle issues. Understanding the internals is not required for normal use.

### 12.1 The seqlock — the heart of `SourceRing`

The core primitive that makes the ring lock-free is the **seqlock** (sequence
lock). Each slot in the ring has a generation counter — a `std::atomic<uint64_t>`.
The protocol is:

**Writer (producer)**:
1. Increment generation to an odd number — this signals "write in progress".
2. Write the payload bytes and header fields (timestamp, bytes_written).
3. Increment generation to the next even number with `memory_order_release`.
   This makes all prior writes visible to any reader that sees the even generation.

**Reader (consumer)**:
1. Load the generation counter with `memory_order_acquire`. If odd, the slot is
   being written — skip or retry.
2. Read the payload.
3. Re-read the generation.
4. If the generation changed between steps 1 and 3, the slot was overwritten during
   the read — the data is potentially torn. Discard and skip forward.

This is correct even on ARM (which has a weaker memory model than x86) because the
`acquire`/`release` pairing creates the necessary happens-before relationships. The
compiler and CPU cannot reorder reads before the acquire or writes after the release.

#### The two fences that make it actually work on arm64

The loads and stores above are necessary but not sufficient, because **`acquire`
and `release` on a load/store are one-way barriers**, and in both directions the
seqlock needs the *other* way. Both fences compile to nothing on x86-TSO and are
the difference between working and silently-not on arm64.

**Writer — release fence after publishing the odd generation** (`acquireWriteSlot()`
and `getSlotByIndex()`, `source_ring.cpp`):

```cpp
write_seq_.store(seq + 1, std::memory_order_release);
std::atomic_thread_fence(std::memory_order_release);   // ← this
return WriteSlot{...};
```

The store-release orders everything *before* it and stops nothing *after* it from
moving up. But the caller's payload stores happen after `acquireWriteSlot()`
returns — the marker is published first and the bytes it is supposed to cover come
later. Without the second fence those payload stores may become visible **before**
the gen=odd marker, and a consumer recycling the slot then copies half-written
bytes while the generation still looks stable: a torn read that `validate()` calls
clean.

**Reader — acquire fence before re-reading the generation** (`ReadHandle::validate()`):

```cpp
std::atomic_thread_fence(std::memory_order_acquire);   // ← this
uint64_t gen_after = hdr.generation.load(std::memory_order_relaxed);
return gen_after == generation_snapshot;
```

An acquire *load* cannot supply this. Acquire stops later accesses from floating
up; what has to be pinned here is the payload copy that already happened —
`copyBytesRacy()`'s loads must not sink *below* the check meant to vet them. An
acquire *fence* is the barrier with that direction (no prior load may cross it), so
the generation is re-read strictly after the bytes were taken. Without it the CPU
may satisfy the copy late, from a slot the producer has since recycled, and
`validate()` certifies those bytes against a generation read before they were
fetched. Note the load itself is then `relaxed` — the fence is doing the ordering,
so an acquire load would be redundant.

If you touch the seqlock, these two fences are the part to leave alone.

There is a deliberate data race in the seqlock: the reader reads payload bytes at
the same time a writer might be writing to them (if the ring wraps). ThreadSanitizer
(TSAN) flags this race. This is benign — the generation check is the safety mechanism,
not the absence of concurrent access. Narrow TSAN suppressions in
`src/Buffer/tests/tsan_suppressions.txt` cover exactly and only this pattern.

### 12.2 Cache line alignment and false sharing

Every hot atomic is on its own 64-byte cache line with `alignas(64)`:

- `SourceRing::write_seq_` — incremented by the producer on every publish
- `SourceStats` struct — updated on every publish and by the watchdog
- Each `SlotHeader` — read and written on every access

Without this alignment, two atomics that happen to share a cache line cause
**false sharing** — a cache coherency storm where every write on one CPU
invalidates the cache line on every other CPU holding it, even if they are
accessing different variables. At 200Hz × 5 sources the savings are measurable.

`SlotHeader` is `alignas(64)` and padded to exactly 64 bytes:

```cpp
struct alignas(64) SlotHeader {
    std::atomic<uint64_t> generation{0};  // 8 bytes
    int64_t  timestamp_us{0};             // 8 bytes
    uint32_t bytes_written{0};            // 4 bytes
    uint8_t  _pad[64 - 8 - 8 - 4];       // 44 bytes padding
};
static_assert(sizeof(SlotHeader) == 64);
```

The payload bytes follow immediately after the header in the pre-allocated storage.
`slot_stride_` = `sizeof(SlotHeader) + slot_max_bytes_`, rounded up to the next
multiple of 64, so each slot also begins on a cache line boundary.

### 12.3 The merger and the reorder window

The merger thread runs a k-way merge over all source ring tails. Because k ≤ 16
(the maximum number of sources), the merge uses a simple linear scan to find the
minimum — which is faster than a heap for small N due to cache locality and no
allocation.

The **reorder window** exists because sources do not share a hardware clock.
USB cameras may timestamp a frame at the driver level a few milliseconds after
the sensor read-out; BLE IMU packets arrive in bursts. Without a reorder window,
two events with nearly the same real-world timestamp could be emitted in the wrong
order if the IMU packet arrived first.

```
safe_emit_until = min(latest_observed_ts across all sources) - reorder_window_us
```

An event is only emitted when `event_ts ≤ safe_emit_until`. This means all
sources must have produced at least one event with a timestamp beyond the event's
timestamp plus the window before the event can be released. The cost is latency
(~13ms end-to-end for the default 5ms window with a 60fps camera).

#### Overrun recovery — resume at the oldest resident slot, not the write head

When the merger finds `write_seq - next > slot_count`, the producer has lapped it
and the sequence it wanted is gone. The obvious recovery — skip to the current
write head — is wrong, and used to be what this did. The backlog
`[write_seq - slot_count, write_seq)` is *published, still-valid data*; jumping to
the head throws away a whole extra ring lap (seconds of frames) on top of what the
producer already overwrote. So the merger resumes at the oldest sequence still
resident:

```cpp
next = ws_now - slot.ring->slotCount();
```

Salvaging into a region the producer is still advancing through is safe because
`peekTimestamp()`'s seqlock and lap checks reject any sequence the producer reaches
while the merger drains — a salvaged slot that goes stale mid-recovery is simply
dropped again.

### 12.4 Producer signals — how the merger wakes

Without a signal mechanism, the merger would need to poll constantly or sleep on a
timer. Timer-based sleep on macOS has poor granularity (Darwin timer coalescing can
turn a 500µs sleep into 5–10ms). The solution: every `EventBuffer::publish()` call
increments an atomic counter `source_published_` and calls `notifyAll()` on it.
The merger's cold path waits on this counter rather than on a timer, so it wakes
within microseconds of a publish rather than on the next scheduler tick.

This is what produces the measured p50 merger latency of ~4µs — the merger is
event-driven, not polled.

### 12.5 The `TimelineIndex`

The `TimelineIndex` uses the same seqlock pattern as `SourceRing`. The difference
is access pattern: `SourceRing` is SPSC (one producer, the merger as sole consumer
via `peekTimestamp`), while `TimelineIndex` is SPMC (merger writes, multiple
consumer subscriptions read). The seqlock handles both correctly because it is
purely a property of the slot's generation counter, not of queue topology.

The index capacity is a power of 2 (default 8192). At 600 events/second aggregate,
this is ~13 seconds of history — more than the 5s window, providing headroom for
slow consumers.

### 12.6 `SwingWindow` safety without pinning

A ring-backed `SwingWindow` accesses slot memory directly with no reference
counting. This is safe because:

1. `captureSwingWindow()` is only callable when `state() == Paused`.
2. In Paused state, `capturing_ = false`, so `acquireWriteSlot()` returns
   `valid = false` — no producer can write to any slot.
3. The merger is quiesced and not advancing the write head.
4. Therefore, ring slots cannot be overwritten while the buffer is Paused.
5. `SwingWindow::payloadOf()` returns a `ReadHandle` with a generation snapshot.
   Even though overwrite cannot happen while Paused, `validate()` is still called
   after use — correct code should not rely on state invariants for correctness.

What keeps the buffer *in* Paused for the window's lifetime is the
`swing_window_live_` flag, and note **where it now lives**: `RingPayloadSource`
sets it in its constructor and clears it in its destructor. That placement is the
whole trick. The flag used to be set/cleared by `SwingWindow` itself, which meant
every move constructor and move assignment had to hand the guard over by hand and
null out the source. Now the window's moves are `= default`: the guard rides along
with the `unique_ptr<SwingPayloadSource>`, is held until the *final* owner is
destroyed, and a disk-backed window simply never acquires one — because
`SwingDiskSource` has no buffer to freeze.

---

## 13. Platform-Specific Code and Why

### 13.1 `WaitFlag` — three different wait primitives

**The problem**: the merger's cold path needs to block efficiently until a producer
publishes. C++20 `std::atomic::wait` would be ideal, but it has two problems:
1. No timeout in C++20 (timed waits are proposed for C++26).
2. On Apple platforms, the implementation uses polling-with-exponential-backoff
   rather than a native futex because `__ulock_wait` (Darwin's futex equivalent)
   is a private API that causes App Store rejection.

**The solution**: `WaitFlag` in `src/Buffer/wait_flag.cpp` implements three paths:

**Linux** (`PINPOINT_PLATFORM_LINUX`):
Uses a spin loop with `PINPOINT_CPU_PAUSE()` (x86 `PAUSE` instruction or ARM
`yield`) followed by `std::this_thread::sleep_for` with a manual deadline check.
`std::atomic::notify_all()` is available as a wakeup primitive (it maps to the
Linux futex syscall). The result is a genuinely low-latency wait — typically wakes
within 10–50µs of notification.

**Windows** (`PINPOINT_PLATFORM_WINDOWS`):
Uses `WaitOnAddress` / `WakeByAddressAll` — the Windows equivalent of futex,
available since Windows 8 (Win10 1809 is our minimum). These are the same
primitives that underlie `std::atomic::wait` on MSVC, so behaviour is similar to
Linux.

**macOS / iOS** (`PINPOINT_PLATFORM_APPLE`):
Uses a hybrid spin+yield loop:
```cpp
// Phase 1: tight spin (~2µs)
for (int i = 0; i < 32; ++i) { PINPOINT_CPU_PAUSE(); check(); }

// Phase 2: yield loop — avoids Darwin timer coalescing
while (now < deadline) { sched_yield(); check(); }
```
`sched_yield()` gives up the current timeslice without requesting a timer wakeup.
It returns when the scheduler has no other runnable threads, typically 1–20µs.
This avoids the 5–10ms minimum latency of `condition_variable::wait_for` under
Darwin timer coalescing. The trade-off is slightly higher CPU usage during idle
periods — acceptable because the merger is genuinely busy most of the time when
the buffer is Capturing.

On Apple the **timed** `waitFor()` above still avoids `condition_variable`
entirely — the spin+yield loop is all the merger's hot cold-path uses. But the
struct's `mtx_`/`cv_` members are no longer dead: they back the **untimed**
`wait()` primitive described next (Apple also gains a `waiters_` counter).

### 13.1a `WaitFlag::wait()` — the untimed pause gate

`waitFor()` polls, which is right for the hot cold-path (the merger is busy most
of the time while Capturing) but wrong while **Paused**: there the merger has
nothing to do and must not burn CPU. Before this primitive existed, the paused
merger looped on `waitFor(..., 1000µs)` against a flag that never changed while
paused — so the timeout was the only thing ending each wait and it re-armed
immediately, a continuous poll (a full core on macOS, ~20k wakeups/s on Linux).

`WaitFlag::wait(expected)` blocks **indefinitely** until the value differs from
`expected` and a `notifyAll()` arrives — no timeout, no polling. Timer coalescing
only afflicts *timed* waits, so the constraint that forced `waitFor()` onto
spin/yield does not apply here:

- **Linux**: `std::atomic::wait` (futex-backed) — parks while `value_ == expected`.
- **Windows**: `WaitOnAddress(..., INFINITE)` in a spurious-wakeup re-check loop.
- **Apple**: a real `condition_variable` (`cv_`/`mtx_`). To keep the hot-path
  `notifyAll()` as cheap as the old no-op, a `seq_cst` `waiters_` counter gates
  it: with nobody parked, `notifyAll()` is a single atomic load + branch (no
  mutex). `store()` is promoted to `seq_cst` so a `0→1` waiter transition racing
  a `store()` can never be lost (no missed wakeup).

`EventBuffer::control_wait_` is a dedicated generation flag the merger parks on
while paused; `resume()`/`stop()` bump it and `notifyAll()` to wake the merger
(see §7). **Cold-path only** — never call `wait()` on the capture/merge hot path.

### 13.1b Windows timer resolution — scoped to capturing periods (Windows only)

The default Windows timer resolution is ~15.6ms. The merger raises it to 1ms via
`timeBeginPeriod(1)` so that its sub-millisecond cold-path `waitFor()` timeouts
and the 5ms reorder window behave as intended (without it, a 200µs wait sleeps
~15ms). `timeBeginPeriod` is **process-wide**: it raises the whole machine's
scheduler tick rate, which increases idle power draw and defeats CPU power-state
coalescing for as long as it is held.

The high-resolution timer is only needed on the **normal (hot) path**, where the
merger is draining and emitting events. While **Paused** the merger is parked
untimed on `control_wait_` (§13.1a) and needs no fine granularity. So the merger
brackets the resolution around capturing periods rather than its whole lifetime:

- **Raise** on the normal path (idempotent — a no-op if already raised).
- **Lower** in the draining branch, immediately after it sets `drained_` and
  before it parks on `control_wait_` — so the paused process runs at the system
  default resolution.
- **Balance on exit:** a final lower after the run loop covers `stop()` while on
  the normal path, so no `timeBeginPeriod` is ever leaked.

`timeBeginPeriod`/`timeEndPeriod` are reference-counted per-process, so every
begin **must** have exactly one matching end. A merger-local `bool hires_active`
guards the pair so calls are balanced and never doubled. They fire only on the
`Capturing ⇄ Paused` edges, never per merge iteration — rapid pause/resume cycles
(swing replay) just toggle them, which is cheap relative to a pause. All of this
is `#if defined(PINPOINT_PLATFORM_WINDOWS)`-guarded; on Linux/macOS the
raise/lower helpers are no-ops and there is no behavioural change.

> **Manual validation** (no automated assertion exists — the buffer tests do not
> probe global timer resolution): use `ClockRes`, `NtQueryTimerResolution`, or
> `powercfg /energy` to confirm the resolution sits at the system default while
> the buffer is Paused and rises to 1ms only while Capturing, and that it returns
> to its pre-`start()` value after `stop()` (no leaked `timeBeginPeriod`).

### 13.2 `ThreadPolicy` — priority elevation per platform

Capture and merger threads should run at elevated priority to reduce scheduling
jitter. The approach differs significantly per platform:

**Windows**: `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)` works for any
process without administrator rights. No special configuration needed.

**macOS / iOS**: `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)`
works for any process. Grand Central Dispatch honours this class with very low
scheduling latency. No special configuration needed. Available from macOS 8+ / iOS 8+.

**Linux**: The ideal is `SCHED_FIFO` with priority 50, which gives the thread
real-time scheduling guarantees. However, this requires `CAP_SYS_NICE`. Without it,
the fallback is `setpriority(PRIO_PROCESS, 0, -10)` (nice value -10), which
requires no special privileges but gives only a hint to the scheduler. To grant
RT priority without running as root:

```bash
sudo setcap cap_sys_nice+ep pinpoint
```

Or via systemd: `AmbientCapabilities=CAP_SYS_NICE`

`ThreadPolicy::apply()` tries the best available option, logs the result via
`lastApplyDescription()`, and never throws. Pinpoint always starts whether or not
priority elevation succeeds.

**Android**: `setpriority(PRIO_PROCESS, 0, -19)` (equivalent to
`THREAD_PRIORITY_URGENT_AUDIO`) works for an app's own threads without elevation.

### 13.3 Clock source

`EventBuffer::nowMicros()` wraps `std::chrono::steady_clock::now()`, which the
standard library maps to the best available monotonic clock per platform:

| Platform | Underlying call | Resolution | Notes |
|---|---|---|---|
| Linux | `clock_gettime(CLOCK_MONOTONIC)` | ~20ns | Unaffected by NTP |
| Windows | `QueryPerformanceCounter` | ~100ns | Unaffected by time changes |
| macOS | `mach_absolute_time` | ~40ns | Mach timebase |
| Android | `clock_gettime(CLOCK_MONOTONIC)` | ~100ns | Same as Linux |

All are monotonic — they never go backwards. All are lock-free — the call compiles
to a single syscall or hardware register read. Using any other clock source, even
`std::chrono::high_resolution_clock` (which may alias `system_clock`), risks
introducing non-monotonicity if the system time is adjusted.

### 13.4 Aligned allocation

`SourceRing::storage_` requires 64-byte alignment. `std::aligned_alloc` is
standard C++17 but has a platform quirk: **on MSVC, the allocation size must be a
multiple of the alignment**. The implementation ensures `slot_count_ × slot_stride_`
is always a multiple of 64 by rounding `slot_stride_` up during construction. On
MSVC, the implementation falls back to `_aligned_malloc` / `_aligned_free` to avoid
this constraint.

### 13.5 `PINPOINT_CPU_PAUSE()`

The spin loops in `WaitFlag` and the merger use `PINPOINT_CPU_PAUSE()` to signal
to the CPU that this is a spin-wait, allowing the processor to reduce power
consumption and avoid memory order violations on x86 (the `PAUSE` instruction also
avoids the memory order machine clear that occurs when a speculative load sees a
store to the same address). Defined in `platform.h`:

```cpp
#if defined(_MSC_VER)
  #define PINPOINT_CPU_PAUSE() _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
  #define PINPOINT_CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__arm__) || defined(__aarch64__)
  #define PINPOINT_CPU_PAUSE() __asm__ __volatile__("yield")
#else
  #define PINPOINT_CPU_PAUSE() ((void)0)
#endif
```

---

## 14. Common Mistakes

### Assuming a camera frame's row stride equals its width

`plane_strides` exists because drivers pad rows. Computing `row * width` instead of
`row * plane_strides[0]` produces a progressively sheared image — which looks like
a camera or decode bug and is neither. Read the stride from
`SwingWindow::formatOf()` / `EventBuffer::formatOf()`; never derive it.

### Reading `slot.data` when `slot.valid == false`

`acquireWriteSlot()` returns a `WriteSlot` with `data = nullptr` and `valid = false`
when the buffer is Paused. Writing to `nullptr` is undefined behaviour. Always check
`slot.valid` before accessing `slot.data`.

### Holding a pointer to slot data after `publish()`

The slot can be overwritten by the next `acquireWriteSlot()` call on the same ring
(when the ring wraps). Any pointer into slot memory is invalid after `publish()`.
If you need to retain the data, copy it before publishing — but this defeats the
zero-copy purpose. The correct pattern is to publish and let the consumer read via
`acquireReadHandle`.

### Using `std::chrono::system_clock` for timestamps

`system_clock` is not guaranteed to be monotonic and changes when the system time
is adjusted (e.g. NTP correction). Use only `EventBuffer::nowMicros()`. This is
enforced as a "MUST NOT" in the producer protocol and is the most common source
of ordering bugs during integration.

### Accessing `SwingWindow` payloads after `resume()`

`resume()` calls `SourceRing::reset()` on all rings, which resets the write
position to zero and overwrites all generation counters. Any existing `ReadHandle`
obtained from the ring is now invalid. `validate()` will catch this — it will
return false — but only if you call it. Destroy the `SwingWindow` before `resume()`.

### Registering with `max_payload_bytes` too small

`publish()` clamps `bytes_written` to `slot_max_bytes_` in release builds and
increments `bounds_violations`. The stored payload will be truncated — the consumer
will see partial data with no error. Monitor `bounds_violations` in diagnostics.
Camera registrations should use the actual frame size reported by the driver after
the camera starts.

### Overwriting a source's format descriptor with a partially-filled one

`updateSourceFormat()` replaces the whole `FormatDescriptor`. Re-stamps built from
frame data typically carry no `device_serial`, so a naive replace would erase the
attribution `registerSource()` normalised in. The buffer defends against exactly
that one field — an empty `device_serial` in the update preserves the registered
one — but nothing else is merged. Fill the descriptor from the current one rather
than from scratch. `CameraInstance` additionally refuses to re-stamp while
`swingWindowLive()`, preserving the invariant that a descriptor is immutable for
as long as a worker is reading through it.

### Calling `deregisterSource()` while a `SwingWindow` is live

`deregisterSource()` asserts that `swing_window_live_` is false. If you call it while
a `SwingWindow` exists that references data in that source's ring, you get a use-after-free.
Always destroy the `SwingWindow` before deregistering any source. The typical flow:

```cpp
window = {};                           // destroy SwingWindow first
buffer.deregisterSource(cam_id);       // now safe — ring memory freed
```

### Calling `deregisterSource()` while Capturing

`deregisterSource()` asserts `Paused` state. It is never safe to free ring memory
while the merger is running — the merger holds `SourceSlot&` references from its
`drainSources` lambda. `CameraManager::setSelected()` handles the pause/deregister/resume
sequence automatically — do not call `deregisterSource()` directly from application code
unless you also manage the state transition.

### Mixing `QDateTime` or `QElapsedTimer` for timestamps

Qt's time classes are not guaranteed to be monotonic across all platforms and some
(e.g. `QDateTime::currentMSecsSinceEpoch()`) are system-clock-based. Only
`EventBuffer::nowMicros()` guarantees the properties the merger relies on.

---

## 15. File Map

```
src/Buffer/
├── CMakeLists.txt              pinpoint_buffer library; PINPOINT_ENABLE_{ASAN,
│                               UBSAN,TSAN} options; tests added only when
│                               PROJECT_IS_TOP_LEVEL
├── platform.h                  Platform detection macros; PINPOINT_CPU_PAUSE()
├── types.h                     SourceId, DeviceKind (incl. Marker_App),
│                               PixelFormat, IndexEntry, IndexEntryFlags
├── format_descriptor.h         FormatDescriptor, CameraFormat (strides,
│                               exposure), ImuFormat, ExposureSource + names
├── pixel_format_names.h        PixelFormat ↔ swing.json string, both directions
│                               derived from one table. Qt-free.
├── imu_sample.h                ImuSample (40B, "imu_sample_v2") + makeImuSample
│                               — the single source of truth for the stored frame
├── source_descriptor.h         SourceDescriptor, SyncSource, slot-count/size math
├── source_stats.h              SourceStats (cache-line aligned, all atomic)
├── event_buffer_config.h       EventBufferConfig — all tunable parameters
│
├── source_ring.h               SourceRing — seqlock SPSC ring, core primitive
├── source_ring.cpp             …including the two seqlock fences (§12.1)
│
├── timeline_index.h            TimelineIndex — SPMC seqlock ring of IndexEntry
├── timeline_index.cpp
│
├── wait_flag.h                 WaitFlag — platform-abstracted timed + untimed wait
├── wait_flag.cpp               Three platform implementations in one file
│
├── thread_policy.h             ThreadPolicy — unprivileged priority elevation
├── thread_policy.cpp           Per-platform implementations
│
├── event_buffer.h              EventBuffer, Subscription, DiagnosticsSnapshot
├── event_buffer.cpp            Merger loop, lifecycle, watchdog, registration,
│                               lifetime counters, RingPayloadSource
│
├── swing_payload_source.h      SwingPayloadSource — the pluggable window backing
├── swing_window.h              SwingWindow — frozen analysis view
├── swing_window.cpp
│
└── tests/
    ├── CMakeLists.txt              8 gtest targets + the benchmark
    ├── source_ring_test.cpp        SPSC stress, overrun, seqlock correctness
    ├── adversarial_fuzz_test.cpp   Producer/consumer race timing
    ├── timeline_index_test.cpp     Append, read, snapshot, reset
    ├── wait_flag_test.cpp          Timeout accuracy, wakeup latency; untimed
    │                               wait() — immediate-return, wake-on-notify,
    │                               and no-lost-wakeup-under-race (Apple handshake)
    ├── event_buffer_test.cpp       End-to-end, lifecycle, multi-cycle;
    │                               ResumeWakesParkedMerger (pause-gate park/wake)
    ├── swing_window_test.cpp       Payload access, lifetime, freeze semantics
    ├── watchdog_test.cpp           Stall detection, recovery
    ├── thread_policy_test.cpp      Priority elevation, thread-local description
    ├── latency_benchmark.cpp       Two-mode benchmark (see below)
    └── tsan_suppressions.txt       Narrow seqlock race suppressions
```

One Buffer-related test lives **outside** this tree:
`src/Analysis/tests/swing_window_parity_test.cpp` builds a real `EventBuffer`,
captures a window from it, and asserts a ring-backed window and a disk-backed one
present the same data — i.e. that the `SwingPayloadSource` split did not fork
behaviour. It sits in the Analysis suite because it needs the reanalyzer.

### Running the tests

The Buffer suite has **not** migrated to the shared `pp_add_test` helper — it
predates it, keeps its own GoogleTest `FetchContent`, and uses its own
`PINPOINT_ENABLE_*` sanitizer options rather than the repo-wide `PP_SANITIZE`.
Both entry points below are current; see `testing_developer_guide.md` for the
wider picture.

```bash
# Standalone — Buffer as its own top-level project (this is what enables its tests)
cmake -S src/Buffer -B build/buffer
cmake --build build/buffer -j6
ctest --test-dir build/buffer --output-on-failure

# Under the umbrella, alongside every other suite
cmake -S tests -B build/tests
cmake --build build/tests -j6
ctest --test-dir build/tests --output-on-failure
```

Sanitizers use a separate build dir (the flags bake into objects), and for these
targets the knob is `PINPOINT_ENABLE_*`, **not** `PP_SANITIZE`:

```bash
# ASAN + UBSAN (correctness)
cmake -S src/Buffer -B build/buffer-asan \
      -DPINPOINT_ENABLE_ASAN=ON -DPINPOINT_ENABLE_UBSAN=ON
cmake --build build/buffer-asan -j6
ctest --test-dir build/buffer-asan --output-on-failure

# TSAN (threading) — the seqlock's deliberate benign race needs the suppressions
cmake -S src/Buffer -B build/buffer-tsan -DPINPOINT_ENABLE_TSAN=ON
cmake --build build/buffer-tsan -j6
TSAN_OPTIONS="suppressions=$PWD/src/Buffer/tests/tsan_suppressions.txt" \
    ctest --test-dir build/buffer-tsan --output-on-failure
```

> On arm64 macOS, do not add `detect_leaks=1` — LSan is unsupported there and the
> whole suite reports as failing. See `testing_developer_guide.md`.

### Latency benchmark (two modes)

`latency_benchmark` is deliberately **not** registered with CTest — it is a
manual-run timing harness, and its targets are wall-clock assertions that would be
flaky in CI.

```bash
cmake -S src/Buffer -B build/buffer && cmake --build build/buffer -j6

# Mode A: merger response latency (single source, direct ring read)
# Target: p50 < 100µs, p99 < 500µs
./build/buffer/tests/latency_benchmark --merger

# Mode B: end-to-end latency (multi-source via Subscription)
# Target: p50 ≈ reorder_window + 0.5 × slowest_source_interval
# For 5ms reorder + 60Hz camera: p50 ≈ 13ms — this is by design, not a bug
./build/buffer/tests/latency_benchmark --e2e

# Both modes
./build/buffer/tests/latency_benchmark
```

---

*For the full design rationale, architectural decisions, and revision history, see
`docs/design/event_buffer_design.md`.*
