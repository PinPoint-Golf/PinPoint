cmake_minimum_required(VERSION 3.16)

project(PinPoint VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 SerialPort Bluetooth Multimedia)

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
    src/IMU/wt9011dcl_base.h src/IMU/wt9011dcl_base.cpp
    src/IMU/wt9011dcl.h src/IMU/wt9011dcl.cpp
    src/IMU/wt9011dcl_ble.h src/IMU/wt9011dcl_ble.cpp
    src/Audio/audio_processor_base.h src/Audio/audio_processor_base.cpp
    src/Audio/audio_input_base.h src/Audio/audio_input_base.cpp
    src/Audio/audio_input.h src/Audio/audio_input.cpp
    src/Audio/audio_output_base.h src/Audio/audio_output_base.cpp
    src/Audio/audio_output.h src/Audio/audio_output.cpp
    src/Audio/audio_stream_saver.h src/Audio/audio_stream_saver.cpp
    src/STT/whisper_processor.h src/STT/whisper_processor.cpp
)

target_include_directories(PinPoint PRIVATE src/Gui src/IMU src/Audio src/STT)

qt_add_qml_module(PinPoint
    URI PinPoint
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/PinPoint
    QML_FILES
        src/Gui/Main.qml
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
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::SerialPort Qt6::Bluetooth Qt6::Multimedia
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
