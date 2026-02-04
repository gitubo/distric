# Test executables for distric_raft

# Test: Basic Raft functionality
add_executable(test_raft_basic test_raft_basic.c)
target_link_libraries(test_raft_basic distric_raft distric_protocol distric_obs Threads::Threads)
add_test(NAME test_raft_basic COMMAND test_raft_basic)

# Test: RPC Raft functionality
add_executable(test_raft_rpc test_raft_rpc.c)
target_link_libraries(test_raft_rpc distric_raft distric_protocol distric_obs Threads::Threads)
add_test(NAME test_raft_rpc COMMAND test_raft_rpc)

# Test: RPC Replication
add_executable(test_raft_replication test_raft_replication.c)
target_link_libraries(test_raft_replication distric_raft distric_protocol distric_obs Threads::Threads)
add_test(NAME test_raft_replication COMMAND test_raft_replication)