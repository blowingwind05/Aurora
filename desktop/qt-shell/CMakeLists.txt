# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraQtShell LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Quick QuickControls2)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Quick QuickControls2)

add_executable(AuroraQtShell bridge/main.cpp)
set_target_properties(AuroraQtShell PROPERTIES AUTOMOC ON AUTORCC ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(WIN32)
    enable_language(RC)
    configure_file(bridge/aurora-icon.rc.in aurora-icon.rc @ONLY)
    target_sources(AuroraQtShell PRIVATE bridge/aurora-brand.qrc "${CMAKE_CURRENT_BINARY_DIR}/aurora-icon.rc")
endif()
target_link_libraries(AuroraQtShell PRIVATE Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::QuickControls2)