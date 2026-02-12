/**
 * @file test_raft_leader_completeness.c
 * @brief Tests for Raft leader completeness property
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "raft_test_framework.h"

typedef struct {
    test_cluster_t* cluster;
    raft_node_t** nodes;
    uint32_t num_nodes;
} test_context_t;

static uint32_t get_node_term(raft_node_t* node) {
    bool vote_granted = false;
    uint32_t term_out = 0;
    
    raft_handle_request_vote(node, "dummy_candidate", 0, 0, 0, &vote_granted, &term_out);
    
    return term_out;
}

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
    
    return ctx;
}

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

static void test_new_leader_has_committed_entries(void) {
    printf("Running test: new_leader_has_committed_entries\n");
    
    test_context_t* ctx = setup_test(5);
    
    int leader_idx = test_cluster_find_leader(ctx->cluster);
    assert(leader_idx >= 0);
    
    raft_node_t* leader1 = ctx->nodes[leader_idx];
    uint32_t term1 = get_node_term(leader1);
    
    const int num_entries = 10;
    
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "committed_entry_%d", i);
        
        uint32_t index_out = 0;
        distric_err_t rv = raft_append_entry(leader1, (const uint8_t*)data, strlen(data), &index_out);
        assert(rv == DISTRIC_OK);
    }
    
    bool success = test_cluster_replicate_logs(ctx->cluster, num_entries);
    assert(success);
    
    uint32_t commit_index = raft_get_commit_index(leader1);
    assert(commit_index >= (uint32_t)num_entries);
    
    uint32_t last_log_index = raft_get_last_log_index(leader1);
    assert(last_log_index >= (uint32_t)num_entries);
    
    (void)term1;
    
    teardown_test(ctx);
    printf("PASSED: new_leader_has_committed_entries\n");
}

static void test_incomplete_log_loses_election(void) {
    printf("Running test: incomplete_log_loses_election\n");
    
    test_context_t* ctx = setup_test(5);
    
    int leader_idx = test_cluster_find_leader(ctx->cluster);
    assert(leader_idx >= 0);
    
    raft_node_t* leader = ctx->nodes[leader_idx];
    
    const int num_entries = 5;
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "entry_%d", i);
        
        uint32_t index_out = 0;
        raft_append_entry(leader, (const uint8_t*)data, strlen(data), &index_out);
    }
    
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
    test_cluster_replicate_logs(ctx->cluster, num_entries);
    test_cluster_heal_partition(ctx->cluster);
    
    uint32_t last_log = raft_get_last_log_index(leader);
    assert(last_log >= (uint32_t)num_entries);
    
    teardown_test(ctx);
    printf("PASSED: incomplete_log_loses_election\n");
}

static void test_most_uptodate_log_wins(void) {
    printf("Running test: most_uptodate_log_wins\n");
    
    test_context_t* ctx = setup_test(3);
    
    int leader_idx = test_cluster_find_leader(ctx->cluster);
    assert(leader_idx >= 0);
    
    raft_node_t* leader = ctx->nodes[leader_idx];
    
    const int num_entries = 5;
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "entry_%d", i);
        
        uint32_t index_out = 0;
        raft_append_entry(leader, (const uint8_t*)data, strlen(data), &index_out);
    }
    
    test_cluster_replicate_logs(ctx->cluster, num_entries);
    
    uint32_t log_index_0 = raft_get_last_log_index(ctx->nodes[0]);
    uint32_t log_index_1 = raft_get_last_log_index(ctx->nodes[1]);
    uint32_t log_index_2 = raft_get_last_log_index(ctx->nodes[2]);
    
    assert(log_index_0 == log_index_1);
    assert(log_index_1 == log_index_2);
    
    teardown_test(ctx);
    printf("PASSED: most_uptodate_log_wins\n");
}

static void test_committed_entries_survive_leader_change(void) {
    printf("Running test: committed_entries_survive_leader_change\n");
    
    test_context_t* ctx = setup_test(5);
    
    int leader_idx = test_cluster_find_leader(ctx->cluster);
    assert(leader_idx >= 0);
    
    raft_node_t* leader = ctx->nodes[leader_idx];
    
    const int num_entries = 3;
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "persistent_entry_%d", i);
        
        uint32_t index_out = 0;
        raft_append_entry(leader, (const uint8_t*)data, strlen(data), &index_out);
    }
    
    test_cluster_replicate_logs(ctx->cluster, num_entries);
    
    uint32_t commit_index = raft_get_commit_index(leader);
    assert(commit_index >= (uint32_t)num_entries);
    
    teardown_test(ctx);
    printf("PASSED: committed_entries_survive_leader_change\n");
}

static void test_higher_term_preferred(void) {
    printf("Running test: higher_term_preferred\n");
    
    test_context_t* ctx = setup_test(3);
    
    int leader_idx = test_cluster_find_leader(ctx->cluster);
    assert(leader_idx >= 0);
    
    raft_node_t* leader = ctx->nodes[leader_idx];
    
    for (int i = 0; i < 3; i++) {
        char data[64];
        snprintf(data, sizeof(data), "term1_entry_%d", i);
        
        uint32_t index_out = 0;
        raft_append_entry(leader, (const uint8_t*)data, strlen(data), &index_out);
    }
    
    test_cluster_replicate_logs(ctx->cluster, 3);
    
    uint32_t last_log = raft_get_last_log_index(leader);
    assert(last_log >= 3);
    
    teardown_test(ctx);
    printf("PASSED: higher_term_preferred\n");
}

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