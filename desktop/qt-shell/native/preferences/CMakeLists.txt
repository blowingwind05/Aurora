# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraPreferenceFixtures LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

add_executable(AuroraPreferenceFixtures DesktopPreferences.cpp PreferenceFixtures.cpp)
target_compile_features(AuroraPreferenceFixtures PRIVATE cxx_std_17)
target_link_libraries(AuroraPreferenceFixtures PRIVATE Qt${QT_VERSION_MAJOR}::Core)

enable_testing()
foreach(fixture fresh migrated coexistence failures arguments session-control)
    add_test(NAME preferences-${fixture} COMMAND AuroraPreferenceFixtures ${fixture})
endforeach()
