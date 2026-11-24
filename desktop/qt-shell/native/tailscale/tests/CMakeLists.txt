# SPDX-License-Identifier: GPL-3.0-only
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

add_executable(AuroraTailnetFixture TailnetFixture.cpp)
target_compile_features(AuroraTailnetFixture PRIVATE cxx_std_17)
target_link_libraries(AuroraTailnetFixture PRIVATE Qt${QT_VERSION_MAJOR}::Core)

add_executable(AuroraTailnetTests StatusTests.cpp)
set_target_properties(AuroraTailnetTests PROPERTIES AUTOMOC ON)
target_link_libraries(AuroraTailnetTests PRIVATE AuroraNativeTailscale Qt${QT_VERSION_MAJOR}::Test)
target_compile_definitions(AuroraTailnetTests PRIVATE
    TAILNET_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_dependencies(AuroraTailnetTests AuroraTailnetFixture)

add_test(NAME AuroraTailnetStatus COMMAND AuroraTailnetTests)
set_tests_properties(AuroraTailnetStatus PROPERTIES TIMEOUT 50)
