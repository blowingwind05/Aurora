# SPDX-License-Identifier: GPL-3.0-only
add_executable(WallpaperFakeMoonlight FakeMoonlight.cpp)
set_target_properties(WallpaperFakeMoonlight PROPERTIES OUTPUT_NAME AuroraStream)
target_compile_features(WallpaperFakeMoonlight PRIVATE cxx_std_17)
target_link_libraries(WallpaperFakeMoonlight PRIVATE Qt${QT_VERSION_MAJOR}::Core)

add_executable(AuroraWallpaperClientFixtures WallpaperClientFixtures.cpp)
target_link_libraries(AuroraWallpaperClientFixtures PRIVATE aurora_native_wallpaper_client Qt${QT_VERSION_MAJOR}::Gui)
foreach(fixture cache protocol storage authorization timeout)
    add_test(NAME wallpaper-client-${fixture} COMMAND AuroraWallpaperClientFixtures
        ${fixture} $<TARGET_FILE:WallpaperFakeMoonlight>)
    set_tests_properties(wallpaper-client-${fixture} PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
endforeach()
