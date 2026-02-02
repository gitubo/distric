# Test executables for distric_protocol

# Test: Binary Protocol
add_executable(test_binary test_binary.c)
target_link_libraries(test_binary distric_protocol distric_obs Threads::Threads)
add_test(NAME test_binary COMMAND test_binary)