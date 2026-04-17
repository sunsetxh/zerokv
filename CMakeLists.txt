cmake_minimum_required(VERSION 3.20)
project(zerokv VERSION 1.0.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

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
option(ZEROKV_BUILD_STATIC     "Build static library"                      OFF)
option(ZEROKV_BUILD_EXAMPLES   "Build examples"                           ON)
option(ZEROKV_BUILD_TESTS      "Build tests (requires GTest)"             ON)
option(ZEROKV_BUILD_BENCHMARK  "Build benchmarks (requires Google Benchmark)"   ON)
option(ZEROKV_BUILD_PYTHON     "Build Python bindings (requires nanobind)" ON)
option(ZEROKV_LINK_UCX_STATIC  "Statically link UCX into zerokv shared libraries" OFF)
set(UCX_ROOT "" CACHE PATH "Root directory of a UCX installation")
set(ZEROKV_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH "Root directory for vendored third-party dependencies")

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

add_library(zerokv_ucx INTERFACE)
target_include_directories(zerokv_ucx INTERFACE ${UCX_INCLUDE_DIRS})

if(ZEROKV_LINK_UCX_STATIC)
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --libs --static ucx
        OUTPUT_VARIABLE UCX_STATIC_LINK_LINE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE UCX_STATIC_RESULT
    )
    if(NOT UCX_STATIC_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to resolve static UCX link flags with pkg-config --static")
    endif()

    separate_arguments(UCX_STATIC_ARGS NATIVE_COMMAND "${UCX_STATIC_LINK_LINE}")
    set(UCX_STATIC_LINK_DIRS "")
    set(UCX_STATIC_LINK_OPTIONS "")
    set(UCX_STATIC_LINK_LIBS "")
    foreach(arg IN LISTS UCX_STATIC_ARGS)
        if(arg MATCHES "^-L(.+)")
            list(APPEND UCX_STATIC_LINK_DIRS "${CMAKE_MATCH_1}")
        elseif(arg MATCHES "^-Wl,")
            list(APPEND UCX_STATIC_LINK_OPTIONS "${arg}")
        else()
            list(APPEND UCX_STATIC_LINK_LIBS "${arg}")
        endif()
    endforeach()

    target_link_directories(zerokv_ucx INTERFACE ${UCX_STATIC_LINK_DIRS})
    target_link_libraries(zerokv_ucx INTERFACE ${UCX_STATIC_LINK_LIBS})
    target_link_options(zerokv_ucx INTERFACE ${UCX_STATIC_LINK_OPTIONS})
    if(UNIX AND NOT APPLE)
        target_link_options(
            zerokv_ucx
            INTERFACE
                "-Wl,--exclude-libs,libucp.a:libuct.a:libucs.a:libucm.a"
        )
    endif()
else()
    target_link_libraries(zerokv_ucx INTERFACE PkgConfig::UCX)
endif()

# Optional: Google Test
if(ZEROKV_BUILD_TESTS)
    if(EXISTS "${ZEROKV_THIRD_PARTY_DIR}/googletest/CMakeLists.txt")
        message(STATUS "GoogleTest: vendored (${ZEROKV_THIRD_PARTY_DIR}/googletest)")
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        add_subdirectory("${ZEROKV_THIRD_PARTY_DIR}/googletest" "${CMAKE_BINARY_DIR}/third_party/googletest" EXCLUDE_FROM_ALL)
    else()
        find_package(GTest QUIET)
        if(NOT GTest_FOUND)
            message(STATUS "GoogleTest not found, tests will not be built")
            set(ZEROKV_BUILD_TESTS OFF)
        else()
            message(STATUS "GoogleTest: system")
        endif()
    endif()
endif()

# Optional: Google Benchmark
if(ZEROKV_BUILD_BENCHMARK)
    if(EXISTS "${ZEROKV_THIRD_PARTY_DIR}/benchmark/CMakeLists.txt")
        message(STATUS "Google Benchmark: vendored (${ZEROKV_THIRD_PARTY_DIR}/benchmark)")
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory("${ZEROKV_THIRD_PARTY_DIR}/benchmark" "${CMAKE_BINARY_DIR}/third_party/benchmark" EXCLUDE_FROM_ALL)
    else()
        find_package(benchmark CONFIG QUIET)
        if(NOT benchmark_FOUND)
            message(STATUS "Google Benchmark not found, benchmarks will not be built")
            set(ZEROKV_BUILD_BENCHMARK OFF)
        else()
            message(STATUS "Google Benchmark: system")
        endif()
    endif()
endif()

# Optional: nanobind for Python bindings
if(ZEROKV_BUILD_PYTHON)
    # Find Python first
    find_package(Python REQUIRED COMPONENTS Interpreter Development)

    if(EXISTS "${ZEROKV_THIRD_PARTY_DIR}/nanobind/CMakeLists.txt")
        message(STATUS "nanobind: vendored (${ZEROKV_THIRD_PARTY_DIR}/nanobind)")
        add_subdirectory("${ZEROKV_THIRD_PARTY_DIR}/nanobind" "${CMAKE_BINARY_DIR}/third_party/nanobind" EXCLUDE_FROM_ALL)
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
            set(ZEROKV_BUILD_PYTHON OFF)
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
set(ZEROKV_SOURCES
    src/status.cpp
    src/cluster.cpp
    src/config.cpp
    src/internal/logging.cpp
    src/transport/worker.cpp
    src/transport/endpoint.cpp
    src/transport/future.cpp
    src/transport/memory.cpp
    src/kv.cpp
    src/core/protocol.cpp
    src/core/metadata_store.cpp
    src/core/tcp_transport.cpp
    src/core/tcp_framing.cpp
    src/core/server.cpp
    src/core/node.cpp
)

add_library(zerokv SHARED ${ZEROKV_SOURCES})
target_include_directories(zerokv
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            ${UCX_INCLUDE_DIRS}
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(zerokv PUBLIC cxx_std_20)
target_link_libraries(zerokv PRIVATE zerokv_ucx Threads::Threads)

add_library(alps_kv_wrap_core STATIC
    src/compat/alps_kv_channel.cpp
)
target_include_directories(alps_kv_wrap_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(alps_kv_wrap_core PUBLIC cxx_std_20)
target_link_libraries(alps_kv_wrap_core PUBLIC zerokv Threads::Threads)
if(ZEROKV_BUILD_TESTS)
    target_compile_definitions(alps_kv_wrap_core PUBLIC ZEROKV_ALPS_TEST_HOOKS)
endif()

add_library(alps_kv_wrap SHARED
    src/compat/alps_kv_api.cpp
)
target_include_directories(alps_kv_wrap
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(alps_kv_wrap PUBLIC cxx_std_20)
target_link_libraries(alps_kv_wrap PRIVATE alps_kv_wrap_core)

# Static library variant
if(ZEROKV_BUILD_STATIC)
    add_library(zerokv_static STATIC ${ZEROKV_SOURCES})
    target_include_directories(zerokv_static
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                ${UCX_INCLUDE_DIRS}
        PRIVATE ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(zerokv_static PUBLIC cxx_std_20)
    target_link_libraries(zerokv_static PUBLIC zerokv_ucx Threads::Threads)
    set_target_properties(zerokv_static PROPERTIES OUTPUT_NAME zerokv)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(ZEROKV_BUILD_EXAMPLES)
    add_executable(ping_pong examples/ping_pong.cpp)
    target_link_libraries(ping_pong PRIVATE zerokv)

    add_executable(send_recv examples/send_recv.cpp)
    target_link_libraries(send_recv PRIVATE zerokv)

    add_executable(rdma_put_get examples/rdma_put_get.cpp)
    target_link_libraries(rdma_put_get PRIVATE zerokv)

    add_executable(kv_demo examples/kv_demo.cpp)
    target_link_libraries(kv_demo PRIVATE zerokv)

    add_executable(kv_wait_fetch examples/kv_wait_fetch.cpp)
    target_link_libraries(kv_wait_fetch PRIVATE zerokv)

    add_executable(message_kv_demo examples/message_kv_demo.cpp)
    target_link_libraries(message_kv_demo PRIVATE zerokv)

    add_executable(kv_bench
        examples/kv_bench.cpp
        src/core/bench_utils.cpp
    )
    target_link_libraries(kv_bench PRIVATE zerokv)
    target_include_directories(kv_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

    add_executable(alps_kv_bench examples/alps_kv_bench.cpp)
    target_link_libraries(alps_kv_bench PRIVATE alps_kv_wrap Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ZEROKV_BUILD_TESTS)
    enable_testing()

    # Unit tests
    add_executable(test_config tests/unit/test_config.cpp)
    target_link_libraries(test_config PRIVATE zerokv GTest::gtest GTest::gtest_main)
    target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME UnitConfig COMMAND test_config)

    add_executable(test_status tests/unit/test_status.cpp)
    target_link_libraries(test_status PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME UnitStatus COMMAND test_status)

    add_executable(test_logging tests/unit/test_logging.cpp)
    target_link_libraries(test_logging PRIVATE zerokv GTest::gtest GTest::gtest_main)
    target_include_directories(test_logging PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME UnitLogging COMMAND test_logging)

    add_executable(test_future tests/unit/test_future.cpp)
    target_link_libraries(test_future PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME UnitFuture COMMAND test_future)

    add_executable(test_memory tests/unit/test_memory.cpp)
    target_link_libraries(test_memory PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME UnitMemory COMMAND test_memory)

    add_executable(test_listener_addr tests/unit/test_listener_addr.cpp)
    target_link_libraries(test_listener_addr PRIVATE zerokv GTest::gtest GTest::gtest_main)
    target_include_directories(test_listener_addr PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME UnitListenerAddr COMMAND test_listener_addr)

    add_executable(test_cluster tests/unit/test_cluster.cpp)
    target_link_libraries(test_cluster PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME UnitCluster COMMAND test_cluster)

    add_executable(test_kv_metadata_store tests/unit/test_kv_metadata_store.cpp)
    target_link_libraries(test_kv_metadata_store PRIVATE zerokv GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_metadata_store PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME UnitKvMetadataStore COMMAND test_kv_metadata_store)

    add_executable(test_kv_bench
        tests/unit/test_kv_bench.cpp
        src/core/bench_utils.cpp
    )
    target_link_libraries(test_kv_bench PRIVATE zerokv GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME UnitKvBench COMMAND test_kv_bench)

    # Integration tests
    add_executable(test_connection tests/integration/test_connection.cpp)
    target_link_libraries(test_connection PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationConnection COMMAND test_connection)

    add_executable(test_tag_messaging tests/integration/test_tag_messaging.cpp)
    target_link_libraries(test_tag_messaging PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationTagMessaging COMMAND test_tag_messaging)

    add_executable(test_rdma tests/integration/test_rdma.cpp)
    target_link_libraries(test_rdma PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationRdma COMMAND test_rdma)
    add_executable(test_loopback tests/integration/test_loopback.cpp)
    target_link_libraries(test_loopback PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationLoopback COMMAND test_loopback)

    add_executable(test_cluster_discovery tests/integration/test_cluster_discovery.cpp)
    target_link_libraries(test_cluster_discovery PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationClusterDiscovery COMMAND test_cluster_discovery)

    add_executable(test_kv_server tests/integration/test_kv_server.cpp)
    target_link_libraries(test_kv_server PRIVATE zerokv GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME IntegrationKvServer COMMAND test_kv_server)

    add_executable(test_kv_node tests/integration/test_kv_node.cpp)
    target_link_libraries(test_kv_node PRIVATE zerokv GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_node PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME IntegrationKvNode COMMAND test_kv_node)

    add_executable(test_kv_bench_integration
        tests/integration/test_kv_bench.cpp
        src/core/bench_utils.cpp
    )
    target_link_libraries(test_kv_bench_integration PRIVATE zerokv GTest::gtest GTest::gtest_main)
    target_include_directories(test_kv_bench_integration PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME IntegrationKvBench COMMAND test_kv_bench_integration)

    add_executable(test_message_kv tests/integration/test_message_kv.cpp)
    target_link_libraries(test_message_kv PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationMessageKV COMMAND test_message_kv)

    add_executable(test_message_kv_demo tests/integration/test_message_kv_demo.cpp)
    target_sources(test_message_kv_demo PRIVATE examples/message_kv_demo.cpp)
    target_compile_definitions(test_message_kv_demo PRIVATE MESSAGE_KV_DEMO_BUILD_TESTS)
    target_link_libraries(test_message_kv_demo PRIVATE zerokv GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationMessageKvDemo COMMAND test_message_kv_demo)

    add_executable(test_alps_kv_wrap tests/integration/test_alps_kv_wrap.cpp)
    target_link_libraries(test_alps_kv_wrap PRIVATE alps_kv_wrap_core GTest::gtest GTest::gtest_main)
    target_include_directories(test_alps_kv_wrap PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME IntegrationAlpsKvWrap COMMAND test_alps_kv_wrap)

    add_executable(test_alps_kv_bench tests/integration/test_alps_kv_bench.cpp)
    target_sources(test_alps_kv_bench PRIVATE examples/alps_kv_bench.cpp)
    target_compile_definitions(test_alps_kv_bench PRIVATE ALPS_KV_BENCH_BUILD_TESTS)
    target_link_libraries(test_alps_kv_bench PRIVATE alps_kv_wrap GTest::gtest GTest::gtest_main)
    add_test(NAME IntegrationAlpsKvBench COMMAND test_alps_kv_bench)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(ZEROKV_BUILD_BENCHMARK)
    add_executable(bench_pingpong tests/benchmark/bench_pingpong.cpp)
    target_link_libraries(bench_pingpong PRIVATE zerokv benchmark::benchmark)

    add_executable(bench_throughput tests/benchmark/bench_throughput.cpp)
    target_link_libraries(bench_throughput PRIVATE zerokv benchmark::benchmark)
endif()

# Single-process benchmark (no GBenchmark dependency)
add_executable(bench_single_process tests/benchmark/bench_single_process.cpp)
target_link_libraries(bench_single_process PRIVATE zerokv)

# ---------------------------------------------------------------------------
# Python bindings
# ---------------------------------------------------------------------------
if(ZEROKV_BUILD_PYTHON)
    nanobind_add_module(_core
        src/python/bindings.cpp
    )

    target_link_libraries(_core PRIVATE zerokv)

    target_include_directories(_core PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )

    # Install Python module to the zerokv package directory
    set_target_properties(_core PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/zerokv"
    )

    message(STATUS "Python bindings will be built: _core module")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS zerokv alps_kv_wrap EXPORT zerokvTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/zerokv DESTINATION include)
install(DIRECTORY include/yr DESTINATION include)
if(TARGET alps_kv_bench)
    install(TARGETS alps_kv_bench
        RUNTIME DESTINATION bin
    )
endif()
install(FILES "${CMAKE_SOURCE_DIR}/docs/alps_kv_wrap/README.md"
    DESTINATION share/doc/alps_kv_wrap
)
if(ZEROKV_LINK_UCX_STATIC)
    set(ZEROKV_PACKAGE_NEEDS_UCX_PKGCONFIG OFF)
else()
    set(ZEROKV_PACKAGE_NEEDS_UCX_PKGCONFIG ON)
endif()
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/zerokvConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/zerokvConfig.cmake"
    INSTALL_DESTINATION lib/cmake/zerokv
)
install(EXPORT zerokvTargets
    FILE zerokvTargets.cmake
    NAMESPACE zerokv::
    DESTINATION lib/cmake/zerokv
)
install(FILES "${CMAKE_BINARY_DIR}/zerokvConfig.cmake"
    DESTINATION lib/cmake/zerokv
)
