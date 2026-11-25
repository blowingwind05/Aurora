# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraNativeTailscale LANGUAGES CXX)
    include(CTest)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

add_library(AuroraNativeTailscale STATIC
    EmbeddedStatusClient.cpp EmbeddedStatusClient.h
    Status.cpp Status.h)
set_target_properties(AuroraNativeTailscale PROPERTIES AUTOMOC ON)
target_compile_features(AuroraNativeTailscale PUBLIC cxx_std_17)
target_include_directories(AuroraNativeTailscale PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(AuroraNativeTailscale PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)

if(BUILD_TESTING AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_subdirectory(tests)
endif()
