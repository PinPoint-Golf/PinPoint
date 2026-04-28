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
    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.PinPoint
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(PinPoint
    PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::SerialPort Qt6::Bluetooth Qt6::Multimedia Qt6::Network
)

install(TARGETS PinPoint
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
