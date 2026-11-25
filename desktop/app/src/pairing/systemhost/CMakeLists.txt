# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraSystemPairingHost LANGUAGES CXX)
    include(CTest)
endif()

if(NOT WIN32)
    return()
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

if(NOT TARGET aurora_native_pairing)
    add_subdirectory(.. pairing-wire)
endif()
if(NOT TARGET AuroraSystemTailscale)
    add_subdirectory(../../adaptors/tailscale/system system-tailscale)
endif()

add_library(AuroraSystemPairingHost STATIC
    SystemPairingHost.cpp SystemPairingHost.h
    WindowsTransport.cpp WindowsTransport.h)
add_library(Aurora::SystemPairingHost ALIAS AuroraSystemPairingHost)
set_target_properties(AuroraSystemPairingHost PROPERTIES AUTOMOC ON)
target_include_directories(AuroraSystemPairingHost PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(AuroraSystemPairingHost PUBLIC AuroraSystemTailscale
    PRIVATE aurora_native_pairing iphlpapi ws2_32)

if(NOT TARGET Aurora::SunshineLifecycle)
    add_subdirectory(../../adaptors/sunshine/lifecycle sunshine-lifecycle)
endif()
add_library(aurora_system_pairing_coordinator STATIC SystemPairingCoordinator.cpp SystemPairingCoordinator.h)
add_library(Aurora::SystemPairingCoordinator ALIAS aurora_system_pairing_coordinator)
set_target_properties(aurora_system_pairing_coordinator PROPERTIES AUTOMOC ON)
target_include_directories(aurora_system_pairing_coordinator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../host")
target_link_libraries(aurora_system_pairing_coordinator
    PUBLIC Aurora::SystemPairingHost Aurora::SunshineLifecycle aurora_native_pairing)
if(MSVC)
    target_compile_options(aurora_system_pairing_coordinator PRIVATE /utf-8)
endif()

if(BUILD_TESTING AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(AuroraSystemPairingHostFixture tests/FakeCli.cpp)
    target_compile_features(AuroraSystemPairingHostFixture PRIVATE cxx_std_17)
    add_executable(AuroraSystemPairingHostTests tests/HostTests.cpp)
    target_link_libraries(AuroraSystemPairingHostTests PRIVATE AuroraSystemPairingHost aurora_native_pairing)
    add_dependencies(AuroraSystemPairingHostTests AuroraSystemPairingHostFixture)
    foreach(scenario IN ITEMS transport startup peers cancellation)
        add_test(NAME system-pair-host-${scenario} COMMAND AuroraSystemPairingHostTests
            ${scenario} $<TARGET_FILE:AuroraSystemPairingHostFixture>)
        set_tests_properties(system-pair-host-${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(AuroraSystemPairingCoordinatorTests tests/CoordinatorTests.cpp)
    target_link_libraries(AuroraSystemPairingCoordinatorTests PRIVATE Aurora::SystemPairingCoordinator)
    add_dependencies(AuroraSystemPairingCoordinatorTests AuroraSystemPairingHostFixture)
    foreach(scenario IN ITEMS approved denied revoked expired)
        add_test(NAME system-pair-coordinator-${scenario} COMMAND AuroraSystemPairingCoordinatorTests
            ${scenario} $<TARGET_FILE:AuroraSystemPairingHostFixture>
            "${CMAKE_CURRENT_SOURCE_DIR}/../../adaptors/sunshine/admin/tests/tls")
        set_tests_properties(system-pair-coordinator-${scenario} PROPERTIES TIMEOUT 45)
    endforeach()
endif()
