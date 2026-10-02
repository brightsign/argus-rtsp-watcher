cmake_minimum_required(VERSION 3.14)
project(ArgusEngineTests)

# Unit tests always build for the native (host) architecture, independent of
# the cross-compile flags the parent CMakeLists.txt sets up for TARGET_SOC.
set(CMAKE_CXX_FLAGS "-pthread")
set(CMAKE_EXE_LINKER_FLAGS "")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Fetch GoogleTest for the host test build. libgtest-dev is not assumed to be
# installed on the build machine, so pull a pinned release instead of relying
# on find_package(GTest).
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Match the parent build's runtime library linkage.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB TEST_SOURCES "test_*.cpp")

# Source files needed to satisfy symbols referenced by the ported tests:
# - util.cpp: get_opt/file_exists (test_cli_parsing.cpp), needs log_global.cpp
#   for its LG_INFO/LG_WARN logging macros.
# - configuration.cpp: config::load_from_file/AppConfig::validate
#   (test_config.cpp), needs model_spec.cpp/processing_params.cpp/
#   publisher_config.cpp/runtime_settings.cpp for the per-section
#   ::validate() calls AppConfig::validate() makes.
# test_tracking.cpp and the rest of the ported tests only exercise header
# structs/inline functions, so no further engine sources are needed.
set(TEST_DEPENDENCIES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/util/util.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/metrics/log_global.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/configuration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/model_spec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/processing_params.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/publisher_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/runtime_settings.cpp
    # npu_core.cc: select_core_mask (test_core_mask.cpp)
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/npu_core.cc
)

add_executable(run_engine_tests ${TEST_SOURCES} ${TEST_DEPENDENCIES})

target_link_libraries(run_engine_tests
    gtest
    gtest_main
    pthread
)

enable_testing()
add_test(NAME EngineUnitTests COMMAND run_engine_tests)
