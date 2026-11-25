# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(AuroraRuntimeEntryTests LANGUAGES CXX)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(host_entry "${CMAKE_CURRENT_LIST_DIR}/../../desktop/runtime/host")
add_executable(AuroraHostEntryFixture FakeHostEngine.cpp
    "${host_entry}/AuroraHostMain.cpp" "${host_entry}/AuroraHostBootstrap.cpp")
target_compile_features(AuroraHostEntryFixture PRIVATE cxx_std_17)
target_include_directories(AuroraHostEntryFixture PRIVATE "${host_entry}")
target_link_libraries(AuroraHostEntryFixture PRIVATE OpenSSL::Crypto)

enable_testing()
add_test(NAME AuroraHostBootstrap COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/HostBootstrapTest.py" "$<TARGET_FILE:AuroraHostEntryFixture>")
set_tests_properties(AuroraHostBootstrap PROPERTIES TIMEOUT 25)
