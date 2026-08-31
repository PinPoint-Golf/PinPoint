# CTest suite for the resource profiler (pp_profiler, pp_os_metrics, PpStatsLog,
# pp_gpu_metrics + the Gui profiler_controller bridge). Self-contained — NOT part
# of the app build. Runs standalone or under the tests/ umbrella; shared infra
# (Qt prefix, sanitizers via -DPP_SANITIZE, gtest, pp_add_test) lives in
# tests/cmake/PinPointTests.cmake.
#
#   cmake -S src/Core/tests -B build/core-tests
#   cmake --build build/core-tests -j && ctest --test-dir build/core-tests --output-on-failure
#
# Each profiler test compiles the Core sources it needs directly (there is no
# pinpoint_core library) + the REAL log (pp_log_stream.cpp + PpMessageLog.cpp).
# It used to be pp_log_stub.cpp, which satisfied the linker and threw every line
# away, because ppWarn() lived in the whisper/ggml-dependent pp_debug.cpp; the
# primitive stands on its own now, so these tests log for real and a future one
# can assert on a line. Tests build with BOTH tiers ON so the deep tier is
# exercised; the compile-out test builds with both OFF.

if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_core_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

set(CORE    ${CMAKE_CURRENT_SOURCE_DIR}/..)        # src/Core
set(MONITOR ${CORE}/../Gui/monitor)                # src/Gui/monitor

# The profiler core + the application log, compiled directly into each test.
set(PROFILER_CORE
    ${CORE}/pp_profiler.cpp
    ${CORE}/pp_os_metrics.cpp
    ${CORE}/PpStatsLog.cpp
    ${CORE}/pp_log_stream.cpp
    ${CORE}/PpMessageLog.cpp)

# Profiler tests with BOTH tiers compiled in (deep exercised).
pp_add_test(pp_profiler_test
    SOURCES pp_profiler_test.cpp ${PROFILER_CORE}
    INCLUDE ${CORE}  LINK Threads::Threads
    DEFINES PINPOINT_PROFILE_BASELINE=1 PINPOINT_PROFILE=1  GTEST)
pp_add_test(pp_os_metrics_test
    SOURCES pp_os_metrics_test.cpp ${CORE}/pp_os_metrics.cpp
    INCLUDE ${CORE}  LINK Threads::Threads
    DEFINES PINPOINT_PROFILE_BASELINE=1 PINPOINT_PROFILE=1  GTEST)
pp_add_test(pp_stats_log_test
    SOURCES pp_stats_log_test.cpp ${CORE}/PpStatsLog.cpp
    INCLUDE ${CORE}  LINK Threads::Threads
    DEFINES PINPOINT_PROFILE_BASELINE=1 PINPOINT_PROFILE=1  GTEST)
pp_add_test(pp_profiler_concurrency_test
    SOURCES pp_profiler_concurrency_test.cpp ${PROFILER_CORE}
    INCLUDE ${CORE}  LINK Threads::Threads
    DEFINES PINPOINT_PROFILE_BASELINE=1 PINPOINT_PROFILE=1  GTEST)

# GPU metrics — graceful-degradation contract (verifiable without a GPU). Pure
# C++ + dlopen (no Qt, no profiler core); the Metal backend is an ObjC++ .mm on
# Apple. Linked against ${CMAKE_DL_LIBS} for dlopen on glibc < 2.34.
pp_add_test(pp_gpu_metrics_test
    SOURCES pp_gpu_metrics_test.cpp ${CORE}/pp_gpu_metrics.cpp
    INCLUDE ${CORE}  LINK Threads::Threads ${CMAKE_DL_LIBS}
    NO_QT  GTEST)
if(APPLE)
    target_sources(pp_gpu_metrics_test PRIVATE ${CORE}/pp_gpu_metrics_mac.mm)
    set_source_files_properties(${CORE}/pp_gpu_metrics_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(pp_gpu_metrics_test PRIVATE "-framework Metal" "-framework Foundation")
elseif(WIN32)
    target_link_libraries(pp_gpu_metrics_test PRIVATE dxgi)
endif()

# Zero-overhead claim: BOTH tiers OFF → every macro expands to ((void)0).
pp_add_test(pp_profiler_compileout_test
    SOURCES pp_profiler_compileout_test.cpp ${PROFILER_CORE}
    INCLUDE ${CORE}  LINK Threads::Threads
    DEFINES PINPOINT_PROFILE_BASELINE=0 PINPOINT_PROFILE=0  GTEST)

# Phase 2 controller — Q_OBJECT + QTimer, so it needs a QCoreApplication: the
# test supplies its own main(), hence GTEST_NO_MAIN (GTest::gtest, not gtest_main).
pp_add_test(profiler_controller_test
    SOURCES profiler_controller_test.cpp
            ${MONITOR}/profiler_controller.cpp
            ${CORE}/pp_gpu_metrics.cpp   # profiler_controller.cpp calls gpumetrics::init/sample/reset
            ${CORE}/AnalysisProfileLog.cpp  # pullAnalysisRuns() reads the analysis-run ring
            ${PROFILER_CORE}
    INCLUDE ${CORE} ${MONITOR}  LINK Threads::Threads
    DEFINES PINPOINT_PROFILE_BASELINE=1 PINPOINT_PROFILE=1  GTEST_NO_MAIN  AUTOMOC)
if(APPLE)
    target_sources(profiler_controller_test PRIVATE ${CORE}/pp_gpu_metrics_mac.mm)
    set_source_files_properties(${CORE}/pp_gpu_metrics_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(profiler_controller_test PRIVATE "-framework Metal" "-framework Foundation")
elseif(WIN32)
    target_link_libraries(profiler_controller_test PRIVATE dxgi)
endif()
