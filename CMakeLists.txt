cmake_minimum_required(VERSION 3.16)

project(PinPoint VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 SerialPort Bluetooth Multimedia Network WebSockets)

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
        include(CheckLanguage)
        check_language(CUDA)
        if(CMAKE_CUDA_COMPILER)
            enable_language(CUDA)
            set(GGML_CUDA ON CACHE BOOL "" FORCE)
            set(_whisper_gpu_backend "CUDA")
            message(STATUS "Whisper: CUDA GPU acceleration enabled")
        else()
            message(STATUS "Whisper: CPU only (Vulkan and CUDA not found)")
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

qt_add_executable(PinPoint
    src/Gui/main.cpp
    src/Gui/transcription_controller.h src/Gui/transcription_controller.cpp
    src/Gui/tts_controller.h           src/Gui/tts_controller.cpp
    src/IMU/wt9011dcl_base.h src/IMU/wt9011dcl_base.cpp
    src/IMU/wt9011dcl.h      src/IMU/wt9011dcl.cpp
    src/IMU/wt9011dcl_ble.h  src/IMU/wt9011dcl_ble.cpp
    src/Audio/audio_processor_base.h src/Audio/audio_processor_base.cpp
    src/Audio/audio_input_base.h     src/Audio/audio_input_base.cpp
    src/Audio/audio_input.h          src/Audio/audio_input.cpp
    src/Audio/audio_output_base.h    src/Audio/audio_output_base.cpp
    src/Audio/audio_output.h         src/Audio/audio_output.cpp
    src/Audio/audio_stream_saver.h   src/Audio/audio_stream_saver.cpp
    src/Video/video_input_base.h           src/Video/video_input_base.cpp
    src/Video/video_input.h                src/Video/video_input.cpp
    src/Gui/video_controller.h             src/Gui/video_controller.cpp
    src/STT/stt_processor.h          src/STT/stt_processor.cpp
    src/TTS/TTSEngine.h              src/TTS/TTSEngine.cpp
    src/TTS/TTSEngineFactory.h       src/TTS/TTSEngineFactory.cpp
    src/TTS/AzureTTSEngine.h         src/TTS/AzureTTSEngine.cpp
    src/TTS/KokoroTTSEngine.h        src/TTS/KokoroTTSEngine.cpp
    src/TTS/PhonemeTokenizer.h       src/TTS/PhonemeTokenizer.cpp
    src/TTS/TtsWorker.h              src/TTS/TtsWorker.cpp
    src/TTS/ModelDownloader.h        src/TTS/ModelDownloader.cpp
)

target_include_directories(PinPoint PRIVATE src/Gui src/IMU src/Audio src/Video src/STT src/TTS src/Secrets)

qt_add_qml_module(PinPoint
    URI PinPoint
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/PinPoint
    QML_FILES
        src/Gui/Main.qml
        src/Gui/AudioPage.qml
        src/Gui/CapturePage.qml
        src/Gui/VideoPage.qml
)

target_compile_definitions(PinPoint PRIVATE
    QT_APPLICATION_NAME="PinPoint"
)

set_target_properties(PinPoint PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

# Qt's CMake infrastructure ignores MACOSX_BUNDLE_INFO_PLIST in some
# configurations.  Generate the plist at configure time and copy it into
# the bundle with a POST_BUILD step so it always wins.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    COPYONLY
)


target_link_libraries(PinPoint
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::SerialPort Qt6::Bluetooth Qt6::Multimedia Qt6::Network Qt6::WebSockets
)

target_sources(PinPoint PRIVATE
    src/Audio/AudioConverter.h
    src/Audio/AudioConverter.cpp
    src/STT/STTBackend.h
    src/STT/STTBackendFactory.h
    src/STT/STTBackendFactory.cpp
    src/STT/STTBackendWhisperCpp.h
    src/STT/STTBackendWhisperCpp.cpp
    src/STT/STTBackendAssemblyAI.h
    src/STT/STTBackendAssemblyAI.cpp
    src/STT/STTWorker.h
    src/STT/STTWorker.cpp
)

target_sources(PinPoint PRIVATE
    src/Secrets/SecretsManager.h
    src/Secrets/SecretsManager.cpp
)

# ── AssemblyAI cloud STT fallback ────────────────────────────────────────────
# Pass -DASSEMBLYAI_API_KEY=<key> at cmake configure time to seed the key into
# QSettings on first run. After that the key is read from QSettings and this
# flag is no longer needed. The key can also be supplied at any time via the
# ASSEMBLYAI_API_KEY environment variable (takes priority over QSettings).
set(ASSEMBLYAI_API_KEY "" CACHE STRING
    "AssemblyAI API key — written to QSettings on first run, not needed after that")
if(ASSEMBLYAI_API_KEY)
    target_compile_definitions(PinPoint PRIVATE
        ASSEMBLYAI_API_KEY_DEFAULT="${ASSEMBLYAI_API_KEY}")
    message(STATUS "AssemblyAI: API key provided (will be seeded to QSettings on first run)")
else()
    message(STATUS "AssemblyAI: no API key — cloud STT fallback disabled until key is set")
endif()

target_link_libraries(PinPoint PRIVATE whisper samplerate)
target_include_directories(PinPoint PRIVATE
    ${whisper_SOURCE_DIR}/include
    ${whisper_SOURCE_DIR}/ggml/include
    ${libsamplerate_SOURCE_DIR}/include
)

# ── ONNX Runtime (auto-fetched prebuilt) ─────────────────────────────────────
# ORT_GPU=ON downloads the GPU-enabled binary:
#   - Linux/Windows: CUDA execution provider (falls back to CPU at runtime if no GPU)
#   - macOS ARM64:   CoreML is always available in the standard binary (no separate download)
#   - macOS x86_64:  CoreML dylib present but unusable with Kokoro's dynamic shapes — CPU only
option(ORT_GPU "Download GPU-enabled ONNX Runtime build (CUDA on Linux/Windows)" ON)

set(ORT_VERSION "1.25.1")
set(_ort_coreml FALSE)
set(_ort_cuda   FALSE)

if(WIN32)
    if(ORT_GPU)
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-gpu-${ORT_VERSION}.zip")
        set(_ort_cuda TRUE)
    else()
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-${ORT_VERSION}.zip")
    endif()
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-osx-arm64-${ORT_VERSION}.tgz")
        set(_ort_coreml TRUE)  # CoreML is included in every macOS ARM64 prebuilt
    else()
        # ONNX Runtime 1.21+ dropped Intel macOS (x86_64) prebuilts; pin to the last available.
        set(ORT_VERSION "1.20.1")
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-osx-x86_64-${ORT_VERSION}.tgz")
        # CoreML is present in the x86_64 dylib but the Kokoro model has fully
        # dynamic sequence lengths throughout — CoreML on Intel rejects virtually
        # every subgraph ("unbounded dimension"), then hangs or fails to compile
        # what remains. CPU is faster and more correct here.
    endif()
else()
    if(ORT_GPU)
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-gpu-${ORT_VERSION}.tgz")
        set(_ort_cuda TRUE)
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

target_include_directories(PinPoint PRIVATE "${_ort_root}/include")
target_compile_definitions(PinPoint PRIVATE HAVE_ONNXRUNTIME)
if(_ort_coreml)
    target_compile_definitions(PinPoint PRIVATE ORT_COREML)
endif()
if(_ort_cuda)
    target_compile_definitions(PinPoint PRIVATE ORT_CUDA)
endif()
if(WIN32)
    target_link_libraries(PinPoint PRIVATE "${_ort_root}/lib/onnxruntime.lib")
    install(FILES "${_ort_root}/lib/onnxruntime.dll" DESTINATION bin)
    if(_ort_cuda)
        # Provider shared libraries needed at runtime for CUDA
        file(GLOB _ort_provider_dlls "${_ort_root}/lib/onnxruntime_providers*.dll")
        foreach(_dll ${_ort_provider_dlls})
            install(FILES "${_dll}" DESTINATION bin)
        endforeach()
    endif()
elseif(APPLE)
    file(GLOB _ort_dylib "${_ort_root}/lib/libonnxruntime*.dylib")
    list(GET _ort_dylib 0 _ort_dylib_path)
    target_link_libraries(PinPoint PRIVATE "${_ort_dylib_path}")
else()
    target_link_libraries(PinPoint PRIVATE "${_ort_root}/lib/libonnxruntime.so")
endif()
message(STATUS "ONNX Runtime ${ORT_VERSION}: ${_ort_root} [CoreML=${_ort_coreml}, CUDA=${_ort_cuda}]")

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
    target_include_directories(PinPoint PRIVATE ${ESPEAK_INCLUDE_DIRS})
    target_link_libraries(PinPoint PRIVATE ${ESPEAK_LIBRARIES})
    target_link_directories(PinPoint PRIVATE ${ESPEAK_LIBRARY_DIRS})
    target_compile_definitions(PinPoint PRIVATE HAVE_ESPEAK_NG)
    message(STATUS "espeak-ng ${ESPEAK_VERSION}: system")
else()
    # 1.52.0 is the first espeak-ng release with a CMake build system.
    # PATCH_COMMAND changes the espeak-include link from PUBLIC to PRIVATE so
    # its compat/ shim directory does not leak into PinPoint's include path and
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
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchEspeakNg.cmake"
    )
    set(ESPEAK_BUILD_MANPAGES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(espeak_ng)
    target_link_libraries(PinPoint PRIVATE espeak-ng)
    target_include_directories(PinPoint PRIVATE "${espeak_ng_SOURCE_DIR}/src/include")
    target_compile_definitions(PinPoint PRIVATE HAVE_ESPEAK_NG)

    # espeak-ng's own `data` target (defined in cmake/data.cmake) compiles the
    # phoneme tables and language dictionaries using espeak-ng-bin as a
    # build-time tool.  Depending on it directly avoids cross-directory
    # custom-command scoping issues.
    add_dependencies(PinPoint data)

    if(APPLE)
        # Bundle the compiled data (+ lang/ and voices/ copied at configure time
        # by data.cmake) into the macOS app so espeak_Initialize can find it.
        add_custom_command(TARGET PinPoint POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${espeak_ng_BINARY_DIR}/espeak-ng-data"
                "$<TARGET_BUNDLE_CONTENT_DIR:PinPoint>/Resources/espeak-ng-data"
            COMMENT "Bundling espeak-ng data into app"
        )
    else()
        # On Linux (and Windows) building from source, the compiled-in PKGDATADIR
        # won't match the build tree.  Pass the actual build-dir path so
        # espeak_Initialize can find the phoneme tables at runtime.
        target_compile_definitions(PinPoint PRIVATE
            ESPEAK_DATA_PATH="${espeak_ng_BINARY_DIR}")
    endif()

    message(STATUS "espeak-ng 1.52.0: built from source")
endif()

if(APPLE)
    target_sources(PinPoint PRIVATE
        src/Gui/macos_permissions.h
        src/Gui/macos_permissions.mm
        src/STT/STTBackendApple.h
        src/STT/STTBackendApple.mm
        src/Video/VideoInputApple.h
        src/Video/VideoInputApple.mm
    )
    add_custom_command(TARGET PinPoint POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:PinPoint>/Info.plist"
        COMMENT "Installing Info.plist into bundle"
    )
    target_link_libraries(PinPoint PRIVATE
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Accelerate"
        "-framework Speech"
    )
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(PinPoint PRIVATE Threads::Threads)
endif()

if(MSVC)
    target_compile_definitions(PinPoint PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

install(TARGETS PinPoint
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
