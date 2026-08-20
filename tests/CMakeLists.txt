# PinPoint umbrella test project (DRAFT SKELETON — Option A).
#
# One configure / one build / one ctest for every standalone unit-test suite,
# WITHOUT building the app (no whisper/ggml, FFmpeg, espeak, OpenCV-app, or QML
# module). Each suite still recompiles only the .cpp it needs, exactly as today.
#
#   cmake -S tests -B build/tests          # configure all suites
#   cmake --build build/tests -j6          # build all tests
#   ctest --test-dir build/tests --output-on-failure   # run all tests
#
# Or via the presets in tests/CMakePresets.json (run from THIS dir — cmake reads
# CMakePresets.json from the current directory):
#   cd tests
#   cmake --preset tests && cmake --build --preset tests && ctest --preset tests
#   # tests-asan / tests-tsan presets build the suites under sanitizers.

# 3.21, not 3.16: the Gui suite declares a QML module (qt_add_qml_module) and Qt's QML
# macros need the newer policy set. CMakePresets.json already declared 3.21.
cmake_minimum_required(VERSION 3.21)
project(pinpoint_tests LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PinPointTests)   # Qt prefix, Eigen locate, sanitizers, gtest, pp_add_test

enable_testing()

# --- Buffer is special -------------------------------------------------------
# Its tests live under src/Buffer/tests but are PROJECT_IS_TOP_LEVEL-gated and
# need the real `pinpoint_buffer` library target. Build the library first, then
# pull its tests in directly. (Post-migration, IMU/tests must reuse this target
# instead of add_subdirectory-ing Buffer again — see MIGRATION.md.)
add_subdirectory("${PP_SRC}/Buffer"       "${CMAKE_BINARY_DIR}/Buffer-lib")
add_subdirectory("${PP_SRC}/Buffer/tests" "${CMAKE_BINARY_DIR}/Buffer")

# --- The remaining standalone suites -----------------------------------------
foreach(suite Analysis Audio Core Gui IMU LaunchMonitor Pose Update)
    add_subdirectory("${PP_SRC}/${suite}/tests" "${CMAKE_BINARY_DIR}/${suite}")
endforeach()
