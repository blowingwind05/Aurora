# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraNativeServerInfo LANGUAGES CXX)
    include(CTest)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

if(NOT TARGET aurora_native_discovery)
    add_subdirectory(".." "${CMAKE_CURRENT_BINARY_DIR}/discovery")
endif()

add_library(aurora_native_serverinfo STATIC HttpBody.cpp ServerInfo.cpp ServerInfoClient.cpp ServerInfoClient.h)
set_target_properties(aurora_native_serverinfo PROPERTIES AUTOMOC ON)
target_compile_features(aurora_native_serverinfo PUBLIC cxx_std_17)
target_include_directories(aurora_native_serverinfo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_native_serverinfo PUBLIC aurora_native_discovery Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    add_executable(AuroraServerInfoFixtures ServerInfoFixtures.cpp)
    target_link_libraries(AuroraServerInfoFixtures PRIVATE aurora_native_serverinfo)
    foreach(fixture xml framing http ipv6 lifecycle revalidate embedded)
        add_test(NAME serverinfo-${fixture} COMMAND AuroraServerInfoFixtures ${fixture})
        set_tests_properties(serverinfo-${fixture} PROPERTIES TIMEOUT 12)
    endforeach()
    set_tests_properties(serverinfo-ipv6 PROPERTIES SKIP_RETURN_CODE 77)
endif()
