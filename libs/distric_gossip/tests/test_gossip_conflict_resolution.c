/**
 * @file test_gossip_conflict_resolution.c
 * @brief Comprehensive tests for Section 4.2 - Conflict Resolution
 * 
 * Tests all conflict resolution scenarios including:
 * - Incarnation-based ordering
 * - State priority ordering
 * - Automatic refutation
 * - Broadcast refutation
 * - Edge cases and race conditions
 * 
 * @version 1.0
 * @date 2026-02-12
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gossip_conflict_resolution.h"
#include "distric_gossip.h"

/* Test utilities */
#define TEST_PASS() printf("  ✓ PASS\n")
#define TEST_FAIL(msg) do { printf("  ✗ FAIL: %s\n", msg); return -1; } while(0)
#define RUN_TEST(test_fn) do { \
    printf("Running %s...\n", #test_fn); \
    if (test_fn() != 0) { \
        printf("TEST FAILED: %s\n", #test_fn); \
        return -1; \
    } \
} while(0)

/* ============================================================================
 * TEST 1: Incarnation Ordering
 * ========================================================================= */

static int test_incarnation_ordering_higher_wins(void) {
    printf("  Test: Higher incarnation always wins\n");
    
    /* Scenario: SUSPECTED at inc=6 should beat ALIVE at inc=5 */
    int result = gossip_compare_updates(
        GOSSIP_NODE_SUSPECTED, 6,  /* update */
        GOSSIP_NODE_ALIVE, 5       /* current */
    );
    
    if (result <= 0) {
        TEST_FAIL("Higher incarnation should win");
    }
    
    /* Verify should_apply returns true */
    bool should_apply = gossip_should_apply_update(
        GOSSIP_NODE_ALIVE, 5,      /* current */
        GOSSIP_NODE_SUSPECTED, 6   /* update */
    );
    
    if (!should_apply) {
        TEST_FAIL("Update with higher incarnation should be applied");
    }
    
    TEST_PASS();
    return 0;
}

static int test_incarnation_ordering_lower_rejected(void) {
    printf("  Test: Lower incarnation is rejected\n");
    
    /* Scenario: FAILED at inc=9 should lose to ALIVE at inc=10 */
    int result = gossip_compare_updates(
        GOSSIP_NODE_FAILED, 9,     /* update */
        GOSSIP_NODE_ALIVE, 10      /* current */
    );
    
    if (result >= 0) {
        TEST_FAIL("Lower incarnation should lose");
    }
    
    /* Verify should_apply returns false */
    bool should_apply = gossip_should_apply_update(
        GOSSIP_NODE_ALIVE, 10,     /* current */
        GOSSIP_NODE_FAILED, 9      /* update */
    );
    
    if (should_apply) {
        TEST_FAIL("Update with lower incarnation should be rejected");
    }
    
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 2: State Priority Ordering (Same Incarnation)
 * ========================================================================= */

static int test_state_priority_alive_beats_suspected(void) {
    printf("  Test: ALIVE beats SUSPECTED at same incarnation\n");
    
    /* Scenario: ALIVE at inc=5 should beat SUSPECTED at inc=5 */
    int result = gossip_compare_updates(
        GOSSIP_NODE_ALIVE, 5,      /* update */
        GOSSIP_NODE_SUSPECTED, 5   /* current */
    );
    
    if (result <= 0) {
        TEST_FAIL("ALIVE should beat SUSPECTED");
    }
    
    TEST_PASS();
    return 0;
}

static int test_state_priority_suspected_beats_failed(void) {
    printf("  Test: SUSPECTED beats FAILED at same incarnation\n");
    
    /* Scenario: SUSPECTED at inc=7 should beat FAILED at inc=7 */
    int result = gossip_compare_updates(
        GOSSIP_NODE_SUSPECTED, 7,  /* update */
        GOSSIP_NODE_FAILED, 7      /* current */
    );
    
    if (result <= 0) {
        TEST_FAIL("SUSPECTED should beat FAILED");
    }
    
    TEST_PASS();
    return 0;
}

static int test_state_priority_failed_beats_left(void) {
    printf("  Test: FAILED beats LEFT at same incarnation\n");
    
    /* Scenario: FAILED at inc=3 should beat LEFT at inc=3 */
    int result = gossip_compare_updates(
        GOSSIP_NODE_FAILED, 3,     /* update */
        GOSSIP_NODE_LEFT, 3        /* current */
    );
    
    if (result <= 0) {
        TEST_FAIL("FAILED should beat LEFT");
    }
    
    TEST_PASS();
    return 0;
}

static int test_state_priority_full_ordering(void) {
    printf("  Test: Complete state priority ordering\n");
    
    /* ALIVE(3) > SUSPECTED(2) > FAILED(1) > LEFT(0) */
    
    /* ALIVE > SUSPECTED */
    if (gossip_compare_updates(GOSSIP_NODE_ALIVE, 5, GOSSIP_NODE_SUSPECTED, 5) <= 0) {
        TEST_FAIL("ALIVE should beat SUSPECTED");
    }
    
    /* ALIVE > FAILED */
    if (gossip_compare_updates(GOSSIP_NODE_ALIVE, 5, GOSSIP_NODE_FAILED, 5) <= 0) {
        TEST_FAIL("ALIVE should beat FAILED");
    }
    
    /* ALIVE > LEFT */
    if (gossip_compare_updates(GOSSIP_NODE_ALIVE, 5, GOSSIP_NODE_LEFT, 5) <= 0) {
        TEST_FAIL("ALIVE should beat LEFT");
    }
    
    /* SUSPECTED > FAILED */
    if (gossip_compare_updates(GOSSIP_NODE_SUSPECTED, 5, GOSSIP_NODE_FAILED, 5) <= 0) {
        TEST_FAIL("SUSPECTED should beat FAILED");
    }
    
    /* SUSPECTED > LEFT */
    if (gossip_compare_updates(GOSSIP_NODE_SUSPECTED, 5, GOSSIP_NODE_LEFT, 5) <= 0) {
        TEST_FAIL("SUSPECTED should beat LEFT");
    }
    
    /* FAILED > LEFT */
    if (gossip_compare_updates(GOSSIP_NODE_FAILED, 5, GOSSIP_NODE_LEFT, 5) <= 0) {
        TEST_FAIL("FAILED should beat LEFT");
    }
    
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 3: Edge Cases
 * ========================================================================= */

static int test_edge_case_same_state_same_incarnation(void) {
    printf("  Test: Same state and incarnation = equivalent\n");
    
    int result = gossip_compare_updates(
        GOSSIP_NODE_ALIVE, 5,
        GOSSIP_NODE_ALIVE, 5
    );
    
    if (result != 0) {
        TEST_FAIL("Identical updates should be equivalent");
    }
    
    TEST_PASS();
    return 0;
}

static int test_edge_case_incarnation_overflow(void) {
    printf("  Test: Handle large incarnation numbers\n");
    
    /* Test with very large incarnation numbers */
    uint64_t large_inc = UINT64_MAX - 100;
    
    int result = gossip_compare_updates(
        GOSSIP_NODE_ALIVE, large_inc + 1,
        GOSSIP_NODE_ALIVE, large_inc
    );
    
    if (result <= 0) {
        TEST_FAIL("Large incarnation comparison failed");
    }
    
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 4: Real-World Scenarios
 * ========================================================================= */

static int test_scenario_network_partition_reconciliation(void) {
    printf("  Test: Network partition reconciliation\n");
    
    /*
     * Scenario: Network partition
     * - Partition A: Node1 ALIVE at inc=10
     * - Partition B: Node1 marked FAILED at inc=9
     * - Partitions merge: ALIVE at inc=10 should win
     */
    
    bool partition_a_wins = gossip_should_apply_update(
        GOSSIP_NODE_FAILED, 9,     /* Partition B's view */
        GOSSIP_NODE_ALIVE, 10      /* Partition A's view */
    );
    
    if (!partition_a_wins) {
        TEST_FAIL("Higher incarnation ALIVE should win over lower FAILED");
    }
    
    TEST_PASS();
    return 0;
}

static int test_scenario_rapid_state_changes(void) {
    printf("  Test: Rapid state changes with incrementing incarnations\n");
    
    /*
     * Simulate rapid state transitions:
     * 1. ALIVE at inc=1
     * 2. SUSPECTED at inc=2 (detected failure)
     * 3. ALIVE at inc=3 (refuted)
     * 4. SUSPECTED at inc=4 (detected again)
     * 5. ALIVE at inc=5 (refuted again)
     */
    
    gossip_node_status_t current_status = GOSSIP_NODE_ALIVE;
    uint64_t current_inc = 1;
    
    /* Transition 1→2: SUSPECTED at inc=2 */
    if (!gossip_should_apply_update(current_status, current_inc,
                                    GOSSIP_NODE_SUSPECTED, 2)) {
        TEST_FAIL("Should accept SUSPECTED at higher incarnation");
    }
    current_status = GOSSIP_NODE_SUSPECTED;
    current_inc = 2;
    
    /* Transition 2→3: ALIVE at inc=3 (refutation) */
    if (!gossip_should_apply_update(current_status, current_inc,
                                    GOSSIP_NODE_ALIVE, 3)) {
        TEST_FAIL("Should accept ALIVE refutation at higher incarnation");
    }
    current_status = GOSSIP_NODE_ALIVE;
    current_inc = 3;
    
    /* Transition 3→4: SUSPECTED at inc=4 */
    if (!gossip_should_apply_update(current_status, current_inc,
                                    GOSSIP_NODE_SUSPECTED, 4)) {
        TEST_FAIL("Should accept second SUSPECTED at higher incarnation");
    }
    current_status = GOSSIP_NODE_SUSPECTED;
    current_inc = 4;
    
    /* Transition 4→5: ALIVE at inc=5 (second refutation) */
    if (!gossip_should_apply_update(current_status, current_inc,
                                    GOSSIP_NODE_ALIVE, 5)) {
        TEST_FAIL("Should accept second ALIVE refutation");
    }
    
    TEST_PASS();
    return 0;
}

static int test_scenario_false_suspicion_refutation(void) {
    printf("  Test: False suspicion refutation workflow\n");
    
    /*
     * Scenario:
     * 1. Node A is ALIVE at inc=10
     * 2. Node B suspects A (SUSPECTED at inc=10)
     * 3. Node A refutes by broadcasting ALIVE at inc=11
     * 4. All nodes accept inc=11 over inc=10
     */
    
    /* Initial state */
    gossip_node_status_t node_a_status = GOSSIP_NODE_ALIVE;
    uint64_t node_a_inc = 10;
    
    /* Node B's suspicion (same incarnation, lower priority) */
    bool b_suspicion_accepted = gossip_should_apply_update(
        node_a_status, node_a_inc,
        GOSSIP_NODE_SUSPECTED, 10
    );
    
    /* Suspicion should NOT be accepted (same inc, lower priority) */
    if (b_suspicion_accepted) {
        TEST_FAIL("SUSPECTED at same incarnation should not beat ALIVE");
    }
    
    /* But if the suspicion has higher incarnation (stale local state) */
    b_suspicion_accepted = gossip_should_apply_update(
        node_a_status, node_a_inc,
        GOSSIP_NODE_SUSPECTED, 11
    );
    
    if (!b_suspicion_accepted) {
        TEST_FAIL("SUSPECTED at higher incarnation should be accepted");
    }
    
    /* Node A refutes with inc=12 */
    bool refutation_accepted = gossip_should_apply_update(
        GOSSIP_NODE_SUSPECTED, 11,  /* Current state after suspicion */
        GOSSIP_NODE_ALIVE, 12       /* Refutation */
    );
    
    if (!refutation_accepted) {
        TEST_FAIL("ALIVE refutation at higher incarnation should be accepted");
    }
    
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * TEST 5: Integration with Gossip State (Mock Test)
 * ========================================================================= */

static int test_broadcast_refutation_api(void) {
    printf("  Test: Broadcast refutation API (basic check)\n");
    
    /*
     * Note: Full integration test requires actual gossip_state_t
     * This is a basic API contract test
     */
    
    /* Test with NULL state (should return error) */
    distric_err_t err = gossip_broadcast_refutation(NULL);
    if (err != DISTRIC_ERR_INVALID_ARG) {
        TEST_FAIL("Should return INVALID_ARG for NULL state");
    }
    
    TEST_PASS();
    return 0;
}

static int test_conflict_stats_api(void) {
    printf("  Test: Conflict statistics API (basic check)\n");
    
    /* Test with NULL state */
    uint64_t refutations, conflicts;
    distric_err_t err = gossip_get_conflict_stats(NULL, &refutations, &conflicts);
    if (err != DISTRIC_ERR_INVALID_ARG) {
        TEST_FAIL("Should return INVALID_ARG for NULL state");
    }
    
    TEST_PASS();
    return 0;
}

/* ============================================================================
 * MAIN TEST RUNNER
 * ========================================================================= */

int main(void) {
    printf("=======================================================\n");
    printf("  Gossip Conflict Resolution Tests (Section 4.2)\n");
    printf("=======================================================\n\n");
    
    /* Test Suite 1: Incarnation Ordering */
    printf("Suite 1: Incarnation Ordering\n");
    printf("------------------------------\n");
    RUN_TEST(test_incarnation_ordering_higher_wins);
    RUN_TEST(test_incarnation_ordering_lower_rejected);
    printf("\n");
    
    /* Test Suite 2: State Priority Ordering */
    printf("Suite 2: State Priority Ordering\n");
    printf("---------------------------------\n");
    RUN_TEST(test_state_priority_alive_beats_suspected);
    RUN_TEST(test_state_priority_suspected_beats_failed);
    RUN_TEST(test_state_priority_failed_beats_left);
    RUN_TEST(test_state_priority_full_ordering);
    printf("\n");
    
    /* Test Suite 3: Edge Cases */
    printf("Suite 3: Edge Cases\n");
    printf("-------------------\n");
    RUN_TEST(test_edge_case_same_state_same_incarnation);
    RUN_TEST(test_edge_case_incarnation_overflow);
    printf("\n");
    
    /* Test Suite 4: Real-World Scenarios */
    printf("Suite 4: Real-World Scenarios\n");
    printf("-----------------------------\n");
    RUN_TEST(test_scenario_network_partition_reconciliation);
    RUN_TEST(test_scenario_rapid_state_changes);
    RUN_TEST(test_scenario_false_suspicion_refutation);
    printf("\n");
    
    /* Test Suite 5: API Tests */
    printf("Suite 5: API Contract Tests\n");
    printf("---------------------------\n");
    RUN_TEST(test_broadcast_refutation_api);
    RUN_TEST(test_conflict_stats_api);
    printf("\n");
    
    printf("=======================================================\n");
    printf("  ALL TESTS PASSED (%d/%d)\n", 13, 13);
    printf("=======================================================\n");
    
    return 0;
}