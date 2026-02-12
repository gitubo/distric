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
    test_cluster_start(ctx->cluster);
    ctx->leader_id = test_cluster_wait_for_leader(ctx->cluster, 5000);
    assert(ctx->leader_id >= 0);
    assert(raft_get_state(ctx->nodes[ctx->leader_id]) == RAFT_STATE_LEADER);
    
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
    uint64_t initial_term = raft_get_term(leader);
    
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
        assert(raft_get_term(leader) == initial_term);
        
        // Verify no follower became candidate
        for (int i = 0; i < NUM_NODES; i++) {
            if (i != ctx->leader_id) {
                raft_state_t state = raft_get_state(ctx->nodes[i]);
                assert(state == RAFT_STATE_FOLLOWER);
            }
        }
    }
    
    uint64_t duration = test_get_time_ms() - start;
    
    printf("  ✓ Leader remained stable for %lu ms\n", duration);
    printf("  ✓ No unnecessary elections occurred\n");
    printf("  ✓ Term remained constant at %lu\n", initial_term);
    
    teardown_test(ctx);
}

/**
 * Test: Heartbeats reset follower election timers
 */
static void test_heartbeats_reset_election_timers(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Heartbeats reset follower election timers\n");
    
    // Election timeout is typically 150-300ms
    // Heartbeat interval is typically 50ms  
    // Run for 3x max election timeout (900ms)
    uint64_t test_duration = 3 * 300;  // 3x max election timeout
    
    uint64_t start = test_get_time_ms();
    
    while (test_get_time_ms() - start < test_duration) {
        test_cluster_tick(ctx->cluster, 10);
        
        // No follower should start an election
        for (int i = 0; i < NUM_NODES; i++) {
            if (i != ctx->leader_id) {
                assert(raft_get_state(ctx->nodes[i]) == RAFT_STATE_FOLLOWER);
            }
        }
    }
    
    uint64_t duration = test_get_time_ms() - start;
    
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
    uint64_t initial_term = raft_get_term(leader);
    
    const int num_entries = 100;
    int committed_count = 0;
    
    // Continuously append entries
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "load_entry_%d", i);
        
        uint32_t index_out;
        raft_append_entry(leader, (const uint8_t*)data, strlen(data), &index_out);
        
        // Allow replication
        test_cluster_tick(ctx->cluster, 10);
        
        // Verify leader is still leader
        assert(raft_get_state(leader) == RAFT_STATE_LEADER);
        assert(raft_get_term(leader) == initial_term);
        
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
    
    uint64_t test_duration = 5000;  // 5 seconds
    uint64_t start = test_get_time_ms();
    
    while (test_get_time_ms() - start < test_duration) {
        test_cluster_tick(ctx->cluster, 50);
        
        int leaders = 0, followers = 0;
        
        for (int i = 0; i < NUM_NODES; i++) {
            raft_state_t state = raft_get_state(ctx->nodes[i]);
            
            if (state == RAFT_STATE_LEADER) {
                leaders++;
            } else if (state == RAFT_STATE_FOLLOWER) {
                followers++;
            } else {
                // Should never have candidates
                assert(false);
            }
        }
        
        assert(leaders == 1);
        assert(followers == NUM_NODES - 1);
    }
    
    printf("  ✓ All %d followers remained stable for %lu ms\n", 
           NUM_NODES - 1, test_duration);
    printf("  ✓ No followers started elections\n");
    
    teardown_test(ctx);
}

/**
 * Test: Stability with occasional lost heartbeats
 */
static void test_stability_with_occasional_lost_heartbeats(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Stability with occasional lost heartbeats\n");
    
    // Enable 5% message loss
    test_cluster_set_message_loss(ctx->cluster, 0.05);
    
    uint64_t test_duration = 3000;
    uint64_t start = test_get_time_ms();
    
    while (test_get_time_ms() - start < test_duration) {
        test_cluster_tick(ctx->cluster, 50);
        
        // Leader should remain stable
        assert(raft_get_state(ctx->nodes[ctx->leader_id]) == RAFT_STATE_LEADER);
        
        // Followers should remain followers (heartbeats still get through)
        for (int i = 0; i < NUM_NODES; i++) {
            if (i != ctx->leader_id) {
                assert(raft_get_state(ctx->nodes[i]) == RAFT_STATE_FOLLOWER);
            }
        }
    }
    
    // Disable message loss
    test_cluster_set_message_loss(ctx->cluster, 0.0);
    
    printf("  ✓ Cluster remained stable despite 5%% message loss\n");
    printf("  ✓ Heartbeats sufficient to prevent elections\n");
    
    teardown_test(ctx);
}

/**
 * Test: Term remains constant during stability
 */
static void test_term_remains_constant(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Term remains constant during stability\n");
    
    uint64_t initial_term = raft_get_term(ctx->nodes[0]);
    
    printf("  Initial term: %lu\n", initial_term);
    
    // Track term across all nodes
    uint64_t max_term = initial_term;
    
    uint64_t start = test_get_time_ms();
    
    while (test_get_time_ms() - start < STABILITY_PERIOD_MS) {
        test_cluster_tick(ctx->cluster, 100);
        
        for (int i = 0; i < NUM_NODES; i++) {
            uint64_t node_term = raft_get_term(ctx->nodes[i]);
            
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
            
            uint32_t index_out;
            raft_append_entry(leader, (const uint8_t*)data, strlen(data), &index_out);
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