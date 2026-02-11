/**
 * @file test_raft_leader_completeness.c
 * @brief Test RAFT Leader Completeness Property
 *
 * Scenario:
 * An entry is committed in one term and then a new leader is elected in a
 * higher term. The Leader Completeness property states that if an entry is
 * committed, all future leaders must contain that entry.
 *
 * Why Important:
 * This is a fundamental safety property of RAFT. Violating it means committed
 * data can be lost, breaking the core guarantee of consensus.
 *
 * Assertions:
 * - New leader contains all committed entries
 * - No leader missing committed entries is elected
 * - Committed entries are never overwritten
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "raft.h"
#include "test_framework.h"

#define NUM_NODES 5
#define TEST_TIMEOUT_MS 10000

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
 * Test: New leader has all committed entries from previous term
 */
static void test_new_leader_has_committed_entries(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: New leader contains all committed entries\n");
    
    // Elect initial leader (node 0)
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader1 = ctx->nodes[0];
    assert(raft_get_state(leader1) == RAFT_STATE_LEADER);
    
    uint64_t term1 = raft_get_current_term(leader1);
    
    // Append and commit entries
    const int num_entries = 10;
    uint64_t committed_indices[num_entries];
    
    for (int i = 0; i < num_entries; i++) {
        char data[64];
        snprintf(data, sizeof(data), "committed_entry_%d", i);
        
        raft_entry_t entry = {
            .term = term1,
            .data = data,
            .data_len = strlen(data)
        };
        
        committed_indices[i] = raft_append_entry(leader1, &entry);
    }
    
    // Replicate to majority and commit
    test_cluster_replicate_and_commit(ctx->cluster);
    
    // Verify entries are committed on leader
    uint64_t commit_index = raft_get_commit_index(leader1);
    assert(commit_index >= num_entries);
    
    printf("  ✓ Committed %d entries (commit_index=%lu)\n", num_entries, commit_index);
    
    // Crash the leader
    test_cluster_crash_node(ctx->cluster, 0);
    
    // Elect new leader from remaining nodes
    // Try nodes 1-4 to find one with complete log
    int new_leader_id = -1;
    for (int i = 1; i < NUM_NODES; i++) {
        if (raft_get_last_log_index(ctx->nodes[i]) >= commit_index) {
            new_leader_id = i;
            break;
        }
    }
    
    assert(new_leader_id != -1);
    test_cluster_elect_leader(ctx->cluster, new_leader_id);
    
    raft_node_t *leader2 = ctx->nodes[new_leader_id];
    assert(raft_get_state(leader2) == RAFT_STATE_LEADER);
    
    uint64_t term2 = raft_get_current_term(leader2);
    assert(term2 > term1);
    
    // Verify new leader has all committed entries
    for (int i = 0; i < num_entries; i++) {
        raft_entry_t *entry = raft_get_entry(leader2, committed_indices[i]);
        assert(entry != NULL);
        
        char expected[64];
        snprintf(expected, sizeof(expected), "committed_entry_%d", i);
        assert(strcmp(entry->data, expected) == 0);
        assert(entry->term == term1);
    }
    
    printf("  ✓ New leader elected in term %lu (previous term %lu)\n", term2, term1);
    printf("  ✓ New leader has all %d committed entries\n", num_entries);
    
    teardown_test(ctx);
}

/**
 * Test: Node with incomplete log cannot win election
 */
static void test_incomplete_log_loses_election(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Candidate with incomplete log loses election\n");
    
    // Elect leader
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader = ctx->nodes[0];
    
    // Commit entries on majority (nodes 0, 1, 2)
    for (int i = 0; i < 5; i++) {
        char data[32];
        snprintf(data, sizeof(data), "entry_%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader, &entry);
    }
    
    // Replicate to nodes 0, 1, 2 only
    test_cluster_partition(ctx->cluster, (int[]){0, 1, 2}, 3, (int[]){3, 4}, 2);
    test_cluster_replicate_and_commit(ctx->cluster);
    
    uint64_t commit_index = raft_get_commit_index(leader);
    
    // Verify nodes 3 and 4 are behind
    assert(raft_get_last_log_index(ctx->nodes[3]) < commit_index);
    assert(raft_get_last_log_index(ctx->nodes[4]) < commit_index);
    
    // Heal partition and crash leader
    test_cluster_heal_partition(ctx->cluster);
    test_cluster_crash_node(ctx->cluster, 0);
    
    // Try to elect node 3 (which is behind)
    test_cluster_start_election(ctx->cluster, 3);
    test_cluster_tick(ctx->cluster, 1000);
    
    // Node 3 should NOT win election
    assert(raft_get_state(ctx->nodes[3]) != RAFT_STATE_LEADER);
    
    // Eventually, node 1 or 2 should win
    uint64_t start = test_get_time_ms();
    bool leader_elected = false;
    
    while (test_get_time_ms() - start < TEST_TIMEOUT_MS) {
        test_cluster_tick(ctx->cluster, 50);
        
        for (int i = 1; i < NUM_NODES; i++) {
            if (i == 0 || i == 3 || i == 4) continue;
            
            if (raft_get_state(ctx->nodes[i]) == RAFT_STATE_LEADER) {
                // Verify this leader has all committed entries
                assert(raft_get_last_log_index(ctx->nodes[i]) >= commit_index);
                leader_elected = true;
                printf("  ✓ Node %d elected (has complete log)\n", i);
                break;
            }
        }
        
        if (leader_elected) break;
    }
    
    assert(leader_elected);
    printf("  ✓ Node with incomplete log rejected\n");
    
    teardown_test(ctx);
}

/**
 * Test: Committed entries survive multiple leader changes
 */
static void test_committed_entries_survive_leader_changes(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Committed entries survive multiple leader changes\n");
    
    const int num_leader_changes = 3;
    const int entries_per_leader = 3;
    
    uint64_t all_committed_indices[num_leader_changes * entries_per_leader];
    int total_committed = 0;
    
    for (int leader_idx = 0; leader_idx < num_leader_changes; leader_idx++) {
        int leader_id = leader_idx % NUM_NODES;
        
        // Elect new leader
        test_cluster_elect_leader(ctx->cluster, leader_id);
        raft_node_t *leader = ctx->nodes[leader_id];
        
        uint64_t term = raft_get_current_term(leader);
        
        // Commit some entries
        for (int i = 0; i < entries_per_leader; i++) {
            char data[64];
            snprintf(data, sizeof(data), "leader%d_entry%d_term%lu", 
                     leader_id, i, term);
            
            raft_entry_t entry = {
                .term = term,
                .data = data,
                .data_len = strlen(data)
            };
            
            all_committed_indices[total_committed++] = 
                raft_append_entry(leader, &entry);
        }
        
        test_cluster_replicate_and_commit(ctx->cluster);
        
        printf("  ✓ Leader %d (term %lu) committed %d entries\n", 
               leader_id, term, entries_per_leader);
        
        // Crash this leader to force election
        if (leader_idx < num_leader_changes - 1) {
            test_cluster_crash_node(ctx->cluster, leader_id);
        }
    }
    
    // Final leader must have all committed entries
    int final_leader_id = -1;
    for (int i = 0; i < NUM_NODES; i++) {
        if (raft_get_state(ctx->nodes[i]) == RAFT_STATE_LEADER) {
            final_leader_id = i;
            break;
        }
    }
    
    assert(final_leader_id != -1);
    raft_node_t *final_leader = ctx->nodes[final_leader_id];
    
    // Verify all committed entries are present
    for (int i = 0; i < total_committed; i++) {
        raft_entry_t *entry = raft_get_entry(final_leader, all_committed_indices[i]);
        assert(entry != NULL);
    }
    
    printf("  ✓ Final leader has all %d committed entries from %d leaders\n",
           total_committed, num_leader_changes);
    
    teardown_test(ctx);
}

/**
 * Test: Election safety - at most one leader per term
 */
static void test_election_safety_with_committed_entries(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Election safety with committed entries\n");
    
    // Elect initial leader and commit entries
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader1 = ctx->nodes[0];
    
    for (int i = 0; i < 5; i++) {
        char data[32];
        snprintf(data, sizeof(data), "safety_entry_%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader1),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    uint64_t commit_index1 = raft_get_commit_index(leader1);
    uint64_t term1 = raft_get_current_term(leader1);
    
    // Partition network to create potential for split vote
    test_cluster_partition(ctx->cluster, (int[]){0, 1}, 2, (int[]){2, 3, 4}, 3);
    
    // Trigger elections in both partitions
    test_cluster_crash_node(ctx->cluster, 0);
    test_cluster_start_election(ctx->cluster, 1);
    test_cluster_start_election(ctx->cluster, 2);
    
    test_cluster_tick(ctx->cluster, 500);
    
    // Count leaders in each partition
    int leaders_partition1 = 0;
    int leaders_partition2 = 0;
    
    if (raft_get_state(ctx->nodes[1]) == RAFT_STATE_LEADER) {
        leaders_partition1++;
        // Must have committed entries
        assert(raft_get_last_log_index(ctx->nodes[1]) >= commit_index1);
    }
    
    for (int i = 2; i <= 4; i++) {
        if (raft_get_state(ctx->nodes[i]) == RAFT_STATE_LEADER) {
            leaders_partition2++;
            assert(raft_get_last_log_index(ctx->nodes[i]) >= commit_index1);
        }
    }
    
    // At most one leader per partition
    assert(leaders_partition1 <= 1);
    assert(leaders_partition2 <= 1);
    
    // Heal partition
    test_cluster_heal_partition(ctx->cluster);
    test_cluster_tick(ctx->cluster, 1000);
    
    // Eventually exactly one leader
    int total_leaders = 0;
    uint64_t leader_term = 0;
    
    for (int i = 1; i < NUM_NODES; i++) {
        if (raft_get_state(ctx->nodes[i]) == RAFT_STATE_LEADER) {
            total_leaders++;
            leader_term = raft_get_current_term(ctx->nodes[i]);
            
            // Must have all committed entries
            assert(raft_get_last_log_index(ctx->nodes[i]) >= commit_index1);
        }
    }
    
    assert(total_leaders == 1);
    assert(leader_term > term1);
    
    printf("  ✓ Election safety maintained across partition\n");
    printf("  ✓ Final leader has all committed entries\n");
    
    teardown_test(ctx);
}

/**
 * Test: Committed entry from old term is not overwritten
 */
static void test_committed_entry_not_overwritten(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Committed entries cannot be overwritten\n");
    
    // Elect leader and commit entry
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader1 = ctx->nodes[0];
    uint64_t term1 = raft_get_current_term(leader1);
    
    const char *committed_data = "critical_committed_entry";
    raft_entry_t entry1 = {
        .term = term1,
        .data = (char *)committed_data,
        .data_len = strlen(committed_data)
    };
    
    uint64_t committed_index = raft_append_entry(leader1, &entry1);
    test_cluster_replicate_and_commit(ctx->cluster);
    
    // Verify commit
    assert(raft_get_commit_index(leader1) >= committed_index);
    
    // Crash leader and elect new leader
    test_cluster_crash_node(ctx->cluster, 0);
    test_cluster_elect_leader(ctx->cluster, 1);
    
    raft_node_t *leader2 = ctx->nodes[1];
    
    // Attempt to append conflicting entry (should fail)
    const char *conflicting_data = "conflicting_entry";
    raft_entry_t entry2 = {
        .term = raft_get_current_term(leader2),
        .data = (char *)conflicting_data,
        .data_len = strlen(conflicting_data)
    };
    
    // Try to overwrite at committed index (should be prevented)
    int rv = raft_set_entry(leader2, committed_index, &entry2);
    assert(rv != 0);  // Should fail
    
    // Verify original committed entry is intact
    raft_entry_t *retrieved = raft_get_entry(leader2, committed_index);
    assert(retrieved != NULL);
    assert(strcmp(retrieved->data, committed_data) == 0);
    assert(retrieved->term == term1);
    
    printf("  ✓ Committed entry protected from overwrite\n");
    printf("  ✓ Original data intact: '%s'\n", committed_data);
    
    teardown_test(ctx);
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  RAFT Leader Completeness Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_init();
    
    test_new_leader_has_committed_entries();
    printf("\n");
    
    test_incomplete_log_loses_election();
    printf("\n");
    
    test_committed_entries_survive_leader_changes();
    printf("\n");
    
    test_election_safety_with_committed_entries();
    printf("\n");
    
    test_committed_entry_not_overwritten();
    printf("\n");
    
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  All Leader Completeness Tests Passed ✓\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_cleanup();
    return 0;
}