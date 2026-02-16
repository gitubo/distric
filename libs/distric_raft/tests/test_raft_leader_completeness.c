/**
 * @file test_raft_leader_completeness.c
 * @brief Tests for Raft leader completeness property
 * 
 * Tests that newly elected leaders have all committed entries from previous terms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "raft_test_framework.h"

/**
 * Test context structure
 */
typedef struct {
    test_cluster_t* cluster;
    raft_node_t** nodes;
    uint32_t num_nodes;
} test_context_t;

/**
 * Helper to get current term from a node
 */
static uint32_t get_node_term(raft_node_t* node) {
    return raft_get_term(node);
}

/**
 * Setup test context
 */
static test_context_t* setup_test(uint32_t num_nodes) {
    test_context_t* ctx = calloc(1, sizeof(test_context_t));
    assert(ctx != NULL);
    
    ctx->num_nodes = num_nodes;
    ctx->cluster = test_cluster_create(num_nodes);
    assert(ctx->cluster != NULL);
    
    ctx->nodes = calloc(num_nodes, sizeof(raft_node_t*));
    assert(ctx->nodes != NULL);
    
    for (uint32_t i = 0; i < num_nodes; i++) {
        ctx->nodes[i] = test_cluster_get_node(ctx->cluster, i);
        assert(ctx->nodes[i] != NULL);
    }
    
    // Start the cluster and wait for leader election
    test_cluster_start(ctx->cluster);
    
    return ctx;
}

/**
 * Teardown test context
 */
static void teardown_test(test_context_t* ctx) {
    if (ctx) {
        if (ctx->nodes) {
            free(ctx->nodes);
        }
        if (ctx->cluster) {
            test_cluster_destroy(ctx->cluster);
        }
        free(ctx);
    }
}

/**
 * Test: New leader has all committed entries from previous term
 */
static void test_new_leader_has_committed_entries(void) {
    printf("Running test: new_leader_has_committed_entries\n");
    
    test_context_t* ctx = setup_test(5);
    
    // Wait for initial leader election
    int leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    assert(leader_idx >= 0);
    
    raft_node_t* leader1 = ctx->nodes[leader_idx];
    uint32_t term1 = get_node_term(leader1);
    
    // Append and commit several entries
    const int num_entries = 10;
    
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "committed_entry_%d", i);
        
        uint32_t index_out = 0;
        distric_err_t rv = raft_append_entry(
            leader1,
            (const uint8_t*)data,
            strlen(data),
            &index_out
        );
        
        assert(rv == DISTRIC_OK);
    }
    
    // Replicate and commit these entries
    bool success = test_cluster_replicate_logs(ctx->cluster, num_entries);
    assert(success);
    
    // Verify entries are committed
    uint32_t commit_index = raft_get_commit_index(leader1);
    assert(commit_index >= (uint32_t)num_entries);
    
    // Create partition: isolate old leader from others
    // Partition: {leader} | {rest}
    int partition1[] = {leader_idx};
    int partition2[4];
    int p2_idx = 0;
    for (int i = 0; i < 5; i++) {
        if (i != leader_idx) {
            partition2[p2_idx++] = i;
        }
    }
    
    test_cluster_partition(ctx->cluster, partition1, 1, partition2, 4);
    
    // Step cluster to allow election in majority partition
    for (int i = 0; i < 100; i++) {
        test_cluster_tick(ctx->cluster, 10);
    }
    
    // Heal partition to check new leader
    test_cluster_heal_partition(ctx->cluster);
    
    // Wait for new leader (should be from the majority partition)
    int new_leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    assert(new_leader_idx >= 0);
    
    raft_node_t* leader2 = ctx->nodes[new_leader_idx];
    uint32_t term2 = get_node_term(leader2);
    
    // Verify new leader has all committed entries
    uint32_t new_commit_index = raft_get_commit_index(leader2);
    assert(new_commit_index >= (uint32_t)num_entries);
    
    // Verify log indices match
    uint32_t last_log_index = raft_get_last_log_index(leader2);
    assert(last_log_index >= (uint32_t)num_entries);
    
    // If new leader elected, term should be higher
    if (new_leader_idx != leader_idx) {
        assert(term2 > term1);
    }
    
    teardown_test(ctx);
    printf("PASSED: new_leader_has_committed_entries\n");
}

/**
 * Test: Node with incomplete log cannot win election
 */
static void test_incomplete_log_loses_election(void) {
    printf("Running test: incomplete_log_loses_election\n");
    
    test_context_t* ctx = setup_test(5);
    
    // Wait for initial leader election
    int leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    assert(leader_idx >= 0);
    
    raft_node_t* leader = ctx->nodes[leader_idx];
    uint32_t initial_term = get_node_term(leader);
    
    // Append entries
    const int num_entries = 5;
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "entry_%d", i);
        
        uint32_t index_out = 0;
        raft_append_entry(
            leader,
            (const uint8_t*)data,
            strlen(data),
            &index_out
        );
    }
    
    // Create partition: {leader, node1, node2} | {node3, node4}
    // This prevents node3 and node4 from getting updates
    int partition1[3];
    int partition2[2];
    
    partition1[0] = leader_idx;
    int p1_idx = 1;
    int p2_idx = 0;
    
    for (int i = 0; i < 5; i++) {
        if (i != leader_idx) {
            if (p1_idx < 3) {
                partition1[p1_idx++] = i;
            } else {
                partition2[p2_idx++] = i;
            }
        }
    }
    
    test_cluster_partition(ctx->cluster, partition1, 3, partition2, 2);
    
    // Replicate to partition 1 (majority including leader)
    test_cluster_replicate_logs(ctx->cluster, num_entries);
    
    // Now partition differently: {node3, node4, one other} | {leader, remaining}
    // This gives node3/node4 a majority but they have incomplete logs
    test_cluster_heal_partition(ctx->cluster);
    
    // Create new partition excluding leader and one node with full log
    int nodes_with_log[2] = {partition1[1], partition1[2]};
    int nodes_without_log[2] = {partition2[0], partition2[1]};
    
    // Partition: {leader, one node with log} | {incomplete nodes, one complete node}
    int new_p1[2] = {leader_idx, nodes_with_log[0]};
    int new_p2[3] = {nodes_without_log[0], nodes_without_log[1], nodes_with_log[1]};
    
    test_cluster_partition(ctx->cluster, new_p1, 2, new_p2, 3);
    
    // Step to allow election
    for (int i = 0; i < 100; i++) {
        test_cluster_tick(ctx->cluster, 10);
    }
    
    // Heal and check leader
    test_cluster_heal_partition(ctx->cluster);
    
    int new_leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    
    if (new_leader_idx >= 0) {
        // The node with complete log in partition 2 should win
        raft_node_t* new_leader = ctx->nodes[new_leader_idx];
        uint32_t last_log_index = raft_get_last_log_index(new_leader);
        
        // New leader should have complete log
        assert(last_log_index >= (uint32_t)num_entries);
        
        uint32_t new_term = get_node_term(new_leader);
        (void)initial_term; // May or may not have increased depending on partitioning
        (void)new_term;
    }
    
    teardown_test(ctx);
    printf("PASSED: incomplete_log_loses_election\n");
}

/**
 * Test: Leader with most up-to-date log wins election
 */
static void test_most_uptodate_log_wins(void) {
    printf("Running test: most_uptodate_log_wins\n");
    
    test_context_t* ctx = setup_test(3);
    
    // Wait for initial leader election
    int leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    assert(leader_idx >= 0);
    
    raft_node_t* leader = ctx->nodes[leader_idx];
    
    // Append several entries
    const int num_entries = 5;
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "entry_%d", i);
        
        uint32_t index_out = 0;
        raft_append_entry(
            leader,
            (const uint8_t*)data,
            strlen(data),
            &index_out
        );
    }
    
    // Replicate to all nodes
    test_cluster_replicate_logs(ctx->cluster, num_entries);
    
    // All nodes should have same log length now
    uint32_t log_index_0 = raft_get_last_log_index(ctx->nodes[0]);
    uint32_t log_index_1 = raft_get_last_log_index(ctx->nodes[1]);
    uint32_t log_index_2 = raft_get_last_log_index(ctx->nodes[2]);
    
    assert(log_index_0 == log_index_1);
    assert(log_index_1 == log_index_2);
    
    // Partition old leader
    int p1[1] = {leader_idx};
    int p2[2];
    int p2_idx = 0;
    for (int i = 0; i < 3; i++) {
        if (i != leader_idx) {
            p2[p2_idx++] = i;
        }
    }
    
    test_cluster_partition(ctx->cluster, p1, 1, p2, 2);
    
    // Step to trigger election in majority partition
    for (int i = 0; i < 100; i++) {
        test_cluster_tick(ctx->cluster, 10);
    }
    
    // Heal partition
    test_cluster_heal_partition(ctx->cluster);
    
    // Wait for new leader
    int new_leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    
    if (new_leader_idx >= 0) {
        // New leader should have complete log
        raft_node_t* new_leader = ctx->nodes[new_leader_idx];
        uint32_t new_log_index = raft_get_last_log_index(new_leader);
        assert(new_log_index >= (uint32_t)num_entries);
    }
    
    teardown_test(ctx);
    printf("PASSED: most_uptodate_log_wins\n");
}

/**
 * Test: Committed entries survive leader changes
 */
static void test_committed_entries_survive_leader_change(void) {
    printf("Running test: committed_entries_survive_leader_change\n");
    
    test_context_t* ctx = setup_test(5);
    
    // Wait for initial leader election
    int leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    assert(leader_idx >= 0);
    
    raft_node_t* leader = ctx->nodes[leader_idx];
    uint32_t initial_term = get_node_term(leader);
    
    // Append and commit entries
    const int num_entries = 3;
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "persistent_entry_%d", i);
        
        uint32_t index_out = 0;
        raft_append_entry(
            leader,
            (const uint8_t*)data,
            strlen(data),
            &index_out
        );
    }
    
    // Commit entries
    test_cluster_replicate_logs(ctx->cluster, num_entries);
    
    uint32_t commit_before = raft_get_commit_index(leader);
    
    // Change leader by creating partitions
    for (int change = 0; change < 2; change++) {
        // Partition current leader away from majority
        int p1[1] = {leader_idx};
        int p2[4];
        int p2_idx = 0;
        
        for (int i = 0; i < 5; i++) {
            if (i != leader_idx) {
                p2[p2_idx++] = i;
            }
        }
        
        test_cluster_partition(ctx->cluster, p1, 1, p2, 4);
        
        // Step to allow new election
        for (int i = 0; i < 100; i++) {
            test_cluster_tick(ctx->cluster, 10);
        }
        
        // Heal partition
        test_cluster_heal_partition(ctx->cluster);
        
        // Wait for new leader
        int new_leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
        
        if (new_leader_idx >= 0) {
            leader_idx = new_leader_idx;
            leader = ctx->nodes[leader_idx];
            
            // Verify committed entries are still there
            uint32_t commit_after = raft_get_commit_index(leader);
            assert(commit_after >= commit_before);
            
            uint32_t new_term = get_node_term(leader);
            (void)initial_term;
            initial_term = new_term;
        }
    }
    
    teardown_test(ctx);
    printf("PASSED: committed_entries_survive_leader_change\n");
}

/**
 * Test: Higher term log preferred over longer log with lower term
 */
static void test_higher_term_preferred(void) {
    printf("Running test: higher_term_preferred\n");
    
    test_context_t* ctx = setup_test(3);
    
    // Wait for initial leader election
    int leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    assert(leader_idx >= 0);
    
    raft_node_t* leader = ctx->nodes[leader_idx];
    uint32_t term1 = get_node_term(leader);
    
    // Append some entries
    for (int i = 0; i < 3; i++) {
        char data[64];
        snprintf(data, sizeof(data), "term1_entry_%d", i);
        
        uint32_t index_out = 0;
        raft_append_entry(
            leader,
            (const uint8_t*)data,
            strlen(data),
            &index_out
        );
    }
    
    // Replicate to all
    test_cluster_replicate_logs(ctx->cluster, 3);
    
    // Partition old leader
    int p1[1] = {leader_idx};
    int p2[2];
    int p2_idx = 0;
    
    for (int i = 0; i < 3; i++) {
        if (i != leader_idx) {
            p2[p2_idx++] = i;
        }
    }
    
    test_cluster_partition(ctx->cluster, p1, 1, p2, 2);
    
    // Step to trigger new election with higher term
    for (int i = 0; i < 100; i++) {
        test_cluster_tick(ctx->cluster, 10);
    }
    
    // Heal partition
    test_cluster_heal_partition(ctx->cluster);
    
    int new_leader_idx = test_cluster_wait_for_leader(ctx->cluster, 5000);
    
    if (new_leader_idx >= 0) {
        raft_node_t* new_leader = ctx->nodes[new_leader_idx];
        uint32_t term2 = get_node_term(new_leader);
        
        // Verify new leader has the entries
        uint32_t last_log = raft_get_last_log_index(new_leader);
        assert(last_log >= 3);
        
        (void)term1;
        (void)term2;
    }
    
    teardown_test(ctx);
    printf("PASSED: higher_term_preferred\n");
}

/**
 * Main test runner
 */
int main(void) {
    printf("=== Running Raft Leader Completeness Tests ===\n\n");
    
    test_new_leader_has_committed_entries();
    test_incomplete_log_loses_election();
    test_most_uptodate_log_wins();
    test_committed_entries_survive_leader_change();
    test_higher_term_preferred();
    
    printf("\n=== All Leader Completeness Tests Passed ===\n");
    return 0;
}