# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(AuroraStreamControl LANGUAGES CXX)
    include(CTest)
endif()

if(NOT DEFINED QT_VERSION_MAJOR)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
endif()
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

if(NOT TARGET aurora_native_stream)
    add_subdirectory(".." "${CMAKE_CURRENT_BINARY_DIR}/stream")
endif()
if(NOT TARGET aurora_native_serverinfo)
    add_subdirectory("../../../discovery/serverinfo" "${CMAKE_CURRENT_BINARY_DIR}/serverinfo")
endif()
if(NOT TARGET AuroraNativeTailscale)
    add_subdirectory("../../tailscale" "${CMAKE_CURRENT_BINARY_DIR}/tailscale")
endif()

add_library(aurora_native_stream_control STATIC ControlAdapter.cpp ControlAdapter.h)
add_library(Aurora::StreamControl ALIAS aurora_native_stream_control)
set_target_properties(aurora_native_stream_control PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_compile_features(aurora_native_stream_control PUBLIC cxx_std_17)
target_include_directories(aurora_native_stream_control PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../protocol/bitrate")
target_link_libraries(aurora_native_stream_control PUBLIC aurora_native_stream aurora_native_serverinfo AuroraNativeTailscale)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
    add_executable(aurora_control_fake_worker tests/FakeWorker.cpp)
    set_target_properties(aurora_control_fake_worker PROPERTIES OUTPUT_NAME AuroraStream)
    target_compile_features(aurora_control_fake_worker PRIVATE cxx_std_17)
    target_link_libraries(aurora_control_fake_worker PRIVATE Qt${QT_VERSION_MAJOR}::Core)

    add_executable(aurora_control_fake_tailnet ../../tailscale/tests/TailnetFixture.cpp)
    target_compile_features(aurora_control_fake_tailnet PRIVATE cxx_std_17)
    target_link_libraries(aurora_control_fake_tailnet PRIVATE Qt${QT_VERSION_MAJOR}::Core)

    add_executable(aurora_stream_control_test tests/ControlTest.cpp)
    set_target_properties(aurora_stream_control_test PROPERTIES AUTOMOC ON)
    target_link_libraries(aurora_stream_control_test PRIVATE Aurora::StreamControl Qt${QT_VERSION_MAJOR}::Test)
    target_compile_definitions(aurora_stream_control_test PRIVATE
        CONTROL_WORKER="$<TARGET_FILE:aurora_control_fake_worker>"
        CONTROL_TAILNET="$<TARGET_FILE:aurora_control_fake_tailnet>")
    add_dependencies(aurora_stream_control_test aurora_control_fake_worker aurora_control_fake_tailnet)
    add_test(NAME stream-control-offline COMMAND aurora_stream_control_test)
    set_tests_properties(stream-control-offline PROPERTIES TIMEOUT 45)
endif()
