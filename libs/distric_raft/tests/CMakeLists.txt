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

# Test: Persistence
add_executable(test_raft_persistence test_raft_persistence.c)
target_link_libraries(test_raft_persistence distric_raft distric_protocol distric_obs Threads::Threads)
add_test(NAME test_raft_persistence COMMAND test_raft_persistence)

# Test: Snapshot
add_executable(test_raft_snapshot test_raft_snapshot.c)
target_link_libraries(test_raft_snapshot distric_raft distric_protocol distric_obs Threads::Threads)
add_test(NAME test_raft_snapshot COMMAND test_raft_snapshot)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_cluster test_raft_cluster.c)
target_link_libraries(test_raft_cluster distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_cluster COMMAND test_raft_cluster)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_higher_term test_raft_higher_term.c)
target_link_libraries(test_raft_higher_term distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_higher_term COMMAND test_raft_higher_term)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_leader_completeness test_raft_leader_completeness.c)
target_link_libraries(test_raft_leader_completeness distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_leader_completeness COMMAND test_raft_leader_completeness)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_conflicting_logs test_raft_conflicting_logs.c)
target_link_libraries(test_raft_conflicting_logs distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_conflicting_logs COMMAND test_raft_conflicting_logs)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_split_vote test_raft_split_vote.c)
target_link_libraries(test_raft_split_vote distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_split_vote COMMAND test_raft_split_vote)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_leader_stability test_raft_leader_stability.c)
target_link_libraries(test_raft_leader_stability distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_leader_stability COMMAND test_raft_leader_stability)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_minority_no_leader test_raft_minority_no_leader.c)
target_link_libraries(test_raft_minority_no_leader distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_minority_no_leader COMMAND test_raft_minority_no_leader)