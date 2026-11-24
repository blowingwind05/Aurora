# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraSunshineHost LANGUAGES CXX)
    include(CTest)
endif()

if(NOT TARGET Aurora::SunshineConfig)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}/catalog")
endif()

add_library(aurora_sunshine_host STATIC BundledHost.cpp BundledHost.h HostProfile.cpp NativeHostProcess.cpp)
add_library(Aurora::SunshineHost ALIAS aurora_sunshine_host)
target_compile_features(aurora_sunshine_host PUBLIC cxx_std_17)
set_target_properties(aurora_sunshine_host PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(aurora_sunshine_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_sunshine_host PUBLIC Aurora::SunshineConfig)
if(MSVC)
    target_compile_options(aurora_sunshine_host PRIVATE /utf-8)
endif()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_executable(aurora_sunshine_host_test tests/BundledHostTest.cpp)
    target_link_libraries(aurora_sunshine_host_test PRIVATE Aurora::SunshineHost)
    add_test(NAME sunshine_host_offline COMMAND aurora_sunshine_host_test)
endif()
