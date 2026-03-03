add_executable(benchmark
    benchmark.cc
)

target_link_libraries(benchmark
    zerokv
    pthread
)

install(TARGETS benchmark DESTINATION bin)
