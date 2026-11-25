# SPDX-License-Identifier: GPL-3.0-only
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Gui)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Gui)

add_library(aurora_native_session STATIC StreamSurface.cpp StreamSurface.h SessionSupervisor.cpp SessionSupervisor.h
    SessionPrivacy.cpp)
set_target_properties(aurora_native_session PROPERTIES AUTOMOC ON)
target_compile_features(aurora_native_session PUBLIC cxx_std_17)
target_include_directories(aurora_native_session PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_native_session PUBLIC Qt${QT_VERSION_MAJOR}::Gui aurora_native_stream
    AuroraNativeTailscale Aurora::StreamControl)
if(MSVC)
    target_compile_options(aurora_native_session PRIVATE /utf-8)
endif()
if(WIN32)
    target_link_libraries(aurora_native_session PRIVATE user32)
endif()
