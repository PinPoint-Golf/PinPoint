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
            ${SRC}/Core/tests/pp_log_stub.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core
    LINK    Qt6::Qml Qt6::Concurrent  AUTOMOC)
