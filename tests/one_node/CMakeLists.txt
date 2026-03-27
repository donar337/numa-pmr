include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)

FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# Fetched whenever tests are enabled so clangd gets compile commands for
# benchmark/bench_allocators.cpp even when NUMA_ALLOCATOR_BUILD_BENCHMARKS is OFF.
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

function(add_numa_one_node_test target source label)
    add_executable(${target} ${source})

    target_link_libraries(${target}
        PRIVATE
            numa_allocator
            Catch2::Catch2WithMain
    )

    target_include_directories(${target}
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    numa_allocator_enable_coverage(${target})

    catch_discover_tests(${target}
        TEST_PREFIX "one_node."
        PROPERTIES
            LABELS "${label}"
    )
endfunction()

add_numa_one_node_test(
    one_node_unit_smoke
    unit/test_unit_smoke.cpp
    one_node_unit
)

add_numa_one_node_test(
    one_node_integration_smoke
    integration/test_integration_smoke.cpp
    one_node_integration
)

if(NUMA_ALLOCATOR_BUILD_BENCHMARKS)
    add_executable(one_node_bench_allocators
        benchmark/bench_allocators.cpp
    )

    target_link_libraries(one_node_bench_allocators
        PRIVATE
            numa_allocator
            benchmark::benchmark
    )
else()
    add_library(one_node_bench_allocators_index OBJECT EXCLUDE_FROM_ALL
        benchmark/bench_allocators.cpp
    )

    target_link_libraries(one_node_bench_allocators_index
        PRIVATE
            numa_allocator
            benchmark::benchmark
    )
endif()

if(NUMA_ALLOCATOR_ENABLE_COVERAGE)
    find_program(GCOVR_EXECUTABLE gcovr)

    if(GCOVR_EXECUTABLE)
        add_custom_target(coverage_one_node_unit
            COMMAND
                "${CMAKE_CTEST_COMMAND}"
                --output-on-failure
                -L one_node_unit
            COMMAND
                "${GCOVR_EXECUTABLE}"
                -r "${CMAKE_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
                --filter "${CMAKE_SOURCE_DIR}/src"
                --html
                --html-details
                -o "${CMAKE_BINARY_DIR}/coverage_one_node_unit.html"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Running one-node unit tests and generating coverage report"
            DEPENDS one_node_unit_smoke
            VERBATIM
        )
    else()
        message(WARNING "gcovr was not found; coverage_one_node_unit target will not be available")
    endif()
endif()
