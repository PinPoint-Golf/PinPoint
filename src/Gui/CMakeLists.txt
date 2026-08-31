# ── The offscreen QML UI suite lives HERE, not in tests/, and that is forced ──
#
# This CMakeLists exists for one target: qml_ui_test. It is pulled in by
# src/Gui/tests/CMakeLists.txt via add_subdirectory and is INERT for the app build, which
# never adds this directory.
#
# ⚠ WHY IT CANNOT SIT IN src/Gui/tests WITH THE OTHER GUI SUITES. qt_add_qml_module derives
# each file's qmlcachegen output directory from the path RELATIVE TO CMAKE_CURRENT_SOURCE_DIR
# (Qt6QmlMacros.cmake:3841, `file(RELATIVE_PATH file_relative ${CMAKE_CURRENT_SOURCE_DIR}
# ${file_absolute})`) — with no property to override it. Declared from src/Gui/tests, every
# one of the 157 files is reached by going UP, so the cache directory becomes
# ".rcc/qmlcache/qml_ui_test_../theme". macOS resolves that and builds; ninja on Windows
# fails outright with
#     mkdir(Gui/.rcc/qmlcache/qml_ui_test_../theme): No such file or directory
# Declared from src/Gui — an ancestor of all 157 — the relative paths are clean and both
# platforms agree. That is the whole reason for this file's location.
#
# The RESOURCE ALIASES stay repo-relative regardless (src/Gui/theme/Theme.qml), so the
# qmldir this module generates matches the app's entry for entry.

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

    # Absolute and normalised, so nothing downstream carries literal ".." segments — see the
    # note at the top of this file for what those cost on Windows.
    get_filename_component(PP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

    include(${PP_ROOT}/cmake/PinPointQmlModule.cmake)
    include(${PP_ROOT}/cmake/PinPointFonts.cmake)

    # pp_add_test cannot express this target: it hardcodes add_executable + Qt6::Core/Gui and has no
    # hook for a test ENVIRONMENT. So this one is spelled out.
    qt_add_executable(qml_ui_test
        tests/qml/qml_ui_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/app/app_settings.cpp
        # Beyond the module's own SOURCES: the link closure those types reach into. Every one of
        # these is already compiled by another suite in this umbrella, so nothing new is dragged in.
        ${PP_ROOT}/src/Metrics/metric_catalogue.cpp
        ${PP_ROOT}/src/Metrics/metric_resolver.cpp
        ${PP_ROOT}/src/Metrics/metric_catalogue_manifest.cpp
        ${PP_ROOT}/src/Metrics/metric_providers.cpp
        ${PP_ROOT}/src/LaunchMonitor/launch_monitor_reading.cpp
        ${PP_ROOT}/src/LaunchMonitor/launch_monitor_base.cpp
        ${PP_ROOT}/src/LaunchMonitor/launch_monitor_factory.cpp
        ${PP_ROOT}/src/LaunchMonitor/gcquad_monitor.cpp
        ${PP_ROOT}/src/LaunchMonitor/gcquad_csv_parser.cpp
        ${PP_ROOT}/src/Export/swing_doc.cpp
        ${PP_ROOT}/src/Export/swing_paths.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shot/shot_list_model.cpp
        ${PP_ROOT}/src/Core/pp_log_stream.cpp
        ${PP_ROOT}/src/Core/PpMessageLog.cpp)

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
    # falls back to — see loadBundledFonts() in tests/qml/qml_ui_test.cpp for the two different wrong
    # answers that produced.
    list(TRANSFORM PP_FONT_FILES PREPEND "${PP_ROOT}/")
    qt_add_resources(qml_ui_test "test_fonts"
        PREFIX "/fonts"
        BASE "${PP_ROOT}/src/Resources/fonts"
        FILES ${PP_FONT_FILES})

    set_target_properties(qml_ui_test PROPERTIES AUTOMOC ON MACOSX_BUNDLE OFF)
    target_include_directories(qml_ui_test PRIVATE
        ${SRC} ${PP_ROOT}/src/Core ${PP_ROOT}/src/Buffer ${PP_ROOT}/src/Metrics ${PP_ROOT}/src/Analysis ${PP_ROOT}/src/Diagnostics
        ${PP_ROOT}/src/Export ${PP_ROOT}/src/LaunchMonitor ${PP_ROOT}/src/Audio ${PP_ROOT}/src/Video
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app ${CMAKE_CURRENT_SOURCE_DIR}/review ${CMAKE_CURRENT_SOURCE_DIR}/viz ${CMAKE_CURRENT_SOURCE_DIR}/shot ${CMAKE_CURRENT_SOURCE_DIR}/launchmonitor
        ${CMAKE_CURRENT_SOURCE_DIR}/diagnosticmodel ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics)
    target_compile_definitions(qml_ui_test PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")
    # NOT linked against QuickShapes, deliberately — PpLmGraphicsBody's FLIGHT card draws with
    # Shape/ShapePath and that plugin resolves off the QML import path at run time exactly as it
    # does for the app, which also links only Qt6::Quick.
    target_link_libraries(qml_ui_test PRIVATE
        Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickTest Qt6::Multimedia Qt6::GuiPrivate)

    # Offscreen is set on the TEST, not asked of whoever runs it, so headless CI and a developer's
    # desk do the same thing and neither steals focus.
    add_test(NAME qml_ui COMMAND qml_ui_test)
    # ⚠ RUN_SERIAL, AND IT IS ABOUT STARVATION RATHER THAN TIDINESS. This is the only suite
    # that renders: it drives a real scene graph offscreen and presses live components, so
    # unlike every other target here its results depend on frames actually being produced.
    # Under `ctest -j` alongside 145 compute-bound tests on a loaded machine the render loop
    # is starved, a frame lands against half-settled geometry, and what should be a layout
    # assertion becomes an intermittent process-level crash. Serialised it is deterministic;
    # it was the only test in the suite that ever varied by how it was scheduled.
    set_tests_properties(qml_ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        RUN_SERIAL TRUE)
endif()
