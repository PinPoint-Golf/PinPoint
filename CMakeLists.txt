# Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc., 51
# Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

cmake_minimum_required(VERSION 3.16)

# ── Version: single source of truth is src/Core/version.h ─────────────────────
# Derive the CMake/CPack/Inno project version from version.h so the installer
# version and the in-app updaters never drift from the compiled version (Windows
# design §7). MAJOR.MINOR come from the macros; PATCH is the monotonic build number
# (PINPOINT_VERSION_BUILD) so the Inno AppVersion strictly increases every release
# and WinSparkle's compare key matches. Bump version.h only.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/version.h" _pp_version_h)
string(REGEX MATCH "#define[ \t]+PINPOINT_VERSION_MAJOR[ \t]+([0-9]+)" _ "${_pp_version_h}")
set(_pp_ver_major "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define[ \t]+PINPOINT_VERSION_MINOR[ \t]+([0-9]+)" _ "${_pp_version_h}")
set(_pp_ver_minor "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define[ \t]+PINPOINT_VERSION_BUILD[ \t]+([0-9]+)" _ "${_pp_version_h}")
set(_pp_ver_build "${CMAKE_MATCH_1}")
# POSTFIX is the prerelease suffix ("-alpha3", "" for a clean release). Used for the
# human display version (macOS CFBundleShortVersionString / Sparkle shortVersionString).
string(REGEX MATCH "#define[ \t]+PINPOINT_VERSION_POSTFIX[ \t]+\"([^\"]*)\"" _ "${_pp_version_h}")
set(_pp_ver_postfix "${CMAKE_MATCH_1}")
# Compare against "" (not NOT), since a valid MAJOR of 0 is falsey to CMake's if().
if("${_pp_ver_major}" STREQUAL "" OR "${_pp_ver_minor}" STREQUAL "" OR "${_pp_ver_build}" STREQUAL "")
    message(FATAL_ERROR "Could not parse version from src/Core/version.h")
endif()

project(PinPointStudio
    VERSION ${_pp_ver_major}.${_pp_ver_minor}.${_pp_ver_build}
    LANGUAGES CXX)

# Standard install dirs (bin/, lib/, share/). Without this CMAKE_INSTALL_BINDIR is
# empty and install(TARGETS ... RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}) would
# place the executable at the install-prefix root while the DLLs (DESTINATION bin)
# land in bin/ — splitting the runtime tree. Including this pins BINDIR=bin.
include(GNUInstallDirs)

# ── Packaged-build marker ─────────────────────────────────────────────────────
# OFF for plain dev builds (QtCreator, run-from-build/); the packaging scripts
# configure with -DPINPOINT_INSTALLED=ON so the produced bundle/installer is
# marked as a real distributable. The in-app update engines key off this (plus a
# runtime check) to stay inert in a dev tree — the cross-platform analogue of the
# Linux "$APPIMAGE unset" guard. macOS design §3; Windows uses its installer's
# unins000.exe probe today, this is the portable knob.
option(PINPOINT_INSTALLED "Mark this as a packaged/installed build" OFF)

# ── Windows runtime bundling helper ───────────────────────────────────────────
# Copy a list of DLLs/files next to the dev executable (POST_BUILD) AND register
# them for install/packaging under bin/, tagged with a CPack component. Windows
# only; assumes a flat layout next to the executable. The function body is
# evaluated lazily at call time, so it may be defined before the target exists.
function(pp_win_bundle_dll _component)
    if(NOT WIN32 OR NOT ARGN)
        return()
    endif()
    foreach(_f ${ARGN})
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_f}" "$<TARGET_FILE_DIR:PinPointStudio>"
            VERBATIM)
    endforeach()
    install(FILES ${ARGN} DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${_component})
endfunction()

# Build provenance recorded into saved shots (swing.json "capture.host"). The
# short git sha is captured at configure time — good enough for corpus
# provenance; reconfigure refreshes it.
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE PP_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PP_GIT_SHA)
    set(PP_GIT_SHA "unknown")
endif()
configure_file(cmake/pp_version.h.in "${CMAKE_BINARY_DIR}/generated/pp_version.h" @ONLY)

# Qt GuiPrivate is used for QRhi (public API since Qt 6.6, accessed via GuiPrivate includes).
set(QT_NO_PRIVATE_MODULE_WARNING ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Quick3D SerialPort Bluetooth Multimedia Network WebSockets Concurrent ShaderTools GuiPrivate CorePrivate)

qt_standard_project_setup(REQUIRES 6.10)

include(FetchContent)

FetchContent_Declare(
  whisper
  GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
  GIT_TAG        v1.7.2
  GIT_SHALLOW    TRUE
)

set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)

# ── GPU acceleration for whisper.cpp (Linux / Windows) ─────────────────────
# Vulkan works on NVIDIA, AMD, and Intel without proprietary drivers.
# CUDA is tried as a fallback when Vulkan headers are not present.
if(NOT APPLE)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
        set(_whisper_gpu_backend "Vulkan")
        message(STATUS "Whisper: Vulkan GPU acceleration enabled")
    else()
        # check_language(CUDA) is verbose even on failure; guard with a silent
        # find_program first so machines without nvcc don't get noisy output.
        find_program(_nvcc_path nvcc QUIET)
        if(_nvcc_path)
            include(CheckLanguage)
            check_language(CUDA)
        endif()
        if(CMAKE_CUDA_COMPILER)
            enable_language(CUDA)
            set(GGML_CUDA ON CACHE BOOL "" FORCE)
            # Target only the GPU(s) present; avoids compiling for old architectures
            # (e.g. compute_52) that CUDA 12.x dropped support for.
            set(CMAKE_CUDA_ARCHITECTURES native CACHE STRING "" FORCE)
            if(MSVC)
                # CUDA 13+ CCCL headers require the MSVC standard-conforming
                # preprocessor; the traditional one triggers a hard #error.
                string(APPEND CMAKE_CUDA_FLAGS " -Xcompiler /Zc:preprocessor")
            endif()
            # CUDA 13+ CUB/CCCL headers require C++17.
            set(CMAKE_CUDA_STANDARD 17 CACHE STRING "" FORCE)
            set(CMAKE_CUDA_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
            set(_whisper_gpu_backend "CUDA")
            message(STATUS "Whisper: CUDA GPU acceleration enabled")
        else()
            message(STATUS "Whisper: CPU only (install Vulkan SDK or CUDA Toolkit for GPU acceleration)")
        endif()
    endif()
endif()

FetchContent_MakeAvailable(whisper)

FetchContent_Declare(
  libsamplerate
  GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
  GIT_TAG        0.2.2
  GIT_SHALLOW    TRUE
  PATCH_COMMAND
    ${CMAKE_COMMAND}
    "-DFILE=${CMAKE_BINARY_DIR}/_deps/libsamplerate-src/CMakeLists.txt"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchLibsamplerate.cmake"
)
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING          OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libsamplerate)

# Eigen 3 (header-only) — required by the vendored ESKF orientation filter
# (third_party/imu_ekf). We only need the headers; EXCLUDE_FROM_ALL plus the
# disabled build options below keep Eigen's own tests/BLAS/doc targets out of
# the build. Fallback for offline builds: vendor the Eigen/ header tree under
# third_party/eigen/ and point the include path there instead.
set(EIGEN_BUILD_DOC       OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_TESTING   OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  eigen
  GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
  GIT_TAG        3.4.0
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(eigen)

# EventBuffer library — lock-free ring-buffer capture store
add_subdirectory(src/Buffer)

qt_add_executable(PinPointStudio
    src/Gui/main.cpp
    $<$<BOOL:${APPLE}>:${CMAKE_CURRENT_SOURCE_DIR}/src/Resources/icons/PinPointStudio.icns>
    $<$<BOOL:${WIN32}>:${CMAKE_CURRENT_SOURCE_DIR}/PinPointStudio.rc>
    src/Gui/app/app_settings.h               src/Gui/app/app_settings.cpp
    src/Gui/imu/imu_manager.h                src/Gui/imu/imu_manager.cpp
    src/Gui/imu/imu_instance.h               src/Gui/imu/imu_instance.cpp
    src/Gui/media/transcription_controller.h src/Gui/media/transcription_controller.cpp
    src/Gui/media/tts_controller.h           src/Gui/media/tts_controller.cpp
    src/Gui/media/film_controller.h          src/Gui/media/film_controller.cpp
    src/Gui/athlete/athlete_controller.h     src/Gui/athlete/athlete_controller.cpp
    src/Gui/shell/navigation_controller.h    src/Gui/shell/navigation_controller.cpp
    src/Gui/monitor/resource_monitor_controller.h src/Gui/monitor/resource_monitor_controller.cpp
    src/Gui/monitor/profiler_controller.h         src/Gui/monitor/profiler_controller.cpp
    src/Gui/app/clipboard_helper.h
    src/Gui/session/session_controller.h       src/Gui/session/session_controller.cpp
    src/Gui/review/session_review_controller.h src/Gui/review/session_review_controller.cpp
    src/Gui/review/session_list_model.h        src/Gui/review/session_list_model.cpp
    src/Gui/review/session_summary.h
    src/Gui/shot/shot_arbiter.h
    src/Gui/shot/shot_controller.h          src/Gui/shot/shot_controller.cpp
    src/Gui/shot/shot_list_model.h          src/Gui/shot/shot_list_model.cpp
    src/Gui/shot/shot_processor.h           src/Gui/shot/shot_processor.cpp
    src/Gui/shot/shot_replay_controller.h   src/Gui/shot/shot_replay_controller.cpp
    src/Gui/shot/reanalysis_controller.h    src/Gui/shot/reanalysis_controller.cpp
    src/Gui/markup/markup_truth.h           src/Gui/markup/markup_truth.cpp
    src/Gui/markup/markup_controller.h      src/Gui/markup/markup_controller.cpp
    src/Gui/markup/markup_image_provider.h
    src/Gui/shot/replay_source.h
    src/Gui/shot/disk_replay_source.h       src/Gui/shot/disk_replay_source.cpp
    src/Gui/viz/live_wrist_angles.h         src/Gui/viz/live_wrist_angles.cpp
    src/IMU/imu_base.h        src/IMU/imu_base.cpp
    src/IMU/imu_capabilities.h
    src/IMU/iorientation_filter.h
    src/IMU/orientation_filter.h
    src/IMU/eskf_orientation_filter.h  src/IMU/eskf_orientation_filter.cpp
    src/IMU/imu_calibration.h
    src/IMU/impact_detector.h
    src/IMU/imu_factory.h     src/IMU/imu_factory.cpp
    src/IMU/wt9011dcl_base.h  src/IMU/wt9011dcl_base.cpp
    src/IMU/wt9011dcl.h       src/IMU/wt9011dcl.cpp
    src/IMU/ble_adapter_pool.h  src/IMU/ble_adapter_pool.cpp
    src/IMU/ble_imu_transport.h src/IMU/ble_imu_transport.cpp
    src/IMU/wt9011dcl_ble.h   src/IMU/wt9011dcl_ble.cpp
    src/IMU/imu_io_worker.h   src/IMU/imu_io_worker.cpp
    src/Audio/onset_detector.h
    src/Audio/acoustic_shot_detector.h src/Audio/acoustic_shot_detector.cpp
    src/Audio/audio_processor_base.h src/Audio/audio_processor_base.cpp
    src/Audio/audio_input_base.h     src/Audio/audio_input_base.cpp
    src/Audio/audio_input.h          src/Audio/audio_input.cpp
    src/Audio/audio_output_base.h    src/Audio/audio_output_base.cpp
    src/Audio/audio_output.h         src/Audio/audio_output.cpp
    src/Audio/audio_stream_saver.h   src/Audio/audio_stream_saver.cpp
    src/Video/camera_capabilities.h
    src/Video/video_input_base.h           src/Video/video_input_base.cpp
    src/Video/video_input.h                src/Video/video_input.cpp
    src/Video/video_input_factory.h        src/Video/video_input_factory.cpp
    src/Video/video_preprocessor_base.h   src/Video/video_preprocessor_base.cpp
    src/Video/video_overlay_base.h        src/Video/video_overlay_base.cpp
    src/Video/raw_video_frame.h            src/Video/raw_video_frame.cpp
    src/Video/frame_crop.h                 src/Video/frame_crop.cpp
    src/Pose/pose_estimator_base.h        src/Pose/pose_estimator_base.cpp
    src/Core/device_enumerator.h           src/Core/device_enumerator.cpp
    src/Video/VideoInputAravis.h           src/Video/VideoInputAravis.cpp
    src/Gui/cameras/camera_instance.h     src/Gui/cameras/camera_instance.cpp
    src/Gui/cameras/camera_manager.h      src/Gui/cameras/camera_manager.cpp
    src/Gui/cameras/buffer_controller.h   src/Gui/cameras/buffer_controller.cpp
    src/STT/stt_processor.h          src/STT/stt_processor.cpp
    src/TTS/TTSEngine.h              src/TTS/TTSEngine.cpp
    src/TTS/TTSEngineFactory.h       src/TTS/TTSEngineFactory.cpp
    src/TTS/AzureTTSEngine.h         src/TTS/AzureTTSEngine.cpp
    src/TTS/KokoroTTSEngine.h        src/TTS/KokoroTTSEngine.cpp
    src/TTS/PhonemeTokenizer.h       src/TTS/PhonemeTokenizer.cpp
    src/TTS/TtsWorker.h              src/TTS/TtsWorker.cpp
    src/TTS/ModelDownloader.h        src/TTS/ModelDownloader.cpp
    src/LLM/LlmEngine.h              src/LLM/LlmEngine.cpp
    src/LLM/LocalLlmEngine.h         src/LLM/LocalLlmEngine.cpp
    src/LLM/GeminiLlmEngine.h        src/LLM/GeminiLlmEngine.cpp
    src/LLM/LlmWorker.h              src/LLM/LlmWorker.cpp
    src/Gui/media/llm_controller.h   src/Gui/media/llm_controller.cpp
)

target_include_directories(PinPointStudio PRIVATE
    src/Gui
    src/Gui/app src/Gui/shell src/Gui/athlete src/Gui/monitor src/Gui/cameras
    src/Gui/imu src/Gui/viz src/Gui/session src/Gui/shot src/Gui/review
    src/Gui/media src/Gui/calibration src/Gui/diagnostics src/Gui/markup
    src/IMU src/Audio src/Video src/Pose src/STT src/TTS src/LLM src/Secrets src/Core src/Buffer
    src/Analysis src/Metrics
    src/Update
    "${CMAKE_BINARY_DIR}/generated")

set_source_files_properties(src/Gui/theme/Theme.qml            PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(src/Gui/components/ClubFormat.qml  PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(src/Gui/settings/SettingsIndex.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(src/Gui/session/ViewLayout.qml     PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(src/Gui/session/SessionMode.qml    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(src/Gui/cameras/AnnotationTool.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(src/Gui/review/MetricRoute.qml     PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(PinPointStudio
    URI PinPointStudio
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/PinPointStudio
    QML_FILES
        src/Gui/theme/Theme.qml
        src/Gui/components/ClubFormat.qml
        src/Gui/session/ViewLayout.qml
        src/Gui/session/SessionMode.qml
        src/Gui/shell/Main.qml
        src/Gui/media/AudioPage.qml
        src/Gui/imu/CapturePage.qml
        src/Gui/viz/ImuVizView.qml
        src/Gui/viz/ArmVizView.qml
        src/Gui/viz/BodyVizView.qml
        src/Gui/media/FilmPage.qml
        src/Gui/cameras/VideoPage.qml
        src/Gui/cameras/PpCameraFrame.qml
        src/Gui/cameras/AnnotationTool.qml
        src/Gui/cameras/PpAnnotationIcon.qml
        src/Gui/cameras/PpAnnotationLayer.qml
        src/Gui/cameras/PpAnnotationToolbar.qml
        src/Gui/components/PpSegmentedControl.qml
        src/Gui/session/PpStagePanel.qml
        src/Gui/session/PpViewPanel.qml
        src/Gui/session/PpMotionPanel.qml
        src/Gui/session/PpClubPanel.qml
        src/Gui/cameras/PpCameraTiles.qml
        src/Gui/session/PpModeStage.qml
        src/Gui/review/PpReplayCharts.qml
        src/Gui/review/PpTransitTimeline.qml
        src/Gui/review/PpReplayTransport.qml
        src/Gui/shell/PpRail.qml
        src/Gui/shell/PpRailButton.qml
        src/Gui/components/PpDivider.qml
        src/Gui/shell/PpHeader.qml
        src/Gui/shell/PpUpdateBanner.qml
        src/Gui/shell/PpDetectCluster.qml
        src/Gui/shell/PpAnalysingBadge.qml
        src/Gui/shell/PlayPage.qml
        src/Gui/shell/ScreenPlaceholder.qml
        src/Gui/athlete/ScreenAthleteForm.qml
        src/Gui/athlete/AthleteClubsSection.qml
        src/Gui/athlete/ScreenAthletePicker.qml
        src/Gui/components/PpTextField.qml
        src/Gui/components/PpComboBox.qml
        src/Gui/components/PpChipGroup.qml
        src/Gui/components/PpUnitToggle.qml
        src/Gui/components/PpButton.qml
        src/Gui/components/PpConnectingFrame.qml
        src/Gui/components/PpPressable.qml
        src/Gui/components/PpDisplayText.qml
        src/Gui/athlete/PpAthleteCard.qml
        src/Gui/athlete/PpAthleteRow.qml
        src/Gui/monitor/ScreenResourceMonitor.qml
        src/Gui/monitor/RmDeviceCard.qml
        src/Gui/monitor/RmSourceRow.qml
        src/Gui/monitor/RmProfilerRow.qml
        src/Gui/monitor/RmStatRow.qml
        src/Gui/monitor/RmAnalysisRunRow.qml
        src/Gui/monitor/RmTimelineChart.qml
        src/Gui/monitor/RmWarningNotice.qml
        src/Gui/home/ScreenHome.qml
        src/Gui/settings/ScreenSettings.qml
        src/Gui/settings/AppearancePanel.qml
        src/Gui/settings/GeneralPanel.qml
        src/Gui/settings/DisplaysPanel.qml
        src/Gui/settings/CamerasPanel.qml
        src/Gui/settings/ImusPanel.qml
        src/Gui/settings/MicrophonesPanel.qml
        src/Gui/settings/StoragePanel.qml
        src/Gui/settings/SettingsIndex.qml
        src/Gui/home/HmTypeCard.qml
        src/Gui/session/ScreenSessionWizard.qml
        src/Gui/media/CoachPage.qml
        src/Gui/session/PpSessionToolbar.qml
        src/Gui/cameras/PpCameraPanel.qml
        src/Gui/imu/PpImuPanel.qml
        src/Gui/session/ScreenWrist.qml
        src/Gui/session/ScreenSessionMode.qml
        src/Gui/calibration/ImuCalibrationFlow.qml
        src/Gui/calibration/CameraCalibrationFlow.qml
        src/Gui/shot/PpShotCarousel.qml
        src/Gui/shot/PpShotActionBar.qml
        src/Gui/shot/PpExportOptionsSheet.qml
        src/Gui/shot/PpShotCard.qml
        src/Gui/session/PpSessionDrawer.qml
        src/Gui/shot/PpShotFilter.qml
        src/Gui/shot/PpSwingEditPanel.qml
        src/Gui/shot/PpDashboardPanel.qml
        src/Gui/shot/PpDashboardVerdictZone.qml
        src/Gui/shot/PpDashboardSetupZone.qml
        src/Gui/shot/PpDashboardMotionZone.qml
        src/Gui/shot/PpDashboardSequenceZone.qml
        src/Gui/shot/PpDashboardPresetEditor.qml
        src/Gui/shot/PpDashboardWindow.qml
        # Dashboard paint primitives — every metric renders as one of these
        # (no bare numbers); each is Shape-painted and fed pre-reduced values.
        src/Gui/shot/PpBandRail.qml
        src/Gui/shot/PpDonutStat.qml
        src/Gui/shot/PpRangeBar.qml
        src/Gui/shot/PpSequenceStrip.qml
        src/Gui/shot/PpStateChip.qml
        src/Gui/shot/PpOrientationGlyph.qml
        src/Gui/components/PpQualityPill.qml
        src/Gui/components/PpStarRating.qml
        src/Gui/components/PpTrace.qml
        src/Gui/components/PpToolPill.qml
        src/Gui/components/PpTopoBackground.qml
        src/Gui/review/PpMetricChart.qml
        src/Gui/review/PpChartPlot.qml
        src/Gui/review/PpSegmentBrush.qml
        src/Gui/review/PpChartSummary.qml
        src/Gui/review/PpSpeedSelector.qml
        src/Gui/components/PpToast.qml
        src/Gui/review/PpDataViewer.qml
        src/Gui/review/PpCoverageStrip.qml
        src/Gui/review/PpPropertiesPanel.qml
        src/Gui/review/MetricRow.qml
        src/Gui/review/NormativeBar.qml
        src/Gui/review/MetricDetail.qml
        src/Gui/review/MetricLibrary.qml
        src/Gui/review/MetricRoute.qml
        src/Gui/diagnostics/WristDiagnostics.qml
        src/Gui/diagnostics/DofTrajectoryStrip.qml
        src/Gui/diagnostics/PositionAngleGrid.qml
        src/Gui/diagnostics/WristScorePill.qml
        src/Gui/diagnostics/FindingCard.qml
        src/Gui/diagnostics/FindingsList.qml
        src/Gui/diagnostics/StrengthsList.qml
        src/Gui/session/PpMarkupPanel.qml
        src/Gui/shell/PpAboutDialog.qml
        src/Gui/shell/MacAboutMenu.qml
    SOURCES
        src/Audio/ting_player.h
        src/Audio/ting_player.cpp
        src/Video/bayer_video_item.h
        src/Video/bayer_video_item.cpp
        src/Gui/viz/body_pose_adapter.h
        src/Gui/viz/body_pose_adapter.cpp
        src/Gui/shot/shot_filter_proxy_model.h
        src/Gui/shot/shot_filter_proxy_model.cpp
        src/Gui/review/swing_data_source.h
        src/Gui/review/swing_data_source.cpp
        src/Gui/review/swing_coverage_model.h
        src/Gui/review/swing_coverage_model.cpp
        src/Gui/review/swing_series_model.h
        src/Gui/review/swing_series_model.cpp
        src/Gui/review/timeline_labels.h
        src/Gui/review/timeline_labels.cpp
        src/Gui/review/chart_metrics.h
        src/Gui/review/chart_metrics.cpp
        src/Gui/review/metric_catalog.h
        src/Gui/review/metric_catalog.cpp
        src/Gui/diagnostics/wrist_diagnostics_model.h
        src/Gui/diagnostics/wrist_diagnostics_model.cpp
        src/Analysis/reference_bands.h
        src/Analysis/reference_bands.cpp
        src/Analysis/wrist_assessment_engine.h
        src/Analysis/wrist_assessment_engine.cpp
        src/Analysis/wrist_analysis_adapter.h
        src/Analysis/wrist_analysis_adapter.cpp
        src/Analysis/assessment_rule.h
        src/Analysis/assessment_rules.h
        src/Analysis/assessment_rules.cpp
)

qt6_add_shaders(PinPointStudio "debayer_shaders"
    BATCHABLE
    PRECOMPILE
    PREFIX "/shaders"
    FILES
        "src/Shaders/debayer.vert"
        "src/Shaders/debayer.frag"
)

qt6_add_shaders(PinPointStudio "bayer_demosaic_shaders"
    PRECOMPILE
    OPTIMIZED
    PREFIX "/shaders"
    FILES
        "src/Shaders/bayer_demosaic.vert"
        "src/Shaders/bayer_demosaic.frag"
)

qt6_add_shaders(PinPointStudio "topo_shaders"
    BATCHABLE
    PRECOMPILE
    OPTIMIZED
    PREFIX "/shaders"
    FILES
        "src/Shaders/topo.vert"
        "src/Shaders/topo.frag"
)

target_compile_definitions(PinPointStudio PRIVATE
    QT_APPLICATION_NAME="PinPointStudio"
)

# ── PinPointStudio debug level ──────────────────────────────────────────────────────
# Controls pp_debug.h verbosity at compile time:
#   0 = silent   1 = warnings+errors only (default for all builds)
#   2 = startup info + warnings/errors    (pass -DPINPOINT_DEBUG_LEVEL=2)
#   3 = verbose debug + all of the above
if(NOT DEFINED PINPOINT_DEBUG_LEVEL)
    set(PINPOINT_DEBUG_LEVEL 1)
endif()
message(STATUS "PinPointStudio debug level: ${PINPOINT_DEBUG_LEVEL}")
target_compile_definitions(PinPointStudio PRIVATE PINPOINT_DEBUG_LEVEL=${PINPOINT_DEBUG_LEVEL})

# ── Resource profiler ───────────────────────────────────────────────────────
# Baseline tier ships in ALL builds (incl. release) through alpha/beta; flip OFF
# for GA. Deep tier is opt-in at compile time AND gated by a runtime toggle.
option(PINPOINT_PROFILE_BASELINE "Always-on baseline resource profiling" ON)
option(PINPOINT_PROFILE          "Deep, toggle-gated resource profiling"  OFF)
message(STATUS "PinPointStudio profiler: baseline=${PINPOINT_PROFILE_BASELINE} deep=${PINPOINT_PROFILE}")
target_compile_definitions(PinPointStudio PRIVATE
    PINPOINT_PROFILE_BASELINE=$<BOOL:${PINPOINT_PROFILE_BASELINE}>
    PINPOINT_PROFILE=$<BOOL:${PINPOINT_PROFILE}>)

target_sources(PinPointStudio PRIVATE
    src/Core/pp_debug.h
    src/Core/pp_debug.cpp
    src/Core/app_info.h
    src/Core/app_info.cpp
    src/Core/pp_settings.h
    src/Core/PpMessageLog.h
    src/Core/PpMessageLog.cpp
    src/Core/PpStatsLog.h
    src/Core/PpStatsLog.cpp
    src/Core/AnalysisProfileLog.h
    src/Core/AnalysisProfileLog.cpp
    src/Core/pp_profiler.h
    src/Core/pp_profiler.cpp
    src/Core/pp_os_metrics.h
    src/Core/pp_os_metrics.cpp
    src/Core/pp_gpu_metrics.h
    src/Core/pp_gpu_metrics.cpp
)

# ── Application icon ─────────────────────────────────────────────────────────
if(APPLE)
    set(MACOSX_BUNDLE_ICON_FILE PinPointStudio)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Resources/icons/PinPointStudio.icns"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
endif()

set_target_properties(PinPointStudio PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_ICON_FILE PinPointStudio
    WIN32_EXECUTABLE TRUE
)

# Packaged-build marker (see the option() above) — compiled in only when the
# packaging scripts pass -DPINPOINT_INSTALLED=ON.
if(PINPOINT_INSTALLED)
    target_compile_definitions(PinPointStudio PRIVATE PINPOINT_INSTALLED)
endif()

# ── Info.plist substitution variables (macOS) ────────────────────────────────
# Single source of truth is src/Core/version.h (parsed above). These feed the
# @VAR@ placeholders in Info.plist.in so the bundle version, the Sparkle compare
# key, and the appcast can never drift (macOS design §7).
#   • CFBundleShortVersionString = display version (e.g. 0.1-alpha3)
#   • CFBundleVersion            = monotonic build int (Sparkle's compare key)
set(PP_DISPLAY_VERSION "${_pp_ver_major}.${_pp_ver_minor}${_pp_ver_postfix}")
set(PP_VERSION_BUILD   "${_pp_ver_build}")

# Sparkle feed + pinned EdDSA public key (macOS design §4.1, §6). The feed URL is
# static (GitHub latest-release redirect); the key is read from the repo's pinned
# .pub so the generated Info.plist carries it as SUPublicEDKey. PLACEHOLDER until
# the offline key is generated — the macOS updater (Stage 2) refuses to arm while
# it is the placeholder, exactly as the Windows/Linux gates do.
set(PP_SU_FEED_URL "https://github.com/PinPoint-Golf/PinPointStudio/releases/latest/download/appcast-mac.xml")
set(_pp_mac_eddsa_pub "${CMAKE_CURRENT_SOURCE_DIR}/src/Resources/keys/pinpoint_release_mac_eddsa.pub")
if(EXISTS "${_pp_mac_eddsa_pub}")
    file(READ "${_pp_mac_eddsa_pub}" PP_SU_PUBLIC_ED_KEY)
    string(STRIP "${PP_SU_PUBLIC_ED_KEY}" PP_SU_PUBLIC_ED_KEY)
else()
    set(PP_SU_PUBLIC_ED_KEY "PLACEHOLDER")
endif()

# Qt's CMake infrastructure ignores MACOSX_BUNDLE_INFO_PLIST in some
# configurations.  Generate the plist at configure time (substituting @VAR@ only,
# so the XML's literal text is untouched) and copy it into the bundle with a
# POST_BUILD step so it always wins.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    @ONLY
)


qt_add_resources(PinPointStudio "app_icons"
    PREFIX "/icons"
    BASE   "src/Resources/icons"
    FILES
        src/Resources/icons/pinpointstudio_256.png
        src/Resources/icons/pinpointstudio_512.png
)

# Pinned release public keys for the in-app updaters' signature gates:
#   *.asc          → Linux AppImage GPG gate      (docs/design/linux_update.md §6)
#   *_win_eddsa.pub → Windows WinSparkle EdDSA gate (docs/design/windows_update.md §6)
#   *_mac_eddsa.pub → macOS Sparkle EdDSA gate     (docs/design/macos_update.md §6)
# PLACEHOLDERS until the real keys are generated — the gates refuse every update
# while the key is a placeholder.
#
# The Linux/Windows keys are compiled in as :/keys/ Qt resources (read by the
# updater code at runtime). The macOS key is NOT a Qt resource: Sparkle reads it
# from the bundle's Info.plist (SUPublicEDKey), which CMake fills from the .pub at
# configure time (see the PP_SU_PUBLIC_ED_KEY block above). The file is committed
# for transparency and as that single source of truth.
qt_add_resources(PinPointStudio "app_keys"
    PREFIX "/keys"
    BASE "src/Resources/keys"
    FILES
        src/Resources/keys/pinpoint_release_pubkey.asc
        src/Resources/keys/pinpoint_release_win_eddsa.pub
)

qt_add_resources(PinPointStudio "app_fonts"
    PREFIX "/fonts"
    BASE "src/Resources/fonts"
    FILES
        src/Resources/fonts/Georgia.ttf
        src/Resources/fonts/Georgiab.ttf
        src/Resources/fonts/Georgiai.ttf
        src/Resources/fonts/Georgiaz.ttf
        src/Resources/fonts/DMSans-Variable.ttf
        src/Resources/fonts/DMSans-Italic-Variable.ttf
        src/Resources/fonts/DMMono-Regular.ttf
        src/Resources/fonts/DMMono-Medium.ttf
        src/Resources/fonts/DMSerifDisplay-Regular.ttf
        src/Resources/fonts/Fraunces-Variable.ttf
        src/Resources/fonts/Fraunces-Italic-Variable.ttf
        src/Resources/fonts/Fraunces-Regular.ttf
        src/Resources/fonts/Fraunces-SemiBold.ttf
        src/Resources/fonts/SourceSerif4-Variable.ttf
        src/Resources/fonts/SourceSerif4-Italic-Variable.ttf
        src/Resources/fonts/HankenGrotesk-Variable.ttf
        src/Resources/fonts/HankenGrotesk-Italic-Variable.ttf
        src/Resources/fonts/Literata-Variable.ttf
        src/Resources/fonts/Literata-Italic-Variable.ttf
        src/Resources/fonts/Literata-Regular.ttf
        src/Resources/fonts/Literata-Medium.ttf
        src/Resources/fonts/InstrumentSans-Variable.ttf
        src/Resources/fonts/JetBrainsMono-Variable.ttf
        src/Resources/fonts/PlayfairDisplay-Variable.ttf
        src/Resources/fonts/Geist-Variable.ttf
        src/Resources/fonts/GeistMono-Variable.ttf
        src/Resources/fonts/SpaceGrotesk-Variable.ttf
        src/Resources/fonts/SpaceMono-Regular.ttf
        src/Resources/fonts/SpaceMono-Bold.ttf
)

qt_add_resources(PinPointStudio "app_tiles"
    PREFIX "/assets/tiles"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/Resources/tiles"
    FILES
        src/Resources/tiles/golfswing.png
        src/Resources/tiles/grip.png
        src/Resources/tiles/feet.png
        src/Resources/tiles/coach.png
)

qt_add_resources(PinPointStudio "app_body"
    PREFIX "/assets/body"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/Resources/body"
    FILES
        src/Resources/body/ybot.glb
        src/Resources/body/arm_LeftArm.glb
        src/Resources/body/arm_LeftForeArm.glb
        src/Resources/body/arm_LeftHand.glb
        src/Resources/body/arm_RightArm.glb
        src/Resources/body/arm_RightForeArm.glb
        src/Resources/body/arm_RightHand.glb
        src/Resources/body/body_Hips.glb
        src/Resources/body/body_Spine.glb
        src/Resources/body/body_Spine1.glb
        src/Resources/body/body_Spine2.glb
        src/Resources/body/body_Head.glb
        src/Resources/body/body_LeftShoulder.glb
        src/Resources/body/body_RightShoulder.glb
        src/Resources/body/body_LeftUpLeg.glb
        src/Resources/body/body_LeftLeg.glb
        src/Resources/body/body_LeftFoot.glb
        src/Resources/body/body_RightUpLeg.glb
        src/Resources/body/body_RightLeg.glb
        src/Resources/body/body_RightFoot.glb
)

# ── Linux: deploy icon + .desktop to user dirs on every build ────────────────
# GNOME's taskbar icon comes from the .desktop file, not setWindowIcon().
# Copying to ~/.local/share avoids requiring cmake --install or sudo.
if(UNIX AND NOT APPLE)
    set(_xdg_icon_dir "$ENV{HOME}/.local/share/icons/hicolor/256x256/apps")
    set(_xdg_apps_dir "$ENV{HOME}/.local/share/applications")

    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pinpointstudio.desktop"
        CONTENT "[Desktop Entry]
Name=PinPoint Studio
Exec=$<TARGET_FILE:PinPointStudio>
Icon=pinpointstudio
Type=Application
Terminal=false
Categories=Sports;
StartupWMClass=PinPointStudio
"
    )

    add_custom_command(TARGET PinPointStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_xdg_icon_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/src/Resources/icons/pinpointstudio_256.png"
            "${_xdg_icon_dir}/pinpointstudio.png"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_xdg_apps_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/pinpointstudio.desktop"
            "${_xdg_apps_dir}/pinpointstudio.desktop"
        COMMENT "Registering PinPointStudio icon and launcher in user dirs"
    )
endif()

target_link_libraries(PinPointStudio
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Quick3D Qt6::SerialPort Qt6::Bluetooth Qt6::Multimedia Qt6::Network Qt6::WebSockets Qt6::Concurrent Qt6::GuiPrivate Qt6::CorePrivate
            pinpoint_buffer
)


target_sources(PinPointStudio PRIVATE
    src/Audio/AudioConverter.h
    src/Audio/AudioConverter.cpp
    src/STT/STTBackend.h
    src/STT/STTBackendFactory.h
    src/STT/STTBackendFactory.cpp
    src/STT/STTBackendWhisperCpp.h
    src/STT/STTBackendWhisperCpp.cpp
    src/STT/STTBackendAssemblyAI.h
    src/STT/STTBackendAssemblyAI.cpp
    src/STT/STTBackendAzure.h
    src/STT/STTBackendAzure.cpp
    src/STT/STTWorker.h
    src/STT/STTWorker.cpp
)

target_sources(PinPointStudio PRIVATE
    src/Secrets/SecretsManager.h
    src/Secrets/SecretsManager.cpp
    src/Secrets/SecretsBridge.h
    src/Secrets/SecretsBridge.cpp
)

# ── In-app update ─────────────────────────────────────────────────────────────
# UpdateController is the QML façade on ALL platforms; the platform engine behind
# it is one UpdateBackend chosen by makeUpdateBackend() (the sole #ifdef). See
# docs/design/{linux,windows,macos}_update.md.
target_sources(PinPointStudio PRIVATE
    src/Update/update_controller.h
    src/Update/update_controller.cpp
    src/Update/update_backend.h
    src/Update/update_backend_factory.h
    src/Update/update_backend_factory.cpp
    src/Update/inert_update_backend.h
    # (OS,CPU) target — arch-aware asset/feed selection; cheap, platform-agnostic.
    src/Update/platform_target.h
    src/Update/platform_target.cpp
    # Hardware-adaptive CUDA runtime offer (Windows; inert elsewhere) — design §4.4.
    src/Update/cuda_runtime_controller.h
    src/Update/cuda_runtime_controller.cpp
)

# Linux AppImage backend + its pure logic + the appimageupdatetool driver — the
# only backend that drives a full state machine. Compiled where the factory
# instantiates it (UNIX-non-Apple, matching the Q_OS_LINUX path). Pure Qt
# (Network/Concurrent/Process); no libappimageupdate/gpgme deps — design linux_update.md.
if(UNIX AND NOT APPLE)
    target_sources(PinPointStudio PRIVATE
        src/Update/linux_appimage_backend.h
        src/Update/linux_appimage_backend.cpp
        src/Update/linux_update_logic.h
        src/Update/linux_update_logic.cpp
        src/Update/appimage_update.h
        src/Update/appimage_update.cpp)
endif()

# ── WinSparkle (Windows in-app update engine) ─────────────────────────────────
# The WinSparkle/Sparkle analogue for Windows. See docs/design/windows_update.md.
# Prebuilt DLL + import lib fetched like the ORT package; linked and bundled into
# the "core" component so it travels with the small auto-update payload. EdDSA
# signature pinning requires WinSparkle >= 0.9.0.
if(WIN32)
    set(WINSPARKLE_VERSION "0.9.3")
    FetchContent_Declare(winsparkle
        URL "https://github.com/vslavik/winsparkle/releases/download/v${WINSPARKLE_VERSION}/WinSparkle-${WINSPARKLE_VERSION}.zip"
        URL_HASH SHA256=745985f41d2ab26b2d5a1cf87d76e4ed851039db19038e50610eb25ea0b73772
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(winsparkle)

    # The archive keeps a top-level WinSparkle-<ver>/ dir; probe for the header in
    # case a CMake version strips it (mirrors the ORT root probe above).
    if(EXISTS "${winsparkle_SOURCE_DIR}/include/winsparkle.h")
        set(_ws_root "${winsparkle_SOURCE_DIR}")
    else()
        file(GLOB _ws_candidates LIST_DIRECTORIES true "${winsparkle_SOURCE_DIR}/WinSparkle-*")
        list(GET _ws_candidates 0 _ws_root)
    endif()

    target_include_directories(PinPointStudio PRIVATE "${_ws_root}/include")
    target_link_libraries(PinPointStudio PRIVATE "${_ws_root}/x64/Release/WinSparkle.lib")
    target_compile_definitions(PinPointStudio PRIVATE HAVE_WINSPARKLE)
    pp_win_bundle_dll(core "${_ws_root}/x64/Release/WinSparkle.dll")

    target_sources(PinPointStudio PRIVATE
        src/Update/win_sparkle_update.h
        src/Update/win_sparkle_update.cpp
        src/Update/win_sparkle_backend.h     # UpdateBackend over WinSparkleUpdater
        src/Update/win_sparkle_backend.cpp)
endif()

# ── Sparkle (macOS in-app update engine) ──────────────────────────────────────
# The macOS analogue of WinSparkle. See docs/design/macos_update.md. The prebuilt
# universal Sparkle.framework is fetched like the ORT/WinSparkle packages, linked,
# and (in tools/package_macos.sh §4e) embedded into Contents/Frameworks/ with its
# Autoupdate/Updater.app/XPCServices helpers and re-signed bottom-up. EdDSA
# verification + the XPC helpers require Sparkle >= 2.x. The .tar.xz also carries
# bin/generate_keys + bin/sign_update — the offline release-key + appcast tools.
if(APPLE)
    set(SPARKLE_VERSION "2.9.3")
    FetchContent_Declare(sparkle
        URL "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"
        URL_HASH SHA256=74a07da821f92b79310009954c0e15f350173374a3abe39095b4fc5096916be6
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(sparkle)

    # The archive extracts Sparkle.framework at the top level; probe in case a CMake
    # version nests it (mirrors the ORT/WinSparkle root probes above).
    if(EXISTS "${sparkle_SOURCE_DIR}/Sparkle.framework/Headers/Sparkle.h")
        set(_sparkle_root "${sparkle_SOURCE_DIR}")
    else()
        file(GLOB _sparkle_candidates LIST_DIRECTORIES true "${sparkle_SOURCE_DIR}/Sparkle-*")
        list(GET _sparkle_candidates 0 _sparkle_root)
    endif()

    # -F<dir> so `#import <Sparkle/Sparkle.h>` resolves and `-framework Sparkle` links;
    # BUILD_RPATH so a dev build run from the build tree loads the framework straight
    # from _deps (its install name is @rpath/Sparkle.framework/...). The packaged build
    # gets @executable_path/../Frameworks from macdeployqt + the §4e embed step.
    target_compile_options(PinPointStudio PRIVATE "-F${_sparkle_root}")
    target_link_options(PinPointStudio PRIVATE "-F${_sparkle_root}")
    target_link_libraries(PinPointStudio PRIVATE "-framework Sparkle")
    set_property(TARGET PinPointStudio APPEND PROPERTY BUILD_RPATH "${_sparkle_root}")
    target_compile_definitions(PinPointStudio PRIVATE HAVE_SPARKLE)

    # Expose the framework + tool paths to the packaging script (read from the build
    # tree's CMakeCache) so it copies the exact fetched framework, not a guessed one.
    set(PP_SPARKLE_FRAMEWORK "${_sparkle_root}/Sparkle.framework" CACHE INTERNAL "")
    set(PP_SPARKLE_BIN "${_sparkle_root}/bin" CACHE INTERNAL "")

    target_sources(PinPointStudio PRIVATE
        src/Update/mac_sparkle_update.h
        src/Update/mac_sparkle_update.mm
        src/Update/mac_sparkle_backend.h     # UpdateBackend over MacSparkleUpdater
        src/Update/mac_sparkle_backend.cpp)  # plain C++ — Cocoa stays behind the shim
    # The shim manages two long-lived Cocoa objects via __bridge_retained/_transfer and
    # holds the postponed-relaunch block in a copy property — compile it under ARC.
    set_source_files_properties(src/Update/mac_sparkle_update.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# ── Azure cloud STT fallback ──────────────────────────────────────────────────
# The Azure STT backend uses the same Cognitive Services Speech resource as the
# TTS backend, so AZURE_TTS_API_KEY is checked automatically as a fallback when
# AZURE_STT_API_KEY is not set.  Set either env var before the first launch and
# the value is saved to QSettings so the env var is not required on future runs.
message(STATUS "Azure STT: key supplied via AZURE_STT_API_KEY env var or shared AZURE_TTS_API_KEY")

# ── AssemblyAI cloud STT (kept for reference, not used as fallback) ───────────
# Pass -DASSEMBLYAI_API_KEY=<key> at cmake configure time to seed the key into
# QSettings on first run. The key can also be supplied via the
# ASSEMBLYAI_API_KEY environment variable (takes priority over QSettings).
set(ASSEMBLYAI_API_KEY "" CACHE STRING
    "AssemblyAI API key — written to QSettings on first run, not needed after that")
if(ASSEMBLYAI_API_KEY)
    target_compile_definitions(PinPointStudio PRIVATE
        ASSEMBLYAI_API_KEY_DEFAULT="${ASSEMBLYAI_API_KEY}")
    message(STATUS "AssemblyAI: API key provided (will be seeded to QSettings on first run)")
else()
    message(STATUS "AssemblyAI: no API key configured")
endif()

target_link_libraries(PinPointStudio PRIVATE whisper samplerate)
target_include_directories(PinPointStudio PRIVATE
    ${whisper_SOURCE_DIR}/include
    ${whisper_SOURCE_DIR}/ggml/include
    ${libsamplerate_SOURCE_DIR}/include
    # ESKF orientation filter: Eigen + the vendored hobbeshunter/IMU_EKF headers.
    # imu_ekf_compat must precede imu_ekf so the vendored Arduino-style
    # <Eigen.h>/<Wire.h> includes resolve to our desktop shims.
    ${eigen_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imu_ekf_compat
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imu_ekf
)

# FFmpeg log capture — pp_debug.cpp reaches into every loaded libavutil
# instance at runtime to call av_log_set_callback() (Qt's bundled copy and the
# system/exporter package), routing FFmpeg log lines into the in-app message
# log.  No compile-time FFmpeg header/link dependency needed for this.
# UNIX uses dlopen()/dlsym() (needs libdl + this define); Windows uses
# GetModuleHandle()/GetProcAddress() guarded by _WIN32 directly in
# pp_debug.cpp — no define or extra library needed there.
if(UNIX)
    target_link_libraries(PinPointStudio PRIVATE dl)
    target_compile_definitions(PinPointStudio PRIVATE PP_HAS_DLSYM)
    message(STATUS "FFmpeg log capture: via dlsym (UNIX)")
endif()

# ── ONNX Runtime (auto-fetched prebuilt) ─────────────────────────────────────
# Three independent EP flags — each defaults ON; platform guards below control
# which ones are actually wired up and which GPU binary is downloaded.
# CPU EP is always the implicit fallback at runtime regardless of these flags.
option(WITH_CUDA       "Enable CUDA execution provider (Linux/Windows)"                        ON)
option(WITH_COREML     "Enable CoreML execution provider (macOS ARM64)"                        ON)
option(WITH_MEDIAPIPE  "Build with MediaPipe BlazePose pose estimator (requires Python + qai-hub-models)"  OFF)
option(WITH_VITPOSE    "Download and build ViTPose-B pose estimator (ONNX, all platforms)"     ON)

set(ORT_VERSION "1.26.0")

# Select prebuilt binary.
# Windows GPU packages (ORT 1.26+):
#   gpu package       — CUDA 12 + TensorRT
#   gpu_cuda13 package— CUDA 13 + TensorRT  (new in 1.26)
#   std package       — CPU only (DirectML NuGet discontinued after ORT 1.24)
# The CUDA provider DLL imports versioned CUDA DLLs (e.g. cublasLt64_12.dll /
# cublasLt64_13.dll) so the package must match the installed CUDA major version.
if(WIN32)
    if(WITH_CUDA)
        find_package(CUDAToolkit QUIET)
        if(CUDAToolkit_FOUND AND CUDAToolkit_VERSION_MAJOR EQUAL 12)
            message(STATUS "ORT CUDA EP: CUDA 12 — using GPU package")
            set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-gpu-${ORT_VERSION}.zip")
        elseif(CUDAToolkit_FOUND AND CUDAToolkit_VERSION_MAJOR EQUAL 13)
            message(STATUS "ORT CUDA EP: CUDA 13 — using CUDA 13 GPU package")
            set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-gpu_cuda13-${ORT_VERSION}.zip")
        else()
            if(CUDAToolkit_FOUND)
                message(STATUS "ORT CUDA EP: CUDA ${CUDAToolkit_VERSION} has no matching ORT package — falling back to CPU")
            endif()
            set(WITH_CUDA OFF CACHE BOOL "" FORCE)
            set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-${ORT_VERSION}.zip")
        endif()
    else()
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-${ORT_VERSION}.zip")
    endif()
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-osx-arm64-${ORT_VERSION}.tgz")
        # CoreML is included in every macOS ARM64 prebuilt — no separate download needed.
    else()
        # ONNX Runtime 1.21+ dropped Intel macOS (x86_64) prebuilts; pin to the last available.
        set(ORT_VERSION "1.20.1")
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-osx-x86_64-${ORT_VERSION}.tgz")
        # CoreML is present in the x86_64 dylib but the Kokoro model has fully
        # dynamic sequence lengths throughout — CoreML on Intel rejects virtually
        # every subgraph ("unbounded dimension"), then hangs or fails to compile
        # what remains. CPU is faster and more correct here.
    endif()
else()  # Linux
    if(WITH_CUDA)
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-gpu-${ORT_VERSION}.tgz")
    else()
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz")
    endif()
endif()

FetchContent_Declare(onnxruntime URL "${_ort_url}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(onnxruntime)

# The archive may or may not strip its top-level directory depending on the
# CMake version; probe for the headers to find the actual root.
if(EXISTS "${onnxruntime_SOURCE_DIR}/include/onnxruntime_cxx_api.h")
    set(_ort_root "${onnxruntime_SOURCE_DIR}")
else()
    file(GLOB _ort_candidates LIST_DIRECTORIES true "${onnxruntime_SOURCE_DIR}/onnxruntime-*")
    list(GET _ort_candidates 0 _ort_root)
endif()

target_include_directories(PinPointStudio PRIVATE "${_ort_root}/include")
target_compile_definitions(PinPointStudio PRIVATE HAVE_ONNXRUNTIME)

# Propagate each EP flag to C++ only on platforms where it is supported.
if(WITH_COREML AND APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    target_compile_definitions(PinPointStudio PRIVATE WITH_COREML)
endif()
if(WITH_CUDA AND NOT APPLE)
    target_compile_definitions(PinPointStudio PRIVATE WITH_CUDA)
endif()

# DirectML NuGet was discontinued after ORT 1.24; standard Windows zip is CPU-only
# from ORT 1.20+. No WITH_DIRECTML define on Windows — CUDA EP covers GPU on this platform.

if(WIN32)
    target_link_libraries(PinPointStudio PRIVATE "${_ort_root}/lib/onnxruntime.lib")
    # Collect all ORT DLLs (main + any provider DLLs present in the package).
    # The CUDA/TensorRT provider DLLs are included here in the "core" component:
    # they are versioned with ONNX Runtime (i.e. with the app), not with the
    # NVIDIA toolkit, so they travel with every app update. Only the NVIDIA
    # toolkit + cuDNN DLLs go in the separately-packaged "cuda" component.
    file(GLOB _ort_all_dlls "${_ort_root}/lib/onnxruntime*.dll")
    pp_win_bundle_dll(core ${_ort_all_dlls})
elseif(APPLE)
    file(GLOB _ort_dylib "${_ort_root}/lib/libonnxruntime*.dylib")
    list(GET _ort_dylib 0 _ort_dylib_path)
    target_link_libraries(PinPointStudio PRIVATE "${_ort_dylib_path}")
else()
    target_link_libraries(PinPointStudio PRIVATE "${_ort_root}/lib/libonnxruntime.so")
endif()

# cuDNN: the NVIDIA Windows installer places DLLs under a versioned subdirectory
# that is not on PATH.  Glob by CUDA major version and copy next to the executable.
if(WIN32 AND WITH_CUDA)
    file(GLOB _cudnn_dlls
        "C:/Program Files/NVIDIA/CUDNN/*/bin/${CUDAToolkit_VERSION_MAJOR}.*/x64/cudnn*.dll")
    if(_cudnn_dlls)
        # cuDNN is part of the GPU runtime → "cuda" component. Keep the dev-tree
        # copy: unlike the CUDA toolkit DLLs, cuDNN's versioned install dir is not
        # on PATH, so the dev build needs them next to the executable.
        pp_win_bundle_dll(cuda ${_cudnn_dlls})
        message(STATUS "cuDNN DLLs: ${_cudnn_dlls}")
    else()
        message(WARNING "cuDNN DLLs not found under "
            "C:/Program Files/NVIDIA/CUDNN/*/bin/${CUDAToolkit_VERSION_MAJOR}.*/x64/ — "
            "CUDA EP will fail at runtime. Install cuDNN 9 and reconfigure.")
    endif()
endif()

# CUDA toolkit runtime: the ORT CUDA EP imports versioned cuBLAS/cuFFT/cudart DLLs
# the first time a model runs. On the dev box these are on the system (Machine)
# PATH, so they are NOT copied into the dev tree — but a target machine may have
# only the NVIDIA driver. Bundle them for packaging in the separately-distributed
# "cuda" component (install-only; cuDNN above completes that component). These are
# the large (~hundreds of MB each) libraries that change only with the CUDA
# version, so isolating them keeps routine app updates small.
if(WIN32 AND WITH_CUDA AND CUDAToolkit_FOUND)
    # CUDA 12 keeps these in bin/; CUDA 13 moved them to bin/x64/. Glob both.
    file(GLOB _cuda_rt_dlls
        "${CUDAToolkit_BIN_DIR}/cudart64_*.dll"
        "${CUDAToolkit_BIN_DIR}/cublas64_*.dll"
        "${CUDAToolkit_BIN_DIR}/cublasLt64_*.dll"
        "${CUDAToolkit_BIN_DIR}/cufft64_*.dll"
        "${CUDAToolkit_BIN_DIR}/x64/cudart64_*.dll"
        "${CUDAToolkit_BIN_DIR}/x64/cublas64_*.dll"
        "${CUDAToolkit_BIN_DIR}/x64/cublasLt64_*.dll"
        "${CUDAToolkit_BIN_DIR}/x64/cufft64_*.dll")
    if(_cuda_rt_dlls)
        install(FILES ${_cuda_rt_dlls}
                DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT cuda)
        message(STATUS "CUDA runtime (cuda component): ${_cuda_rt_dlls}")
    else()
        message(WARNING "CUDA runtime DLLs not found in ${CUDAToolkit_BIN_DIR} — "
            "the cuda component will be incomplete; GPU EP would rely on system PATH.")
    endif()
endif()

set(_ort_ep_list "CPU(fallback)")
if(WITH_COREML AND APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    list(PREPEND _ort_ep_list "CoreML")
endif()
if(WITH_CUDA AND NOT APPLE)
    list(PREPEND _ort_ep_list "CUDA")
endif()
list(JOIN _ort_ep_list "+" _ort_ep_str)
message(STATUS "ONNX Runtime ${ORT_VERSION}: ${_ort_root} [EP=${_ort_ep_str}]")

# ── ONNX Runtime GenAI (local LLM engine) ────────────────────────────────────
# Separate from the main ORT download; ships its own bundled libonnxruntime.so
# (different version) which coexists safely due to distinct sonames.
option(WITH_ORTGENAI "Enable ONNX Runtime GenAI local LLM engine" ON)
if(APPLE AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" AND WITH_ORTGENAI)
    # ORT-GenAI dropped Intel macOS prebuilts after v0.11.4; v0.13.x is arm64-only.
    # LocalLlmEngine falls back to Gemini when built without HAVE_ORTGENAI.
    message(STATUS "ONNX Runtime GenAI: no Intel macOS prebuilt — disabling (local LLM falls back to Gemini)")
    set(WITH_ORTGENAI OFF CACHE BOOL "" FORCE)
endif()
if(WITH_ORTGENAI)
    set(ORTGENAI_VERSION "0.13.1")

    if(WIN32)
        if(WITH_CUDA)
            set(_ortgenai_url "https://github.com/microsoft/onnxruntime-genai/releases/download/v${ORTGENAI_VERSION}/onnxruntime-genai-${ORTGENAI_VERSION}-win-x64-cuda.zip")
        else()
            set(_ortgenai_url "https://github.com/microsoft/onnxruntime-genai/releases/download/v${ORTGENAI_VERSION}/onnxruntime-genai-${ORTGENAI_VERSION}-win-x64.zip")
        endif()
    elseif(APPLE)
        # No CUDA on macOS; CoreML acceleration is handled internally by ORT-GenAI.
        set(_ortgenai_url "https://github.com/microsoft/onnxruntime-genai/releases/download/v${ORTGENAI_VERSION}/onnxruntime-genai-${ORTGENAI_VERSION}-osx-arm64.tar.gz")
    else()  # Linux
        if(WITH_CUDA)
            set(_ortgenai_url "https://github.com/microsoft/onnxruntime-genai/releases/download/v${ORTGENAI_VERSION}/onnxruntime-genai-${ORTGENAI_VERSION}-linux-x64-cuda.tar.gz")
        else()
            set(_ortgenai_url "https://github.com/microsoft/onnxruntime-genai/releases/download/v${ORTGENAI_VERSION}/onnxruntime-genai-${ORTGENAI_VERSION}-linux-x64.tar.gz")
        endif()
    endif()

    FetchContent_Declare(onnxruntime_genai URL "${_ortgenai_url}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(onnxruntime_genai)

    if(EXISTS "${onnxruntime_genai_SOURCE_DIR}/include/ort_genai.h")
        set(_ortgenai_root "${onnxruntime_genai_SOURCE_DIR}")
    else()
        file(GLOB _ortgenai_candidates LIST_DIRECTORIES true "${onnxruntime_genai_SOURCE_DIR}/onnxruntime-genai-*")
        if(_ortgenai_candidates)
            list(GET _ortgenai_candidates 0 _ortgenai_root)
        else()
            set(_ortgenai_root "${onnxruntime_genai_SOURCE_DIR}")
        endif()
    endif()

    target_include_directories(PinPointStudio PRIVATE "${_ortgenai_root}/include")
    target_compile_definitions(PinPointStudio PRIVATE HAVE_ORTGENAI)

    if(WIN32)
        target_link_libraries(PinPointStudio PRIVATE "${_ortgenai_root}/lib/onnxruntime-genai.lib")
        file(GLOB _ortgenai_dlls "${_ortgenai_root}/lib/onnxruntime-genai*.dll")
        pp_win_bundle_dll(core ${_ortgenai_dlls})
    elseif(APPLE)
        file(GLOB _ortgenai_dylib "${_ortgenai_root}/lib/libonnxruntime-genai*.dylib")
        list(GET _ortgenai_dylib 0 _ortgenai_dylib_path)
        target_link_libraries(PinPointStudio PRIVATE "${_ortgenai_dylib_path}")
    else()  # Linux
        target_link_libraries(PinPointStudio PRIVATE "${_ortgenai_root}/lib/libonnxruntime-genai.so")
        # Deploy ORT-GenAI's bundled ORT alongside the app (distinct soname from main ORT).
        file(GLOB _ortgenai_rt_libs "${_ortgenai_root}/lib/libonnxruntime*.so*")
        foreach(_lib ${_ortgenai_rt_libs})
            add_custom_command(TARGET PinPointStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lib}" "$<TARGET_FILE_DIR:PinPointStudio>"
            )
        endforeach()
        foreach(_lib ${_ortgenai_rt_libs})
            install(FILES "${_lib}" DESTINATION lib)
        endforeach()
    endif()

    message(STATUS "ONNX Runtime GenAI ${ORTGENAI_VERSION}: ${_ortgenai_root}")
endif()

# ── MoveNet (pose estimation via ONNX Runtime) ────────────────────────────────
# Downloads MoveNet Lightning (~9 MB, 192×192) and Thunder (~30 MB, 256×256)
# SinglePose ONNX models from Hugging Face.
# No additional inference library required — ONNX Runtime (above) handles it.
# Cached in _deps/movenet/ and reused across reconfigures; only lost on a full
# build-directory wipe.
#
# Lightning input:  [1, 192, 192, 3] int32 NHWC, raw pixel values [0, 255]
# Thunder  input:   [1, 256, 256, 3] int32 NHWC, raw pixel values [0, 255]
# Both outputs:     [1, 1, 17, 3]    float32, keypoints as (y, x, score)

set(_movenet_model_dir "${CMAKE_BINARY_DIR}/_deps/movenet")
file(MAKE_DIRECTORY "${_movenet_model_dir}")

# ── Lightning ────────────────────────────────────────────────────────────────
set(_movenet_lightning_file "movenet_singlepose_lightning.onnx")
set(_movenet_lightning_path "${_movenet_model_dir}/${_movenet_lightning_file}")

if(NOT EXISTS "${_movenet_lightning_path}")
    message(STATUS "MoveNet: downloading Lightning model (~9 MB)...")
    file(DOWNLOAD
        "https://huggingface.co/Xenova/movenet-singlepose-lightning/resolve/main/onnx/model.onnx"
        "${_movenet_lightning_path}"
        SHOW_PROGRESS
        TLS_VERIFY ON
        STATUS _movenet_dl_status
    )
    list(GET _movenet_dl_status 0 _movenet_dl_code)
    if(NOT _movenet_dl_code EQUAL 0)
        list(GET _movenet_dl_status 1 _movenet_dl_msg)
        message(WARNING "MoveNet: Lightning download failed (${_movenet_dl_msg}) -- pose estimation disabled")
        file(REMOVE "${_movenet_lightning_path}")
    endif()
endif()

# ── Thunder ──────────────────────────────────────────────────────────────────
set(_movenet_thunder_file "movenet_singlepose_thunder.onnx")
set(_movenet_thunder_path "${_movenet_model_dir}/${_movenet_thunder_file}")

if(NOT EXISTS "${_movenet_thunder_path}")
    message(STATUS "MoveNet: downloading Thunder model (~30 MB)...")
    file(DOWNLOAD
        "https://huggingface.co/Xenova/movenet-singlepose-thunder/resolve/main/onnx/model.onnx"
        "${_movenet_thunder_path}"
        SHOW_PROGRESS
        TLS_VERIFY ON
        STATUS _movenet_dl_status
    )
    list(GET _movenet_dl_status 0 _movenet_dl_code)
    if(NOT _movenet_dl_code EQUAL 0)
        list(GET _movenet_dl_status 1 _movenet_dl_msg)
        message(WARNING "MoveNet: Thunder download failed (${_movenet_dl_msg}) -- Thunder model unavailable")
        file(REMOVE "${_movenet_thunder_path}")
    endif()
endif()

# ── Enable pose estimation if Lightning is available ─────────────────────────
if(EXISTS "${_movenet_lightning_path}")
    message(STATUS "MoveNet Lightning: model ready (${_movenet_lightning_path})")
    target_compile_definitions(PinPointStudio PRIVATE
        HAVE_MOVENET
        MOVENET_LIGHTNING_FILE="${_movenet_lightning_file}"
    )

    if(EXISTS "${_movenet_thunder_path}")
        message(STATUS "MoveNet Thunder:   model ready (${_movenet_thunder_path})")
        target_compile_definitions(PinPointStudio PRIVATE
            HAVE_MOVENET_THUNDER
            MOVENET_THUNDER_FILE="${_movenet_thunder_file}"
        )
    else()
        message(STATUS "MoveNet Thunder:   model unavailable -- selector will show Lightning only")
    endif()

    # Copy model(s) into the platform-appropriate location alongside the executable.
    if(APPLE)
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_movenet_lightning_path}"
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models/${_movenet_lightning_file}"
            COMMENT "Bundling MoveNet Lightning model into app"
        )
        if(EXISTS "${_movenet_thunder_path}")
            add_custom_command(TARGET PinPointStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_movenet_thunder_path}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models/${_movenet_thunder_file}"
                COMMENT "Bundling MoveNet Thunder model into app"
            )
        endif()
    else()
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PinPointStudio>/models"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_movenet_lightning_path}"
                "$<TARGET_FILE_DIR:PinPointStudio>/models/${_movenet_lightning_file}"
            COMMENT "Copying MoveNet Lightning model to output directory"
        )
        install(FILES "${_movenet_lightning_path}"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/models" COMPONENT core)
        if(EXISTS "${_movenet_thunder_path}")
            add_custom_command(TARGET PinPointStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_movenet_thunder_path}"
                    "$<TARGET_FILE_DIR:PinPointStudio>/models/${_movenet_thunder_file}"
                COMMENT "Copying MoveNet Thunder model to output directory"
            )
            install(FILES "${_movenet_thunder_path}"
                    DESTINATION "${CMAKE_INSTALL_BINDIR}/models" COMPONENT core)
        endif()
    endif()
else()
    message(STATUS "MoveNet: Lightning model unavailable -- pose estimation disabled")
endif()

if(WITH_MEDIAPIPE)
# ── MediaPipe Pose (two-model BlazePose pipeline from Qualcomm AI Hub) ────────
# The ONNX models cannot be downloaded directly (Qualcomm distributes them as
# device-specific compiled artefacts, not as plain HTTP files).  cmake exports
# them locally via the qai-hub-models Python package.
#
# cmake will try this automatically if Python3 + qai-hub-models are importable
# in the current environment (including an active venv).  If not, it writes a
# helper script and prints instructions:
#
#   python3 -m venv .venv
#   source .venv/bin/activate          # Windows: .venv\Scripts\activate
#   pip install qai-hub-models torch onnxscript
#   python3 <build>/_deps/mediapipe/export_mediapipe_pose.py \
#           <build>/_deps/mediapipe
#   cmake ..      ← re-run to pick up the exported models
#
# Detector input:  [1, 3, 128, 128] float32 NCHW, RGB, normalised [0,1]
# Detector output: 4 tensors — scores×2 + boxes×2, split by stride level
#   [1, 512, 1], [1, 384, 1] scores; [1, 512, 12], [1, 384, 12] boxes
#   896 total anchors (512 at stride-8, 384 at stride-16)
#
# Landmark input:  [1, 3, 256, 256] float32 NCHW, RGB, normalised [0,1]
# Landmark output: flag [1] + landmarks [1, 31, 4] (x,y,z,vis normalised [0,1])
#   + heatmap [1, 128, 128] (ignored)

set(_mediapipe_model_dir "${CMAKE_BINARY_DIR}/_deps/mediapipe")
file(MAKE_DIRECTORY "${_mediapipe_model_dir}")

set(_mediapipe_detector_file "MediaPipePoseDetector.onnx")
set(_mediapipe_landmark_file "MediaPipePoseLandmarkDetector.onnx")
set(_mediapipe_detector_path "${_mediapipe_model_dir}/${_mediapipe_detector_file}")
set(_mediapipe_landmark_path "${_mediapipe_model_dir}/${_mediapipe_landmark_file}")

# ── Write the export helper so the user can run it manually if cmake can't. ──
set(_mediapipe_export_script "${_mediapipe_model_dir}/export_mediapipe_pose.py")
file(WRITE "${_mediapipe_export_script}" [=[
#!/usr/bin/env python3
"""
Export MediaPipe BlazePose detector + landmark models to ONNX.

Usage:
    python3 export_mediapipe_pose.py <output_dir>

Requirements:
    pip install qai-hub-models torch onnxscript
    (Python >=3.10, <3.14 required by qai-hub-models)

Output shapes (confirmed from qai_hub_models 0.53.x):
  Detector  input : [1, 3, 128, 128]
  Detector  output: 4 tensors — [1,512,12], [1,384,12] boxes,
                                [1,512,1],  [1,384,1]  scores
  Landmark  input : [1, 3, 256, 256]
  Landmark  output: flag [1], landmarks [1,31,4] (x,y,z,vis), heatmap [1,128,128]
"""
import builtins
import sys
import pathlib
import traceback


def main() -> int:
    output_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    det_path = output_dir / "MediaPipePoseDetector.onnx"
    lm_path  = output_dir / "MediaPipePoseLandmarkDetector.onnx"

    if det_path.exists() and lm_path.exists():
        print(f"Models already present in {output_dir}")
        return 0

    # ── Check dependencies ────────────────────────────────────────────────────
    try:
        import torch
    except ImportError:
        print("ERROR: torch not installed.  Run: pip install torch", file=sys.stderr)
        return 1

    try:
        import onnxscript  # noqa: F401  required by newer torch.onnx
    except ImportError:
        print("ERROR: onnxscript not installed.  Run: pip install onnxscript",
              file=sys.stderr)
        return 1

    try:
        from qai_hub_models.models.mediapipe_pose.model import (
            PoseDetector, PoseLandmarkDetector)
    except ImportError:
        print("ERROR: qai_hub_models not installed.", file=sys.stderr)
        print("  Run: pip install qai-hub-models", file=sys.stderr)
        return 1

    # ── Download weights ──────────────────────────────────────────────────────
    # qai-hub-models may interactively ask permission to clone MediaPipePyTorch.
    # Auto-accept by patching builtins.input before the import/load calls.
    _real_input = builtins.input
    builtins.input = lambda *a, **kw: "y"

    print("Loading MediaPipe Pose weights (may clone a repo + download on first run)...")
    try:
        det_model = PoseDetector.from_pretrained()
        lm_model  = PoseLandmarkDetector.from_pretrained()
    except Exception as exc:
        builtins.input = _real_input
        print(f"ERROR: from_pretrained() failed: {exc}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        return 1
    finally:
        builtins.input = _real_input

    # The qai_hub_models wrappers expose the raw PyTorch model as .detector.
    detector = det_model.detector
    landmark = lm_model.detector
    detector.eval()
    landmark.eval()

    # torch.onnx.export with the new exporter (PyTorch >= 2.x) writes an
    # external .data sidecar when the model is large.  We immediately merge
    # it back into a single self-contained file using onnx.save_model so
    # cmake only needs to copy one file per model.
    try:
        import onnx as _onnx
    except ImportError:
        print("ERROR: onnx not installed.  Run: pip install onnx onnxscript",
              file=sys.stderr)
        return 1

    def _export_and_merge(net, dummy_input, out_path):
        torch.onnx.export(
            net, dummy_input, str(out_path),
            opset_version=18,
            input_names=["image"],
            do_constant_folding=True,
        )
        # Merge external data sidecar (if any) into a single self-contained file.
        data_file = out_path.parent / (out_path.name + ".data")
        if data_file.exists():
            model = _onnx.load(str(out_path))
            _onnx.save_model(model, str(out_path), save_as_external_data=False)
            data_file.unlink()
        print(f"  -> {out_path}  ({out_path.stat().st_size // 1024} KB, self-contained)")

    # ── Export detector [1,3,128,128] ─────────────────────────────────────────
    print("Exporting detector (BlazePose person detector, 128x128 input)...")
    try:
        _export_and_merge(detector, torch.zeros(1, 3, 128, 128), det_path)
    except Exception as exc:
        print(f"ERROR: detector export failed: {exc}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        return 1

    # ── Export landmark detector [1,3,256,256] ────────────────────────────────
    print("Exporting landmark detector (BlazePose landmark, 256x256 input)...")
    try:
        _export_and_merge(landmark, torch.zeros(1, 3, 256, 256), lm_path)
    except Exception as exc:
        print(f"ERROR: landmark export failed: {exc}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        return 1

    print("Export complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
]=])

# ── Try the automatic export if the models are not already present ────────────
if(NOT EXISTS "${_mediapipe_detector_path}" OR NOT EXISTS "${_mediapipe_landmark_path}")

    # qai-hub-models requires Python >=3.10, <3.14.
    # Search order: active venv → in-tree venv → specific minor versions (so a
    # compatible version is preferred over an incompatible system python3) →
    # bare python3/python fallback.
    find_program(_mp_python
        NAMES python3.13 python3.12 python3.11 python3.10 python3 python
        HINTS
            "$ENV{VIRTUAL_ENV}/bin"
            "$ENV{VIRTUAL_ENV}/Scripts"
            "${CMAKE_SOURCE_DIR}/.venv/bin"
            "${CMAKE_SOURCE_DIR}/.venv/Scripts"
            "${CMAKE_SOURCE_DIR}/venv/bin"
            "${CMAKE_SOURCE_DIR}/venv/Scripts"
            "${CMAKE_BINARY_DIR}/.venv/bin"
            "${CMAKE_BINARY_DIR}/.venv/Scripts"
            "${CMAKE_BINARY_DIR}/venv/bin"
            "${CMAKE_BINARY_DIR}/venv/Scripts"
        NO_DEFAULT_PATH
    )
    if(NOT _mp_python)
        find_program(_mp_python
            NAMES python3.13 python3.12 python3.11 python3.10 python3 python)
    endif()

    if(_mp_python)
        # Get the version string for diagnostic messages.
        execute_process(
            COMMAND "${_mp_python}" --version
            OUTPUT_VARIABLE _py_ver_stdout  OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE  _py_ver_stderr  ERROR_STRIP_TRAILING_WHITESPACE
        )
        if(_py_ver_stdout)
            set(_py_ver "${_py_ver_stdout}")
        else()
            set(_py_ver "${_py_ver_stderr}")  # Python <3.4 printed to stderr
        endif()

        # Check compatibility: qai-hub-models requires >=3.10, <3.14.
        execute_process(
            COMMAND "${_mp_python}" -c
                "import sys; v=sys.version_info; sys.exit(0 if (3,10)<=v<(3,14) else 1)"
            RESULT_VARIABLE _py_compat
            OUTPUT_QUIET ERROR_QUIET
        )

        if(NOT _py_compat EQUAL 0)
            message(STATUS "MediaPipe: ${_py_ver} at ${_mp_python} is not compatible")
            message(STATUS "  qai-hub-models requires Python >=3.10, <3.14")
            message(STATUS "  Install a compatible Python then create a venv and re-run cmake:")
            message(STATUS "    # Ubuntu/Debian:")
            message(STATUS "    sudo add-apt-repository ppa:deadsnakes/ppa")
            message(STATUS "    sudo apt install python3.12 python3.12-venv")
            message(STATUS "    python3.12 -m venv .venv && source .venv/bin/activate")
            message(STATUS "    pip install qai-hub-models torch onnxscript && cmake ..")
            message(STATUS "    # macOS:  brew install python@3.12")
            message(STATUS "    # Windows: winget install Python.Python.3.12")
        else()
            # Version is compatible — check whether qai_hub_models is importable.
            execute_process(
                COMMAND "${_mp_python}" -c "import qai_hub_models"
                RESULT_VARIABLE _qhm_available
                OUTPUT_QUIET ERROR_QUIET
            )

            if(_qhm_available EQUAL 0)
                message(STATUS
                    "MediaPipe: exporting via qai-hub-models using ${_py_ver}"
                    " (may download ~150 MB on first run)...")
                execute_process(
                    COMMAND "${_mp_python}" "${_mediapipe_export_script}"
                            "${_mediapipe_model_dir}"
                    RESULT_VARIABLE _mp_result
                    OUTPUT_VARIABLE _mp_out
                    ERROR_VARIABLE  _mp_err
                )
                if(_mp_result EQUAL 0)
                    message(STATUS "MediaPipe: export succeeded")
                    if(_mp_out)
                        message(STATUS "${_mp_out}")
                    endif()
                else()
                    message(WARNING
                        "MediaPipe: export failed (exit ${_mp_result}).\n"
                        "${_mp_err}\n"
                        "Fix the error above then run manually:\n"
                        "  ${_mp_python} ${_mediapipe_export_script}"
                        " ${_mediapipe_model_dir}\n"
                        "and re-run cmake."
                    )
                endif()
            else()
                message(STATUS
                    "MediaPipe: qai-hub-models not installed in ${_py_ver}"
                    " — skipping auto-export")
                message(STATUS
                    "  Activate a venv (or install into ${_mp_python}) and re-run cmake:")
                message(STATUS
                    "    pip install qai-hub-models torch onnxscript && cmake ..")
                message(STATUS "  Or create a fresh venv first:")
                message(STATUS
                    "    python3.11 -m venv .venv && source .venv/bin/activate"
                    "  # Windows: .venv\\Scripts\\activate")
                message(STATUS
                    "    pip install qai-hub-models torch onnxscript && cmake ..")
            endif()
        endif()
    else()
        message(STATUS "MediaPipe: no Python found — skipping auto-export")
        message(STATUS "  Install Python 3.12 (or 3.10–3.13) and re-run cmake:")
        message(STATUS "    # Ubuntu/Debian:")
        message(STATUS "    sudo add-apt-repository ppa:deadsnakes/ppa")
        message(STATUS "    sudo apt install python3.12 python3.12-venv")
        message(STATUS "    python3.12 -m venv .venv && source .venv/bin/activate")
        message(STATUS "    pip install qai-hub-models torch onnxscript && cmake ..")
        message(STATUS "    # macOS:  brew install python@3.12 && ...")
        message(STATUS "    # Windows: winget install Python.Python.3.12 && ...")
    endif()

endif() # NOT EXISTS both models

if(EXISTS "${_mediapipe_detector_path}" AND EXISTS "${_mediapipe_landmark_path}")
    message(STATUS "MediaPipe Pose: models ready")
    target_compile_definitions(PinPointStudio PRIVATE
        HAVE_MEDIAPIPE
        MEDIAPIPE_DETECTOR_FILE="${_mediapipe_detector_file}"
        MEDIAPIPE_LANDMARK_FILE="${_mediapipe_landmark_file}"
    )

    if(APPLE)
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mediapipe_detector_path}"
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models/${_mediapipe_detector_file}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mediapipe_landmark_path}"
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models/${_mediapipe_landmark_file}"
            COMMENT "Bundling MediaPipe Pose models into app"
        )
    else()
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mediapipe_detector_path}"
                "$<TARGET_FILE_DIR:PinPointStudio>/models/${_mediapipe_detector_file}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mediapipe_landmark_path}"
                "$<TARGET_FILE_DIR:PinPointStudio>/models/${_mediapipe_landmark_file}"
            COMMENT "Copying MediaPipe Pose models to output directory"
        )
        install(FILES "${_mediapipe_detector_path}" "${_mediapipe_landmark_path}"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/models" COMPONENT core)
    endif()
else()
    message(STATUS "MediaPipe Pose: models unavailable -- MediaPipe option disabled")
endif()

endif() # WITH_MEDIAPIPE

# ── Person segmenter (background suppression before MoveNet) ─────────────────
# Downloads the MediaPipe Selfie Segmentation ONNX model (~480 KB).
# Runs before MoveNet in the Film tab to isolate the subject so background
# people do not corrupt the pose estimate.
#
# Input:  [1, 3, 320, 320] float32 NCHW, ImageNet-normalised
# Output: 7 side outputs; "d0" is the final salient-object mask [0,1]

set(_seg_model_dir  "${CMAKE_BINARY_DIR}/_deps/segmenter")
set(_seg_model_file "u2netp.onnx")
set(_seg_model_path "${_seg_model_dir}/${_seg_model_file}")
file(MAKE_DIRECTORY "${_seg_model_dir}")

if(NOT EXISTS "${_seg_model_path}")
    message(STATUS "Segmenter: downloading u2netp person segmentation model (~4.7 MB)...")
    file(DOWNLOAD
        "https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2netp.onnx"
        "${_seg_model_path}"
        SHOW_PROGRESS TLS_VERIFY ON STATUS _seg_dl_status)
    list(GET _seg_dl_status 0 _seg_dl_code)
    if(NOT _seg_dl_code EQUAL 0)
        list(GET _seg_dl_status 1 _seg_dl_msg)
        message(WARNING "Segmenter: download failed (${_seg_dl_msg}) -- background suppression disabled")
        file(REMOVE "${_seg_model_path}")
    endif()
endif()

if(EXISTS "${_seg_model_path}")
    message(STATUS "Segmenter: model ready (${_seg_model_path})")
    target_compile_definitions(PinPointStudio PRIVATE
        HAVE_SEGMENTER
        SEGMENTER_MODEL_FILE="${_seg_model_file}"
    )
    if(APPLE)
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_seg_model_path}"
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models/${_seg_model_file}"
            COMMENT "Bundling segmenter model into app"
        )
    else()
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_seg_model_path}"
                "$<TARGET_FILE_DIR:PinPointStudio>/models/${_seg_model_file}"
            COMMENT "Copying segmenter model to output directory"
        )
        install(FILES "${_seg_model_path}"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/models" COMPONENT core)
    endif()
    if(EXISTS "${_movenet_lightning_path}")
        target_sources(PinPointStudio PRIVATE
            src/Pose/person_segmenter.h
            src/Pose/person_segmenter.cpp
        )
        message(STATUS "PersonSegmenter: enabled")
    endif()
else()
    message(STATUS "Segmenter: model unavailable -- background suppression disabled")
endif()

# ── ViTPose-B (whole-body ONNX model) ────────────────────────────────────────
# Downloads the JunkyByte/easy_ViTPose whole-body model (~330 MB) at configure
# time, cached in _deps/vitpose/ so reconfigures do not re-download.
#
# Model I/O (confirmed from easy_ViTPose source):
#   Input:  [1, 3, 256, 192] float32 NCHW, RGB, ImageNet-normalised
#   Output: [1, 133, 64, 48] float32 heatmaps — COCO-WholeBody 133-keypoint format
#
# The C++ estimator currently consumes only the first 17 COCO body channels.
# See pose_estimator_vitpose.cpp for the note about 133-keypoint future support.

if(WITH_VITPOSE)
    set(_vitpose_model_dir  "${CMAKE_BINARY_DIR}/_deps/vitpose")
    set(_vitpose_model_file "vitpose-b-wholebody.onnx")
    set(_vitpose_model_path "${_vitpose_model_dir}/${_vitpose_model_file}")
    file(MAKE_DIRECTORY "${_vitpose_model_dir}")

    if(NOT EXISTS "${_vitpose_model_path}")
        message(STATUS "ViTPose: downloading vitpose-b-wholebody model (~330 MB)...")
        file(DOWNLOAD
            "https://huggingface.co/JunkyByte/easy_ViTPose/resolve/main/onnx/wholebody/vitpose-b-wholebody.onnx"
            "${_vitpose_model_path}"
            SHOW_PROGRESS TLS_VERIFY ON STATUS _vtp_dl_status)
        list(GET _vtp_dl_status 0 _vtp_dl_code)
        if(NOT _vtp_dl_code EQUAL 0)
            list(GET _vtp_dl_status 1 _vtp_dl_msg)
            message(WARNING "ViTPose: download failed (${_vtp_dl_msg}) -- ViTPose estimator disabled")
            file(REMOVE "${_vitpose_model_path}")
        endif()
    endif()

    if(EXISTS "${_vitpose_model_path}")
        message(STATUS "ViTPose: model ready (${_vitpose_model_path})")
        target_compile_definitions(PinPointStudio PRIVATE
            HAVE_VITPOSE
            VITPOSE_MODEL_FILE="${_vitpose_model_file}"
        )
        if(APPLE)
            add_custom_command(TARGET PinPointStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_vitpose_model_path}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models/${_vitpose_model_file}"
                COMMENT "Bundling ViTPose model into app"
            )
        else()
            add_custom_command(TARGET PinPointStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_vitpose_model_path}"
                    "$<TARGET_FILE_DIR:PinPointStudio>/models/${_vitpose_model_file}"
                COMMENT "Copying ViTPose model to output directory"
            )
            install(FILES "${_vitpose_model_path}"
                    DESTINATION "${CMAKE_INSTALL_BINDIR}/models" COMPONENT core)
        endif()
    else()
        message(STATUS "ViTPose: model unavailable -- estimator disabled")
    endif()
endif() # WITH_VITPOSE

# ── Whisper model (ggml-base.en.bin) ─────────────────────────────────────────
# Downloaded at configure time, cached in _deps/whisper-models/ so it survives
# reconfigures without re-downloading.  Copied to models/whisper/ next to the
# executable so STTProcessor finds it without any user setup.

set(_whisper_model_dir  "${CMAKE_BINARY_DIR}/_deps/whisper-models")
set(_whisper_model_file "ggml-base.en.bin")
set(_whisper_model_path "${_whisper_model_dir}/${_whisper_model_file}")
file(MAKE_DIRECTORY "${_whisper_model_dir}")

if(NOT EXISTS "${_whisper_model_path}")
    message(STATUS "Whisper: downloading ggml-base.en model (~148 MB)...")
    file(DOWNLOAD
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"
        "${_whisper_model_path}"
        SHOW_PROGRESS
        TLS_VERIFY ON
        STATUS _whisper_dl_status
    )
    list(GET _whisper_dl_status 0 _whisper_dl_code)
    if(NOT _whisper_dl_code EQUAL 0)
        list(GET _whisper_dl_status 1 _whisper_dl_msg)
        message(WARNING "Whisper: model download failed (${_whisper_dl_msg}) — STT will use cloud fallback")
        file(REMOVE "${_whisper_model_path}")
    endif()
endif()

if(EXISTS "${_whisper_model_path}")
    message(STATUS "Whisper: model ready (${_whisper_model_path})")
    if(APPLE)
        # macOS: data must live under Resources/, NOT MacOS/ — a data file in
        # Contents/MacOS/ makes codesign treat it as unsigned nested code and the
        # bundle fails to sign/notarize. Mirrors the other models' Apple branches;
        # the STT resolver checks ../Resources/models/whisper/ (stt_processor.cpp).
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models/whisper"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_whisper_model_path}"
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/models/whisper/${_whisper_model_file}"
            COMMENT "Bundling Whisper model into app (Resources/models/whisper)"
        )
    else()
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PinPointStudio>/models/whisper"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_whisper_model_path}"
                "$<TARGET_FILE_DIR:PinPointStudio>/models/whisper/${_whisper_model_file}"
            COMMENT "Copying Whisper model to output directory"
        )
        install(FILES "${_whisper_model_path}"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/models/whisper" COMPONENT core)
    endif()
else()
    message(STATUS "Whisper: model unavailable — STT will use cloud fallback")
endif()

# ── yt-dlp (bundled binary for YouTube download in Film tab) ──────────────────
# Downloads the correct single-file yt-dlp binary for this platform at cmake
# configure time.  Cached in _deps/ytdlp/ so it survives reconfigures.
# The app locates it via YTDLP_BINARY_NAME next to the executable.

set(_ytdlp_dir "${CMAKE_BINARY_DIR}/_deps/ytdlp")
file(MAKE_DIRECTORY "${_ytdlp_dir}")

if(APPLE)
    set(_ytdlp_name "yt-dlp_macos")
    set(_ytdlp_url  "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos")
elseif(WIN32)
    set(_ytdlp_name "yt-dlp.exe")
    set(_ytdlp_url  "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe")
else()
    set(_ytdlp_name "yt-dlp")
    set(_ytdlp_url  "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp")
endif()
set(_ytdlp_path "${_ytdlp_dir}/${_ytdlp_name}")

if(NOT EXISTS "${_ytdlp_path}")
    message(STATUS "yt-dlp: downloading binary...")
    file(DOWNLOAD "${_ytdlp_url}" "${_ytdlp_path}"
        SHOW_PROGRESS TLS_VERIFY ON STATUS _ytdlp_dl_status)
    list(GET _ytdlp_dl_status 0 _ytdlp_dl_code)
    if(NOT _ytdlp_dl_code EQUAL 0)
        list(GET _ytdlp_dl_status 1 _ytdlp_dl_msg)
        message(WARNING "yt-dlp: download failed (${_ytdlp_dl_msg}) -- Film tab download unavailable")
        file(REMOVE "${_ytdlp_path}")
    elseif(NOT WIN32)
        file(CHMOD "${_ytdlp_path}"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE
                        WORLD_READ WORLD_EXECUTE)
    endif()
endif()

if(EXISTS "${_ytdlp_path}")
    message(STATUS "yt-dlp: ready (${_ytdlp_path})")
    target_compile_definitions(PinPointStudio PRIVATE
        HAVE_YTDLP
        YTDLP_BINARY_NAME="${_ytdlp_name}"
    )
    add_custom_command(TARGET PinPointStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ytdlp_path}" "$<TARGET_FILE_DIR:PinPointStudio>/${_ytdlp_name}"
        COMMENT "Copying yt-dlp to output directory"
    )
    # install(PROGRAMS) sets the executable bit on non-Windows targets.
    install(PROGRAMS "${_ytdlp_path}"
            DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME "${_ytdlp_name}" COMPONENT core)
    if(NOT WIN32)
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND chmod +x "$<TARGET_FILE_DIR:PinPointStudio>/${_ytdlp_name}"
            COMMENT "Setting yt-dlp executable bit"
        )
    endif()
else()
    message(STATUS "yt-dlp: unavailable -- Film tab download disabled")
endif()

# ── espeak-ng ────────────────────────────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ESPEAK QUIET espeak-ng)
endif()

if(NOT ESPEAK_FOUND AND APPLE)
    # pkg-config is often absent on macOS; probe the Homebrew prefix directly.
    execute_process(
        COMMAND brew --prefix espeak-ng
        OUTPUT_VARIABLE _brew_espeak
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _brew_result
    )
    if(_brew_result EQUAL 0 AND EXISTS "${_brew_espeak}/lib/libespeak-ng.dylib")
        set(ESPEAK_FOUND     TRUE)
        set(ESPEAK_INCLUDE_DIRS "${_brew_espeak}/include")
        set(ESPEAK_LIBRARY_DIRS "${_brew_espeak}/lib")
        set(ESPEAK_LIBRARIES    "espeak-ng")
        set(ESPEAK_VERSION      "Homebrew")
    endif()
endif()

if(ESPEAK_FOUND)
    target_include_directories(PinPointStudio PRIVATE ${ESPEAK_INCLUDE_DIRS})
    target_link_libraries(PinPointStudio PRIVATE ${ESPEAK_LIBRARIES})
    target_link_directories(PinPointStudio PRIVATE ${ESPEAK_LIBRARY_DIRS})
    target_compile_definitions(PinPointStudio PRIVATE HAVE_ESPEAK_NG)
    message(STATUS "espeak-ng ${ESPEAK_VERSION}: system")
else()
    # 1.52.0 is the first espeak-ng release with a CMake build system.
    # PATCH_COMMAND changes the espeak-include link from PUBLIC to PRIVATE so
    # its compat/ shim directory does not leak into PinPointStudio's include path and
    # shadow system <wctype.h> on macOS.
    # EXCLUDE_FROM_ALL limits compilation to the library target only.
    FetchContent_Declare(
        espeak_ng
        GIT_REPOSITORY https://github.com/espeak-ng/espeak-ng.git
        GIT_TAG        1.52.0
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
        PATCH_COMMAND
            ${CMAKE_COMMAND}
            "-DFILE1=${CMAKE_BINARY_DIR}/_deps/espeak_ng-src/src/CMakeLists.txt"
            "-DFILE2=${CMAKE_BINARY_DIR}/_deps/espeak_ng-src/src/libespeak-ng/CMakeLists.txt"
            "-DFILE3=${CMAKE_BINARY_DIR}/_deps/espeak_ng-src/CMakeLists.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchEspeakNg.cmake"
    )
    set(ESPEAK_BUILD_MANPAGES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(espeak_ng)
    target_link_libraries(PinPointStudio PRIVATE espeak-ng)
    target_include_directories(PinPointStudio PRIVATE "${espeak_ng_SOURCE_DIR}/src/include")
    target_compile_definitions(PinPointStudio PRIVATE HAVE_ESPEAK_NG)

    # espeak-ng's own `data` target (defined in cmake/data.cmake) compiles the
    # phoneme tables and language dictionaries using espeak-ng-bin as a
    # build-time tool.  Depending on it directly avoids cross-directory
    # custom-command scoping issues.
    add_dependencies(PinPointStudio data)

    if(APPLE)
        # Bundle the compiled data (+ lang/ and voices/ copied at configure time
        # by data.cmake) into the macOS app so espeak_Initialize can find it.
        add_custom_command(TARGET PinPointStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${espeak_ng_BINARY_DIR}/espeak-ng-data"
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/espeak-ng-data"
            COMMENT "Bundling espeak-ng data into app"
        )
    else()
        # On Linux (and Windows) building from source, the compiled-in PKGDATADIR
        # won't match the build tree.  Pass the actual build-dir path so
        # espeak_Initialize can find the phoneme tables at runtime (dev fallback).
        target_compile_definitions(PinPointStudio PRIVATE
            ESPEAK_DATA_PATH="${espeak_ng_BINARY_DIR}")

        if(WIN32)
            # ESPEAK_DATA_PATH points into the build tree, which does not exist on
            # an installed machine. Copy espeak-ng-data next to the executable so
            # the runtime (PhonemeTokenizer) can resolve it relative to the exe,
            # and install it for packaging. The `data` target dependency above
            # guarantees the compiled tables exist before this POST_BUILD copy.
            add_custom_command(TARGET PinPointStudio POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${espeak_ng_BINARY_DIR}/espeak-ng-data"
                    "$<TARGET_FILE_DIR:PinPointStudio>/espeak-ng-data"
                COMMENT "Copying espeak-ng data next to the executable"
            )
            install(DIRECTORY "${espeak_ng_BINARY_DIR}/espeak-ng-data"
                    DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT core)
        endif()
    endif()

    message(STATUS "espeak-ng 1.52.0: built from source")
endif()

# ── Aravis (Industrial Camera Support) ──────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ARAVIS aravis-0.8)
endif()

if(NOT ARAVIS_FOUND)
    if(APPLE)
        # Probe Homebrew directly as a fallback
        execute_process(
            COMMAND brew --prefix aravis
            OUTPUT_VARIABLE _brew_aravis
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _brew_result
        )
        if(_brew_result EQUAL 0 AND EXISTS "${_brew_aravis}/lib/libaravis-0.8.dylib")
            set(ARAVIS_FOUND        TRUE)
            set(ARAVIS_INCLUDE_DIRS "${_brew_aravis}/include/aravis-0.8")
            set(ARAVIS_LIBRARY_DIRS "${_brew_aravis}/lib")
            set(ARAVIS_LIBRARIES    "aravis-0.8")
            set(ARAVIS_VERSION      "Homebrew")
        endif()
    elseif(WIN32)
        # Manual search fallback for Windows
        find_path(ARAVIS_INCLUDE_DIRS NAMES arv.h PATH_SUFFIXES aravis-0.8 HINTS ${ARAVIS_ROOT} $ENV{ARAVIS_ROOT})
        find_library(ARAVIS_LIBRARIES NAMES aravis-0.8 HINTS ${ARAVIS_ROOT} $ENV{ARAVIS_ROOT})
        if(ARAVIS_INCLUDE_DIRS AND ARAVIS_LIBRARIES)
            set(ARAVIS_FOUND TRUE)
            set(ARAVIS_VERSION "Manual")
        endif()
    endif()
endif()

if(ARAVIS_FOUND)
    message(STATUS "Aravis ${ARAVIS_VERSION}: found")
    target_compile_definitions(PinPointStudio PRIVATE HAVE_ARAVIS)
    target_include_directories(PinPointStudio PRIVATE ${ARAVIS_INCLUDE_DIRS})
    target_link_libraries(PinPointStudio PRIVATE ${ARAVIS_LIBRARIES})
    if(ARAVIS_LIBRARY_DIRS)
        target_link_directories(PinPointStudio PRIVATE ${ARAVIS_LIBRARY_DIRS})
    endif()
    
    # Aravis depends on GLib, GObject, and GIO. pkg-config usually handles this,
    # but for manual/brew fallbacks we need to ensure they are available.
    if(PkgConfig_FOUND)
        pkg_check_modules(GLIB REQUIRED glib-2.0 gobject-2.0 gio-2.0)
        target_include_directories(PinPointStudio PRIVATE ${GLIB_INCLUDE_DIRS})
        target_link_libraries(PinPointStudio PRIVATE ${GLIB_LIBRARIES})
    endif()
else()
    message(STATUS "Aravis 0.8: not found (industrial camera support disabled)")
endif()

# ── Spinnaker (High-Speed Camera Support, Windows Only) ──────────────────────
if(WIN32)
    set(SPINNAKER_ROOT "C:/Program Files/Teledyne/Spinnaker")
    find_path(SPINNAKER_INCLUDE_DIRS NAMES Spinnaker.h HINTS ${SPINNAKER_ROOT}/include)
    find_library(SPINNAKER_LIBRARIES NAMES Spinnaker_v140 HINTS ${SPINNAKER_ROOT}/lib64/vs2015)

    if(SPINNAKER_INCLUDE_DIRS AND SPINNAKER_LIBRARIES)
        message(STATUS "Spinnaker: found at ${SPINNAKER_ROOT} (build-time headers + import lib; delay-loaded, not redistributed)")
        target_compile_definitions(PinPointStudio PRIVATE HAVE_SPINNAKER)
        target_include_directories(PinPointStudio PRIVATE ${SPINNAKER_INCLUDE_DIRS})
        target_link_libraries(PinPointStudio PRIVATE ${SPINNAKER_LIBRARIES})

        target_sources(PinPointStudio PRIVATE
            src/Video/VideoInputSpinnaker.h
            src/Video/VideoInputSpinnaker.cpp
            src/Video/spinnaker_runtime.h
            src/Video/spinnaker_runtime.cpp
        )

        # The Spinnaker SDK is proprietary and its EULA forbids redistributing it in
        # an installer, so we ship NONE of its DLLs. Instead the sole imported DLL is
        # delay-loaded: the build links the import lib (Spinnaker_v140.lib) for symbols,
        # but the DLL is only touched on first use. At runtime spinnaker_runtime.cpp
        # discovers a user-installed SDK, preloads Spinnaker_v140.dll (which pulls in
        # its GenICam/runtime siblings from the SDK folder), and gates every Spinnaker
        # entry point on that — so a machine without the SDK simply runs without the
        # high-speed camera backend instead of failing to launch. /DELAYLOAD auto-links
        # delayimp.lib (MSVC). Only Spinnaker_v140.dll is directly imported; its
        # dependencies load transitively.
        target_link_options(PinPointStudio PRIVATE "/DELAYLOAD:Spinnaker_v140.dll")
        target_link_libraries(PinPointStudio PRIVATE delayimp.lib)
    else()
        message(STATUS "Spinnaker: not found (high-speed camera support disabled)")
    endif()
endif()

# ── libudev (Linux V4L2 serial number lookup) ────────────────────────────────
if(UNIX AND NOT APPLE)
    if(PkgConfig_FOUND)
        pkg_check_modules(UDEV libudev)
    endif()
    if(NOT UDEV_FOUND)
        find_path(UDEV_INCLUDE_DIRS NAMES libudev.h)
        find_library(UDEV_LIBRARIES NAMES udev)
        if(UDEV_INCLUDE_DIRS AND UDEV_LIBRARIES)
            set(UDEV_FOUND TRUE)
        endif()
    endif()
    if(UDEV_FOUND)
        message(STATUS "libudev: found (V4L2 camera serial number lookup enabled)")
        target_compile_definitions(PinPointStudio PRIVATE HAVE_LIBUDEV)
        target_include_directories(PinPointStudio PRIVATE ${UDEV_INCLUDE_DIRS})
        target_link_libraries(PinPointStudio PRIVATE ${UDEV_LIBRARIES})
    else()
        message(STATUS "libudev: not found (V4L2 cameras will use device path as identifier)")
    endif()
endif()

if(WIN32)
    # cfgmgr32: device tree traversal to extract USB serial numbers for Qt Multimedia cameras.
    # dxgi:    per-process / per-adapter VRAM query for pp_gpu_metrics (IDXGIAdapter3).
    target_link_libraries(PinPointStudio PRIVATE cfgmgr32 dxgi)
endif()

# ── OpenCV ───────────────────────────────────────────────────────────────────
# macOS: inject Homebrew prefix so CMake can locate OpenCVConfig.cmake
if(APPLE)
    execute_process(
        COMMAND brew --prefix opencv
        OUTPUT_VARIABLE _brew_opencv
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _brew_result
    )
    if(_brew_result EQUAL 0 AND EXISTS "${_brew_opencv}")
        list(APPEND CMAKE_PREFIX_PATH "${_brew_opencv}")
    endif()
endif()

# Windows: probe common install locations; user can override with -DOpenCV_DIR=
if(WIN32 AND NOT OpenCV_DIR)
    file(GLOB _opencv_candidates LIST_DIRECTORIES true "C:/opencv/build" "C:/tools/opencv/build")
    if(_opencv_candidates)
        list(GET _opencv_candidates 0 OpenCV_DIR)
    endif()
endif()

find_package(OpenCV REQUIRED)
if(OpenCV_VERSION VERSION_LESS "3.0")
    message(FATAL_ERROR "OpenCV 3.0 or higher is required (found ${OpenCV_VERSION})")
endif()
message(STATUS "OpenCV ${OpenCV_VERSION}: found")

# Link ONLY the OpenCV modules the code actually uses (core, imgproc, imgcodecs —
# verified: those are the only opencv2/ headers included anywhere in the tree). The
# full ${OpenCV_LIBS} from a modular OpenCV (e.g. Homebrew) lists every module,
# including opencv_viz — which links the entire VTK tree (hundreds of MB of dylibs
# the app never calls). Pulling viz into the link makes macdeployqt bundle VTK,
# bloating the macOS .app/.dmg ~3-4x. Linking just the three modules keeps VTK out
# of the dependency graph entirely. Falls back to opencv_world (monolithic Windows
# builds) or the full set if the modular imported targets aren't present.
if(TARGET opencv_world)
    set(PP_OPENCV_LIBS opencv_world)
elseif(TARGET opencv_core AND TARGET opencv_imgproc AND TARGET opencv_imgcodecs)
    # opencv_videoio is needed by the offline re-analysis MP4 fallback (SwingDiskSource
    # in swing_reanalyzer.cpp) when a swing has no raw sidecar. It pulls FFmpeg backends,
    # NOT VTK (that's opencv_viz, still omitted), so the macOS-bundle bloat concern that
    # drove the modular link does not apply. No-op on monolithic opencv_world.
    set(PP_OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs
        $<TARGET_NAME_IF_EXISTS:opencv_videoio>)
else()
    set(PP_OPENCV_LIBS ${OpenCV_LIBS})
    message(WARNING "OpenCV: modular targets (opencv_core/imgproc/imgcodecs) not found "
                    "— linking the full ${OpenCV_LIBS}; the bundle may pull in VTK and be large.")
endif()
message(STATUS "OpenCV link set: ${PP_OPENCV_LIBS}")

target_compile_definitions(PinPointStudio PRIVATE HAVE_OPENCV)
target_include_directories(PinPointStudio PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(PinPointStudio PRIVATE ${PP_OPENCV_LIBS})

# Windows OpenCV runtime: previously resolved only from the dev PATH (e.g.
# C:\tools\opencv\build\x64\vc16\bin) — nothing was copied or installed. Bundle
# the world DLL (config-aware via the imported target). The app no longer uses
# OpenCV's videoio, so its loose FFmpeg backend DLL is not bundled here (the
# swinglab_run tool, which does use videoio, bundles it under its own target).
if(WIN32)
    set(_opencv_runtime_files "")
    if(TARGET opencv_world)
        list(APPEND _opencv_runtime_files "$<TARGET_FILE:opencv_world>")
    else()
        message(WARNING "OpenCV: 'opencv_world' target not found — modular OpenCV "
            "build? The world DLL will not be bundled; installer may be incomplete.")
    endif()
    if(_opencv_runtime_files)
        pp_win_bundle_dll(core ${_opencv_runtime_files})
        message(STATUS "OpenCV runtime: bundling ${_opencv_runtime_files}")
    else()
        message(WARNING "OpenCV runtime DLLs not located under ${OpenCV_DIR} — "
            "installer may be incomplete (set OpenCV_DIR to the build/ or lib/ dir).")
    endif()
endif()
target_sources(PinPointStudio PRIVATE
    src/Video/video_preprocessor_opencv.h
    src/Video/video_preprocessor_opencv.cpp
    src/Video/video_overlay_pose.h
    src/Video/video_overlay_pose.cpp
    src/Video/frame_throttle.h
    src/Video/frame_throttle.cpp
    src/Pose/ball_detector.h
    src/Pose/ball_detector.cpp
    src/Pose/pose_backend.h
    src/Pose/pose_backend.cpp
    src/Pose/motion_capture_probe.h
    src/Pose/motion_capture_probe.cpp
)
if(EXISTS "${_movenet_lightning_path}")
    target_sources(PinPointStudio PRIVATE
        src/Pose/pose_estimator_movenet.h
        src/Pose/pose_estimator_movenet.cpp
    )
    message(STATUS "MoveNet pose estimator: enabled")
endif()

if(WITH_MEDIAPIPE AND EXISTS "${_mediapipe_detector_path}" AND EXISTS "${_mediapipe_landmark_path}")
    target_sources(PinPointStudio PRIVATE
        src/Pose/pose_estimator_mediapipe.h
        src/Pose/pose_estimator_mediapipe.cpp
    )
    message(STATUS "MediaPipe pose estimator: enabled")
endif()

if(WITH_VITPOSE AND EXISTS "${_vitpose_model_path}")
    target_sources(PinPointStudio PRIVATE
        src/Pose/pose_estimator_vitpose.h
        src/Pose/pose_estimator_vitpose.cpp
    )
    message(STATUS "ViTPose-B pose estimator: enabled")
endif()

# ── FFmpeg (swing export encoder) ────────────────────────────────────────────
# Real link dependency on libavcodec/libavformat/libavutil/libswscale for the
# swing export H.264 encoder.  Distinct from the dlsym-based log suppression in
# pp_debug.cpp, which targets Qt Multimedia's *bundled* libavutil and needs no
# link.  GPL builds of FFmpeg (libx264) are fine: the project is GPLv2+.
# macOS: inject Homebrew prefix so pkg-config can locate the .pc files
if(APPLE)
    execute_process(
        COMMAND brew --prefix ffmpeg
        OUTPUT_VARIABLE _brew_ffmpeg
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _brew_ffmpeg_result
    )
    if(_brew_ffmpeg_result EQUAL 0 AND EXISTS "${_brew_ffmpeg}")
        set(ENV{PKG_CONFIG_PATH} "${_brew_ffmpeg}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
endif()

# Windows: probe common install locations; user can override with -DFFMPEG_DIR=
if(WIN32 AND NOT FFMPEG_DIR)
    file(GLOB _ffmpeg_candidates LIST_DIRECTORIES true "C:/ffmpeg" "C:/tools/ffmpeg")
    if(_ffmpeg_candidates)
        list(GET _ffmpeg_candidates 0 FFMPEG_DIR)
    endif()
endif()
if(WIN32 AND FFMPEG_DIR)
    set(ENV{PKG_CONFIG_PATH} "${FFMPEG_DIR}/lib/pkgconfig;$ENV{PKG_CONFIG_PATH}")
endif()

# Export module sources are always compiled; the FFmpeg-backed encoder is added
# only when the libraries are found.  Without FFmpeg the app still builds and
# runs — makeVideoEncoder() returns nullptr and swing export is disabled.
target_sources(PinPointStudio PRIVATE
    src/Export/video_encoder.h
    src/Export/video_encoder.cpp
    src/Export/swing_paths.h
    src/Export/swing_paths.cpp
    src/Export/frame_decode.h
    src/Export/frame_decode.cpp
    src/Export/swing_exporter.h
    src/Export/swing_exporter.cpp
    src/Export/swing_zip_exporter.h
    src/Export/swing_zip_exporter.cpp
    src/Export/swing_doc.h
    src/Export/swing_doc.cpp
    src/Analysis/swing_analysis.h
    src/Analysis/analysis_stage.h
    src/Analysis/wrist_angles.h
    src/Analysis/metric_extractor.h
    src/Analysis/metric_extractor.cpp
    src/Analysis/imu_vision_fuser.h
    src/Analysis/imu_vision_fuser.cpp
    src/Analysis/phase_segmenter.h
    src/Analysis/phase_segmenter.cpp
    src/Analysis/phase_signals.h
    src/Analysis/phase_signals.cpp
    src/Analysis/shaft_tracker_math.h
    src/Analysis/shaft_tracker_math.cpp
    src/Analysis/shaft_track_assembly.h
    src/Analysis/shaft_track_assembly.cpp
    src/Analysis/clubhead_track.h
    src/Analysis/clubhead_track.cpp
    src/Analysis/pose_smoother.h
    src/Analysis/pose_smoother.cpp
    src/Analysis/head_track.h
    src/Analysis/head_track.cpp
    src/Analysis/foot_metrics.h
    src/Analysis/foot_metrics.cpp
    src/Analysis/ball_position.h
    src/Analysis/ball_position.cpp
    src/Analysis/tempo_metrics.h
    src/Analysis/tempo_metrics.cpp
    src/Analysis/hand_axis.h
    src/Analysis/pose_wrist_angle_source.h
    src/Analysis/pose_wrist_angle_source.cpp
    src/Analysis/shaft_tracker.h
    src/Analysis/shaft_tracker.cpp
    src/Analysis/ball_anchor.h
    src/Analysis/ball_anchor.cpp
    src/Analysis/event_refine.h
    src/Analysis/event_refine.cpp
    src/Analysis/swing_scorer.h
    src/Analysis/swing_scorer.cpp
    src/Analysis/wrist_resemblance.h
    src/Analysis/wrist_resemblance.cpp
    src/Analysis/score_uncertainty.h
    src/Analysis/score_uncertainty.cpp
    src/Analysis/wrist_analyzer.h
    src/Analysis/wrist_analyzer.cpp
    src/Analysis/analysis_profiling.h
    src/Analysis/analysis_profiling.cpp
    src/Analysis/kinematic_series.h
    src/Analysis/kinematic_series.cpp
    src/Analysis/pose_runner.h
    src/Analysis/pose_runner.cpp
    src/Analysis/ball_runner.h
    src/Analysis/ball_runner.cpp
    src/Analysis/ball_baseline_io.h
    src/Analysis/shot_analyzer.h
    src/Analysis/shot_analyzer.cpp
    src/Analysis/swing_reanalyzer.h
    src/Analysis/swing_reanalyzer.cpp
    src/Metrics/metric_type.h
    src/Metrics/metric_descriptor.h
    src/Metrics/metric_provider.h
    src/Metrics/metric_catalogue.h
    src/Metrics/metric_catalogue.cpp
    src/Metrics/metric_resolver.h
    src/Metrics/metric_resolver.cpp
    src/Metrics/metric_catalogue_manifest.cpp
    src/Metrics/metric_providers.h
    src/Metrics/metric_providers.cpp
)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
endif()
if(FFMPEG_FOUND)
    target_compile_definitions(PinPointStudio PRIVATE HAVE_FFMPEG)
    target_link_libraries(PinPointStudio PRIVATE PkgConfig::FFMPEG)
    target_sources(PinPointStudio PRIVATE
        src/Export/ffmpeg_video_encoder.h
        src/Export/ffmpeg_video_encoder.cpp
    )
    if(WIN32)
        # POST_BUILD: copy the linked FFmpeg DLLs (and swresample, a runtime
        # dependency of avcodec) next to the executable, same as ORT above.
        # FFMPEG_LIBDIR is only set for single-module pkg_check_modules calls;
        # FFMPEG_LIBRARY_DIRS is the one populated for multi-module checks.
        list(GET FFMPEG_LIBRARY_DIRS 0 _ffmpeg_libdir)
        file(GLOB _ffmpeg_dlls
            "${_ffmpeg_libdir}/../bin/avcodec-*.dll"
            "${_ffmpeg_libdir}/../bin/avformat-*.dll"
            "${_ffmpeg_libdir}/../bin/avutil-*.dll"
            "${_ffmpeg_libdir}/../bin/swscale-*.dll"
            "${_ffmpeg_libdir}/../bin/swresample-*.dll")
        # NOTE: windeployqt (wired in at the CPack/install-deploy stage) ships Qt's
        # own LGPL FFmpeg DLLs under the same names — those lack libx264 and break
        # swing export. The install-deploy script must re-copy these BtbN GPL DLLs
        # *after* windeployqt runs so they win. See packaging step.
        pp_win_bundle_dll(core ${_ffmpeg_dlls})
    endif()
    message(STATUS "FFmpeg ${FFMPEG_libavcodec_VERSION}: found — swing export enabled")
else()
    message(STATUS "FFmpeg: not found — swing export disabled")
endif()

if(APPLE)
    # The Bluetooth permission plugin is a static library that must be explicitly
    # imported; without this, checkPermission(QBluetoothPermission{}) always
    # returns Denied even when the OS has granted access.
    qt_import_plugins(PinPointStudio INCLUDE Qt6::QDarwinBluetoothPermissionPlugin)

    target_sources(PinPointStudio PRIVATE
        src/Gui/app/macos_permissions.h
        src/Gui/app/macos_permissions.mm
        src/STT/STTBackendApple.h
        src/STT/STTBackendApple.mm
        src/Video/VideoInputApple.h
        src/Video/VideoInputApple.mm
        src/Core/pp_gpu_metrics_mac.mm
    )
    # ARC manages the MTLDevice returned by MTLCreateSystemDefaultDevice().
    set_source_files_properties(src/Core/pp_gpu_metrics_mac.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    add_custom_command(TARGET PinPointStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Info.plist"
        COMMENT "Installing Info.plist into bundle"
    )
    add_custom_command(TARGET PinPointStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/src/Resources/icons/PinPointStudio.icns"
            "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Resources/PinPointStudio.icns"
        COMMENT "Installing PinPointStudio.icns into app bundle"
    )

    # The POST_BUILD copy above only fires when the executable actually
    # relinks.  Qt's bundle finalisation regenerates Contents/Info.plist with
    # its own (wrong) CFBundleIdentifier, and on an incremental build where
    # only Info.plist.in changed — no source relink — that stale, Qt-generated
    # plist survives.  This is the perennial "bundle id / usage strings don't
    # update until a clean build" bug.
    #
    # A custom target with no output is *always* considered out of date, so it
    # runs on every build invocation.  Making it part of ALL and depending on
    # the app target guarantees it executes as the final step, after Qt has
    # finalised the bundle, force-copying the configured plist in last so the
    # bundle's Info.plist can never go stale.  (configure_file() above re-runs
    # CMake automatically when Info.plist.in changes, refreshing the source.)
    add_custom_target(PinPointStudio_force_plist ALL
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:PinPointStudio>/Info.plist"
        COMMENT "Force-installing Info.plist into bundle (runs every build)"
        VERBATIM
    )
    add_dependencies(PinPointStudio_force_plist PinPointStudio)

    # Sign the finished bundle with a stable self-signed identity so macOS TCC
    # retains the camera / microphone / speech / Bluetooth grants across rebuilds
    # (see cmake/DevCodesign.cmake).  This is the LAST thing to touch the bundle:
    # it depends on PinPointStudio_force_plist so the plist copy lands before the
    # signature, and like that target it is always out of date so it re-signs on
    # every build (the plist copy would otherwise invalidate a prior seal).
    set(PINPOINT_DEV_CODESIGN_IDENTITY "PinPoint Dev" CACHE STRING
        "Keychain identity used to sign the dev bundle so TCC permission grants persist across rebuilds")
    add_custom_target(PinPointStudio_codesign ALL
        COMMAND ${CMAKE_COMMAND}
            "-DBUNDLE=$<TARGET_BUNDLE_DIR:PinPointStudio>"
            "-DIDENTITY=${PINPOINT_DEV_CODESIGN_IDENTITY}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DevCodesign.cmake"
        COMMENT "Signing dev bundle with '${PINPOINT_DEV_CODESIGN_IDENTITY}' (TCC-stable identity)"
        VERBATIM
    )
    add_dependencies(PinPointStudio_codesign PinPointStudio_force_plist)

    target_link_libraries(PinPointStudio PRIVATE
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Accelerate"
        "-framework Speech"
        "-framework Metal"        # pp_gpu_metrics_mac.mm — MTLDevice memory query
        "-framework Foundation"
    )
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(PinPointStudio PRIVATE Threads::Threads)

endif()

if(MSVC)
    target_compile_definitions(PinPointStudio PRIVATE _CRT_SECURE_NO_WARNINGS)
    # M_PI and friends: MSVC only exposes them from <cmath>/<math.h> when
    # _USE_MATH_DEFINES is defined *before* that header is first included. The
    # vendored imu_ekf sources (third_party/imu_ekf) rely on M_PI, but by the
    # time their <Eigen.h> compat shim defines it, MSVC's <memory> has already
    # pulled in <cmath> and set its include guard. Define it on the command
    # line so it is in effect before any header is parsed.
    target_compile_definitions(PinPointStudio PRIVATE _USE_MATH_DEFINES)
endif()

install(TARGETS PinPointStudio
    BUNDLE  DESTINATION . COMPONENT core
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT core
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT core
)

# ── Qt deployment (windeployqt) at install time ──────────────────────────────
# The POST_BUILD steps above make the *build* tree runnable from the dev PATH, but
# Qt itself (Qt6 DLLs, platform/imageformats/multimedia/tls plugins, QML modules)
# is never copied. Run windeployqt against the *installed* exe to pull Qt into the
# install tree. Runs as part of the "core" component, after install(TARGETS) above
# so the exe it inspects already exists.
#
# We invoke windeployqt directly via install(CODE) rather than
# qt_generate_deploy_qml_app_script: the latter pre-copies the QML plugin DLLs into
# the tree and then has windeployqt overwrite them, which fails deterministically
# here with "Cannot remove existing file …plugind.dll: being used by another
# process" (the just-written DLL is briefly locked, e.g. by AV real-time scan).
# windeployqt deploying into a clean tree never overwrites, so it is reliable.
if(WIN32)
    get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_prefix}/bin" REQUIRED)
    # Do NOT pass --force. windeployqt reaches some QML plugins via multiple import
    # paths and maps each plugin DLL to read its dependency table. With --force it
    # then removes-and-recopies that same still-mapped file, hitting "Cannot remove
    # existing file …plugind.dll: being used by another process" (it locks its own
    # file). Without --force it simply skips already-current files. The retry is a
    # belt for genuine transient AV locks on freshly written DLLs.
    install(CODE "
        set(_wdq_ok FALSE)
        foreach(_try RANGE 1 4)
            message(STATUS \"Deploying Qt (windeployqt) attempt \${_try} into \${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
            execute_process(
                COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                        --qmldir \"${CMAKE_CURRENT_SOURCE_DIR}\"
                        --compiler-runtime
                        --no-translations
                        \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/PinPointStudio.exe\"
                RESULT_VARIABLE _wdq_rc)
            if(_wdq_rc EQUAL 0)
                set(_wdq_ok TRUE)
                break()
            endif()
            message(STATUS \"  windeployqt attempt \${_try} failed (exit \${_wdq_rc}); retrying after 3s\")
            execute_process(COMMAND \"${CMAKE_COMMAND}\" -E sleep 3)
        endforeach()
        if(NOT _wdq_ok)
            message(FATAL_ERROR \"windeployqt failed after 4 attempts.\")
        endif()
    " COMPONENT core)

    # windeployqt ships Qt Multimedia's own LGPL FFmpeg DLLs under the SAME names
    # as our BtbN GPL build — but Qt's lack libx264, breaking swing export. They
    # were installed earlier (via pp_win_bundle_dll), then clobbered by windeployqt
    # just above. Re-copy the BtbN DLLs LAST so they win. install() runs in
    # declaration order, so this must come *after* the deploy install(CODE).
    if(FFMPEG_FOUND AND _ffmpeg_dlls)
        install(FILES ${_ffmpeg_dlls}
                DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT core)
    endif()
endif()

# ── Linux: install hicolor PNG icons + .desktop file ─────────────────────────
if(UNIX AND NOT APPLE)
    foreach(_sz 16 32 48 64 128 256)
        install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/src/Resources/icons/pinpointstudio_${_sz}.png"
            DESTINATION "share/icons/hicolor/${_sz}x${_sz}/apps"
            RENAME "pinpointstudio.png"
        )
    endforeach()
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/PinPointStudio.desktop"
        DESTINATION "share/applications"
    )
endif()

# ── SwingLab offline tools (docs/implementation/swinglab_impl.md) ─────────────
# swinglab_run replays a recorded swing dir through the PRODUCTION analysis
# pipeline (same sources, same configs) with JSON-injectable tuning params and
# per-frame trace dumps. Built only on request — it shares the app's heavy
# dependency set (OpenCV incl. videoio, ONNX Runtime, ViTPose model).
option(PINPOINT_BUILD_TOOLS "Build the SwingLab offline analysis tools" OFF)
if(PINPOINT_BUILD_TOOLS)
    # Shared offline reconstruction + analysis sources (the disk → SwingWindow →
    # analyzer stack), used by both the SwingLab runner and the parity test. The
    # parity test additionally compiles the exporter to build its fixture.
    set(_pinpoint_offline_sources
        src/Analysis/swing_reanalyzer.cpp
        src/Analysis/shot_analyzer.cpp
        src/Analysis/wrist_analyzer.cpp
        src/Analysis/analysis_profiling.cpp
        src/Analysis/kinematic_series.cpp
        src/Analysis/imu_vision_fuser.cpp
        src/Analysis/phase_segmenter.cpp
        src/Analysis/phase_signals.cpp
        src/Analysis/metric_extractor.cpp
        src/Analysis/swing_scorer.cpp
        src/Analysis/wrist_resemblance.cpp
        src/Analysis/score_uncertainty.cpp
        # Tier-2 wrist assessment (offline known-groups, ShotAnalysisJob::runAssessment)
        src/Analysis/wrist_analysis_adapter.cpp
        src/Analysis/wrist_assessment_engine.cpp
        src/Analysis/assessment_rules.cpp
        src/Analysis/reference_bands.cpp
        src/Analysis/pose_runner.cpp
        src/Analysis/ball_runner.cpp
        src/Analysis/shaft_tracker.cpp
        src/Analysis/ball_anchor.cpp
        src/Analysis/event_refine.cpp
        src/Analysis/shaft_tracker_math.cpp
        src/Analysis/shaft_track_assembly.cpp
        src/Analysis/clubhead_track.cpp
        src/Analysis/pose_smoother.cpp
        src/Analysis/head_track.cpp
        src/Analysis/foot_metrics.cpp
        src/Analysis/ball_position.cpp
        src/Analysis/tempo_metrics.cpp
        src/Analysis/pose_wrist_angle_source.cpp
        src/Metrics/metric_catalogue.cpp
        src/Metrics/metric_resolver.cpp
        src/Metrics/metric_catalogue_manifest.cpp
        src/Metrics/metric_providers.cpp
        src/Export/frame_decode.cpp
        src/Export/swing_doc.cpp
        src/Export/swing_paths.cpp
        src/Pose/pose_estimator_base.cpp
        src/Pose/pose_estimator_vitpose.cpp
        src/Core/pp_debug.cpp
        src/Core/PpMessageLog.cpp
        src/Core/PpStatsLog.cpp
        src/Core/AnalysisProfileLog.cpp
        src/Core/pp_profiler.cpp
        src/Core/pp_os_metrics.cpp
    )

    # Common include/define/link wiring for an offline tool target.
    function(pinpoint_configure_offline_target tgt)
        target_include_directories(${tgt} PRIVATE
            src/Buffer src/Analysis src/Metrics src/Export src/Pose src/Core src/IMU
            "${_ort_root}/include"
            ${OpenCV_INCLUDE_DIRS}
            ${whisper_SOURCE_DIR}/include
            ${whisper_SOURCE_DIR}/ggml/include
        )
        target_compile_definitions(${tgt} PRIVATE
            HAVE_OPENCV HAVE_ONNXRUNTIME
            $<$<BOOL:${_vitpose_model_file}>:HAVE_VITPOSE>
            $<$<BOOL:${_vitpose_model_file}>:VITPOSE_MODEL_FILE="${_vitpose_model_file}">
        )
        # Same platform-guarded EP defines as the app target (~line 951ff);
        # without them the offline pose pass is compile-time pinned to CPU even
        # on the CUDA studio box. The EP cascade try/catches registration at
        # runtime, so a GPU-less machine still degrades safely to CPU.
        if(WITH_CUDA AND NOT APPLE)
            target_compile_definitions(${tgt} PRIVATE WITH_CUDA)
        endif()
        if(WITH_COREML AND APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
            target_compile_definitions(${tgt} PRIVATE WITH_COREML)
        endif()
        target_link_libraries(${tgt} PRIVATE
            Qt6::Core Qt6::Gui Qt6::Multimedia
            pinpoint_buffer whisper
            ${PP_OPENCV_LIBS}   # same core/imgproc/imgcodecs subset — no viz/VTK (see above)
            # The offline tools read recorded MP4s via cv::VideoCapture — pull in
            # opencv_videoio (no-op on monolithic opencv_world via TARGET_NAME_IF_EXISTS).
            $<TARGET_NAME_IF_EXISTS:opencv_videoio>
        )
        if(WIN32)
            target_link_libraries(${tgt} PRIVATE "${_ort_root}/lib/onnxruntime.lib")
        elseif(APPLE)
            target_link_libraries(${tgt} PRIVATE "${_ort_dylib_path}")
        else()
            target_link_libraries(${tgt} PRIVATE "${_ort_root}/lib/libonnxruntime.so")
        endif()
    endfunction()

    add_executable(swinglab_run tools/swinglab/src/swinglab_run.cpp ${_pinpoint_offline_sources})
    pinpoint_configure_offline_target(swinglab_run)
    # The runner resolves the ViTPose model next to its own binary, same as the app.
    message(STATUS "SwingLab tools: swinglab_run enabled")

    # RAM-vs-disk SwingWindow parity test — builds its fixture with the real
    # exporter, so it additionally compiles the Export encoder stack + FFmpeg.
    add_executable(swing_window_parity_test
        src/Analysis/tests/swing_window_parity_test.cpp
        ${_pinpoint_offline_sources}
        src/Export/swing_exporter.cpp
        src/Export/video_encoder.cpp
    )
    pinpoint_configure_offline_target(swing_window_parity_test)
    if(FFMPEG_FOUND)
        target_sources(swing_window_parity_test PRIVATE src/Export/ffmpeg_video_encoder.cpp)
        target_compile_definitions(swing_window_parity_test PRIVATE HAVE_FFMPEG)
        target_link_libraries(swing_window_parity_test PRIVATE PkgConfig::FFMPEG)
    endif()
    add_test(NAME swing_window_parity COMMAND swing_window_parity_test)
    message(STATUS "SwingLab tools: swing_window_parity_test enabled")
endif()

# ── Dependency version header for the About box ──────────────────────────────
# Generated LAST so every find_package / FetchContent version variable above is
# already resolved (the pp_version.h configure at the top runs too early for
# these). Feeds src/Core/app_info.cpp -> the About dialog's library list. Tag-
# pinned deps with no version variable are supplied as literals here (kept in
# lockstep with their FetchContent GIT_TAG above).
set(PP_WHISPER_VERSION    "1.7.2")   # whisper FetchContent GIT_TAG v1.7.2
set(PP_EIGEN_VERSION      "3.4.0")   # eigen  FetchContent GIT_TAG 3.4.0
set(PP_SAMPLERATE_VERSION "0.2.2")   # libsamplerate FetchContent GIT_TAG 0.2.2
configure_file(cmake/pp_deps.h.in "${CMAKE_BINARY_DIR}/generated/pp_deps.h" @ONLY)

# ── Windows installer packaging (CPack + Inno Setup) ─────────────────────────
# Must come last: include(CPack) snapshots the CPACK_* variables and component
# definitions set inside, and all install() rules must already be declared.
include(cmake/PinPointPackaging.cmake)
