cmake_minimum_required(VERSION 3.16)

project(PinPoint VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 SerialPort Bluetooth Multimedia Network)

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
    src/STT/whisper_processor.h      src/STT/whisper_processor.cpp
    src/TTS/TTSEngine.h              src/TTS/TTSEngine.cpp
    src/TTS/TTSEngineFactory.h       src/TTS/TTSEngineFactory.cpp
    src/TTS/KokoroTTSEngine.h        src/TTS/KokoroTTSEngine.cpp
    src/TTS/PhonemeTokenizer.h       src/TTS/PhonemeTokenizer.cpp
    src/TTS/TtsWorker.h              src/TTS/TtsWorker.cpp
    src/TTS/ModelDownloader.h        src/TTS/ModelDownloader.cpp
)

target_include_directories(PinPoint PRIVATE src/Gui src/IMU src/Audio src/STT src/TTS)

qt_add_qml_module(PinPoint
    URI PinPoint
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/PinPoint
    QML_FILES
        src/Gui/Main.qml
        src/Gui/AudioPage.qml
        src/Gui/CapturePage.qml
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
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::SerialPort Qt6::Bluetooth Qt6::Multimedia Qt6::Network
)

target_sources(PinPoint PRIVATE
    src/Audio/AudioConverter.h
    src/Audio/AudioConverter.cpp
    src/STT/WhisperBackend.h
    src/STT/WhisperBackendFactory.h
    src/STT/WhisperBackendFactory.cpp
    src/STT/WhisperBackendWhisperCpp.h
    src/STT/WhisperBackendWhisperCpp.cpp
    src/STT/WhisperWorker.h
    src/STT/WhisperWorker.cpp
)

target_link_libraries(PinPoint PRIVATE whisper samplerate)
target_include_directories(PinPoint PRIVATE
    ${whisper_SOURCE_DIR}/include
    ${libsamplerate_SOURCE_DIR}/include
)

# ── ONNX Runtime (auto-fetched prebuilt) ─────────────────────────────────────
set(ORT_VERSION "1.25.1")
if(WIN32)
    set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-${ORT_VERSION}.zip")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-osx-arm64-${ORT_VERSION}.tgz")
    else()
        set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-osx-x86_64-${ORT_VERSION}.tgz")
    endif()
else()
    set(_ort_url "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz")
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
if(WIN32)
    target_link_libraries(PinPoint PRIVATE "${_ort_root}/lib/onnxruntime.lib")
    install(FILES "${_ort_root}/lib/onnxruntime.dll" DESTINATION bin)
elseif(APPLE)
    file(GLOB _ort_dylib "${_ort_root}/lib/libonnxruntime*.dylib")
    list(GET _ort_dylib 0 _ort_dylib_path)
    target_link_libraries(PinPoint PRIVATE "${_ort_dylib_path}")
else()
    target_link_libraries(PinPoint PRIVATE "${_ort_root}/lib/libonnxruntime.so")
endif()
message(STATUS "ONNX Runtime ${ORT_VERSION}: ${_ort_root}")

# ── espeak-ng ────────────────────────────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ESPEAK QUIET espeak-ng)
endif()

if(ESPEAK_FOUND)
    target_include_directories(PinPoint PRIVATE ${ESPEAK_INCLUDE_DIRS})
    target_link_libraries(PinPoint PRIVATE ${ESPEAK_LIBRARIES})
    target_link_directories(PinPoint PRIVATE ${ESPEAK_LIBRARY_DIRS})
    target_compile_definitions(PinPoint PRIVATE HAVE_ESPEAK_NG)
    message(STATUS "espeak-ng ${ESPEAK_VERSION}: system")
else()
    FetchContent_Declare(
        espeak_ng
        GIT_REPOSITORY https://github.com/espeak-ng/espeak-ng.git
        GIT_TAG        1.51.1
        GIT_SHALLOW    TRUE
    )
    set(ESPEAK_NG_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
    set(ESPEAK_NG_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(espeak_ng)
    target_include_directories(PinPoint PRIVATE "${espeak_ng_SOURCE_DIR}/src/include")
    target_compile_definitions(PinPoint PRIVATE HAVE_ESPEAK_NG)
    target_link_libraries(PinPoint PRIVATE espeak-ng)
    message(STATUS "espeak-ng 1.51.1: built from source")
endif()

if(APPLE)
    target_sources(PinPoint PRIVATE src/Gui/macos_permissions.h src/Gui/macos_permissions.mm)
    add_custom_command(TARGET PinPoint POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:PinPoint>/Info.plist"
        COMMENT "Installing Info.plist into bundle"
    )
    target_link_libraries(PinPoint PRIVATE "-framework AVFoundation" "-framework Accelerate")
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
