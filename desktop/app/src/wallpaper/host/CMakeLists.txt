# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraHostWallpaper LANGUAGES CXX)
    include(CTest)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Gui)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Gui)

add_library(aurora_host_wallpaper STATIC HostWallpaper.cpp HostWallpaper.h)
add_library(Aurora::HostWallpaper ALIAS aurora_host_wallpaper)
set_target_properties(aurora_host_wallpaper PROPERTIES AUTOMOC ON)
target_compile_features(aurora_host_wallpaper PUBLIC cxx_std_17)
target_include_directories(aurora_host_wallpaper PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_host_wallpaper PUBLIC Qt${QT_VERSION_MAJOR}::Gui)

if(WIN32)
    target_sources(aurora_host_wallpaper PRIVATE WallpaperWindows.cpp)
    target_compile_definitions(aurora_host_wallpaper PRIVATE NOMINMAX)
    target_link_libraries(aurora_host_wallpaper PRIVATE ole32 uuid user32)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(aurora_host_wallpaper PRIVATE WallpaperMac.mm)
    set_target_properties(aurora_host_wallpaper PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
    target_link_libraries(aurora_host_wallpaper PRIVATE
        "-framework AppKit" "-framework ImageIO" "-framework CoreFoundation" "-framework CoreGraphics")
else()
    target_sources(aurora_host_wallpaper PRIVATE WallpaperUnavailable.cpp)
endif()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_subdirectory(tests)
endif()
