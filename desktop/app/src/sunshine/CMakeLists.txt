# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraSunshineConfig LANGUAGES CXX)
    include(CTest)
endif()

if(NOT TARGET Qt${QT_VERSION_MAJOR}::Core)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
endif()

add_library(aurora_sunshine_config STATIC SunshineConfig.cpp SunshineConfig.h aurora_sunshine_options.qrc)
add_library(Aurora::SunshineConfig ALIAS aurora_sunshine_config)
set_target_properties(aurora_sunshine_config PROPERTIES AUTORCC ON CXX_EXTENSIONS OFF)
target_compile_features(aurora_sunshine_config PUBLIC cxx_std_17)
target_include_directories(aurora_sunshine_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_sunshine_config PUBLIC Qt${QT_VERSION_MAJOR}::Core)
if(MSVC)
    target_compile_options(aurora_sunshine_config PRIVATE /utf-8)
endif()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_executable(aurora_sunshine_config_test tests/Equivalence.cpp)
    target_link_libraries(aurora_sunshine_config_test PRIVATE Aurora::SunshineConfig)
    if(MSVC)
        target_compile_options(aurora_sunshine_config_test PRIVATE /utf-8)
    endif()
    add_test(NAME sunshine_config_equivalence COMMAND aurora_sunshine_config_test
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../SunshineOptions.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-inputs.json")
endif()
