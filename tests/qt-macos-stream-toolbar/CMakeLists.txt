# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.20)
project(AuroraMacStreamToolbar LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "This fixture requires macOS Cocoa and a logged-in desktop session")
endif()
enable_language(OBJCXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Quick)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Quick QuickControls2 Svg)

set(CMAKE_AUTOMOC ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(desktop "${CMAKE_CURRENT_LIST_DIR}/../../desktop")
set(MOONLIGHT_SOURCE "${desktop}/upstream/moonlight-aurora" CACHE PATH "Prepared AuroraStream engine source")
set(moonlight "${MOONLIGHT_SOURCE}")
set(frameworks "${moonlight}/libs/mac/Frameworks")
find_library(sdl SDL2 PATHS "${frameworks}" NO_DEFAULT_PATH REQUIRED)
find_library(appkit AppKit REQUIRED)
find_library(quartz QuartzCore REQUIRED)
find_library(foundation CoreFoundation REQUIRED)

# Mirror the source layout assembled by Build-Mac-Streamer.
set(engine "${CMAKE_CURRENT_BINARY_DIR}/engine")
foreach(name aurora-session-toolbar.mm aurora-session-toolbar.h aurora-session-toolbar-layout.h
             aurora-session-privacy.h aurora-connection-details.h)
    configure_file("${moonlight}/app/streaming/${name}" "${engine}/streaming/${name}" COPYONLY)
endforeach()
foreach(name AuroraMacTitleBarView.h AuroraMacControlsView.h AuroraMacPrivacyAppearanceView.h
             AuroraPrivacyArtwork.h AuroraDisplayControls.h
    configure_file("${desktop}/runtime/stream/qt-session/${name}" "${engine}/qt-session/${name}" COPYONLY)
endforeach()
configure_file("${desktop}/protocol/bitrate/ClientProtocol.h" "${engine}/backend/aurora-bitrate.h" COPYONLY)

add_executable(AuroraMacStreamToolbar MACOSX_BUNDLE main.mm
    "${engine}/streaming/aurora-session-toolbar.mm"
    "${engine}/qt-session/AuroraMacTitleBarView.h"
    "${engine}/qt-session/AuroraMacControlsView.h"
    "${engine}/qt-session/AuroraMacPrivacyAppearanceView.h")
target_include_directories(AuroraMacStreamToolbar PRIVATE "${engine}" "${frameworks}/SDL2.framework/Versions/A/Headers")
target_link_libraries(AuroraMacStreamToolbar PRIVATE Qt${QT_VERSION_MAJOR}::Quick
    Qt${QT_VERSION_MAJOR}::QuickControls2 Qt${QT_VERSION_MAJOR}::Svg
    "${sdl}" "${appkit}" "${quartz}" "${foundation}")
set_target_properties(AuroraMacStreamToolbar PROPERTIES BUILD_RPATH "${frameworks}")

set(ui "$<TARGET_BUNDLE_CONTENT_DIR:AuroraMacStreamToolbar>/Resources/ui/desktop")
add_custom_target(StageStreamToolbarFixture ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ui}/app" "${ui}/Assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${desktop}/app/qml" "${ui}/app/qml"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${desktop}/Assets/icons" "${ui}/Assets/icons"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${desktop}/Assets/Wallpapers" "${ui}/Assets/Wallpapers"
    COMMAND "${CMAKE_COMMAND}" -E copy "${desktop}/Assets/Aurora.png" "${ui}/Assets/Aurora.png"
    VERBATIM)
add_dependencies(StageStreamToolbarFixture AuroraMacStreamToolbar)

enable_testing()
add_test(NAME MacStreamToolbarCocoa COMMAND $<TARGET_FILE:AuroraMacStreamToolbar>)
set_tests_properties(MacStreamToolbarCocoa PROPERTIES TIMEOUT 20)
