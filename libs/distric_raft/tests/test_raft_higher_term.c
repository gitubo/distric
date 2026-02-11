/**
 * @file test_raft_higher_term.c
 * @brief Test RAFT higher term handling
 *
 * Scenario:
 * A leader receives a RequestVote or AppendEntries message containing a higher
 * term than its own.
 *
 * Why Important:
 * Failing to step down on higher term breaks RAFT safety and can create
 * multiple leaders, violating the single-leader guarantee.
 *
 * Assertions:
 * - Leader transitions immediately to follower
 * - Local term is updated to the higher term
 * - Leader stops sending AppendEntries
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "raft.h"
#include "test_framework.h"

#define NUM_NODES 3
#define TEST_TIMEOUT_MS 5000

typedef struct {
    raft_node_t *nodes[NUM_NODES];
    test_cluster_t *cluster;
    uint64_t initial_term;
} test_context_t;

static test_context_t *setup_test(void) {
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    assert(ctx != NULL);
    
    // Create a cluster with 3 nodes
    ctx->cluster = test_cluster_create(NUM_NODES);
    assert(ctx->cluster != NULL);
    
    // Initialize nodes
    for (int i = 0; i < NUM_NODES; i++) {
        ctx->nodes[i] = test_cluster_get_node(ctx->cluster, i);
        assert(ctx->nodes[i] != NULL);
    }
    
    // Establish a leader (node 0)
    test_cluster_elect_leader(ctx->cluster, 0);
    
    // Verify node 0 is leader
    assert(raft_get_state(ctx->nodes[0]) == RAFT_STATE_LEADER);
    ctx->initial_term = raft_get_current_term(ctx->nodes[0]);
    
    return ctx;
}

static void teardown_test(test_context_t *ctx) {
    if (ctx) {
        test_cluster_destroy(ctx->cluster);
        free(ctx);
    }
}

/**
 * Test: Leader receives RequestVote with higher term
 */
static void test_higher_term_request_vote(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Leader receives RequestVote with higher term\n");
    
    raft_node_t *leader = ctx->nodes[0];
    uint64_t higher_term = ctx->initial_term + 5;
    
    // Create a RequestVote RPC with higher term
    raft_request_vote_req_t req = {
        .term = higher_term,
        .candidate_id = 2,  // From node 2
        .last_log_index = raft_get_last_log_index(leader),
        .last_log_term = raft_get_last_log_term(leader)
    };
    
    raft_request_vote_resp_t resp = {0};
    
    // Send RequestVote to leader
    int rv = raft_handle_request_vote(leader, &req, &resp);
    assert(rv == 0);
    
    // Assertions
    // 1. Leader must transition to follower
    assert(raft_get_state(leader) == RAFT_STATE_FOLLOWER);
    
    // 2. Local term must be updated to higher term
    assert(raft_get_current_term(leader) == higher_term);
    
    // 3. Vote should be granted (since logs are up to date)
    assert(resp.vote_granted == true);
    
    // 4. Leader must stop sending heartbeats
    // Allow time for any pending heartbeats to be sent
    test_cluster_tick(ctx->cluster, 100);
    
    // Verify no AppendEntries are sent from old leader
    uint32_t append_entries_count = test_cluster_count_messages(
        ctx->cluster, 0, RAFT_MSG_APPEND_ENTRIES
    );
    assert(append_entries_count == 0);
    
    printf("  ✓ Leader transitioned to follower on higher term\n");
    printf("  ✓ Term updated from %lu to %lu\n", ctx->initial_term, higher_term);
    printf("  ✓ No further AppendEntries sent\n");
    
    teardown_test(ctx);
}

/**
 * Test: Leader receives AppendEntries with higher term
 */
static void test_higher_term_append_entries(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Leader receives AppendEntries with higher term\n");
    
    raft_node_t *leader = ctx->nodes[0];
    uint64_t higher_term = ctx->initial_term + 3;
    
    // Create an AppendEntries RPC with higher term (empty heartbeat)
    raft_append_entries_req_t req = {
        .term = higher_term,
        .leader_id = 1,  // Pretend node 1 is new leader
        .prev_log_index = 0,
        .prev_log_term = 0,
        .leader_commit = 0,
        .entries = NULL,
        .n_entries = 0
    };
    
    raft_append_entries_resp_t resp = {0};
    
    // Send AppendEntries to leader
    int rv = raft_handle_append_entries(leader, &req, &resp);
    assert(rv == 0);
    
    // Assertions
    // 1. Leader must transition to follower
    assert(raft_get_state(leader) == RAFT_STATE_FOLLOWER);
    
    // 2. Local term must be updated
    assert(raft_get_current_term(leader) == higher_term);
    
    // 3. Response should indicate success
    assert(resp.success == true);
    
    // 4. Node should recognize node 1 as new leader
    assert(raft_get_current_leader_id(leader) == 1);
    
    printf("  ✓ Leader stepped down on higher term AppendEntries\n");
    printf("  ✓ Recognized new leader (node 1)\n");
    printf("  ✓ Term updated to %lu\n", higher_term);
    
    teardown_test(ctx);
}

/**
 * Test: Higher term with outdated log should still cause step down
 */
static void test_higher_term_outdated_log(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Higher term with outdated log causes step down\n");
    
    raft_node_t *leader = ctx->nodes[0];
    
    // Append some entries to leader's log
    for (int i = 0; i < 5; i++) {
        char data[32];
        snprintf(data, sizeof(data), "entry_%d", i);
        raft_entry_t entry = {
            .term = ctx->initial_term,
            .index = i + 1,
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader, &entry);
    }
    
    test_cluster_replicate_logs(ctx->cluster);
    
    uint64_t leader_log_index = raft_get_last_log_index(leader);
    uint64_t higher_term = ctx->initial_term + 2;
    
    // RequestVote with higher term but outdated log
    raft_request_vote_req_t req = {
        .term = higher_term,
        .candidate_id = 2,
        .last_log_index = 2,  // Behind leader's log
        .last_log_term = ctx->initial_term
    };
    
    raft_request_vote_resp_t resp = {0};
    
    int rv = raft_handle_request_vote(leader, &req, &resp);
    assert(rv == 0);
    
    // Assertions
    // 1. Must step down even with outdated log
    assert(raft_get_state(leader) == RAFT_STATE_FOLLOWER);
    
    // 2. Term must be updated
    assert(raft_get_current_term(leader) == higher_term);
    
    // 3. Vote should be denied (log is outdated)
    assert(resp.vote_granted == false);
    
    printf("  ✓ Stepped down despite having more complete log\n");
    printf("  ✓ Vote denied due to outdated candidate log\n");
    printf("  ✓ Term updated correctly\n");
    
    teardown_test(ctx);
}

/**
 * Test: Term update is persistent across restart
 */
static void test_higher_term_persistence(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Higher term update is persistent\n");
    
    raft_node_t *leader = ctx->nodes[0];
    uint64_t higher_term = ctx->initial_term + 10;
    
    // Receive higher term message
    raft_request_vote_req_t req = {
        .term = higher_term,
        .candidate_id = 1,
        .last_log_index = 0,
        .last_log_term = 0
    };
    
    raft_request_vote_resp_t resp = {0};
    raft_handle_request_vote(leader, &req, &resp);
    
    // Simulate restart
    test_cluster_restart_node(ctx->cluster, 0);
    leader = test_cluster_get_node(ctx->cluster, 0);
    
    // Verify term persisted
    assert(raft_get_current_term(leader) == higher_term);
    assert(raft_get_state(leader) == RAFT_STATE_FOLLOWER);
    
    printf("  ✓ Term update persisted across restart\n");
    printf("  ✓ State restored correctly\n");
    
    teardown_test(ctx);
}

/**
 * Test: Multiple higher term updates
 */
static void test_multiple_higher_terms(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Multiple sequential higher term updates\n");
    
    raft_node_t *node = ctx->nodes[0];
    uint64_t current_term = ctx->initial_term;
    
    // Simulate multiple term updates
    for (int i = 0; i < 5; i++) {
        uint64_t new_term = current_term + i + 1;
        
        raft_append_entries_req_t req = {
            .term = new_term,
            .leader_id = (i % 2) + 1,  // Alternate between nodes 1 and 2
            .prev_log_index = 0,
            .prev_log_term = 0,
            .leader_commit = 0,
            .entries = NULL,
            .n_entries = 0
        };
        
        raft_append_entries_resp_t resp = {0};
        raft_handle_append_entries(node, &req, &resp);
        
        // Verify term monotonically increases
        assert(raft_get_current_term(node) == new_term);
        assert(raft_get_current_term(node) > current_term);
        
        current_term = new_term;
    }
    
    printf("  ✓ Term increased monotonically through %d updates\n", 5);
    printf("  ✓ Final term: %lu\n", current_term);
    
    teardown_test(ctx);
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  RAFT Higher Term Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_init();
    
    test_higher_term_request_vote();
    printf("\n");
    
    test_higher_term_append_entries();
    printf("\n");
    
    test_higher_term_outdated_log();
    printf("\n");
    
    test_higher_term_persistence();
    printf("\n");
    
    test_multiple_higher_terms();
    printf("\n");
    
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  All Higher Term Tests Passed ✓\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_cleanup();
    return 0;
}