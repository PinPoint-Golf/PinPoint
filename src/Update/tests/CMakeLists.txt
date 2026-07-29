# CTest suite for the in-app updater (src/Update). Self-contained — NOT part of
# the app build: no network, no gpg, no appimageupdatetool, no WinSparkle/Sparkle.
# Two kinds of test:
#   • pure logic   — version compare, asset selection across arches, gpg-status
#                    parse, placeholder-key refusal, PlatformTarget arch tokens.
#   • controller   — UpdateController's state-machine + session-safety policy,
#                    driven by a FakeUpdateBackend (no real platform engine).
# Shared infra (Qt prefix, sanitizers via -DPP_SANITIZE, gtest, pp_add_test) lives
# in tests/cmake/PinPointTests.cmake.
#
#   cmake -S src/Update/tests -B build/update-tests
#   cmake --build build/update-tests -j && ctest --test-dir build/update-tests --output-on-failure

if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_update_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

set(UPDATE ${CMAKE_CURRENT_SOURCE_DIR}/..)   # src/Update
set(SRC    ${UPDATE}/..)                      # src

# --- Pure logic: version compare, asset selection (x86_64 + aarch64), gpg parse --
pp_add_test(linux_update_logic_test
    SOURCES linux_update_logic_test.cpp ${UPDATE}/linux_update_logic.cpp
    INCLUDE ${UPDATE}
    GTEST)

# --- PlatformTarget arch-token mapping (pure) ------------------------------------
pp_add_test(platform_target_test
    SOURCES platform_target_test.cpp ${UPDATE}/platform_target.cpp
    INCLUDE ${UPDATE}
    GTEST)

# --- Controller state machine + session-safety policy, via a FakeUpdateBackend ----
# Compiles the real UpdateController + the real (lightweight) AppSettings /
# SessionController, drives them through an injected fake backend. SessionController
# is a QML_ELEMENT (needs Qt6::Qml) and logs via ppWarn() (reuse Core's PpLogStream
# stub — never link pp_debug.cpp, it drags in whisper). Own main() → GTEST_NO_MAIN.
find_package(Qt6 REQUIRED COMPONENTS Qml Test)   # Qml: SessionController QML_ELEMENT; Test: QSignalSpy
pp_add_test(update_controller_policy_test
    SOURCES update_controller_policy_test.cpp
            fake_update_backend.h          # Q_OBJECT with no paired .cpp → list for AUTOMOC
            ${UPDATE}/update_backend.h      # abstract Q_OBJECT base; moc supplies its signals
            ${UPDATE}/update_controller.cpp
            ${SRC}/Gui/app/app_settings.cpp
            ${SRC}/Gui/session/session_controller.cpp
            ${SRC}/Gui/athlete/athlete_controller.cpp
            ${SRC}/Core/tests/pp_log_stub.cpp
            # athlete_controller.cpp includes Diagnostics/norm_pack.h for Cohort/cohortFor()/
            # cohortToMap(), so the definitions have to come with it. norm_pack.cpp needs
            # context_tree.cpp in turn — the same pairing every other suite that compiles it uses.
            ${SRC}/Diagnostics/norm_pack.cpp
            ${SRC}/Diagnostics/context_tree.cpp
            ${SRC}/Diagnostics/characteristic_pack.cpp
            ${SRC}/Diagnostics/characteristic.cpp
            # characteristic_pack.cpp resolves a citation against the bibliography to decide whether
            # a peer tier rests on a book, so reference_pack.cpp comes with it on every link line.
            ${SRC}/Diagnostics/reference_pack.cpp
            ${SRC}/Diagnostics/measure_facets.cpp
            ${SRC}/Diagnostics/anatomy_vocabulary.cpp
    # Diagnostics -> Metrics -> Analysis, all three: metric_reducer.h includes swing_analysis.h
    # non-relatively. Same reason as NORM_INC in src/Analysis/tests/CMakeLists.txt.
    INCLUDE ${UPDATE} ${SRC}/Core ${SRC}/Gui/app ${SRC}/Gui/session ${SRC}/Gui/athlete
            ${SRC}/Diagnostics ${SRC}/Metrics ${SRC}/Analysis
    LINK    Qt6::Qml Qt6::Test
    GTEST_NO_MAIN  AUTOMOC)
