cmake_minimum_required(VERSION 3.20)
project(MarketLab VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization and warning flags
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_options("$<$<CONFIG:Release>:/O2 /Ob2 /Oi /Ot>")
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options("$<$<CONFIG:Release>:-O3>")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

# Core library
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_STATUS_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_COMMIT_HASH)
    if(GIT_STATUS_OUTPUT)
        set(MARKETLAB_BUILD_REVISION "${GIT_COMMIT_HASH}-dirty")
    else()
        set(MARKETLAB_BUILD_REVISION "${GIT_COMMIT_HASH}")
    endif()
else()
    set(MARKETLAB_BUILD_REVISION "unavailable")
endif()

add_compile_definitions(MARKETLAB_BUILD_REVISION="${MARKETLAB_BUILD_REVISION}")

add_library(marketlab_core
    src/order_book.cpp
    src/reference_matcher.cpp
    src/workload_generator.cpp
    src/serializer.cpp
    src/invariants.cpp
    src/portfolio.cpp
    src/strategy.cpp
    src/simulation_engine.cpp
    src/market_data_normalizer.cpp
    src/historical_replay_engine.cpp
)

target_include_directories(marketlab_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# CLI application
add_executable(marketlab app/main.cpp)
target_link_libraries(marketlab PRIVATE marketlab_core)

# Tests executable
enable_testing()
add_executable(marketlab_tests tests/test_matcher.cpp)
target_link_libraries(marketlab_tests PRIVATE marketlab_core)
add_test(NAME AllTests COMMAND marketlab_tests)

add_executable(marketlab_regressions tests/test_regressions.cpp)
target_link_libraries(marketlab_regressions PRIVATE marketlab_core)
add_test(NAME RegressionTests COMMAND marketlab_regressions)

add_executable(marketlab_sim_tests tests/test_simulation.cpp)
target_link_libraries(marketlab_sim_tests PRIVATE marketlab_core)
add_test(NAME SimTests COMMAND marketlab_sim_tests)

add_executable(marketlab_hist_tests tests/test_historical.cpp)
target_link_libraries(marketlab_hist_tests PRIVATE marketlab_core)
add_test(NAME HistTests COMMAND marketlab_hist_tests)

# Benchmark executable
add_executable(marketlab_benchmark benchmarks/benchmark.cpp)
target_link_libraries(marketlab_benchmark PRIVATE marketlab_core)
