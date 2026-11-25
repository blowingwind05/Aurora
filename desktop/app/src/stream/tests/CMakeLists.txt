# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraNativeStreamTests LANGUAGES CXX)

add_subdirectory(.. stream)
enable_testing()

add_executable(AuroraFakeMoonlight FakeMoonlight.cpp)
target_compile_features(AuroraFakeMoonlight PRIVATE cxx_std_17)
target_include_directories(AuroraFakeMoonlight PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../../upstream/moonlight-qt")
target_link_libraries(AuroraFakeMoonlight PRIVATE aurora_native_stream)

add_executable(AuroraNativeStreamTests StreamTests.cpp)
target_link_libraries(AuroraNativeStreamTests PRIVATE aurora_native_stream)
add_test(NAME AuroraNativeStreamTests COMMAND AuroraNativeStreamTests $<TARGET_FILE:AuroraFakeMoonlight>)
set_tests_properties(AuroraNativeStreamTests PROPERTIES TIMEOUT 30)
