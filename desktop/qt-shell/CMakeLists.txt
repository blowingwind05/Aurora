# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraQtShell VERSION 0.8.0 LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Quick QuickControls2 Network Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Quick QuickControls2 Network Widgets)

add_executable(AuroraQtShell bridge/main.cpp bridge/AuroraMore.cpp bridge/AuroraMore.h)
target_sources(AuroraQtShell PRIVATE
    bridge/aurora-brand.qrc
    native/DesktopTray.cpp native/DesktopTray.h native/TrayMenuPlacement.h
    native/NativeDevicePresence.cpp native/NativeDevicePresence.h
    native/NativeDesktop.cpp native/NativeDesktop.h
    native/NativeInstance.cpp native/NativeInstance.h
    native/preferences/DesktopPreferences.cpp native/preferences/DesktopPreferences.h)
target_compile_definitions(AuroraQtShell PRIVATE AURORA_DESKTOP_VERSION="${PROJECT_VERSION}")
set_target_properties(AuroraQtShell PROPERTIES AUTOMOC ON AUTORCC ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(WIN32)
    enable_language(RC)
    configure_file(bridge/aurora-icon.rc.in aurora-icon.rc @ONLY)
    target_sources(AuroraQtShell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/aurora-icon.rc")
endif()
if(APPLE)
    enable_language(OBJCXX)
    find_library(APPKIT_FRAMEWORK AppKit)
    target_sources(AuroraQtShell PRIVATE bridge/MacTitleBar.mm)
    set_target_properties(AuroraQtShell PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_link_libraries(AuroraQtShell PRIVATE "${APPKIT_FRAMEWORK}")
endif()
if(MSVC)
    target_compile_options(AuroraQtShell PRIVATE /utf-8)
endif()
add_subdirectory(native/platform)
add_subdirectory(native/discovery)
add_subdirectory(native/pairing)
add_subdirectory(native/presence)
add_subdirectory(native/session)
add_subdirectory(native/sunshine)
add_subdirectory(native/sunshine/admin)
add_subdirectory(native/sunshine/host)
add_subdirectory(native/sunshine/lifecycle)
add_subdirectory(native/stream)
add_subdirectory(native/tailscale)
if(WIN32)
    add_subdirectory(native/tailscale/system)
    target_link_libraries(AuroraQtShell PRIVATE AuroraSystemTailscale)
endif()
target_link_libraries(AuroraQtShell PRIVATE Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::QuickControls2
    Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Widgets
    aurora_native_platform aurora_native_discovery aurora_native_pairing aurora_native_presence aurora_native_session
    Aurora::SunshineLifecycle aurora_native_stream AuroraNativeTailscale)
