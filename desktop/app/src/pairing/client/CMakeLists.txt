# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraNativePairingClient LANGUAGES CXX)
    include(CTest)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

if(NOT TARGET aurora_native_pairing)
    add_subdirectory(".." "${CMAKE_CURRENT_BINARY_DIR}/pairing-wire")
endif()
if(NOT TARGET aurora_native_discovery)
    add_subdirectory("../../discovery" "${CMAKE_CURRENT_BINARY_DIR}/discovery")
endif()
if(NOT TARGET aurora_native_stream)
    add_subdirectory("../../stream" "${CMAKE_CURRENT_BINARY_DIR}/stream")
endif()

add_library(aurora_native_pairing_client STATIC PairingClient.cpp PairingClient.h MoonlightPin.cpp)
set_target_properties(aurora_native_pairing_client PROPERTIES AUTOMOC ON)
target_compile_features(aurora_native_pairing_client PUBLIC cxx_std_17)
target_include_directories(aurora_native_pairing_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_native_pairing_client PUBLIC aurora_native_discovery aurora_native_stream
    PRIVATE aurora_native_pairing Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_executable(PairingFakeMoonlight FakeMoonlight.cpp)
    set_target_properties(PairingFakeMoonlight PROPERTIES OUTPUT_NAME AuroraStream)
    target_link_libraries(PairingFakeMoonlight PRIVATE Qt${QT_VERSION_MAJOR}::Core)
    add_executable(AuroraPairingClientFixtures PairingClientFixtures.cpp)
    target_link_libraries(AuroraPairingClientFixtures PRIVATE aurora_native_pairing_client aurora_native_pairing)
    foreach(fixture embedded direct-stdio rejected trust deadlines cancellation)
        add_test(NAME pairing-client-${fixture} COMMAND AuroraPairingClientFixtures
            ${fixture} $<TARGET_FILE:PairingFakeMoonlight>
            "${CMAKE_CURRENT_SOURCE_DIR}/../../sunshine/admin/tests/tls/pinned-cert.pem")
        set_tests_properties(pairing-client-${fixture} PROPERTIES TIMEOUT 24)
    endforeach()
endif()
