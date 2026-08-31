# Test-suite consolidation — Option A migration plan (DRAFT for review)

This describes the per-suite edits needed to make the umbrella in `tests/`
(`CMakeLists.txt` + `cmake/PinPointTests.cmake` + `CMakePresets.json`) build all
7 suites with one configure / build / ctest, while each suite still works
standalone for fast iteration.

**Nothing in `src/**/tests/` has been edited yet.** The skeleton files are inert
until the steps below are applied. Review the skeleton first; then we apply the
migration suite-by-suite (each suite is independently verifiable, so we can do
them one at a time and keep `ctest` green throughout).

## What the shared module replaces

| Concern | Today (divergent) | After (single source) |
|---|---|---|
| Qt prefix | docs say Linux `gcc_64`; macOS is `macos` | `PinPointTests.cmake` picks per-platform |
| Eigen | Analysis globs `build/*`; **IMU hardcodes a Linux build dir → `eskf_gyro_units_test` is skipped on macOS** | `pp_find_eigen()` (glob + fetch fallback) |
| Sanitizers | `PINPOINT_ENABLE_ASAN/UBSAN/TSAN` (Buffer, Core), `PP_SANITIZE` (Gui), `IMU_TESTS_TSAN` (IMU) | one `-DPP_SANITIZE=address;undefined;thread` |
| `pp_add_test` | redefined 6× w/ different link sets | one keyword-arg helper |
| googletest | FetchContent declared in Buffer **and** Core | `pp_require_gtest()`, fetched lazily only when a suite links it |
| C++ standard | each suite sets its own (17 or 20) | shared module defaults to C++20 (app/Buffer parity); per-target `STD` overrides |

## The standard per-suite edit (applies to all 7)

Each `src/<Sub>/tests/CMakeLists.txt` gets a small bootstrap header so it works
**both** standalone and when pulled in by the umbrella, then drops its private
copies of the infra:

```cmake
# Standalone bootstrap: when configured on its own (cmake -S src/<Sub>/tests),
# pull in the shared module. Under the umbrella, pp_add_test already exists.
if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_<sub>_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()
```

Then **delete** from the suite: its own `cmake_minimum_required`/`project`, the
`find_package(Qt6 Core Gui)`, `enable_testing()`, the local `pp_add_test`
function, and any local sanitizer function. Suite-specific `find_package`
(OpenCV, Qt6::Qml, Qt6::Bluetooth, Threads) **stays**. Convert each
`pp_add_test(name a.cpp b.cpp)` to the keyword form
`pp_add_test(name SOURCES a.cpp b.cpp [LINK ...] [INCLUDE ...] [STD 20] [AUTOMOC] [GTEST])`.

## Per-suite specifics

- **Analysis** — replace the ~20-line Eigen block (lines ~138-156) with
  `pp_find_eigen(EIGEN_INCLUDE_DIR)`. Keep `find_package(OpenCV)` and
  `find_package(Qt6 ... Bluetooth)`. `pipeline_test` and `imu_driver_frame_test`
  → `STD 20`; `imu_driver_frame_test` → `AUTOMOC`. OpenCV-only targets
  (`shaft_tracker_test`) → `NO_QT`.
- **Audio** — trivial: one `pp_add_test` call, bootstrap header, delete boilerplate.
- **Buffer** — special: stays configured as `-S src/Buffer` standalone (its tests
  are `PROJECT_IS_TOP_LEVEL`-gated and need the `pinpoint_buffer` lib). Drop its
  private googletest `FetchContent` (shared owns it); keep
  `pinpoint_apply_sanitizers` **or** switch its tests to `pp_add_test ... GTEST`.
- **Core** — delete the copied `pinpoint_apply_sanitizers` and the googletest
  `FetchContent`. `STD 20`, `AUTOMOC`. Most targets `GTEST`;
  `profiler_controller_test` uses `GTEST_NO_MAIN` (own `main()`). Keep the
  Apple `.mm` / Metal handling as `LINK`/post-`target_sources`.
- **Gui** — delete the local `PP_SANITIZE` cache var (shared owns it). `AUTOMOC`;
  `LINK Qt6::Qml` (+ keep `find_package(Qt6 ... Qml)`). See "shared stub" below.
- **IMU** — the highest-value one (worked example below): replace the hardcoded
  Eigen path with `pp_find_eigen()`, guard the Buffer `add_subdirectory` so it
  doesn't collide with the umbrella's, drop `IMU_TESTS_TSAN` for shared
  `PP_SANITIZE`, `STD 20`, `AUTOMOC`.
- **Pose** — keep `find_package(OpenCV ... features2d)`. `AUTOMOC`. The two
  OpenCV-only targets (`ball_model_test`, `ball_calibration_test`) → `NO_QT`;
  `ball_detector_contract_test` keeps Qt + `DEFINES HAVE_OPENCV`.

## Conflicts the umbrella must resolve (handled in migration, not the skeleton)

1. **Double Buffer `add_subdirectory`.** The umbrella adds `src/Buffer`; today
   `src/IMU/tests` also does `add_subdirectory(../../Buffer pinpoint_buffer)`.
   Guard it: `if(NOT TARGET pinpoint_buffer) add_subdirectory(...) endif()`.
2. **Function redefinition warnings.** Disappear once suites stop defining their
   own `pp_add_test` / sanitizer helpers.
3. **`enable_testing()` at top only.** The bootstrap header guards it behind
   `if(NOT COMMAND pp_add_test)`, so it runs only in standalone configures.

## Shared stub — DONE, and by removal rather than consolidation (31 Aug 2026)

Three near-identical `PpLogStream` stubs existed (`Core/tests/pp_log_stub.cpp`,
`Analysis/tests/imu_test_stubs.cpp`, `Pose/tests/pose_test_stubs.cpp`), with Gui
reaching cross-tree into Core's. They existed because `ppWarn()` lived in
`pp_debug.cpp` beside its whisper/ggml installer, so every suite paid for a speech
model or threw its log away — and all of them threw it away.

The primitive now lives in `src/Core/pp_log_stream.cpp`, needing Qt and
`PpMessageLog` only, so there is nothing left to consolidate: every suite links
the REAL log. `Pose/tests/pose_test_stubs.cpp` is gone (it existed only for this),
`imu_test_stubs.cpp` keeps its `EventBuffer::nowMicros()` fake and nothing else,
and `Core/tests/pp_log_stub.cpp` is kept only for anything that deliberately wants
silence — nothing does.

## Suggested order (each step ends green)

1. ~~Land the skeleton — no behaviour change.~~ **done**
2. ~~Migrate **IMU** first — un-skips `eskf_gyro_units_test` on macOS (a real fix),
   smallest blast radius, exercises `pp_find_eigen` + the Buffer-target guard.~~
   **done** — standalone now runs 3 tests (was 2); verified composing with Buffer
   under a single umbrella `ctest` (11/11). The integration surfaced one shared-
   module addition: a default `CMAKE_CXX_STANDARD 20` (Buffer/tests, added as a
   sibling by the umbrella, otherwise lost Buffer's own scope-local setting).
3. ~~Migrate Audio, Pose, Gui, Core, Analysis.~~ **done** — all build + pass both
   standalone and under the umbrella. One fix surfaced: `profiler_controller_test`
   (Core) needs per-target `AUTOMOC` (it relied on the suite's old global
   `CMAKE_AUTOMOC ON`).
4. ~~Wire Buffer into the umbrella (the umbrella already special-cases it).~~
   **done** — Buffer/tests still uses its own helpers; full umbrella is green.
5. ~~Bring the offscreen QML UI suite in.~~ **done** — `qml_ui` was the one suite
   left in the APP build, because it borrowed the app's QML module through a staged,
   prefer-stripped copy. It now declares that module itself from
   `cmake/PinPointQmlModule.cmake` and depends on no app target, which is what puts
   UI coverage into the three release runbooks (they all run this umbrella and none
   of them ran that suite). `cmake/StripQmlPrefer.cmake` was deleted with it. Two
   things surfaced: the Gui suite needs its own `qt_standard_project_setup(REQUIRES 6.10)`
   matching the app's, and the suite had never loaded the app's bundled fonts — so it
   had been measuring layout in a host-chosen fallback face.
6. **TODO:** add the umbrella to CI; optionally migrate Buffer/tests onto
   `pp_add_test ... GTEST` (drop its private googletest declare + sanitizer
   helper) and consolidate the three log stubs into `tests/stubs/`.

## Status — migration complete

All 7 suites run two ways:

| | standalone | under umbrella |
|---|---|---|
| one command | `cmake -S src/<Sub>/tests -B <bd> && cmake --build <bd> && ctest --test-dir <bd>` | `cmake -S tests -B build/tests && cmake --build build/tests -j6 && ctest --test-dir build/tests` |

Full umbrella: **54/54 pass** (Analysis 28, Buffer 8, Core 7, Gui 4, IMU 3, Pose 3,
Audio 1). The +1 vs. the old manual count is IMU's `eskf_gyro_units_test`, no
longer skipped on macOS.

Presets (`tests/CMakePresets.json`) work when run from `tests/`:
`cd tests && cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
(+ `tests-asan` / `tests-tsan`). `--preset` reads `CMakePresets.json` from the
CWD, hence the `cd tests`.

Not yet done: Buffer/tests internal cleanup (step 5) — purely cosmetic, the
umbrella is already green without it.

## Worked example — `src/IMU/tests/CMakeLists.txt`

Before (excerpt — the Eigen + TSan parts that diverge):

```cmake
set(PINPOINT_EIGEN_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/Desktop_Qt_6_11_0-Debug/_deps/eigen-src"
    CACHE PATH "...")                       # ← Linux build-dir name; misses on macOS
if(EXISTS "${PINPOINT_EIGEN_DIR}/Eigen/Core")
    pp_add_test(eskf_gyro_units_test eskf_gyro_units_test.cpp ../eskf_orientation_filter.cpp)
    target_include_directories(eskf_gyro_units_test PRIVATE ...)
else()
    message(STATUS "Eigen not found ... skipping eskf_gyro_units_test")   # ← happens on macOS
endif()

option(IMU_TESTS_TSAN "..." OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../Buffer pinpoint_buffer)
```

After:

```cmake
if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_imu_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

pp_add_test(impact_detector_test SOURCES impact_detector_test.cpp)

if(NOT TARGET pinpoint_buffer)                         # umbrella already added it
    add_subdirectory(${PP_BUFFER} ${CMAKE_BINARY_DIR}/_imu_buffer)
endif()
pp_add_test(imu_io_worker_test
    SOURCES imu_io_worker_test.cpp ../imu_io_worker.cpp
    INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/..
    LINK pinpoint_buffer  STD 20  AUTOMOC)

pp_find_eigen(EIGEN_DIR)                               # ← now found on every platform
pp_add_test(eskf_gyro_units_test
    SOURCES eskf_gyro_units_test.cpp ../eskf_orientation_filter.cpp
    INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${EIGEN_DIR}
            ${PP_THIRD}/imu_ekf_compat ${PP_THIRD}/imu_ekf)
```

Net for IMU: ~30 lines deleted, the `eskf` test runs on macOS, and TSan comes
from the shared `-DPP_SANITIZE=thread` instead of a suite-private option.
