cmake_minimum_required(VERSION 3.15)
project(ZeroKV VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")

# Options
option(BUILD_PYTHON "Build Python bindings" ON)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARK "Build benchmark tool" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(ENABLE_LOGGING "Enable logging" ON)
option(ENABLE_METRICS "Enable metrics" ON)
option(BUILD_SHARED_LIBS "Build shared library" OFF)

# UCX - Build from source (version 1.19.0)
set(UCX_VERSION "1.19.0" CACHE STRING "UCX version")
set(UCX_SOURCE_DIR "${CMAKE_SOURCE_DIR}/thirdparty/ucx" CACHE PATH "UCX source directory")
set(UCX_INSTALL_DIR "${CMAKE_SOURCE_DIR}/thirdparty/ucx-install" CACHE PATH "UCX install directory")

# Check if UCX is installed locally
if(EXISTS "${UCX_INSTALL_DIR}/lib/cmake/ucx/ucx-config.cmake")
    # Set paths for UCX
    set(UCX_INCLUDE_DIRS "${UCX_INSTALL_DIR}/include")
    set(UCX_LIBRARY_DIRS "${UCX_INSTALL_DIR}/lib")
    set(UCX_LIBRARIES ucp ucs uct)

    # Try to find UCX packages
    find_package(UCP QUIET PATHS "${UCX_INSTALL_DIR}" NO_DEFAULT_PATH)
    find_package(UCS QUIET PATHS "${UCX_INSTALL_DIR}" NO_DEFAULT_PATH)
    find_package(UCT QUIET PATHS "${UCX_INSTALL_DIR}" NO_DEFAULT_PATH)

    if(UCP_FOUND OR EXISTS "${UCX_INCLUDE_DIRS}/ucp/api/ucp.h")
        set(USE_UCX ON)
        message(STATUS "UCX found (local): ${UCX_INSTALL_DIR}")
        # Add include and library paths
        include_directories(${UCX_INCLUDE_DIRS})
        link_directories(${UCX_LIBRARY_DIRS})
    else()
        set(USE_UCX OFF)
        message(WARNING "UCX installed but not detected, network features will be limited")
    endif()
elseif(EXISTS "${UCX_SOURCE_DIR}/configure.ac")
    # UCX source exists but not built - use system package
    find_package(UCP QUIET)
    find_package(UCS QUIET)
    find_package(UCT QUIET)

    if(UCP_FOUND)
        set(USE_UCX ON)
        message(STATUS "UCX found (system): ${UCP_VERSION}")
    else()
        set(USE_UCX OFF)
        message(WARNING "UCX not found, network features will be limited")
    endif()
else()
    # Fallback to system package
    find_package(UCP QUIET)
    find_package(UCS QUIET)
    find_package(UCT QUIET)

    if(UCP_FOUND)
        set(USE_UCX ON)
        message(STATUS "UCX found (system): ${UCP_VERSION}")
    else()
        set(USE_UCX OFF)
        message(WARNING "UCX not found, network features will be limited")
    endif()
endif()

# Python (optional)
if(BUILD_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development QUIET)
    if(Python3_FOUND)
        find_package(pybind11 REQUIRED QUIET)
        set(USE_PYTHON ON)
    else()
        set(USE_PYTHON OFF)
        message(WARNING "Python3 or pybind11 not found, Python bindings disabled")
    endif()
endif()

# Position independent code for shared library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Compiler optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-O3 -march=native -mtune=native)
    endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall -Wextra)
endif()

# Sanitizers (optional)
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(ENABLE_USAN "Enable Undefined Behavior Sanitizer" OFF)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_USAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

# Header files
set(PUBLIC_HEADERS
    include/zerokv/common.h
    include/zerokv/client.h
    include/zerokv/storage.h
    include/zerokv/protocol.h
    include/zerokv/transport.h
    include/zerokv/metadata.h
    include/zerokv/connection_pool.h
    include/zerokv/logging.h
    include/zerokv/checksum.h
    include/zerokv/config.h
    include/zerokv/metrics.h
    include/zerokv/retrier.h
)

# Source files
set(SOURCES
    src/protocol/codec.cc
    src/storage/storage.cc
    src/metadata/metadata.cc
    src/pool/connection_pool.cc
    src/logging/logging.cc
    src/checksum/checksum.cc
    src/config/config.cc
    src/metrics/metrics.cc
    src/error/retrier.cc
    src/client/client.cc
    src/client/client_v2.cc
)

# Transport sources (always needed)
list(APPEND SOURCES
    src/transport/transport.cc
)

# UCX-dependent sources
if(USE_UCX)
    list(APPEND SOURCES
        src/transport/ucx_transport.cc
        src/server/server.cc
    )
endif()

# Server executable (requires UCX)
if(USE_UCX)
    add_executable(zerokv_server
        src/server/main.cc
    )
    target_include_directories(zerokv_server PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(zerokv_server
        zerokv
        pthread
    )
    install(TARGETS zerokv_server DESTINATION bin)
endif()

# Python bindings
if(USE_PYTHON)
    pybind11_add_module(_zerokv python/pybind.cc)
    target_link_libraries(_zerokv PRIVATE zerokv)
    install(TARGETS _zerokv DESTINATION ${Python3_SITEARCH})
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmark
if(BUILD_BENCHMARK)
    add_subdirectory(tools/benchmark)
endif()

# Examples
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Generate CMake config files for find_package support
include(CMakePackageConfigHelpers)
include(GenerateExportHeader)

# Create exported targets (supports both static and shared based on BUILD_SHARED_LIBS)
add_library(zerokv ${SOURCES} ${PUBLIC_HEADERS})
set_target_properties(zerokv PROPERTIES
    OUTPUT_NAME "zerokv"
    OUTPUT_NAME_DEBUG "zerokv_d"
)

# Generate export header for shared library
generate_export_header(zerokv EXPORT_FILE_NAME zerokv_export.h)

target_include_directories(zerokv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(USE_UCX)
    target_link_libraries(zerokv PUBLIC ucp ucs uct)
    target_compile_definitions(zerokv PUBLIC USE_UCX)
endif()

if(ENABLE_LOGGING)
    target_compile_definitions(zerokv PUBLIC ENABLE_LOGGING)
endif()

if(ENABLE_METRICS)
    target_compile_definitions(zerokv PUBLIC ENABLE_METRICS)
endif()

configure_package_config_file(
    cmake/zerokv-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/zerokv-config.cmake
    INSTALL_DESTINATION lib/cmake/zerokv
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/zerokv-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Install
install(DIRECTORY include/zerokv DESTINATION include)
install(TARGETS zerokv ARCHIVE DESTINATION lib)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/zerokv-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/zerokv-config-version.cmake
    DESTINATION lib/cmake/zerokv
)

# Print configuration summary
message(STATUS "")
message(STATUS "=== ZeroKV Configuration ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "UCX: ${USE_UCX}")
message(STATUS "Python: ${USE_PYTHON}")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "Benchmark: ${BUILD_BENCHMARK}")
message(STATUS "Examples: ${BUILD_EXAMPLES}")
message(STATUS "Logging: ${ENABLE_LOGGING}")
message(STATUS "Metrics: ${ENABLE_METRICS}")
message(STATUS "===========================")
message(STATUS "")
