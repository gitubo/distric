# Test executables for distric_cluster

# Test: Cluster Integration
add_executable(test_cluster_integration test_cluster_integration.c)
target_link_libraries(test_cluster_integration 
    distric_cluster
    distric_raft
    distric_gossip
    distric_obs
    Threads::Threads
)
add_test(NAME test_cluster_integration COMMAND test_cluster_integration)