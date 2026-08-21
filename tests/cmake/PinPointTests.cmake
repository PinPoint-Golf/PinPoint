# PinPointTests.cmake — shared infrastructure for the standalone unit-test suites.
#
# DRAFT SKELETON (Option A). Included once by tests/CMakeLists.txt (the umbrella)
# and — after migration — by each src/<Sub>/tests/CMakeLists.txt for fast single-
# suite iteration. It centralises the four things that currently diverge across
# the 7 suites:
#   1. the Qt prefix             (suites hardcode the Linux gcc_64 path in docs)
#   2. the Eigen locate routine  (Analysis globs; IMU hardcodes a Linux build dir
#                                 → eskf_gyro_units_test is silently skipped on macOS)
#   3. the sanitizer convention  (3 different flag names today)
#   4. the pp_add_test helper    (redefined 6× with subtly different link sets)
#
# Nothing here pulls in the app, whisper/ggml, FFmpeg or espeak — the lightweight,
# app-decoupled property of the current design is preserved.
#
# ⚠ THE QML MODULE IS NO LONGER ON THAT LIST, AND THE DISTINCTION IS THE WHOLE POINT.
# The Gui suite now declares the PinPointStudio QML module itself, from the same lists
# the app builds it from (cmake/PinPointQmlModule.cmake), so that the offscreen UI suite
# can press real components. What it does NOT do is depend on the app target: the module's
# C++ needs only Qt (plus Multimedia and GuiPrivate) and its link closure stays inside
# sources other suites here already compile. "Decoupled from the app" is the invariant;
# "never builds QML" was only ever a proxy for it, and holding the proxy was what kept
# this suite out of every release gate on every platform.

include_guard(GLOBAL)

# --- Default C++ standard -----------------------------------------------------
# Match the app (and src/Buffer): C++20. This propagates to every suite added by
# the umbrella, so a suite/test dir pulled in as a sibling still gets the right
# standard. Per-target `STD 17|20` on pp_add_test overrides it where needed.
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# --- MSVC math constants ------------------------------------------------------
# MSVC's <cmath> does not expose M_PI without this. Several suites need it: the
# vendored imu_ekf ESKF/Quaternion headers (reached via eskf_orientation_filter.cpp
# in both the Analysis and IMU suites) and other angle math. The app target defines
# the same globally (see root CMakeLists.txt), so centralise it here for parity.
if(MSVC)
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# --- Repo layout --------------------------------------------------------------
# This file lives at <repo>/tests/cmake/, so the repo root is two levels up.
get_filename_component(PP_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PP_REPO_ROOT "${PP_REPO_ROOT}" CACHE INTERNAL "PinPoint repo root")
set(PP_SRC    "${PP_REPO_ROOT}/src"          CACHE INTERNAL "")
set(PP_BUFFER "${PP_SRC}/Buffer"             CACHE INTERNAL "")  # types.h, swing_window.h
set(PP_CORE   "${PP_SRC}/Core"               CACHE INTERNAL "")
set(PP_THIRD  "${PP_REPO_ROOT}/third_party"  CACHE INTERNAL "")  # imu_ekf, imu_ekf_compat

# --- Qt prefix ----------------------------------------------------------------
# Shared with the app's top-level CMakeLists.txt so a developer gets the same Qt
# whether they configure the app or a suite. Picks the newest ~/Qt/<ver>/<abi>;
# see that file for why it is not a hardcoded version.
include(${PP_REPO_ROOT}/cmake/PinPointQtPrefix.cmake)

# Components needed by nearly every suite. Suites that need more (Qml, Bluetooth)
# add their own find_package — that is additive and cheap (results are cached).
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
find_package(Threads REQUIRED)

# --- googletest (lazy; only suites that link it pay the fetch/build) ----------
# Buffer and Core use gtest; the hand-rolled-main suites do not. pp_add_test
# calls this automatically when GTEST/GTEST_NO_MAIN is requested, so a standalone
# configure of a gtest-free suite (e.g. IMU) never fetches googletest.
function(pp_require_gtest)
    if(NOT TARGET GTest::gtest_main)
        include(FetchContent)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0)
        FetchContent_MakeAvailable(googletest)
    endif()
endfunction()

# --- Eigen (single locate routine — replaces Analysis' glob + IMU's hardcode) --
# Resolution order: explicit -DPP_EIGEN_DIR; any app build's FetchContent copy
# (build/*/_deps/eigen-src, matched regardless of the build-dir name); else fetch.
set(PP_EIGEN_DIR "" CACHE PATH "Eigen include root (dir containing Eigen/Core)")
function(pp_find_eigen out_var)
    if(PP_EIGEN_DIR AND EXISTS "${PP_EIGEN_DIR}/Eigen/Core")
        set(${out_var} "${PP_EIGEN_DIR}" PARENT_SCOPE)
        return()
    endif()
    file(GLOB _cand "${PP_REPO_ROOT}/build/*/_deps/eigen-src")
    foreach(_c ${_cand})
        if(EXISTS "${_c}/Eigen/Core")
            set(${out_var} "${_c}" PARENT_SCOPE)
            message(STATUS "PinPointTests: Eigen from app build ${_c}")
            return()
        endif()
    endforeach()
    message(STATUS "PinPointTests: Eigen not found under build/* — fetching 3.4.0")
    include(FetchContent)
    FetchContent_Declare(eigen
        GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git GIT_TAG 3.4.0)
    FetchContent_MakeAvailable(eigen)
    set(${out_var} "${eigen_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# --- libwrist (lazy; defines the `wrist` target) --------------------
# Shaped like pp_require_gtest rather than pp_find_eigen because what a suite
# needs here is a TARGET, not an include dir. The umbrella deliberately pulls in
# none of the app's dependencies (see the header above), so it resolves its own.
#
# Resolution order mirrors the app's, with one extra step: explicit
# -DPP_LIBWRIST_DIR; a sibling ../libwrist checkout; any app build's
# FetchContent copy (build/*/_deps/wrist-src); else fetch main from GitHub.
# Everything routes through FetchContent even when the source is already on disk,
# so an out-of-tree source still gets a binary dir inside this build.
set(PP_LIBWRIST_DIR "" CACHE PATH "libwrist source root (dir containing CMakeLists.txt)")
function(pp_require_wrist)
    if(TARGET wrist)
        return()
    endif()

    set(_wrist_src "")
    if(PP_LIBWRIST_DIR AND EXISTS "${PP_LIBWRIST_DIR}/CMakeLists.txt")
        set(_wrist_src "${PP_LIBWRIST_DIR}")
    elseif(EXISTS "${PP_REPO_ROOT}/../libwrist/CMakeLists.txt")
        get_filename_component(_wrist_src "${PP_REPO_ROOT}/../libwrist" ABSOLUTE)
    else()
        file(GLOB _cand "${PP_REPO_ROOT}/build/*/_deps/wrist-src")
        foreach(_c ${_cand})
            if(EXISTS "${_c}/CMakeLists.txt")
                set(_wrist_src "${_c}")
                break()
            endif()
        endforeach()
    endif()

    if(_wrist_src)
        message(STATUS "PinPointTests: libwrist from ${_wrist_src}")
        set(FETCHCONTENT_SOURCE_DIR_WRIST "${_wrist_src}" CACHE PATH "" FORCE)
    else()
        # Clear it, don't just skip setting it: the branch above writes FORCE, so
        # a build dir that once found a local source would keep using that path
        # after it moved away or -DPP_LIBWRIST_DIR was pointed elsewhere.
        unset(FETCHCONTENT_SOURCE_DIR_WRIST CACHE)
        message(STATUS "PinPointTests: libwrist not found locally — fetching main")
    endif()

    # The library defaults its tests, tools, FFI object, -Werror and install
    # rules off when embedded, so there is nothing to switch off here. Record is
    # the exception — it defaults ON as a library target, and no suite reads a
    # .wrwire container, so skip compiling it.
    set(WR_BUILD_RECORD OFF CACHE BOOL "" FORCE)

    include(FetchContent)
    FetchContent_Declare(wrist
        GIT_REPOSITORY https://github.com/PinPoint-Golf/libwrist.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL)

    FetchContent_MakeAvailable(wrist)
endfunction()

# --- Sanitizers (one convention for ALL suites) -------------------------------
# -DPP_SANITIZE=address  |  "address;undefined"  |  thread
# Replaces the three current spellings (PINPOINT_ENABLE_ASAN/UBSAN/TSAN,
# PP_SANITIZE, IMU_TESTS_TSAN) with a single knob applied by pp_add_test.
set(PP_SANITIZE "" CACHE STRING "Sanitizers for the test build (address;undefined;thread)")
function(pp_apply_sanitizers target)
    if(PP_SANITIZE)
        string(REPLACE ";" "," _s "${PP_SANITIZE}")
        target_compile_options(${target} PRIVATE -fsanitize=${_s} -g -fno-omit-frame-pointer)
        target_link_options(${target}    PRIVATE -fsanitize=${_s})
    endif()
endfunction()

# --- Canonical test helper ----------------------------------------------------
# pp_add_test(<name>
#     SOURCES <files...>            # required
#     [LINK <libs...>]              # extra link libs (OpenCV_LIBS, Qt6::Qml, ...)
#     [INCLUDE <dirs...>]           # extra include dirs
#     [DEFINES <defs...>]           # extra compile definitions
#     [COMPILE_OPTIONS <opts...>]   # e.g. -O2 for timing tests
#     [STD 17|20]                   # CXX_STANDARD (default: inherit)
#     [AUTOMOC]                     # Q_OBJECT in the test/sources
#     [GTEST]                       # link GTest::gtest_main
#     [GTEST_NO_MAIN]               # link GTest::gtest (suite supplies its own main)
#     [NO_QT])                      # do not auto-link Qt6::Core/Gui (e.g. OpenCV-only)
#
# Buffer's PP_BUFFER include dir is always on the path (types.h is ubiquitous).
function(pp_add_test name)
    cmake_parse_arguments(T
        "AUTOMOC;GTEST;GTEST_NO_MAIN;NO_QT"
        "STD"
        "SOURCES;LINK;INCLUDE;DEFINES;COMPILE_OPTIONS"
        ${ARGN})

    add_executable(${name} ${T_SOURCES})
    target_include_directories(${name} PRIVATE ${PP_BUFFER} ${T_INCLUDE})

    if(NOT T_NO_QT)
        target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Gui)
    endif()
    if(T_GTEST)
        pp_require_gtest()
        target_link_libraries(${name} PRIVATE GTest::gtest_main)
    endif()
    if(T_GTEST_NO_MAIN)
        pp_require_gtest()
        target_link_libraries(${name} PRIVATE GTest::gtest)
    endif()
    if(T_LINK)
        target_link_libraries(${name} PRIVATE ${T_LINK})
    endif()
    if(T_DEFINES)
        target_compile_definitions(${name} PRIVATE ${T_DEFINES})
    endif()
    if(T_COMPILE_OPTIONS)
        target_compile_options(${name} PRIVATE ${T_COMPILE_OPTIONS})
    endif()
    if(T_STD)
        set_target_properties(${name} PROPERTIES CXX_STANDARD ${T_STD} CXX_STANDARD_REQUIRED ON)
    endif()
    if(T_AUTOMOC)
        set_target_properties(${name} PROPERTIES AUTOMOC ON)
    endif()

    pp_apply_sanitizers(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()
