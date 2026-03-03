# Examples
add_executable(example_basic
    example_basic.cc
)
target_link_libraries(example_basic
    zerokv
)

# Embedded Server Example
add_executable(embedded_server
    embedded_server.cc
)
target_link_libraries(embedded_server
    zerokv
)

# Embedded Client Example
add_executable(embedded_client
    embedded_client.cc
)
target_link_libraries(embedded_client
    zerokv
)
