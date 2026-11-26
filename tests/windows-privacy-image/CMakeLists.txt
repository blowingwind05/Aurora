# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.20)
project(AuroraPrivacyImageTests LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This fixture requires Windows WIC and GDI")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(AuroraPrivacyImageTests PrivacyImageTests.cpp)
target_compile_features(AuroraPrivacyImageTests PRIVATE cxx_std_17)
target_include_directories(AuroraPrivacyImageTests PRIVATE ../../desktop/runtime/host)
target_link_libraries(AuroraPrivacyImageTests PRIVATE windowscodecs ole32 shell32 gdi32 user32 advapi32)

enable_testing()
add_test(NAME WindowsPrivacyImage COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/replay.py"
    "$<TARGET_FILE:AuroraPrivacyImageTests>" "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
