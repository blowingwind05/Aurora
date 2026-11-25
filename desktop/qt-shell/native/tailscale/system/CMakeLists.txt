# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraSystemTailscale LANGUAGES CXX)
    include(CTest)
endif()

if(NOT WIN32)
    return()
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)
if(NOT TARGET AuroraNativeTailscale)
    add_subdirectory(.. shared-tailscale)
endif()

add_library(AuroraSystemTailscale STATIC
    SystemTailscaleClient.cpp SystemTailscaleClient.h
    SystemPairingNc.cpp SystemPairingNc.h
    SystemStatus.cpp SystemStatus.h
    WindowsInstallation.cpp WindowsInstallation.h)
set_target_properties(AuroraSystemTailscale PROPERTIES AUTOMOC ON)
target_include_directories(AuroraSystemTailscale PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(AuroraSystemTailscale PUBLIC AuroraNativeTailscale PRIVATE advapi32 shell32)

if(BUILD_TESTING AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_subdirectory(tests)
endif()
