# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraAvatarLoader LANGUAGES CXX)
    include(CTest)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Gui Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Gui Network)

add_library(AuroraAvatarLoader STATIC AvatarLoader.cpp AvatarLoader.h)
set_target_properties(AuroraAvatarLoader PROPERTIES AUTOMOC ON)
target_compile_features(AuroraAvatarLoader PUBLIC cxx_std_17)
target_include_directories(AuroraAvatarLoader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(AuroraAvatarLoader PUBLIC
    Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Network)

if(BUILD_TESTING AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
    add_executable(AuroraAvatarLoaderTests AvatarLoaderTests.cpp)
    set_target_properties(AuroraAvatarLoaderTests PROPERTIES AUTOMOC ON)
    target_link_libraries(AuroraAvatarLoaderTests PRIVATE AuroraAvatarLoader Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME AuroraAvatarLoader COMMAND AuroraAvatarLoaderTests)
    set_tests_properties(AuroraAvatarLoader PROPERTIES TIMEOUT 20)
endif()
