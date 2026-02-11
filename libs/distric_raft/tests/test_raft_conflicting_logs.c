/**
 * @file test_raft_conflicting_logs.c
 * @brief Test RAFT log conflict resolution
 *
 * Scenario:
 * Different leaders append conflicting entries to their logs during a network
 * partition, then the partition heals. RAFT must reconcile these logs safely.
 *
 * Why Important:
 * Log reconciliation is core to RAFT's safety guarantees. Incorrect handling
 * can lead to divergent state machines and data loss.
 *
 * Assertions:
 * - Conflicting entries are rolled back
 * - Logs converge to a single consistent sequence
 * - No conflicting entry is committed
 * - Log matching property is preserved
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "raft_test_framework.h"

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
 * Test: Basic log conflict resolution
 */
static void test_basic_conflict_resolution(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Basic log conflict resolution\n");
    
    // Establish initial leader (node 0) and replicate baseline
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader1 = ctx->nodes[0];
    uint64_t term1 = raft_get_current_term(leader1);
    
    // Append and commit baseline entries
    for (int i = 0; i < 3; i++) {
        char data[32];
        snprintf(data, sizeof(data), "baseline_%d", i);
        raft_entry_t entry = {
            .term = term1,
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    uint64_t baseline_commit = raft_get_commit_index(leader1);
    
    printf("  ✓ Baseline: %lu entries committed in term %lu\n", 
           baseline_commit, term1);
    
    // Create partition: {0,1} vs {2,3,4}
    test_cluster_partition(ctx->cluster, (int[]){0, 1}, 2, (int[]){2, 3, 4}, 3);
    
    // Leader 0 (minority) appends conflicting entries
    for (int i = 0; i < 3; i++) {
        char data[32];
        snprintf(data, sizeof(data), "conflict_A_%d", i);
        raft_entry_t entry = {
            .term = term1,
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    // These should NOT commit (no majority)
    test_cluster_tick(ctx->cluster, 200);
    assert(raft_get_commit_index(leader1) == baseline_commit);
    
    // Majority partition elects new leader (node 2)
    test_cluster_elect_leader(ctx->cluster, 2);
    raft_node_t *leader2 = ctx->nodes[2];
    uint64_t term2 = raft_get_current_term(leader2);
    assert(term2 > term1);
    
    // New leader appends different entries
    for (int i = 0; i < 3; i++) {
        char data[32];
        snprintf(data, sizeof(data), "conflict_B_%d", i);
        raft_entry_t entry = {
            .term = term2,
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader2, &entry);
    }
    
    // These SHOULD commit (majority available)
    test_cluster_replicate_and_commit(ctx->cluster);
    uint64_t new_commit = raft_get_commit_index(leader2);
    assert(new_commit > baseline_commit);
    
    printf("  ✓ Partition created conflicting logs\n");
    printf("    - Minority (node 0): uncommitted 'conflict_A' entries\n");
    printf("    - Majority (node 2): committed 'conflict_B' entries\n");
    
    // Heal partition
    test_cluster_heal_partition(ctx->cluster);
    test_cluster_tick(ctx->cluster, 500);
    
    // Old leader (node 0) should step down
    assert(raft_get_state(ctx->nodes[0]) == RAFT_STATE_FOLLOWER);
    assert(raft_get_current_term(ctx->nodes[0]) >= term2);
    
    // Replicate logs from new leader
    test_cluster_replicate_logs(ctx->cluster);
    
    // Verify all nodes have identical logs
    uint64_t final_index = raft_get_last_log_index(leader2);
    
    for (int node_id = 0; node_id < NUM_NODES; node_id++) {
        raft_node_t *node = ctx->nodes[node_id];
        
        assert(raft_get_last_log_index(node) == final_index);
        
        // Check each entry matches leader2
        for (uint64_t idx = 1; idx <= final_index; idx++) {
            raft_entry_t *entry_leader = raft_get_entry(leader2, idx);
            raft_entry_t *entry_node = raft_get_entry(node, idx);
            
            assert(entry_leader != NULL);
            assert(entry_node != NULL);
            assert(entry_leader->term == entry_node->term);
            assert(strcmp(entry_leader->data, entry_node->data) == 0);
        }
    }
    
    // Verify conflict_A entries were rolled back
    for (uint64_t idx = baseline_commit + 1; idx <= final_index; idx++) {
        raft_entry_t *entry = raft_get_entry(ctx->nodes[0], idx);
        assert(entry != NULL);
        assert(strstr(entry->data, "conflict_B") != NULL);
        assert(strstr(entry->data, "conflict_A") == NULL);
    }
    
    printf("  ✓ Logs converged: all nodes have identical %lu entries\n", final_index);
    printf("  ✓ Conflicting entries rolled back successfully\n");
    
    teardown_test(ctx);
}

/**
 * Test: Multiple conflicting entries at different indices
 */
static void test_multiple_conflicts_at_different_indices(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Multiple conflicts at different log indices\n");
    
    // Start with committed baseline
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader1 = ctx->nodes[0];
    
    for (int i = 0; i < 5; i++) {
        char data[32];
        snprintf(data, sizeof(data), "base_%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader1),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    
    // Create partition
    test_cluster_partition(ctx->cluster, (int[]){0}, 1, (int[]){1, 2, 3, 4}, 4);
    
    // Isolated node appends many conflicting entries
    for (int i = 0; i < 10; i++) {
        char data[32];
        snprintf(data, sizeof(data), "isolated_%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader1),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    uint64_t isolated_log_size = raft_get_last_log_index(leader1);
    
    // Majority elects new leader and commits different entries
    test_cluster_elect_leader(ctx->cluster, 1);
    raft_node_t *leader2 = ctx->nodes[1];
    
    for (int i = 0; i < 7; i++) {
        char data[32];
        snprintf(data, sizeof(data), "majority_%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader2),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader2, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    
    uint64_t majority_log_size = raft_get_last_log_index(leader2);
    
    printf("  ✓ Isolated node: %lu entries\n", isolated_log_size);
    printf("  ✓ Majority: %lu entries\n", majority_log_size);
    
    // Heal partition
    test_cluster_heal_partition(ctx->cluster);
    test_cluster_replicate_logs(ctx->cluster);
    
    // Verify logs converged
    for (int i = 0; i < NUM_NODES; i++) {
        assert(raft_get_last_log_index(ctx->nodes[i]) == majority_log_size);
        
        // Verify no "isolated_" entries remain on node 0
        for (uint64_t idx = 1; idx <= majority_log_size; idx++) {
            raft_entry_t *entry = raft_get_entry(ctx->nodes[i], idx);
            assert(strstr(entry->data, "isolated_") == NULL);
        }
    }
    
    printf("  ✓ All %d conflicting entries rolled back\n", 
           (int)(isolated_log_size - 5));
    printf("  ✓ Logs converged to majority version\n");
    
    teardown_test(ctx);
}

/**
 * Test: Conflict with overlapping but different entries
 */
static void test_overlapping_conflicting_logs(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Overlapping but conflicting log entries\n");
    
    // Establish baseline
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader1 = ctx->nodes[0];
    uint64_t term1 = raft_get_current_term(leader1);
    
    for (int i = 0; i < 4; i++) {
        char data[32];
        snprintf(data, sizeof(data), "shared_%d", i);
        raft_entry_t entry = {
            .term = term1,
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    
    // Partition: node 0 isolated
    test_cluster_partition(ctx->cluster, (int[]){0}, 1, (int[]){1, 2, 3, 4}, 4);
    
    // Node 0 appends entries
    for (int i = 4; i < 8; i++) {
        char data[32];
        snprintf(data, sizeof(data), "branch_A_%d", i);
        raft_entry_t entry = {
            .term = term1,
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    // Majority gets new leader in higher term
    test_cluster_elect_leader(ctx->cluster, 1);
    raft_node_t *leader2 = ctx->nodes[1];
    uint64_t term2 = raft_get_current_term(leader2);
    
    // Leader 2 appends fewer but conflicting entries
    for (int i = 4; i < 6; i++) {
        char data[32];
        snprintf(data, sizeof(data), "branch_B_%d", i);
        raft_entry_t entry = {
            .term = term2,
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader2, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    
    // Heal and replicate
    test_cluster_heal_partition(ctx->cluster);
    test_cluster_replicate_logs(ctx->cluster);
    
    // All nodes should have branch_B, not branch_A
    uint64_t expected_index = 4 + 2;  // 4 shared + 2 from branch_B
    
    for (int i = 0; i < NUM_NODES; i++) {
        assert(raft_get_last_log_index(ctx->nodes[i]) == expected_index);
        
        // Entries 5 and 6 should be branch_B
        raft_entry_t *e5 = raft_get_entry(ctx->nodes[i], 5);
        raft_entry_t *e6 = raft_get_entry(ctx->nodes[i], 6);
        
        assert(e5 != NULL && strstr(e5->data, "branch_B") != NULL);
        assert(e6 != NULL && strstr(e6->data, "branch_B") != NULL);
    }
    
    printf("  ✓ Shorter log from higher term won\n");
    printf("  ✓ Longer log from lower term truncated\n");
    
    teardown_test(ctx);
}

/**
 * Test: Log matching property after conflict resolution
 */
static void test_log_matching_property_preserved(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Log matching property preserved after conflicts\n");
    
    // Create conflicts and resolve them
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader1 = ctx->nodes[0];
    
    for (int i = 0; i < 3; i++) {
        char data[16];
        snprintf(data, sizeof(data), "e%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader1),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    
    // Create conflicts via partition
    test_cluster_partition(ctx->cluster, (int[]){0, 1}, 2, (int[]){2, 3, 4}, 3);
    
    for (int i = 3; i < 6; i++) {
        char data[16];
        snprintf(data, sizeof(data), "X%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader1),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    test_cluster_elect_leader(ctx->cluster, 2);
    raft_node_t *leader2 = ctx->nodes[2];
    
    for (int i = 3; i < 7; i++) {
        char data[16];
        snprintf(data, sizeof(data), "Y%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader2),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader2, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    
    // Heal
    test_cluster_heal_partition(ctx->cluster);
    test_cluster_replicate_logs(ctx->cluster);
    
    // Verify log matching property:
    // If two entries have same index and term, all preceding entries match
    for (int n1 = 0; n1 < NUM_NODES; n1++) {
        for (int n2 = n1 + 1; n2 < NUM_NODES; n2++) {
            uint64_t max_idx = raft_get_last_log_index(ctx->nodes[n1]);
            
            for (uint64_t idx = 1; idx <= max_idx; idx++) {
                raft_entry_t *e1 = raft_get_entry(ctx->nodes[n1], idx);
                raft_entry_t *e2 = raft_get_entry(ctx->nodes[n2], idx);
                
                assert(e1 != NULL && e2 != NULL);
                assert(e1->term == e2->term);
                assert(strcmp(e1->data, e2->data) == 0);
            }
        }
    }
    
    printf("  ✓ Log matching property verified across all node pairs\n");
    
    teardown_test(ctx);
}

/**
 * Test: No conflicting entry is ever committed
 */
static void test_conflicting_entries_never_committed(void) {
    test_context_t *ctx = setup_test();
    
    printf("Test: Conflicting entries are never committed\n");
    
    // Baseline
    test_cluster_elect_leader(ctx->cluster, 0);
    raft_node_t *leader1 = ctx->nodes[0];
    
    for (int i = 0; i < 2; i++) {
        char data[16];
        snprintf(data, sizeof(data), "safe_%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader1),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    uint64_t safe_commit = raft_get_commit_index(leader1);
    
    // Partition
    test_cluster_partition(ctx->cluster, (int[]){0}, 1, (int[]){1, 2, 3, 4}, 4);
    
    // Isolated leader appends but cannot commit
    for (int i = 0; i < 3; i++) {
        char data[32];
        snprintf(data, sizeof(data), "unsafe_%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader1),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader1, &entry);
    }
    
    test_cluster_tick(ctx->cluster, 500);
    
    // Verify uncommitted on isolated node
    assert(raft_get_commit_index(leader1) == safe_commit);
    
    // Majority commits different entries
    test_cluster_elect_leader(ctx->cluster, 1);
    raft_node_t *leader2 = ctx->nodes[1];
    
    for (int i = 0; i < 3; i++) {
        char data[32];
        snprintf(data, sizeof(data), "committed_%d", i);
        raft_entry_t entry = {
            .term = raft_get_current_term(leader2),
            .data = data,
            .data_len = strlen(data)
        };
        raft_append_entry(leader2, &entry);
    }
    
    test_cluster_replicate_and_commit(ctx->cluster);
    uint64_t new_commit = raft_get_commit_index(leader2);
    assert(new_commit > safe_commit);
    
    // Track what was committed on majority
    uint64_t majority_entries[10];
    int majority_count = 0;
    
    for (uint64_t idx = safe_commit + 1; idx <= new_commit; idx++) {
        raft_entry_t *entry = raft_get_entry(leader2, idx);
        assert(strstr(entry->data, "committed_") != NULL);
        majority_entries[majority_count++] = idx;
    }
    
    // Heal partition
    test_cluster_heal_partition(ctx->cluster);
    test_cluster_replicate_logs(ctx->cluster);
    
    // Verify "unsafe_" entries were never committed
    for (int i = 0; i < NUM_NODES; i++) {
        uint64_t commit_idx = raft_get_commit_index(ctx->nodes[i]);
        
        for (uint64_t idx = 1; idx <= commit_idx; idx++) {
            raft_entry_t *entry = raft_get_entry(ctx->nodes[i], idx);
            assert(strstr(entry->data, "unsafe_") == NULL);
        }
    }
    
    printf("  ✓ Conflicting entries never reached commit state\n");
    printf("  ✓ Only majority-agreed entries were committed\n");
    
    teardown_test(ctx);
}

int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  RAFT Conflicting Logs Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_init();
    
    test_basic_conflict_resolution();
    printf("\n");
    
    test_multiple_conflicts_at_different_indices();
    printf("\n");
    
    test_overlapping_conflicting_logs();
    printf("\n");
    
    test_log_matching_property_preserved();
    printf("\n");
    
    test_conflicting_entries_never_committed();
    printf("\n");
    
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  All Conflicting Logs Tests Passed ✓\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    test_framework_cleanup();
    return 0;
}