# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraQtMoreTests LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Quick QuickControls2 Network Test)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Quick QuickControls2 Network Test)

set(app "../../desktop/app")
add_executable(AuroraQtMoreTests MoreTests.cpp "${app}/src/ui/AuroraMore.cpp" "${app}/src/ui/AuroraMore.h")
set_target_properties(AuroraQtMoreTests PROPERTIES AUTOMOC ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(AuroraQtMoreTests PRIVATE "${app}/src/ui")
target_compile_definitions(AuroraQtMoreTests PRIVATE MORE_MAIN_QML="${CMAKE_CURRENT_SOURCE_DIR}/${app}/qml/Main.qml")
if(MSVC)
    target_compile_options(AuroraQtMoreTests PRIVATE /utf-8)
endif()
target_link_libraries(AuroraQtMoreTests PRIVATE Qt${QT_VERSION_MAJOR}::Quick Qt${QT_VERSION_MAJOR}::QuickControls2
    Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)

enable_testing()
add_test(NAME AuroraQtMoreTests COMMAND AuroraQtMoreTests)
set_tests_properties(AuroraQtMoreTests PROPERTIES TIMEOUT 25)
