# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraSunshineLifecycle LANGUAGES CXX)
    include(CTest)
endif()

if(NOT DEFINED QT_VERSION_MAJOR)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
endif()
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

if(NOT TARGET Aurora::SunshineHost)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../host" "${CMAKE_CURRENT_BINARY_DIR}/host")
endif()
if(NOT TARGET Aurora::SunshineAdmin)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../admin" "${CMAKE_CURRENT_BINARY_DIR}/admin")
endif()

add_library(aurora_sunshine_lifecycle STATIC LifecycleController.cpp LifecycleController.h
    PortPreflight.cpp PortPreflight.h SettingsSession.cpp SettingsSession.h SunshineSettings.h)
add_library(Aurora::SunshineLifecycle ALIAS aurora_sunshine_lifecycle)
set_target_properties(aurora_sunshine_lifecycle PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_compile_features(aurora_sunshine_lifecycle PUBLIC cxx_std_17)
target_include_directories(aurora_sunshine_lifecycle PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_sunshine_lifecycle PUBLIC Aurora::SunshineHost Aurora::SunshineAdmin Qt${QT_VERSION_MAJOR}::Network)
if(WIN32)
    target_link_libraries(aurora_sunshine_lifecycle PRIVATE iphlpapi)
endif()
if(MSVC)
    target_compile_options(aurora_sunshine_lifecycle PRIVATE /utf-8)
endif()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_executable(aurora_sunshine_lifecycle_test tests/LifecycleTest.cpp tests/LoopbackFixture.h)
    target_link_libraries(aurora_sunshine_lifecycle_test PRIVATE Aurora::SunshineLifecycle)
    add_test(NAME sunshine_lifecycle_offline COMMAND aurora_sunshine_lifecycle_test
        "${CMAKE_CURRENT_SOURCE_DIR}/../admin/tests/tls")
    set_tests_properties(sunshine_lifecycle_offline PROPERTIES TIMEOUT 25)
endif()
