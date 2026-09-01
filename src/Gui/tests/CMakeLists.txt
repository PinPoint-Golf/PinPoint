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
# ALSO here, at the bottom: the QML UI suite in qml/, which loads real components offscreen and
# presses them. It used to live in the app build because it borrowed the app's QML module through a
# staged, rewritten copy — so running it meant building whisper, FFmpeg, OpenCV and espeak first,
# and it therefore ran in NO release gate on any platform. It now declares the SAME module itself
# from cmake/PinPointQmlModule.cmake and depends on no app target. See the block at the end.

if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.21)
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

# ChartMetrics::seriesGroups — the chart's METRICS presets, which are the metric catalogue's
# `.group` field joined against the series a swing actually produced. It links the REAL manifest
# (not a fixture) on purpose: the whole design is that the chart and the Metric Library share one
# grouping vocabulary, so a descriptor changing group has to be able to fail this test.
#
# The norm stack comes along because metric_providers.cpp -> reference_bands.cpp pulls it in, the
# same chain metric_catalogue_test links; pp_norm_env's JSON is not read by anything here, so the
# shipped resources are not needed and the env is left unset.
pp_add_test(chart_metrics_test
    SOURCES chart_metrics_test.cpp
            ${GUI}/review/chart_metrics.cpp
            # segments()/nearestPhase() ask TimelineLabels which phases are P-positions, so
            # the chips can never be labelled "→P2" from an untagged endpoint.
            ${GUI}/review/timeline_labels.cpp
            ${SRC}/Metrics/metric_catalogue.cpp
            ${SRC}/Metrics/metric_resolver.cpp
            ${SRC}/Metrics/metric_catalogue_manifest.cpp
            ${SRC}/Metrics/metric_providers.cpp
            ${SRC}/Analysis/reference_bands.cpp
            ${SRC}/Diagnostics/norm.cpp
            ${SRC}/Diagnostics/norm_pack.cpp ${SRC}/Diagnostics/context_tree.cpp
            ${SRC}/Diagnostics/pack_io.cpp   # the registries' shared vocabulary and LoadResult<T>
            ${SRC}/Diagnostics/characteristic_pack.cpp ${SRC}/Diagnostics/characteristic.cpp
            ${SRC}/Diagnostics/reference_pack.cpp
            ${SRC}/Diagnostics/measure_facets.cpp ${SRC}/Diagnostics/anatomy_vocabulary.cpp
            ${SRC}/Diagnostics/resource_norm_provider.cpp
            ${SRC}/Diagnostics/file_norm_provider.cpp
            ${SRC}/Diagnostics/merged_norm_provider.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core ${SRC}/Metrics ${SRC}/Analysis ${SRC}/Diagnostics
    LINK    Qt6::Qml  AUTOMOC)

# SwingSeriesModel — playhead→row mapping (rowForTimeUs) for the table cross-highlight.
pp_add_test(swing_series_model_test
    SOURCES swing_series_model_test.cpp ${GUI}/review/swing_series_model.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core
    LINK    Qt6::Qml  AUTOMOC)

# ShotListModel::shotSummary — focused-shot metadata map for PpShotActionBar.
# Links the model + the swing.json writer it references + the application log
# (pp_log_stream.cpp + PpMessageLog.cpp — the primitive no longer arrives with
# pp_debug.cpp's whisper/ggml installer behind it, so this logs for real).
pp_add_test(shot_list_model_test
    SOURCES shot_list_model_test.cpp
            ${GUI}/shot/shot_list_model.cpp
            ${SRC}/Export/swing_doc.cpp
            ${SRC}/Export/swing_paths.cpp
            ${SRC}/LaunchMonitor/launch_monitor_reading.cpp
            ${SRC}/Core/pp_log_stream.cpp
            ${SRC}/Core/PpMessageLog.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core ${SRC}/LaunchMonitor
    LINK    Qt6::Qml  AUTOMOC)

# ReanalysisController — the single re-analyse seam the carousel forwards to.
# The controller's worker body (reanalyzeSwingDir / writeSwingJson) is satisfied
# by reanalysis_stubs.cpp so the funnel test stays free of the analysis pipeline.
pp_add_test(reanalysis_controller_test
    SOURCES reanalysis_controller_test.cpp
            reanalysis_stubs.cpp
            ${GUI}/shot/reanalysis_controller.cpp
            ${SRC}/Core/pp_os_metrics.cpp   # reanalysis_controller.cpp scopes its worker thread
            ${SRC}/Core/pp_log_stream.cpp
            ${SRC}/Core/PpMessageLog.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core ${SRC}/LaunchMonitor
    LINK    Qt6::Qml Qt6::Concurrent  AUTOMOC)

# NotificationCenter — the one notification sink: stable ids, coalescing,
# cause-suppression and condition latching (design doc §7.4-7.5, item H-g).
# Drives a clock it owns, so the 7 s dwell and 30 s grace window cost nothing.
pp_add_test(notification_center_test
    SOURCES notification_center_test.cpp
            ${GUI}/app/notification_center.cpp
            ${SRC}/Core/pp_log_stream.cpp
            ${SRC}/Core/PpMessageLog.cpp
    INCLUDE ${GUI} ${SRC} ${SRC}/Core
    LINK    Qt6::Qml  AUTOMOC)

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


# ── The offscreen QML UI suite ────────────────────────────────────────────────
#
# 11 tst_*.qml that load real components, render them offscreen and press them — the only coverage
# of the QML↔C++ seam and of the layouts themselves. It used to live in the APP build, borrowing the
# app's QML module through a staged, prefer-stripped copy, which is why it ran in no release gate on
# any platform. It now declares that module itself.
#
# ⚠ DECLARED FROM src/Gui, NOT FROM HERE, AND THAT IS FORCED RATHER THAN STYLISTIC. Qt derives each
# file's qmlcachegen output directory from its path relative to CMAKE_CURRENT_SOURCE_DIR, so from
# THIS directory every one of the 157 QML files is reached by going up and the cache directory comes
# out as "qml_ui_test_../theme" — which macOS resolves and Windows ninja refuses. src/Gui is an
# ancestor of all of them, so the paths come out clean. See src/Gui/CMakeLists.txt.
add_subdirectory(${GUI} ${CMAKE_CURRENT_BINARY_DIR}/qmlui)
