# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraNativePresence LANGUAGES CXX)
    include(CTest)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

add_library(aurora_native_presence STATIC
    PresencePacket.cpp PresenceProbe.cpp UdpPresenceTransport.cpp UdpPresenceTransport.h)
set_target_properties(aurora_native_presence PROPERTIES AUTOMOC ON)
target_compile_features(aurora_native_presence PUBLIC cxx_std_17)
target_include_directories(aurora_native_presence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_native_presence PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_executable(AuroraPresenceFixtures PresenceFixtures.cpp)
    target_link_libraries(AuroraPresenceFixtures PRIVATE aurora_native_presence)
    foreach(fixture wire names datagram embedded)
        add_test(NAME presence-${fixture} COMMAND AuroraPresenceFixtures ${fixture})
    endforeach()

    add_executable(AuroraUdpPresenceFixtures UdpPresenceFixtures.cpp)
    target_link_libraries(AuroraUdpPresenceFixtures PRIVATE aurora_native_presence)
    foreach(fixture subnet responder probe lifecycle dual-stack)
        add_test(NAME presence-udp-${fixture} COMMAND AuroraUdpPresenceFixtures ${fixture})
        set_tests_properties(presence-udp-${fixture} PROPERTIES RUN_SERIAL TRUE TIMEOUT 8)
    endforeach()
    set_tests_properties(presence-udp-dual-stack PROPERTIES SKIP_RETURN_CODE 77)
endif()
