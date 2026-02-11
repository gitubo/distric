# CMakeLists.txt for RAFT Test Framework
#
# This creates a reusable test framework library that can be linked
# by all RAFT test executables.

cmake_minimum_required(VERSION 3.10)

# Test Framework Library
# This library provides common test infrastructure for RAFT tests
add_library(raft_test_framework STATIC
    raft_test_framework.c
)

target_include_directories(raft_test_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libs/distric_raft/include
    ${CMAKE_SOURCE_DIR}/libs/distric_obs/include
    ${CMAKE_SOURCE_DIR}/libs/distric_protocol/include
    ${CMAKE_SOURCE_DIR}/libs/distric_transport/include
)

target_link_libraries(raft_test_framework
    distric_raft
    distric_protocol
    distric_transport
    distric_obs
    Threads::Threads
    m
)

# Example: Build one of the test suites using the framework
# (This assumes test_raft_higher_term.c has been updated to use the framework)
#if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_raft_higher_term.c)
#    add_executable(test_raft_higher_term test_raft_higher_term.c)
#    target_link_libraries(test_raft_higher_term
#        raft_test_framework
#        Threads::Threads
#    )
#    add_test(NAME test_raft_higher_term COMMAND test_raft_higher_term)
#endif()

# Add more test executables here as they are created/updated
# Each test just needs to link against raft_test_framework

# Example pattern for adding new tests:
# add_executable(test_raft_XXX test_raft_XXX.c)
# target_link_libraries(test_raft_XXX raft_test_framework Threads::Threads)
# add_test(NAME test_raft_XXX COMMAND test_raft_XXX)

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
target_link_libraries(test_raft_higher_term raft_test_framework distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_higher_term COMMAND test_raft_higher_term)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_leader_completeness test_raft_leader_completeness.c)
target_link_libraries(test_raft_leader_completeness raft_test_framework distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_leader_completeness COMMAND test_raft_leader_completeness)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_conflicting_logs test_raft_conflicting_logs.c)
target_link_libraries(test_raft_conflicting_logs raft_test_framework distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_conflicting_logs COMMAND test_raft_conflicting_logs)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_split_vote test_raft_split_vote.c)
target_link_libraries(test_raft_split_vote raft_test_framework distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_split_vote COMMAND test_raft_split_vote)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_leader_stability test_raft_leader_stability.c)
target_link_libraries(test_raft_leader_stability raft_test_framework distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_leader_stability COMMAND test_raft_leader_stability)

# Test: Multi-Node Cluster Integration (Session 3.6)
add_executable(test_raft_minority_no_leader test_raft_minority_no_leader.c)
target_link_libraries(test_raft_minority_no_leader raft_test_framework distric_raft distric_protocol distric_transport distric_obs Threads::Threads)
add_test(NAME test_raft_minority_no_leader COMMAND test_raft_minority_no_leader)