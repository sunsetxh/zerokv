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
# UCX (Unified Communication X)
# - Try system UCX first
# - If not found and BUILD_UCX_FROM_SOURCE=ON, build from source
# - Otherwise, use stub for development
#==============================================================================

option(USE_UCX_STUB "Use UCX stub implementation instead of real UCX" OFF)
option(BUILD_UCX_FROM_SOURCE "Download and build UCX from source if not found" OFF)

# Try to find system UCX first
find_path(UCX_INCLUDE_DIR ucp/api/ucp.h
    HINTS /usr/local/include /usr/include
    PATH_SUFFIXES ucx)

find_library(UCX_UCP_LIBRARY ucp
    HINTS /usr/local/lib /usr/lib /usr/local/lib64 /usr/lib64)

find_library(UCX_UCS_LIBRARY ucs
    HINTS /usr/local/lib /usr/lib /usr/local/lib64 /usr/lib64)

if(UCX_INCLUDE_DIR AND UCX_UCP_LIBRARY AND UCX_UCS_LIBRARY AND NOT USE_UCX_STUB)
    message(STATUS "Found system UCX: ${UCX_INCLUDE_DIR}")
    set(UCX_LIBRARIES ${UCX_UCP_LIBRARY} ${UCX_UCS_LIBRARY})
    set(UCX_INCLUDE_DIRS ${UCX_INCLUDE_DIR})
    set(HAVE_REAL_UCX ON)
else()
    # System UCX not found
    if(BUILD_UCX_FROM_SOURCE AND NOT USE_UCX_STUB)
        message(STATUS "System UCX not found. Building UCX from source...")
        message(STATUS "Note: This may take 5-10 minutes and requires build tools (autoconf, libtool, etc.)")
        include(FetchContent)

        # UCX 1.20.0 release
        FetchContent_Declare(
            ucx
            URL https://github.com/openucx/ucx/releases/download/1.20.0/ucx-1.20.0.tar.gz
            URL_HASH SHA256=d1300a4b58e8bb41756dc6f78bbf85bb4baa7bc56208cb39a1bbfbbde2d3c49d
        )

        # UCX build configuration
        set(INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/ucx-install)
        set(UCX_SRC_DIR ${ucx_SOURCE_DIR})

        # Configure UCX build
        set(ucx_configure_args
            --prefix=${INSTALL_DIR}
            --enable-shared
            --disable-static
            --disable-debug
            --disable-params-check
            --disable-assertions
            --disable-mt
            --enable-cma
            --enable-rdm
            --enable-mm
            --enable-ud
            --disable-async
            --disable-perftest
            --disable-examples
            --disable-tests
            --disable-fortran
            --disable-java
        )

        # Build UCX
        FetchContent_GetProperties(ucx)
        if(NOT ucx_POPULATED)
            FetchContent_Populate(ucx)

            # Autogen
            execute_process(
                COMMAND ${UCX_SRC_DIR}/autogen.sh
                WORKING_DIRECTORY ${UCX_SRC_DIR}
                RESULT_VARIABLE autogen_result
            )

            # Configure
            execute_process(
                COMMAND ${UCX_SRC_DIR}/configure ${ucx_configure_args}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ucx-build
                RESULT_VARIABLE config_result
            )

            # Build and install
            execute_process(
                COMMAND make -j${CMAKE_BUILD_PARALLEL_LEVEL} install
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ucx-build
                RESULT_VARIABLE build_result
            )

            if(NOT build_result EQUAL 0)
                message(WARNING "UCX build failed. Falling back to stub implementation.")
                set(USE_UCX_STUB ON)
            else()
                # Set UCX variables from installed build
                set(UCX_INCLUDE_DIRS ${INSTALL_DIR}/include)
                find_library(UCX_UCP_LIB ucp HINTS ${INSTALL_DIR}/lib)
                find_library(UCX_UCS_LIB ucs HINTS ${INSTALL_DIR}/lib)
                set(UCX_LIBRARIES ${UCX_UCP_LIB} ${UCX_UCS_LIB})
                set(HAVE_REAL_UCX ON)
                message(STATUS "Built UCX from source: ${INSTALL_DIR}")
            endif()
        endif()
    else()
        # Not building from source, use stub
        message(STATUS "Using UCX stub implementation (set BUILD_UCX_FROM_SOURCE=ON to build UCX)")
        set(USE_UCX_STUB ON)
    endif()

    # Fall back to stub if UCX build failed or explicitly requested
    if(USE_UCX_STUB OR NOT HAVE_REAL_UCX)
        message(STATUS "Using UCX stub implementation for development")
        set(UCX_LIBRARIES "")
        set(UCX_INCLUDE_DIRS "")
        add_definitions(-DUSE_UCX_STUB)
        set(HAVE_REAL_UCX OFF)
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
    # src/common/ucx_control_client.cpp  # Task #9 - TODO
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

# Client library will be built in Task #9
# add_library(zerokv_client SHARED ${CLIENT_SRCS})
# target_link_libraries(zerokv_client
#     zerokv_common
#     ${NPU_LIBRARIES}
# )

# Install headers
install(DIRECTORY include/zerokv
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Install libraries
install(TARGETS zerokv_common
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
# install(TARGETS zerokv_server zerokv_client  # Will be enabled in later milestones
#     LIBRARY DESTINATION lib
#     ARCHIVE DESTINATION lib
# )

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
        if(test_name MATCHES "ucx_control_server|ucx_control_client|zerokv_server|zerokv_client")
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

    # Integration tests (will be enabled in later milestones)
    # file(GLOB INTEGRATION_TEST_SRCS tests/integration/*.cpp)
    # foreach(test_src ${INTEGRATION_TEST_SRCS})
    #     get_filename_component(test_name ${test_src} NAME_WE)
    #     add_executable(${test_name} ${test_src})
    #     target_link_libraries(${test_name}
    #         zerokv_server
    #         zerokv_client
    #         GTest::gtest_main
    #     )
    #     add_test(NAME ${test_name} COMMAND ${test_name})
    # endforeach()
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
