# Test executables for distric_gossip

# Test: Binary Protocol
add_executable(test_gossip_basic test_gossip_basic.c)
target_link_libraries(test_gossip_basic distric_gossip distric_obs Threads::Threads)
add_test(NAME test_gossip_basic COMMAND test_gossip_basic)
