cmake_minimum_required(VERSION 3.20)
project(axon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable strict warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(AXON_BUILD_STATIC     "Build static library"                      OFF)
option(AXON_BUILD_EXAMPLES   "Build examples"                           ON)
option(AXON_BUILD_TESTS      "Build tests (requires GTest)"             ON)
option(AXON_BUILD_BENCHMARK  "Build benchmarks (requires Google Benchmark)"   ON)
option(AXON_BUILD_PYTHON     "Build Python bindings (requires nanobind)" ON)
set(UCX_ROOT "" CACHE PATH "Root directory of a UCX installation")
set(AXON_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH "Root directory for vendored third-party dependencies")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
if(UCX_ROOT)
    set(_ucx_pkgconfig_dirs "")
    foreach(_dir IN ITEMS "${UCX_ROOT}/lib/pkgconfig" "${UCX_ROOT}/lib64/pkgconfig")
        if(IS_DIRECTORY "${_dir}")
            list(APPEND _ucx_pkgconfig_dirs "${_dir}")
        endif()
    endforeach()

    if(_ucx_pkgconfig_dirs)
        list(JOIN _ucx_pkgconfig_dirs ":" _ucx_pkgconfig_joined)
        if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
            set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${_ucx_pkgconfig_joined}")
        else()
            set(ENV{PKG_CONFIG_PATH} "${_ucx_pkgconfig_joined}")
        endif()
        message(STATUS "UCX_ROOT=${UCX_ROOT}")
        message(STATUS "UCX pkg-config search dirs: ${_ucx_pkgconfig_dirs}")
    else()
        message(STATUS "UCX_ROOT=${UCX_ROOT}, but no pkg-config dirs found under lib/pkgconfig or lib64/pkgconfig")
    endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(UCX REQUIRED IMPORTED_TARGET ucx)

# Optional: Google Test
if(AXON_BUILD_TESTS)
    if(EXISTS "${AXON_THIRD_PARTY_DIR}/googletest/CMakeLists.txt")
        message(STATUS "GoogleTest: vendored (${AXON_THIRD_PARTY_DIR}/googletest)")
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        add_subdirectory("${AXON_THIRD_PARTY_DIR}/googletest" "${CMAKE_BINARY_DIR}/third_party/googletest" EXCLUDE_FROM_ALL)
    else()
        find_package(GTest QUIET)
        if(NOT GTest_FOUND)
            message(STATUS "GoogleTest not found, tests will not be built")
            set(AXON_BUILD_TESTS OFF)
        else()
            message(STATUS "GoogleTest: system")
        endif()
    endif()
endif()

# Optional: Google Benchmark
if(AXON_BUILD_BENCHMARK)
    if(EXISTS "${AXON_THIRD_PARTY_DIR}/benchmark/CMakeLists.txt")
        message(STATUS "Google Benchmark: vendored (${AXON_THIRD_PARTY_DIR}/benchmark)")
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory("${AXON_THIRD_PARTY_DIR}/benchmark" "${CMAKE_BINARY_DIR}/third_party/benchmark" EXCLUDE_FROM_ALL)
    else()
        find_package(benchmark CONFIG QUIET)
        if(NOT benchmark_FOUND)
            message(STATUS "Google Benchmark not found, benchmarks will not be built")
            set(AXON_BUILD_BENCHMARK OFF)
        else()
            message(STATUS "Google Benchmark: system")
        endif()
    endif()
endif()

# Optional: nanobind for Python bindings
if(AXON_BUILD_PYTHON)
    # Find Python first
    find_package(Python REQUIRED COMPONENTS Interpreter Development)

    if(EXISTS "${AXON_THIRD_PARTY_DIR}/nanobind/CMakeLists.txt")
        message(STATUS "nanobind: vendored (${AXON_THIRD_PARTY_DIR}/nanobind)")
        add_subdirectory("${AXON_THIRD_PARTY_DIR}/nanobind" "${CMAKE_BINARY_DIR}/third_party/nanobind" EXCLUDE_FROM_ALL)
    else()
        # Get nanobind cmake dir from Python
        execute_process(
            COMMAND "${Python_EXECUTABLE}" "-c"
                    "import nanobind; print(nanobind.cmake_dir())"
            OUTPUT_VARIABLE nanobind_ROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE nanobind_status
        )

        if(NOT nanobind_status EQUAL 0)
            message(STATUS "nanobind not found, Python bindings will not be built")
            set(AXON_BUILD_PYTHON OFF)
        else()
            message(STATUS "nanobind: system (${nanobind_ROOT})")
            list(APPEND CMAKE_PREFIX_PATH "${nanobind_ROOT}")
            find_package(nanobind CONFIG REQUIRED)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library sources
# ---------------------------------------------------------------------------
set(AXON_SOURCES
    src/status.cpp
    src/cluster.cpp
    src/config.cpp
    src/worker.cpp
    src/endpoint.cpp
    src/future.cpp
    src/memory.cpp
    src/kv/protocol.cpp
    src/kv/metadata_store.cpp
    src/kv/tcp_transport.cpp
    src/kv/tcp_framing.cpp
    src/kv/server.cpp
    src/kv/node.cpp
)

add_library(axon SHARED ${AXON_SOURCES})
target_include_directories(axon
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(axon PUBLIC PkgConfig::UCX pthread)

# Static library variant
if(AXON_BUILD_STATIC)
    add_library(axon_static STATIC ${AXON_SOURCES})
    target_include_directories(axon_static
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(axon_static PUBLIC PkgConfig::UCX pthread)
    set_target_properties(axon_static PROPERTIES OUTPUT_NAME axon)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(AXON_BUILD_EXAMPLES)
    add_executable(ping_pong examples/ping_pong.cpp)
    target_link_libraries(ping_pong PRIVATE axon)

    add_executable(send_recv examples/send_recv.cpp)
    target_link_libraries(send_recv PRIVATE axon)

    add_executable(rdma_put_get examples/rdma_put_get.cpp)
    target_link_libraries(rdma_put_get PRIVATE axon)

    add_executable(kv_demo examples/kv_demo.cpp)
    target_link_libraries(kv_demo PRIVATE axon)

    add_executable(kv_wait_fetch examples/kv_wait_fetch.cpp)
    target_link_libraries(kv_wait_fetch PRIVATE axon)

    add_executable(kv_bench
        examples/kv_bench.cpp
        src/kv/bench_utils.cpp
    )
    target_link_libraries(kv_bench PRIVATE axon)
    target_include_directories(kv_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(AXON_BUILD_TESTS)
    enable_testing()

    # Unit tests
    add_executable(test_config tests/unit/test_config.cpp)
    target_link_libraries(test_config PRIVATE axon GTest::gtest GTest::gtest_main)
    target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME UnitConfig COMMAND test_config)

    add_executable(test_status tests/unit/test_status.cpp)
    target_link_libraries(test_status PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME UnitStatus COMMAND test_status)

    add_executable(test_future tests/unit/test_future.cpp)
    target_link_libraries(test_future PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME UnitFuture COMMAND test_future)

    add_executable(test_memory tests/unit/test_memory.cpp)
    target_link_libraries(test_memory PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME UnitMemory COMMAND test_memory)

    add_executable(test_cluster tests/unit/test_cluster.cpp)
    target_link_libraries(test_cluster PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME UnitCluster COMMAND test_cluster)

    add_executable(test_kv_metadata_store tests/unit/test_kv_metadata_store.cpp)
    target_link_libraries(test_kv_metadata_store PRIVATE axon GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_metadata_store PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME UnitKvMetadataStore COMMAND test_kv_metadata_store)

    add_executable(test_kv_bench
        tests/unit/test_kv_bench.cpp
        src/kv/bench_utils.cpp
    )
    target_link_libraries(test_kv_bench PRIVATE axon GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME UnitKvBench COMMAND test_kv_bench)

    # Integration tests
    add_executable(test_connection tests/integration/test_connection.cpp)
    target_link_libraries(test_connection PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationConnection COMMAND test_connection)

    add_executable(test_tag_messaging tests/integration/test_tag_messaging.cpp)
    target_link_libraries(test_tag_messaging PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationTagMessaging COMMAND test_tag_messaging)

    add_executable(test_rdma tests/integration/test_rdma.cpp)
    target_link_libraries(test_rdma PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationRdma COMMAND test_rdma)
    add_executable(test_loopback tests/integration/test_loopback.cpp)
    target_link_libraries(test_loopback PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationLoopback COMMAND test_loopback)

    add_executable(test_cluster_discovery tests/integration/test_cluster_discovery.cpp)
    target_link_libraries(test_cluster_discovery PRIVATE axon GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationClusterDiscovery COMMAND test_cluster_discovery)

    add_executable(test_kv_server tests/integration/test_kv_server.cpp)
    target_link_libraries(test_kv_server PRIVATE axon GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME IntegrationKvServer COMMAND test_kv_server)

    add_executable(test_kv_node tests/integration/test_kv_node.cpp)
    target_link_libraries(test_kv_node PRIVATE axon GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_node PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME IntegrationKvNode COMMAND test_kv_node)

    add_executable(test_kv_bench_integration
        tests/integration/test_kv_bench.cpp
        src/kv/bench_utils.cpp
    )
    target_link_libraries(test_kv_bench_integration PRIVATE axon GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_bench_integration PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME IntegrationKvBench COMMAND test_kv_bench_integration)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(AXON_BUILD_BENCHMARK)
    add_executable(bench_pingpong tests/benchmark/bench_pingpong.cpp)
    target_link_libraries(bench_pingpong PRIVATE axon benchmark::benchmark)

    add_executable(bench_throughput tests/benchmark/bench_throughput.cpp)
    target_link_libraries(bench_throughput PRIVATE axon benchmark::benchmark)
endif()

# Single-process benchmark (no GBenchmark dependency)
add_executable(bench_single_process tests/benchmark/bench_single_process.cpp)
target_link_libraries(bench_single_process PRIVATE axon)

# ---------------------------------------------------------------------------
# Python bindings
# ---------------------------------------------------------------------------
if(AXON_BUILD_PYTHON)
    nanobind_add_module(_core
        src/python/bindings.cpp
    )

    target_link_libraries(_core PRIVATE axon)

    target_include_directories(_core PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )

    # Install Python module to the axon package directory
    set_target_properties(_core PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/axon"
    )

    message(STATUS "Python bindings will be built: _core module")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS axon EXPORT axonTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/axon DESTINATION include)
install(EXPORT axonTargets
    FILE axonConfig.cmake
    NAMESPACE axon::
    DESTINATION lib/cmake/axon
)
