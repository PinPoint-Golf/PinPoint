cmake_minimum_required(VERSION 3.16)

project(PinPoint VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick SerialPort Bluetooth)

qt_standard_project_setup(REQUIRES 6.10)

qt_add_executable(PinPoint
    main.cpp
    IMU/wt9011dcl_base.h IMU/wt9011dcl_base.cpp
    IMU/wt9011dcl.h IMU/wt9011dcl.cpp
    IMU/wt9011dcl_ble.h IMU/wt9011dcl_ble.cpp
)

target_include_directories(PinPoint PRIVATE IMU)

qt_add_qml_module(PinPoint
    URI PinPoint
    QML_FILES
        Main.qml
)

target_compile_definitions(PinPoint PRIVATE
    QT_APPLICATION_NAME="PinPoint"
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(PinPoint PROPERTIES
#    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.PinPoint
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(PinPoint
    PRIVATE Qt6::Quick Qt6::SerialPort Qt6::Bluetooth
)

install(TARGETS PinPoint
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
