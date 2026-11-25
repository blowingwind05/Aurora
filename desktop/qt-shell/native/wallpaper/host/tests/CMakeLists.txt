# SPDX-License-Identifier: GPL-3.0-only
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

add_executable(AuroraHostWallpaperTest HostWallpaperTest.cpp)
set_target_properties(AuroraHostWallpaperTest PROPERTIES AUTOMOC ON)
target_link_libraries(AuroraHostWallpaperTest PRIVATE Aurora::HostWallpaper Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME aurora_host_wallpaper_offline COMMAND AuroraHostWallpaperTest)
set_tests_properties(aurora_host_wallpaper_offline PROPERTIES TIMEOUT 25)
