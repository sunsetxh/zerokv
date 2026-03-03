cmake_minimum_required(VERSION 3.15)
project(MyApp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find ZeroKV package
# Adjust the path if ZeroKV is installed in a non-standard location
# set(zerokv_DIR "/path/to/zerokv/build/lib/cmake/zerokv")
find_package(zerokv REQUIRED)

# Create executable
add_executable(my_app main.cc)

# Link against ZeroKV
target_link_libraries(my_app PRIVATE zerokv::zerokv)

# Include directories
target_include_directories(my_app PRIVATE ${zerokv_INCLUDE_DIRS})
