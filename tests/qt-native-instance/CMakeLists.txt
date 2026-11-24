# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraNativeInstanceTests LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Network)

set(native "${CMAKE_CURRENT_SOURCE_DIR}/../../desktop/qt-shell/native")
add_executable(AuroraNativeInstanceTests NativeInstanceTests.cpp
    "${native}/NativeInstance.cpp" "${native}/NativeInstance.h")
set_target_properties(AuroraNativeInstanceTests PROPERTIES AUTOMOC ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(AuroraNativeInstanceTests PRIVATE "${native}")
target_link_libraries(AuroraNativeInstanceTests PRIVATE Qt${QT_VERSION_MAJOR}::Network)
if(WIN32)
    target_link_libraries(AuroraNativeInstanceTests PRIVATE user32)
endif()
enable_testing()
add_test(NAME AuroraNativeInstanceTests COMMAND AuroraNativeInstanceTests)
set_tests_properties(AuroraNativeInstanceTests PROPERTIES TIMEOUT 12)
