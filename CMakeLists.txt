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

# UCX
find_path(UCX_INCLUDE_DIR ucp/api/ucp.h
    HINTS /usr/include /usr/local/include)
find_library(UCX_UCP_LIBRARY ucp
    HINTS /usr/lib /usr/local/lib)
find_library(UCX_UCS_LIBRARY ucs
    HINTS /usr/lib /usr/local/lib)

if(UCX_INCLUDE_DIR AND UCX_UCP_LIBRARY AND UCX_UCS_LIBRARY)
    message(STATUS "Found UCX: ${UCX_INCLUDE_DIR}")
    set(UCX_LIBRARIES ${UCX_UCP_LIBRARY} ${UCX_UCS_LIBRARY})
    include_directories(${UCX_INCLUDE_DIR})
else()
    message(FATAL_ERROR "UCX not found. Please install UCX library.")
endif()

# Protobuf
find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})

# Generate protobuf files
set(PROTO_FILES
    ${PROJECT_SOURCE_DIR}/protos/zerokv.proto
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

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
    src/common/ucx_control_server.cpp
    src/common/ucx_control_client.cpp
    ${PROTO_SRCS}
)

# Server source files
set(SERVER_SRCS
    src/server/zerokv_server.cpp
    src/monitor/performance_monitor.cpp
)

# Client source files
set(CLIENT_SRCS
    src/client/zerokv_client.cpp
)

# Build shared library
add_library(zerokv_common SHARED ${COMMON_SRCS})
target_link_libraries(zerokv_common
    ${UCX_LIBRARIES}
    ${Protobuf_LIBRARIES}
    Threads::Threads
)

add_library(zerokv_server SHARED ${SERVER_SRCS})
target_link_libraries(zerokv_server
    zerokv_common
    ${NPU_LIBRARIES}
)

add_library(zerokv_client SHARED ${CLIENT_SRCS})
target_link_libraries(zerokv_client
    zerokv_common
    ${NPU_LIBRARIES}
)

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
        target_link_libraries(${test_name}
            zerokv_server
            zerokv_client
            GTest::gtest_main
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    # Integration tests
    file(GLOB INTEGRATION_TEST_SRCS tests/integration/*.cpp)
    foreach(test_src ${INTEGRATION_TEST_SRCS})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name}
            zerokv_server
            zerokv_client
            GTest::gtest_main
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# Build examples
if(BUILD_EXAMPLES)
    add_executable(simple_server examples/simple_server.cpp)
    target_link_libraries(simple_server zerokv_server)

    add_executable(simple_client examples/simple_client.cpp)
    target_link_libraries(simple_client zerokv_client)

    add_executable(benchmark examples/benchmark.cpp)
    target_link_libraries(benchmark zerokv_server zerokv_client)
endif()

# Python bindings
if(BUILD_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

    # pybind11
    find_package(pybind11 QUIET)
    if(NOT pybind11_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            pybind11
            URL https://github.com/pybind/pybind11/archive/v2.11.1.tar.gz
        )
        FetchContent_MakeAvailable(pybind11)
    endif()

    pybind11_add_module(zerokv_native
        python/zerokv_python.cpp
    )

    target_link_libraries(zerokv_native PRIVATE
        zerokv_server
        zerokv_client
    )

    # Install Python module
    install(TARGETS zerokv_native
        LIBRARY DESTINATION python/zerokv
    )
endif()

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
