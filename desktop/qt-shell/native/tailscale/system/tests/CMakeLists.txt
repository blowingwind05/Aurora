# SPDX-License-Identifier: GPL-3.0-only
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

add_executable(AuroraSystemTailnetFixture CliFixture.cpp)
target_compile_features(AuroraSystemTailnetFixture PRIVATE cxx_std_17)
target_link_libraries(AuroraSystemTailnetFixture PRIVATE Qt${QT_VERSION_MAJOR}::Core)

add_executable(AuroraSystemTailnetTests SystemTests.cpp)
set_target_properties(AuroraSystemTailnetTests PROPERTIES AUTOMOC ON)
target_link_libraries(AuroraSystemTailnetTests PRIVATE AuroraSystemTailscale Qt${QT_VERSION_MAJOR}::Test)
add_dependencies(AuroraSystemTailnetTests AuroraSystemTailnetFixture)

add_test(NAME AuroraSystemTailscaleRead COMMAND AuroraSystemTailnetTests)
set_tests_properties(AuroraSystemTailscaleRead PROPERTIES TIMEOUT 50)
