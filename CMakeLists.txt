cmake_minimum_required(VERSION 3.16)

project(PinPoint VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 SerialPort Bluetooth Multimedia Network)

qt_standard_project_setup(REQUIRES 6.10)

qt_add_executable(PinPoint
    Gui/main.cpp
    Gui/transcription_controller.h Gui/transcription_controller.cpp
    IMU/wt9011dcl_base.h IMU/wt9011dcl_base.cpp
    IMU/wt9011dcl.h IMU/wt9011dcl.cpp
    IMU/wt9011dcl_ble.h IMU/wt9011dcl_ble.cpp
    Audio/audio_processor_base.h Audio/audio_processor_base.cpp
    Audio/audio_input_base.h Audio/audio_input_base.cpp
    Audio/audio_input.h Audio/audio_input.cpp
    Audio/audio_stream_saver.h Audio/audio_stream_saver.cpp
    STT/whisper_processor.h STT/whisper_processor.cpp
)

target_include_directories(PinPoint PRIVATE Gui IMU Audio STT)

qt_add_qml_module(PinPoint
    URI PinPoint
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/PinPoint
    QML_FILES
        Gui/Main.qml
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

if(APPLE)
    target_sources(PinPoint PRIVATE Gui/macos_permissions.h Gui/macos_permissions.mm)
    add_custom_command(TARGET PinPoint POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            "$<TARGET_BUNDLE_CONTENT_DIR:PinPoint>/Info.plist"
        COMMENT "Installing Info.plist into bundle"
    )
    target_link_libraries(PinPoint PRIVATE "-framework AVFoundation")
endif()

install(TARGETS PinPoint
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
