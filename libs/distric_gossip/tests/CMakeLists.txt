# Test executables for distric_gossip

# Test: Binary Protocol
add_executable(test_gossip test_gossip.c)
target_link_libraries(test_gossip distric_obs Threads::Threads)
add_test(NAME test_gossip COMMAND test_gossip)
