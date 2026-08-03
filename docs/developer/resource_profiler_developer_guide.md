# PinPointStudio Resource Profiler — Developer Guide

**Audience**: Developers and Claude Code investigating performance, capacity, or behavioural issues in PinPointStudio  
**Location**: `src/Core/` (profiler core), `src/Gui/monitor/` (controller + UI)  
**Language**: C++17 / QML  
**Status**: Production. Core, controller, monitor panel and the seed instrumentation all ship; GPU-memory sampling has a backend per platform; analysis stages are auto-instrumented and each run is recorded for drill-down. 7 test targets in `src/Core/tests/`.

---

## Contents

1. [What the Resource Profiler Is](#1-what-the-resource-profiler-is)
2. [Where It Fits in PinPointStudio](#2-where-it-fits-in-pinpointstudio)
3. [Core Concepts](#3-core-concepts)
4. [Instrumenting Code — The Profiler API](#4-instrumenting-code--the-profiler-api)
5. [Reading the Profiler in the Resource Monitor](#5-reading-the-profiler-in-the-resource-monitor)
6. [Capturing Diagnostics — Stats Ring vs Application Log](#6-capturing-diagnostics--stats-ring-vs-application-log)
7. [Investigating a Performance or Capacity Issue](#7-investigating-a-performance-or-capacity-issue)
8. [Build Configuration](#8-build-configuration)
9. [Internals — How It Works](#9-internals--how-it-works)
10. [Platform-Specific Code and Why](#10-platform-specific-code-and-why)
11. [Common Mistakes](#11-common-mistakes)
12. [File Map](#12-file-map)

---

## 1. What the Resource Profiler Is

The Resource Profiler is a lightweight, cross-platform instrumentation layer for finding
performance and capacity problems during development. It surfaces three things, attributed
by a **named scope or category** (the "by X" axis):

- **Memory** consumed by a category (current + peak bytes).
- **CPU** — whole-process and per-named-thread utilisation.
- **GPU memory** — this process's VRAM plus the device-wide picture, via one backend
  per platform (§10).
- **Call and timing accumulation** by scope (call count, total / min / max / average wall
  time, and — in the deep tier — per-scope CPU time).

It is a sibling of the application log: a thread-safe singleton that never touches a lock on
a producer hot path. Its periodic/session-end summaries are routed into a **dedicated stats
ring (`PpStatsLog`), deliberately separate from the main `PpMessageLog`** so that profiling
data never pollutes the application log or its Export. The stats ring is surfaced in its own
**STATS HISTORY** section (with its own category filter and Export button), and everything
the profiler records can be **captured by exporting that stats buffer** for after-the-fact
analysis.

It is designed around two operating modes (see [Core Concepts](#3-core-concepts)): a cheap
**baseline** tier that is always collecting (so there is standard data on every run), and a
**deep** tier you switch on while reproducing a specific problem.

## 2. Where It Fits in PinPointStudio

The profiler lives alongside the logging core in `src/Core/` and is surfaced in the
**Resource Monitor** screen (`src/Gui/monitor/`), in a dedicated **PROFILER** panel beneath
the existing event-buffer and device sections.

It is deliberately complementary to the **Event Buffer diagnostics** already in the Resource
Monitor. The buffer panel owns *data-flow* health — ring fill, source stalls, wraps,
inter-arrival times, bytes per source. The profiler owns *compute and memory* health — where
CPU time goes, which subsystems hold memory, how long operations take. The two read together:
a stalled source whose producer thread shows near-zero CPU while another thread is pinned is a
scheduling/starvation story the profiler makes visible that the buffer panel alone cannot.

Memory categories in the profiler deliberately **exclude the event-buffer rings**, which the
buffer panel already reports per source. The profiler tracks the consumers the buffer panel
does not see — ONNX arenas, decoded-frame pools, export staging, analysis series.

## 3. Core Concepts

**Two tiers.** This mirrors `pp_debug.h`'s always-on `ppInfo` versus dev-only `ppDebug`:

- **Baseline** (`PINPOINT_PROFILE_BASELINE`, default ON — ships in release through
  alpha/beta). Always collects, ignores the runtime toggle. Cheap: a handful of relaxed
  atomic adds per call. This is the *standard* data — the process/thread gauge, watermarks, a
  small fixed set of structural scopes, the memory categories, and a periodic + session-end
  dump into the dedicated stats ring (`PpStatsLog`), surfaced in STATS HISTORY.
- **Deep** (`PINPOINT_PROFILE`, default OFF, **and** a runtime toggle, default off).
  Fine-grained scopes plus per-scope CPU time (an extra thread-clock read per scope). Switch
  it on from the monitor while chasing a problem; switched off, the timer is never even
  constructed.

**Scope.** A named timed region. On exit it adds one call and its wall time to the scope's
record. The same name from multiple call sites collapses to one record.

**Counter.** A named call count with no timing — for events you want to count but not time.

**Memory category.** A named pair of `current` / `peak` byte counters, updated explicitly at
the allocation sites. `current` reflects live allocation; `peak` is a high-water mark.

**The gauge.** Whole-process RSS and CPU%, and per-named-thread CPU%, sampled at a fixed
cadence. Threads appear in the table by registering themselves (one line at a worker's
run-loop entry).

**Wall vs CPU.** Scope time is monotonic **wall** time — what you waited for, the right
number for latency and throughput. Per-scope **CPU** time (deep tier) tells you how much of
that wall time was actually on-core for the calling thread. The **process/thread gauge** is
the CPU-*saturation* signal. Use wall to find slow operations; use CPU to find busy threads.

**Watermarks.** Peak RSS, peak CPU%, and per-scope max wall time are retained. These are the
highest-value standard signal — they record the spike that happened while no one was watching.

**Per-session windows.** Counters reset at session start, so the standard profile is
per-session. Live allocations (memory `current`) are preserved across reset; only the peak is
rebased.

## 4. Instrumenting Code — The Profiler API

Include the header and use the macros. When a tier is compiled out, its macros expand to
`((void)0)` — no binary footprint, no cost.

```cpp
#include "pp_profiler.h"

void PoseEstimatorVitPose::run(const Frame &f)
{
    PP_PROFILE_SCOPE("Pose.ViTPose.run");          // baseline: wall time + call count

    {
        PP_PROFILE_SCOPE_DEEP("Pose.ViTPose.infer"); // deep: + per-scope CPU, only when toggled on
        session_->Run(...);
    }
}
```

Quick reference:

| Macro | Tier | Records |
|---|---|---|
| `PP_PROFILE_SCOPE("Name")` | baseline | wall time + call count (RAII) |
| `PP_PROFILE_COUNT("Name")` | baseline | call count only |
| `PP_PROFILE_MEM_ADD("Cat", bytes)` | baseline | `current += bytes`, tracks peak |
| `PP_PROFILE_MEM_SUB("Cat", bytes)` | baseline | `current -= bytes` |
| `PP_PROFILE_MEM_SCOPE("Cat", bytes)` | baseline | add on entry, release on exit (RAII) |
| `PP_PROFILE_SCOPE_DEEP("Name")` | deep | wall + per-scope CPU, only when toggled on |
| `PP_PROFILE_COUNT_DEEP("Name")` | deep | call count only, when toggled on |

**Memory.** Use `MEM_ADD` / `MEM_SUB` for buffers whose lifetime you manage manually, and
`MEM_SCOPE` for buffers whose lifetime matches a C++ scope (decode scratch, encode staging):

```cpp
// Manual lifetime — pair these:
PP_PROFILE_MEM_ADD("ONNX.Pose", arenaBytes);   // at session/arena creation
PP_PROFILE_MEM_SUB("ONNX.Pose", arenaBytes);   // at teardown

// Scoped lifetime — one line:
void VideoPreprocessorOpenCv::process(...) {
    PP_PROFILE_MEM_SCOPE("Video.Preproc", scratch.total() * scratch.elemSize());
    // ...
}
```

**Threads.** So a worker shows up in the per-thread CPU table with a meaningful name, register
it once at its run-loop entry (and unregister on exit if it is transient):

```cpp
void CaptureThread::run() {
    pinpoint::osmetrics::registerThread("Capture");
    // ... loop ...
    pinpoint::osmetrics::unregisterThread();
}
```

One thread cannot register itself directly: the EventBuffer merger. `src/Buffer/`
is a standalone library with no `src/Core/` dependency — its own tests build
without any of this. So `EventBuffer` exposes a hook instead, and `main.cpp`
supplies the call:

```cpp
eventBuffer.setThreadRegisterHook([](const char *name) {
    pinpoint::osmetrics::registerThread(name);      // name == "Buffer.Merger"
});
eventBuffer.start();     // the hook fires once, at merger loop entry
```

That is the pattern to copy for any other subsystem that must stay
Core-independent: an injected `std::function`, set before the thread starts.

**Naming convention.** Dot-namespaced, `Subsystem.Detail` (`Pose.ViTPose.infer`,
`Video.preprocess`, `Export.Staging`). Keep names static string literals — they are interned
once per call site by pointer, so the steady-state cost is a deref plus a few relaxed atomics.

**The standard instrumentation set** (seeded in Phase 3) is: scopes `Pose.ViTPose.run` /
`Pose.ViTPose.infer`, `Analysis.PoseRunner.run`, `Video.preprocess`; categories `ONNX.Pose`,
`ONNX.LLM`, `Video.FramePool`, `Video.Preproc`, `Export.Staging`, `Analysis.Series`;
registered threads `UI`, `Camera.Capture`, `IMU.IO`, `Buffer.Merger`, `Pose.Worker`,
`Export.Zip`, `Analysis.Worker`. Add to it freely.

**Analysis-pipeline stages are auto-instrumented.** Every shot-analysis pipeline
stage feeds a scope `Analysis.Stage.<StageName>` (plus a whole-run
`Analysis.analyze`) — not via a per-call-site macro but from the orchestrator's
in-memory stage trace: `analysis_profiling.cpp` (`recordAnalysisRun`, called at
the tail of `WristAnalyzer::analyze()` / `CameraKinematicsAnalyzer::analyze()`)
reads `ctx.trace` and calls `Profiler::recordWall()` for each ran stage. A new
analysis stage therefore appears **automatically** — no profiler edit is needed
when adding one. These scopes are surfaced in a dedicated **ANALYSIS STAGES**
table (see §5), and each run is also recorded to `AnalysisProfileLog` for the
per-run drill-down. Per-stage CPU is intentionally *not* attributed (the pose
stage's work is spread across ORT's own thread pool that a calling-thread CPU
clock cannot see) — read the process/per-thread gauge for the CPU story.

## 5. Reading the Profiler in the Resource Monitor

Open **Resource Monitor**. The **PROFILER** panel (hidden when the feature is compiled out)
shows:

- **Gauge strip** — process CPU% and RSS, each with its peak watermark.
- **Per-thread CPU** — one row per registered thread, CPU% over the last sample interval.
- **Scopes table** — NAME / CALLS / TOTAL / AVG / MAX, plus a CPU column when the deep tier is
  on. Deep-only scopes are greyed while the toggle is off.
- **Analysis stages table** — the `Analysis.Stage.*` scopes (STAGE / CALLS / TOTAL / AVG / MAX),
  pulled out of the generic scope list into their own table so per-stage analysis wall time
  (averaged and peaked across the session) is a first-class view. No CPU column (see §4).
- **Memory table** — CATEGORY / CURRENT / PEAK.
- **Stats history** — the dedicated `PpStatsLog` ring (periodic + session-end summaries),
  each row tagged GAUGE / THREAD / SCOPE / MEM, with its own **category-chip filter**, text
  filter, **Clear**, and **Export**. This is separate from the Message Log below it — profiling
  data never lands in the application log.
- **Analysis runs** — a third bottom tab (beside Message Log / Stats History) backed by
  `AnalysisProfileLog`: one row per `analyze()` call (time · session · ok/halted ·
  frames/total/score), tap to expand its full per-stage breakdown (each stage's ms, or the
  skip reason for a stage that did not run — so a camera-only or IMU-only run is legible from
  the breakdown alone). Each ran stage also draws a **horizontal time bar**, normalised
  against the slowest stage in *that* run, so the dominant stage fills its track and the
  rest scale against it; skipped stages get no bar. (Low-alpha accent behind the row text,
  which stays on top and legible — pure QML in `RmAnalysisRunRow.qml`, reading the per-stage
  ms the controller already exposes.) Its own **Clear** and **Export**
  (`PinPointStudio_analysis_*.txt`, one block per run with the stage list) — the artifact to
  attach when a user reports a slow analysis. Runtime only — never persisted to swing.json.
- **Controls** — a **deep** toggle (greyed when `PINPOINT_PROFILE` is not compiled in),
  **Reset** (start a fresh measurement window), and **Dump to log** (append a summary to the
  stats ring now).

The gauge is sampled by a single owner every ~1 second regardless of whether the screen is
visible; the on-screen tables refresh every 500 ms while the screen is open. The deep toggle
maps directly to the runtime gate — turning it on begins per-scope CPU attribution and
populates any deep scopes.

> The first gauge reading after launch or **Reset** shows 0% and seeds the baseline; real
> percentages appear from the next sample. This is expected — CPU% is a delta over an interval.

## 6. Capturing Diagnostics — Stats Ring vs Application Log

There are **two separate diagnostics rings**, each with its own on-screen section and its own
Export. Keeping them apart is deliberate: profiler data must never drown the application log,
and the application log must stay a clean record of warnings/errors for a bug report.

### The stats ring (profiler) — `PpStatsLog`

`PpStatsLog` (`src/Core/PpStatsLog.{h,cpp}`) is a thread-safe ring of the most recent profiler
summary lines, each `{ timestamp, category, message, seq }` where category is one of `GAUGE`,
`THREAD`, `SCOPE`, `MEM`. The profiler's `dumpToLog()` is its **only** producer — it appends a
categorised summary (RSS / CPU / per-thread / top scopes / memory peaks) on a 60-second cadence,
at session end, and on demand via **Dump to log**. Nothing else writes to it, and it writes to
nothing else: profiler data never reaches `PpMessageLog`.

Read it in the Resource Monitor's **STATS HISTORY** section:

- **Category chips** (`GAUGE` / `THREAD` / `SCOPE` / `MEM`) filter the view; the **text filter**
  narrows to a substring (e.g. `ONNX`, a scope name). Filtering is resolved in the controller
  (C++), not in QML.
- **Clear** resets the in-app view; **Export** writes the full stats buffer to
  `PinPointStudio_stats_YYYYMMDD_HHmmss.txt` in the user's home directory.

The stats Export file is the artefact to attach to a performance/capacity bug report or hand to
Claude Code — it carries the timestamped RSS / CPU / per-thread / top-scope / memory-peak trend.

### The application log — `PpMessageLog`

`PpMessageLog` (`src/Core/PpMessageLog.{h,cpp}`) is the *separate* ring of the most recent 500
log entries, each `{ timestamp, severity, message, seq }` (severity `DEBUG`/`INFO`/`WARN`/`ERROR`/
`FATAL`), read by the **Message Log** panel. It is fed by:

- **PinPointStudio code** via `pp_debug.h`: `ppInfo()` / `ppWarn()` / `ppError()` → `PpMessageLog`
  (and stderr); `ppDebug()` → **stderr only**, compiled out below debug level 3.
- **Qt** warnings and above (category-prefixed; high-volume categories like `qt.multimedia.ffmpeg`,
  `qt.bluetooth` are suppressed by default), **OpenCV** (`[OpenCV]`), and **FFmpeg** (`[FFmpeg]`).

The profiler is **not** a producer here — the one exception is the `setDeepEnabled()` breadcrumb
(`[Profiler] deep profiling enabled/disabled`), logged because it is a user action, not data.

In the Message Log panel, **severity chips** filter the view (`INFO` off by default — FFmpeg/OpenCV
capture makes it noisy), **Clear** resets the view, and **Export** writes
`PinPointStudio_log_YYYYMMDD_HHmmss.txt` to the home directory. Attach *this* file for a
crash/behaviour bug, and the **stats** Export for a performance/capacity one.

### Turning up verbosity

Console verbosity is controlled at build time by `PINPOINT_DEBUG_LEVEL` (see
[Build Configuration](#8-build-configuration)). For a noisy investigation, build with
`-DPINPOINT_DEBUG_LEVEL=3` to get `ppDebug()` traces on stderr in addition to the in-app log.
Two environment variables help with specific subsystems:

- `PINPOINT_BLE_TRACE=1` together with `QT_LOGGING_RULES="qt.bluetooth*=true"` surfaces the
  otherwise-suppressed Qt Bluetooth category to stderr — the only way to see where a BLE
  connection stalls.

## 7. Investigating a Performance or Capacity Issue

A practical playbook. The general loop is: **Reset → reproduce → read the panel → Dump to log →
Export**. Switch the **deep** toggle on (requires a `-DPINPOINT_PROFILE=ON` build) when you need
per-scope CPU or fine-grained scopes.

**High CPU / fan spin-up.** Read the **process CPU%** and the **per-thread CPU** table to find
the busy thread. Enable the deep tier and read the scopes' **CPU** column: the scope with the
largest `cpu_ns_total` on that thread is where the time goes. Example: `Pose` thread pinned →
`Pose.ViTPose.infer` CPU dominant → inference is the cost, not pre/post-processing.

**Memory growth or a suspected leak.** Watch the **memory table** across several shots. A
category whose **current** keeps climbing shot-over-shot — rather than returning to baseline
after each operation — is leaking or growing unbounded in that subsystem (`ONNX.Pose`,
`Export.Staging`, …). Cross-check the **peak RSS** watermark. If process RSS grows but no
profiler category does, the growth is in something untracked — most likely the event-buffer
rings (check the buffer panel) or an allocation site with no `PP_PROFILE_MEM_*` yet; add one.

**A slow operation / dropped frames / UI hitches.** In the **scopes table**, look for a high
**AVG** (consistently slow) or a **MAX** far above AVG (occasional spikes — GC-like pauses,
lock contention, or I/O). `Video.preprocess` or `Analysis.PoseRunner.run` are the usual
suspects. Enable deep to split a coarse scope into its parts.

**Capacity / ring fill / source stalls.** This is primarily the **buffer panel** (fill %,
stalls, max inter-arrival, wraps). Use the profiler to explain *why*: a source stalls while its
producer thread shows low CPU and another thread is saturated → CPU starvation, not a device
fault. A source whose bytes/sec is high and whose `Video.FramePool` peak is near its ceiling →
a capacity ceiling, not a logic bug.

**Intermittent or overnight issues.** Rely on the **standard baseline data**: the peak
watermarks and the 60-second stats-ring dumps are recorded even with no one watching and even
with the monitor screen closed (the 1 s gauge sampler runs regardless of screen visibility).
After the event, **Export** the stats buffer (STATS HISTORY → Export) and read the dump that
straddles the spike. This is the main reason baseline collection ships on by default.

**Handing it to Claude Code.** Attach the exported **stats** file (and the message-log Export if
there are related warnings/errors) and name the symptom. The stats lines give CC the
RSS/CPU/scope/memory trend; point CC at this guide's File Map and the relevant subsystem source.
For a live deep dive, build with `-DPINPOINT_PROFILE=ON -DPINPOINT_DEBUG_LEVEL=3`, reproduce with
the deep toggle on, and Dump to log.

## 8. Build Configuration

Two compile-time flags (CMake), plus one runtime toggle.

| Flag | Default | Effect |
|---|---|---|
| `PINPOINT_PROFILE_BASELINE` | `ON` | Compiles the baseline tier (gauge, watermarks, structural scopes, memory categories, 60 s/​session-end dump). Ships in release through alpha/beta. Flip OFF for GA. |
| `PINPOINT_PROFILE` | `OFF` | Compiles the deep tier (fine scopes + per-scope CPU). |
| runtime deep toggle | off | Gates deep collection at run time (monitor panel, or `Profiler::setDeepEnabled(true)`). |

```bash
# Standard development build — baseline on, deep available, verbose console:
cmake -B build -DPINPOINT_PROFILE=ON -DPINPOINT_DEBUG_LEVEL=3

# Release-shaped build during alpha/beta — baseline only (deep compiled out):
cmake -B build -DPINPOINT_PROFILE=OFF

# GA — strip baseline too:
cmake -B build -DPINPOINT_PROFILE_BASELINE=OFF -DPINPOINT_PROFILE=OFF
```

`PINPOINT_DEBUG_LEVEL` (separate, controls `pp_debug.h`): `0` silent, `1` warnings+errors
(release default), `2` +startup info, `3` +verbose `ppDebug` traces. When both profiler flags
are off, the monitor panel hides itself (`profiler.available == false`).

## 9. Internals — How It Works

**Interning.** Each macro call site caches its record pointer in a function-local `static`,
interned once on first hit (`Profiler::internScope` / `internMem`, deduplicated by name under a
mutex). After that first hit the hot path never touches the registry mutex — it is a pointer
deref plus relaxed atomic adds. Records are owned by the registry via `unique_ptr`, so their
addresses are stable for the life of the process.

**Lock-free accumulation.** Counters are `std::atomic` updated with `memory_order_relaxed`;
min/max watermarks use a relaxed compare-exchange loop. Nothing here orders against other
memory, and nothing locks — safe to call from the lock-free SPSC producer threads. `snapshot()`
takes the registry mutex only to copy records out; it never blocks the adds.

**The gauge has a single owner.** `osmetrics::sampleProcess()` / `sampleThreads()` compute CPU%
from the delta of CPU time against wall time **since the previous call**, using global baseline
state. They must be called by exactly one owner at a fixed cadence (the controller's ~1 s
timer). The 500 ms on-screen refresh reads the controller's **cached** gauge and must not
re-sample — splitting the delta across two callers would corrupt the percentages. The first
sample after construction or `reset()` returns 0% and seeds the baseline.

**Watermarks and reset.** Peak RSS / peak CPU% live in the OS-metrics layer; per-scope max wall
time lives in the record. `reset()` zeroes the cumulative scope counters and rebases the OS
peaks; it **leaves memory `current` intact** (it tracks live allocations) and only pulls each
memory `peak` down to its current level. This is what makes per-session windows correct without
double-counting still-live buffers.

**Stats dump.** `dumpToLog()` snapshots scopes + memory, samples the gauge once, and appends a
compact categorised summary to the dedicated `PpStatsLog` ring (capped to the top scopes by total
wall time). The controller's 1 s sampler calls it every 60 s, and `main.cpp` calls it at session
end; the controller surfaces the ring in STATS HISTORY. It deliberately does **not** touch
`PpMessageLog`. (Sampling the gauge inside the dump re-seeds the OS delta baseline; because the
controller caches the clean 1 s gauge value just before the 60 s dump, only the logged line — not
the on-screen gauge — is affected, a sub-millisecond same-thread artifact.)

**[seam]** If a single hot scope is hammered from many threads and its shared cache line
contends, shard the record per thread and merge in `snapshot()` — the public API does not change.

## 10. Platform-Specific Code and Why

All per-OS code is isolated in `pp_os_metrics.cpp`, in the same `#if defined(_WIN32)/__APPLE__/
else` structure as `pp_debug.cpp`. There is no platform-specific code anywhere above this layer.

| Metric | Linux | macOS | Windows |
|---|---|---|---|
| Process CPU time | `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` | same | `GetProcessTimes` (kernel+user) |
| Process RSS | `/proc/self/statm` × page size | `mach_task_basic_info.resident_size` | `K32GetProcessMemoryInfo.WorkingSetSize` |
| Thread CPU time (self) | `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` | same | `GetThreadTimes(GetCurrentThread())` |
| Thread CPU time (other) | `pthread_getcpuclockid` + `clock_gettime` | `thread_info(THREAD_BASIC_INFO)` | `GetThreadTimes(OpenThread(...))` |

Why each:

- **Process CPU via the POSIX clock** avoids parsing `/proc/self/stat` (whose comm field can
  contain spaces and parentheses). RSS still needs `/proc/self/statm` on Linux, mach on macOS,
  and the kernel32 `K32` export on Windows — chosen so **no `psapi.lib` link** is required.
- **Per-thread CPU is obtained differently per OS** because the portable APIs diverge:
  `pthread_getcpuclockid` exists on Linux but **not macOS**, so macOS uses the mach thread port
  captured at registration (`pthread_mach_thread_np`). Windows stores a real thread handle from
  `OpenThread` (the registering thread opens itself) and queries it with `GetThreadTimes`.
- **Windows `GetThreadTimes` granularity is coarse (~15 ms quantum)**, so per-thread CPU there
  is an interval-sampled signal rather than exact per-call — adequate for the gauge, and the
  reason per-scope CPU is a deep-tier-only number.
- **Thread names come from self-registration** (`registerThread`), not an OS query, so the table
  shows meaningful labels uniformly across platforms. Enumerate-all-threads is a deferred
  fallback.

### GPU memory — `pp_gpu_metrics`

The GPU sibling of `pp_os_metrics`, and the second per-OS surface. One small API
(`init` / `sample` / `processGpuBytes` / `reset`), exactly **one backend per
platform**, each picked to match where ONNX Runtime actually uses the GPU:

| Platform | Backend | Covers | How |
|---|---|---|---|
| Linux | NVML | CUDA — ONNX's only GPU EP there, so NVIDIA is the only relevant vendor | `dlopen("libnvidia-ml.so.1")` |
| Windows | DXGI | DirectML *and* CUDA in one vendor-neutral query | `IDXGIAdapter3::QueryVideoMemoryInfo` |
| macOS | Metal | CoreML | `MTLDevice currentAllocatedSize` / `recommendedMaxWorkingSetSize` (`pp_gpu_metrics_mac.mm`) |

`GpuSample` reports `processUsedBytes` (the headline number — this process's
VRAM), `peakProcessBytes`, the device-wide total/used where the backend can see
them, the resolved `backend` and `deviceName`, and a `unified` flag. That flag
matters when reading the numbers: on Apple Silicon GPU memory is unified, so it
**overlaps RSS** rather than adding to it — do not sum the two.

Three properties worth knowing:

- **No hard GPU dependency.** NVML is `dlopen`'d, never linked. On a host with no
  usable source (a non-NVIDIA Linux box) `init()` returns `false`, every field
  reads 0/empty, and the library still loads.
- **`init()` is idempotent and thread-safe**; `sample()` and `processGpuBytes()`
  call it internally. The controller calls it once at startup only to log which
  backend resolved.
- **`processGpuBytes()` is the cheap query**, used to *bracket* an
  `Ort::Session` creation for per-subsystem attribution — so it is called from
  model-load worker threads (pose, TTS) as well as the 1 s sampler. Everything is
  mutex-guarded for that reason. It is still a driver query: low-cadence only,
  never a hot path.

> The gauge and the GPU sampler are the only per-OS surfaces in the profiler.
> Everything above `pp_os_metrics.cpp` / `pp_gpu_metrics.cpp` is portable.

## 11. Common Mistakes

- **Re-sampling the gauge in `refresh()`.** Calling `sampleProcess()` / `sampleThreads()` from
  both the 1 s sampler and the 500 ms refresh splits the CPU% delta and produces garbage
  percentages. Only the sampler samples; everything else reads the cached values.
- **Expecting the first sample to be non-zero.** CPU% needs a prior baseline; the first reading
  after launch or **Reset** is 0% by design.
- **Using `ppDebug()` to capture something in the log.** `ppDebug()` goes to stderr only and is
  compiled out below level 3. Use `ppInfo()` (or above) to get an entry into `PpMessageLog` and
  its Export. For profiler metrics, use **Dump to log** → it lands in the separate stats ring
  (STATS HISTORY → Export), never in `PpMessageLog`.
- **Unbalanced `MEM_ADD` / `MEM_SUB`.** A missing `SUB` makes `current` drift upward and look
  like a leak that is actually a bookkeeping bug. Prefer `MEM_SCOPE` where the buffer's lifetime
  matches a C++ scope.
- **Putting buffer-ring memory into a profiler category.** The rings are already reported per
  source in the buffer panel; double-counting them here misleads. Profiler categories are for
  the consumers the buffer panel does not see.
- **Forgetting `registerThread`.** A worker that never registers contributes to process CPU% but
  shows no per-thread row — you lose the "which thread" signal. One line at the run-loop entry.
- **Reading scope wall time as CPU time.** A high wall time can be a thread that was *blocked*,
  not busy. Confirm with the per-thread CPU table or a deep scope's CPU column before concluding
  a scope is compute-bound.

## 12. File Map

| File | Role |
|---|---|
| `src/Core/pp_profiler.h` / `.cpp` | Tiered macros, lock-free records, singleton registry, `snapshot`, `reset`, `dumpToLog` (→ `PpStatsLog`), `ScopeTimer` / `MemScope`. |
| `src/Core/pp_os_metrics.h` / `.cpp` | Cross-platform process + per-thread RSS / CPU sampling and thread registration. |
| `src/Core/pp_gpu_metrics.h` / `.cpp` | GPU-memory sampling: NVML (Linux) / DXGI (Windows) dispatch, `GpuSample`, `processGpuBytes`. |
| `src/Core/pp_gpu_metrics_mac.mm` | The Metal backend (Objective-C++), Apple only. |
| `src/Core/cpu_topology.h` | Physical-core count — sizes the ORT intra-op pool (shot-analyzer guide §11). |
| `src/Core/AnalysisProfileLog.h` / `.cpp` | Per-`analyze()`-run record + its per-stage breakdown; backs the ANALYSIS RUNS tab and its Export. Runtime only. |
| `src/Core/PpStatsLog.h` / `.cpp` | Dedicated stats ring the profiler dumps into — separate from the application log. |
| `src/Core/PpMessageLog.h` / `.cpp` | Application-log ring (warnings/errors + Qt/OpenCV/FFmpeg capture); the profiler does **not** write here. |
| `src/Core/pp_debug.h` / `.cpp` | `ppInfo/ppWarn/ppError/ppDebug` and the Qt/OpenCV/FFmpeg capture. |
| `src/Analysis/analysis_profiling.h` / `.cpp` | `recordAnalysisRun` — turns a stage trace into `Analysis.Stage.*` scopes + an `AnalysisProfileLog` entry (§4). |
| `src/Gui/monitor/profiler_controller.h` / `.cpp` | QObject bridge: 1 s gauge sampler, cached gauge, formatted scope/memory/thread models, 60 s dump cadence, stats-ring filter + export. |
| `src/Gui/monitor/resource_monitor_controller.h` / `.cpp` | The screen's other half — the EventBuffer diagnostics snapshot (incl. session-lifetime totals) and device cards. |
| `src/Gui/monitor/ScreenResourceMonitor.qml` | Hosts the PROFILER panel, STATS HISTORY, and the bottom tabs. |
| `src/Gui/monitor/RmProfilerRow.qml` | Scope table row component. |
| `src/Gui/monitor/RmStatRow.qml` | STATS HISTORY row component (time / category / message). |
| `src/Gui/monitor/RmAnalysisRunRow.qml` | ANALYSIS RUNS row + the expandable per-stage breakdown with time bars (§5). |
| `src/Gui/monitor/RmDeviceCard.qml`, `RmSourceRow.qml`, `RmTimelineChart.qml`, `RmWarningNotice.qml` | The buffer/device half of the screen. |
| `src/Core/tests/` | 7 GoogleTest targets: `pp_profiler_test`, `pp_profiler_concurrency_test` (TSan), `pp_profiler_compileout_test`, `pp_os_metrics_test`, `pp_gpu_metrics_test`, `pp_stats_log_test`, `profiler_controller_test`. |

### Running the tests

```bash
cmake -S src/Core/tests -B build/core-tests
cmake --build build/core-tests -j6
ctest --test-dir build/core-tests --output-on-failure
```

> `profiler_controller_test` needs `AnalysisProfileLog.cpp` in its target — it
> reads `pullAnalysisRuns()`. It once silently reported as **"Not Run"** rather
> than failing, because a link error at that stage is not a test failure to
> CTest. If a target you added disappears from the results rather than going red,
> check the link line first.

---

*For the broader data-flow diagnostics this complements, see
`docs/developer/event_buffer_developer_guide.md` (§10, the diagnostics snapshot and
its session-lifetime counters). For what the analysis stages actually are, see
`docs/developer/analysis_pipeline_developer_guide.md` §7 (Managing Performance).*
