# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraSunshineAdmin LANGUAGES CXX)
    include(CTest)
endif()

if(NOT TARGET Qt${QT_VERSION_MAJOR}::Network)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Network)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Network)
endif()

add_library(aurora_sunshine_admin STATIC BundledAdmin.cpp BundledAdmin.h)
add_library(Aurora::SunshineAdmin ALIAS aurora_sunshine_admin)
target_compile_features(aurora_sunshine_admin PUBLIC cxx_std_17)
set_target_properties(aurora_sunshine_admin PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(aurora_sunshine_admin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_sunshine_admin PUBLIC Qt${QT_VERSION_MAJOR}::Network)
if(MSVC)
    target_compile_options(aurora_sunshine_admin PRIVATE /utf-8)
endif()

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_executable(aurora_sunshine_admin_test tests/BundledAdminTest.cpp)
    target_link_libraries(aurora_sunshine_admin_test PRIVATE Aurora::SunshineAdmin)
    add_test(NAME sunshine_admin_https COMMAND aurora_sunshine_admin_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/tls")
    set_tests_properties(sunshine_admin_https PROPERTIES TIMEOUT 25)
endif()
