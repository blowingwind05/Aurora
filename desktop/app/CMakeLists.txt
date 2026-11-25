# SPDX-License-Identifier: GPL-3.0-only
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Quick QuickControls2 Network Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Quick QuickControls2 Network Widgets)

add_executable(Aurora src/main.cpp src/ui/AuroraMore.cpp src/ui/AuroraMore.h)
target_sources(Aurora PRIVATE
    src/ui/aurora-brand.qrc
    src/ui/DesktopBridge.cpp src/ui/DesktopBridge.h
    src/ui/StreamSessionWindow.cpp src/ui/StreamSessionWindow.h
    src/ui/DesktopBridge.cpp src/ui/DesktopBridge.h
    src/ui/StreamSessionWindow.cpp src/ui/StreamSessionWindow.h
    src/desktop/NativeDesktop.cpp src/desktop/NativeDesktop.h
    src/desktop/NativeDesktopSessions.cpp src/desktop/NativeDesktopPairing.cpp src/desktop/NativeDesktopPairingHost.cpp
    src/desktop/NativeDesktopWallpapers.cpp src/desktop/NativeDesktopPower.cpp
    src/desktop/NativeDevicePresence.cpp src/desktop/NativeDevicePresence.h
    src/platform/DesktopTray.cpp src/platform/DesktopTray.h src/platform/TrayMenuPlacement.h
    src/platform/NativeInstance.cpp src/platform/NativeInstance.h
    src/preferences/DesktopPreferences.cpp src/preferences/DesktopPreferences.h)
target_include_directories(Aurora PRIVATE src)
target_compile_definitions(Aurora PRIVATE AURORA_DESKTOP_VERSION="${PROJECT_VERSION}")
set_target_properties(Aurora PROPERTIES AUTOMOC ON AUTORCC ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "Use the MSVC toolchain for Aurora on Windows.")
    endif()
    enable_language(RC)
    configure_file(src/ui/aurora-icon.rc.in aurora-icon.rc @ONLY)
    target_sources(Aurora PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/aurora-icon.rc")
    set_target_properties(Aurora PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(Aurora PRIVATE "/ENTRY:mainCRTStartup" "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/../Aurora.manifest")
endif()
if(APPLE)
    enable_language(OBJCXX)
    find_library(APPKIT_FRAMEWORK AppKit)
    target_sources(Aurora PRIVATE src/platform/MacTitleBar.mm)
    set_target_properties(Aurora PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_link_libraries(Aurora PRIVATE "${APPKIT_FRAMEWORK}")
endif()
if(MSVC)
    target_compile_options(Aurora PRIVATE /utf-8)
endif()
add_subdirectory(src/platform)
add_subdirectory(src/discovery)
add_subdirectory(src/discovery/serverinfo)
add_subdirectory(src/pairing)
add_subdirectory(src/presence)
add_subdirectory(src/sunshine)
add_subdirectory(src/sunshine/admin)
add_subdirectory(src/sunshine/host)
add_subdirectory(src/sunshine/lifecycle)
add_subdirectory(src/stream)
add_subdirectory(src/pairing/client)
add_subdirectory(src/tailscale)
add_subdirectory(src/stream/control)
add_subdirectory(src/session)
add_subdirectory(src/pairing/host)
add_subdirectory(src/tailscale/avatar)
add_subdirectory(src/wallpaper/host)
add_subdirectory(src/wallpaper/client)
if(WIN32)
    add_subdirectory(src/tailscale/system)
    add_subdirectory(src/pairing/systemhost)
    target_link_libraries(Aurora PRIVATE AuroraSystemTailscale Aurora::SystemPairingCoordinator)
endif()
target_link_libraries(Aurora PRIVATE Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::QuickControls2
    Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Widgets
    aurora_native_platform aurora_native_discovery aurora_native_serverinfo aurora_native_pairing_client aurora_native_presence aurora_native_session
    Aurora::SunshineLifecycle Aurora::EmbeddedPairingHost Aurora::HostWallpaper
    aurora_native_stream AuroraNativeTailscale AuroraTailscaleAvatar aurora_native_wallpaper_client)
