/**
 * @file test_raft_leader_stability.c
 * @brief Test RAFT leader stability
 *
 * Scenario:
 * A stable cluster with no failures and a functioning leader sending regular
 * heartbeats. Followers must not start unnecessary elections.
 *
 * Why Important:
 * Followers starting elections unnecessarily causes leadership churn, degrading
 * performance and potentially causing availability issues.
 *
 * Assertions:
 * - No follower becomes candidate during stable period
 * - Leader remains leader
 * - Term does not change
 * - Election timers are reset by heartbeats
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "raft_test_framework.h"

#define NUM_NODES 5
#define STABILITY_PERIOD_MS 10000  // 10 seconds of stable operation

typedef struct {
    raft_node_t *nodes[NUM_NODES];
    test_cluster_t *cluster;
    int leader_id;
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
    
    // Elect a leader
    test_cluster_elect_leader(ctx->cluster, 0);
    ctx->leader_id = 0;
    
    assert(raft_get_state(ctx->nodes[0]) == RAFT_STATE_LEADER);
    
    return ctx;
}

static void teardown_test(test_context_t *ctx) {
    if (ctx) {
        test_cluster_destroy(ctx->cluster);
        free(ctx);
    }
}

/**
 * Test: Leader remains stable with regular heartbeats
 */
static void test_basic_leader_stability(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Leader stability with regular heartbeats\n");
    
    raft_node_t *leader = ctx->nodes[ctx->leader_id];
    uint64_t initial_term = raft_get_current_term(leader);
    
    printf("  Initial state: Node %d is leader in term %lu\n", 
           ctx->leader_id, initial_term);
    
    // Run cluster for stability period
    uint64_t start = test_get_time_ms();
    int heartbeat_count = 0;
    
    while (test_get_time_ms() - start < STABILITY_PERIOD_MS) {
        test_cluster_tick(ctx->cluster, 50);
        
        // Count heartbeats sent
        if (test_get_time_ms() % 100 < 50) {  // Approximate counting
            heartbeat_count++;
        }
        
        // Verify leader is still leader
        assert(raft_get_state(leader) == RAFT_STATE_LEADER);
        
        // Verify all followers are still followers
        for (int i = 0; i < NUM_NODES; i++) {
            if (i != ctx->leader_id) {
                raft_state_t state = raft_get_state(ctx->nodes[i]);
                assert(state == RAFT_STATE_FOLLOWER);
            }
        }
        
        // Verify term has not changed
        assert(raft_get_current_term(leader) == initial_term);
    }
    
    printf("  ✓ Leader remained stable for %d ms\n", STABILITY_PERIOD_MS);
    printf("  ✓ Term unchanged: %lu\n", initial_term);
    printf("  ✓ No follower became candidate\n");
    printf("  ✓ Approximate heartbeats sent: %d\n", heartbeat_count);
    
    teardown_test(ctx);
}

/**
 * Test: Heartbeats reset election timers
 */
static void test_heartbeats_reset_election_timers(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Heartbeats reset follower election timers\n");
    
    raft_node_t *leader = ctx->nodes[ctx->leader_id];
    
    // Set known election timeout
    int election_timeout = 300;
    for (int i = 0; i < NUM_NODES; i++) {
        if (i != ctx->leader_id) {
            raft_set_election_timeout(ctx->nodes[i], 
                                     election_timeout, 
                                     election_timeout);
        }
    }
    
    // Set heartbeat interval shorter than election timeout
    raft_set_heartbeat_interval(leader, 100);
    
    printf("  Election timeout: %d ms\n", election_timeout);
    printf("  Heartbeat interval: 100 ms\n");
    
    // Run for 3x election timeout
    uint64_t start = test_get_time_ms();
    uint64_t duration = election_timeout * 3;
    
    while (test_get_time_ms() - start < duration) {
        test_cluster_tick(ctx->cluster, 25);
        
        // No follower should timeout
        for (int i = 0; i < NUM_NODES; i++) {
            if (i != ctx->leader_id) {
                assert(raft_get_state(ctx->nodes[i]) == RAFT_STATE_FOLLOWER);
            }
        }
    }
    
    printf("  ✓ Ran for %lu ms (3x election timeout)\n", duration);
    printf("  ✓ No follower started election\n");
    printf("  ✓ Election timers successfully reset by heartbeats\n");
    
    teardown_test(ctx);
}

/**
 * Test: Leader stability under load
 */
static void test_leader_stability_under_load(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Leader stability while replicating entries\n");
    
    raft_node_t *leader = ctx->nodes[ctx->leader_id];
    uint64_t initial_term = raft_get_current_term(leader);
    
    const int num_entries = 100;
    int committed_count = 0;
    
    // Continuously append entries
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "load_entry_%d", i);
        
        raft_entry_t entry = {
            .term = raft_get_current_term(leader),
            .data = data,
            .data_len = strlen(data)
        };
        
        raft_append_entry(leader, &entry);
        
        // Allow replication
        test_cluster_tick(ctx->cluster, 10);
        
        // Verify leader is still leader
        assert(raft_get_state(leader) == RAFT_STATE_LEADER);
        assert(raft_get_current_term(leader) == initial_term);
        
        // Verify no follower became candidate
        for (int j = 0; j < NUM_NODES; j++) {
            if (j != ctx->leader_id) {
                assert(raft_get_state(ctx->nodes[j]) == RAFT_STATE_FOLLOWER);
            }
        }
    }
    
    // Final replication
    test_cluster_replicate_and_commit(ctx->cluster);
    committed_count = raft_get_commit_index(leader);
    
    printf("  ✓ Appended and committed %d entries\n", committed_count);
    printf("  ✓ Leader remained stable throughout\n");
    printf("  ✓ No unnecessary elections\n");
    
    teardown_test(ctx);
}

/**
 * Test: Multiple followers all remain stable
 */
static void test_all_followers_remain_stable(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: All followers remain stable simultaneously\n");
    
    // Track state of each follower over time
    typedef struct {
        int transitions_to_candidate;
        int transitions_to_leader;
        uint64_t term_changes;
    } follower_stats_t;
    
    follower_stats_t stats[NUM_NODES] = {0};
    
    uint64_t prev_terms[NUM_NODES];
    raft_state_t prev_states[NUM_NODES];
    
    for (int i = 0; i < NUM_NODES; i++) {
        prev_terms[i] = raft_get_current_term(ctx->nodes[i]);
        prev_states[i] = raft_get_state(ctx->nodes[i]);
    }
    
    // Monitor for stability period
    uint64_t start = test_get_time_ms();
    
    while (test_get_time_ms() - start < STABILITY_PERIOD_MS) {
        test_cluster_tick(ctx->cluster, 50);
        
        for (int i = 0; i < NUM_NODES; i++) {
            raft_state_t current_state = raft_get_state(ctx->nodes[i]);
            uint64_t current_term = raft_get_current_term(ctx->nodes[i]);
            
            // Track state transitions
            if (prev_states[i] == RAFT_STATE_FOLLOWER && 
                current_state == RAFT_STATE_CANDIDATE) {
                stats[i].transitions_to_candidate++;
            }
            
            if (prev_states[i] != RAFT_STATE_LEADER && 
                current_state == RAFT_STATE_LEADER) {
                stats[i].transitions_to_leader++;
            }
            
            if (current_term != prev_terms[i]) {
                stats[i].term_changes++;
            }
            
            prev_states[i] = current_state;
            prev_terms[i] = current_term;
        }
    }
    
    // Verify stability
    for (int i = 0; i < NUM_NODES; i++) {
        if (i == ctx->leader_id) {
            assert(stats[i].transitions_to_candidate == 0);
            assert(stats[i].transitions_to_leader == 0);  // Was already leader
        } else {
            assert(stats[i].transitions_to_candidate == 0);
            assert(stats[i].transitions_to_leader == 0);
        }
        
        assert(stats[i].term_changes == 0);
        
        printf("  Node %d: %s - no transitions\n", i,
               i == ctx->leader_id ? "LEADER" : "FOLLOWER");
    }
    
    printf("  ✓ All nodes remained in stable state\n");
    
    teardown_test(ctx);
}

/**
 * Test: Stability with delayed/lost heartbeats
 */
static void test_stability_with_occasional_lost_heartbeats(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Stability with occasional lost heartbeats\n");
    
    raft_node_t *leader = ctx->nodes[ctx->leader_id];
    uint64_t initial_term = raft_get_current_term(leader);
    
    // Set message loss rate (10% of heartbeats may be lost)
    test_cluster_set_message_loss_rate(ctx->cluster, 0.10);
    
    printf("  Message loss rate: 10%%\n");
    
    uint64_t start = test_get_time_ms();
    int election_attempts = 0;
    
    while (test_get_time_ms() - start < STABILITY_PERIOD_MS) {
        test_cluster_tick(ctx->cluster, 50);
        
        // Check for any election attempts
        for (int i = 0; i < NUM_NODES; i++) {
            if (i != ctx->leader_id && 
                raft_get_state(ctx->nodes[i]) == RAFT_STATE_CANDIDATE) {
                election_attempts++;
            }
        }
        
        // Leader should still be leader
        assert(raft_get_state(leader) == RAFT_STATE_LEADER);
    }
    
    // Some heartbeats were lost, but should still be stable
    assert(raft_get_current_term(leader) == initial_term);
    
    // Should be no or very few election attempts
    assert(election_attempts <= 2);  // Allow minimal attempts due to packet loss
    
    printf("  ✓ Remained stable despite packet loss\n");
    printf("  ✓ Election attempts: %d (acceptable)\n", election_attempts);
    
    // Reset message loss
    test_cluster_set_message_loss_rate(ctx->cluster, 0.0);
    
    teardown_test(ctx);
}

/**
 * Test: Term does not increase unnecessarily
 */
static void test_term_remains_constant(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Term remains constant during stability\n");
    
    uint64_t initial_term = raft_get_current_term(ctx->nodes[0]);
    
    printf("  Initial term: %lu\n", initial_term);
    
    // Track term across all nodes
    uint64_t max_term = initial_term;
    
    uint64_t start = test_get_time_ms();
    
    while (test_get_time_ms() - start < STABILITY_PERIOD_MS) {
        test_cluster_tick(ctx->cluster, 100);
        
        for (int i = 0; i < NUM_NODES; i++) {
            uint64_t node_term = raft_get_current_term(ctx->nodes[i]);
            
            if (node_term > max_term) {
                max_term = node_term;
            }
            
            // Term should never decrease
            assert(node_term >= initial_term);
            
            // Term should not increase (stable cluster)
            assert(node_term == initial_term);
        }
    }
    
    assert(max_term == initial_term);
    
    printf("  ✓ Term remained at %lu for entire stability period\n", initial_term);
    printf("  ✓ No term increases detected\n");
    
    teardown_test(ctx);
}

/**
 * Test: Leader continues to commit entries
 */
static void test_leader_continues_committing(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Leader continues to commit entries while stable\n");
    
    raft_node_t *leader = ctx->nodes[ctx->leader_id];
    uint64_t initial_commit = raft_get_commit_index(leader);
    
    // Add entries periodically
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 5; i++) {
            char data[32];
            snprintf(data, sizeof(data), "entry_r%d_i%d", round, i);
            
            raft_entry_t entry = {
                .term = raft_get_current_term(leader),
                .data = data,
                .data_len = strlen(data)
            };
            
            raft_append_entry(leader, &entry);
        }
        
        // Replicate and commit
        test_cluster_replicate_and_commit(ctx->cluster);
        
        // Verify still stable
        assert(raft_get_state(leader) == RAFT_STATE_LEADER);
        
        for (int i = 0; i < NUM_NODES; i++) {
            if (i != ctx->leader_id) {
                assert(raft_get_state(ctx->nodes[i]) == RAFT_STATE_FOLLOWER);
            }
        }
        
        test_cluster_tick(ctx->cluster, 200);
    }
    
    uint64_t final_commit = raft_get_commit_index(leader);
    
    printf("  ✓ Commit index: %lu → %lu\n", initial_commit, final_commit);
    printf("  ✓ Successfully committed %lu new entries\n", 
           final_commit - initial_commit);
    printf("  ✓ Cluster remained stable throughout\n");
    
    teardown_test(ctx);
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  RAFT Leader Stability Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_init();
    
    test_basic_leader_stability();
    printf("\n");
    
    test_heartbeats_reset_election_timers();
    printf("\n");
    
    test_leader_stability_under_load();
    printf("\n");
    
    test_all_followers_remain_stable();
    printf("\n");
    
    test_stability_with_occasional_lost_heartbeats();
    printf("\n");
    
    test_term_remains_constant();
    printf("\n");
    
    test_leader_continues_committing();
    printf("\n");
    
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  All Leader Stability Tests Passed ✓\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_cleanup();
    return 0;
}