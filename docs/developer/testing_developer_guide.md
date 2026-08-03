# PinPoint Studio — Testing Developer Guide

**Audience**: Developers adding or running unit tests in PinPoint Studio
**Location**: `tests/` (umbrella + shared CMake module + presets), `src/<Sub>/tests/` (the suites)
**Language**: CMake + C++17/20 (GoogleTest or a hand-rolled `main()`)
**Status**: 8 suites, 117 test targets, runnable as one umbrella build or individually

---

## Contents

1. [Philosophy — why tests are decoupled from the app](#1-philosophy--why-tests-are-decoupled-from-the-app)
2. [The big picture](#2-the-big-picture)
3. [How the umbrella orchestration works](#3-how-the-umbrella-orchestration-works)
4. [The shared module — `tests/cmake/PinPointTests.cmake`](#4-the-shared-module--testscmakepinpointtestscmake)
5. [`pp_add_test` reference](#5-pp_add_test-reference)
6. [Running the tests](#6-running-the-tests)
7. [Adding a test to an existing suite](#7-adding-a-test-to-an-existing-suite)
8. [Adding a brand-new suite](#8-adding-a-brand-new-suite)
9. [Conventions and gotchas](#9-conventions-and-gotchas)
10. [File map](#10-file-map)

---

## 1. Philosophy — why tests are decoupled from the app

The app target is heavy: configuring it pulls in whisper.cpp/ggml, FFmpeg,
espeak-ng (built from source), OpenCV, ONNX Runtime and the full QML module — on
the order of 20+ seconds just to *configure*, before a single object compiles. A
unit test for, say, pure shaft-kinematics math has no business waiting on any of
that.

So **the test suites are not part of the app build.** The root `CMakeLists.txt`
forces `BUILD_TESTING OFF`; building the app never compiles a test. Instead each
suite is a small, self-contained CMake project that **recompiles only the handful
of `.cpp` it needs** and stubs out anything that would drag in the heavy
dependencies (most notably `PpLogStream`, whose real implementation in
`pp_debug.cpp` pulls in whisper/ggml).

Two consequences worth internalising:

- There is **no `pinpoint_analysis` / `pinpoint_core` / `pinpoint_gui` library.**
  Tests list source files directly. The only real library target is
  `pinpoint_buffer` (`src/Buffer`), which the EventBuffer and IMU suites link.
- Tests must be **buildable offline-ish and fast.** Anything a test needs that
  the app would normally provide (a log sink, `EventBuffer::nowMicros`, …) is
  supplied by a local stub file in the suite.

This decoupling is the property to preserve. Do not "simplify" by folding the
suites into the app build under `BUILD_TESTING ON` — that reintroduces the heavy
configure on every test iteration.

---

## 2. The big picture

```
tests/
├── CMakeLists.txt            # the umbrella: builds all suites at once
├── CMakePresets.json         # tests / tests-asan / tests-tsan presets
├── cmake/
│   └── PinPointTests.cmake   # shared infra: Qt prefix, Eigen, sanitizers, pp_add_test
└── MIGRATION.md              # how the suites were consolidated (historical record)

src/<Sub>/tests/
└── CMakeLists.txt            # one suite; uses pp_add_test; works standalone too
```

Every suite runs **two ways**, from the same `src/<Sub>/tests/CMakeLists.txt`:

- **Standalone** — `cmake -S src/<Sub>/tests -B <build>` for fast single-suite
  iteration. A small bootstrap block at the top of each suite pulls in the shared
  module.
- **Under the umbrella** — `cmake -S tests -B build/tests` configures *all* suites
  in one shot; the bootstrap block is skipped (the shared module is already
  loaded) and the suite is `add_subdirectory`'d.

This is the same file in both cases — there is no duplication and no "umbrella
copy" of a suite to keep in sync.

---

## 3. How the umbrella orchestration works

`tests/CMakeLists.txt` is a thin driver:

```cmake
cmake_minimum_required(VERSION 3.16)
project(pinpoint_tests LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PinPointTests)        # Qt prefix, Eigen locate, sanitizers, gtest, pp_add_test
enable_testing()

# Buffer is special — see below.
add_subdirectory("${PP_SRC}/Buffer"       "${CMAKE_BINARY_DIR}/Buffer-lib")
add_subdirectory("${PP_SRC}/Buffer/tests" "${CMAKE_BINARY_DIR}/Buffer")

foreach(suite Analysis Audio Core Gui IMU Pose Update)
    add_subdirectory("${PP_SRC}/${suite}/tests" "${CMAKE_BINARY_DIR}/${suite}")
endforeach()
```

Three details make this work:

1. **The shared module is included once, at the top.** It defines `pp_add_test`,
   `pp_find_eigen`, the sanitizer knob and the repo-layout variables (`PP_SRC`,
   `PP_BUFFER`, `PP_CORE`, `PP_THIRD`). Because it is loaded before any
   `add_subdirectory`, every suite sees `pp_add_test` already defined — which is
   exactly the signal each suite's bootstrap block uses to decide it is running
   under the umbrella (see §7).

2. **Buffer is special-cased.** Its tests live under `src/Buffer/tests` but are
   gated on `PROJECT_IS_TOP_LEVEL` *and* need the real `pinpoint_buffer` library
   target. So the umbrella adds the Buffer library first, then pulls in
   `src/Buffer/tests` directly. The IMU suite also needs `pinpoint_buffer`; it
   guards its own `add_subdirectory(Buffer)` with `if(NOT TARGET pinpoint_buffer)`
   so the umbrella and standalone paths don't collide ("binary dir already used").

3. **A default C++20 standard** is set by the shared module. Suites added as
   sibling directories don't inherit a parent suite's scope-local
   `CMAKE_CXX_STANDARD`, so the shared module pins the app's standard (C++20)
   globally. Individual targets override with `STD 17` / `STD 20` on
   `pp_add_test` where they care.

One configure produces one CTest registry spanning all suites:

```bash
cmake -S tests -B build/tests
cmake --build build/tests -j6
ctest --test-dir build/tests --output-on-failure   # all 117 targets
```

---

## 4. The shared module — `tests/cmake/PinPointTests.cmake`

`include(PinPointTests)` sets up everything a suite needs. It is idempotent
(`include_guard(GLOBAL)`), so the umbrella and a standalone bootstrap can both
include it safely.

What it provides:

| Thing | Detail |
|---|---|
| **Repo layout** | `PP_REPO_ROOT`, `PP_SRC`, `PP_BUFFER`, `PP_CORE`, `PP_THIRD` |
| **Qt prefix** | Per-platform default (`~/Qt/6.11.0/macos`, `…/gcc_64`, `…/msvc2022_64`) unless `-DCMAKE_PREFIX_PATH` / env already set. Replaces the stale `gcc_64`-everywhere docs. |
| **Qt + Threads** | `find_package(Qt6 Core Gui)` and `find_package(Threads)` once. Suites needing more (Qml, Bluetooth, OpenCV, Test for `QSignalSpy`) add their own — cheap and additive. |
| **C++ standard** | Default C++20 (app/Buffer parity); per-target `STD` overrides. |
| **`pp_find_eigen(<out>)`** | One Eigen locator: explicit `-DPP_EIGEN_DIR`, then any app build's `build/*/_deps/eigen-src` (matched regardless of build-dir name), then fetch 3.4.0. |
| **`pp_apply_sanitizers(<tgt>)`** | Applied automatically by `pp_add_test` from the `PP_SANITIZE` cache var. |
| **`pp_require_gtest()`** | Lazily fetches GoogleTest the first time a suite links it — gtest-free suites never pay for it. Called automatically by `pp_add_test` when `GTEST`/`GTEST_NO_MAIN` is requested. |
| **`pp_add_test(...)`** | The canonical test-target helper (see §5). |

### Eigen — the bug this fixed

Before consolidation, `src/Analysis/tests` globbed `build/*/_deps/eigen-src`
(correct) while `src/IMU/tests` **hardcoded the Linux build-dir name**
(`build/Desktop_Qt_6_11_0-Debug/...`). On macOS that path never matched, so
`eskf_gyro_units_test` was silently *skipped* — not failed, just absent. The
single `pp_find_eigen()` routine removed that whole class of per-suite path drift.
Always resolve Eigen through it.

### Sanitizers — one knob

All suites share `-DPP_SANITIZE`:

```bash
cmake -S tests -B build/tests-asan -DPP_SANITIZE="address;undefined"
cmake -S tests -B build/tests-tsan -DPP_SANITIZE=thread
```

Use a **separate build dir per sanitizer config** (the flags bake into objects).
`pp_add_test` applies them to every target automatically.

Two gotchas that have each cost an afternoon:

- **The Buffer suite does not use this knob.** It predates `pp_add_test` and keeps
  its own `-DPINPOINT_ENABLE_ASAN` / `_UBSAN` / `_TSAN` options (see
  `src/Buffer/CMakeLists.txt`). `-DPP_SANITIZE` configures the umbrella without
  touching Buffer's targets — which reads as "TSan found nothing" when TSan never
  ran.
- **Never set `detect_leaks=1` on arm64 macOS.** LeakSanitizer is unsupported
  there, and every test aborts at exit — a whole suite reporting as failing, with
  nothing wrong. ASan itself is fine; it is only the leak detector.

---

## 5. `pp_add_test` reference

```cmake
pp_add_test(<name>
    SOURCES <files...>            # required — test .cpp + the production .cpp it needs
    [LINK <libs...>]              # extra link libs (Qt6::Qml, ${OpenCV_LIBS}, pinpoint_buffer, …)
    [INCLUDE <dirs...>]           # extra include dirs (PP_BUFFER is always added)
    [DEFINES <defs...>]           # extra compile definitions
    [COMPILE_OPTIONS <opts...>]   # e.g. -O2 for timing-sensitive tests
    [STD 17|20]                   # CXX_STANDARD for this target (default: C++20)
    [AUTOMOC]                     # the test or its sources contain Q_OBJECT / QML_ELEMENT
    [GTEST]                       # link GTest::gtest_main (gtest supplies main())
    [GTEST_NO_MAIN]               # link GTest::gtest (the test supplies its own main())
    [NO_QT])                      # do NOT auto-link Qt6::Core/Gui (e.g. OpenCV-only math)
```

Behaviour:

- By default the target links **Qt6::Core + Qt6::Gui** and adds **`PP_BUFFER`**
  to the include path (`types.h` is near-ubiquitous). Pass `NO_QT` for a pure
  OpenCV or header-only math test.
- `GTEST` / `GTEST_NO_MAIN` trigger the lazy GoogleTest fetch. Use
  `GTEST_NO_MAIN` when the test needs its own `main()` — e.g. to spin a
  `QCoreApplication` for a `Q_OBJECT` + `QTimer` class.
- Every target is registered with CTest (`add_test`) and gets sanitizer flags
  from `PP_SANITIZE`.

Examples (real, from the suites):

```cmake
# Pure header-only math.
pp_add_test(wrist_angles_test SOURCES wrist_angles_test.cpp)

# OpenCV-only, no Qt, optimised hot loop.
pp_add_test(shaft_tracker_test
    SOURCES shaft_tracker_test.cpp ${ANALYSIS}/shaft_tracker_math.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}  LINK ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2  NO_QT)

# GoogleTest + profiler core + a log stub (no whisper).
pp_add_test(pp_profiler_test
    SOURCES pp_profiler_test.cpp ${PROFILER_CORE}
    INCLUDE ${CORE}  LINK Threads::Threads
    DEFINES PINPOINT_PROFILE_BASELINE=1 PINPOINT_PROFILE=1  GTEST)

# Q_OBJECT controller with its own main().
pp_add_test(profiler_controller_test
    SOURCES profiler_controller_test.cpp ${MONITOR}/profiler_controller.cpp ${PROFILER_CORE}
    INCLUDE ${CORE} ${MONITOR}  LINK Threads::Threads
    GTEST_NO_MAIN  AUTOMOC)
```

Platform-specific bits that don't fit the keyword form (Apple `.mm` Metal
backends, `dxgi` on Windows) are applied **after** the `pp_add_test` call with
plain `target_sources` / `target_link_libraries` — see the GPU-metrics targets in
`src/Core/tests/CMakeLists.txt`.

---

## 6. Running the tests

### Everything at once (umbrella)

```bash
cmake -S tests -B build/tests
cmake --build build/tests -j6
ctest --test-dir build/tests --output-on-failure
```

### A single suite (standalone — fastest iteration)

```bash
cmake -S src/IMU/tests -B build/imu-tests
cmake --build build/imu-tests -j6
ctest --test-dir build/imu-tests --output-on-failure
```

(The Qt prefix is auto-resolved; pass `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.11.x/<abi>`
only if Qt is installed somewhere non-standard.)

### Via presets

`tests/CMakePresets.json` defines `tests`, `tests-asan`, `tests-tsan`. CMake
reads `CMakePresets.json` from the current directory, so run them **from
`tests/`**:

```bash
cd tests
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

### Useful CTest filters

```bash
ctest --test-dir build/tests -R shaft        # only tests whose name matches /shaft/
ctest --test-dir build/tests -j6             # run tests in parallel
ctest --test-dir build/tests --rerun-failed --output-on-failure
```

---

## 7. Adding a test to an existing suite

1. Drop `my_thing_test.cpp` into `src/<Sub>/tests/`.
2. Add one `pp_add_test` call to that suite's `CMakeLists.txt`, listing the test
   plus any production `.cpp` it must compile (remember: there is no library to
   link — name the sources). Reach for the right flags from §5.
3. If the production code touches `PpLogStream`, add the suite's existing log stub
   to `SOURCES` (e.g. `src/Core/tests/pp_log_stub.cpp`, or the suite-local
   `*_test_stubs.cpp`) — **do not** pull in `pp_debug.cpp` (it drags in whisper).
4. Build + run standalone, then once under the umbrella.

For reference, every suite's `CMakeLists.txt` begins with this bootstrap so it
works standalone *and* under the umbrella — you won't normally touch it:

```cmake
# Standalone bootstrap: pull in the shared module when configured on our own.
# Under the umbrella pp_add_test already exists, so this block is skipped.
if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_<sub>_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()
```

---

## 8. Adding a brand-new suite

Put tests **next to the code they exercise**, in a `tests/` subfolder of that
subsystem — `src/<NewSub>/tests/`. This keeps the relative `../foo.cpp` includes
short and the co-located stubs/fixtures obvious. (Don't create a single global
`tests/` dump of all test sources — only the *orchestration* is centralised, not
the test files.)

**A `tests/` folder does not have to mean a new suite.** `src/Diagnostics/tests/`,
`src/Export/tests/` and `src/Metrics/tests/` hold sources with no `CMakeLists.txt`
of their own — the Analysis suite compiles them by absolute path
(`SOURCES ${SRC}/Export/tests/swing_doc_test.cpp …`), because each links
`swing_analysis.h` or the pack value types and a separate project would duplicate
that whole dependency set for a handful of targets. Prefer this when the new tests
would pull in the same dependencies as an existing suite; add a real suite only
when the dependency set genuinely differs.

1. Create `src/<NewSub>/tests/CMakeLists.txt` starting with the bootstrap block
   from §7 (substitute the project name).
2. Add your `pp_add_test` calls.
3. Register the suite in the umbrella — add its name to the `foreach` list in
   `tests/CMakeLists.txt` (or, if it needs a library like Buffer, add a
   dedicated `add_subdirectory` with the same special-casing).
4. Verify standalone, then under the umbrella.
5. Add a one-line entry to the suite catalog in `BUILDING.md`.

If the new suite needs Eigen, call `pp_find_eigen(<var>)`. If it needs a
dependency the shared module doesn't `find_package` (OpenCV, Qt6::Bluetooth,
Qt6::Qml), `find_package` it in the suite — that is additive and cached.

---

## 9. Conventions and gotchas

- **Test framework is per-suite, not enforced.** Buffer, Core and Update use
  GoogleTest; Analysis, Audio, Gui, IMU and Pose use a hand-rolled `main()` +
  `CHECK`/`CHECK_NEAR` macros. Both are fine — match the suite you're editing.
- **`AUTOMOC` is per-target now.** The old suites set a global `CMAKE_AUTOMOC ON`;
  with `pp_add_test` you pass `AUTOMOC` on the targets that have `Q_OBJECT` /
  `QML_ELEMENT`. Forgetting it shows up as undefined-symbol link errors for the
  class's signals/vtable.
- **A `Q_OBJECT` header with no paired `.cpp` must be listed in `SOURCES`.** AUTOMOC
  moc's a header automatically when it shares a basename with a compiled `.cpp`
  (e.g. `foo.h` ↔ `foo.cpp`). A pure-interface or test-double header that has *no*
  matching `.cpp` (e.g. an abstract base, or a `FakeXxx` test double) is **not**
  picked up just by being `#include`d — list the `.h` itself in `SOURCES` so AUTOMOC
  generates its `moc_*.cpp`. Symptom otherwise: `undefined reference to vtable` /
  `staticMetaObject` for that class. (The Update suite lists `update_backend.h` and
  `fake_update_backend.h` for exactly this reason.)
- **`QSignalSpy` lives in `Qt6::Test`, not Core/Gui.** A suite that spies on signal
  emissions/ordering must `find_package(Qt6 COMPONENTS Test)` and `LINK Qt6::Test`.
- **Compiling a real settings-backed class? Isolate QSettings.** The Update policy
  test compiles the real `AppSettings`/`SessionController` (they expose exactly the
  API the controller touches) rather than maintaining doubles. Because `AppSettings`
  uses a fixed `QSettings` org/app, the test's `main()` calls
  `QStandardPaths::setTestModeEnabled(true)` **before** `QCoreApplication` so it
  never reads or writes the developer's real settings file.
- **Never link `pp_debug.cpp`.** It pulls in whisper/ggml. Use a `PpLogStream`
  stub instead. Three exist today (`Core/tests/pp_log_stub.cpp`,
  `Analysis/tests/imu_test_stubs.cpp`, `Pose/tests/pose_test_stubs.cpp`); reuse
  the nearest one.
- **C++20 unless you say otherwise.** Anything pulling in `event_buffer.h` →
  `swing_window.h` (`std::span`) needs C++20 — that's the default, so it just
  works; only set `STD 17` to deliberately pin an older standard.
- **Sanitizers need their own build dir.** Don't reconfigure an existing dir with
  `PP_SANITIZE` — make `build/tests-asan` / `build/tests-tsan`.
- **Windows/MSVC:** the OpenCV-using suites (Analysis, Pose) need
  `-DOpenCV_DIR=...` at configure (the app's auto-probe isn't in the test
  projects), and CTest needs Qt's `bin` + OpenCV's `bin` on `PATH` or the test
  exes fail with `0xc0000135` (DLL not found).
- **`build/` is gitignored.** Build dirs are disposable; never commit them.
- **Never name a lambda `emit`.** It is a Qt keyword macro. The symptom is not "redefinition" but a
  wall of `left operand of comma operator has no effect` on every call site, because the macro
  expands to nothing and `push(a, b, c)` becomes a comma expression.
- **A `find()` helper returns a pointer INTO a vector — bind the vector.**
  `find(buildXSeries(...), "key")` dangles the moment the full expression ends, and the crash is a
  null deref somewhere later that looks nothing like the cause. `club_delivery_test` was written
  that way first and segfaulted; every call there now goes through a named `const auto`.
- **Make a synthetic swing look like a swing, in TIME as well as geometry.** Producers resolve their
  address reference over a ±250 ms window about the Address event. A fixture compressed into 160 ms
  puts the TOP inside that window and contaminates the very reference the metric is measured
  against — which reads as a producer bug and is not one. `body_rotation_test` builds ~1.6 s at 240
  fps for exactly this reason.
- **Assert SIGNS on synthetic tracks, accuracy on a corpus.** A sign is the one thing a synthetic
  fixture can pin exactly, and it is also the thing that silently grades every swing backwards when
  it is wrong. `upper_body_metrics_test` runs its whole sign suite twice — once normally, once
  through a MIRRORED camera with a left-handed golfer — because a convention that only holds for a
  right-hander filmed from one side is not a convention.

---

## 10. File map

| Path | Role |
|---|---|
| `tests/CMakeLists.txt` | Umbrella — builds all suites in one configure |
| `tests/cmake/PinPointTests.cmake` | Shared infra: Qt prefix, `pp_find_eigen`, sanitizers, lazy gtest, `pp_add_test` |
| `tests/CMakePresets.json` | `tests` / `tests-asan` / `tests-tsan` presets (run from `tests/`) |
| `tests/MIGRATION.md` | Historical record of the consolidation + remaining cleanup |
| `src/Analysis/tests/` | **84** — by far the largest. Wrist-assessment engine, the stage mechanism and one test per stage, segmentation/metrics, shaft + clubhead + ball tracking, pose helpers, scoring and norms, the diagnostics/characteristic packs, orientation filters, IMU driver parse, the shot arbiter, and the Export round-trip. Reach for `ctest -R <pattern>` here, not the whole suite. |
| `src/Buffer/tests/` | Lock-free ring, timeline merge, watchdog, thread-policy, fuzz (8, GoogleTest, links `pinpoint_buffer`). **Not migrated to `pp_add_test`** — own gtest fetch, own `PINPOINT_ENABLE_*` sanitizer options. |
| `src/Core/tests/` | Resource profiler (core, concurrency/TSan, compile-out), OS + GPU metrics, stats log, profiler controller (7, GoogleTest) |
| `src/Gui/tests/` | TimelineLabels, chart metrics, SwingSeriesModel, ShotListModel, ReanalysisController, QML reactivity (6) |
| `src/IMU/tests/` | Impact detector, ImuIoWorker, ESKF gyro-unit pin (3) |
| `src/Pose/tests/` | v2 temporal ball tracker + its live/offline parity, BallDetector throttle contract, heatmap decode, pose-model selection (5) |
| `src/Audio/tests/` | Acoustic onset detector (1) |
| `src/Diagnostics/tests/`, `src/Export/tests/`, `src/Metrics/tests/` | **Sources only — no suite of their own.** Compiled by the Analysis suite (counted in its 84) because they link `swing_analysis.h` / the pack types. See below. |
| `src/Update/tests/` | Linux updater pure logic (version compare, arch-aware AppImage asset selection, GPG VALIDSIG parse, placeholder-key refusal), `PlatformTarget` arch tokens, and `UpdateController` state-machine + relaunch session-safety policy via a `FakeUpdateBackend` (3, GoogleTest; policy test needs Qt6 Qml + Test) |

See also: the per-subsystem developer guides in this folder (e.g.
`shot_detector_developer_guide.md`, `resource_profiler_developer_guide.md`) for
what each suite actually asserts.
