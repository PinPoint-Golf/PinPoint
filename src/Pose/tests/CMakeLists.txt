# CTest suite for the v2 ball-detection core — the temporal matched-filter
# (ball_temporal.h, with a byte-exact C++<->python parity gate) plus the
# BallDetector throttle contract. Self-contained — NOT part of the app build.
# Runs standalone or under the tests/ umbrella; shared infra lives in
# tests/cmake/PinPointTests.cmake.
#
#   cmake -S src/Pose/tests -B build/pose-tests
#   cmake --build build/pose-tests -j && ctest --test-dir build/pose-tests --output-on-failure

if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_pose_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

# ball_temporal.h is OpenCV-only (no Qt); ball_detector.cpp pulls in Qt + the profiler.
find_package(OpenCV REQUIRED COMPONENTS core imgproc features2d)

set(POSE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # src/Pose
set(CORE ${POSE}/../Core)                  # profiler core

# Ball Detection v2 temporal matched-filter core (ball_temporal.h): DoG band-pass,
# robust noise, is_blob shape gate, sub-pixel peak, padded-crop response, and the
# SEARCH->CANDIDATE->LOCKED->VANISHED state machine over synthetic scenes.
pp_add_test(ball_temporal_test
    SOURCES ball_temporal_test.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    NO_QT)

# Ball v2 NUMERIC PARITY vs the python exemplar (ball_state_machine.py): run
# TemporalBallTracker over python's OWN dumped DoG response stack + baseline (NOT a
# C++ decode — independent OpenCV versions decode the H.264 to different pixels), a
# byte-exact test of the state machine. Diffs locked position / launch frame /
# median novelty against the python reference. SKIPs when the fixture is absent.
# Qt (JSON fixture parsing); the DoG/padding pipeline is covered by ball_temporal_test.
pp_add_test(ball_temporal_parity_test
    SOURCES ball_temporal_parity_test.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS})

# Tier -> pose-model decision (pose_model_selection.h): High + downloaded L -> ViTPose++-L,
# every other combination -> ViTPose-B. Pure QString logic; no ONNX Runtime (the suite
# deliberately excludes ORT — model inference is not exercised here).
pp_add_test(pose_model_selection_test
    SOURCES pose_model_selection_test.cpp
    INCLUDE ${POSE})

# Heatmap sub-pixel decode (heatmap_decode.h, WB1): DARK Gaussian-peak recovery,
# raw-score preservation, and the border/flat degenerate fallbacks to the argmax
# rule. Pure C++ — no ORT / OpenCV / Qt.
pp_add_test(heatmap_decode_test
    SOURCES heatmap_decode_test.cpp
    INCLUDE ${POSE}
    NO_QT)

# BallDetector throttle contract: exactly one ballDetected/detectionSkipped per
# detect() on every path (disabled, no-ROI, empty, seeding, lock/present, removal).
# Builds the real ball_detector.cpp (Q_OBJECT → AUTOMOC) with the real application
# log (pp_log_stream.cpp + PpMessageLog.cpp — ppWarn() no longer arrives with
# pp_debug.cpp's whisper/ggml installer behind it) and the profiler core, since
# ball_detector.cpp carries a PP_PROFILE_SCOPE.
pp_add_test(ball_detector_contract_test
    SOURCES ball_detector_contract_test.cpp
            ${CORE}/pp_log_stream.cpp
            ${CORE}/PpMessageLog.cpp
            ${POSE}/ball_detector.cpp
            ${CORE}/pp_profiler.cpp
            ${CORE}/pp_os_metrics.cpp
            ${CORE}/PpStatsLog.cpp
    INCLUDE ${POSE} ${CORE} ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    DEFINES HAVE_OPENCV
    AUTOMOC)
