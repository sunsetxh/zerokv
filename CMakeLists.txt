cmake_minimum_required(VERSION 3.15)
project(zerokv VERSION 1.0.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_PYTHON "Build Python bindings" ON)
option(USE_REAL_NPU "Use real NPU (requires hardware)" OFF)
option(ENABLE_PROFILING "Enable profiling support" OFF)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Find dependencies
find_package(Threads REQUIRED)

#==============================================================================
# UCX (Unified Communication X) Configuration
#
# Build Modes:
#   1. Stub Mode (for development without UCX)
#      cmake .. -DUSE_UCX_STUB=ON
#
#   2. Auto Build Mode (default, builds UCX 1.20.0 from source)
#      cmake .. -DUSE_UCX_STUB=OFF
#
#   3. Custom UCX Path Mode (use pre-built UCX)
#      cmake .. -DUSE_UCX_STUB=OFF -DUCX_ROOT=/path/to/ucx
#
# Note: UCX build requires Linux with build tools (autoconf, libtool, etc.)
#==============================================================================

option(USE_UCX_STUB "Use UCX stub implementation for development" OFF)
set(UCX_ROOT "" CACHE PATH "Path to existing UCX installation (optional)")

if(USE_UCX_STUB)
    # Mode 1: Use stub implementation
    message(STATUS "UCX Mode: Stub implementation (for development)")
    set(UCX_LIBRARIES "")
    set(UCX_INCLUDE_DIRS "")
    add_definitions(-DUSE_UCX_STUB)
    set(HAVE_REAL_UCX OFF)
    set(NEED_UCX_DEPENDENCY FALSE)

else()
    # Real UCX required
    if(UCX_ROOT)
        # Mode 3: Use user-provided UCX installation
        message(STATUS "UCX Mode: Using custom installation at ${UCX_ROOT}")

        # Verify UCX_ROOT is valid
        if(NOT EXISTS "${UCX_ROOT}/include/ucp/api/ucp.h")
            message(FATAL_ERROR
                "Invalid UCX_ROOT: ${UCX_ROOT}\n"
                "Could not find ucp/api/ucp.h in ${UCX_ROOT}/include\n"
                "Please provide a valid UCX installation path or remove UCX_ROOT to build from source.")
        endif()

        # Find libraries in UCX_ROOT
        find_library(UCX_UCP_LIB ucp
            PATHS ${UCX_ROOT}/lib ${UCX_ROOT}/lib64
            NO_DEFAULT_PATH
            REQUIRED)
        find_library(UCX_UCS_LIB ucs
            PATHS ${UCX_ROOT}/lib ${UCX_ROOT}/lib64
            NO_DEFAULT_PATH
            REQUIRED)

        # Optional: UCT library
        find_library(UCX_UCT_LIB uct
            PATHS ${UCX_ROOT}/lib ${UCX_ROOT}/lib64
            NO_DEFAULT_PATH)

        set(UCX_INCLUDE_DIRS ${UCX_ROOT}/include)
        if(UCX_UCT_LIB)
            set(UCX_LIBRARIES ${UCX_UCP_LIB} ${UCX_UCS_LIB} ${UCX_UCT_LIB})
        else()
            set(UCX_LIBRARIES ${UCX_UCP_LIB} ${UCX_UCS_LIB})
        endif()

        set(HAVE_REAL_UCX ON)
        set(NEED_UCX_DEPENDENCY FALSE)

        message(STATUS "  UCX include: ${UCX_INCLUDE_DIRS}")
        message(STATUS "  UCX libraries: ${UCX_LIBRARIES}")

    else()
        # Mode 2: Build UCX 1.20.0 from source
        message(STATUS "UCX Mode: Building UCX 1.20.0 from source")
        message(STATUS "  This requires Linux with autoconf, libtool, make, gcc, etc.")
        message(STATUS "  To use pre-built UCX, set: -DUCX_ROOT=/path/to/ucx")
        message(STATUS "  For development without UCX, set: -DUSE_UCX_STUB=ON")

        include(ExternalProject)

        # UCX 1.20.0 release
        ExternalProject_Add(ucx
            URL https://github.com/openucx/ucx/releases/download/1.20.0/ucx-1.20.0.tar.gz
            URL_HASH SHA256=8a2776c3e8d7da8aa45abe640a6956b33f71b59cf4120566a888ee2b0ecfe667
            BUILD_IN_SOURCE 1
            CONFIGURE_COMMAND ${CMAKE_CURRENT_BINARY_DIR}/ucx-prefix/src/ucx/configure
                --prefix=${CMAKE_CURRENT_BINARY_DIR}/ucx-install
                --enable-shared
                --disable-static
                --disable-debug
                --disable-assertions
                --disable-mt
            BUILD_COMMAND make -j${CMAKE_BUILD_PARALLEL_LEVEL}
            INSTALL_COMMAND make install
            BUILD_BYPRODUCTS
                ${CMAKE_CURRENT_BINARY_DIR}/ucx-install/lib/libucp${CMAKE_SHARED_LIBRARY_SUFFIX}
                ${CMAKE_CURRENT_BINARY_DIR}/ucx-install/lib/libucs${CMAKE_SHARED_LIBRARY_SUFFIX}
        )

        # Set UCX variables for our project
        set(UCX_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/ucx-install)
        set(UCX_INCLUDE_DIRS ${UCX_INSTALL_DIR}/include)
        set(UCX_UCP_LIB ${UCX_INSTALL_DIR}/lib/libucp${CMAKE_SHARED_LIBRARY_SUFFIX})
        set(UCX_UCS_LIB ${UCX_INSTALL_DIR}/lib/libucs${CMAKE_SHARED_LIBRARY_SUFFIX})
        set(UCX_LIBRARIES ${UCX_UCP_LIB} ${UCX_UCS_LIB})
        set(HAVE_REAL_UCX ON)
        set(NEED_UCX_DEPENDENCY TRUE)

        message(STATUS "  UCX will be installed to: ${UCX_INSTALL_DIR}")
    endif()
endif()

include_directories(${UCX_INCLUDE_DIRS})

# Protobuf - Try to find system installation first, otherwise build from source
find_package(Protobuf QUIET)

if(NOT Protobuf_FOUND)
    message(STATUS "System Protobuf not found. Building from source...")

    include(FetchContent)
    FetchContent_Declare(
        protobuf
        URL https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protobuf-cpp-3.21.12.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    # Configure protobuf options
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "Build protobuf tests")
    set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "Build protobuf examples")
    set(protobuf_WITH_ZLIB OFF CACHE BOOL "Build with zlib support")

    # Add -fPIC for protobuf (required for ARM64 shared libraries)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "PIC" FORCE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

    # Disable -Werror for protobuf to avoid compilation errors
    set(CMAKE_CXX_FLAGS_SAVED "${CMAKE_CXX_FLAGS}")
    string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

    FetchContent_MakeAvailable(protobuf)

    # Restore original flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_SAVED}")

    # Set protobuf variables for compatibility
    set(Protobuf_INCLUDE_DIRS ${protobuf_SOURCE_DIR}/src)
    set(Protobuf_LIBRARIES libprotobuf)
    set(Protobuf_PROTOC_EXECUTABLE $<TARGET_FILE:protoc>)

    message(STATUS "Built Protobuf from source")
else()
    message(STATUS "Found system Protobuf: ${Protobuf_VERSION}")
endif()

include_directories(${Protobuf_INCLUDE_DIRS})

# Generate protobuf files
set(PROTO_FILES
    ${PROJECT_SOURCE_DIR}/protos/zerokv.proto
)

if(Protobuf_FOUND)
    protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
else()
    # Manual generation when using FetchContent
    set(PROTO_SRCS ${CMAKE_CURRENT_BINARY_DIR}/zerokv.pb.cc)
    set(PROTO_HDRS ${CMAKE_CURRENT_BINARY_DIR}/zerokv.pb.h)

    add_custom_command(
        OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             -I${PROJECT_SOURCE_DIR}/protos
             ${PROTO_FILES}
        DEPENDS ${PROTO_FILES} protoc
        COMMENT "Generating protobuf C++ files"
    )
endif()

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# NPU ACL (optional, for real NPU)
if(USE_REAL_NPU)
    find_path(ACL_INCLUDE_DIR acl/acl.h
        HINTS /usr/local/Ascend/ascend-toolkit/latest/include)
    find_library(ACL_LIBRARY ascendcl
        HINTS /usr/local/Ascend/ascend-toolkit/latest/lib64)

    if(ACL_INCLUDE_DIR AND ACL_LIBRARY)
        message(STATUS "Found ACL: ${ACL_INCLUDE_DIR}")
        include_directories(${ACL_INCLUDE_DIR})
        set(NPU_LIBRARIES ${ACL_LIBRARY})
        add_definitions(-DUSE_REAL_NPU)
    else()
        message(WARNING "ACL not found. Using mock implementation.")
        set(USE_REAL_NPU OFF)
    endif()
endif()

# Common source files
set(COMMON_SRCS
    src/common/p2p_ucx_mock.cpp
    src/common/logger.cpp
)

# Add protobuf generated files separately to set compile options
set(PROTO_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/zerokv.pb.cc
)

# Add UCX stub if using stub implementation
if(USE_UCX_STUB)
    list(APPEND COMMON_SRCS src/common/ucx_stub.cpp)
endif()

# Server source files
set(SERVER_SRCS
    src/server/ucx_control_server.cpp  # Task #8
    # src/server/zerokv_server.cpp  # Milestone #2 - TODO
    # src/monitor/performance_monitor.cpp  # Milestone #5 - TODO
)

# Client source files
set(CLIENT_SRCS
    src/client/ucx_control_client.cpp  # Task #9
    # src/client/zerokv_client.cpp  # Milestone #2 - TODO
)

# Build shared library
add_library(zerokv_common SHARED ${COMMON_SRCS} ${PROTO_SRCS})
target_link_libraries(zerokv_common
    ${UCX_LIBRARIES}
    ${Protobuf_LIBRARIES}
    Threads::Threads
)

# Suppress unused parameter warnings for protobuf generated code
set_source_files_properties(${PROTO_SRCS} PROPERTIES
    COMPILE_FLAGS "-Wno-unused-parameter"
)

# Also add compiler flag to suppress warnings in protobuf headers
target_compile_options(zerokv_common PRIVATE -Wno-unused-parameter)

# Build server library
add_library(zerokv_server SHARED ${SERVER_SRCS})
target_link_libraries(zerokv_server
    zerokv_common
    ${UCX_LIBRARIES}
    ${Protobuf_LIBRARIES}
    Threads::Threads
)
target_include_directories(zerokv_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_options(zerokv_server PRIVATE -Wno-unused-parameter)

# Add UCX dependency if building from source
if(NEED_UCX_DEPENDENCY)
    add_dependencies(zerokv_common ucx)
    add_dependencies(zerokv_server ucx)
    if(TARGET zerokv_client)
        add_dependencies(zerokv_client ucx)
    endif()
endif()

# Build client library (Task #9)
add_library(zerokv_client SHARED ${CLIENT_SRCS})
target_link_libraries(zerokv_client
    zerokv_common
    ${UCX_LIBRARIES}
    ${Protobuf_LIBRARIES}
    Threads::Threads
)
target_include_directories(zerokv_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_options(zerokv_client PRIVATE -Wno-unused-parameter)

# Install headers
install(DIRECTORY include/zerokv
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Install libraries
install(TARGETS zerokv_common zerokv_server zerokv_client
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Build tests
if(BUILD_TESTS)
    enable_testing()

    # Google Test
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/release-1.12.1.zip
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    # Unit tests
    file(GLOB TEST_SRCS tests/unit/*.cpp)
    foreach(test_src ${TEST_SRCS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/common
            ${CMAKE_CURRENT_BINARY_DIR}
        )
        target_compile_options(${test_name} PRIVATE -Wno-unused-parameter)

        # Link appropriate libraries based on test name
        if(test_name MATCHES "ucx_control_client")
            target_link_libraries(${test_name}
                zerokv_client
                zerokv_server
                zerokv_common
                GTest::gtest_main
            )
        elseif(test_name MATCHES "ucx_control_server|zerokv_server|zerokv_client")
            target_link_libraries(${test_name}
                zerokv_server
                zerokv_common
                GTest::gtest_main
            )
        else()
            target_link_libraries(${test_name}
                zerokv_common
                GTest::gtest_main
            )
        endif()

        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    # Integration tests
    file(GLOB INTEGRATION_TEST_SRCS tests/integration/*.cpp)
    foreach(test_src ${INTEGRATION_TEST_SRCS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/common
            ${CMAKE_CURRENT_BINARY_DIR}
        )
        target_compile_options(${test_name} PRIVATE -Wno-unused-parameter)
        target_link_libraries(${test_name}
            zerokv_server
            zerokv_client
            zerokv_common
            GTest::gtest_main
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    # Performance benchmark tests
    file(GLOB PERF_TEST_SRCS tests/perf/*.cpp)
    foreach(test_src ${PERF_TEST_SRCS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/common
            ${CMAKE_CURRENT_BINARY_DIR}
        )
        target_compile_options(${test_name} PRIVATE -Wno-unused-parameter)
        target_link_libraries(${test_name}
            zerokv_server
            zerokv_client
            zerokv_common
            GTest::gtest_main
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# Build examples (will be enabled in later milestones)
# if(BUILD_EXAMPLES)
#     add_executable(simple_server examples/simple_server.cpp)
#     target_link_libraries(simple_server zerokv_server)

#     add_executable(simple_client examples/simple_client.cpp)
#     target_link_libraries(simple_client zerokv_client)

#     add_executable(benchmark examples/benchmark.cpp)
#     target_link_libraries(benchmark zerokv_server zerokv_client)
# endif()

# Python bindings (will be enabled in Milestone #3)
# if(BUILD_PYTHON)
#     find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

#     # pybind11
#     find_package(pybind11 QUIET)
#     if(NOT pybind11_FOUND)
#         include(FetchContent)
#         FetchContent_Declare(
#             pybind11
#             URL https://github.com/pybind/pybind11/archive/v2.11.1.tar.gz
#         )
#         FetchContent_MakeAvailable(pybind11)
#     endif()

#     pybind11_add_module(zerokv_native
#         python/zerokv_python.cpp
#     )

#     target_link_libraries(zerokv_native PRIVATE
#         zerokv_server
#         zerokv_client
#     )

#     # Install Python module
#     install(TARGETS zerokv_native
#         LIBRARY DESTINATION python/zerokv
#     )
# endif()

# Print configuration summary
message(STATUS "")
message(STATUS "ZeroKV Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build tests: ${BUILD_TESTS}")
message(STATUS "  Build examples: ${BUILD_EXAMPLES}")
message(STATUS "  Build Python: ${BUILD_PYTHON}")
message(STATUS "  Use real NPU: ${USE_REAL_NPU}")
message(STATUS "  UCX libraries: ${UCX_LIBRARIES}")
message(STATUS "")
