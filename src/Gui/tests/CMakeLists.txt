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
# Links the model + the swing.json writer it references + a stub PpLogStream
# (pp_debug.cpp drags in whisper/ggml — reuse the Core stub).
pp_add_test(shot_list_model_test
    SOURCES shot_list_model_test.cpp
            ${GUI}/shot/shot_list_model.cpp
            ${SRC}/Export/swing_doc.cpp
            ${SRC}/Export/swing_paths.cpp
            ${SRC}/LaunchMonitor/launch_monitor_reading.cpp
            ${SRC}/Core/tests/pp_log_stub.cpp
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
            ${SRC}/Core/tests/pp_log_stub.cpp
    INCLUDE ${GUI} ${GUI}/review ${SRC} ${SRC}/Core ${SRC}/LaunchMonitor
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

# ── The offscreen QML UI suite ────────────────────────────────────────────────
#
# 11 tst_*.qml files that load real components, render them offscreen and press them. This is the
# ONLY coverage of the QML↔C++ seam and of the layouts themselves.
#
# ⚠ IT DECLARES THE APP'S QML MODULE RATHER THAN BORROWING IT. Previously the module was attached to
# the PinPointStudio executable, so the test could only import it via a staged copy with its
# `prefer` line stripped (cmake/StripQmlPrefer.cmake, now deleted) and could only run from an app
# build. That is why it was in no release gate on any platform, and why a 19-hour-stale app build
# on the studio PC was silently running current tests against yesterday's UI — QUICK_TEST_SOURCE_DIR
# reads the tst_*.qml live from the source tree while the module came from a build artifact.
#
# Declaring the module here compiles the same .qml into THIS binary's resources, so `prefer` points
# at resources it actually has, the staging step is unnecessary, and nothing depends on the app.
#
# ⚠ AND IT NOW RUNS AOT-COMPILED rather than interpreted. The staged-copy arrangement loaded the
# .qml from disk, which meant qmlcachegen's output was never what the suite exercised. It now runs
# the way the app runs. That is the fidelity gain; it does NOT make qml_reactivity_test redundant,
# which reads source to catch the dead statements the compiler drops (and the interpreter keeps).
if(NOT TARGET Qt6::QuickTest)
    find_package(Qt6 QUIET COMPONENTS Quick QuickTest Multimedia GuiPrivate)
endif()

if(NOT Qt6QuickTest_FOUND)
    message(STATUS "Gui tests: Qt6::QuickTest not found — offscreen QML UI suite skipped")
else()
    # Not called by tests/cmake/PinPointTests.cmake (which finds only Core + Gui and uses plain
    # add_executable); the QML macros need it.
    #
    # ⚠ REQUIRES 6.10 MATCHES THE ROOT CMakeLists EXACTLY (line 147), and that is the point rather
    # than housekeeping: the argument selects which Qt QML policies are NEW, and QTP0004 among them
    # governs how a module spanning many directories is assembled. Two modules built from one set of
    # lists under two different policy sets are two different modules, which would quietly undo the
    # reason this suite declares the app's module rather than an approximation of it.
    qt_standard_project_setup(REQUIRES 6.10)

    include(${SRC}/../cmake/PinPointQmlModule.cmake)
    include(${SRC}/../cmake/PinPointFonts.cmake)
    set(PP_ROOT ${SRC}/..)

    # pp_add_test cannot express this target: it hardcodes add_executable + Qt6::Core/Gui and has no
    # hook for a test ENVIRONMENT. So this one is spelled out.
    qt_add_executable(qml_ui_test
        qml/qml_ui_test.cpp
        ${GUI}/app/app_settings.cpp
        # Beyond the module's own SOURCES: the link closure those types reach into. Every one of
        # these is already compiled by another suite in this umbrella, so nothing new is dragged in.
        ${SRC}/Metrics/metric_catalogue.cpp
        ${SRC}/Metrics/metric_resolver.cpp
        ${SRC}/Metrics/metric_catalogue_manifest.cpp
        ${SRC}/Metrics/metric_providers.cpp
        ${SRC}/LaunchMonitor/launch_monitor_reading.cpp
        ${SRC}/LaunchMonitor/launch_monitor_base.cpp
        ${SRC}/LaunchMonitor/launch_monitor_factory.cpp
        ${SRC}/LaunchMonitor/gcquad_monitor.cpp
        ${SRC}/LaunchMonitor/gcquad_csv_parser.cpp
        ${SRC}/Export/swing_doc.cpp
        ${SRC}/Export/swing_paths.cpp
        ${GUI}/shot/shot_list_model.cpp
        ${SRC}/Core/tests/pp_log_stub.cpp)

    # ⚠ RESOURCE ALIASES ARE MANDATORY HERE, unlike in the app. The app declares the module from the
    # repo root, so each .qml is already a relative path and Qt derives the alias from it. These
    # files sit outside THIS directory, so without an explicit alias Qt refuses them outright
    # ("specified with an absolute path and is used in a Qt resource"). Setting the alias to the
    # repo-relative path is what makes the generated qmldir identical to the app's.
    set(_pp_qml_abs "")
    foreach(_f IN LISTS PP_QML_FILES)
        set(_abs "${PP_ROOT}/${_f}")
        set_source_files_properties("${_abs}" PROPERTIES QT_RESOURCE_ALIAS "${_f}")
        list(APPEND _pp_qml_abs "${_abs}")
    endforeach()
    foreach(_s IN LISTS PP_QML_SINGLETONS)
        set_source_files_properties("${PP_ROOT}/${_s}" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    endforeach()
    list(TRANSFORM PP_QML_SOURCES PREPEND "${PP_ROOT}/")

    qt_add_qml_module(qml_ui_test
        URI PinPointStudio
        VERSION 1.0
        QML_FILES ${_pp_qml_abs}
        SOURCES   ${PP_QML_SOURCES})

    # The faces the app ships. Without these every containment assertion measures whatever the HOST
    # falls back to — see loadBundledFonts() in qml/qml_ui_test.cpp for the two different wrong
    # answers that produced.
    list(TRANSFORM PP_FONT_FILES PREPEND "${PP_ROOT}/")
    qt_add_resources(qml_ui_test "test_fonts"
        PREFIX "/fonts"
        BASE "${PP_ROOT}/src/Resources/fonts"
        FILES ${PP_FONT_FILES})

    set_target_properties(qml_ui_test PROPERTIES AUTOMOC ON MACOSX_BUNDLE OFF)
    target_include_directories(qml_ui_test PRIVATE
        ${SRC} ${SRC}/Core ${SRC}/Buffer ${SRC}/Metrics ${SRC}/Analysis ${SRC}/Diagnostics
        ${SRC}/Export ${SRC}/LaunchMonitor ${SRC}/Audio ${SRC}/Video
        ${GUI} ${GUI}/app ${GUI}/review ${GUI}/viz ${GUI}/shot ${GUI}/launchmonitor
        ${GUI}/diagnosticmodel ${GUI}/diagnostics)
    target_compile_definitions(qml_ui_test PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    # NOT linked against QuickShapes, deliberately — PpLmGraphicsBody's FLIGHT card draws with
    # Shape/ShapePath and that plugin resolves off the QML import path at run time exactly as it
    # does for the app, which also links only Qt6::Quick.
    target_link_libraries(qml_ui_test PRIVATE
        Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickTest Qt6::Multimedia Qt6::GuiPrivate)

    # Offscreen is set on the TEST, not asked of whoever runs it, so headless CI and a developer's
    # desk do the same thing and neither steals focus.
    add_test(NAME qml_ui COMMAND qml_ui_test)
    set_tests_properties(qml_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
