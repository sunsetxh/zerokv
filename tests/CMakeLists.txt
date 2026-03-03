find_package(GTest REQUIRED)

# Unit tests
add_executable(test_storage
    unit/test_storage.cc
)

target_link_libraries(test_storage
    zerokv
    GTest::gtest
    GTest::gtest_main
    pthread
)

include(GoogleTest)
gtest_discover_tests(test_storage)

install(TARGETS test_storage DESTINATION bin/tests)
