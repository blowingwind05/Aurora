# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraNativeSessionFixtures LANGUAGES CXX)
if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "The native session fixtures require Windows or macOS.")
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Quick Test)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Quick Test)
add_subdirectory(../../stream stream)
add_subdirectory(../../tailscale tailscale)
add_subdirectory(.. session)

add_executable(AuroraSessionFakeChild FakeChild.cpp)
target_compile_features(AuroraSessionFakeChild PRIVATE cxx_std_17)
target_include_directories(AuroraSessionFakeChild PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../upstream/moonlight-qt")
target_link_libraries(AuroraSessionFakeChild PRIVATE Qt${QT_VERSION_MAJOR}::Core)
if(WIN32)
    target_link_libraries(AuroraSessionFakeChild PRIVATE user32 gdi32)
endif()

enable_testing()
if(WIN32)
    add_executable(AuroraSessionFixtures SessionFixtures.cpp)
    set_target_properties(AuroraSessionFixtures PROPERTIES AUTOMOC ON)
    target_link_libraries(AuroraSessionFixtures PRIVATE aurora_native_session
        Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::Test)
    target_compile_definitions(AuroraSessionFixtures PRIVATE
        SESSION_PREVIEW_QML="${CMAKE_CURRENT_SOURCE_DIR}/SessionPreview.qml"
        SESSION_FAKE_CHILD="$<TARGET_FILE:AuroraSessionFakeChild>")
    add_dependencies(AuroraSessionFixtures AuroraSessionFakeChild)

    add_test(NAME AuroraSessionFixtures COMMAND AuroraSessionFixtures)
    set_tests_properties(AuroraSessionFixtures PROPERTIES TIMEOUT 45 ENVIRONMENT
        "QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Fusion;QML_DISABLE_DISK_CACHE=1")
endif()

add_executable(AuroraSessionTailnetFixture ../../tailscale/tests/TailnetFixture.cpp)
target_compile_features(AuroraSessionTailnetFixture PRIVATE cxx_std_17)
target_link_libraries(AuroraSessionTailnetFixture PRIVATE Qt${QT_VERSION_MAJOR}::Core)

add_executable(AuroraSessionSupervisorTests SupervisorTests.cpp)
set_target_properties(AuroraSessionSupervisorTests PROPERTIES AUTOMOC ON)
target_link_libraries(AuroraSessionSupervisorTests PRIVATE aurora_native_session Qt${QT_VERSION_MAJOR}::Test)
target_compile_definitions(AuroraSessionSupervisorTests PRIVATE
    SESSION_FAKE_CHILD="$<TARGET_FILE:AuroraSessionFakeChild>"
    SESSION_TAILNET_FIXTURE="$<TARGET_FILE:AuroraSessionTailnetFixture>")
add_dependencies(AuroraSessionSupervisorTests AuroraSessionFakeChild AuroraSessionTailnetFixture)
add_test(NAME AuroraSessionSupervisor COMMAND AuroraSessionSupervisorTests)
set_tests_properties(AuroraSessionSupervisor PROPERTIES TIMEOUT 40)
if(WIN32)
    set_tests_properties(AuroraSessionSupervisor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
endif()
