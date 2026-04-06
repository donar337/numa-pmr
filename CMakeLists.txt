cmake_minimum_required(VERSION 3.20)

project(numa_allocator
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NUMA_ALLOCATOR_BUILD_TESTS "Build NUMA allocator tests" ON)
option(NUMA_ALLOCATOR_BUILD_PLAYGROUND "Build allocator playground executable" OFF)
option(NUMA_ALLOCATOR_BUILD_BENCHMARKS "Build NUMA allocator benchmarks" OFF)
option(NUMA_ALLOCATOR_ENABLE_COVERAGE "Enable coverage flags for supported targets" OFF)

find_package(Threads REQUIRED)

find_path(NUMA_INCLUDE_DIR numa.h)
find_library(NUMA_LIBRARY numa)

if(NOT NUMA_INCLUDE_DIR OR NOT NUMA_LIBRARY)
    message(FATAL_ERROR "libnuma development files were not found. Install libnuma-dev/numactl-devel.")
endif()

add_library(NUMA::NUMA UNKNOWN IMPORTED)
set_target_properties(NUMA::NUMA PROPERTIES
    IMPORTED_LOCATION "${NUMA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NUMA_INCLUDE_DIR}"
)

add_library(numa_allocator STATIC
    src/virtual_memory.cpp
    src/size_divide/large_object/large_object_allocator.cpp
    src/size_divide/small_object/small_object_allocator.cpp
    src/numa_arena.cpp
    src/thread_local/thread_local_cache.cpp
)

target_include_directories(numa_allocator
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(numa_allocator PUBLIC cxx_std_20)

target_link_libraries(numa_allocator
    PUBLIC
        NUMA::NUMA
        Threads::Threads
)

function(numa_allocator_enable_coverage target)
    if(NOT NUMA_ALLOCATOR_ENABLE_COVERAGE)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -O0 -g --coverage)
        target_link_options(${target} PRIVATE --coverage)
    else()
        message(WARNING "Coverage is only configured for GCC/Clang-like compilers")
    endif()
endfunction()

numa_allocator_enable_coverage(numa_allocator)

if(NUMA_ALLOCATOR_BUILD_PLAYGROUND)
    add_executable(numa_allocator_playground
        tools/allocator_playground.cpp
    )

    target_compile_features(numa_allocator_playground PRIVATE cxx_std_20)

    target_link_libraries(numa_allocator_playground
        PRIVATE
            numa_allocator
    )

    numa_allocator_enable_coverage(numa_allocator_playground)
endif()

if(NUMA_ALLOCATOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
