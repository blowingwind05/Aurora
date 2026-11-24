# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraNativeLauncher LANGUAGES CXX)

add_subdirectory(../qt-shell qt-shell)
set_target_properties(AuroraQtShell PROPERTIES
    OUTPUT_NAME Aurora
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "Use the Windows x64 MSVC toolchain for the native Aurora launcher.")
    endif()
    set_target_properties(AuroraQtShell PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(AuroraQtShell PRIVATE
        "/ENTRY:mainCRTStartup"
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/../Aurora.manifest")
endif()
