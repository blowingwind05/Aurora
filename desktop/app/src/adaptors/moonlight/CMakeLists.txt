# SPDX-License-Identifier: GPL-3.0-only
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

add_library(aurora_native_stream STATIC MoonlightCommand.cpp MoonlightProcess.cpp MoonlightPin.cpp MoonlightPin.h)
target_compile_features(aurora_native_stream PUBLIC cxx_std_17)
target_include_directories(aurora_native_stream PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_native_stream PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)
