# CTest suite for the IMU detector + I/O worker + ESKF gyro-unit pin.
# Self-contained — NOT part of the app build. Runs two ways:
#
#   Standalone (fast single-suite iteration):
#     cmake -S src/IMU/tests -B build/imu-tests
#     cmake --build build/imu-tests -j && ctest --test-dir build/imu-tests --output-on-failure
#
#   Under the umbrella (all suites at once):
#     cmake -S tests -B build/tests && cmake --build build/tests -j && ctest --test-dir build/tests
#
# Shared infra (Qt prefix, Eigen locate, sanitizers, pp_add_test) lives in
# tests/cmake/PinPointTests.cmake. -DPP_SANITIZE=thread builds the suite under TSan.

# Standalone bootstrap: pull in the shared module when configured on our own.
# Under the umbrella pp_add_test already exists, so this block is skipped.
if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_imu_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

set(IMU ${CMAKE_CURRENT_SOURCE_DIR}/..)   # src/IMU

# ── Impact-detector truth table ──────────────────────────────────────────────
# strike fires once; mat tap / waggle / slow swell rejected; refractory;
# orientation gate; back-dated est_t; 100↔200 Hz rate independence.
# impact_detector.h is pure math (<cmath>/<array>); no BLE, no app build.
pp_add_test(impact_detector_test
    SOURCES impact_detector_test.cpp
    STD 20)

# ── ImuIoWorker (IMU I/O thread migration, stage W0) ─────────────────────────
# The per-packet hot path extracted from ImuInstance, validated across real
# threads against the real EventBuffer: snapshot coherence, atomic calibration
# swap, ring writes + the detachBuffer stop barrier, impact detection with a
# deterministic clock, attach/detach/deregister churn. event_buffer.h →
# swing_window.h uses std::span, so this target needs C++20.
#
# The real lock-free EventBuffer is the `pinpoint_buffer` library. Under the
# umbrella that target is already added; standalone we add it ourselves (its own
# tests stay out — they are PROJECT_IS_TOP_LEVEL-gated).
if(NOT TARGET pinpoint_buffer)
    add_subdirectory(${PP_BUFFER} ${CMAKE_BINARY_DIR}/_imu_buffer)
endif()

pp_add_test(imu_io_worker_test
    SOURCES imu_io_worker_test.cpp ${IMU}/imu_io_worker.cpp
    INCLUDE ${IMU}
    LINK    pinpoint_buffer
    STD 20  AUTOMOC)

# ── EskfOrientationFilter gyro-unit pin (R2-7) ───────────────────────────────
# Pins the rad/s gyro convention through the vendored ESKF (correctGyr converts
# deg/s internally; the adapter pre-scales rad→deg). Needs Eigen + the vendored
# imu_ekf headers. pp_find_eigen() locates Eigen on every platform (was a
# hardcoded Linux build-dir path before, so this test was silently skipped on
# macOS).
pp_find_eigen(EIGEN_DIR)
pp_add_test(eskf_gyro_units_test
    SOURCES eskf_gyro_units_test.cpp ${IMU}/eskf_orientation_filter.cpp
    INCLUDE ${IMU} ${EIGEN_DIR}
            ${PP_THIRD}/imu_ekf_compat   # <Eigen.h>/<Wire.h> shims — MUST precede imu_ekf
            ${PP_THIRD}/imu_ekf          # ESKF.h, Quaternion.h
    STD 20)

# ── libhackmotion link + ABI smoke test ──────────────────────────────────────
# The dependency is linked into the app but nothing references a HackMotion
# symbol until the integration lands, so a library that compiles and fails to
# LINK would otherwise go unnoticed. This target is what makes "added for Linux,
# macOS and Windows" a claim rather than an expectation, and hm_abi_check()
# additionally catches headers and archive coming from different builds — the
# characteristic failure of a dependency that tracks a branch.
#
# NO_QT: the library is C11 with no Qt anywhere near it, and the test is a
# self-contained main(). pp_require_hackmotion() resolves the source the same way
# the app does (sibling checkout, app build's _deps, else fetch main).
pp_require_hackmotion()
pp_add_test(hackmotion_link_test
    SOURCES hackmotion_link_test.cpp
    LINK    hackmotion
    NO_QT
    STD 20)

# ── HackMotion block → ImuSample conversion pin ───────────────────────────────
# hm_sample_convert.h is the ONE place a HackMotion reading becomes an
# ImuSample, and every way of getting it wrong still looks plausible: a 0.068 %
# accel scale error (via the library's scaled m/s² field instead of the raw
# milli-g counts), a gyro re-derived with the wrong config-dependent divisor
# (out by exactly 2×), or a conjugated/swapped quaternion — which leaves the
# wrist ANGLE correct while mirroring every decomposed component, so no
# plausibility check downstream can catch it. Nothing else in the tree asserts
# any of that, so it is asserted here, against the wrong values by name.
#
# The header is pure and header-only, so there is nothing to compile alongside
# it: ImuSample comes from ${PP_BUFFER}, which pp_add_test puts on the include
# path for every target. We LINK hackmotion only to inherit its PUBLIC include
# directory — the test calls no library symbol and would link without it.
pp_add_test(hm_sample_convert_test
    SOURCES hm_sample_convert_test.cpp
    INCLUDE ${IMU}
    LINK    hackmotion
    NO_QT
    STD 20)

# The window arithmetic that decides what a recorded swing may claim about itself
# (Phase B′). Pure and header-only for exactly this reason: a HackMotion lane
# reaches swing.json as ten floats, so a sample taken before calibration and one
# taken after are byte-identical there and the device's transform cannot be
# recovered afterwards — which makes these rules the only thing standing between a
# reader and a confident wrong answer. No hackmotion link: the header takes its
# three state integers as ints rather than depending on the library enum, so this
# runs anywhere.
pp_add_test(hm_capture_provenance_test
    SOURCES hm_capture_provenance_test.cpp
    INCLUDE ${IMU}
    NO_QT
    STD 20)

# The inter-unit skew aggregation. Pure because the statistic it computes decides
# whether the application tells a coach their sensor is misbehaving — and the first
# version of that check was wrong in a way no test would have caught: it warned on
# the min/max spread, which measures ±½-sample pairing jitter rather than the
# offset, and would have fired on a healthy device. The even-count median branch is
# the other reason: getting it wrong returns the upper middle every time, which is
# right half the time and survives casual inspection.
pp_add_test(hm_skew_stats_test
    SOURCES hm_skew_stats_test.cpp
    INCLUDE ${IMU}
    NO_QT
    STD 20)
