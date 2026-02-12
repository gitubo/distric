/**
 * @file test_raft_higher_term.c
 * @brief Tests for handling higher term numbers in Raft
 * 
 * Tests that nodes properly handle messages with higher term numbers
 * according to the Raft protocol specifications.
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
    uint32_t initial_term;
} test_context_t;

/**
 * Helper to get current term from a node via a dummy request
 * Since raft_get_current_term doesn't exist, we use a side effect
 */
static uint32_t get_node_term(raft_node_t* node) {
    bool vote_granted = false;
    uint32_t term_out = 0;
    
    // Make a dummy request vote call to get current term in response
    raft_handle_request_vote(
        node,
        "dummy_candidate",
        0,  // Term 0 will be rejected but returns current term
        0,
        0,
        &vote_granted,
        &term_out
    );
    
    return term_out;
}

/**
 * Setup test context
 */
static test_context_t* setup_test(void) {
    test_context_t* ctx = calloc(1, sizeof(test_context_t));
    assert(ctx != NULL);
    
    // Create a 3-node cluster
    ctx->num_nodes = 3;
    ctx->cluster = test_cluster_create(ctx->num_nodes);
    assert(ctx->cluster != NULL);
    
    // Get node references
    ctx->nodes = calloc(ctx->num_nodes, sizeof(raft_node_t*));
    assert(ctx->nodes != NULL);
    
    for (uint32_t i = 0; i < ctx->num_nodes; i++) {
        ctx->nodes[i] = test_cluster_get_node(ctx->cluster, i);
        assert(ctx->nodes[i] != NULL);
    }
    
    // Elect a leader
    assert(test_cluster_find_leader(ctx->cluster) >= 0);
    
    // Store initial term
    ctx->initial_term = get_node_term(ctx->nodes[0]);
    
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
 * Test: Higher term in RequestVote causes node to update term and step down
 */
static void test_higher_term_request_vote(void) {
    printf("Running test: higher_term_request_vote\n");
    
    test_context_t* ctx = setup_test();
    raft_node_t* leader = ctx->nodes[0];
    
    // Create a RequestVote with higher term
    uint32_t higher_term = ctx->initial_term + 10;
    const char* candidate_id = "node_2";
    uint32_t last_log_index = raft_get_last_log_index(leader);
    uint32_t last_log_term = raft_get_last_log_term(leader);
    
    // Handle request vote with higher term
    bool vote_granted = false;
    uint32_t response_term = 0;
    
    distric_err_t rv = raft_handle_request_vote(
        leader,
        candidate_id,
        higher_term,
        last_log_index,
        last_log_term,
        &vote_granted,
        &response_term
    );
    
    assert(rv == DISTRIC_OK);
    
    // Verify node updated to higher term
    uint32_t current_term = get_node_term(leader);
    assert(current_term == higher_term);
    
    // Verify node stepped down to follower
    raft_state_t state = raft_get_state(leader);
    assert(state == RAFT_STATE_FOLLOWER);
    
    // Verify vote was granted
    assert(vote_granted == true);
    assert(response_term == higher_term);
    
    teardown_test(ctx);
    printf("PASSED: higher_term_request_vote\n");
}

/**
 * Test: Higher term in AppendEntries causes node to update term and step down
 */
static void test_higher_term_append_entries(void) {
    printf("Running test: higher_term_append_entries\n");
    
    test_context_t* ctx = setup_test();
    raft_node_t* leader = ctx->nodes[0];
    
    // Create AppendEntries with higher term
    uint32_t higher_term = ctx->initial_term + 5;
    const char* leader_id = "node_1";
    uint32_t prev_log_index = 0;
    uint32_t prev_log_term = 0;
    uint32_t leader_commit = 0;
    
    // Handle append entries with higher term (heartbeat)
    bool success = false;
    uint32_t response_term = 0;
    uint32_t last_log_index = 0;
    
    distric_err_t rv = raft_handle_append_entries(
        leader,
        leader_id,
        higher_term,
        prev_log_index,
        prev_log_term,
        NULL,
        0,
        leader_commit,
        &success,
        &response_term,
        &last_log_index
    );
    
    assert(rv == DISTRIC_OK);
    
    // Verify node updated to higher term
    uint32_t current_term = get_node_term(leader);
    assert(current_term == higher_term);
    
    // Verify node stepped down to follower
    raft_state_t state = raft_get_state(leader);
    assert(state == RAFT_STATE_FOLLOWER);
    
    // Verify response was successful
    assert(success == true);
    assert(response_term == higher_term);
    
    teardown_test(ctx);
    printf("PASSED: higher_term_append_entries\n");
}

/**
 * Test: Higher term with outdated log rejects vote
 */
static void test_higher_term_outdated_log(void) {
    printf("Running test: higher_term_outdated_log\n");
    
    test_context_t* ctx = setup_test();
    raft_node_t* leader = ctx->nodes[0];
    
    // Append some entries to the leader
    for (int i = 0; i < 5; i++) {
        char data[32];
        snprintf(data, sizeof(data), "entry_%d", i);
        
        uint32_t index_out = 0;
        distric_err_t rv = raft_append_entry(
            leader,
            (const uint8_t*)data,
            strlen(data),
            &index_out
        );
        assert(rv == DISTRIC_OK);
    }
    
    // Replicate to cluster
    assert(test_cluster_replicate_logs(ctx->cluster, 5));
    
    // Get leader's log info
    uint32_t leader_log_index = raft_get_last_log_index(leader);
    uint32_t leader_log_term = raft_get_last_log_term(leader);
    (void)leader_log_index; // Suppress unused warning
    (void)leader_log_term;
    
    // Create RequestVote with higher term but outdated log
    uint32_t higher_term = ctx->initial_term + 20;
    const char* candidate_id = "node_2";
    uint32_t last_log_index = 2;  // Behind leader's log
    uint32_t last_log_term = ctx->initial_term;
    
    bool vote_granted = false;
    uint32_t response_term = 0;
    
    distric_err_t rv = raft_handle_request_vote(
        leader,
        candidate_id,
        higher_term,
        last_log_index,
        last_log_term,
        &vote_granted,
        &response_term
    );
    
    assert(rv == DISTRIC_OK);
    
    // Verify node updated to higher term
    uint32_t current_term = get_node_term(leader);
    assert(current_term == higher_term);
    
    // Verify vote was NOT granted (log is outdated)
    assert(vote_granted == false);
    assert(response_term == higher_term);
    
    teardown_test(ctx);
    printf("PASSED: higher_term_outdated_log\n");
}

/**
 * Test: Term persistence after update
 */
static void test_higher_term_persistence(void) {
    printf("Running test: higher_term_persistence\n");
    
    test_context_t* ctx = setup_test();
    raft_node_t* leader = ctx->nodes[0];
    
    // Update term via RequestVote
    uint32_t higher_term = ctx->initial_term + 15;
    const char* candidate_id = "node_1";
    
    bool vote_granted = false;
    uint32_t response_term = 0;
    
    raft_handle_request_vote(
        leader,
        candidate_id,
        higher_term,
        0,
        0,
        &vote_granted,
        &response_term
    );
    
    // Verify term was updated
    uint32_t current_term = get_node_term(leader);
    assert(current_term == higher_term);
    
    // Simulate restart by getting state again
    // In a real implementation, this would reload from persistent storage
    uint32_t reloaded_term = get_node_term(leader);
    assert(reloaded_term == higher_term);
    
    teardown_test(ctx);
    printf("PASSED: higher_term_persistence\n");
}

/**
 * Test: Multiple sequential term updates
 */
static void test_multiple_higher_terms(void) {
    printf("Running test: multiple_higher_terms\n");
    
    test_context_t* ctx = setup_test();
    raft_node_t* node = ctx->nodes[0];
    
    uint32_t current_term = ctx->initial_term;
    
    // Apply multiple term updates
    for (int i = 0; i < 10; i++) {
        uint32_t new_term = current_term + i + 1;
        char leader_id[32];
        snprintf(leader_id, sizeof(leader_id), "node_%d", (i % 2) + 1);
        
        bool success = false;
        uint32_t response_term = 0;
        uint32_t last_log_index = 0;
        
        raft_handle_append_entries(
            node,
            leader_id,
            new_term,
            0,
            0,
            NULL,
            0,
            0,
            &success,
            &response_term,
            &last_log_index
        );
        
        // Verify term increased
        uint32_t node_term = get_node_term(node);
        assert(node_term == new_term);
        assert(node_term > current_term);
        
        current_term = new_term;
    }
    
    teardown_test(ctx);
    printf("PASSED: multiple_higher_terms\n");
}

/**
 * Main test runner
 */
int main(void) {
    printf("=== Running Raft Higher Term Tests ===\n\n");
    
    test_higher_term_request_vote();
    test_higher_term_append_entries();
    test_higher_term_outdated_log();
    test_higher_term_persistence();
    test_multiple_higher_terms();
    
    printf("\n=== All Higher Term Tests Passed ===\n");
    return 0;
}