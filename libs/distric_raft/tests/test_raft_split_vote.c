/**
 * @file test_raft_split_vote.c
 * @brief Test RAFT split vote handling
 *
 * Scenario:
 * Multiple candidates start elections simultaneously, causing votes to be
 * split such that no candidate achieves majority. System must eventually
 * elect a leader through randomized timeouts.
 *
 * Why Important:
 * Split votes are common in real deployments and can cause livelock if not
 * handled correctly. Proper randomization ensures eventual progress.
 *
 * Assertions:
 * - No leader is elected in the first election round
 * - A new election is eventually triggered
 * - Exactly one leader is elected eventually
 * - Term number increases monotonically
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "raft.h"
#include "test_framework.h"

#define NUM_NODES 5
#define TEST_TIMEOUT_MS 30000  // Generous timeout for multiple election rounds

typedef struct {
    raft_node_t *nodes[NUM_NODES];
    test_cluster_t *cluster;
} test_context_t;

static test_context_t *setup_test(void) {
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    assert(ctx != NULL);
    
    ctx->cluster = test_cluster_create(NUM_NODES);
    assert(ctx->cluster != NULL);
    
    for (int i = 0; i < NUM_NODES; i++) {
        ctx->nodes[i] = test_cluster_get_node(ctx->cluster, i);
        assert(ctx->nodes[i] != NULL);
    }
    
    return ctx;
}

static void teardown_test(test_context_t *ctx) {
    if (ctx) {
        test_cluster_destroy(ctx->cluster);
        free(ctx);
    }
}

/**
 * Count how many nodes are in each state
 */
static void count_node_states(test_context_t *ctx, int *leaders, 
                              int *candidates, int *followers) {
    *leaders = 0;
    *candidates = 0;
    *followers = 0;
    
    for (int i = 0; i < NUM_NODES; i++) {
        raft_state_t state = raft_get_state(ctx->nodes[i]);
        
        switch (state) {
            case RAFT_STATE_LEADER:
                (*leaders)++;
                break;
            case RAFT_STATE_CANDIDATE:
                (*candidates)++;
                break;
            case RAFT_STATE_FOLLOWER:
                (*followers)++;
                break;
        }
    }
}

/**
 * Test: Basic split vote scenario
 */
static void test_basic_split_vote(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Basic split vote with eventual leader election\n");
    
    // Set identical election timeouts to force simultaneous elections
    for (int i = 0; i < NUM_NODES; i++) {
        raft_set_election_timeout(ctx->nodes[i], 150, 150);
    }
    
    // Start cluster - all nodes timeout simultaneously
    test_cluster_start(ctx->cluster);
    test_cluster_tick(ctx->cluster, 150);
    
    int leaders, candidates, followers;
    count_node_states(ctx, &leaders, &candidates, &followers);
    
    printf("  After first election timeout:\n");
    printf("    Leaders: %d, Candidates: %d, Followers: %d\n", 
           leaders, candidates, followers);
    
    // Should have multiple candidates, no leader
    assert(leaders == 0);
    assert(candidates >= 2);  // At least 2 candidates
    
    // Check that votes were split
    int total_votes_received = 0;
    for (int i = 0; i < NUM_NODES; i++) {
        total_votes_received += raft_get_vote_count(ctx->nodes[i]);
    }
    
    // No single candidate should have majority
    for (int i = 0; i < NUM_NODES; i++) {
        if (raft_get_state(ctx->nodes[i]) == RAFT_STATE_CANDIDATE) {
            assert(raft_get_vote_count(ctx->nodes[i]) < (NUM_NODES / 2) + 1);
        }
    }
    
    printf("  ✓ Split vote detected: %d total votes cast, no majority\n", 
           total_votes_received);
    
    // Track term before new election
    uint64_t term_before = raft_get_current_term(ctx->nodes[0]);
    
    // Allow time for randomized timeouts to trigger new election
    uint64_t start = test_get_time_ms();
    bool leader_elected = false;
    
    while (test_get_time_ms() - start < TEST_TIMEOUT_MS) {
        test_cluster_tick(ctx->cluster, 50);
        
        count_node_states(ctx, &leaders, &candidates, &followers);
        
        if (leaders == 1) {
            leader_elected = true;
            break;
        }
    }
    
    assert(leader_elected);
    
    uint64_t term_after = raft_get_current_term(ctx->nodes[0]);
    
    printf("  ✓ Leader eventually elected after split vote\n");
    printf("  ✓ Term increased from %lu to %lu\n", term_before, term_after);
    assert(term_after > term_before);
    
    // Verify exactly one leader
    count_node_states(ctx, &leaders, &candidates, &followers);
    assert(leaders == 1);
    assert(followers == NUM_NODES - 1);
    
    printf("  ✓ Final state: 1 leader, %d followers\n", followers);
    
    teardown_test(ctx);
}

/**
 * Test: Three-way split vote
 */
static void test_three_way_split(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Three-way split vote scenario\n");
    
    // Configure timeouts to create 3-way split
    // Nodes 0, 1, 2 become candidates
    raft_set_election_timeout(ctx->nodes[0], 100, 100);
    raft_set_election_timeout(ctx->nodes[1], 100, 100);
    raft_set_election_timeout(ctx->nodes[2], 100, 100);
    raft_set_election_timeout(ctx->nodes[3], 500, 500);  // Delayed
    raft_set_election_timeout(ctx->nodes[4], 500, 500);  // Delayed
    
    test_cluster_start(ctx->cluster);
    test_cluster_tick(ctx->cluster, 100);
    
    int leaders, candidates, followers;
    count_node_states(ctx, &leaders, &candidates, &followers);
    
    // Should have 3 candidates
    assert(candidates == 3);
    assert(leaders == 0);
    
    printf("  ✓ Three-way split: %d candidates\n", candidates);
    
    // Each candidate should have voted for themselves
    int self_votes = 0;
    for (int i = 0; i < 3; i++) {
        if (raft_get_state(ctx->nodes[i]) == RAFT_STATE_CANDIDATE) {
            assert(raft_get_vote_count(ctx->nodes[i]) >= 1);  // At least self-vote
            self_votes++;
        }
    }
    
    assert(self_votes == 3);
    
    // Eventually elect leader
    uint64_t start = test_get_time_ms();
    bool leader_elected = false;
    int election_rounds = 0;
    uint64_t last_term = raft_get_current_term(ctx->nodes[0]);
    
    while (test_get_time_ms() - start < TEST_TIMEOUT_MS) {
        test_cluster_tick(ctx->cluster, 50);
        
        uint64_t current_term = raft_get_current_term(ctx->nodes[0]);
        if (current_term > last_term) {
            election_rounds++;
            last_term = current_term;
        }
        
        count_node_states(ctx, &leaders, &candidates, &followers);
        if (leaders == 1) {
            leader_elected = true;
            break;
        }
    }
    
    assert(leader_elected);
    printf("  ✓ Leader elected after %d election rounds\n", election_rounds);
    assert(election_rounds >= 2);  // Should take multiple rounds
    
    teardown_test(ctx);
}

/**
 * Test: Split vote with network delays
 */
static void test_split_vote_with_delays(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Split vote with message delays\n");
    
    // Enable network delays
    test_cluster_set_message_delay(ctx->cluster, 10, 50);  // 10-50ms delay
    
    // Simultaneous candidate starts
    for (int i = 0; i < NUM_NODES; i++) {
        raft_set_election_timeout(ctx->nodes[i], 150, 150);
    }
    
    test_cluster_start(ctx->cluster);
    test_cluster_tick(ctx->cluster, 150);
    
    // May have split vote due to delays
    int leaders, candidates, followers;
    count_node_states(ctx, &leaders, &candidates, &followers);
    
    printf("  With delays - Leaders: %d, Candidates: %d\n", leaders, candidates);
    
    // System must still converge to single leader
    uint64_t start = test_get_time_ms();
    bool converged = false;
    
    while (test_get_time_ms() - start < TEST_TIMEOUT_MS) {
        test_cluster_tick(ctx->cluster, 50);
        
        count_node_states(ctx, &leaders, &candidates, &followers);
        
        if (leaders == 1 && followers == NUM_NODES - 1) {
            converged = true;
            break;
        }
    }
    
    assert(converged);
    printf("  ✓ Converged to single leader despite message delays\n");
    
    // Disable delays
    test_cluster_set_message_delay(ctx->cluster, 0, 0);
    
    teardown_test(ctx);
}

/**
 * Test: Repeated split votes eventually resolve
 */
static void test_repeated_split_votes(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Repeated split votes eventually resolve\n");
    
    uint64_t initial_term = 1;
    int split_vote_rounds = 0;
    const int max_split_rounds = 10;
    
    for (int i = 0; i < NUM_NODES; i++) {
        // Very similar timeouts to encourage splits
        int timeout = 150 + (i % 2) * 5;
        raft_set_election_timeout(ctx->nodes[i], timeout, timeout);
    }
    
    test_cluster_start(ctx->cluster);
    
    uint64_t start = test_get_time_ms();
    uint64_t last_term = initial_term;
    
    while (test_get_time_ms() - start < TEST_TIMEOUT_MS) {
        test_cluster_tick(ctx->cluster, 50);
        
        uint64_t current_term = raft_get_current_term(ctx->nodes[0]);
        
        // Count split vote rounds
        if (current_term > last_term) {
            int leaders, candidates, followers;
            count_node_states(ctx, &leaders, &candidates, &followers);
            
            if (leaders == 0 && candidates > 0) {
                split_vote_rounds++;
                printf("    Split vote round %d (term %lu)\n", 
                       split_vote_rounds, current_term);
            }
            
            last_term = current_term;
        }
        
        int leaders, candidates, followers;
        count_node_states(ctx, &leaders, &candidates, &followers);
        
        if (leaders == 1) {
            break;
        }
    }
    
    int final_leaders, final_candidates, final_followers;
    count_node_states(ctx, &final_leaders, &final_candidates, &final_followers);
    
    assert(final_leaders == 1);
    printf("  ✓ Resolved after %d split vote rounds\n", split_vote_rounds);
    printf("  ✓ Final term: %lu\n", raft_get_current_term(ctx->nodes[0]));
    
    teardown_test(ctx);
}

/**
 * Test: Term monotonicity during split votes
 */
static void test_term_monotonicity_split_vote(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Terms increase monotonically during split votes\n");
    
    for (int i = 0; i < NUM_NODES; i++) {
        raft_set_election_timeout(ctx->nodes[i], 150, 150);
    }
    
    test_cluster_start(ctx->cluster);
    
    uint64_t terms[NUM_NODES][100];  // Track term history for each node
    int term_counts[NUM_NODES] = {0};
    
    uint64_t start = test_get_time_ms();
    bool leader_elected = false;
    
    while (test_get_time_ms() - start < TEST_TIMEOUT_MS) {
        test_cluster_tick(ctx->cluster, 25);
        
        // Record current terms
        for (int i = 0; i < NUM_NODES; i++) {
            uint64_t current_term = raft_get_current_term(ctx->nodes[i]);
            
            int count = term_counts[i];
            if (count == 0 || terms[i][count - 1] != current_term) {
                assert(count < 100);
                terms[i][count] = current_term;
                term_counts[i]++;
            }
        }
        
        int leaders, candidates, followers;
        count_node_states(ctx, &leaders, &candidates, &followers);
        
        if (leaders == 1) {
            leader_elected = true;
            break;
        }
    }
    
    assert(leader_elected);
    
    // Verify monotonicity for each node
    for (int node = 0; node < NUM_NODES; node++) {
        for (int i = 1; i < term_counts[node]; i++) {
            assert(terms[node][i] > terms[node][i - 1]);
        }
        
        printf("  Node %d: term progression with %d changes\n", 
               node, term_counts[node] - 1);
    }
    
    printf("  ✓ All nodes maintained term monotonicity\n");
    
    teardown_test(ctx);
}

/**
 * Test: Vote rejection prevents double-voting
 */
static void test_vote_rejection_in_split(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Vote rejection prevents double-voting in split scenario\n");
    
    // Setup for split vote
    for (int i = 0; i < 3; i++) {
        raft_set_election_timeout(ctx->nodes[i], 100, 100);
    }
    for (int i = 3; i < NUM_NODES; i++) {
        raft_set_election_timeout(ctx->nodes[i], 300, 300);
    }
    
    test_cluster_start(ctx->cluster);
    test_cluster_tick(ctx->cluster, 100);
    
    // Multiple candidates should exist
    int leaders, candidates, followers;
    count_node_states(ctx, &leaders, &candidates, &followers);
    assert(candidates >= 2);
    
    // Each follower should have voted for exactly one candidate
    for (int i = 0; i < NUM_NODES; i++) {
        if (raft_get_state(ctx->nodes[i]) == RAFT_STATE_FOLLOWER) {
            uint64_t voted_for = raft_get_voted_for(ctx->nodes[i]);
            assert(voted_for != RAFT_NO_VOTE);  // Should have voted
            
            // Verify cannot vote again in same term
            uint64_t current_term = raft_get_current_term(ctx->nodes[i]);
            
            // Simulate another RequestVote in same term
            raft_request_vote_req_t req = {
                .term = current_term,
                .candidate_id = (voted_for + 1) % NUM_NODES,
                .last_log_index = 0,
                .last_log_term = 0
            };
            
            raft_request_vote_resp_t resp = {0};
            raft_handle_request_vote(ctx->nodes[i], &req, &resp);
            
            // Vote should be rejected
            assert(resp.vote_granted == false);
        }
    }
    
    printf("  ✓ Followers voted for exactly one candidate\n");
    printf("  ✓ Double-voting prevented\n");
    
    teardown_test(ctx);
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  RAFT Split Vote Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_init();
    
    test_basic_split_vote();
    printf("\n");
    
    test_three_way_split();
    printf("\n");
    
    test_split_vote_with_delays();
    printf("\n");
    
    test_repeated_split_votes();
    printf("\n");
    
    test_term_monotonicity_split_vote();
    printf("\n");
    
    test_vote_rejection_in_split();
    printf("\n");
    
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  All Split Vote Tests Passed ✓\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_cleanup();
    return 0;
}