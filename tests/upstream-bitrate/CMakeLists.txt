# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraUpstreamBitrateTests LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
enable_testing()

add_executable(AuroraUpstreamBitrateTests BitrateTests.cpp)
target_compile_features(AuroraUpstreamBitrateTests PRIVATE cxx_std_20)
target_link_libraries(AuroraUpstreamBitrateTests PRIVATE Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME AuroraUpstreamBitrateTests COMMAND AuroraUpstreamBitrateTests)
