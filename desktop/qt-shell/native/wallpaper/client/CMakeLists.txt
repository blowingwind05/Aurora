# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraNativeWallpaperClient LANGUAGES CXX)
    include(CTest)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Network)

if(NOT TARGET aurora_native_discovery)
    add_subdirectory("../../discovery" "${CMAKE_CURRENT_BINARY_DIR}/discovery")
endif()
if(NOT TARGET aurora_native_stream)
    add_subdirectory("../../stream" "${CMAKE_CURRENT_BINARY_DIR}/stream")
endif()

add_library(aurora_native_wallpaper_client STATIC WallpaperClient.cpp WallpaperClient.h)
set_target_properties(aurora_native_wallpaper_client PROPERTIES AUTOMOC ON)
target_compile_features(aurora_native_wallpaper_client PUBLIC cxx_std_17)
target_include_directories(aurora_native_wallpaper_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_native_wallpaper_client PUBLIC aurora_native_discovery aurora_native_stream
    PRIVATE Qt${QT_VERSION_MAJOR}::Gui)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_subdirectory(tests)
endif()
