include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)

FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

function(add_numa_multi_node_test target source label)
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
        TEST_PREFIX "multi_node."
        PROPERTIES
            LABELS "${label}"
    )
endfunction()

add_numa_multi_node_test(
    multi_node_unit_virtual_memory
    unit/virtual_memory.cpp
    multi_node_unit
)

add_numa_multi_node_test(
    multi_node_unit_block
    unit/size_divide/block.cpp
    multi_node_unit
)

add_numa_multi_node_test(
    multi_node_unit_small_object
    unit/size_divide/small_object.cpp
    multi_node_unit
)

add_numa_multi_node_test(
    multi_node_unit_large_object
    unit/size_divide/large_object.cpp
    multi_node_unit
)

add_numa_multi_node_test(
    multi_node_unit_arena
    unit/arena.cpp
    multi_node_unit
)

add_numa_multi_node_test(
    multi_node_unit_arena_manager
    unit/arena_manager.cpp
    multi_node_unit
)

add_numa_multi_node_test(
    multi_node_unit_thread_cache
    unit/thread_cache.cpp
    multi_node_unit
)

add_numa_multi_node_test(
    multi_node_unit_pmr_resources
    unit/pmr_resources.cpp
    multi_node_unit
)

add_numa_multi_node_test(
    multi_node_integration_numa_simple_memory_resource
    integration/numa_simple_memory_resource.cpp
    multi_node_integration
)

add_numa_multi_node_test(
    multi_node_integration_numa_arena_memory_resource
    integration/numa_arena_memory_resource.cpp
    multi_node_integration
)

add_numa_multi_node_test(
    multi_node_integration_numa_memory_resource
    integration/numa_memory_resource.cpp
    multi_node_integration
)

if(NUMA_ALLOCATOR_ENABLE_COVERAGE)
    find_program(GCOVR_EXECUTABLE gcovr)

    if(GCOVR_EXECUTABLE)
        add_custom_target(coverage_multi_node_unit
            COMMAND
                "${CMAKE_COMMAND}"
                -D "COVERAGE_BINARY_DIR=${CMAKE_BINARY_DIR}"
                -P "${CMAKE_SOURCE_DIR}/cmake/remove_coverage_data.cmake"
            COMMAND
                "${CMAKE_CTEST_COMMAND}"
                --output-on-failure
                -L multi_node_unit
            COMMAND
                "${GCOVR_EXECUTABLE}"
                -r "${CMAKE_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
                --filter "${CMAKE_SOURCE_DIR}/src"
                --gcov-ignore-parse-errors negative_hits.warn_once_per_file
                --html
                --html-details
                -o "${CMAKE_BINARY_DIR}/coverage_multi_node_unit.html"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Running multi-node unit tests and generating coverage report"
            DEPENDS
                multi_node_unit_virtual_memory
                multi_node_unit_block
                multi_node_unit_small_object
                multi_node_unit_large_object
                multi_node_unit_arena
                multi_node_unit_arena_manager
                multi_node_unit_thread_cache
                multi_node_unit_pmr_resources
            VERBATIM
        )
    else()
        message(WARNING "gcovr was not found; coverage_multi_node_unit target will not be available")
    endif()
endif()
