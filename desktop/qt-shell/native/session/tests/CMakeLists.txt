# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraNativeSessionFixtures LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "The HWND handshake fixture requires Windows.")
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Quick Test)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Quick Test)
add_subdirectory(.. session)

add_executable(AuroraSessionFakeChild FakeChild.cpp)
target_compile_features(AuroraSessionFakeChild PRIVATE cxx_std_17)
target_link_libraries(AuroraSessionFakeChild PRIVATE Qt${QT_VERSION_MAJOR}::Core user32 gdi32)

add_executable(AuroraSessionFixtures SessionFixtures.cpp)
set_target_properties(AuroraSessionFixtures PROPERTIES AUTOMOC ON)
target_link_libraries(AuroraSessionFixtures PRIVATE aurora_native_session
    Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::Test)
target_compile_definitions(AuroraSessionFixtures PRIVATE
    SESSION_PREVIEW_QML="${CMAKE_CURRENT_SOURCE_DIR}/SessionPreview.qml"
    SESSION_FAKE_CHILD="$<TARGET_FILE:AuroraSessionFakeChild>")
add_dependencies(AuroraSessionFixtures AuroraSessionFakeChild)

enable_testing()
add_test(NAME AuroraSessionFixtures COMMAND AuroraSessionFixtures)
set_tests_properties(AuroraSessionFixtures PROPERTIES TIMEOUT 45 ENVIRONMENT
    "QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Fusion;QML_DISABLE_DISK_CACHE=1")
