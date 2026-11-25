# SPDX-License-Identifier: GPL-3.0-only
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

add_library(aurora_native_platform STATIC HostSleepPrevention.cpp StartupRegistration.cpp)
target_compile_features(aurora_native_platform PUBLIC cxx_std_17)
target_include_directories(aurora_native_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(aurora_native_platform PUBLIC Qt${QT_VERSION_MAJOR}::Core)

if(WIN32)
    target_sources(aurora_native_platform PRIVATE PlatformWindows.cpp)
    target_link_libraries(aurora_native_platform PRIVATE advapi32 powrprof)
elseif(APPLE)
    target_sources(aurora_native_platform PRIVATE PlatformMac.mm)
    set_target_properties(aurora_native_platform PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
    target_link_libraries(aurora_native_platform PRIVATE
        "-framework Foundation" "-framework ServiceManagement" "-framework IOKit" "-framework CoreFoundation")
else()
    target_sources(aurora_native_platform PRIVATE PlatformUnsupported.cpp)
endif()
if(MSVC)
    target_compile_options(aurora_native_platform PRIVATE /utf-8)
endif()
