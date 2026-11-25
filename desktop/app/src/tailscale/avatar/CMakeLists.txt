# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraTailscaleAvatar LANGUAGES CXX)
    include(CTest)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Gui Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Gui Network)

if(NOT TARGET AuroraNativeTailscale)
    add_subdirectory(.. native-tailscale)
endif()

add_library(AuroraTailscaleAvatar STATIC AvatarLoader.cpp AvatarLoader.h)
set_target_properties(AuroraTailscaleAvatar PROPERTIES AUTOMOC ON)
target_include_directories(AuroraTailscaleAvatar PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(AuroraTailscaleAvatar PUBLIC AuroraNativeTailscale
    Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Network)

if(BUILD_TESTING AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
    add_executable(AuroraTailscaleAvatarTests AvatarLoaderTests.cpp)
    set_target_properties(AuroraTailscaleAvatarTests PROPERTIES AUTOMOC ON)
    target_link_libraries(AuroraTailscaleAvatarTests PRIVATE AuroraTailscaleAvatar Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME AuroraTailscaleAvatar COMMAND AuroraTailscaleAvatarTests)
    set_tests_properties(AuroraTailscaleAvatar PROPERTIES TIMEOUT 20)
endif()
