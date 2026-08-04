# CTest suite for the launch monitor connector (src/LaunchMonitor). Self-contained
# — NOT part of the app build, and needs no launch monitor, no FSX2020 and no
# Windows. Two kinds of test:
#   • parser  — LastShot.CSV → LaunchMonitorReading: column matching by name,
#               unit conversion (FSX2020 can be switched to imperial), the three
#               derived values, and every way the file can be malformed or torn.
#   • pairing — which swing a reading is attributed to: arming, displacement by
#               the next shot, parking until a swingDir exists, and the watermark
#               that stops a stale file being claimed at startup.
# Shared infra (Qt prefix, sanitizers via -DPP_SANITIZE, gtest, pp_add_test) lives
# in tests/cmake/PinPointTests.cmake.
#
#   cmake -S src/LaunchMonitor/tests -B build/lm-tests
#   cmake --build build/lm-tests -j && ctest --test-dir build/lm-tests --output-on-failure

if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_launchmonitor_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

set(LM  ${CMAKE_CURRENT_SOURCE_DIR}/..)   # src/LaunchMonitor
set(SRC ${LM}/..)                          # src

# PinPointTests.cmake finds only Core + Gui; suites add what else they need.
# QSignalSpy (Qt6::Test) is how the pairing test observes readingAvailable.
find_package(Qt6 REQUIRED COMPONENTS Test)

# --- LastShot.CSV parsing: columns, units, derivations, malformed input ----------
# Pure functions only — no QObject, so no AUTOMOC.
pp_add_test(gcquad_csv_parser_test
    SOURCES gcquad_csv_parser_test.cpp
            ${LM}/gcquad_csv_parser.cpp
            ${LM}/launch_monitor_reading.cpp
    INCLUDE ${LM}
    GTEST)

# --- Attribution: which swing a reading belongs to -------------------------------
# ShotPairing is header-only and pure. GcQuadMonitor is a QObject and does real file
# I/O against a QTemporaryDir (the repo convention — tests write their own fixtures),
# so this target needs AUTOMOC and Qt6::Core's event loop for its QTimer.
pp_add_test(gcquad_pairing_test
    SOURCES gcquad_pairing_test.cpp
            ${LM}/gcquad_monitor.cpp
            ${LM}/launch_monitor_base.cpp
            ${LM}/gcquad_csv_parser.cpp
            ${LM}/launch_monitor_reading.cpp
            ${LM}/gcquad_monitor.h
            ${LM}/launch_monitor_base.h
    INCLUDE ${LM}
    LINK    Qt6::Test
    GTEST_NO_MAIN AUTOMOC)
