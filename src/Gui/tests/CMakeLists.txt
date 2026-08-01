# CTest suite for src/Gui model/helper logic — TimelineLabels, SwingSeriesModel,
# ShotListModel and ReanalysisController. Self-contained — NOT part of the app
# build. Runs standalone or under the tests/ umbrella; shared infra lives in
# tests/cmake/PinPointTests.cmake.
#
#   cmake -S src/Gui/tests -B build/gui-tests
#   cmake --build build/gui-tests -j && ctest --test-dir build/gui-tests --output-on-failure
#
# Qt6::Qml is required because the Gui headers use QML_ELEMENT (qqmlregistration.h);
# the QML_ELEMENT classes carry Q_OBJECT, so every target enables AUTOMOC.
#
# NOT here: the QML UI suite in qml/, which loads real components offscreen and presses them. It
# needs the app's QML module — the one attached to the PinPointStudio executable — so it is built by
# the root CMakeLists and run from the app's build directory:
#
#   ctest --test-dir <app build> -R qml_ui --output-on-failure
#
# Keeping it out of here is what preserves the property stated at the top of
# tests/cmake/PinPointTests.cmake: nothing in these suites pulls in the app.

if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_gui_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

find_package(Qt6 REQUIRED COMPONENTS Qml Concurrent)

set(GUI ${CMAKE_CURRENT_SOURCE_DIR}/..)   # src/Gui
set(SRC ${GUI}/..)                         # src

# TimelineLabels — 1-D label distribution solver + nearest/active searches + phase names.
pp_add_test(timeline_labels_test
    SOURCES timeline_labels_test.cpp ${GUI}/review/timeline_labels.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core
    LINK    Qt6::Qml  AUTOMOC)

# SwingSeriesModel — playhead→row mapping (rowForTimeUs) for the table cross-highlight.
pp_add_test(swing_series_model_test
    SOURCES swing_series_model_test.cpp ${GUI}/review/swing_series_model.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core
    LINK    Qt6::Qml  AUTOMOC)

# ShotListModel::shotSummary — focused-shot metadata map for PpShotActionBar.
# Links the model + the swing.json writer it references + a stub PpLogStream
# (pp_debug.cpp drags in whisper/ggml — reuse the Core stub).
pp_add_test(shot_list_model_test
    SOURCES shot_list_model_test.cpp
            ${GUI}/shot/shot_list_model.cpp
            ${SRC}/Export/swing_doc.cpp
            ${SRC}/Export/swing_paths.cpp
            ${SRC}/Core/tests/pp_log_stub.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core
    LINK    Qt6::Qml  AUTOMOC)

# ReanalysisController — the single re-analyse seam the carousel forwards to.
# The controller's worker body (reanalyzeSwingDir / writeSwingJson) is satisfied
# by reanalysis_stubs.cpp so the funnel test stays free of the analysis pipeline.
pp_add_test(reanalysis_controller_test
    SOURCES reanalysis_controller_test.cpp
            reanalysis_stubs.cpp
            ${GUI}/shot/reanalysis_controller.cpp
            ${SRC}/Core/pp_os_metrics.cpp   # reanalysis_controller.cpp scopes its worker thread
            ${SRC}/Core/tests/pp_log_stub.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core
    LINK    Qt6::Qml Qt6::Concurrent  AUTOMOC)

# No binding may subscribe to a dependency it does not USE.
#
# A revision counter named on a line of its own is an expression statement whose value is discarded;
# the QML compiler drops it and the binding is left subscribed to nothing, answering once and then
# forever with the past. It shipped that way in the Diagnostic Model panel, it is invisible to review
# and to qmllint, and any test that loads QML through QQmlComponent::setData will pass regardless
# because the interpreter keeps what the compiler removes. So this reads the source.
pp_add_test(qml_reactivity_test
    SOURCES qml_reactivity_test.cpp
    INCLUDE ${GUI} ${SRC}
    LINK    Qt6::Core
    COMPILE_OPTIONS -DPP_QML_ROOT="${GUI}")
